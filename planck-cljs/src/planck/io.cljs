(ns planck.io
  (:require [clojure.string :as s]
            [planck.core])
  (:import goog.Uri))

(defrecord File [path]
  Object
  (toString [_] path))

(defn build-uri
    "Builds a URI"
    [scheme server-name server-port uri query-string]
    (doto (Uri.)
      (.setScheme (name (or scheme "http")))
      (.setDomain server-name)
      (.setPort server-port)
      (.setPath uri)
      (.setQuery query-string true)))


(defprotocol Coercions
  "Coerce between various 'resource-namish' things."
  (as-file [x] "Coerce argument to a File.")
  (as-url [x] "Coerce argument to a goog.Uri."))

(extend-protocol Coercions
  nil
  (as-file [_] nil)
  (as-url [_] nil)

  string
  (as-file [s] (File. s))
  (as-url [s] (Uri. s))

  File
  (as-file [f] f)
  (as-url [f] (build-uri :file nil nil (:path f) nil)))

(defn- as-url-or-file [f]
  (if (s/starts-with? f "http")
    (as-url f)
    (as-file f)))

(defprotocol IOFactory
  "Factory functions that create ready-to-use versions of
  the various stream types, on top of anything that can
  be unequivocally converted to the requested kind of stream.

  Common options include

    :append   true to open stream in append mode
    :encoding  string name of encoding to use, e.g. \"UTF-8\".

    Callers should generally prefer the higher level API provided by
    reader, writer, input-stream, and output-stream."
  (make-reader [x opts] "Creates an IReader. See also IOFactory docs.")
  (make-writer [x opts] "Creates an IWriter. See also IOFactory docs.")
  (make-input-stream [x opts] "Creates an IInputStream. See also IOFactory docs.")
  (make-output-stream [x opts] "Creates an IOutputStream. See also IOFactory docs."))

(extend-protocol IOFactory
  string
  (make-reader [s opts]
    (make-reader (as-url-or-file s) opts))
  (make-writer [s opts]
    (make-writer (as-url-or-file s) opts))
  (make-input-stream [s opts]
    (make-input-stream (as-file s) opts))
  (make-output-stream [s opts]
    (make-output-stream (as-file s) opts))

  File
  (make-reader [file opts]
    (let [file-reader (js/PLANCK_FILE_READER_OPEN (:path file) (:encoding opts))]
      (planck.core/BufferedReader.
        (fn [] (let [[result err] (js/PLANCK_FILE_READER_READ file-reader)]
                 (if err
                   (throw (js/Error. err)))
                 result))
        (fn [] (js/PLANCK_FILE_READER_CLOSE file-reader))
        (atom nil))))
  (make-writer [file opts]
    (let [file-writer (js/PLANCK_FILE_WRITER_OPEN (:path file) (boolean (:append opts)) (:encoding opts))]
      (planck.core/Writer.
        (fn [s] (if-let [err (js/PLANCK_FILE_WRITER_WRITE file-writer s)]
                  (throw (js/Error. err)))
          nil)
        (fn [])
        (fn [] (js/PLANCK_FILE_WRITER_CLOSE file-writer)))))
  (make-input-stream [file opts]
    (let [file-input-stream (js/PLANCK_FILE_INPUT_STREAM_OPEN (:path file))]
      (planck.core/InputStream.
        (fn [] (js->clj (js/PLANCK_FILE_INPUT_STREAM_READ file-input-stream)))
        (fn [] (js/PLANCK_FILE_INPUT_STREAM_CLOSE file-input-stream)))))
  (make-output-stream [file opts]
    (let [file-output-stream (js/PLANCK_FILE_OUTPUT_STREAM_OPEN (:path file) (boolean (:append opts)))]
      (planck.core/OutputStream.
        (fn [byte-array] (js/PLANCK_FILE_OUTPUT_STREAM_WRITE file-output-stream (clj->js byte-array)))
        (fn [])
        (fn [] (js/PLANCK_FILE_OUTPUT_STREAM_CLOSE file-output-stream))))))

(defn reader
  "Attempts to coerce its argument into an open IBufferedReader."
  [x & opts]
  (make-reader x (when opts (apply hash-map opts))))

(defn writer
  "Attempts to coerce its argument into an open IWriter."
  [x & opts]
  (make-writer x (when opts (apply hash-map opts))))

(defn input-stream
  "Attempts to coerce its argument into an open IInputStream."
  [x & opts]
  (make-input-stream x (when opts (apply hash-map opts))))

(defn output-stream
  "Attempts to coerce its argument into an open IOutputStream."
  [x & opts]
  (make-output-stream x (when opts (apply hash-map opts))))

(def path-separator "/")

(defn file
  "Returns a File for given path.  Multiple-arg
   versions treat the first argument as parent and subsequent args as
   children relative to the parent."
  ([path]
   (File. path))
  ([parent & more]
   (File. (apply str parent (interleave (repeat path-separator) more)))))

(defn file-attributes
  "Returns a map containing the attributes of the item at a given path."
  [path]
  (some-> path
          as-file
          :path
          js/PLANCK_FSTAT
          (js->clj :keywordize-keys true)
          (update-in [:type] keyword)
          (update-in [:created] #(js/Date. %))
          (update-in [:modified] #(js/Date. %))))

(defn delete-file
  "Delete file f."
  [f]
  (js/PLANCK_DELETE (:path (as-file f))))

(defn directory?
  "Checks if dir is a directory."
  [dir]
  (js/PLANCK_IS_DIRECTORY (:path (as-file dir))))

(defn- create-listener [fn]
  (fn [socket client]
    (let [m (js/PLANCK_SOCKET_READ socket client)]
      (println "message was" m)
      (fn m))))

(defn socket-open [host port fn]
  (let [socket (js/PLANCK_SOCKET_OPEN host port)]
    (js/PLANCK_SOCKET_LISTEN socket (create-listener fn))))

(comment 
  (planck.io/socket-open "localhost" 8080 #(do
                                           (println "Foo")
                                           (str "you said: " % "\n"))))

;; These have been moved
(def ^:deprecated read-line planck.core/read-line)
(def ^:deprecated slurp planck.core/slurp)
(def ^:deprecated spit planck.core/spit)

(set! planck.core/*reader-fn* reader)
(set! planck.core/*writer-fn* writer)
(set! planck.core/*as-file-fn* as-file)
