#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>

#include <JavaScriptCore/JavaScript.h>

#include "bundle.h"
#include "globals.h"
#include "io.h"
#include "jsc_utils.h"
#include "str.h"
#include "archive.h"
#include "file.h"
#include "timers.h"
#include "engine.h"
#include "repl.h"
#include "clock.h"
#include "sockets.h"
#include "tasks.h"
#include "linenoise.h"

JSValueRef make_error_with_errno(JSContextRef ctx) {
    JSValueRef arguments[1];
    arguments[0] = c_string_to_value(ctx, strerror(errno));
    return JSObjectMakeError(ctx, 1, arguments, NULL);
}

#define CONSOLE_LOG_BUF_SIZE 1000
char console_log_buf[CONSOLE_LOG_BUF_SIZE];

extern char **environ;

JSValueRef function_console_stdout(JSContextRef ctx, JSObjectRef function, JSObjectRef this_object,
                                   size_t argc, JSValueRef const *args, JSValueRef *exception) {
    int i;
    for (i = 0; i < argc; i++) {
        if (i > 0) {
            fprintf(stdout, " ");
        }

        JSStringRef str = to_string(ctx, args[i]);
        JSStringGetUTF8CString(str, console_log_buf, CONSOLE_LOG_BUF_SIZE);
        fprintf(stdout, "%s", console_log_buf);
    }
    fprintf(stdout, "\n");

    return JSValueMakeUndefined(ctx);
}

JSValueRef function_console_stderr(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                   size_t argc, JSValueRef const *args, JSValueRef *exception) {
    int i;
    for (i = 0; i < argc; i++) {
        if (i > 0) {
            fprintf(stderr, " ");
        }

        JSStringRef str = to_string(ctx, args[i]);
        JSStringGetUTF8CString(str, console_log_buf, CONSOLE_LOG_BUF_SIZE);
        fprintf(stderr, "%s", console_log_buf);
    }
    fprintf(stderr, "\n");

    return JSValueMakeUndefined(ctx);
}

JSValueRef function_read_file(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                              size_t argc, const JSValueRef args[], JSValueRef *exception) {
    // TODO: implement fully

    if (argc == 1 && JSValueGetType(ctx, args[0]) == kJSTypeString) {
        char path[PATH_MAX];
        JSStringRef path_str = JSValueToStringCopy(ctx, args[0], NULL);
        assert(JSStringGetLength(path_str) < PATH_MAX);
        JSStringGetUTF8CString(path_str, path, PATH_MAX);
        JSStringRelease(path_str);

        // debug_print_value("read_file", ctx, args[0]);

        time_t last_modified = 0;
        char *contents = get_contents(path, &last_modified);
        if (contents != NULL) {
            JSStringRef contents_str = JSStringCreateWithUTF8CString(contents);
            free(contents);

            JSValueRef res[2];
            res[0] = JSValueMakeString(ctx, contents_str);
            res[1] = JSValueMakeNumber(ctx, last_modified);
            return JSObjectMakeArray(ctx, 2, res, NULL);
        }
    }

    return JSValueMakeNull(ctx);
}

JSValueRef function_load(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                         size_t argc, const JSValueRef args[], JSValueRef *exception) {
    // TODO: implement fully

    if (argc == 1 && JSValueGetType(ctx, args[0]) == kJSTypeString) {
        char path[PATH_MAX];
        JSStringRef path_str = JSValueToStringCopy(ctx, args[0], NULL);
        assert(JSStringGetLength(path_str) < PATH_MAX);
        JSStringGetUTF8CString(path_str, path, PATH_MAX);
        JSStringRelease(path_str);

        // debug_print_value("load", ctx, args[0]);

        time_t last_modified = 0;
        char *contents = NULL;
        char *loaded_path = strdup(path);
        char *loaded_type = NULL;
        char *loaded_location = NULL;

        bool developing = (config.num_src_paths == 1 &&
                           strcmp(config.src_paths[0].type, "src") == 0 &&
                           str_has_suffix(config.src_paths[0].path, "/planck-cljs/src/") == 0);

        if (!developing) {
            contents = bundle_get_contents(path);
            loaded_type = "bundled";
            last_modified = 0;
        }

        // load from classpath
        if (contents == NULL) {
            int i;
            for (i = 0; i < config.num_src_paths; i++) {
                if (config.src_paths[i].blacklisted) {
                    continue;
                }

                char *type = config.src_paths[i].type;
                char *location = config.src_paths[i].path;

                if (strcmp(type, "src") == 0) {
                    char *full_path = str_concat(location, path);
                    contents = get_contents(full_path, &last_modified);
                    if (contents != NULL) {
                        free(loaded_path);
                        loaded_path = strdup(full_path);
                        loaded_type = type;
                        loaded_location = location;
                    }
                    free(full_path);
                } else if (strcmp(type, "jar") == 0) {
                    struct stat file_stat;
                    if (stat(location, &file_stat) == 0) {
                        char *error_msg = NULL;
                        if (!config.src_paths[i].archive) {
                            config.src_paths[i].archive = open_archive(location, &error_msg);
                            if (error_msg) {
                                engine_print(error_msg);
                                engine_print("\n");
                                free(error_msg);
                            }
                        }
                        if (config.src_paths[i].archive) {
                            contents_zip_t contents_zip;
                            contents_zip = get_contents_zip(config.src_paths[i].archive, path,
                                                            &last_modified, &error_msg);
                            contents = (char *) contents_zip.payload;
                            if (!contents && error_msg) {
                                engine_print(error_msg);
                                engine_print("\n");
                                free(error_msg);
                            }
                        }
                        loaded_type = type;
                        loaded_location = location;
                    } else {
                        engine_perror(location);
                        config.src_paths[i].blacklisted = true;
                    }
                }

                if (contents != NULL) {
                    break;
                }
            }
        }

        // load from out/
        if (contents == NULL) {
            if (config.out_path != NULL) {
                char *full_path = str_concat(config.out_path, path);
                contents = get_contents(full_path, &last_modified);
                free(full_path);
            }
        }

        if (developing && contents == NULL) {
            contents = bundle_get_contents(path);
            last_modified = 0;
        }

        if (contents != NULL) {
            JSStringRef contents_str = JSStringCreateWithUTF8CString(contents);
            free(contents);
            JSStringRef loaded_path_str = JSStringCreateWithUTF8CString(loaded_path);
            free(loaded_path);
            JSStringRef loaded_type_str = JSStringCreateWithUTF8CString(loaded_type);
            JSStringRef loaded_location_str = JSStringCreateWithUTF8CString(loaded_location);


            JSValueRef res[5];
            res[0] = JSValueMakeString(ctx, contents_str);
            res[1] = JSValueMakeNumber(ctx, last_modified);
            res[2] = JSValueMakeString(ctx, loaded_path_str);
            res[3] = JSValueMakeString(ctx, loaded_type_str);
            res[4] = JSValueMakeString(ctx, loaded_location_str);
            return JSObjectMakeArray(ctx, 5, res, NULL);
        }

        free(loaded_path);
    }

    return JSValueMakeNull(ctx);
}

