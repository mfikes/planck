#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>

#include <JavaScriptCore/JavaScript.h>

#include "bundle.h"
#include "functions.h"
#include "globals.h"
#include "http.h"
#include "shell.h"
#include "io.h"
#include "jsc_utils.h"
#include "str.h"
#include "engine.h"
#include "clock.h"

JSGlobalContextRef ctx = NULL;

pthread_mutex_t eval_lock = PTHREAD_MUTEX_INITIALIZER;

void acquire_eval_lock() {
    pthread_mutex_lock(&eval_lock);
}

void release_eval_lock() {
    pthread_mutex_unlock(&eval_lock);
}

static volatile int keep_running = 1;

void int_handler(int dummy) {
    keep_running = 0;
    signal(SIGINT, NULL);
}

void set_int_handler() {
    signal(SIGINT, int_handler);
}

void clear_int_handler() {
    signal(SIGINT, NULL);
    keep_running = 1;
}

bool should_keep_running() {
    return keep_running != 0;
}

bool engine_ready = false;
pthread_mutex_t engine_init_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t engine_init_cond = PTHREAD_COND_INITIALIZER;
pthread_t engine_init_thread;

int block_until_engine_ready() {
    int err = pthread_mutex_lock(&engine_init_lock);
    if (err) return err;

    while (!engine_ready) {
        err = pthread_cond_wait(&engine_init_cond, &engine_init_lock);
        if (err) {
            pthread_mutex_unlock(&engine_init_lock);
            return err;
        }
    }

    return pthread_mutex_unlock(&engine_init_lock);
}

const char *block_until_engine_ready_failed_msg = "Failed waiting for JavaScript engine to initialize.";

int signal_engine_ready() {
    int err = pthread_mutex_lock(&engine_init_lock);
    if (err) return err;

    engine_ready = true;

    err = pthread_cond_signal(&engine_init_cond);
    if (err) {
        pthread_mutex_unlock(&engine_init_lock);
        return err;
    }

    return pthread_mutex_unlock(&engine_init_lock);
}

char *munge(char *s) {
    size_t len = strlen(s);
    size_t new_len = 0;
    int i;
    for (i = 0; i < len; i++) {
        switch (s[i]) {
            case '!':
                new_len += 6; // _BANG_
                break;
            case '?':
                new_len += 7; // _QMARK_
                break;
            default:
                new_len += 1;
        }
    }

    char *ms = malloc((new_len + 1) * sizeof(char));
    int j = 0;
    for (i = 0; i < len; i++) {
        switch (s[i]) {
            case '-':
                ms[j++] = '_';
                break;
            case '!':
                ms[j++] = '_';
                ms[j++] = 'B';
                ms[j++] = 'A';
                ms[j++] = 'N';
                ms[j++] = 'G';
                ms[j++] = '_';
                break;
            case '?':
                ms[j++] = '_';
                ms[j++] = 'Q';
                ms[j++] = 'M';
                ms[j++] = 'A';
                ms[j++] = 'R';
                ms[j++] = 'K';
                ms[j++] = '_';
                break;

            default:
                ms[j++] = s[i];
        }
    }
    ms[new_len] = '\0';

    return ms;
}

JSValueRef get_value_on_object(JSContextRef ctx, JSObjectRef obj, char *name) {
    JSStringRef name_str = JSStringCreateWithUTF8CString(name);
    JSValueRef val = JSObjectGetProperty(ctx, obj, name_str, NULL);
    JSStringRelease(name_str);
    return val;
}

JSValueRef get_value(JSContextRef ctx, char *namespace, char *name) {
    JSValueRef ns_val = NULL;

    // printf("get_value: '%s'\n", namespace);
    char *ns_tmp = strdup(namespace);
    char *saveptr = NULL;
    char *ns_part = strtok_r(ns_tmp, ".", &saveptr);
    while (ns_part != NULL) {
        char *munged_ns_part = munge(ns_part);
        if (ns_val) {
            if (JSValueIsUndefined(ctx, ns_val)) {
                return ns_val;
            }
            ns_val = get_value_on_object(ctx, JSValueToObject(ctx, ns_val, NULL), munged_ns_part);
        } else {
            ns_val = get_value_on_object(ctx, JSContextGetGlobalObject(ctx), munged_ns_part);
        }
        free(munged_ns_part); // TODO: Use a fixed buffer for this?  (Which would restrict namespace part length...)

        ns_part = strtok_r(NULL, ".", &saveptr);
    }
    free(ns_tmp);

    char *munged_name = munge(name);
    JSValueRef val = get_value_on_object(ctx, JSValueToObject(ctx, ns_val, NULL), munged_name);
    free(munged_name);
    return val;
}

