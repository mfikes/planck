#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>

#include "bundle.h"
#include "engine.h"
#include "globals.h"
#include "io.h"
#include "legal.h"
#include "repl.h"
#include "str.h"
#include "theme.h"
#include "tasks.h"
#include "clock.h"

void ignore_sigpipe() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(struct sigaction));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    if (sigaction(SIGPIPE, &sa, 0) == -1) {
        perror("sigaction");
    }
}

void usage(char *program_name) {
    printf("\n");
    printf("Usage:  %s [init-opt*] [main-opt] [arg*]\n", program_name);
    printf("\n");
    printf("  With no options or args, runs an interactive Read-Eval-Print Loop\n");
    printf("\n");
    printf("  init options:\n");
    printf("    -i path, --init path        Load a file or resource\n");
    printf("    -e string, --eval string    Evaluate expressions in string; print non-nil\n");
    printf("                                values\n");
    printf("    -c cp, --classpath cp       Use colon-delimited cp for source directories\n");
    printf("                                and JARs. PLANCK_CLASSPATH env var may be used\n");
    printf("                                instead.\n");
    printf("    -D dep, --dependencies dep  Use comma-separated list of dependencies to\n");
    printf("                                look for in the local Maven repository.\n");
    printf("                                Dependencies should be specified in the form\n");
    printf("                                SYM:VERSION (e.g.: foo/bar:1.2.3).\n");
    printf("    -L path, --local-repo path  Path to the local Maven repository where Planck\n");
    printf("                                will look for dependencies. Defaults to\n");
    printf("                                ~/.m2/repository.\n");
    printf("    -K, --auto-cache            Create and use .planck_cache dir for cache\n");
    printf("    -k path, --cache path       If dir exists at path, use it for cache\n");
    printf("    -q, --quiet                 Quiet mode\n");
    printf("    -v, --verbose               Emit verbose diagnostic output\n");
    printf("    -d, --dumb-terminal         Disable line editing / VT100 terminal control\n");
    printf("    -t theme, --theme theme     Set the color theme\n");
    printf("    -n x, --socket-repl x       Enable socket REPL where x is port or IP:port\n");
    printf("    -s, --static-fns            Generate static dispatch function calls\n");
    printf("    -f, --fn-invoke-direct      Do not not generate .call(null...) calls\n");
    printf("                                for unknown functions, but instead direct\n");
    printf("                                invokes via f(a0,a1...).\n");
    printf("    -O x, --optimizations x     Closure compiler level applied to source loaded\n");
    printf("                                from namespaces: none, whitespace, or simple.\n");
    printf("    -A x, --checked-arrays x    Enables checked arrays where x is either warn\n");
    printf("                                or error.\n");
    printf("    -a, --elide-asserts         Set *assert* to false to remove asserts\n");
    printf("\n");
    printf("  main options:\n");
    printf("    -m ns-name, --main ns-name Call the -main function from a namespace with\n");
    printf("                               args\n");
    printf("    -r, --repl                 Run a repl\n");
    printf("    path                       Run a script from a file or resource\n");
    printf("    -                          Run a script from standard input\n");
    printf("    -h, -?, --help             Print this help message and exit\n");
    printf("    -l, --legal                Show legal info (licenses and copyrights)\n");
    printf("    -V, --version              Show version and exit\n");
    printf("\n");
    printf("  operation:\n");
    printf("\n");
    printf("    - Enters the cljs.user namespace\n");
    printf("    - Binds *command-line-args* to a seq of strings containing command line\n");
    printf("      args that appear after any main option\n");
    printf("    - Runs all init options in order\n");
    printf("    - Calls a -main function or runs a repl or script if requested\n");
    printf("\n");
    printf("  The init options may be repeated and mixed freely, but must appear before\n");
    printf("  any main option.\n");
    printf("\n");
    printf("  Paths may be absolute or relative in the filesystem or relative to\n");
    printf("  classpath. Classpath-relative paths have prefix of @ or @/\n");
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
        return strdup("(Unknown)");
    }
}

struct config config;
int exit_value = 0;
bool return_termsize = false;

void banner() {
    printf("ClojureScript %s\n", config.clojurescript_version);
}