JSValueRef function_load_all_files(const char* filename, JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                         size_t argc, const JSValueRef args[], JSValueRef *exception) {
    size_t num_files = 0;
    char **paths = NULL;
    char **sources = NULL;

    if (argc == 0) {
        int i;
        for (i = 0; i < config.num_src_paths; i++) {
            if (config.src_paths[i].blacklisted) {
                continue;
            }
            char *type = config.src_paths[i].type;
            char *location = config.src_paths[i].path;

            if (strcmp(type, "jar") == 0) {
                struct stat file_stat;
                if (stat(location, &file_stat) == 0) {
                    char *error_msg = NULL;
                    if (!config.src_paths[i].archive) {
                        config.src_paths[i].archive = open_archive(location, &error_msg);
                        if (error_msg) {
                            engine_print(error_msg);
                            engine_print("\n");
                            free(error_msg);
                        }
                    }
                    if (config.src_paths[i].archive) {
                        contents_zip_t contents_zip;
                        contents_zip = get_contents_zip(config.src_paths[i].archive, filename,
                                                        NULL, &error_msg);
                        char *source = (char *) contents_zip.payload;
                        if (source != NULL) {
                            num_files += 1;
                            paths = realloc(paths, num_files * sizeof(char *));
                            sources = realloc(sources, num_files * sizeof(char *));
                            char buffer[1024];
                            snprintf(buffer, 1024, "jar:file://%s!/%s", location, filename);
                            paths[num_files - 1] = strdup(buffer);
                            sources[num_files - 1] = source;
                        } else {
                            if (error_msg) {
                                engine_print(error_msg);
                                engine_print("\n");
                                free(error_msg);
                            }
                        }
                    }
                } else {
                    engine_perror(location);
                    config.src_paths[i].blacklisted = true;
                }
            } else {
                char *full_path = str_concat(location, filename);
                char *source = get_contents(full_path, NULL);
                if (source != NULL) {
                    num_files += 1;
                    paths = realloc(paths, num_files * sizeof(char *));
                    sources = realloc(sources, num_files * sizeof(char *));
                    char buffer[1024];
                    snprintf(buffer, 1024, "file://%s%s", location, filename);
                    paths[num_files - 1] = strdup(buffer);
                    sources[num_files - 1] = source;
                }
                free(full_path);
            }
        }
    }

    JSValueRef files[num_files];
    int i;
    for (i = 0; i < num_files; i++) {
        JSValueRef res[2];
        JSStringRef path = JSStringCreateWithUTF8CString(paths[i]);
        res[0] = JSValueMakeString(ctx, path);
        JSStringRef source = JSStringCreateWithUTF8CString(sources[i]);
        res[1] = JSValueMakeString(ctx, source);
        files[i] = JSObjectMakeArray(ctx, 2, res, NULL);
        free(paths[i]);
        free(sources[i]);
    }
    free(paths);
    free(sources);

    return JSObjectMakeArray(ctx, num_files, files, NULL);
}

JSValueRef function_load_deps_cljs_files(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                         size_t argc, const JSValueRef args[], JSValueRef *exception) {
    return function_load_all_files("deps.cljs", ctx, function, thisObject, argc, args, exception);
}

JSValueRef function_load_data_readers_files(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                         size_t argc, const JSValueRef args[], JSValueRef *exception) {
    return function_load_all_files("data_readers.cljc", ctx, function, thisObject, argc, args, exception);
}

JSValueRef function_load_from_jar(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                  size_t argc, const JSValueRef args[], JSValueRef *exception) {

    if (argc == 3
        && JSValueGetType(ctx, args[0]) == kJSTypeString
        && JSValueGetType(ctx, args[1]) == kJSTypeString
        && JSValueGetType(ctx, args[2]) == kJSTypeBoolean) {

        char jar_path[PATH_MAX];
        JSStringRef path_str = JSValueToStringCopy(ctx, args[0], NULL);
        assert(JSStringGetLength(path_str) < PATH_MAX);
        JSStringGetUTF8CString(path_str, jar_path, PATH_MAX);
        JSStringRelease(path_str);

        char resource_path[PATH_MAX];
        JSStringRef resource_path_str = JSValueToStringCopy(ctx, args[1], NULL);
        assert(JSStringGetLength(resource_path_str) < PATH_MAX);
        JSStringGetUTF8CString(resource_path_str, resource_path, PATH_MAX);
        JSStringRelease(resource_path_str);

        bool convertToString = JSValueToBoolean(ctx, args[2]);

        contents_zip_t contents;
        contents.payload = NULL;
        JSStringRef contents_str = NULL;
        JSValueRef* contents_arr = NULL;

        char *error_msg = NULL;
        void *archive = open_archive(jar_path, &error_msg);
        if (!archive) {
            if (!error_msg) {
                error_msg = strdup("Failed to open JAR");
            }
        } else {
            contents = get_contents_zip(archive, resource_path, NULL, &error_msg);

            close_archive(archive);

            if (contents.payload != NULL) {
                if (convertToString) {
                   contents_str = JSStringCreateWithUTF8CString((char*)contents.payload);
                } else {
                    contents_arr = malloc(sizeof(JSValueRef) * contents.length);
                    for (size_t i=0; i<contents.length; i++) {
                        contents_arr[i] = JSValueMakeNumber(ctx, contents.payload[i]);
                    }
                }
                free(contents.payload);
            } else {
                if (!error_msg) {
                    error_msg = strdup("Resource not found in JAR");
                }
            }
        }

        JSStringRef error_msg_str = NULL;
        if (error_msg != NULL) {
            error_msg_str = JSStringCreateWithUTF8CString(error_msg);
            free(error_msg);
        }

        JSValueRef res[2];
        if (contents.payload) {
            if (convertToString) {
                res[0] = JSValueMakeString(ctx, contents_str);
            } else {
                res[0] = JSObjectMakeArray(ctx, contents.length, contents_arr, NULL);
                free(contents_arr);
            }
        } else {
            res[0] = JSValueMakeNull(ctx);
        }
        res[1] = error_msg ? JSValueMakeString(ctx, error_msg_str) : JSValueMakeNull(ctx);
        return JSObjectMakeArray(ctx, 2, res, NULL);

    }

    return JSValueMakeNull(ctx);
}