JSObjectRef get_function(char *namespace, char *name) {
    JSValueRef val = get_value(ctx, namespace, name);
    if (JSValueIsUndefined(ctx, val)) {
        char buffer[1024];
        snprintf(buffer, 1024, "Failed to get function %s/%s\n", namespace, name);
        engine_print(buffer);
        assert(false);
    }
    return JSValueToObject(ctx, val, NULL);
}


void evaluate_source(char *type, char *source, bool expression, bool print_nil, char *set_ns, const char *theme,
                bool block_until_ready, int session_id) {
    if (block_until_ready) {
        int err = block_until_engine_ready();
        if (err) {
            engine_println(block_until_engine_ready_failed_msg);
            return;
        }
    }

    acquire_eval_lock();
    JSValueRef args[6];
    size_t num_args = 6;

    JSValueRef source_args[2];
    JSStringRef type_str = JSStringCreateWithUTF8CString(type);
    source_args[0] = JSValueMakeString(ctx, type_str);
    JSStringRef source_str = JSStringCreateWithUTF8CString(source);
    source_args[1] = JSValueMakeString(ctx, source_str);
    args[0] = JSObjectMakeArray(ctx, 2, source_args, NULL);

    args[1] = JSValueMakeBoolean(ctx, expression);
    args[2] = JSValueMakeBoolean(ctx, print_nil);
    JSValueRef set_ns_val = NULL;
    if (set_ns != NULL) {
        JSStringRef set_ns_str = JSStringCreateWithUTF8CString(set_ns);
        set_ns_val = JSValueMakeString(ctx, set_ns_str);
    }
    args[3] = set_ns_val;
    JSStringRef theme_str = JSStringCreateWithUTF8CString(theme);
    args[4] = JSValueMakeString(ctx, theme_str);
    args[5] = JSValueMakeNumber(ctx, session_id);

    static JSObjectRef execute_fn = NULL;
    if (!execute_fn) {
        execute_fn = get_function("planck.repl", "execute");
        JSValueProtect(ctx, execute_fn);
    }
    JSObjectRef global_obj = JSContextGetGlobalObject(ctx);

    JSObjectCallAsFunction(ctx, execute_fn, global_obj, num_args, args, NULL);
    release_eval_lock();
}

