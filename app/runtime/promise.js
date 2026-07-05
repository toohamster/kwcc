/* app/runtime/promise.js — MiniPromise (ES5 compatible)
 *
 * Minimal Promise implementation for mquickjs.
 * Independent of HTTP — reusable by any async module (WS, Timer, etc.).
 */

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
        function handleFulfilled(val) {
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
        function handleRejected(err) {
            try {
                var result = onRejected ? onRejected(err) : err;
                if (result && typeof result.then === "function") {
                    result.then(resolve, reject);
                } else if (onRejected) {
                    resolve(result);
                } else {
                    reject(result);
                }
            } catch (e) {
                reject(e);
            }
        }
        if (self.status === "FULFILLED") {
            handleFulfilled(self.value);
        } else if (self.status === "REJECTED") {
            handleRejected(self.value);
        } else {
            self.callbacks.push({ fn: handleFulfilled, reject: handleRejected });
        }
    });
};

MiniPromise.prototype.catch = function(onRejected) {
    return this.then(null, onRejected);
};
