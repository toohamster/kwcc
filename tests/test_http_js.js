/* tests/test_http_js.js — JS integration tests for HTTP Plugin + $notify + MiniPromise
 *
 * A group: synchronous, no network required
 * B group: asynchronous, requires curl + network
 */

var test_pass = 0;
var test_fail = 0;

function assert(cond, name) {
    if (cond) {
        test_pass = test_pass + 1;
        print("  PASS: " + name);
    } else {
        test_fail = test_fail + 1;
        print("  FAIL: " + name);
    }
}

/* ═══════════════════════════════════════════════════════════════
 * A group: synchronous tests (no network)
 * ═══════════════════════════════════════════════════════════════ */

/* A1-A6: $http object */
assert(typeof $http === "object", "A1: $http object exists");
assert($http.state && $http.state.activeRequests === 0, "A2: $http.state.activeRequests === 0");
assert(typeof $http.cancel === "function", "A3: $http.cancel is function");
assert(typeof $http.fetch === "function", "A4: $http.fetch is function");
assert(typeof $http.callbacks === "object", "A5: $http.callbacks exists");

/* A6-A9: $notify object */
assert(typeof $notify === "object", "A6: $notify object exists");
assert(typeof $notify.on === "function", "A7: $notify.on is function");
assert(typeof $notify.emit === "function", "A8: $notify.emit is function");
assert($notify.registry && $notify.registry["http"] !== undefined, "A9: $notify.registry['http'] registered");

/* A10-A12: MiniPromise */
assert(typeof MiniPromise === "function", "A10: MiniPromise constructor exists");

/* A11: sync resolve */
var a11_val = null;
new MiniPromise(function(r) { r(42); }).then(function(v) { a11_val = v; });
assert(a11_val === 42, "A11: MiniPromise sync resolve (value=" + a11_val + ")");

/* A12: sync reject */
var a12_val = null;
new MiniPromise(function(_, r) { r("err"); }).catch(function(e) { a12_val = e; });
assert(a12_val === "err", "A12: MiniPromise sync reject (value=" + a12_val + ")");

/* A13-A15: kwcc_js_call_c dispatch */
assert(typeof kwcc_js_call_c === "function", "A13: kwcc_js_call_c global exists");
assert(kwcc_js_call_c("http", "request") === undefined, "A14: kwcc_js_call_c insufficient args returns undefined");
assert(kwcc_js_call_c("unknown", "func") === undefined, "A15: kwcc_js_call_c unknown module returns undefined");

print("=== HTTP JS A group: " + test_pass + " passed, " + test_fail + " failed ===");

/* ═══════════════════════════════════════════════════════════════
 * B group: asynchronous tests (requires curl + network)
 * ═══════════════════════════════════════════════════════════════ */

var test_async_pass = 0;
var test_async_fail = 0;
var test_async_done = 0;
var test_async_total = 4;  /* B1-B4, B5 may be skipped */

function asyncAssert(cond, name) {
    if (cond) {
        test_async_pass = test_async_pass + 1;
        print("  PASS: " + name);
    } else {
        test_async_fail = test_async_fail + 1;
        print("  FAIL: " + name);
    }
}

/* Pre-check: can we start a request? */
var checkId = kwcc_js_call_c("http", "request", "GET", "https://httpbin.org/get");
if (!checkId) {
    print("=== HTTP JS B group: SKIPPED (curl not found or max concurrent reached) ===");
} else {
    /* Cancel the check request — we just wanted to verify curl works */
    kwcc_js_call_c("http", "cancel", checkId);

    /* B1: fetch GET success */
    $http.fetch("https://httpbin.org/get").then(function(data) {
        asyncAssert(data.status === 200, "B1: GET status === 200 (got " + data.status + ")");
        asyncAssert(data.body.length > 0, "B1: GET body non-empty");
        asyncAssert(typeof data.headers === "object", "B1: GET headers is object");
        asyncAssert(data.reqId.length > 0, "B1: GET reqId present");
        test_async_done = test_async_done + 1;
    }).catch(function(err) {
        asyncAssert(false, "B1: GET unexpected reject");
        test_async_done = test_async_done + 1;
    });

    /* B2: fetch 404 */
    $http.fetch("https://httpbin.org/status/404").then(function(data) {
        asyncAssert(data.status === 404, "B2: 404 status === 404 (got " + data.status + ")");
        test_async_done = test_async_done + 1;
    }).catch(function(err) {
        asyncAssert(false, "B2: 404 unexpected reject");
        test_async_done = test_async_done + 1;
    });

    /* B3: fetch POST */
    $http.fetch("https://httpbin.org/post", {method: "POST", body: "hello"}).then(function(data) {
        asyncAssert(data.status === 200, "B3: POST status === 200 (got " + data.status + ")");
        test_async_done = test_async_done + 1;
    }).catch(function(err) {
        asyncAssert(false, "B3: POST unexpected reject");
        test_async_done = test_async_done + 1;
    });

    /* B4: cancel → reject */
    var cancelReqId = kwcc_js_call_c("http", "request", "GET", "https://httpbin.org/delay/5");
    if (cancelReqId) {
        $http.callbacks[cancelReqId] = {
            resolve: function() { asyncAssert(false, "B4: cancel should not resolve"); test_async_done = test_async_done + 1; },
            reject: function(err) { asyncAssert(err && err.error === "cancelled", "B4: cancel reject error === cancelled (got " + (err ? err.error : "null") + ")"); test_async_done = test_async_done + 1; },
            onProgress: null
        };
        $http.state.activeRequests = $http.state.activeRequests + 1;
        $http.cancel(cancelReqId);
    } else {
        test_async_done = test_async_done + 1;
    }

    /* B group: each callback prints its own result immediately.
     * No setTimeout polling — mquickjs has no setTimeout.
     * Summary is printed per-callback; overall status visible in log. */
}