char *ensure_trailing_slash(char *s) {
    if (str_has_suffix(s, "/") == 0) {
        return strdup(s);
    } else {
        return str_concat(s, "/");
    }
}

char *fully_qualify(char *cwd, char *path) {
    if (cwd && path && str_has_prefix(path, "/") != 0) {
        return str_concat(cwd, path);
    } else {
        return strdup(path);
    }
}

char *get_current_working_dir() {
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        return ensure_trailing_slash(cwd);
    }
    return NULL;
}

char *calculate_dependencies_classpath(char *dependencies, char *local_repo) {

    char *paths[1024];
    size_t ndx = 0;

    char *saveptr;
    char *dependency = strtok_r(dependencies, ",", &saveptr);
    while (dependency != NULL) {
        char *saveptr2;
        char *sym = strtok_r(dependency, ":", &saveptr2);
        char *version = strtok_r(NULL, ":", &saveptr2);

        char *saveptr3;
        char *group = strtok_r(sym, "/", &saveptr3);
        char *p = group;
        while (*p) {
            if (*p == '.') {
                *p = '/';
            }
            p++;
        }
        char *artifact = strtok_r(NULL, "/", &saveptr3);
        if (artifact == NULL) {
            artifact = group;
        }

        char path[PATH_MAX];
        sprintf(path, "%s/%s/%s/%s/%s-%s.jar", local_repo, group, artifact, version, artifact, version);

        paths[ndx++] = strdup(path);

        dependency = strtok_r(NULL, ",", &saveptr);
    }

    char *result = "";

    size_t n = 0;
    for (;;) {
        result = str_concat(result, paths[n]);
        if (++n < ndx) {
            result = str_concat(result, ":");
        } else {
            break;
        }
    }

    return result;
}

void init_classpath(char *classpath) {

    char *cwd = get_current_working_dir();

    char *source = strtok(classpath, ":");
    while (source != NULL) {
        char *type = "src";
        if (str_has_suffix(source, ".jar") == 0) {
            type = "jar";
        }

        config.num_src_paths += 1;
        config.src_paths = realloc(config.src_paths, config.num_src_paths * sizeof(struct src_path));
        config.src_paths[config.num_src_paths - 1].type = type;
        config.src_paths[config.num_src_paths - 1].archive = NULL;
        config.src_paths[config.num_src_paths - 1].blacklisted = false;
        if (strcmp(type, "jar") == 0) {
            config.src_paths[config.num_src_paths - 1].path = fully_qualify(cwd, source);
        } else {
            char *with_trailing_slash = ensure_trailing_slash(source);
            config.src_paths[config.num_src_paths - 1].path = fully_qualify(cwd, with_trailing_slash);
            free(with_trailing_slash);
        }

        source = strtok(NULL, ":");
    }

    free(cwd);
}

void print_usage_error(char *error_message, char *program_name) {
    printf("%s: %s", program_name, error_message);
    usage(program_name);
}

void err_cache_path(char *program_name) {
    print_usage_error("At most one of -k/--cache or -K/--auto-cache may be specified.", program_name);
}

void split_path_file(char** p, char** f, char *pf) {
    char *slash = pf, *next;
    while ((next = strpbrk(slash + 1, "\\/"))) slash = next;
    if (pf != slash) slash++;
    *p = strndup(pf, slash - pf);
    *f = strdup(slash);
}

void dump_sdk(char* target_path) {
    if (mkdir(target_path, 0755) < 0) {
        fprintf(stderr, "Could not create %s: %s\n", target_path, strerror(errno));
        exit(1);
    }
    char* manifest = bundle_get_contents("bundled_sdk_manifest.txt");
    char *path = strtok(manifest, "\n");
    while (path != NULL) {
        char full_path[PATH_MAX];
        snprintf(full_path, PATH_MAX, "%s/%s", target_path, path);

        char *p = NULL;
        char *f = NULL;
        split_path_file(&p, &f, full_path);
        if (mkdir_parents(p) < 0) {
            fprintf(stderr, "Could not create %s: %s\n", p, strerror(errno));
            exit(1);
        }

        char* contents = bundle_get_contents(path);
        write_contents(full_path, contents);

        path = strtok(NULL, "\n");
    }
}

