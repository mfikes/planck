#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <JavaScriptCore/JavaScript.h>

#include "linenoise.h"

#include "bundle.h"
#include "cljs.h"
#include "globals.h"
#include "io.h"
#include "jsc_utils.h"
#include "legal.h"
#include "repl.h"
#include "str.h"
#include "zip.h"

void completion(const char *buf, linenoiseCompletions *lc);

void usage(char *program_name) {
	printf("Planck %s\n", PLANCK_VERSION);
	printf("Usage:  %s [init-opt*] [main-opt] [arg*]\n", program_name);
	printf("\n");
	printf("  With no options or args, runs an interactive Read-Eval-Print Loop\n");
	printf("\n");
	printf("  init options:\n");
	printf("    -i path, --init=path     Load a file or resource\n");
	printf("    -e string, --eval=string Evaluate expressions in string; print non-nil\n");
	printf("                             values\n");
	printf("    -c cp, --classpath=cp    Use colon-delimited cp for source directories and\n");
	printf("                             JARs\n");
	printf("    -K, --auto-cache         Create and use .planck_cache dir for cache\n");
	printf("    -k path, --cache=path    If dir exists at path, use it for cache\n");
	printf("    -q, --quiet              Quiet mode\n");
	printf("    -v, --verbose            Emit verbose diagnostic output\n");
	printf("    -d, --dumb-terminal      Disable line editing / VT100 terminal control\n");
	printf("    -t theme, --theme=theme  Set the color theme\n");
	// printf("    -n x, --socket-repl=x    Enable socket REPL where x is port or IP:port\n");
	printf("    -s, --static-fns         Generate static dispatch function calls\n");
	printf("    -a, --elide-asserts      Set *assert* to false to remove asserts\n");
	printf("\n");
	printf("  main options:\n");
	printf("    -m ns-name, --main=ns-name Call the -main function from a namespace with\n");
	printf("                               args\n");
	printf("    -r, --repl                 Run a repl\n");
	// printf("    path                       Run a script from a file or resource\n");
	// printf("    -                          Run a script from standard input\n");
	printf("    -h, -?, --help             Print this help message and exit\n");
	printf("    -l, --legal                Show legal info (licenses and copyrights)\n");
	printf("\n");
	printf("  operation:\n");
	printf("\n");
	printf("    - Enters the cljs.user namespace\n");
	// printf("    - Binds planck.core/*command-line-args* to a seq of strings containing\n");
	// printf("      command line args that appear after any main option\n");
	printf("    - Runs all init options in order\n");
	// printf("    - Calls a -main function or runs a repl or script if requested\n");
	printf("    - Runs a repl or script if requested\n");
	printf("\n");
	printf("  The init options may be repeated and mixed freely, but must appear before\n");
	printf("  any main option.\n");
	printf("\n");
	printf("  Paths may be absolute or relative in the filesystem.\n");
	printf("\n");
	printf("  A comprehensive User Guide for Planck can be found at http://planck-repl.org\n");
	printf("\n");
}

char *get_cljs_version() {
	char *bundle_js = bundle_get_contents("planck/bundle.js");
	if (bundle_js != NULL) {
		char *start = bundle_js + 29;
		char *version = strtok(start, " ");
		version = strdup(version);
		free(bundle_js);
		return version;
	} else {
		return "(Unknown)";
	}
}

void banner() {
	printf("Planck %s\n", PLANCK_VERSION);
	printf("ClojureScript %s\n", get_cljs_version());

	printf("    Docs: (doc function-name-here)\n");
	printf("          (find-doc \"part-of-name-here\")\n");
	printf("  Source: (source function-name-here)\n");
	printf("    Exit: Control+D or :cljs/quit or exit or quit\n");
	printf(" Results: Stored in vars *1, *2, *3, an exception in *e\n");

	printf("\n");
}

struct config config;
int exit_value = 0;
bool return_termsize = false;
JSContextRef global_ctx = NULL;

char* ensure_trailing_slash(char* s) {
	if (str_has_suffix(s, "/") == 0) {
		return strdup(s);
	} else {
		return str_concat(s, "/");
	}
}

