/* jsapi.h — kwcc JS runtime support (stdlib stubs + UI bridge)
   Must be included before mqjs_stdlib.h */
#ifndef KWCC_JSAPI_H
#define KWCC_JSAPI_H

#include "mquickjs/mquickjs.h"

JSValue js_print(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_gc(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_load(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_setTimeout(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_clearTimeout(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_date_now(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_performance_now(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_kwcc_ui(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);

typedef JSValue (*JSUICallback)(JSContext *ctx, const char *method, int argc, JSValue *argv);
void kwcc_set_ui_callback(JSUICallback cb);

#endif