void bootstrap(char *out_path) {
    char *deps_file_path = "main.js";
    char *goog_base_path = "goog/base.js";
    if (out_path != NULL) {
        deps_file_path = str_concat(out_path, deps_file_path);
        goog_base_path = str_concat(out_path, goog_base_path);
    }

    char source[] = "<bootstrap>";

    // Setup CLOSURE_IMPORT_SCRIPT
    evaluate_script(ctx, "CLOSURE_IMPORT_SCRIPT = function(src) { AMBLY_IMPORT_SCRIPT('goog/' + src); return true; }",
                    source);

    // Load goog base
    char *base_script_str = NULL;
    if (out_path) {
        base_script_str = get_contents(goog_base_path, NULL);
        free(goog_base_path);
    } else {
        base_script_str = bundle_get_contents(goog_base_path);
    }
    if (base_script_str == NULL) {
        fprintf(stderr, "The goog base JavaScript text could not be loaded\n");
        exit(1);
    }
    evaluate_script(ctx, base_script_str, "<bootstrap:base>");
    free(base_script_str);

    // Load the deps file
    char *deps_script_str = NULL;
    if (out_path) {
        deps_script_str = get_contents(deps_file_path, NULL);
        free(deps_file_path);
    } else {
        deps_script_str = bundle_get_contents(deps_file_path);
    }
    if (deps_script_str == NULL) {
        fprintf(stderr, "The deps JavaScript text could not be loaded\n");
        exit(1);
    }
    evaluate_script(ctx, deps_script_str, "<bootstrap:deps>");
    free(deps_script_str);

    evaluate_script(ctx, "goog.require('cljs.core');", source);

    evaluate_script(ctx, "goog.isProvided_ = function(x) { return false; };", source);

    // redef goog.require to track loaded libs
    evaluate_script(ctx,
                    "goog.require__ = goog.require;\n"
                    "goog.require = (src, reload) => {\n"
                    "if(reload === \"reload-all\") {\n"
                    "goog.cljsReloadAll_ = true\n"
                    "}\n"
                    "if(reload || goog.cljsReloadAll_) {\n"
                    "if(goog.debugLoader_) {\n"
                    "let path = goog.debugLoader_.getPathFromDeps_(src);\n"
                    "goog.object.remove(goog.debugLoader_.written_, path);\n"
                    "goog.object.remove(goog.debugLoader_.written_, goog.basePath + path);\n"
                    "} else {\n"
                    "let path = goog.object.get(goog.dependencies_.nameToPath, src);\n"
                    "goog.object.remove(goog.dependencies_.visited, path);\n"
                    "goog.object.remove(goog.dependencies_.written, path);\n"
                    "goog.object.remove(goog.dependencies_.visited, goog.basePath + path);\n"
                    "}\n"
                    "}\n"
                    "let ret = goog.require__(src);\n"
                    "if(reload === \"reload-all\") {\n"
                    "goog.cljsReloadAll_ = false\n"
                    "}\n"
                    "if(goog.isInModuleLoader_()) {\n"
                    "return goog.module.getInternal_(src);\n"
                    "} else {\n"
                    "return ret;\n"
                    "}\n"
                    "};\n"
                            "};", source);
}

void run_main_in_ns(char *ns, size_t argc, char **argv) {
    int err = block_until_engine_ready();
    if (err) {
        engine_println(block_until_engine_ready_failed_msg);
        return;
    }

    size_t num_arguments = argc + 1;
    JSValueRef arguments[num_arguments];
    arguments[0] = c_string_to_value(ctx, ns);
    int i;
    for (i = 1; i < num_arguments; i++) {
        arguments[i] = c_string_to_value(ctx, argv[i - 1]);
    }

    JSObjectRef global_obj = JSContextGetGlobalObject(ctx);
    JSObjectRef run_main_fn = get_function("planck.repl", "run-main");
    JSObjectCallAsFunction(ctx, run_main_fn, global_obj, num_arguments, arguments, NULL);
}

void run_main_cli_fn() {
    int err = block_until_engine_ready();
    if (err) {
        engine_println(block_until_engine_ready_failed_msg);
        return;
    }

    acquire_eval_lock();
    JSObjectRef global_obj = JSContextGetGlobalObject(ctx);
    JSObjectRef run_main_cli_fn = get_function("planck.repl", "run-main-cli-fn");
    JSObjectCallAsFunction(ctx, run_main_cli_fn, global_obj, 0, NULL, NULL);
    release_eval_lock();
}

char *get_current_ns() {
    int err = block_until_engine_ready();
    if (err) {
        engine_println(block_until_engine_ready_failed_msg);
        return NULL;
    }

    size_t num_arguments = 0;
    JSValueRef arguments[num_arguments];
    static JSObjectRef get_current_ns_fn = NULL;
    if (!get_current_ns_fn) {
        get_current_ns_fn = get_function("planck.repl", "get-current-ns");
        JSValueProtect(ctx, get_current_ns_fn);
    }
    JSValueRef result = JSObjectCallAsFunction(ctx, get_current_ns_fn, JSContextGetGlobalObject(ctx), num_arguments,
                                               arguments, NULL);
    return value_to_c_string(ctx, result);
}

