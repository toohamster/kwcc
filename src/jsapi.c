/* jsapi.c — kwcc JS runtime support (stdlib stubs + UI bridge) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mquickjs/mquickjs.h"
#include "llog.h"

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
static JSUICallback g_ui_callback = NULL;

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