JSValueRef function_cache(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                          size_t argc, const JSValueRef args[], JSValueRef *exception) {
    if (argc == 4 &&
        JSValueGetType(ctx, args[0]) == kJSTypeString &&
        JSValueGetType(ctx, args[1]) == kJSTypeString &&
        (JSValueGetType(ctx, args[2]) == kJSTypeString
         || JSValueGetType(ctx, args[2]) == kJSTypeNull) &&
        (JSValueGetType(ctx, args[3]) == kJSTypeString
         || JSValueGetType(ctx, args[3]) == kJSTypeNull)) {
        // debug_print_value("cache", ctx, args[0]);

        char *cache_prefix = value_to_c_string(ctx, args[0]);
        char *source = value_to_c_string(ctx, args[1]);
        char *cache = value_to_c_string(ctx, args[2]);
        char *sourcemap = value_to_c_string(ctx, args[3]);

        char *suffix = NULL;
        int max_suffix_len = 20;
        size_t prefix_len = strlen(cache_prefix);
        char *path = malloc((prefix_len + max_suffix_len) * sizeof(char));
        memset(path, 0, prefix_len + max_suffix_len);

        suffix = ".js";
        strcpy(path, cache_prefix);
        strcat(path, suffix);
        write_contents(path, source);

        suffix = ".cache.json";
        strcpy(path, cache_prefix);
        strcat(path, suffix);
        write_contents(path, cache);

        suffix = ".js.map.json";
        strcpy(path, cache_prefix);
        strcat(path, suffix);
        if (sourcemap) {
            write_contents(path, sourcemap);
        }

        free(cache_prefix);
        free(source);
        free(cache);
        free(sourcemap);

        free(path);
    }

    return JSValueMakeNull(ctx);
}

JSValueRef function_eval(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                         size_t argc, const JSValueRef args[], JSValueRef *exception) {
    JSValueRef val = NULL;

    if (argc == 2
        && JSValueGetType(ctx, args[0]) == kJSTypeString
        && JSValueGetType(ctx, args[1]) == kJSTypeString) {
        // debug_print_value("eval", ctx, args[0]);

        JSStringRef sourceRef = JSValueToStringCopy(ctx, args[0], NULL);
        JSStringRef pathRef = JSValueToStringCopy(ctx, args[1], NULL);

        JSEvaluateScript(ctx, sourceRef, NULL, pathRef, 0, &val);

        JSStringRelease(pathRef);
        JSStringRelease(sourceRef);
    }

    return val != NULL ? val : JSValueMakeNull(ctx);
}

JSValueRef function_get_term_size(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                  size_t argc, const JSValueRef args[], JSValueRef *exception) {
    if (return_termsize) {
        struct winsize w;
        int rv = ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
        JSValueRef arguments[2];
        arguments[0] = JSValueMakeNumber(ctx, rv == -1 ? 25 : w.ws_row);
        arguments[1] = JSValueMakeNumber(ctx, rv == -1 ? 80 : w.ws_col);
        return JSObjectMakeArray(ctx, 2, arguments, NULL);
    } else {
        return JSValueMakeNull(ctx);
    }
}

JSValueRef function_print_fn(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                             size_t argc, const JSValueRef args[], JSValueRef *exception) {

    if (!should_keep_running()) {
        fprintf(stdout, "\x1b[m\n");
        fflush(stdout);
        *exception = JSValueMakeNull(ctx);
        return NULL;
    }

    if (argc == 1) {
        char *str = value_to_c_string_ext(ctx, args[0], true);

        fprintf(stdout, "%s", str);
        fflush(stdout);

        free(str);
    }

    return JSValueMakeNull(ctx);
}

JSValueRef function_print_err_fn(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                 size_t argc, const JSValueRef args[], JSValueRef *exception) {

    if (!should_keep_running()) {
        fprintf(stderr, "\x1b[m\n");
        *exception = JSValueMakeNull(ctx);
        return NULL;
    }

    if (argc == 1) {
        char *str = value_to_c_string_ext(ctx, args[0], true);

        fprintf(stderr, "%s", str);
        fflush(stderr);

        free(str);
    }

    return JSValueMakeNull(ctx);
}

JSValueRef function_exit_with_value(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                    size_t argc, const JSValueRef args[], JSValueRef *exception) {
    if (argc == 1 && JSValueGetType(ctx, args[0]) == kJSTypeNumber) {
        exit_value = (int) JSValueToNumber(ctx, args[0], NULL);
        exit(exit_value);
    }
    return JSValueMakeNull(ctx);
}

JSValueRef function_raw_read_stdin(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                   size_t argc, const JSValueRef args[], JSValueRef *exception) {
    char buf[1024 + 1];

    size_t n = fread(buf, 1, config.is_tty ? 1 : 1024, stdin);
    if (n > 0) {
        buf[n] = '\0';
        return c_string_to_value(ctx, buf);
    }

    return JSValueMakeNull(ctx);
}

JSValueRef function_raw_write_stdout(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                     size_t argc, const JSValueRef args[], JSValueRef *exception) {
    if (argc == 1 && JSValueGetType(ctx, args[0]) == kJSTypeString) {
        char *s = value_to_c_string(ctx, args[0]);
        fprintf(stdout, "%s", s);
        free(s);
    }

    return JSValueMakeNull(ctx);
}

JSValueRef function_raw_flush_stdout(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                     size_t argc, const JSValueRef args[], JSValueRef *exception) {
    fflush(stdout);

    return JSValueMakeNull(ctx);
}

JSValueRef function_raw_write_stderr(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                     size_t argc, const JSValueRef args[], JSValueRef *exception) {
    if (argc == 1 && JSValueGetType(ctx, args[0]) == kJSTypeString) {
        char *s = value_to_c_string(ctx, args[0]);
        fprintf(stderr, "%s", s);
        free(s);
    }

    return JSValueMakeNull(ctx);
}

JSValueRef function_raw_flush_stderr(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                     size_t argc, const JSValueRef args[], JSValueRef *exception) {
    fflush(stderr);

    return JSValueMakeNull(ctx);
}

unsigned long hash(unsigned char *str) {
    unsigned long hash = 5381;
    int c;

    while ((c = *str++))
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */

    return hash;
}

static unsigned long loaded_goog_hashes[2048];
static size_t count_loaded_goog_hashes = 0;

bool is_loaded(unsigned long h) {
    size_t i;
    for (i = 0; i < count_loaded_goog_hashes; ++i) {
        if (loaded_goog_hashes[i] == h) {
            return true;
        }
    }
    return false;
}