int main(int argc, char **argv) {
	config.verbose = false;
	config.quiet = false;
	config.repl = false;
	config.javascript = false;
	config.static_fns = false;
	config.elide_asserts = false;
	config.cache_path = NULL;
	config.theme = "light";
	config.dumb_terminal = false;

	config.out_path = NULL;
	config.num_src_paths = 0;
	config.src_paths = NULL;
	config.num_scripts = 0;
	config.scripts = NULL;

	config.main_ns_name = NULL;

	struct option long_options[] = {
		{"help", no_argument, NULL, 'h'},
		{"legal", no_argument, NULL, 'l'},
		{"verbose", no_argument, NULL, 'v'},
		{"quiet", no_argument, NULL, 'q'},
		{"repl", no_argument, NULL, 'r'},
		{"static-fns", no_argument, NULL, 's'},
		{"elide-asserts", no_argument, NULL, 'a'},
		{"cache", required_argument, NULL, 'k'},
		{"eval", required_argument, NULL, 'e'},
		{"theme", required_argument, NULL, 't'},
		{"dumb-terminal", no_argument, NULL, 'd'},
		{"classpath", required_argument, NULL, 'c'},
		{"auto-cache", no_argument, NULL, 'K'},
		{"init", required_argument, NULL, 'i'},
		{"main", required_argument, NULL, 'm'},

		// development options
		{"javascript", no_argument, NULL, 'j'},
		{"out", required_argument, NULL, 'o'},

		{0, 0, 0, 0}
	};
	int opt, option_index;
	while ((opt = getopt_long(argc, argv, "h?lvrsak:je:t:dc:o:Ki:qm:", long_options, &option_index)) != -1) {
		switch (opt) {
		case 'h':
			usage(argv[0]);
			exit(0);
		case 'l':
			legal();
			return 0;
		case 'v':
			config.verbose = true;
			break;
		case 'q':
			config.quiet = true;
			break;
		case 'r':
			config.repl = true;
			break;
		case 's':
			config.static_fns = true;
			break;
		case 'a':
			config.elide_asserts = true;
			break;
		case 'k':
			config.cache_path = strdup(optarg);
			break;
		case 'K':
			config.cache_path = ".planck_cache";
			{
				char *path_copy = strdup(config.cache_path);
				char *dir = dirname(path_copy);
				if (mkdir_p(dir) < 0) {
					fprintf(stderr, "Could not create %s: %s\n", config.cache_path, strerror(errno));
				}
				free(path_copy);
			}
			break;
		case 'j':
			config.javascript = true;
			break;
		case 'e':
			config.num_scripts += 1;
			config.scripts = realloc(config.scripts, config.num_scripts * sizeof(struct script));
			config.scripts[config.num_scripts - 1].type = "text";
			config.scripts[config.num_scripts - 1].expression = true;
			config.scripts[config.num_scripts - 1].source = strdup(optarg);
			break;
		case 'i':
			config.num_scripts += 1;
			config.scripts = realloc(config.scripts, config.num_scripts * sizeof(struct script));
			config.scripts[config.num_scripts - 1].type = "path";
			config.scripts[config.num_scripts - 1].expression = false;
			config.scripts[config.num_scripts - 1].source = strdup(optarg);
			break;
		case 'm':
			config.main_ns_name = strdup(optarg);
			break;
		case 't':
			config.theme = strdup(optarg);
			break;
		case 'd':
			config.dumb_terminal = true;
			break;
		case 'c':
			{
				char *classpath = strdup(optarg);
				char *source = strtok(classpath, ":");
				while (source != NULL) {
					char *type = "src";
					if (str_has_suffix(source, ".jar") == 0) {
						type = "jar";
					}

					config.num_src_paths += 1;
					config.src_paths = realloc(config.src_paths, config.num_src_paths * sizeof(struct src_path));
					config.src_paths[config.num_src_paths - 1].type = type;
					config.src_paths[config.num_src_paths - 1].path = strcmp(type, "jar") == 0 ? strdup(source) : ensure_trailing_slash(source);

					source = strtok(NULL, ":");
				}

				break;
			}
		case 'o':
			config.out_path = ensure_trailing_slash(strdup(optarg));
			break;
		case '?':
			usage(argv[0]);
			exit(1);
		default:
			printf("unhandled argument: %c\n", opt);
		}
	}

	if (config.dumb_terminal) {
		config.theme = "dumb";
	}

	config.num_rest_args = 0;
	config.rest_args = NULL;
	if (optind < argc) {
		config.num_rest_args = argc - optind;
		config.rest_args = malloc((argc - optind) * sizeof(char*));
		int i = 0;
		while (optind < argc) {
			config.rest_args[i++] = argv[optind++];
		}
	}

	if (config.num_scripts == 0 && config.main_ns_name == NULL && config.num_rest_args == 0) {
		config.repl = true;
	}

	if (config.main_ns_name != NULL && config.repl) {
		printf("Only one main-opt can be specified.\n");
		exit(1);
	}

	config.is_tty = isatty(STDIN_FILENO) == 1;

	JSGlobalContextRef ctx = JSGlobalContextCreate(NULL);
	global_ctx = ctx;
	cljs_engine_init(ctx);

	// Process init arguments

	for (int i = 0; i < config.num_scripts; i++) {
		// TODO: exit if not successfull
		struct script script = config.scripts[i];
		evaluate_source(ctx, script.type, script.source, script.expression, false, NULL, config.theme, true);
	}

	// Process main arguments

	if (config.main_ns_name != NULL) {
		run_main_in_ns(ctx, config.main_ns_name, config.num_rest_args, config.rest_args);
	} else if (!config.repl && config.num_rest_args > 0) {
		char *path = config.rest_args[0];

		struct script script;
		if (strcmp(path, "-") == 0) {
			char *source = read_all(stdin);
			script.type = "text";
			script.source = source;
			script.expression = false;
		} else {
			script.type = "path";
			script.source = path;
			script.expression = false;
		}

		evaluate_source(ctx, script.type, script.source, script.expression, false, NULL, config.theme, true);
	} else if (config.repl) {
		if (!config.quiet) {
			banner();
		}

		run_repl(ctx);
	}

	return exit_value;
}
