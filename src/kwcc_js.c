/* kwcc_js.c — kwcc JS lifecycle + runtime support (stdlib stubs + $config JS API) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mquickjs/mquickjs.h"
#include "mquickjs/mquickjs_priv.h"
#include "kwcc_js.h"
#include "kwcc_mempool.h"
#include "kwcc_config.h"
#include "kwcc_base.h"
#include "kwcc_bus.h"
#include "kwcc_http.h"
#include "picohttpparser/picohttpparser.h"
#include "llog.h"
#include "mquickjs/mqjs_stdlib.h"

#define KWCC_MEM_SIZE (4 * 1024 * 1024)

/* ═══════════════════════════════════════════════════════════════
 * Facade ops — implementation of kwcc_js_ops_t function pointers
 * ═══════════════════════════════════════════════════════════════ */

kwcc_js_ops_t g_kwcc_js_ops;

/* Module registry — dynamic, no limit */
static kwcc_js_module_t **g_kwcc_js_modules = NULL;
static int g_kwcc_js_module_count = 0;
static int g_kwcc_js_module_cap = 0;

/* API dispatch table: module-grouped, two-level lookup */
typedef struct {
    kwcc_js_dispatch_t *handlers;   /* handler list for one module */
    int handler_count;
    int handler_cap;
} kwcc_js_dispatch_group_t;

static const char **g_kwcc_js_dispatch_modules = NULL;
static kwcc_js_dispatch_group_t *g_kwcc_js_dispatch_groups = NULL;
static int g_kwcc_js_dispatch_group_count = 0;
static int g_kwcc_js_dispatch_group_cap = 0;

/* Forward declarations for dispatch functions (defined later) */
void kwcc_js_dispatch_add(const char *module, const char *func, kwcc_js_handler_t handler);

/* Forward declaration for core API list (defined later) */
static const kwcc_js_api_t g_kwcc_js_core_apis[];

/* $notify.emit reference (global-reachable, GC-safe, no AddGCRef needed) */
static kwcc_js_val_t s_notify_emit_fn;

/* ── ops impl: value creation ── */

static kwcc_js_val_t kwcc_js_new_object_impl(kwcc_js_ops_t *ops) {
    return JS_NewObject((JSContext *)ops->ctx);
}

static kwcc_js_val_t kwcc_js_new_int32_impl(kwcc_js_ops_t *ops, int32_t val) {
    return JS_NewInt32((JSContext *)ops->ctx, val);
}

static kwcc_js_val_t kwcc_js_new_string_impl(kwcc_js_ops_t *ops, const char *buf) {
    return JS_NewString((JSContext *)ops->ctx, buf);
}

static kwcc_js_val_t kwcc_js_new_string_len_impl(kwcc_js_ops_t *ops, const char *buf, size_t len) {
    return JS_NewStringLen((JSContext *)ops->ctx, buf, len);
}

/* ── ops impl: property access ── */

static void kwcc_js_set_str_prop_impl(kwcc_js_ops_t *ops, kwcc_js_val_t obj,
                                       const char *key, kwcc_js_val_t val) {
    JSContext *ctx = (JSContext *)ops->ctx;
    JS_SetPropertyStr(ctx, obj, key, val);
}

static kwcc_js_val_t kwcc_js_get_str_prop_impl(kwcc_js_ops_t *ops, kwcc_js_val_t obj,
                                                 const char *key) {
    JSContext *ctx = (JSContext *)ops->ctx;
    return JS_GetPropertyStr(ctx, obj, key);
}

/* ── ops impl: function call ── */

static int kwcc_js_is_function_impl(kwcc_js_ops_t *ops, kwcc_js_val_t val) {
    return JS_IsFunction((JSContext *)ops->ctx, val);
}

static void kwcc_js_call_cb_impl(kwcc_js_ops_t *ops, kwcc_js_val_t cb,
                                  int argc, kwcc_js_val_t *argv) {
    JSContext *ctx = (JSContext *)ops->ctx;
    JSValue global = JS_GetGlobalObject(ctx);

    /* JS_StackCheck(total_push_count): argv[n-1]..argv[0], func, this_obj */
    JS_StackCheck(ctx, (uint32_t)(argc + 2));
    for (int i = argc - 1; i >= 0; i--) {
        JS_PushArg(ctx, argv[i]);
    }
    JS_PushArg(ctx, cb);
    JS_PushArg(ctx, global);
    JSValue ret = JS_Call(ctx, argc);

    /* Exception handling: log and clear, prevent accumulation */
    if (JS_IsException(ret)) {
        JSValue exc = JS_GetException(ctx);
        JSCStringBuf ebuf;
        const char *s = JS_ToCString(ctx, exc, &ebuf);
        log_warn("js: call_cb exception: %s", s ? s : "(none)");
    }
}

/* ── ops impl: C string conversion ── */