void add_loaded_hash(unsigned long h) {
    if (count_loaded_goog_hashes < 2048) {
        loaded_goog_hashes[count_loaded_goog_hashes++] = h;
    }
}

JSValueRef function_import_script(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                  size_t argc, const JSValueRef args[], JSValueRef *exception) {
    if (argc == 1 && JSValueGetType(ctx, args[0]) == kJSTypeString) {
        JSStringRef path_str_ref = JSValueToStringCopy(ctx, args[0], NULL);
        assert(JSStringGetLength(path_str_ref) < PATH_MAX);
        char tmp[PATH_MAX];
        tmp[0] = '\0';
        JSStringGetUTF8CString(path_str_ref, tmp, PATH_MAX);
        JSStringRelease(path_str_ref);

        bool can_skip_load = false;
        char *path = tmp;
        if (str_has_prefix(path, "goog/../") == 0) {
            path = path + 8;
        } else {
            unsigned long h = hash((unsigned char *) path);
            if (is_loaded(h)) {
                can_skip_load = true;
            } else {
                add_loaded_hash(h);
            }
        }

        if (!can_skip_load) {
            char *source = NULL;
            if (config.out_path == NULL) {
                source = bundle_get_contents(path);
            } else {
                char *full_path = str_concat(config.out_path, path);
                source = get_contents(full_path, NULL);
                free(full_path);
            }

            if (source != NULL) {
                evaluate_script(ctx, source, path);
                display_launch_timing(path);
                free(source);
            }
        }
    }

    return JSValueMakeUndefined(ctx);
}

descriptor_t descriptor_str_to_int(const char *s) {
    return (descriptor_t) atoll(s);
}

char *descriptor_int_to_str(descriptor_t i) {
    char *rv = malloc(21);
    sprintf(rv, "%llu", (unsigned long long) i);
    return rv;
}

JSValueRef function_file_reader_open(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                     size_t argc, const JSValueRef args[], JSValueRef *exception) {
    if (argc == 2
        && JSValueGetType(ctx, args[0]) == kJSTypeString) {

        char *path = value_to_c_string(ctx, args[0]);
        char *encoding = value_to_c_string(ctx, args[1]);

        descriptor_t descriptor = ufile_open_read(path, encoding);

        free(path);
        free(encoding);

        char *descriptor_str = descriptor_int_to_str(descriptor);
        JSValueRef rv = c_string_to_value(ctx, descriptor_str);
        free(descriptor_str);

        return rv;
    }

    return JSValueMakeNull(ctx);
}

JSValueRef function_file_reader_read(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                     size_t argc, const JSValueRef args[], JSValueRef *exception) {
    if (argc == 1
        && JSValueGetType(ctx, args[0]) == kJSTypeString) {

        char *descriptor = value_to_c_string(ctx, args[0]);

        JSStringRef result = ufile_read(descriptor_str_to_int(descriptor));

        free(descriptor);

        JSValueRef arguments[2];
        if (result != NULL) {
            arguments[0] = JSValueMakeString(ctx, result);
            JSStringRelease(result);
        } else {
            arguments[0] = JSValueMakeNull(ctx);
        }
        arguments[1] = JSValueMakeNull(ctx);
        return JSObjectMakeArray(ctx, 2, arguments, NULL);
    }

    return JSValueMakeNull(ctx);
}

JSValueRef function_file_reader_close(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                      size_t argc, const JSValueRef args[], JSValueRef *exception) {
    if (argc == 1
        && JSValueGetType(ctx, args[0]) == kJSTypeString) {

        char *descriptor = value_to_c_string(ctx, args[0]);
        ufile_close(descriptor_str_to_int(descriptor));
        free(descriptor);
    }
    return JSValueMakeNull(ctx);
}

JSValueRef function_file_writer_open(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                     size_t argc, const JSValueRef args[], JSValueRef *exception) {
    if (argc == 3
        && JSValueGetType(ctx, args[0]) == kJSTypeString
        && JSValueGetType(ctx, args[1]) == kJSTypeBoolean) {

        char *path = value_to_c_string(ctx, args[0]);
        bool append = JSValueToBoolean(ctx, args[1]);
        char *encoding = value_to_c_string(ctx, args[2]);

        uint64_t descriptor = ufile_open_write(path, append, encoding);

        free(path);
        free(encoding);

        char *descriptor_str = descriptor_int_to_str(descriptor);
        JSValueRef rv = c_string_to_value(ctx, descriptor_str);
        free(descriptor_str);

        return rv;
    }

    return JSValueMakeNull(ctx);
}

JSValueRef function_file_writer_write(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                      size_t argc, const JSValueRef args[], JSValueRef *exception) {
    if (argc == 2
        && JSValueGetType(ctx, args[0]) == kJSTypeString
        && JSValueGetType(ctx, args[1]) == kJSTypeString) {

        char *descriptor = value_to_c_string(ctx, args[0]);
        JSStringRef str_ref = JSValueToStringCopy(ctx, args[1], NULL);

        ufile_write(descriptor_str_to_int(descriptor), str_ref);

        free(descriptor);
        JSStringRelease(str_ref);
    }

    return JSValueMakeNull(ctx);
}

JSValueRef function_file_writer_flush(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                      size_t argc, const JSValueRef args[], JSValueRef *exception) {
    if (argc == 1
        && JSValueGetType(ctx, args[0]) == kJSTypeString) {

        char *descriptor = value_to_c_string(ctx, args[0]);
        ufile_flush(descriptor_str_to_int(descriptor));
        free(descriptor);
    }
    return JSValueMakeNull(ctx);
}

JSValueRef function_file_writer_close(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                      size_t argc, const JSValueRef args[], JSValueRef *exception) {
    if (argc == 1
        && JSValueGetType(ctx, args[0]) == kJSTypeString) {

        char *descriptor = value_to_c_string(ctx, args[0]);
        ufile_close(descriptor_str_to_int(descriptor));
        free(descriptor);
    }
    return JSValueMakeNull(ctx);
}

JSValueRef function_file_input_stream_open(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                           size_t argc, const JSValueRef args[], JSValueRef *exception) {
    if (argc == 1
        && JSValueGetType(ctx, args[0]) == kJSTypeString) {

        char *path = value_to_c_string(ctx, args[0]);

        uint64_t descriptor = file_open_read(path);

        free(path);

        char *descriptor_str = descriptor_int_to_str(descriptor);
        JSValueRef rv = c_string_to_value(ctx, descriptor_str);
        free(descriptor_str);

        return rv;
    }

    return JSValueMakeNull(ctx);
}