bool should_ignore_arg(const char *opt) {
    if (opt[0] != '-') {
        return false;
    }

    // safely ignore any long opt
    if (opt[1] == '-') {
        return true;
    }

    // opt is a short opt or clump of short opts. If the clump
    // ends with i, e, m, c, n, k, t, S, A, O, D, or L
    // then this opt takes an argument.
    int idx = 0;
    char c = 0;
    char last_c = 0;
    while ((c = opt[idx]) != '\0') {
        last_c = c;
        idx++;
    }

    return (last_c == 'i' ||
            last_c == 'e' ||
            last_c == 'm' ||
            last_c == 'c' ||
            last_c == 'n' ||
            last_c == 'k' ||
            last_c == 't' ||
            last_c == 'S' ||
            last_c == 'A' ||
            last_c == 'O' ||
            last_c == 'D' ||
            last_c == 'L');
}

void control_FTL_JIT() {

    /* Recent versions of JavaScriptCore are crashing in FTL JIT. At least try
     * to avoid this on macOS for now: If on macOS 10.13.4 (or a later 10.13 version)
     * and JSC_useFTLJIT env var not set, disable FTL JIT. If not on macOS,
     * disable FTL JIT if JSC_useFTLJIT env var not set. */


    if (getenv("JSC_useFTLJIT") == NULL) {

#ifdef __APPLE__
        FILE *fp = fopen("/System/Library/CoreServices/SystemVersion.plist", "r");
        char *version_info = read_all(fp);
        fclose(fp);

        char *version_str = strstr(version_info, "<string>10.13");
        if (version_str) {
            version_str += 14;

            int version = atoi(version_str);

            if (version >= 4) {
                putenv("JSC_useFTLJIT=false");
            }
        }

        free(version_info);
#else
        putenv("JSC_useFTLJIT=false");
#endif
    }

}