static const char *kwcc_js_to_cstring_impl(kwcc_js_ops_t *ops, kwcc_js_val_t val,
                                             kwcc_js_cstr_buf_t *buf) {
    JSCStringBuf mbuf;
    const char *s = JS_ToCString((JSContext *)ops->ctx, val, &mbuf);
    /* Copy inline buf if short string (pointer points into mbuf) */
    if (s && s >= (const char *)&mbuf && s < (const char *)&mbuf + sizeof(mbuf)) {
        memcpy(buf->buf, s, 5);
        return buf->buf;
    }
    return s;
}

/* ── ops impl: type checks ── */

static int kwcc_js_is_undefined_impl(kwcc_js_val_t val) {
    return JS_IsUndefined(val);
}

static int kwcc_js_is_null_impl(kwcc_js_val_t val) {
    return JS_IsNull(val);
}

static int kwcc_js_is_exception_impl(kwcc_js_val_t val) {
    return JS_IsException(val);
}

/* ── ops impl: code execution ── */

static kwcc_js_val_t kwcc_js_eval_impl(kwcc_js_ops_t *ops, const char *code, size_t len,
                                         const char *filename, int flags) {
    return JS_Eval((JSContext *)ops->ctx, code, len, filename, flags);
}

/* ── ops impl: array operations ── */

static int kwcc_js_get_class_id_impl(kwcc_js_ops_t *ops, kwcc_js_val_t val) {
    return JS_GetClassID((JSContext *)ops->ctx, val);
}

static int kwcc_js_array_length_impl(kwcc_js_ops_t *ops, kwcc_js_val_t arr) {
    JSContext *ctx = (JSContext *)ops->ctx;
    JSValue len_val = JS_GetPropertyStr(ctx, arr, "length");
    int len = 0;
    JS_ToInt32(ctx, &len, len_val);
    return len;
}

static kwcc_js_val_t kwcc_js_array_get_impl(kwcc_js_ops_t *ops, kwcc_js_val_t arr, uint32_t idx) {
    return JS_GetPropertyUint32((JSContext *)ops->ctx, arr, idx);
}

/* ── ops impl: number conversion ── */

static int kwcc_js_to_int32_impl(kwcc_js_ops_t *ops, kwcc_js_val_t val) {
    int result = 0;
    JS_ToInt32((JSContext *)ops->ctx, &result, val);
    return result;
}

/* ── ops impl: C→JS notification ($notify channel) ── */

static void kwcc_js_notify_js_impl(kwcc_js_ops_t *ops,
                                    const char *type, const char *event,
                                    const char *id, kwcc_js_val_t data,
                                    void (*ack_cleanup)(const char *id)) {
    /* ① Release C-side resources first (data already copied into GC heap) */
    if (ack_cleanup) ack_cleanup(id);

    /* ② Deliver to JS via $notify.emit */
    kwcc_js_val_t args[4];
    args[0] = ops->new_string(ops, type);
    args[1] = ops->new_string(ops, event);
    args[2] = id ? ops->new_string(ops, id) : ops->null;
    args[3] = data;
    ops->call_cb(ops, s_notify_emit_fn, 4, args);
}

/* ── ops initialization ── */

void kwcc_js_ops_init(JSContext *ctx) {
    g_kwcc_js_ops.ctx = ctx;
    g_kwcc_js_ops.undefined  = JS_UNDEFINED;
    g_kwcc_js_ops.null       = JS_NULL;
    g_kwcc_js_ops.exception  = JS_EXCEPTION;
    g_kwcc_js_ops.global_obj = JS_GetGlobalObject(ctx);

    g_kwcc_js_ops.new_object     = kwcc_js_new_object_impl;
    g_kwcc_js_ops.new_int32      = kwcc_js_new_int32_impl;
    g_kwcc_js_ops.new_string     = kwcc_js_new_string_impl;
    g_kwcc_js_ops.new_string_len = kwcc_js_new_string_len_impl;
    g_kwcc_js_ops.set_str_prop   = kwcc_js_set_str_prop_impl;
    g_kwcc_js_ops.get_str_prop   = kwcc_js_get_str_prop_impl;
    g_kwcc_js_ops.is_function    = kwcc_js_is_function_impl;
    g_kwcc_js_ops.call_cb        = kwcc_js_call_cb_impl;
    g_kwcc_js_ops.to_cstring     = kwcc_js_to_cstring_impl;
    g_kwcc_js_ops.is_undefined   = kwcc_js_is_undefined_impl;
    g_kwcc_js_ops.is_null        = kwcc_js_is_null_impl;
    g_kwcc_js_ops.is_exception   = kwcc_js_is_exception_impl;
    g_kwcc_js_ops.eval           = kwcc_js_eval_impl;
    g_kwcc_js_ops.get_class_id   = kwcc_js_get_class_id_impl;
    g_kwcc_js_ops.array_length   = kwcc_js_array_length_impl;
    g_kwcc_js_ops.array_get      = kwcc_js_array_get_impl;
    g_kwcc_js_ops.to_int32       = kwcc_js_to_int32_impl;
    g_kwcc_js_ops.notify_js      = kwcc_js_notify_js_impl;
}

/* ── $notify injection ── */