char **get_completions(const char *buffer, int *num_completions) {
    int err = block_until_engine_ready();
    if (err) return NULL;

    size_t num_arguments = 1;
    JSValueRef arguments[num_arguments];
    arguments[0] = c_string_to_value(ctx, (char *) buffer);
    static JSObjectRef completions_fn = NULL;
    if (!completions_fn) {
        completions_fn = get_function("planck.repl", "get-completions");
        JSValueProtect(ctx, completions_fn);
    }
    JSValueRef result = JSObjectCallAsFunction(ctx, completions_fn, JSContextGetGlobalObject(ctx), num_arguments,
                                               arguments, NULL);

    assert(JSValueIsObject(ctx, result));
    JSObjectRef array = JSValueToObject(ctx, result, NULL);
    JSStringRef length_prop = JSStringCreateWithUTF8CString("length");
    JSValueRef array_len = JSObjectGetProperty(ctx, array, length_prop, NULL);
    JSStringRelease(length_prop);
    assert(JSValueIsNumber(ctx, array_len));
    int n = (int) JSValueToNumber(ctx, array_len, NULL);

    char **completions = malloc(n * sizeof(char *));

    unsigned int i;
    for (i = 0; i < n; i++) {
        JSValueRef v = JSObjectGetPropertyAtIndex(ctx, array, i, NULL);
        completions[i] = value_to_c_string(ctx, v);
    }

    *num_completions = n;
    return completions;
}

void register_global_function(JSContextRef ctx, char *name, JSObjectCallAsFunctionCallback handler) {
    JSObjectRef global_obj = JSContextGetGlobalObject(ctx);

    JSStringRef fn_name = JSStringCreateWithUTF8CString(name);
    JSObjectRef fn_obj = JSObjectMakeFunctionWithCallback(ctx, fn_name, handler);

    JSObjectSetProperty(ctx, global_obj, fn_name, fn_obj, kJSPropertyAttributeNone, NULL);
}

void discarding_sender(const char *msg) {
    /* Intentionally empty. */
}

void maybe_load_user_file() {
    if (config.repl) {
        JSValueRef arguments[0];
        JSValueRef ex = NULL;
        JSObjectCallAsFunction(ctx, get_function("planck.repl", "maybe-load-user-file"), JSContextGetGlobalObject(ctx),
                               0, arguments, &ex);
        debug_print_value("planck.repl/maybe-load-user-file", ctx, ex);

        if (ex) {
            print_value("Error loading user file: ", ctx, ex);
        }
    }
}

void init_paredit(JSContextRef ctx) {
    JSValueRef arguments[0];
    JSValueRef ex = NULL;
    JSObjectCallAsFunction(ctx, get_function("planck.repl", "init-paredit"),
                           JSContextGetGlobalObject(ctx), 0, arguments, &ex);

    if (ex) {
        print_value("Error initializing paredit: ", ctx, ex);
    }
}