JSValueRef function_file_input_stream_read(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                           size_t argc, const JSValueRef args[], JSValueRef *exception) {

    static JSValueRef *charmap = NULL;
    if (!charmap) {
        charmap = malloc(256 * sizeof (JSValueRef));
        int i;
        for (i = 0; i < 256; i++) {
            charmap[i] = JSValueMakeNumber(ctx, i);
            JSValueProtect(ctx, charmap[i]);
        }
    }

    if (argc == 1
        && JSValueGetType(ctx, args[0]) == kJSTypeString) {

        char *descriptor = value_to_c_string(ctx, args[0]);

        size_t buf_size = 4096;
        uint8_t *buf = malloc(buf_size * sizeof(uint8_t));

        size_t read = file_read(descriptor_str_to_int(descriptor), buf_size, buf);

        free(descriptor);

        if (read) {
            // TODO distinguish between eof and error down in fread call and throw if errro
            JSValueRef arguments[read];
            int num_arguments = (int) read;
            int i;
            for (i = 0; i < num_arguments; i++) {
                arguments[i] = charmap[buf[i]];
            }

            return JSObjectMakeArray(ctx, num_arguments, arguments, NULL);
        }
    }

    return JSValueMakeNull(ctx);
}

JSValueRef function_file_input_stream_close(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                            size_t argc, const JSValueRef args[], JSValueRef *exception) {
    if (argc == 1
        && JSValueGetType(ctx, args[0]) == kJSTypeString) {

        char *descriptor = value_to_c_string(ctx, args[0]);
        file_close(descriptor_str_to_int(descriptor));
        free(descriptor);
    }
    return JSValueMakeNull(ctx);
}

JSValueRef function_file_output_stream_open(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                            size_t argc, const JSValueRef args[], JSValueRef *exception) {
    if (argc == 2
        && JSValueGetType(ctx, args[0]) == kJSTypeString
        && JSValueGetType(ctx, args[1]) == kJSTypeBoolean) {

        char *path = value_to_c_string(ctx, args[0]);
        bool append = JSValueToBoolean(ctx, args[1]);

        uint64_t descriptor = file_open_write(path, append);

        free(path);

        char *descriptor_str = descriptor_int_to_str(descriptor);
        JSValueRef rv = c_string_to_value(ctx, descriptor_str);
        free(descriptor_str);

        return rv;
    }

    return JSValueMakeNull(ctx);
}

JSValueRef function_file_output_stream_write(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                             size_t argc, const JSValueRef args[], JSValueRef *exception) {
    if (argc == 2
        && JSValueGetType(ctx, args[0]) == kJSTypeString
        && JSValueGetType(ctx, args[1]) == kJSTypeObject) {

        char *descriptor = value_to_c_string(ctx, args[0]);

        unsigned int count = (unsigned int) array_get_count(ctx, (JSObjectRef) args[1]);

        uint8_t* buf = malloc(sizeof(uint8_t) * count);
        if (!buf) {
            *exception = make_error_with_errno(ctx);
            return JSValueMakeNull(ctx);
        }

        unsigned int i;
        for (i = 0; i < count; i++) {
            JSValueRef v = array_get_value_at_index(ctx, (JSObjectRef) args[1], i);
            if (JSValueIsNumber(ctx, v)) {
                double n = JSValueToNumber(ctx, v, NULL);
                if (0 <= n && n <= 255) {
                    buf[i] = (uint8_t) n;
                } else {
                    fprintf(stderr, "Output stream value out of range %f", n);
                }
            } else {
                fprintf(stderr, "Output stream value not a number");
            }
        }

        file_write(descriptor_str_to_int(descriptor), count, buf);

        free(buf);
        free(descriptor);
    }

    return JSValueMakeNull(ctx);
}

JSValueRef function_file_output_stream_flush(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                             size_t argc, const JSValueRef args[], JSValueRef *exception) {
    if (argc == 1
        && JSValueGetType(ctx, args[0]) == kJSTypeString) {

        char *descriptor = value_to_c_string(ctx, args[0]);
        file_flush(descriptor_str_to_int(descriptor));
        free(descriptor);
    }
    return JSValueMakeNull(ctx);
}

JSValueRef function_file_output_stream_close(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                             size_t argc, const JSValueRef args[], JSValueRef *exception) {
    if (argc == 1
        && JSValueGetType(ctx, args[0]) == kJSTypeString) {

        char *descriptor = value_to_c_string(ctx, args[0]);
        file_close(descriptor_str_to_int(descriptor));
        free(descriptor);
    }
    return JSValueMakeNull(ctx);
}

JSValueRef function_mkdirs(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                           size_t argc, const JSValueRef args[], JSValueRef *exception) {
    if (argc == 1
        && JSValueGetType(ctx, args[0]) == kJSTypeString) {
        char *path = value_to_c_string(ctx, args[0]);
        int rv = mkdir_parents(path);
        free(path);
        
        if (rv == -1) {
            return JSValueMakeBoolean(ctx, false);
        }
    }
    return JSValueMakeBoolean(ctx, true);
}

JSValueRef function_delete_file(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                size_t argc, const JSValueRef args[], JSValueRef *exception) {
    if (argc == 1
        && JSValueGetType(ctx, args[0]) == kJSTypeString) {

        char *path = value_to_c_string(ctx, args[0]);
        remove(path);
        free(path);
    }
    return JSValueMakeNull(ctx);
}

JSValueRef function_copy_file(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                              size_t argc, const JSValueRef args[], JSValueRef *exception) {
    if (argc == 2
        && JSValueGetType(ctx, args[0]) == kJSTypeString
        && JSValueGetType(ctx, args[1]) == kJSTypeString) {

        char *src = value_to_c_string(ctx, args[0]);
        char *dst = value_to_c_string(ctx, args[1]);

        int rv = copy_file(src, dst);
        if (rv) {
            *exception = make_error_with_errno(ctx);
        }

        free(src);
        free(dst);
    }
    return JSValueMakeNull(ctx);
}

JSValueRef function_list_files(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                               size_t argc, const JSValueRef args[], JSValueRef *exception) {
    if (argc == 1
        && JSValueGetType(ctx, args[0]) == kJSTypeString) {

        char *path = value_to_c_string(ctx, args[0]);

        size_t capacity = 32;
        size_t count = 0;

        JSValueRef *paths = malloc(capacity * sizeof(JSValueRef));

        DIR *d = opendir(path);

        if (d) {
            size_t path_len = strlen(path);
            if (path_len && path[path_len - 1] == '/') {
                path[--path_len] = 0;
            }

            struct dirent *dir;
            while ((dir = readdir(d)) != NULL) {
                if (strcmp(dir->d_name, ".") && strcmp(dir->d_name, "..")) {

                    size_t buf_len = path_len + strlen(dir->d_name) + 2;
                    char *buf = malloc(buf_len);
                    snprintf(buf, buf_len, "%s/%s", path, dir->d_name);
                    JSValueRef path_ref = c_string_to_value(ctx, buf);
                    paths[count++] = path_ref;
                    JSValueProtect(ctx, path_ref);
                    free(buf);

                    if (count == capacity) {
                        capacity *= 2;
                        paths = realloc(paths, capacity * sizeof(JSValueRef));
                    }
                }
            }

            closedir(d);
        }

        JSValueRef rv = JSObjectMakeArray(ctx, count, paths, NULL);

        size_t i = 0;
        for (i=0; i<count; ++i) {
            JSValueUnprotect(ctx, paths[i]);
        }

        free(path);
        free(paths);

        return rv;
    }
    return JSValueMakeNull(ctx);
}