static void kwcc_js_inject_notify(kwcc_js_ops_t *ops) {
    /* Create $notify object via C API (consistent with $config style) */
    kwcc_js_val_t notify_obj = ops->new_object(ops);
    ops->set_str_prop(ops, ops->global_obj, "$notify", notify_obj);

    const char *code =
        "$notify.registry = {};\n"
        "$notify.on = function(type, handler) {\n"
        "    $notify.registry[type] = handler;\n"
        "};\n"
        "$notify.emit = function(type, event, id, data) {\n"
        "    var handler = $notify.registry[type];\n"
        "    if (handler) handler(event, id, data);\n"
        "};\n";
    ops->eval(ops, code, strlen(code), "<$notify>", JS_EVAL_REPL);

    /* Cache $notify.emit reference (global-reachable, GC-safe) */
    s_notify_emit_fn = ops->get_str_prop(ops, notify_obj, "emit");
}

/* ── Module registration ── */

void kwcc_js_register_module(kwcc_js_ops_t *ops, kwcc_js_module_t *mod) {
    log_info("js: loading module '%s'", mod->name);
    if (mod->load) mod->load(ops);

    /* Read module's declared API list and register into dispatch table */
    if (mod->apis) {
        for (int i = 0; mod->apis[i].name; i++) {
            kwcc_js_dispatch_add(mod->name, mod->apis[i].name, mod->apis[i].func);
        }
    }

    if (g_kwcc_js_module_count >= g_kwcc_js_module_cap) {
        int new_cap = g_kwcc_js_module_cap ? g_kwcc_js_module_cap * 2 : 16;
        g_kwcc_js_modules = realloc(g_kwcc_js_modules, new_cap * sizeof(kwcc_js_module_t *));
        if (!g_kwcc_js_modules) {
            log_error("js: module registry realloc failed");
            return;
        }
        g_kwcc_js_module_cap = new_cap;
    }
    g_kwcc_js_modules[g_kwcc_js_module_count++] = mod;
    log_info("js: module '%s' registered", mod->name);
}

void kwcc_js_register_modules(kwcc_js_ops_t *ops) {
    /* Inject $notify first — modules need $notify.on in their load */
    log_info("js: injecting $notify channel");
    kwcc_js_inject_notify(ops);

    /* Register core APIs (virtual module name "core") */
    for (int i = 0; g_kwcc_js_core_apis[i].name; i++) {
        kwcc_js_dispatch_add("core", g_kwcc_js_core_apis[i].name, g_kwcc_js_core_apis[i].func);
    }

    /* Register business modules — currently only http; add more here */
    /* extern kwcc_js_module_t kwcc_js_http_module; */
    /* kwcc_js_register_module(ops, &kwcc_js_http_module); */

    log_info("js: %d module(s) loaded", g_kwcc_js_module_count);
}

/* ── Bus consumer: forward C bus events to JS $bus ── */

/* 白名单匹配（逗号分隔的前缀列表）*/
static int kwcc_js_match_whitelist(const char *whitelist, const char *topic) {
    char buf[2048];
    strncpy(buf, whitelist, sizeof(buf) - 1);
    char *tok = strtok(buf, ",");
    while (tok) {
        size_t len = strlen(tok);
        if (len > 0 && tok[len - 1] == '/') {
            if (strncmp(topic, tok, len) == 0) return 1;
        } else if (strcmp(topic, tok) == 0) {
            return 1;
        }
        tok = strtok(NULL, ",");
    }
    return 0;
}

static void kwcc_js_on_bus_event(const char *topic, const void *data, size_t len, void *user_data) {
    (void)user_data;

    /* Dispatch to registered modules' on_bus_event */
    for (int i = 0; i < g_kwcc_js_module_count; i++) {
        if (g_kwcc_js_modules[i]->on_bus_event) {
            g_kwcc_js_modules[i]->on_bus_event(topic, data, len, &g_kwcc_js_ops);
        }
    }

    /* ── Default: forward to JS $bus ── */
    const char *whitelist = kwcc_config_get_core("bus/js_whitelist", "*");
    if (whitelist[0] == '*' && whitelist[1] == '\0') {
        /* * = 全部转发 */
    } else if (whitelist[0] == '\0') {
        return;  /* 空 = 不转发 */
    } else if (!kwcc_js_match_whitelist(whitelist, topic)) {
        return;  /* 不在白名单内 */
    }

    char buf[512];
    char safe[256];
    kwcc_base_topic_sanitize(safe, sizeof(safe), topic);
    snprintf(buf, sizeof(buf), "$bus.emit('%s', 'notify_c', new Object());", safe);
    JS_Eval((JSContext *)g_kwcc_js_ops.ctx, buf, strlen(buf), "<bus>", JS_EVAL_REPL);
}

/* ── JS lifecycle ───────────────────────────────────────────── */

