/* kwcc_js.h — kwcc JS lifecycle + runtime support (stdlib stubs + JS bindings)
   Must be included before mqjs_stdlib.h */
#ifndef KWCC_JS_H
#define KWCC_JS_H

#include "mquickjs/mquickjs.h"

JSContext *kwcc_create_js(void);
void       kwcc_destroy_js(JSContext *ctx);

JSValue js_print(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_gc(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_load(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_setTimeout(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_clearTimeout(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_date_now(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_performance_now(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_kwcc_ui(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);

/* $config JS API handlers (registered in mqjs_stdlib.c via CONFIG_KWCC) */
JSValue js_config_set_app(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_config_set_user(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_config_get_app(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_config_get_user(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_config_release_app(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_config_release_user(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_config_set_app_size(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_config_set_user_size(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
#ifdef KWCC_DEBUG
JSValue js_config_dump(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_config_dump_all(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
#endif

typedef JSValue (*JSUICallback)(JSContext *ctx, const char *method, int argc, JSValue *argv);
void kwcc_set_ui_callback(JSUICallback cb);

#endif