void *do_engine_init(void *data) {
    ctx = JSGlobalContextCreate(NULL);

    display_launch_timing("JS context created");

    evaluate_script(ctx, "var global = this;", "<init>");

    register_global_function(ctx, "AMBLY_IMPORT_SCRIPT", function_import_script);
    bootstrap(config.out_path);

    display_launch_timing("bootstrap");

    register_global_function(ctx, "PLANCK_CONSOLE_STDOUT", function_console_stdout);
    register_global_function(ctx, "PLANCK_CONSOLE_STDERR", function_console_stderr);

    evaluate_script(ctx, "var PLANCK_VERSION = \"" PLANCK_VERSION "\";", "<init>");

    display_launch_timing("version");

    // require app namespaces
    evaluate_script(ctx, "goog.require('planck.repl');", "<init>");

    display_launch_timing("require app namespaces");

    // without this things won't work
    evaluate_script(ctx, "var window = global;", "<init>");

    display_launch_timing("window global");

    register_global_function(ctx, "PLANCK_READ_FILE", function_read_file);
    register_global_function(ctx, "PLANCK_LOAD", function_load);
    register_global_function(ctx, "PLANCK_LOAD_DEPS_CLJS_FILES", function_load_deps_cljs_files);
    register_global_function(ctx, "PLANCK_LOAD_DATA_READERS_FILES", function_load_data_readers_files);
    register_global_function(ctx, "PLANCK_LOAD_FROM_JAR", function_load_from_jar);
    register_global_function(ctx, "PLANCK_CACHE", function_cache);

    register_global_function(ctx, "PLANCK_EVAL", function_eval);

    register_global_function(ctx, "PLANCK_GET_TERM_SIZE", function_get_term_size);

    register_global_function(ctx, "PLANCK_EXIT_WITH_VALUE", function_exit_with_value);

    register_global_function(ctx, "PLANCK_SHELL_SH", function_shellexec);

    register_global_function(ctx, "PLANCK_RAW_READ_STDIN", function_raw_read_stdin);
    register_global_function(ctx, "PLANCK_RAW_WRITE_STDOUT", function_raw_write_stdout);
    register_global_function(ctx, "PLANCK_RAW_FLUSH_STDOUT", function_raw_flush_stdout);
    register_global_function(ctx, "PLANCK_RAW_WRITE_STDERR", function_raw_write_stderr);
    register_global_function(ctx, "PLANCK_RAW_FLUSH_STDERR", function_raw_flush_stderr);

    register_global_function(ctx, "PLANCK_FILE_READER_OPEN", function_file_reader_open);
    register_global_function(ctx, "PLANCK_FILE_READER_READ", function_file_reader_read);
    register_global_function(ctx, "PLANCK_FILE_READER_CLOSE", function_file_reader_close);

    register_global_function(ctx, "PLANCK_FILE_WRITER_OPEN", function_file_writer_open);
    register_global_function(ctx, "PLANCK_FILE_WRITER_WRITE", function_file_writer_write);
    register_global_function(ctx, "PLANCK_FILE_WRITER_FLUSH", function_file_writer_flush);
    register_global_function(ctx, "PLANCK_FILE_WRITER_CLOSE", function_file_writer_close);

    register_global_function(ctx, "PLANCK_FILE_INPUT_STREAM_OPEN", function_file_input_stream_open);
    register_global_function(ctx, "PLANCK_FILE_INPUT_STREAM_READ", function_file_input_stream_read);
    register_global_function(ctx, "PLANCK_FILE_INPUT_STREAM_CLOSE", function_file_input_stream_close);

    register_global_function(ctx, "PLANCK_FILE_OUTPUT_STREAM_OPEN", function_file_output_stream_open);
    register_global_function(ctx, "PLANCK_FILE_OUTPUT_STREAM_WRITE", function_file_output_stream_write);
    register_global_function(ctx, "PLANCK_FILE_OUTPUT_STREAM_FLUSH", function_file_output_stream_flush);
    register_global_function(ctx, "PLANCK_FILE_OUTPUT_STREAM_CLOSE", function_file_output_stream_close);

    register_global_function(ctx, "PLANCK_MKDIRS", function_mkdirs);
    register_global_function(ctx, "PLANCK_DELETE", function_delete_file);
    register_global_function(ctx, "PLANCK_COPY", function_copy_file);

    register_global_function(ctx, "PLANCK_LIST_FILES", function_list_files);

    register_global_function(ctx, "PLANCK_IS_DIRECTORY", function_is_directory);

    register_global_function(ctx, "PLANCK_FSTAT", function_fstat);

    register_global_function(ctx, "PLANCK_MKTEMP", function_mktemp);

    register_global_function(ctx, "PLANCK_REQUEST", function_http_request);

    register_global_function(ctx, "PLANCK_READ_PASSWORD", function_read_password);

    register_global_function(ctx, "PLANCK_HIGH_RES_TIMER", function_high_res_timer);

    register_global_function(ctx, "PLANCK_SOCKET_CONNECT", function_socket_connect);
    register_global_function(ctx, "PLANCK_SOCKET_LISTEN", function_socket_listen);
    register_global_function(ctx, "PLANCK_SOCKET_WRITE", function_socket_write);
    register_global_function(ctx, "PLANCK_SOCKET_CLOSE", function_socket_close);

    register_global_function(ctx, "PLANCK_SLEEP", function_sleep);

    register_global_function(ctx, "PLANCK_SIGNAL_TASK_COMPLETE", function_signal_task_complete);

    register_global_function(ctx, "PLANCK_GETENV", function_getenv);

    register_global_function(ctx, "PLANCK_ISATTY", function_isatty);

    display_launch_timing("register fns");

    // Monkey patch cljs.core/system-time to use Planck's high-res timer
    evaluate_script(ctx, "cljs.core.system_time = PLANCK_HIGH_RES_TIMER", "<init>");

    display_launch_timing("monkey-patch system-time");

    {
        JSValueRef arguments[config.num_rest_args];
        int i;
        for (i = 0; i < config.num_rest_args; i++) {
            arguments[i] = c_string_to_value(ctx, config.rest_args[i]);
        }
        JSValueRef args_ref = JSObjectMakeArray(ctx, config.num_rest_args, arguments, NULL);

        JSValueRef global_obj = JSContextGetGlobalObject(ctx);
        JSStringRef prop = JSStringCreateWithUTF8CString("PLANCK_INITIAL_COMMAND_LINE_ARGS");
        JSObjectSetProperty(ctx, JSValueToObject(ctx, global_obj, NULL), prop, args_ref, kJSPropertyAttributeNone,
                            NULL);
        JSStringRelease(prop);
    }

    display_launch_timing("setup command line args");

    register_global_function(ctx, "PLANCK_SET_TIMEOUT", function_set_timeout);
    register_global_function(ctx, "PLANCK_SET_INTERVAL", function_set_interval);
    evaluate_script(ctx,
                    "var PLANCK_TIMEOUT_CALLBACK_STORE = {};\
                     var setTimeout = function( fn, ms ) {\
                       if ( cljs.core.fn_QMARK_(fn) ) {\
                         var id = PLANCK_SET_TIMEOUT(ms);\
                         PLANCK_TIMEOUT_CALLBACK_STORE[id] = fn;\
                         return id;\
                       } else {\
                         throw new Error(\"Callback must be a function\");\
                       }\
                     };\
                     var PLANCK_RUN_TIMEOUT = function( id ) {\
                       if( PLANCK_TIMEOUT_CALLBACK_STORE[id] ) {\
                         try {\
                           PLANCK_TIMEOUT_CALLBACK_STORE[id]();\
                         } finally {\
                           delete PLANCK_TIMEOUT_CALLBACK_STORE[id];\
                           PLANCK_SIGNAL_TASK_COMPLETE();\
                         }\
                       }\
                     };\
                     var clearTimeout = function( id ) {\
                       if ( PLANCK_TIMEOUT_CALLBACK_STORE[id] ) {\
                         delete PLANCK_TIMEOUT_CALLBACK_STORE[id];\
                         PLANCK_SIGNAL_TASK_COMPLETE();\
                       }\
                     };\
                     var PLANCK_INTERVAL_CALLBACK_STORE = {};\
                     var setInterval = function( fn, ms ) {\
                        if ( cljs.core.fn_QMARK_(fn) ) {\
                          var id = PLANCK_SET_INTERVAL(ms, null);\
                          PLANCK_INTERVAL_CALLBACK_STORE[id] = \
                            function(){ fn(); PLANCK_SET_INTERVAL(ms, id); };\
                          return id;\
                        } else {\
                          throw new Error(\"Callback must be a function\");\
                        }\
                     };\
                     var PLANCK_RUN_INTERVAL = function( id ) {\
                        if( PLANCK_INTERVAL_CALLBACK_STORE[id] ) {\
                          PLANCK_INTERVAL_CALLBACK_STORE[id]();\
                        }\
                     };\
                     var clearInterval = function( id ) {\
                       if ( PLANCK_INTERVAL_CALLBACK_STORE[id] ) {\
                         delete PLANCK_INTERVAL_CALLBACK_STORE[id];\
                         PLANCK_SIGNAL_TASK_COMPLETE();\
                       }\
                     };",
                    "<init>");

    display_launch_timing("setTimeout");

    set_print_sender(&discarding_sender);

    {
        JSValueRef arguments[9];
        arguments[0] = JSValueMakeBoolean(ctx, config.repl);
        arguments[1] = JSValueMakeBoolean(ctx, config.verbose);
        JSValueRef cache_path_ref = NULL;
        if (config.cache_path != NULL) {
            JSStringRef cache_path_str = JSStringCreateWithUTF8CString(config.cache_path);
            cache_path_ref = JSValueMakeString(ctx, cache_path_str);
        }
        arguments[2] = cache_path_ref;
        JSValueRef checked_arrays_ref = NULL;
        if (config.checked_arrays != NULL) {
            JSStringRef checked_arrays_str = JSStringCreateWithUTF8CString(config.checked_arrays);
            checked_arrays_ref = JSValueMakeString(ctx, checked_arrays_str);
        }
        arguments[3] = checked_arrays_ref;
        arguments[4] = JSValueMakeBoolean(ctx, config.static_fns);
        arguments[5] = JSValueMakeBoolean(ctx, config.fn_invoke_direct);
        arguments[6] = JSValueMakeBoolean(ctx, config.elide_asserts);
        JSStringRef optimizations_str = JSStringCreateWithUTF8CString(config.optimizations);
        JSValueRef optimizations_ref = JSValueMakeString(ctx, optimizations_str);
        arguments[7] = optimizations_ref;

        JSValueRef compile_opts[config.num_compile_opts];
        size_t i;
        for (i=0; i<config.num_compile_opts; i++) {
            JSStringRef compile_opts_str = JSStringCreateWithUTF8CString(config.compile_opts[i]);
            compile_opts[i] = JSValueMakeString(ctx, compile_opts_str);
        }
        arguments[8] = JSObjectMakeArray(ctx, config.num_compile_opts, compile_opts, NULL);

        JSValueRef ex = NULL;
        JSObjectCallAsFunction(ctx, get_function("planck.repl", "init"), JSContextGetGlobalObject(ctx), 9,
                               arguments, &ex);
        debug_print_value("planck.repl/init", ctx, ex);

        if (ex) {
            print_value("Error initializing engine: ", ctx, ex);
        }
    }

    display_launch_timing("planck.repl/init");

    char version_script[1024];
    snprintf(version_script, 1024, "cljs.core._STAR_clojurescript_version_STAR_ = \"%s\";", config.clojurescript_version);
    evaluate_script(ctx, version_script, "<init>");

    if (config.repl) {
        evaluate_source("text", "(eval `(~'ns ~'cljs.user (:require ~@(-> @planck.repl/app-env :opts (:repl-requires "
                                "'[[planck.repl :refer-macros [source doc find-doc apropos dir pst]]])))))",
                        true, false, "cljs.user", "dumb", false, 0);
        display_launch_timing("repl requires");
    } else {
        evaluate_source("text", "(require 'planck.repl)",
                        true, false, "cljs.user", "dumb", false, 0);
        display_launch_timing("repl code");
    }

    evaluate_script(ctx, "goog.provide('cljs.user');", "<init>");
    evaluate_script(ctx, "goog.require('cljs.core');", "<init>");

    set_print_sender(NULL);

    JSValueRef arguments[0];
    JSValueRef ex = NULL;
    JSObjectCallAsFunction(ctx, get_function("planck.repl", "init-data-readers"), JSContextGetGlobalObject(ctx), 0,
                           arguments, &ex);

    if (ex) {
        print_value("Error initializing data readers: ", ctx, ex);
    }

    maybe_load_user_file();

    if (config.repl) {
        init_paredit(ctx);
    }

    display_launch_timing("engine ready");

    signal_engine_ready();

    return NULL;
}

