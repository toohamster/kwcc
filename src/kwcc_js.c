/* kwcc_js.c — kwcc JS lifecycle + runtime support (stdlib stubs + JS bindings) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mquickjs/mquickjs.h"
#include "kwcc_js.h"
#include "kwcc_base.h"
#include "llog.h"
#include "mquickjs/mqjs_stdlib.h"

#define KWCC_MEM_SIZE (4 * 1024 * 1024)

/* Extern from kwcc.c */
void kwcc_config_set_jsctx(JSContext *ctx);

/* Forward declaration */
void kwcc_register_config_js(JSContext *ctx);

/* ── JS lifecycle ───────────────────────────────────────────── */

JSContext *kwcc_create_js(void) {
    void *mem_buf = malloc(KWCC_MEM_SIZE);
    JSContext *ctx = JS_NewContext(mem_buf, KWCC_MEM_SIZE, &js_stdlib);
    if (!ctx) {
        log_fatal("kwcc: JS_NewContext failed (not enough memory?)");
        return NULL;
    }
    kwcc_config_set_jsctx(ctx);
    kwcc_register_config_js(ctx);
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

    /* Get file size */
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

/* ── kwcc config set: C binding for kwcc_config_set ─────────── */

JSValue js_kwcc_config_set(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 3) return JS_UNDEFINED;

    JSCStringBuf mbuf, kbuf, vbuf;
    const char *module = JS_ToCString(ctx, argv[0], &mbuf);
    const char *key    = JS_ToCString(ctx, argv[1], &kbuf);
    const char *value  = JS_ToCString(ctx, argv[2], &vbuf);

    if (module && key && value) {
        kwcc_config_set(module, key, value);
    }
    return JS_UNDEFINED;
}

/* ── $config JS API ────────────────────────────────────────── */

#include "kwcc_pool.h"

/* C handler functions (registered in mqjs_stdlib.c via CONFIG_KWCC) */
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

static const char *js_pool_key_to_string(JSContext *ctx, JSValue val, JSCStringBuf *buf) {
    if (JS_IsNull(val) || JS_IsUndefined(val)) return NULL;
    return JS_ToCString(ctx, val, buf);
}

JSValue js_config_set_app(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_UNDEFINED;

    JSCStringBuf kbuf, vbuf;
    const char *key = js_pool_key_to_string(ctx, argv[0], &kbuf);
    if (!key) return JS_UNDEFINED;

    /* null value → release */
    if (JS_IsNull(argv[1])) {
        kwcc_slot_t *s = kwcc_pool_get(&g_app_pool, key);
        if (s) kwcc_pool_release(&g_app_pool, s);
        return JS_UNDEFINED;
    }

    const char *val = JS_ToCString(ctx, argv[1], &vbuf);
    if (!val) return JS_UNDEFINED;

    kwcc_slot_t *s = kwcc_pool_get(&g_app_pool, key);
    if (!s) {
        s = kwcc_pool_alloc(&g_app_pool, key, (uint32_t)strlen(val) + 1, 0);
        if (!s) {
            char warn_buf[256];
            snprintf(warn_buf, sizeof(warn_buf),
                     "console.warn('pool: App pool full, alloc failed key=%s');", key);
            JS_Eval(ctx, warn_buf, strlen(warn_buf), "<$config>", 0);
            return JS_UNDEFINED;
        }
        kwcc_pool_acquire(&g_app_pool, s);
    }
    kwcc_pool_set(&g_app_pool, s, val, (uint32_t)strlen(val) + 1);
    return JS_NewString(ctx, key);
}

JSValue js_config_set_user(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_UNDEFINED;

    JSCStringBuf kbuf, vbuf;
    const char *key = js_pool_key_to_string(ctx, argv[0], &kbuf);
    if (!key) return JS_UNDEFINED;

    /* null value → release */
    if (JS_IsNull(argv[1])) {
        kwcc_slot_t *s = kwcc_pool_get(&g_user_pool, key);
        if (s) kwcc_pool_release(&g_user_pool, s);
        return JS_UNDEFINED;
    }

    const char *val = JS_ToCString(ctx, argv[1], &vbuf);
    if (!val) return JS_UNDEFINED;

    uint32_t timeout = 0;
    if (argc > 2) JS_ToInt32(ctx, (int *)&timeout, argv[2]);

    kwcc_slot_t *s = kwcc_pool_get(&g_user_pool, key);
    if (!s) {
        s = kwcc_pool_alloc(&g_user_pool, key, (uint32_t)strlen(val) + 1, timeout);
        if (!s) {
            char warn_buf[256];
            snprintf(warn_buf, sizeof(warn_buf),
                     "console.warn('pool: User pool full, alloc failed key=%s');", key);
            JS_Eval(ctx, warn_buf, strlen(warn_buf), "<$config>", 0);
            return JS_UNDEFINED;
        }
        kwcc_pool_acquire(&g_user_pool, s);
    }
    kwcc_pool_set(&g_user_pool, s, val, (uint32_t)strlen(val) + 1);
    return JS_NewString(ctx, key);
}

