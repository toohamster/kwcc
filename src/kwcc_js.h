/* kwcc_js.h — kwcc JS lifecycle + runtime support (stdlib stubs + $config JS API) */
#ifndef KWCC_JS_H
#define KWCC_JS_H

#include <stdint.h>
#include <stddef.h>
#include "mquickjs/mquickjs.h"

/* ═══════════════════════════════════════════════════════════════
 * Facade types — sub-modules use these, never include mquickjs.h
 * ═══════════════════════════════════════════════════════════════ */

/* Opaque JS value handle (same layout as JSValue, sub-modules never touch internals) */
typedef JSValue kwcc_js_val_t;

/* C string conversion buffer (matches mquickjs JSCStringBuf[5]) */
typedef struct { char buf[5]; } kwcc_js_cstr_buf_t;

/* Forward declaration for self-referencing ops */
typedef struct kwcc_js_ops kwcc_js_ops_t;

/* JS operation interface — sub-modules operate through this, never touch mquickjs */
struct kwcc_js_ops {
    /* ── Properties ── */
    void           *ctx;          /* Opaque JSContext, sub-modules never touch */
    kwcc_js_val_t  undefined;
    kwcc_js_val_t  null;
    kwcc_js_val_t  exception;
    kwcc_js_val_t  global_obj;

    /* ── Value creation ── */
    kwcc_js_val_t  (*new_object)(kwcc_js_ops_t *ops);
    kwcc_js_val_t  (*new_int32)(kwcc_js_ops_t *ops, int32_t val);
    kwcc_js_val_t  (*new_string)(kwcc_js_ops_t *ops, const char *buf);
    kwcc_js_val_t  (*new_string_len)(kwcc_js_ops_t *ops, const char *buf, size_t len);

    /* ── Property access ── */
    void           (*set_str_prop)(kwcc_js_ops_t *ops, kwcc_js_val_t obj,
                                   const char *key, kwcc_js_val_t val);
    kwcc_js_val_t  (*get_str_prop)(kwcc_js_ops_t *ops, kwcc_js_val_t obj,
                                   const char *key);

    /* ── Function call ── */
    int            (*is_function)(kwcc_js_ops_t *ops, kwcc_js_val_t val);
    void           (*call_cb)(kwcc_js_ops_t *ops, kwcc_js_val_t cb,
                              int argc, kwcc_js_val_t *argv);

    /* ── C string conversion (two-step, caller provides buf) ── */
    const char *   (*to_cstring)(kwcc_js_ops_t *ops, kwcc_js_val_t val,
                                  kwcc_js_cstr_buf_t *buf);

    /* ── Type checks ── */
    int            (*is_undefined)(kwcc_js_val_t val);
    int            (*is_null)(kwcc_js_val_t val);
    int            (*is_exception)(kwcc_js_val_t val);

    /* ── Code execution ── */
    kwcc_js_val_t  (*eval)(kwcc_js_ops_t *ops, const char *code, size_t len,
                           const char *filename, int flags);

    /* ── Array operations ── */
    int            (*get_class_id)(kwcc_js_ops_t *ops, kwcc_js_val_t val);
    int            (*array_length)(kwcc_js_ops_t *ops, kwcc_js_val_t arr);
    kwcc_js_val_t  (*array_get)(kwcc_js_ops_t *ops, kwcc_js_val_t arr, uint32_t idx);

    /* ── Number conversion ── */
    int            (*to_int32)(kwcc_js_ops_t *ops, kwcc_js_val_t val);

    /* ── C→JS notification ($notify channel) ── */
    void           (*notify_js)(kwcc_js_ops_t *ops,
                                const char *type, const char *event,
                                const char *id, kwcc_js_val_t data,
                                void (*ack_cleanup)(const char *id));
};

/* Module descriptor — sub-modules implement this and register with core */
typedef struct kwcc_js_module {
    const char *name;
    void (*load)(kwcc_js_ops_t *ops);
    void (*register_cfun)(kwcc_js_ops_t *ops);
    void (*on_bus_event)(const char *topic, const void *data,
                         size_t len, kwcc_js_ops_t *ops);
    void (*unload)(kwcc_js_ops_t *ops);
} kwcc_js_module_t;

void kwcc_js_ops_init(JSContext *ctx);
void kwcc_js_register_module(kwcc_js_ops_t *ops, kwcc_js_module_t *mod);
void kwcc_js_register_modules(kwcc_js_ops_t *ops);

/* Expose global ops for testing (read-only access) */
extern kwcc_js_ops_t g_kwcc_js_ops;

/* ═══════════════════════════════════════════════════════════════
 * Existing API — preserved unchanged
 * ═══════════════════════════════════════════════════════════════ */

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
