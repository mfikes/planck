(ns planck.repl
  (:require-macros [cljs.env.macros :refer [with-compiler-env]])
  (:require [clojure.string :as s]
            [goog.string :as gstring]
            [cljs.analyzer :as ana]
            [cljs.tools.reader :as r]
            [cljs.tools.reader.reader-types :as rt]
            [cljs.tagged-literals :as tags]
            [cljs.source-map :as sm]
            [cljs.js :as cljs]
            [cljs.repl :as repl]
            [cljs.stacktrace :as st]
            [cognitect.transit :as transit]
            [tailrecursion.cljson :refer [cljson->clj]]
            [planck.repl-resources :refer [special-doc-map repl-special-doc-map]]))

(defn- println-verbose
  [& args]
  (binding [*print-fn* *print-err-fn*]
    (apply println args)))

(declare print-error)
(declare handle-error)

(defonce st (cljs/empty-state))

(defn- known-namespaces
  []
  (keys (:cljs.analyzer/namespaces @st)))

(defn transit-json->cljs
  [json]
  (let [rdr (transit/reader :json)]
    (transit/read rdr json)))

(defn cljs->transit-json
  [x]
  (let [wtr (transit/writer :json)]
    (transit/write wtr x)))

(defn ^:export load-core-analysis-cache
  []
  (cljs/load-analysis-cache! st 'cljs.core
    (transit-json->cljs (first (js/PLANCK_LOAD "cljs/core.cljs.cache.aot.json"))))
  (cljs/load-analysis-cache! st 'cljs.core$macros
    (transit-json->cljs (first (js/PLANCK_LOAD "cljs/core$macros.cljc.cache.json")))))