int main(int argc, char **argv) {

    control_FTL_JIT();

    ignore_sigpipe();

    // A bare hyphen or a script path not preceded by -[iems] are the two types of mainopt not detected
    // by getopt_long(). If one of those two things is found, everything afterward is a *command-line-args* arg.
    // If neither is found, then the first mainopt will be found with getopt_long, and *command-line-args* args
    // will begin at optind + 1.

    int index_of_script_path_or_hyphen = argc;

    int i;
    for (i = 1; i < argc; i++) {
        char* arg = argv[i];

        if (strcmp("-", arg) == 0) {
            // A bare dash means "run a script from standard input." Bind everything after the dash to *command-line-args*.
            index_of_script_path_or_hyphen = i;
            break;
        } else if (arg[0] != '-') {
            // This could be a script path. If it is, bind everything after the path to *command-line-args*.
            char* previous_opt = argv[i - 1];
            if (!should_ignore_arg(previous_opt)) {
                index_of_script_path_or_hyphen = i;
                break;
            }
        }
    }

    config.verbose = false;
    config.quiet = false;
    config.repl = false;
    config.javascript = false;
    config.checked_arrays = NULL;
    config.static_fns = false;
    config.elide_asserts = false;
    config.optimizations = "none";
    config.cache_path = NULL;
    config.theme = NULL;
    config.dumb_terminal = false;

    config.out_path = NULL;
    config.num_src_paths = 0;
    config.src_paths = NULL;
    config.num_scripts = 0;
    config.scripts = NULL;

    config.main_ns_name = NULL;

    config.socket_repl_port = 0;
    config.socket_repl_host = NULL;

    config.clojurescript_version = get_cljs_version();

    char *classpath = NULL;
    char *dependencies = NULL;
    char *local_repo = NULL;

    struct option long_options[] = {
            {"help",             no_argument,       NULL, 'h'},
            {"version",          no_argument,       NULL, 'V'},
            {"dump-sdk",         required_argument, NULL, 'S'},
            {"legal",            no_argument,       NULL, 'l'},
            {"verbose",          no_argument,       NULL, 'v'},
            {"quiet",            no_argument,       NULL, 'q'},
            {"repl",             no_argument,       NULL, 'r'},
            {"checked-arrays",   required_argument, NULL, 'A'},
            {"static-fns",       no_argument,       NULL, 's'},
            {"fn-invoke-direct", no_argument,       NULL, 'f'},
            {"optimizations",    required_argument, NULL, 'O'},
            {"elide-asserts",    no_argument,       NULL, 'a'},
            {"cache",            required_argument, NULL, 'k'},
            {"eval",             required_argument, NULL, 'e'},
            {"theme",            required_argument, NULL, 't'},
            {"socket-repl",      required_argument, NULL, 'n'},
            {"dumb-terminal",    no_argument,       NULL, 'd'},
            {"classpath",        required_argument, NULL, 'c'},
            {"dependencies",     required_argument, NULL, 'D'},
            {"local-repo",       required_argument, NULL, 'L'},
            {"auto-cache",       no_argument,       NULL, 'K'},
            {"compile",          no_argument,       NULL, 'Z'},
            {"init",             required_argument, NULL, 'i'},
            {"main",             required_argument, NULL, 'm'},

            // development options
            {"javascript",       no_argument,       NULL, 'j'},
            {"out",              required_argument, NULL, 'o'},
            {"launch-time",      no_argument,       NULL, 'X'},

            {0, 0, 0,                                     0}
    };
    int opt, option_index;
    bool did_encounter_main_opt = false;
    // pass index_of_script_path_or_hyphen instead of argc to guarantee that everything
    // after a bare dash "-" or a script path gets passed as *command-line-args*
    while (!did_encounter_main_opt &&
           (opt = getopt_long(index_of_script_path_or_hyphen, argv, "O:Xh?VS:D:L:lvrA:sfak:je:t:n:dc:o:Ki:qm:", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'X':
                init_launch_timing();
                break;
            case 'h':
                printf("Planck %s\n", PLANCK_VERSION);
                usage(argv[0]);
                exit(0);
            case 'V':
                printf("%s\n", PLANCK_VERSION);
                exit(0);
            case 'S':
                dump_sdk(optarg);
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
                did_encounter_main_opt = true;
                config.repl = true;
                break;
            case 'A':
                if (!strcmp(optarg, "warn")) {
                    config.checked_arrays = "warn";
                } else if (!strcmp(optarg, "error")) {
                    config.checked_arrays = "error";
                } else {
                    print_usage_error("checked-arrays value must be warn or error", argv[0]);
                    return EXIT_FAILURE;
                }
                break;
            case 'O':
                if (!strcmp(optarg, "none")) {
                    config.optimizations = "none";
                } else if (!strcmp(optarg, "whitespace")) {
                    config.optimizations = "whitespace";
                } else if (!strcmp(optarg, "simple")) {
                    config.optimizations = "simple";
                } else {
                    print_usage_error("optimizations value must be none, whitespace, or simple", argv[0]);
                    return EXIT_FAILURE;
                }
                break;
            case 's':
                config.static_fns = true;
                break;
            case 'f':
                config.fn_invoke_direct = true;
                break;
            case 'a':
                config.elide_asserts = true;
                break;
            case 'k':
                if (config.cache_path) {
                    err_cache_path(argv[0]);
                    return EXIT_FAILURE;
                }
                config.cache_path = strdup(optarg);
                break;
            case 'K':
                if (config.cache_path) {
                    err_cache_path(argv[0]);
                    return EXIT_FAILURE;
                }
                config.cache_path = ".planck_cache";
                if (mkdir_p(config.cache_path) < 0) {
                    fprintf(stderr, "Could not create %s: %s\n", config.cache_path, strerror(errno));
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
                did_encounter_main_opt = true;
                config.main_ns_name = strdup(optarg);
                break;
            case 't':
                config.theme = strdup(optarg);
                break;
            case 'n':
                config.socket_repl_host = malloc(256);
                if (sscanf(optarg, "%255[^:]:%d", config.socket_repl_host, &config.socket_repl_port) != 2) {
                    strcpy(config.socket_repl_host, "localhost");
                    if (sscanf(optarg, "%d", &config.socket_repl_port) != 1) {
                        printf("Could not parse socket REPL params.\n");
                        free(config.socket_repl_host);
                        config.socket_repl_port = 0;
                    }
                }
                break;
            case 'd':
                config.dumb_terminal = true;
                break;
            case 'c': {
                classpath = strdup(optarg);
                break;
            }
            case 'D': {
                dependencies = strdup(optarg);
                break;
            }
            case 'L': {
                local_repo = strdup(optarg);
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

    display_launch_timing("parse opts");

    if (config.cache_path) {
        if (access(config.cache_path, W_OK) != 0) {
            fprintf(stderr, "Warning: Unable to write to cache directory.\n\n");
        }
    }

    display_launch_timing("check cache path");

    char *dependencies_classpath = NULL;
    if (dependencies) {
        if (!local_repo) {
            char *home = getenv("HOME");
            if (home != NULL) {
                local_repo = malloc(PATH_MAX);
                sprintf(local_repo, "%s/.m2/repository", home);
            }
        }
        if (local_repo) {
            dependencies_classpath = calculate_dependencies_classpath(dependencies, local_repo);
            if (classpath) {
                classpath = str_concat(classpath, ":");
                classpath = str_concat(classpath, dependencies_classpath);
            } else {
                classpath = dependencies_classpath;
            }
        }
    }

    if (classpath) {
        init_classpath(classpath);
    }

    if (config.num_src_paths == 0) {
        char *classpath = getenv("PLANCK_CLASSPATH");
        if (classpath) {
            init_classpath(classpath);
        }
    }

    display_launch_timing("init classpath");

    if (config.dumb_terminal) {
        config.theme = "plain";
    } else {
        if (!config.theme) {
            config.theme = default_theme_for_terminal();
        }
    }

    config.num_rest_args = 0;
    config.rest_args = NULL;
    if (optind < argc) {
        config.num_rest_args = (size_t) (argc - optind);
        config.rest_args = malloc((argc - optind) * sizeof(char *));
        int i = 0;
        while (optind < argc) {
            config.rest_args[i++] = argv[optind++];
        }
    }

    if (config.num_scripts == 0 && config.main_ns_name == NULL && config.num_rest_args == 0) {
        config.repl = true;
    }

    if (!check_theme(config.theme)) {
        exit(1);
    }

    display_launch_timing("check theme");

    if (config.main_ns_name != NULL && config.repl) {
        print_usage_error("Only one main-opt can be specified.", argv[0]);
        return EXIT_FAILURE;
    }

    config.is_tty = isatty(STDIN_FILENO) == 1;

    display_launch_timing("check tty");

    engine_init();

    // Process init arguments
    
    for (i = 0; i < config.num_scripts; i++) {
        struct script script = config.scripts[i];
        evaluate_source(script.type, script.source, script.expression, false, NULL, config.theme, true, 0);
        if (exit_value != EXIT_SUCCESS) {
            return exit_value;
        }
    }

    // Process main arguments

    if (config.main_ns_name != NULL) {
        run_main_in_ns(config.main_ns_name, config.num_rest_args, config.rest_args);
    } else if (!config.repl && config.num_rest_args > 0) {
        char *path = config.rest_args[0];
        config.rest_args++;
        config.num_rest_args--;

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

#ifdef JAVASCRIPT_CORE_3
        // These two lines appear to work around a bad bug where things crash on Linux with JSC 3
        // when running planck in non-REPL mode (executing a script)
        evaluate_source("text", "nil", true, false, NULL, config.theme, true, 0);
        evaluate_source("text", "(require 'planck.repl)", true, false, NULL, config.theme, true, 0);
#endif

        evaluate_source(script.type, script.source, script.expression, false, NULL, config.theme, true, 0);
    } else if (config.repl) {
        if (!config.quiet) {
            banner();
        }

        run_repl();
    }

    if (!config.repl && !config.main_ns_name) {
        run_main_cli_fn();
    }

    if (exit_value == EXIT_SUCCESS) {
        block_until_tasks_complete();
    }

    if (exit_value == EXIT_SUCCESS_INTERNAL) {
        exit_value = EXIT_SUCCESS;
    }

    engine_shutdown();

    return exit_value;
}