JSValueRef function_mktemp(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                           size_t argc, const JSValueRef args[], JSValueRef *exception) {
    if (argc == 3
        && JSValueGetType(ctx, args[0]) == kJSTypeString
        && JSValueGetType(ctx, args[1]) == kJSTypeString
        && JSValueGetType(ctx, args[2]) == kJSTypeBoolean) {

        bool directory = JSValueToBoolean(ctx, args[2]);

        char *tmpdir = getenv("TMPDIR");
        if (!tmpdir) {
            tmpdir = "/tmp";
        }

        char* prefix = value_to_c_string(ctx, args[0]);
        char* suffix = value_to_c_string(ctx, args[1]);

        char template[PATH_MAX];
        if (str_has_suffix(tmpdir, "/") == 0) {
            snprintf(template, PATH_MAX, "%s%sXXXXXX%s", tmpdir, prefix, suffix);
        } else {
            snprintf(template, PATH_MAX, "%s/%sXXXXXX%s", tmpdir, prefix, suffix);
        }

        char *temp_name;
        if (directory) {
            temp_name = mkdtemp(template);
        } else {
            int fd = mkstemps(template, strlen(suffix));
            if (fd != -1 && close(fd) != -1) {
                temp_name = template;
            } else {
                temp_name = NULL;
            }
        }

        free(suffix);
        free(prefix);

        if (temp_name) {
            return c_string_to_value(ctx, temp_name);
        } else {
            *exception = make_error_with_errno(ctx);
        }

    }
    return JSValueMakeNull(ctx);
}

JSValueRef function_is_directory(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                 size_t argc, const JSValueRef args[], JSValueRef *exception) {
    if (argc == 1
        && JSValueGetType(ctx, args[0]) == kJSTypeString) {

        char *path = value_to_c_string(ctx, args[0]);

        bool is_directory = false;

        struct stat file_stat;

        int retval = stat(path, &file_stat);

        free(path);

        if (retval == 0) {
            is_directory = S_ISDIR(file_stat.st_mode);
        }

        return JSValueMakeBoolean(ctx, is_directory);

    }
    return JSValueMakeNull(ctx);
}

JSValueRef function_fstat(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                          size_t argc, const JSValueRef args[], JSValueRef *exception) {
    if (argc == 1
        && JSValueGetType(ctx, args[0]) == kJSTypeString) {

        char *path = value_to_c_string(ctx, args[0]);

        struct stat file_stat;

        int retval = lstat(path, &file_stat);

        if (retval == 0) {
            JSObjectRef result = JSObjectMake(ctx, NULL, NULL);

            char *type = "unknown";
            if (S_ISDIR(file_stat.st_mode)) {
                type = "directory";
            } else if (S_ISREG(file_stat.st_mode)) {
                type = "file";
            } else if (S_ISLNK(file_stat.st_mode)) {
                type = "symbolic-link";
            } else if (S_ISSOCK(file_stat.st_mode)) {
                type = "socket";
            } else if (S_ISFIFO(file_stat.st_mode)) {
                type = "fifo";
            } else if (S_ISCHR(file_stat.st_mode)) {
                type = "character-special";
            } else if (S_ISBLK(file_stat.st_mode)) {
                type = "block-special";
            }

            JSObjectSetProperty(ctx, result, JSStringCreateWithUTF8CString("type"),
                                c_string_to_value(ctx, type),
                                kJSPropertyAttributeReadOnly, NULL);


            double device_id = (double) file_stat.st_rdev;
            if (device_id) {
                JSObjectSetProperty(ctx, result, JSStringCreateWithUTF8CString("device-id"),
                                    JSValueMakeNumber(ctx, device_id),
                                    kJSPropertyAttributeReadOnly, NULL);
            }

            double file_number = (double) file_stat.st_ino;
            if (file_number) {
                JSObjectSetProperty(ctx, result, JSStringCreateWithUTF8CString("file-number"),
                                    JSValueMakeNumber(ctx, file_number),
                                    kJSPropertyAttributeReadOnly, NULL);
            }

            JSObjectSetProperty(ctx, result, JSStringCreateWithUTF8CString("permissions"),
                                JSValueMakeNumber(ctx, (double) (ACCESSPERMS & file_stat.st_mode)),
                                kJSPropertyAttributeReadOnly, NULL);

            JSObjectSetProperty(ctx, result, JSStringCreateWithUTF8CString("reference-count"),
                                JSValueMakeNumber(ctx, (double) file_stat.st_nlink),
                                kJSPropertyAttributeReadOnly, NULL);

            JSObjectSetProperty(ctx, result, JSStringCreateWithUTF8CString("uid"),
                                JSValueMakeNumber(ctx, (double) file_stat.st_uid),
                                kJSPropertyAttributeReadOnly, NULL);

            struct passwd *uid_passwd = getpwuid(file_stat.st_uid);

            if (uid_passwd) {
                JSObjectSetProperty(ctx, result, JSStringCreateWithUTF8CString("uname"),
                                    c_string_to_value(ctx, uid_passwd->pw_name),
                                    kJSPropertyAttributeReadOnly, NULL);
            }

            JSObjectSetProperty(ctx, result, JSStringCreateWithUTF8CString("gid"),
                                JSValueMakeNumber(ctx, (double) file_stat.st_gid),
                                kJSPropertyAttributeReadOnly, NULL);

            struct group *gid_group = getgrgid(file_stat.st_gid);

            if (gid_group) {
                JSObjectSetProperty(ctx, result, JSStringCreateWithUTF8CString("gname"),
                                    c_string_to_value(ctx, gid_group->gr_name),
                                    kJSPropertyAttributeReadOnly, NULL);
            }

            JSObjectSetProperty(ctx, result, JSStringCreateWithUTF8CString("file-size"),
                                JSValueMakeNumber(ctx, (double) file_stat.st_size),
                                kJSPropertyAttributeReadOnly, NULL);

#ifdef __APPLE__
#define birthtime(x) x.st_birthtime
#else
#define birthtime(x) x.st_ctime
#endif

            JSObjectSetProperty(ctx, result, JSStringCreateWithUTF8CString("created"),
                                JSValueMakeNumber(ctx, 1000 * birthtime(file_stat)),
                                kJSPropertyAttributeReadOnly, NULL);

            JSObjectSetProperty(ctx, result, JSStringCreateWithUTF8CString("modified"),
                                JSValueMakeNumber(ctx, 1000 * file_stat.st_mtime),
                                kJSPropertyAttributeReadOnly, NULL);

            return result;
        }

        free(path);
    }
    return JSValueMakeNull(ctx);
}