JSContext *kwcc_create_js(void) {
    void *mem_buf = malloc(KWCC_MEM_SIZE);
    JSContext *ctx = JS_NewContext(mem_buf, KWCC_MEM_SIZE, &js_stdlib);
    if (!ctx) {
        log_fatal("kwcc: JS_NewContext failed (not enough memory?)");
        return NULL;
    }
    kwcc_register_config_js(ctx);
    kwcc_register_http_js(ctx);

    /* Initialize ops + inject $notify + register modules */
    kwcc_js_ops_init(ctx);
    kwcc_js_register_modules(&g_kwcc_js_ops);

    kwcc_bus_subscribe(KWCC_BUS_WILDCARD, kwcc_js_on_bus_event, ctx);
    return ctx;
}

void kwcc_destroy_js(JSContext *ctx) {
    if (ctx) {
        JS_FreeContext(ctx);
    }
}

/* ── js_print: print all arguments to stdout and log ─────────── */
JSValue js_print(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    char buf[1024];
    int pos = 0;
    for (int i = 0; i < argc; i++) {
        JSCStringBuf cbuf;
        const char *s = JS_ToCString(ctx, argv[i], &cbuf);
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s%s", i > 0 ? " " : "", s ? s : "(null)");
    }
    fprintf(stdout, "%s\n", buf);
    log_info("%s", buf);
    return JS_UNDEFINED;
}

/* ── js_gc ───────────────────────────────────────────────────── */
JSValue js_gc(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    JS_GC(ctx);
    return JS_UNDEFINED;
}

/* ── js_load: read and eval a JS file ────────────────────────── */
JSValue js_load(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;

    JSCStringBuf path_buf;
    const char *path = JS_ToCString(ctx, argv[0], &path_buf);
    if (!path) return JS_UNDEFINED;

    FILE *f = fopen(path, "rb");
    if (!f) {
        log_error("js_load: cannot open %s", path);
        return JS_UNDEFINED;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return JS_UNDEFINED; }

    size_t n = fread(buf, 1, sz, f);
    buf[n] = '\0';
    fclose(f);

    JSValue result = JS_Eval(ctx, buf, n, path, 0);
    free(buf);
    return result;
}

/* ── js_setTimeout: stub ─────────────────────────────────────── */
JSValue js_setTimeout(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_UNDEFINED;
}

/* ── js_clearTimeout: stub ───────────────────────────────────── */
JSValue js_clearTimeout(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_UNDEFINED;
}

/* ── js_date_now ─────────────────────────────────────────────── */
JSValue js_date_now(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_NewInt64(ctx, 0);
}

/* ── js_performance_now ──────────────────────────────────────── */
JSValue js_performance_now(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_NewInt64(ctx, 0);
}

/* ── kwcc UI bridge ──────────────────────────────────────────── */

typedef JSValue (*JSUICallback)(JSContext *ctx, const char *method, int argc, JSValue *argv);
JSUICallback g_ui_callback = NULL;

void kwcc_set_ui_callback(JSUICallback cb) {
    g_ui_callback = cb;
}

JSValue js_kwcc_ui(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (!g_ui_callback || argc < 1)
        return JS_UNDEFINED;
    JSCStringBuf buf;
    const char *method = JS_ToCString(ctx, argv[0], &buf);
    JSValue result = g_ui_callback(ctx, method, argc - 1, argv + 1);
    return result;
}

/* ═══════════════════════════════════════════════════════════════
 * $config JS API — Phase 4
 * C handler layer: JSValue → C value conversion → delegate to config layer
 * ═══════════════════════════════════════════════════════════════ */

/* ── Dispatch table functions ── */

/* Find module group index by name, returns -1 if not found */
static int kwcc_js_dispatch_find_module(const char *module) {
    for (int i = 0; i < g_kwcc_js_dispatch_group_count; i++) {
        if (strcmp(g_kwcc_js_dispatch_modules[i], module) == 0)
            return i;
    }
    return -1;
}

void kwcc_js_dispatch_add(const char *module, const char *func, kwcc_js_handler_t handler) {
    int mi = kwcc_js_dispatch_find_module(module);

    /* Module group not found — create new one */
    if (mi < 0) {
        if (g_kwcc_js_dispatch_group_count >= g_kwcc_js_dispatch_group_cap) {
            int new_cap = g_kwcc_js_dispatch_group_cap ? g_kwcc_js_dispatch_group_cap * 2 : 16;
            g_kwcc_js_dispatch_modules = realloc(g_kwcc_js_dispatch_modules, new_cap * sizeof(char *));
            g_kwcc_js_dispatch_groups = realloc(g_kwcc_js_dispatch_groups, new_cap * sizeof(kwcc_js_dispatch_group_t));
            if (!g_kwcc_js_dispatch_modules || !g_kwcc_js_dispatch_groups) {
                log_error("js: dispatch group realloc failed");
                return;
            }
            g_kwcc_js_dispatch_group_cap = new_cap;
        }
        mi = g_kwcc_js_dispatch_group_count++;
        g_kwcc_js_dispatch_modules[mi] = module;
        g_kwcc_js_dispatch_groups[mi].handlers = NULL;
        g_kwcc_js_dispatch_groups[mi].handler_count = 0;
        g_kwcc_js_dispatch_groups[mi].handler_cap = 0;
    }

    kwcc_js_dispatch_group_t *grp = &g_kwcc_js_dispatch_groups[mi];

    /* Check for duplicate func name — overwrite if found */
    for (int i = 0; i < grp->handler_count; i++) {
        if (strcmp(grp->handlers[i].func, func) == 0) {
            grp->handlers[i].handler = handler;
            return;
        }
    }

    /* New handler — append */
    if (grp->handler_count >= grp->handler_cap) {
        int new_cap = grp->handler_cap ? grp->handler_cap * 2 : 8;
        grp->handlers = realloc(grp->handlers, new_cap * sizeof(kwcc_js_dispatch_t));
        if (!grp->handlers) {
            log_error("js: dispatch handlers realloc failed");
            return;
        }
        grp->handler_cap = new_cap;
    }
    grp->handlers[grp->handler_count].module = module;
    grp->handlers[grp->handler_count].func = func;
    grp->handlers[grp->handler_count].handler = handler;
    grp->handler_count++;
}

