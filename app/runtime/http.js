/* app/runtime/http.js — $http.fetch + $notify.on('http') callback routing
 *
 * C bridge: $http object injected by http_load() in kwcc_js_http.c.
 * $http.cancel and $http.config are C→JS bridge methods.
 * $http.fetch and callback routing are pure JS logic defined here.
 * C→JS notifications arrive via $notify.emit("http", event, id, data).
 */

/* ── Callback routing ──────────────────────────────────────── */

$http.callbacks = {};

$notify.on('http', function(event, id, data) {
    var cb = $http.callbacks[id];
    if (!cb) return;

    if (event === 'end') {
        cb.resolve(data);
        delete $http.callbacks[id];
        $http.state.activeRequests = $http.state.activeRequests - 1;
    } else if (event === 'error') {
        cb.reject(data);
        delete $http.callbacks[id];
        $http.state.activeRequests = $http.state.activeRequests - 1;
    } else if (event === 'cancel') {
        cb.reject(data || 'request cancelled');
        delete $http.callbacks[id];
        $http.state.activeRequests = $http.state.activeRequests - 1;
    } else if (event === 'progress') {
        if (cb.onProgress) cb.onProgress(data.loaded, data.total);
    }
});

/* ── $http.fetch(url, options) → MiniPromise ───────────────── */

$http.fetch = function(url, options) {
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
        var reqId = kwcc_js_call_c('http', 'request', method, url, headers, body);
        if (!reqId) {
            reject("request failed: unable to start");
            return;
        }
        $http.callbacks[reqId] = {
            resolve: resolve,
            reject: reject,
            onProgress: onProgress
        };
        $http.state.activeRequests = $http.state.activeRequests + 1;
    });
};
