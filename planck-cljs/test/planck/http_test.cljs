(ns planck.http-test
  (:require
   [clojure.test :refer [deftest is testing]]
   [cognitect.transit :as transit]
   [planck.http :as http]
   [planck.io :as io])
  (:import
   (goog Uri)))

;; A server running an instance of https://github.com/mfikes/http-echo-clj
(def http-echo-server "http://http-test.planck-repl.org")

(deftest request-test
  (testing "request"
    (is (every? (#'http/request identity :get "url" {}) [:url :method :headers :timeout]))
    (is (= @#'http/default-timeout
          (:timeout (#'http/request identity :get "url" {}))))
    (is (= "GET" (:method (#'http/request identity :get "url" {}))))
    (is (= "url" (:url (#'http/request identity :get "url" {}))))
    (is (= 30 (:timeout (#'http/request identity :get "url" {:timeout 30}))))
    (is (= "foo=bar" (:body (#'http/request identity :get "url"
                             {:form-params {:foo "bar"}}))))
    (is (= (count "foo=bar") (-> (#'http/request identity :get "url"
                                  {:form-params {:foo "bar"}})
                               (get-in [:headers :Content-length]))))
    (let [params        [["foo" "bar"]]
          expected-body (#'http/generate-multipart-body
                         (#'http/boundary @#'http/boundary-constant)
                         (#'http/generate-form-data params))]
      (is (= (count expected-body) (-> (#'http/request identity :get "url"
                                        {:multipart-params params})
                                     :body
                                     count)))
      (is (= (count expected-body) (-> (#'http/request identity :get "url"
                                        {:multipart-params params})
                                     (get-in [:headers :Content-length])))))))

(deftest generate-query-string-test
  (testing "generate-query-string"
    (is (= "foo=bar" (#'http/generate-query-string {:foo "bar"})))
    (let [q (#'http/generate-query-string {:foo "bar" :bar "baz"})]
      (is (or (= q "bar=baz&foo=bar") (= q "foo=bar&bar=baz")))) ;; same difference
    (is (= "foo=b%3Dar" (#'http/generate-query-string {:foo "b=ar"})))))

(deftest wrap-content-type-test
  (testing "wrap-content-type"
    (is (= "application/json" (-> {:content-type :json :headers {}}
                                ((#'http/wrap-content-type identity))
                                (get-in [:headers "Content-Type"]))))
    (is (= "application/xml" (-> {:content-type :xml}
                               ((#'http/wrap-content-type identity))
                               (get-in [:headers "Content-Type"]))))
    (is (= "application/x-www-form-urlencoded" (-> {:content-type :form-urlencoded}
                                                 ((#'http/wrap-content-type identity))
                                                 (get-in [:headers "Content-Type"]))))
    (is (= "ugle" (-> {:content-type "ugle"}
                    ((#'http/wrap-content-type identity))
                    (get-in [:headers "Content-Type"]))))
    (is (= nil (-> {:content-type "ugle"}
                 ((#'http/wrap-content-type identity))
                 :content-type)))
    (is (= nil (-> {}
                 ((#'http/wrap-content-type identity))
                 (get-in [:headers "Content-Type"]))))))

(deftest wrap-accept-test
  (testing "wrap-accept"
    (is (= "application/json" (-> {:accept :json}
                                ((#'http/wrap-accept identity))
                                (get-in [:headers "Accept"]))))
    (is (= nil (-> {:accept :json}
                 ((#'http/wrap-accept identity))
                 :accept)))))

(deftest wrap-content-length-test
  (testing "wrap-content-length"
    (is (= (count "uglepose") (-> {:body "uglepose"}
                                ((#'http/wrap-add-content-length identity))
                                (get-in [:headers "Content-length"]))))))

(deftest wrap-form-params-test
  (testing "wrap-form-params"
    (is (= nil (-> {:form-params "foo"}
                 ((#'http/wrap-form-params identity))
                 :form-params)))
    (is (= "foo=bar" (-> {:form-params {:foo "bar"}}
                       ((#'http/wrap-form-params identity))
                       :body)))))

(deftest generate-form-data-test
  (testing "fileupload-stuff"
    (let [expected (str @#'http/content-disposition "foo\"\n\nbar")]
      (is (= (map #'planck.http/str->bytes [expected "--\n"])
            (#'http/generate-form-data [["foo" "bar"]]))))
    (is (= (map #'planck.http/str->bytes
             [(str @#'http/content-disposition "foo\"; filename=\"bar\"\n"
                "Content-Type: application/octet-stream\n\n" "baz") "--\n"])
          (#'http/generate-form-data [["foo" ["baz" "bar"]]])))))

(deftest generate-multipart-body-test
  (testing "generate-multipart-body"
    (is (= (#'planck.http/str->bytes "boundarypart1\nboundarypart2")
          (#'http/generate-multipart-body "boundary" (map #'planck.http/str->bytes ["part1" "part2"]))))))

(deftest boundary-test
  (testing "boundary"
    (is (not= nil (re-matches #"u\d{10}" (#'http/boundary "u"))))
    (is (not= (#'http/boundary "u") (#'http/boundary "u")))))

(deftest wrap-multipart-params-test
  (testing "wrap-multipart-params"
    (is (nil? (-> {:multipart-params [["foo" "bar"]]}
                ((#'http/wrap-multipart-params identity))
                :multipart-params)))
    (is (some? (-> {:multipart-params [["foo" "bar"]]}
                 ((#'http/wrap-multipart-params identity))
                 :body)))
    (is (some? (-> {:multipart-params [["foo" "bar"]]}
                 ((#'http/wrap-multipart-params identity))
                 :content-type)))))

(deftest wrap-throw-on-error-test
  (testing "wrap-throw-on-error"
    (is (= "Error: foo" (try (-> {:error "foo"}
                               ((#'http/wrap-throw-on-error identity)))
                             (catch js/Object e
                               (.toString e)))))))

(deftest uri-coercions-test
  (let [http-uri (Uri. "http://example.com")
        file-uri (Uri. "file:///tmp")]
    (is (identical? http-uri (io/as-url http-uri)))
    (is (identical? file-uri (io/as-url file-uri)))
    (is (thrown? js/Error (io/as-file http-uri)))
    (is (= (io/->File "/tmp") (io/as-file file-uri)))))

(defn transit->clj
  [s]
  (let [reader (transit/reader :json)]
    (transit/read reader s)))

(defn form-full-url
  [url-suffix]
  (str http-echo-server url-suffix))

(defn do-request
  ([method url-suffix] (do-request method url-suffix {}))
  ([method url-suffix opts]
   (let [opts (assoc opts :timeout 30)
         full-url (form-full-url url-suffix)]
     (->>
       (case method
         :get (http/get full-url opts)
         :head (http/head full-url opts)
         :delete (http/delete full-url opts)
         :post (http/post full-url opts)
         :put (http/put full-url opts)
         :patch (http/patch full-url opts))
       :body
       transit->clj))))

(deftest request-uri-test
  (is (= "/" (:uri (do-request :get "/"))))
  (is (= "/" (:uri (do-request :post "/"))))
  (is (= "/" (:uri (do-request :get "/"))))
  (is (= "/foo" (:uri (do-request :post "/foo"))))
  (is (= "/foo" (:uri (do-request :put "/foo"))))
  (is (= "/foo" (:uri (do-request :patch "/foo")))))

(deftest request-json-body-test
  (is (= "\"foo\"" (:body (do-request :post "/" {:content-type :json
                                                 :accept :json
                                                 :body "\"foo\""}))))
  (is (= "5" (get-in (do-request :post "/" {:content-type :json
                                            :accept :json
                                            :body "\"foo\""})
                   [:headers "content-length"]))))

(deftest http-request-debug
  (let [url              (form-full-url "/")
        expected-request (fn [method]
                           {:url     url
                            :method  method
                            :headers {}
                            :timeout 5})]
    (is (= (expected-request "GET")
          (:request (http/get url {:debug true}))))
    (is (= (expected-request "DELETE")
          (:request (http/delete url {:debug true}))))
    (is (= (expected-request "POST")
          (:request (http/post url {:debug true}))))
    (is (= (expected-request "PUT")
          (:request (http/put url {:debug true}))))
    (is (= (expected-request "PATCH")
          (:request (http/patch url {:debug true}))))))

;; Commented until https://github.com/planck-repl/planck/issues/966 fixed
#_(deftest binary-body-response-test
  (let [response (http/get "http://planck-repl.org/releases/andare/andare-0.2.0.jar"
                   {:binary-response  true
                    :follow-redirects true})]
    (is (vector? (:body response)))
    (is (= 64328 (count (:body response))))
    (is (= '[0x50 0x4b 0x03] (take 3 (:body response))))))

(deftest http-user-agent
  (let [user-agent "Some-User-Agent/1.2.3"]
    (is (= user-agent (get (:headers (do-request :get "/" {:user-agent user-agent})) "user-agent")))
    (is (not (contains? (:headers (do-request :get "/")) "user-agent")))))