kwcc_js_val_t kwcc_js_dispatch_call(const char *module, const char *func,
                                     int argc, kwcc_js_val_t *argv) {
    int mi = kwcc_js_dispatch_find_module(module);
    if (mi < 0) {
        log_error("js: dispatch: unknown module '%s'", module);
        return g_kwcc_js_ops.undefined;
    }

    kwcc_js_dispatch_group_t *grp = &g_kwcc_js_dispatch_groups[mi];
    for (int i = 0; i < grp->handler_count; i++) {
        if (strcmp(grp->handlers[i].func, func) == 0) {
            return grp->handlers[i].handler(&g_kwcc_js_ops, argc, argv);
        }
    }
    log_error("js: dispatch: unknown handler '%s/%s'", module, func);
    return g_kwcc_js_ops.undefined;
}

/* ── Core handlers (ops-signature, direct C calls) ── */

static kwcc_js_val_t js_core_mempool_dump_stats(kwcc_js_ops_t *ops, int argc, kwcc_js_val_t *argv) {
    (void)ops; (void)argc; (void)argv;
#ifdef KWCC_DEBUG
    kwcc_mempool_dump_stats();
#else
    log_info("dump: not available (build without KWCC_DEBUG)");
#endif
    return ops->undefined;
}

static kwcc_js_val_t js_core_mempool_dump_all(kwcc_js_ops_t *ops, int argc, kwcc_js_val_t *argv) {
    if (argc < 1) return ops->undefined;
#ifdef KWCC_DEBUG
    kwcc_js_cstr_buf_t pbuf;
    const char *path = ops->to_cstring(ops, argv[0], &pbuf);
    if (!path) return ops->undefined;
    int show_content = 0;
    if (argc >= 2) show_content = ops->to_int32(ops, argv[1]);
    kwcc_mempool_dump_all(path, show_content);
#else
    log_info("dumpAll: not available (build without KWCC_DEBUG)");
#endif
    return ops->undefined;
}

static const kwcc_js_api_t g_kwcc_js_core_apis[] = {
    { "mempool_dump_stats", js_core_mempool_dump_stats },
    { "mempool_dump_all",   js_core_mempool_dump_all },
    { NULL, NULL }
};

/* ── New global function: kwcc_js_call_c(module, func, ...args) ── */

JSValue kwcc_js_call_c(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_UNDEFINED;

    /* Extract module and func using ops-style (consistent with rest of code) */
    kwcc_js_cstr_buf_t mbuf, fbuf;
    const char *module = g_kwcc_js_ops.to_cstring(&g_kwcc_js_ops, argv[0], &mbuf);
    const char *func   = g_kwcc_js_ops.to_cstring(&g_kwcc_js_ops, argv[1], &fbuf);
    if (!module || !func) return JS_UNDEFINED;

    /* Args start from argv[2] */
    return kwcc_js_dispatch_call(module, func, argc - 2, argv + 2);
}

/* ── TLV conversion helper ──────────────────────────────────── */

typedef struct {
    JSContext *ctx;
    JSValue    js_obj;
    JSValue    js_keys;
    int        key_count;
    int        key_index;
} kwcc_js_tlv_pack_state_t;

static int kwcc_js_tlv_pack_cb(const char **out_key, const char **out_value,
                                uint8_t *out_type, size_t *out_vlen, void *user_data) {
    kwcc_js_tlv_pack_state_t *st = (kwcc_js_tlv_pack_state_t *)user_data;
    if (st->key_index >= st->key_count) return 0;

    JSValue js_key = JS_GetPropertyUint32(st->ctx, st->js_keys, (uint32_t)st->key_index);
    JSCStringBuf kbuf;
    const char *key = JS_ToCString(st->ctx, js_key, &kbuf);

    JSValue js_val = JS_GetPropertyStr(st->ctx, st->js_obj, key);

    if (JS_GetClassID(st->ctx, js_val) == JS_CLASS_OBJECT) {
        size_t sub_len = 0;
        uint8_t *sub_tlv = kwcc_js_value_to_tlv(st->ctx, js_val, &sub_len);
        if (sub_tlv) {
            *out_key = key;
            *out_value = (const char *)sub_tlv;
            *out_type = KWCC_MEMPOOL_TLV_OBJECT;
            *out_vlen = sub_len;
            st->key_index++;
            return 1;
        }
    } else {
        JSCStringBuf vbuf;
        const char *val = JS_ToCString(st->ctx, js_val, &vbuf);
        if (val) {
            *out_key = key;
            *out_value = val;
            *out_type = KWCC_MEMPOOL_TLV_FIELD;
            *out_vlen = strlen(val);
            st->key_index++;
            return 1;
        }
    }

    st->key_index++;
    return 1;
}