void engine_init() {

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int ret = pthread_create(&engine_init_thread, &attr, do_engine_init, NULL);
    if (ret != 0) {
        engine_perror("pthread_create");
        exit(1);
    }
    pthread_attr_destroy(&attr);
}

void engine_shutdown() {
    JSGlobalContextRelease(ctx);
}

void (*cljs_sender)(const char *msg) = NULL;

void engine_perror(const char *msg) {
    if (cljs_sender == &discarding_sender) {
        perror(msg);
    } else {
        char buffer[1024];
        snprintf(buffer, 1024, "%s: %s\n", msg, strerror(errno));
        engine_print(buffer);
    }
}

void engine_print_err_message(const char *msg, int err) {
    char buffer[1024];
    snprintf(buffer, 1024, "%s: %d\n", msg, err);
    engine_print(buffer);
}

void engine_print(const char *msg) {
    void (*current_sender)(const char *msg) = cljs_sender;
    if (current_sender) {
        current_sender(msg);
    } else {
        fprintf(stderr, "%s", msg);
    }
}

void engine_println(const char *msg) {
    char buffer[1024];
    snprintf(buffer, 1024, "%s\n", msg);
    engine_print(buffer);
}

JSValueRef function_print_fn_sender(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                    size_t argc, const JSValueRef args[], JSValueRef *exception) {
    if (argc == 1) {
        char *str = value_to_c_string_ext(ctx, args[0], true);

        engine_print(str);

        free(str);
    }

    return JSValueMakeNull(ctx);
}