JSValue js_config_get_app(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val; (void)argc;
    JSCStringBuf kbuf;
    const char *key = js_pool_key_to_string(ctx, argv[0], &kbuf);
    if (!key) return JS_UNDEFINED;

    kwcc_slot_t *s = kwcc_pool_get(&g_app_pool, key);
    if (!s || !s->data) return JS_UNDEFINED;
    return JS_NewStringLen(ctx, (const char *)s->data, s->size);
}

JSValue js_config_get_user(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val; (void)argc;
    JSCStringBuf kbuf;
    const char *key = js_pool_key_to_string(ctx, argv[0], &kbuf);
    if (!key) return JS_UNDEFINED;

    kwcc_slot_t *s = kwcc_pool_get(&g_user_pool, key);
    if (!s || !s->data) return JS_UNDEFINED;
    return JS_NewStringLen(ctx, (const char *)s->data, s->size);
}

JSValue js_config_release_app(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val; (void)argc;
    JSCStringBuf kbuf;
    const char *key = js_pool_key_to_string(ctx, argv[0], &kbuf);
    if (!key) return JS_UNDEFINED;
    kwcc_slot_t *s = kwcc_pool_get(&g_app_pool, key);
    if (s) kwcc_pool_release(&g_app_pool, s);
    return JS_UNDEFINED;
}

JSValue js_config_release_user(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val; (void)argc;
    JSCStringBuf kbuf;
    const char *key = js_pool_key_to_string(ctx, argv[0], &kbuf);
    if (!key) return JS_UNDEFINED;
    kwcc_slot_t *s = kwcc_pool_get(&g_user_pool, key);
    if (s) kwcc_pool_release(&g_user_pool, s);
    return JS_UNDEFINED;
}

JSValue js_config_set_app_size(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val; (void)argc;
    int sz = 0;
    if (argc > 0) JS_ToInt32(ctx, &sz, argv[0]);
    if (sz <= 0) return JS_UNDEFINED;
    kwcc_pool_configure(&g_app_pool, (size_t)sz);
    return JS_UNDEFINED;
}

JSValue js_config_set_user_size(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val; (void)argc;
    int sz = 0;
    if (argc > 0) JS_ToInt32(ctx, &sz, argv[0]);
    if (sz <= 0) return JS_UNDEFINED;
    kwcc_pool_configure(&g_user_pool, (size_t)sz);
    return JS_UNDEFINED;
}

#ifdef KWCC_DEBUG
JSValue js_config_dump(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    kwcc_mem_pool_t *pools[] = { &g_core_pool, &g_app_pool, &g_user_pool };
    for (int i = 0; i < 3; i++) {
        if (pools[i]->raw_memory) {
            kwcc_pool_dump_stats(pools[i]);
        }
    }
    return JS_UNDEFINED;
}

JSValue js_config_dump_all(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    JSCStringBuf fbuf;
    const char *filepath = JS_ToCString(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &fbuf);
    if (!filepath) return JS_UNDEFINED;
    int show_content = 0;
    if (argc > 1) {
        show_content = JS_ToInt32(ctx, (int *)&show_content, argv[1]) == 0 ? 0 : 1;
    }
    kwcc_mem_pool_t *pools[] = { &g_core_pool, &g_app_pool, &g_user_pool };
    for (int i = 0; i < 3; i++) {
        if (pools[i]->raw_memory) {
            kwcc_pool_dump_all(pools[i], filepath, show_content);
        }
    }
    return JS_UNDEFINED;
}
#endif

void kwcc_register_config_js(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue config = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, global, "$config", config);

    const char *config_js =
        "$config.setApp = function(k, v) { kwcc_config_set_app(k, v); };\n"
        "$config.setUser = function(k, v, t) { kwcc_config_set_user(k, v, t !== undefined ? t : 0); };\n"
        "$config.getApp = function(k) { return kwcc_config_get_app(k); };\n"
        "$config.getUser = function(k) { return kwcc_config_get_user(k); };\n"
        "$config.releaseApp = function(k) { kwcc_config_release_app(k); };\n"
        "$config.releaseUser = function(k) { kwcc_config_release_user(k); };\n"
        "$config.setAppSize = function(s) { kwcc_config_set_app_size(s); };\n"
        "$config.setUserSize = function(s) { kwcc_config_set_user_size(s); };\n";

#ifdef KWCC_DEBUG
    const char *config_js_debug =
        "$config.dump = function() { kwcc_config_dump(); };\n"
        "$config.dumpAll = function(f, c) { kwcc_config_dump_all(f, c); };\n";
    JSValue r2 = JS_Eval(ctx, config_js_debug, strlen(config_js_debug), "<$config_debug>", 0);
    if (JS_IsException(r2)) {
        JSValue exc = JS_GetException(ctx);
        JSCStringBuf buf;
        const char *s = JS_ToCString(ctx, exc, &buf);
        log_error("$config_debug JS_Eval: %s", s ? s : "(none)");
    }
#endif

    JSValue r = JS_Eval(ctx, config_js, strlen(config_js), "<$config>", 0);
    if (JS_IsException(r)) {
        JSValue exc = JS_GetException(ctx);
        JSCStringBuf buf;
        const char *s = JS_ToCString(ctx, exc, &buf);
        log_error("$config JS_Eval: %s", s ? s : "(none)");
    }
}