JSValueRef function_read_password(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                  size_t argc, const JSValueRef args[], JSValueRef *exception) {
    if (argc == 1
        && JSValueGetType(ctx, args[0]) == kJSTypeString) {

        char *prompt = value_to_c_string(ctx, args[0]);

        char *pass = getpass(prompt);

        JSValueRef rv;

        if (pass) {
            rv = c_string_to_value(ctx, pass);
            memset(pass, 0, strlen(pass));
        } else {
            rv = JSValueMakeNull(ctx);
        }

        free(prompt);
        return rv;
    }
    return JSValueMakeNull(ctx);
}

void do_run_timeout(void *data) {

    unsigned long *timeout_data = data;

    acquire_eval_lock();
    JSValueRef args[1];
    args[0] = JSValueMakeNumber(ctx, (double)*timeout_data);
    free(timeout_data);

    static JSObjectRef run_timeout_fn = NULL;
    if (!run_timeout_fn) {
        run_timeout_fn = get_function("global", "PLANCK_RUN_TIMEOUT");
        JSValueProtect(ctx, run_timeout_fn);
    }
    JSObjectCallAsFunction(ctx, run_timeout_fn, NULL, 1, args, NULL);
    release_eval_lock();
}

static unsigned long timeout_id = 0;

JSValueRef function_set_timeout(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                size_t argc, const JSValueRef args[], JSValueRef *exception) {
    if (argc == 1
        && JSValueGetType(ctx, args[0]) == kJSTypeNumber) {

        int millis = (int) JSValueToNumber(ctx, args[0], NULL);

        if (timeout_id == 9007199254740991) {
            timeout_id = 0;
        } else {
            ++timeout_id;
        }

        JSValueRef rv = JSValueMakeNumber(ctx, (double)timeout_id);

        unsigned long *timeout_data = malloc(sizeof(unsigned long));
        *timeout_data = timeout_id;

        int err = signal_task_started();
        if (err) {
            engine_print_err_message("signal_task_started", err);
        }

        start_timer(millis, do_run_timeout, (void *) timeout_data);

        return rv;
    }
    return JSValueMakeNull(ctx);
}

void do_run_interval(void *data) {

    unsigned long *interval_data = data;

    acquire_eval_lock();
    JSValueRef args[1];
    args[0] = JSValueMakeNumber(ctx, (double)*interval_data);
    free(interval_data);

    static JSObjectRef run_interval_fn = NULL;
    if (!run_interval_fn) {
        run_interval_fn = get_function("global", "PLANCK_RUN_INTERVAL");
        JSValueProtect(ctx, run_interval_fn);
    }

    JSObjectCallAsFunction(ctx, run_interval_fn, NULL, 1, args, NULL);
    release_eval_lock();
}

static unsigned long interval_id = 0;

JSValueRef function_set_interval(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                 size_t argc, const JSValueRef args[], JSValueRef *exception) {
    if (argc == 2
        && JSValueGetType(ctx, args[0]) == kJSTypeNumber) {

        int millis = (int) JSValueToNumber(ctx, args[0], NULL);

        unsigned long curr_interval_id;

        if (JSValueIsNull(ctx, args[1])) {
            if (interval_id == 9007199254740991) {
                interval_id = 0;
            } else {
                ++interval_id;
            }
            curr_interval_id = interval_id;

            int err = signal_task_started();
            if (err) {
                engine_print_err_message("signal_task_started", err);
            }
        } else {
            curr_interval_id = (unsigned long) JSValueToNumber(ctx, args[1], NULL);
        }

        JSValueRef rv = JSValueMakeNumber(ctx, (double)curr_interval_id);

        unsigned long *interval_data = malloc(sizeof(unsigned long));
        *interval_data = curr_interval_id;

        start_timer(millis, do_run_interval, (void *) interval_data);

        return rv;
    }
    return JSValueMakeNull(ctx);
}

JSValueRef function_high_res_timer(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                   size_t argc, const JSValueRef args[], JSValueRef *exception) {

    return JSValueMakeNumber(ctx, 1e-6 * system_time());

}

typedef struct data_arrived_info {
    JSObjectRef data_arrived_cb;
} data_arrived_info_t;

conn_data_cb_ret_t *socket_conn_data_arrived(char *data, int sock, void *info) {

    data_arrived_info_t *data_arrived_info = info;

    acquire_eval_lock();
    JSValueRef args[2];
    args[0] = JSValueMakeNumber(ctx, sock);

    if (data) {
        // TODO what if we need bytes instead of dealing with an encoding?
        args[1] = JSValueMakeString(ctx, JSStringCreateWithUTF8CString(data));
    } else {
        args[1] = JSValueMakeNull(ctx);
    }

    JSObjectCallAsFunction(ctx, data_arrived_info->data_arrived_cb, NULL, 2, args, NULL);
    release_eval_lock();

    conn_data_cb_ret_t *conn_data_arrived_ret = malloc(sizeof(conn_data_cb_ret_t));

    conn_data_arrived_ret->err = 0;
    conn_data_arrived_ret->close = false;

    return conn_data_arrived_ret;
}

typedef struct accept_info {
    JSObjectRef accept_cb;
} accept_info_t;

accepted_conn_cb_ret_t *accepted_socket_connection(int sock, void *info) {

    accept_info_t *accept_info = info;

    acquire_eval_lock();
    JSValueRef args[1];
    args[0] = JSValueMakeNumber(ctx, sock);

    JSValueRef data_arrived_cb_ref = JSObjectCallAsFunction(ctx, accept_info->accept_cb, NULL, 1, args, NULL);

    data_arrived_info_t *data_arrived_info = malloc(sizeof(data_arrived_info_t));
    data_arrived_info->data_arrived_cb = JSValueToObject(ctx, data_arrived_cb_ref, NULL);
    JSValueProtect(ctx, data_arrived_cb_ref);
    release_eval_lock();

    accepted_conn_cb_ret_t *accepted_conn_cb_ret = malloc(sizeof(accepted_conn_cb_ret_t));

    accepted_conn_cb_ret->err = 0;
    accepted_conn_cb_ret->info = data_arrived_info;

    return accepted_conn_cb_ret;
}

