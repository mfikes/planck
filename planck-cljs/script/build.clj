(ns script.bootstrap.build
  (:require [clojure.java.io :as io]
            [cljs.build.api :as api]
            [cljs.analyzer :as ana]
            [cljs.env :as env]
            [cljs.compiler :as comp]
            [cognitect.transit :as transit])
  (:import [java.io ByteArrayOutputStream File FileInputStream]))

(def sandbox-build? (boolean (System/getenv "SANDBOX_BUILD")))

(def experimental-warnings #{:recur-type-mismatch :private-var-access})

(def non-fatal-warnings (into #{:redef} experimental-warnings))

(defn delete-recursively [fname]
  (doseq [f (reverse (file-seq (io/file fname)))]
    (io/delete-file f true)))

(cljs.analyzer/with-warning-handlers
  [(fn [warning-type env extra]
     (when (warning-type cljs.analyzer/*cljs-warnings*)
       (when-let [s (cljs.analyzer/error-message warning-type extra)]
         (binding [*out* *err*]
           (println "WARNING:" (cljs.analyzer/message env s)))
         (when-not (warning-type non-fatal-warnings)
           (System/exit 1)))))]
  (api/build (api/inputs "src")
    {:output-dir         "out"
     :output-to          "out/main.js"
     :optimizations      :none
     :static-fns         true
     :optimize-constants false
     :dump-core          false
     :parallel-build     true
     :foreign-libs       [{:file     "jscomp.js"
                           :provides ["google-closure-compiler-js"]}
                          {:file     "paredit.js"
                           :provides ["paredit"]
                           :global-exports '{paredit paredit}}]
     :compiler-stats     false
     :aot-cache          (not sandbox-build?)}))

(defn copy-source
  [filename]
  (spit (str "out/" filename)
    (slurp (io/resource filename))))

(copy-source "cljs/test.cljc")
(copy-source "cljs/pprint.cljc")
(copy-source "cljs/spec/alpha.cljc")
(copy-source "cljs/spec/test/alpha.cljc")
(copy-source "cljs/spec/test/alpha.cljs")
(copy-source "cljs/spec/gen/alpha.cljc")
(copy-source "cljs/analyzer/api.cljc")
(copy-source "cljs/analyzer/macros.clj")
(copy-source "cljs/compiler/macros.clj")
(copy-source "cljs/env/macros.clj")
(copy-source "clojure/template.clj")

(try
  (copy-source "cljs/core/specs/alpha.cljc")
  (copy-source "cljs/core/specs/alpha.cljs")
  (catch Throwable _))

(defn write-cache [cache out-path]
  (let [out (ByteArrayOutputStream. 1000000)
        writer (transit/writer out :json)]
    (transit/write writer cache)
    (spit (io/file out-path) (.toString out))))

;; Needed in the case that we are depending on a
;; ClojureScript source tree via deps.edn (instead of
;; on a built ClojureScript JAR). This is a rough copy
;; of the functionality in cljs.closure/aot-cache-core
(defn aot-cache-core []
  (let [src (io/file (.getFile (io/resource "cljs/core.cljs")))
        base (io/file "/tmp/cljs-aot-cache")
        dest (io/file base "core.aot.js")
        cache (io/file base "core.cljs.cache.aot.edn")]
    (io/make-parents dest)
    (env/with-compiler-env (env/default-compiler-env {:infer-externs true})
      (comp/compile-file src dest
        {:source-map true
         :source-map-url "core.js.map"
         :output-dir (str "src" File/separator "main" File/separator "cljs")})
      (ana/write-analysis-cache 'cljs.core cache src))))

(try
  (let [res (or (io/resource "cljs/core.cljs.cache.aot.edn")
              (do (aot-cache-core)
                  "/tmp/cljs-aot-cache/core.cljs.cache.aot.edn"))
        cache (read-string (slurp res))]
    (doseq [key (keys cache)]
      (write-cache (key cache) (str "out/cljs/core.cljs.cache.aot." (munge key) ".json"))))
  (finally
    (when (.exists (io/file "/tmp/cljs-aot-cache"))
      (delete-recursively (io/file "/tmp/cljs-aot-cache")))))

(let [res "out/cljs/core$macros.cljc.cache.json"
      cache (transit/read (transit/reader (FileInputStream. res) :json))]
  (doseq [key (keys cache)]
    (write-cache (key cache) (str "out/cljs/core$macros.cljc.cache." (munge key) ".json"))))

(System/exit 0)
