/* kwcc_js.h — kwcc JS lifecycle + runtime support (stdlib stubs + $config JS API) */
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

typedef JSValue (*JSUICallback)(JSContext *ctx, const char *method, int argc, JSValue *argv);
void kwcc_set_ui_callback(JSUICallback cb);

/* $config JS API — Phase 4 */
void kwcc_register_config_js(JSContext *ctx);

/* C handlers (registered as JS global functions) */
JSValue kwcc_js_config_set_app_int(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue kwcc_js_config_set_app_string(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue kwcc_js_config_set_app_bool(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue kwcc_js_config_set_app_json(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue kwcc_js_config_set_app_tlv(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue kwcc_js_config_get_app(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue kwcc_js_config_get_app_tlv_path(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue kwcc_js_config_get_app_tlv_json(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue kwcc_js_config_release_app(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue kwcc_js_config_release_app_prefix(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue kwcc_js_config_set_core_tlv(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue kwcc_js_config_set_max_pools(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);

JSValue kwcc_js_mempool_dump_stats(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue kwcc_js_mempool_dump_all(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);

/* $http JS API */
void kwcc_register_http_js(JSContext *ctx);
JSValue kwcc_js_http_request(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue kwcc_js_http_cancel(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);

/* Proxy: dynamic handler dispatch (avoids modifying mqjs_stdlib.c) */
JSValue kwcc_js_mquickjs_call(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);

/* TLV conversion helper */
uint8_t *kwcc_js_value_to_tlv(JSContext *ctx, JSValue js_val, size_t *out_len);

#endif /* KWCC_JS_H */