void set_print_sender(void (*sender)(const char *msg)) {
    cljs_sender = sender;
    if (sender) {
        register_global_function(ctx, "PLANCK_PRINT_FN", function_print_fn_sender);
        register_global_function(ctx, "PLANCK_PRINT_ERR_FN", function_print_fn_sender);
    } else {
        register_global_function(ctx, "PLANCK_PRINT_FN", function_print_fn);
        register_global_function(ctx, "PLANCK_PRINT_ERR_FN", function_print_err_fn);
    }

    evaluate_script(ctx, "cljs.core.set_print_fn_BANG_.call(null,PLANCK_PRINT_FN);", "<init>");
    evaluate_script(ctx, "cljs.core.set_print_err_fn_BANG_.call(null,PLANCK_PRINT_ERR_FN);", "<init>");
    evaluate_script(ctx, "cljs.core._STAR_print_newline_STAR_ = true;", "<init>");
}

bool engine_print_newline() {
    return JSValueToBoolean(ctx,
                            evaluate_script(ctx, "cljs.core._STAR_print_newline_STAR_",
                                            "<cljs.c:engine_print_newline>"));
}

char *is_readable(char *expression) {
    int err = block_until_engine_ready();
    if (err) {
        engine_println(block_until_engine_ready_failed_msg);
        return NULL;
    }

    static JSObjectRef is_readable_fn = NULL;
    if (!is_readable_fn) {
        is_readable_fn = get_function("planck.repl", "is-readable?");
        JSValueProtect(ctx, is_readable_fn);
    }

    size_t num_arguments = 2;
    JSValueRef arguments[num_arguments];
    arguments[0] = c_string_to_value(ctx, expression);
    arguments[1] = c_string_to_value(ctx, config.theme);
    JSValueRef result = JSObjectCallAsFunction(ctx, is_readable_fn, JSContextGetGlobalObject(ctx),
                                               num_arguments, arguments, NULL);
    return value_to_c_string(ctx, result);
}