(defonce current-ns (atom 'cljs.user))

(defonce app-env (atom nil))

(defn ^:export init-app-env
  [verbose cache-path]
  (reset! planck.repl/app-env {:verbose verbose :cache-path cache-path}))

(defn repl-read-string
  [line]
  (r/read-string {:read-cond :allow :features #{:cljs}} line))

(defn- eof-while-reading?
  [message]
  (or
    (= "EOF while reading" message)
    (= "EOF while reading string" message)))

(defn ^:export is-readable?
  [line]
  (binding [r/*data-readers* tags/*cljs-data-readers*]
    (try
      (repl-read-string line)
      true
      (catch :default e
        (let [message (.-message e)]
          (cond
            (eof-while-reading? message) false
            (= "EOF" message) true
            :else (do
                    (print-error e false)
                    (println)
                    true)))))))

(defn ns-form?
  [form]
  (and (seq? form) (= 'ns (first form))))

(defn extract-namespace
  [source]
  (let [first-form (repl-read-string source)]
    (when (ns-form? first-form)
      (second first-form))))

(defn repl-special?
  [form]
  (and (seq? form) (repl-special-doc-map (first form))))

(defn- special-doc
  [name-symbol]
  (assoc (special-doc-map name-symbol)
    :name name-symbol
    :special-form true))

(defn- repl-special-doc
  [name-symbol]
  (assoc (repl-special-doc-map name-symbol)
    :name name-symbol
    :repl-special-function true))

(defn- make-base-eval-opts
  []
  {:ns      @current-ns
   :context :expr
   :verbose (:verbose @app-env)})

(defn- process-in-ns
  [argument]
  (cljs/eval
    st
    argument
    (make-base-eval-opts)
    (fn [result]
      (if (and (map? result) (:error result))
        (print-error (:error result) false)
        (let [ns-name result]
          (if-not (symbol? ns-name)
            (println "Argument to in-ns must be a symbol.")
            (if (some (partial = ns-name) (known-namespaces))
              (reset! current-ns ns-name)
              (let [ns-form `(~'ns ~ns-name)]
                (cljs/eval
                  st
                  ns-form
                  (make-base-eval-opts)
                  (fn [{e :error}]
                    (if e
                      (print-error e false)
                      (reset! current-ns ns-name))))))))))))

(defn- canonicalize-specs
  [specs]
  (letfn [(canonicalize [quoted-spec-or-kw]
            (if (keyword? quoted-spec-or-kw)
              quoted-spec-or-kw
              (as-> (second quoted-spec-or-kw) spec
                (if (vector? spec) spec [spec]))))]
    (map canonicalize specs)))

(defn- process-reloads!
  [specs]
  (if-let [k (some #{:reload :reload-all} specs)]
    (let [specs (->> specs (remove #{k}))]
      (if (= k :reload-all)
        (reset! cljs.js/*loaded* #{})
        (apply swap! cljs.js/*loaded* disj (map first specs)))
      specs)
    specs))

(defn- self-require?
  [specs]
  (some
    (fn [quoted-spec-or-kw]
      (and (not (keyword? quoted-spec-or-kw))
        (let [spec (second quoted-spec-or-kw)
              ns (if (sequential? spec)
                   (first spec)
                   spec)]
          (= ns @current-ns))))
    specs))

(defn- make-ns-form
  [kind specs target-ns]
  (if (= kind :import)
    (with-meta `(~'ns ~target-ns
                  (~kind
                    ~@(map (fn [quoted-spec-or-kw]
                             (if (keyword? quoted-spec-or-kw)
                               quoted-spec-or-kw
                               (second quoted-spec-or-kw)))
                        specs)))
      {:merge true :line 1 :column 1})
    (with-meta `(~'ns ~target-ns
                  (~kind
                    ~@(-> specs canonicalize-specs process-reloads!)))
      {:merge true :line 1 :column 1})))

(defn- log-ns-form
  [kind ns-form]
  (when (:verbose @app-env)
    (println-verbose "Implementing"
      (name kind)
      "via ns:\n  "
      (pr-str ns-form))))

(defn- process-require
  [kind cb specs]
  (try
    (let [is-self-require? (and (= :kind :require) (self-require? specs))
          [target-ns restore-ns]
          (if-not is-self-require?
            [@current-ns nil]
            ['cljs.user @current-ns])]
      (cljs/eval
        st
        (let [ns-form (make-ns-form kind specs target-ns)]
          (log-ns-form kind ns-form)
          ns-form)
        (make-base-eval-opts)
        (fn [{e :error}]
          (when is-self-require?
            (reset! current-ns restore-ns))
          (when e
            (handle-error e false false))
          (cb))))
    (catch :default e
      (handle-error e true false))))

(defn resolve
  "Given an analysis environment resolve a var. Analogous to
   clojure.core/resolve"
  [env sym]
  {:pre [(map? env) (symbol? sym)]}
  (try
    (ana/resolve-var env sym
      (ana/confirm-var-exists-throw))
    (catch :default _
      (ana/resolve-macro-var env sym))))

(defn ^:export get-current-ns
  []
  (str @current-ns))

(defn completion-candidates-for-ns
  [ns-sym allow-private?]
  (map (comp str key)
    (filter (if allow-private?
              identity
              #(not (:private (:meta (val %)))))
      (apply merge
        ((juxt :defs :macros)
          (get (:cljs.analyzer/namespaces @planck.repl/st) ns-sym))))))

(defn is-completion?
  [buffer-match-suffix candidate]
  (re-find (js/RegExp. (str "^" buffer-match-suffix)) candidate))

(defn- completion-candidates
  [namespace-candidates top-form? typed-ns]
  (set (if typed-ns
         (completion-candidates-for-ns (symbol typed-ns) false)
         (concat namespace-candidates
           (completion-candidates-for-ns 'cljs.core false)
           (completion-candidates-for-ns @current-ns true)
           (when top-form?
             (concat
               (map str (keys special-doc-map))
               (map str (keys repl-special-doc-map))))))))

(defn ^:export get-completions
  [buffer]
  (let [namespace-candidates (map str (known-namespaces))
        top-form? (re-find #"^\s*\(\s*[^()\s]*$" buffer)
        typed-ns (second (re-find #"(\b[a-zA-Z-.]+)/[a-zA-Z-]+$" buffer))]
    (let [buffer-match-suffix (re-find #"[a-zA-Z-]*$" buffer)
          buffer-prefix (subs buffer 0 (- (count buffer) (count buffer-match-suffix)))]
      (clj->js (if (= "" buffer-match-suffix)
                 []
                 (map #(str buffer-prefix %)
                   (sort
                     (filter (partial is-completion? buffer-match-suffix)
                       (completion-candidates namespace-candidates top-form? typed-ns)))))))))

(defn- is-completely-readable?
  [source]
  (let [rdr (rt/indexing-push-back-reader source 1 "noname")]
    (binding [r/*data-readers* tags/*cljs-data-readers*]
      (try
        (r/read {:eof (js-obj) :read-cond :allow :features #{:cljs}} rdr)
        (nil? (rt/peek-char rdr))
        (catch :default _
          false)))))

(defn- form-start
  [total-source total-pos]
  (some identity
    (for [n (range (dec total-pos) -1 -1)]
      (let [candidate-form (subs total-source n (inc total-pos))
            first-char (subs candidate-form 0 1)]
        (if (#{"(" "[" "{" "#"} first-char)
          (if (is-completely-readable? candidate-form)
            (if (= "#" first-char)
              (inc n)
              n)
            nil))))))

(defn- reduce-highlight-coords
  [previous-lines form-start]
  (if form-start
    (reduce (fn [[line-ndx start-pos] line]
              (if (< start-pos (count line))
                (reduced [line-ndx start-pos])
                [(dec line-ndx) (- start-pos (inc (count line)))]))
      [(count previous-lines) form-start]
      previous-lines)
    [-1 -1]))

(defn ^:export get-highlight-coords
  "Gets the highlight coordinates [line pos] for the previous matching
  brace. This is done by progressivly expanding source considered
  until a readable form is encountered with a matching brace on the
  other end. The coordinate system is such that line 0 is the current
  buffer line, line 1 is the previous line, and so on, and pos is the
  position in that line."
  [pos buffer previous-lines]
  (let [previous-lines (js->clj previous-lines)
        previous-source (s/join "\n" previous-lines)
        total-source (if (empty? previous-lines)
                       buffer
                       (str previous-source "\n" buffer))
        total-pos (+ (if (empty? previous-lines)
                       0
                       (inc (count previous-source))) pos)]
    (->> (form-start total-source total-pos)
      (reduce-highlight-coords previous-lines)
      clj->js)))

(defn- cache-prefix-for-path
  [path]
  (str (:cache-path @app-env) "/" (munge path)))

(defn- extract-cache-metadata
  [source]
  (let [file-namespace (extract-namespace source)
        relpath (if file-namespace
                  (cljs/ns->relpath file-namespace)
                  "cljs/user")]
    [file-namespace relpath]))

(def extract-cache-metadata-mem (memoize extract-cache-metadata))

(defn compiled-by-string
  ([] (compiled-by-string nil))
  ([opts]
   (str "// Compiled by ClojureScript "
     *clojurescript-version*
     (when opts
       (str " " (pr-str opts))))))

(defn extract-clojurescript-compiler-version
  [js-source]
  (second (re-find #"// Compiled by ClojureScript (\S*)" js-source)))

(defn caching-js-eval
  [{:keys [path name source cache]}]
  (when (and path source cache (:cache-path @app-env))
    (js/PLANCK_CACHE (cache-prefix-for-path path) (str (compiled-by-string) "\n" source) (cljs->transit-json cache)))
  (js/eval source))

(defn extension->lang
  [extension]
  (if (= ".js" extension)
    :js
    :clj))

(defn add-suffix
  [file suffix]
  (let [candidate (s/replace file #".cljs$" suffix)]
    (if (gstring/endsWith candidate suffix)
      candidate
      (str file suffix))))

(defn cached-js-valid?
  [js-source js-modified source-file-modified]
  (and js-source
    (or (= 0 js-modified source-file-modified)              ;; 0 means bundled
      (and (> js-modified source-file-modified)
        (= *clojurescript-version* (extract-clojurescript-compiler-version js-source))))))

(defn- cached-callback-data
  [path cache-prefix source source-modified raw-load]
  (let [cache-prefix (if (= :calculate-cache-prefix cache-prefix)
                       (cache-prefix-for-path (second (extract-cache-metadata-mem source)))
                       cache-prefix)
        [js-source js-modified] (or (raw-load (add-suffix path ".js"))
                                  (js/PLANCK_READ_FILE (str cache-prefix ".js")))
        [cache-json _] (or (raw-load (str path ".cache.json"))
                         (js/PLANCK_READ_FILE (str cache-prefix ".cache.json")))]
    (when (cached-js-valid? js-source js-modified source-modified)
      (when (:verbose @app-env)
        (println-verbose "Loading precompiled JS" (if cache-json "and analysis cache" "") "for" path))
      (merge {:lang   :js
              :source js-source}
        (when cache-json
          {:cache (transit-json->cljs cache-json)})))))

(defn load-and-callback!
  [path lang cache-prefix cb]
  (let [[raw-load [source modified]] [js/PLANCK_LOAD (js/PLANCK_LOAD path)]
        [raw-load [source modified]] (if source
                                       [raw-load [source modified]]
                                       [js/PLANCK_READ_FILE (js/PLANCK_READ_FILE path)])]
    (when source
      (cb (merge
            {:lang   lang
             :source source}
            (when-not (= :js lang)
              (cached-callback-data path cache-prefix source modified raw-load))))
      :loaded)))

(defn closure-index
  []
  (let [paths-to-provides
        (map (fn [[_ path provides]]
               [path (map second
                       (re-seq #"'(.*?)'" provides))])
          (re-seq #"\ngoog\.addDependency\('(.*)', \[(.*?)\].*"
            (first (js/PLANCK_LOAD "goog/deps.js"))))]
    (into {}
      (for [[path provides] paths-to-provides
            provide provides]
        [(symbol provide) (str "goog/" (second (re-find #"(.*)\.js$" path)))]))))

(def closure-index-mem (memoize closure-index))

(defn- skip-load?
  [{:keys [name macros]}]
  (cond
    (= name 'cljs.core) true
    (and (= name 'cljs.pprint) macros) true
    :else false))

(defn- do-load-file
  [file cb]
  (when-not (load-and-callback! file :clj :calculate-cache-prefix cb)
    (cb nil)))

(defn- do-load-goog
  [name cb]
  (if-let [goog-path (get (closure-index-mem) name)]
    (when-not (load-and-callback! (str goog-path ".js") :js nil cb)
      (cb nil))
    (cb nil)))

(defn- do-load-other
  [path macros cb]
  (loop [extensions (if macros
                      [".clj" ".cljc"]
                      [".cljs" ".cljc" ".js"])]
    (if extensions
      (when-not (load-and-callback!
                  (str path (first extensions))
                  (extension->lang (first extensions))
                  (cache-prefix-for-path path)
                  cb)
        (recur (next extensions)))
      (cb nil))))

; file here is an alternate parameter denoting a filesystem path
(defn load
  [{:keys [name macros path file] :as full} cb]
  (cond
    (skip-load? full) (cb {:lang   :js
                           :source ""})
    file (do-load-file file cb)
    (re-matches #"^goog/.*" path) (do-load-goog name cb)
    :else (do-load-other path macros cb)))

(defn- handle-error
  [e include-stacktrace? in-exit-context?]
  (let [cause (or (.-cause e) e)
        is-planck-exit-exception? (= "PLANCK_EXIT" (.-message cause))]
    (when-not is-planck-exit-exception?
      (print-error e include-stacktrace?))
    (if (and in-exit-context? (not is-planck-exit-exception?))
      (js/PLANCK_SET_EXIT_VALUE 1)
      (set! *e e))))

(defn ^:export run-main
  [main-ns & args]
  (let [main-args (js->clj args)]
    (binding [cljs/*load-fn* load
              cljs/*eval-fn* caching-js-eval]
      (process-require
        :require
        (fn [_]
          (cljs/eval-str st
            (str "(var -main)")
            nil
            (merge (make-base-eval-opts)
              {:ns         (symbol main-ns)
               :source-map true})
            (fn [{:keys [ns value error] :as ret}]
              (try
                (apply value args)
                (catch :default e
                  (handle-error e true true))))))
        `[(quote ~(symbol main-ns))]))
    nil))

(defn load-core-source-maps!
  []
  (when-not (get (:source-maps @planck.repl/st) 'planck.repl)
    (swap! st update-in [:source-maps] merge {'planck.repl
                                              (sm/decode
                                                (cljson->clj
                                                  (first (js/PLANCK_LOAD "planck/repl.js.map"))))
                                              'cljs.core
                                              (sm/decode
                                                (cljson->clj
                                                  (first (js/PLANCK_LOAD "cljs/core.js.map"))))})))

(defn print-error
  ([error]
   (print-error error true))
  ([error include-stacktrace?]
   (let [cause (or (.-cause error) error)]
     (println (.-message cause))
     (when include-stacktrace?
       (load-core-source-maps!)
       (let [canonical-stacktrace (st/parse-stacktrace
                                    {}
                                    (.-stack cause)
                                    {:ua-product :safari}
                                    {:output-dir "file://(/goog/..)?"})]
         (println
           (st/mapped-stacktrace-str
             canonical-stacktrace
             (or (:source-maps @planck.repl/st) {})
             nil)))))))

(defn get-var
  [env sym]
  (let [var (with-compiler-env st (resolve env sym))
        var (or var
              (if-let [macro-var (with-compiler-env st
                                   (resolve env (symbol "cljs.core$macros" (name sym))))]
                (update (assoc macro-var :ns 'cljs.core)
                  :name #(symbol "cljs.core" (name %)))))]
    (if (= (namespace (:name var)) (str (:ns var)))
      (update var :name #(symbol (name %)))
      var)))

(defn- get-file-source
  [filepath]
  (if (symbol? filepath)
    (let [without-extension (s/replace
                              (s/replace (name filepath) #"\." "/")
                              #"-" "_")]
      (or
        (first (js/PLANCK_LOAD (str without-extension ".clj")))
        (first (js/PLANCK_LOAD (str without-extension ".cljc")))
        (first (js/PLANCK_LOAD (str without-extension ".cljs")))))
    (let [file-source (first (js/PLANCK_LOAD filepath))]
      (or file-source
        (first (js/PLANCK_LOAD (s/replace filepath #"^out/" "")))
        (first (js/PLANCK_LOAD (s/replace filepath #"^src/" "")))))))

(defn- fetch-source
  [var]
  (when-let [filepath (or (:file var) (:file (:meta var)))]
    (when-let [file-source (get-file-source filepath)]
      (let [rdr (rt/source-logging-push-back-reader file-source)]
        (dotimes [_ (dec (:line var))] (rt/read-line rdr))
        (-> (r/read {:read-cond :allow :features #{:cljs}} rdr)
          meta :source)))))

(defn- run-async!
  "Like cljs.core/run!, but for an async procedure, and with the
  ability to break prior to processing the entire collection.

  Chains successive calls to the supplied procedure for items in
  the collection. The procedure should accept an item from the
  collection and a callback of one argument. If the break? predicate,
  when applied to the procedure callback value, yields a truthy
  result, terminates early calling the supplied cb with the callback
  value. Otherwise, when complete, calls cb with nil."
  [proc coll break? cb]
  (if (seq coll)
    (proc (first coll)
      (fn [res]
        (if (break? res)
          (cb res)
          (run-async! proc (rest coll) break? cb))))
    (cb nil)))

(defn- process-deps
  [names opts cb]
  (run-async! (fn [name cb]
                (cljs/require name opts cb))
    names
    :error
    cb))

(defn- process-macros-deps
  [cache cb]
  (process-deps (distinct (vals (:require-macros cache))) {:macros-ns true} cb))

(defn- process-libs-deps
  [cache cb]
  (process-deps (distinct (concat (vals (:requires cache)) (vals (:imports cache)))) {} cb))

(declare execute-source)

(defn- process-execute-path
  [file {:keys [in-exit-context?] :as opts}]
  (load {:file file}
    (fn [{:keys [lang source cache]}]
      (if source
        (case lang
          :clj (execute-source ["text" source] opts)
          :js (process-macros-deps cache
                (fn [res]
                  (if-let [error (:error res)]
                    (handle-error (js/Error. error) false in-exit-context?)
                    (process-libs-deps cache
                      (fn [res]
                        (if-let [error (:error res)]
                          (handle-error (js/Error. error) false in-exit-context?)
                          (js/eval source))))))))
        (handle-error (js/Error. (str "Could not load file " file)) false in-exit-context?)))))

(defn- process-doc
  [env sym]
  (cond
    (special-doc-map sym) (repl/print-doc (special-doc sym))
    (repl-special-doc-map sym) (repl/print-doc (repl-special-doc sym))
    :else (repl/print-doc (get-var env sym))))

(defn- process-pst
  [expr]
  (let [expr (or expr '*e)]
    (try (cljs/eval st
           expr
           (make-base-eval-opts)
           print-error)
         (catch js/Error e (prn :caught e)))))

(defn- process-load-file
  [argument {:keys [in-exit-context?] :as opts}]
  (let [filename argument]
    (try
      (execute-source ["path" filename] opts)
      (catch :default e
        (handle-error e false in-exit-context?)))))

(defn- process-repl-special
  [expression-form {:keys [print-nil-expression? in-exit-context?] :as opts}]
  (let [env (assoc (ana/empty-env) :context :expr
                                   :ns {:name @current-ns})
        argument (second expression-form)]
    (case (first expression-form)
      in-ns (process-in-ns argument)
      require (process-require :require identity (rest expression-form))
      require-macros (process-require :require-macros identity (rest expression-form))
      import (process-require :import identity (rest expression-form))
      doc (process-doc env argument)
      source (println (fetch-source (get-var env argument)))
      pst (process-pst argument)
      load-file (process-load-file argument opts))
    (when print-nil-expression?
      (prn nil))))

(defn- process-1-2-3
  [expression-form value]
  (when-not
    (or ('#{*1 *2 *3 *e} expression-form)
      (ns-form? expression-form))
    (set! *3 *2)
    (set! *2 *1)
    (set! *1 value)))

(defn- cache-source-fn
  [source-text]
  (fn [x cb]
    (when (and (= "File" (:name x)) (:source x))
      (let [source (:source x)
            [file-namespace relpath] (extract-cache-metadata-mem source-text)]
        (js/PLANCK_CACHE (cache-prefix-for-path relpath) (str (compiled-by-string) "\n" source)
          (when file-namespace
            (cljs->transit-json (get-in @st [:cljs.analyzer/namespaces file-namespace]))))))
    (cb {:value nil})))

(defn- process-execute-source
  [source-text expression-form {:keys [expression? print-nil-expression? in-exit-context? include-stacktrace?] :as opts}]
  (try
    (cljs/eval-str
      st
      source-text
      (if expression? "Expression" "File")
      (merge
        {:ns         @current-ns
         :source-map false
         :verbose    (:verbose @app-env)}
        (if expression?
          {:context       :expr
           :def-emits-var true}
          (when (:cache-path @app-env)
            {:cache-source (cache-source-fn source-text)})))
      (fn [{:keys [ns value error] :as ret}]
        (if expression?
          (when-not error
            (when (or print-nil-expression?
                    (not (nil? value)))
              (prn value))
            (process-1-2-3 expression-form value)
            (reset! current-ns ns)
            nil))
        (when error
          (handle-error error include-stacktrace? in-exit-context?))))
    (catch :default e
      (handle-error e include-stacktrace? in-exit-context?))))

(defn execute-source
  [[source-type source-value] {:keys [expression?] :as opts}]
  (binding [ana/*cljs-ns* @current-ns
            *ns* (create-ns @current-ns)
            cljs/*load-fn* load
            cljs/*eval-fn* caching-js-eval]
    (if-not (= "text" source-type)
      (process-execute-path source-value opts)
      (let [source-text source-value
            expression-form (and expression? (repl-read-string source-text))]
        (if (repl-special? expression-form)
          (process-repl-special expression-form opts)
          (process-execute-source source-text expression-form opts))))))

(defn ^:export execute
  [source expression? print-nil-expression? in-exit-context?]
  (execute-source source {:expression?           expression?
                          :print-nil-expression? print-nil-expression?
                          :in-exit-context?      in-exit-context?
                          :include-stacktrace?   true}))