JSValueRef function_socket_connect(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                   size_t argc, JSValueRef const *args, JSValueRef *exception) {
    if (argc == 3
        && JSValueGetType(ctx, args[0]) == kJSTypeString
        && JSValueGetType(ctx, args[1]) == kJSTypeNumber
        && JSValueGetType(ctx, args[2]) == kJSTypeObject) {

        char *host = value_to_c_string(ctx, args[0]);
        int port = (int) JSValueToNumber(ctx, args[1], NULL);

        JSValueRef data_arrived_cb_ref = args[2];

        data_arrived_info_t *data_arrived_info = malloc(sizeof(data_arrived_info_t));
        data_arrived_info->data_arrived_cb = JSValueToObject(ctx, data_arrived_cb_ref, NULL);
        JSValueProtect(ctx, data_arrived_cb_ref);

        int sock = connect_socket(host, port, socket_conn_data_arrived, data_arrived_info);

        if (sock == -1) {
            *exception = make_error_with_errno(ctx);
        } else {
            return JSValueMakeNumber(ctx, sock);
        }
    }
    return JSValueMakeNull(ctx);
}

JSValueRef function_socket_listen(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                  size_t argc, const JSValueRef args[], JSValueRef *exception) {
    if (argc == 2
        && JSValueGetType(ctx, args[0]) == kJSTypeNumber
        && JSValueGetType(ctx, args[1]) == kJSTypeObject) {

        int port = (int) JSValueToNumber(ctx, args[0], NULL);

        accept_info_t *accept_info = malloc(sizeof(accept_info_t));
        accept_info->accept_cb = JSValueToObject(ctx, args[1], NULL);
        JSValueProtect(ctx, args[1]);

        socket_accept_info_t *socket_accept_info = malloc(sizeof(socket_accept_info_t));
        socket_accept_info->host = NULL;
        socket_accept_info->port = port;
        socket_accept_info->listen_successful_cb = NULL;
        socket_accept_info->accepted_conn_cb = accepted_socket_connection;
        socket_accept_info->conn_data_cb = socket_conn_data_arrived;
        socket_accept_info->info = accept_info;

        int err = bind_and_listen(socket_accept_info);
        if (err == -1) {
            *exception = make_error_with_errno(ctx);
        } else {
            pthread_t thread;
            pthread_create(&thread, NULL, accept_connections, socket_accept_info);
        }
    }
    return JSValueMakeNull(ctx);
}

JSValueRef function_socket_write(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                 size_t argc, const JSValueRef args[], JSValueRef *exception) {
    if (argc == 2
        && JSValueGetType(ctx, args[0]) == kJSTypeNumber
        && JSValueGetType(ctx, args[1]) == kJSTypeString) {

        int sock = (int) JSValueToNumber(ctx, args[0], NULL);

        int err = write_to_socket(sock, value_to_c_string(ctx, args[1]));

        if (err == -1) {
            *exception = make_error_with_errno(ctx);
        }
    }
    return JSValueMakeNull(ctx);
}

JSValueRef function_socket_close(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                 size_t argc, const JSValueRef args[], JSValueRef *exception) {
    if (argc == 1
        && JSValueGetType(ctx, args[0]) == kJSTypeNumber) {

        int sock = (int) JSValueToNumber(ctx, args[0], NULL);

        int err = close_socket(sock);

        if (err == -1) {
            *exception = make_error_with_errno(ctx);
        }
    }
    return JSValueMakeNull(ctx);
}

JSValueRef function_sleep(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                          size_t argc, const JSValueRef args[], JSValueRef *exception) {
    if (argc == 2
        && JSValueGetType(ctx, args[0]) == kJSTypeNumber
        && JSValueGetType(ctx, args[1]) == kJSTypeNumber) {

        int millis = (int) JSValueToNumber(ctx, args[0], NULL);
        int nanos = (int) JSValueToNumber(ctx, args[1], NULL);

        struct timespec t;
        t.tv_sec = millis / 1000;
        t.tv_nsec = 1000 * 1000 * (millis % 1000) + nanos;
        
        if (t.tv_sec != 0 || t.tv_nsec != 0) {
            int err;
            while ((err = nanosleep(&t, &t)) && errno == EINTR) {}
            if (err) {
                engine_perror("sleep");
            }
        }
    }
    return JSValueMakeNull(ctx);
}

JSValueRef function_signal_task_complete(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                         size_t argc, const JSValueRef args[], JSValueRef *exception) {
    int err = signal_task_complete();
    if (err) {
        engine_print_err_message("signal_task_complete", err);
    }
    return JSValueMakeNull(ctx);
}

JSValueRef function_getenv(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                           size_t argc, const JSValueRef args[], JSValueRef *exception) {
  if (argc == 0) {
      int i = 0;
      JSObjectRef env = JSObjectMake(ctx, NULL, NULL);
      while(environ[i]) {
          char* entry = strdup(environ[i++]);
          char* name = strsep(&entry, "=");
          JSObjectSetProperty(ctx, env, JSStringCreateWithUTF8CString(name), c_string_to_value(ctx, entry),
                              kJSPropertyAttributeReadOnly, NULL);
      }

      return env;
  } else if (argc == 1) {
      JSValueRef arg = args[0];
      char* name = value_to_c_string(ctx, arg);
      char* entry = getenv(name);
      JSValueRef value;
      if(entry == NULL || strlen(entry) == 0) {
          value = JSValueMakeNull(ctx);
      } else {
          value = c_string_to_value(ctx, entry);
      }
      return value;
  } else {
      JSValueRef arguments[1];
      arguments[0] = c_string_to_value(ctx, strerror(7)); // Argument list too long
      *exception = JSObjectMakeError(ctx, 1, arguments, NULL);
  }
  return JSValueMakeNull(ctx);
}

JSValueRef function_isatty(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                           size_t argc, const JSValueRef args[], JSValueRef *exception) {
  if (argc == 1 && JSValueGetType(ctx, args[0]) == kJSTypeNumber) {
    int fd = (int) JSValueToNumber(ctx, args[0], NULL);
    if (fd >= 0) {
      errno = 0;
      bool result = isatty(fd);
      if (errno != EBADF) {
        return JSValueMakeBoolean(ctx, result);
      }
    }
  }
  return JSValueMakeNull(ctx);
}

JSValueRef function_get_keypress(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                 size_t argc, const JSValueRef args[], JSValueRef *exception) {
    char c = linenoiseGetKeyPress();
    return JSValueMakeNumber(ctx, c);
}