uint8_t *kwcc_js_value_to_tlv(JSContext *ctx, JSValue js_val, size_t *out_len) {
    /* Use js_object_keys internal API directly */
    JSValue args[1];
    args[0] = js_val;
    JSValue keys_result = js_object_keys(ctx, &js_val, 1, args);
    if (JS_IsException(keys_result)) { *out_len = 0; return NULL; }

    JSValue js_len = JS_GetPropertyStr(ctx, keys_result, "length");
    int key_count = 0;
    JS_ToInt32(ctx, &key_count, js_len);

    kwcc_js_tlv_pack_state_t st;
    st.ctx = ctx;
    st.js_obj = js_val;
    st.js_keys = keys_result;
    st.key_count = key_count;
    st.key_index = 0;

    uint8_t *tlv = kwcc_mempool_tlv_build(kwcc_js_tlv_pack_cb, &st, out_len);
    return tlv;
}

/* ── C handlers: App domain ─────────────────────────────────── */

JSValue kwcc_js_config_set_app_int(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_UNDEFINED;
    JSCStringBuf kbuf;
    const char *key = JS_ToCString(ctx, argv[0], &kbuf);
    if (!key) return JS_UNDEFINED;
    int32_t val = 0;
    JS_ToInt32(ctx, &val, argv[1]);
    kwcc_config_set_app_int(key, val);
    return JS_UNDEFINED;
}

JSValue kwcc_js_config_set_app_string(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_UNDEFINED;
    JSCStringBuf kbuf, vbuf;
    const char *key = JS_ToCString(ctx, argv[0], &kbuf);
    if (!key) return JS_UNDEFINED;
    const char *val = JS_ToCString(ctx, argv[1], &vbuf);
    if (!val) return JS_UNDEFINED;
    kwcc_config_set_app_string(key, val);
    return JS_UNDEFINED;
}

JSValue kwcc_js_config_set_app_bool(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_UNDEFINED;
    JSCStringBuf kbuf;
    const char *key = JS_ToCString(ctx, argv[0], &kbuf);
    if (!key) return JS_UNDEFINED;
    int val = (argv[1] == JS_TRUE) ? 1 : 0;
    kwcc_config_set_app_bool(key, val);
    return JS_UNDEFINED;
}

JSValue kwcc_js_config_set_app_json(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_UNDEFINED;
    JSCStringBuf kbuf, vbuf;
    const char *key = JS_ToCString(ctx, argv[0], &kbuf);
    if (!key) return JS_UNDEFINED;
    const char *val = JS_ToCString(ctx, argv[1], &vbuf);
    if (!val) return JS_UNDEFINED;
    kwcc_config_set_app_json(key, val);
    return JS_UNDEFINED;
}

JSValue kwcc_js_config_set_app_tlv(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_UNDEFINED;
    JSCStringBuf kbuf;
    const char *key = JS_ToCString(ctx, argv[0], &kbuf);
    if (!key) return JS_UNDEFINED;
    size_t tlv_len = 0;
    uint8_t *tlv = kwcc_js_value_to_tlv(ctx, argv[1], &tlv_len);
    if (tlv) {
        kwcc_config_set_app_tlv(key, tlv, (uint32_t)tlv_len);
        free(tlv);
    }
    return JS_UNDEFINED;
}

JSValue kwcc_js_config_get_app(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    JSCStringBuf kbuf;
    const char *key = JS_ToCString(ctx, argv[0], &kbuf);
    if (!key) return JS_UNDEFINED;
    const char *default_val = NULL;
    if (argc >= 2) {
        JSCStringBuf dbuf;
        default_val = JS_ToCString(ctx, argv[1], &dbuf);
    }
    const char *val = kwcc_config_get_app(key, default_val);
    if (!val) return JS_UNDEFINED;

    /* Check slot type — if JSON, parse and return object */
    kwcc_mempool_slot_t *slot = kwcc_config_get_app_slot(key);
    if (slot && slot->type == KWCC_MEMPOOL_TYPE_JSON) {
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue json_obj = JS_GetPropertyStr(ctx, global, "JSON");
        JSValue parse_fn = JS_GetPropertyStr(ctx, json_obj, "parse");
        JSValue json_str = JS_NewString(ctx, val);
        JS_StackCheck(ctx, 3);
        JS_PushArg(ctx, json_str);
        JS_PushArg(ctx, parse_fn);
        JS_PushArg(ctx, json_obj);
        return JS_Call(ctx, 1);
    }

    return JS_NewString(ctx, val);
}

