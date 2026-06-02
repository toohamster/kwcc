/* kwcc.c — config JSValue storage (internal implementation) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mquickjs/mquickjs.h"
#include "llog.h"
#include "kwcc_base.h"

/* ── Config storage: stored as properties on a global __kwcc_config object ── */

static JSContext *g_js_ctx = NULL;

/* Internal: set JSContext (called from kwcc_create_js) */
void kwcc_config_set_jsctx(JSContext *ctx) {
    g_js_ctx = ctx;
    /* Create __kwcc_config object on global */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue config = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, global, "__kwcc_config", config);
}

/* Internal: store a JS object for a module */
void kwcc_config_set_object(const char *module, JSValue obj) {
    if (!module || !g_js_ctx) return;

    /* Store the object directly on __kwcc_config[module] */
    JSValue global = JS_GetGlobalObject(g_js_ctx);
    JSValue config = JS_GetPropertyStr(g_js_ctx, global, "__kwcc_config");
    if (JS_IsUndefined(config)) {
        config = JS_NewObject(g_js_ctx);
        JS_SetPropertyStr(g_js_ctx, global, "__kwcc_config", config);
    }
    JS_SetPropertyStr(g_js_ctx, config, module, obj);
}

/* C stub: string-based config set (called from js_kwcc_config_set) */
void kwcc_config_set(const char *module, const char *key, const char *value) {
    if (!module || !key || !value || !g_js_ctx) return;

    JSValue global = JS_GetGlobalObject(g_js_ctx);
    JSValue config = JS_GetPropertyStr(g_js_ctx, global, "__kwcc_config");
    if (JS_IsUndefined(config)) {
        config = JS_NewObject(g_js_ctx);
        JS_SetPropertyStr(g_js_ctx, global, "__kwcc_config", config);
    }

    JSValue mod = JS_GetPropertyStr(g_js_ctx, config, module);
    if (JS_IsUndefined(mod)) {
        mod = JS_NewObject(g_js_ctx);
        JS_SetPropertyStr(g_js_ctx, config, module, mod);
    }

    JSValue val = JS_NewString(g_js_ctx, value);
    JS_SetPropertyStr(g_js_ctx, mod, key, val);
}

/* Rotating buffer for string return */
#define CONFIG_STR_BUF_COUNT 4
static char g_config_str_buf[CONFIG_STR_BUF_COUNT][256];
static int  g_config_str_idx = 0;

const char *kwcc_config_get(const char *module, const char *key, const char *default_value) {
    if (!module || !key || !g_js_ctx) return default_value;

    JSValue global = JS_GetGlobalObject(g_js_ctx);
    JSValue config = JS_GetPropertyStr(g_js_ctx, global, "__kwcc_config");
    if (JS_IsUndefined(config)) return default_value;

    JSValue mod = JS_GetPropertyStr(g_js_ctx, config, module);
    if (JS_IsUndefined(mod)) return default_value;

    JSValue val = JS_GetPropertyStr(g_js_ctx, mod, key);
    if (JS_IsUndefined(val)) return default_value;

    JSCStringBuf buf;
    const char *s = JS_ToCString(g_js_ctx, val, &buf);
    if (!s) return default_value;

    /* Rotate buffer */
    int idx = g_config_str_idx++ % CONFIG_STR_BUF_COUNT;
    strncpy(g_config_str_buf[idx], s, 255);
    g_config_str_buf[idx][255] = '\0';
    return g_config_str_buf[idx];
}

int kwcc_config_get_int32(const char *module, const char *key, int default_value) {
    if (!module || !key || !g_js_ctx) return default_value;

    JSValue global = JS_GetGlobalObject(g_js_ctx);
    JSValue config = JS_GetPropertyStr(g_js_ctx, global, "__kwcc_config");
    if (JS_IsUndefined(config)) return default_value;

    JSValue mod = JS_GetPropertyStr(g_js_ctx, config, module);
    if (JS_IsUndefined(mod)) return default_value;

    JSValue val = JS_GetPropertyStr(g_js_ctx, mod, key);
    if (JS_IsUndefined(val)) return default_value;

    int32_t result;
    if (JS_ToInt32(g_js_ctx, &result, val) == 0) {
        return (int)result;
    }
    return default_value;
}
