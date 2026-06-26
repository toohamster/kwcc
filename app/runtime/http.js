/* app/runtime/http.js — MiniPromise + $http high-level API
 *
 * ES5 compatible (mquickjs).
 * C bridge functions are injected by kwcc_register_http_js() in kwcc_js.c.
 */

/* ── MiniPromise (ES5 compatible) ───────────────────────────── */

function MiniPromise(executor) {
    var self = this;
    self.status = "PENDING";
    self.value = null;
    self.callbacks = [];

    executor(function(val) {
        if (self.status === "PENDING") {
            self.status = "FULFILLED";
            self.value = val;
            for (var i = 0; i < self.callbacks.length; i++) {
                self.callbacks[i].fn(val);
            }
        }
    }, function(err) {
        if (self.status === "PENDING") {
            self.status = "REJECTED";
            self.value = err;
            for (var i = 0; i < self.callbacks.length; i++) {
                var cb = self.callbacks[i];
                if (cb.reject) cb.reject(err);
            }
        }
    });
}

MiniPromise.prototype.then = function(onFulfilled, onRejected) {
    var self = this;
    return new MiniPromise(function(resolve, reject) {
        function wrapped(val) {
            try {
                var result = onFulfilled ? onFulfilled(val) : val;
                if (result && typeof result.then === "function") {
                    result.then(resolve, reject);
                } else {
                    resolve(result);
                }
            } catch (e) {
                reject(e);
            }
        }
        if (self.status === "FULFILLED") {
            wrapped(self.value);
        } else {
            self.callbacks.push({ fn: wrapped, reject: reject });
        }
    });
};

MiniPromise.prototype.catch = function(onRejected) {
    return this.then(null, onRejected);
};

/* ── $http.request(url, options) ────────────────────────────── */

$http.request = function(url, options) {
    var method = "GET";
    var headers = [];
    var body = "";
    var onProgress = null;
    if (options) {
        if (options.method) method = options.method;
        if (options.headers) headers = options.headers;
        if (options.body) body = options.body;
        if (options.onProgress) onProgress = options.onProgress;
    }

    return new MiniPromise(function(resolve, reject) {
        /* $http.request is the C bridge injected by kwcc_register_http_js().
         * It calls kwcc_js_http_request which:
         *   1. Calls kwcc_http_request() to start the fork+pipe+curl request
         *   2. Registers resolve/reject/onProgress in the callback registry
         *   3. Returns req_id string
         * When the request completes, kwcc_js_on_bus_event routes the
         * http/end/<req_id> or http/error/<req_id> event to the correct callback. */
        var reqId = $http.request(method, url, headers, body, resolve, reject, onProgress);
        if (!reqId) {
            reject("request failed: unable to start (curl not found or max concurrent reached)");
            return;
        }
        $http.state.activeRequests = $http.state.activeRequests + 1;
    });
};