JSValue kwcc_js_config_release_app(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    JSCStringBuf kbuf;
    const char *key = JS_ToCString(ctx, argv[0], &kbuf);
    if (!key) return JS_UNDEFINED;
    kwcc_config_release_app(key);
    return JS_UNDEFINED;
}

JSValue kwcc_js_config_release_app_prefix(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    JSCStringBuf kbuf;
    const char *key = JS_ToCString(ctx, argv[0], &kbuf);
    if (!key) return JS_UNDEFINED;
    kwcc_config_release_app_prefix(key);
    return JS_UNDEFINED;
}

/* ── C handlers: Core domain ────────────────────────────────── */

JSValue kwcc_js_config_set_core_tlv(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_UNDEFINED;
    JSCStringBuf kbuf;
    const char *key = JS_ToCString(ctx, argv[0], &kbuf);
    if (!key) return JS_UNDEFINED;
    size_t tlv_len = 0;
    uint8_t *tlv = kwcc_js_value_to_tlv(ctx, argv[1], &tlv_len);
    if (tlv) {
        kwcc_config_set_core_tlv(key, tlv, (uint32_t)tlv_len);
        free(tlv);
    }
    return JS_UNDEFINED;
}

/* ── C handlers: TLV getters ────────────────────────────────── */

JSValue kwcc_js_config_get_app_tlv_path(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_UNDEFINED;
    JSCStringBuf kbuf, pbuf;
    const char *key = JS_ToCString(ctx, argv[0], &kbuf);
    if (!key) return JS_UNDEFINED;
    const char *path = JS_ToCString(ctx, argv[1], &pbuf);
    if (!path) return JS_UNDEFINED;
    kwcc_mempool_slot_t *slot = kwcc_config_get_app_slot(key);
    if (!slot || !slot->data || slot->type != KWCC_MEMPOOL_TYPE_TLV) {
        if (argc >= 3) return argv[2];
        return JS_UNDEFINED;
    }
    size_t vlen = 0;
    const char *val = kwcc_mempool_tlv_get_path(slot->data, slot->size, path, &vlen);
    if (!val) {
        if (argc >= 3) return argv[2];
        return JS_UNDEFINED;
    }
    return JS_NewStringLen(ctx, val, vlen);
}

JSValue kwcc_js_config_get_app_tlv_json(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    JSCStringBuf kbuf;
    const char *key = JS_ToCString(ctx, argv[0], &kbuf);
    if (!key) return JS_UNDEFINED;
    kwcc_mempool_slot_t *slot = kwcc_config_get_app_slot(key);
    if (!slot || !slot->data || slot->type != KWCC_MEMPOOL_TYPE_TLV) {
        if (argc >= 2) return argv[1];
        return JS_UNDEFINED;
    }
    size_t json_len = 0;
    char *json = kwcc_mempool_tlv_to_json(slot->data, slot->size, &json_len);
    if (json) {
        JSValue result = JS_NewStringLen(ctx, json, json_len);
        kwcc_mempool_tlv_free_json(json);
        return result;
    }
    if (argc >= 2) return argv[1];
    return JS_UNDEFINED;
}

/* ── C handler: Pool management ─────────────────────────────── */

JSValue kwcc_js_config_set_max_pools(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_UNDEFINED;
    JSCStringBuf tbuf;
    const char *type_str = JS_ToCString(ctx, argv[0], &tbuf);
    if (!type_str) return JS_UNDEFINED;
    int max = 0;
    JS_ToInt32(ctx, &max, argv[1]);
    if (strcmp(type_str, "*") == 0) {
        for (int i = 0; i < KWCC_MEMPOOL_MAX_TYPES; i++) {
            kwcc_config_set_max_pools(i, max);
        }
    } else if (strncmp(type_str, "l", 1) == 0 && strlen(type_str) > 1) {
        int t = atoi(type_str + 1);
        if (t >= 0 && t < KWCC_MEMPOOL_MAX_TYPES) {
            kwcc_config_set_max_pools(t, max);
        }
    }
    return JS_UNDEFINED;
}

/* ── Register $config JS API ────────────────────────────────── */