int indent_space_count(char *text) {
    int err = block_until_engine_ready();
    if (err) return 0;

    size_t num_arguments = 1;
    JSValueRef arguments[num_arguments];
    arguments[0] = c_string_to_value(ctx, text);
    static JSObjectRef indent_space_count_fn = NULL;
    if (!indent_space_count_fn) {
        indent_space_count_fn = get_function("planck.repl", "indent-space-count");
        JSValueProtect(ctx, indent_space_count_fn);
    }
    JSValueRef result = JSObjectCallAsFunction(ctx, indent_space_count_fn, JSContextGetGlobalObject(ctx),
                                               num_arguments, arguments, NULL);
    return (int) JSValueToNumber(ctx, result, NULL);
}

void highlight_coords_for_pos(int pos, const char *buf, size_t num_previous_lines,
                              char **previous_lines, int *num_lines_up, int *highlight_pos) {
    int err = block_until_engine_ready();
    if (err) return;

    size_t num_arguments = 3;
    JSValueRef arguments[num_arguments];
    arguments[0] = JSValueMakeNumber(ctx, pos);
    arguments[1] = c_string_to_value(ctx, buf);
    JSValueRef prev_lines[num_previous_lines];
    int i;
    for (i = 0; i < num_previous_lines; i++) {
        prev_lines[i] = c_string_to_value(ctx, previous_lines[i]);
    }
    arguments[2] = JSObjectMakeArray(ctx, num_previous_lines, prev_lines, NULL);
    static JSObjectRef get_highlight_coords_fn = NULL;
    if (!get_highlight_coords_fn) {
        get_highlight_coords_fn = get_function("planck.repl", "get-highlight-coords");
        JSValueProtect(ctx, get_highlight_coords_fn);
    }
    JSValueRef result = JSObjectCallAsFunction(ctx, get_highlight_coords_fn, JSContextGetGlobalObject(ctx),
                                               num_arguments, arguments, NULL);

    JSObjectRef array = JSValueToObject(ctx, result, NULL);
    if (array) {
        *num_lines_up = (int) JSValueToNumber(ctx, JSObjectGetPropertyAtIndex(ctx, array, 0, NULL), NULL);
        *highlight_pos = (int) JSValueToNumber(ctx, JSObjectGetPropertyAtIndex(ctx, array, 1, NULL), NULL);
    }
}