void kwcc_register_config_js(JSContext *ctx) {
    JSValue global_obj = JS_GetGlobalObject(ctx);
    JSValue config = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, global_obj, "$config", config);

    const char *wrapper =
        "$config.appSetInt = function(k, v) { kwcc_js_config_set_app_int(k, v); };\n"
        "$config.appSetString = function(k, v) { kwcc_js_config_set_app_string(k, v); };\n"
        "$config.appSetBool = function(k, v) { kwcc_js_config_set_app_bool(k, v); };\n"
        "$config.appSetJson = function(k, v) { kwcc_js_config_set_app_json(k, JSON.stringify(v)); };\n"
        "$config.appSetJsonString = function(k, v) { kwcc_js_config_set_app_string(k, v); };\n"
        "$config.appSetTlv = function(k, v) { kwcc_js_config_set_app_tlv(k, v); };\n"
        "$config.appGet = function(k, d) { return kwcc_js_config_get_app(k, d); };\n"
        "$config.appGetTlv = function(k, p, d) {\n"
        "    if (p) { return kwcc_js_config_get_app_tlv_path(k, p, d); }\n"
        "    return kwcc_js_config_get_app_tlv_json(k, d);\n"
        "};\n"
        "$config.appRelease = function(k) { kwcc_js_config_release_app(k); };\n"
        "$config.appReleasePrefix = function(k) { kwcc_js_config_release_app_prefix(k); };\n"
        "$config.coreSetTlv = function(k, v) { kwcc_js_config_set_core_tlv(k, v); };\n"
        "$config.setMaxPools = function(t, n) { kwcc_js_config_set_max_pools(t, n); };\n"
        "$config.dump = function() { kwcc_js_call_c(\"core\", \"mempool_dump_stats\"); };\n"
        "$config.dumpAll = function(p, s) { kwcc_js_call_c(\"core\", \"mempool_dump_all\", p, s || 0); };\n";

    JSValue result = JS_Eval(ctx, wrapper, strlen(wrapper), "<$config>", JS_EVAL_REPL);
    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        JSCStringBuf buf;
        const char *s = JS_ToCString(ctx, exc, &buf);
        log_error("$config JS_Eval: %s", s ? s : "(none)");
    }
}

/* ═══════════════════════════════════════════════════════════════
 * $http JS API — HTTP C handlers + req_id callback registry
 * ═══════════════════════════════════════════════════════════════ */

/* ── C handler: kwcc_js_http_request ────────────────────────── */

JSValue kwcc_js_http_request(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_UNDEFINED;

    JSCStringBuf mbuf, ubuf;
    const char *method = JS_ToCString(ctx, argv[0], &mbuf);
    if (!method) method = "GET";
    const char *url = JS_ToCString(ctx, argv[1], &ubuf);
    if (!url) return JS_UNDEFINED;

    /* headers: argv[2] is an array of "Key: Value" strings */
    const char *headers[16];
    int header_count = 0;
    char *header_copies[16];  /* strdup'd copies to free later */
    memset(header_copies, 0, sizeof(header_copies));
    if (argc >= 3 && JS_GetClassID(ctx, argv[2]) == JS_CLASS_ARRAY) {
        JSValue arr_len = JS_GetPropertyStr(ctx, argv[2], "length");
        int len = 0;
        JS_ToInt32(ctx, &len, arr_len);
        for (int i = 0; i < len && i < 16; i++) {
            JSValue item = JS_GetPropertyUint32(ctx, argv[2], (uint32_t)i);
            JSCStringBuf ibuf;
            const char *s = JS_ToCString(ctx, item, &ibuf);
            if (s) {
                header_copies[header_count] = strdup(s);
                headers[header_count] = header_copies[header_count];
                header_count++;
            }
        }
    }

    /* body: argv[3] is a string */
    const char *body = NULL;
    int body_len = 0;
    JSCStringBuf bbuf;
    if (argc >= 4) {
        body = JS_ToCString(ctx, argv[3], &bbuf);
        if (body) body_len = (int)strlen(body);
    }

    const char *req_id = kwcc_http_request(method, url, headers, header_count, body, body ? body_len : 0);

    /* Free header copies */
    for (int i = 0; i < 16; i++) free(header_copies[i]);

    if (!req_id) return JS_UNDEFINED;

    /* Store resolve/reject callbacks via HTTP module API */
    if (argc >= 7) {
        kwcc_http_register_callback(req_id, &argv[4], &argv[5], &argv[6]);
    }

    return JS_NewString(ctx, req_id);
}

/* ── C handler: kwcc_js_http_cancel ─────────────────────────── */

JSValue kwcc_js_http_cancel(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)ctx; (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    JSCStringBuf rbuf;
    const char *req_id = JS_ToCString(ctx, argv[0], &rbuf);
    if (!req_id) return JS_UNDEFINED;
    kwcc_http_cancel(req_id);
    return JS_UNDEFINED;
}

/* ── Register $http JS API ─────────────────────────────────── */

void kwcc_register_http_js(JSContext *ctx) {
    const char *code =
        "var $http = new Object();\n"
        "$http.state = { activeRequests: 0 };\n"
        "$http.request = function(method, url, headers, body, resolve, reject, onProgress) {\n"
        "    return kwcc_js_call_c('http', 'request', method, url, headers, body, resolve, reject, onProgress);\n"
        "};\n"
        "$http.cancel = function(reqId) {\n"
        "    kwcc_js_call_c('http', 'cancel', reqId);\n"
        "};\n"
        "$http.config = function(key, value) {\n"
        "    $config.coreSetTlv('http/' + key, value);\n"
        "};\n";

    JSValue result = JS_Eval(ctx, code, strlen(code), "<$http>", JS_EVAL_REPL);
    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        JSCStringBuf buf;
        const char *s = JS_ToCString(ctx, exc, &buf);
        log_error("$http JS_Eval: %s", s ? s : "(none)");
    }
}
