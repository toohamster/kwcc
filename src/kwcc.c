/* kwcc.c — kwcc core (JS lifecycle + config storage) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mquickjs/mquickjs.h"
#include "microui/microui.h"
#include "llog.h"
#include "kwcc.h"
#include "kwcc_ui.h"
#include "kwcc_core.h"
#include "kwcc_js.h"
#include "mquickjs/mqjs_stdlib.h"

#define KWCC_MEM_SIZE (4 * 1024 * 1024)

/* Shared microui context */
mu_Context g_mu;

/* ── Config system ──────────────────────────────────────────── */

static kwcc_config_module_t g_config_modules[KWCC_CONFIG_MAX_MODULES];

void kwcc_config_set(const char *module, const char *key, const char *value) {
    if (!module || !key || !value) return;

    int mod_idx = -1;
    for (int i = 0; i < KWCC_CONFIG_MAX_MODULES; i++) {
        if (g_config_modules[i].in_use &&
            strcmp(g_config_modules[i].module, module) == 0) {
            mod_idx = i;
            break;
        }
    }

    if (mod_idx < 0) {
        for (int i = 0; i < KWCC_CONFIG_MAX_MODULES; i++) {
            if (!g_config_modules[i].in_use) {
                mod_idx = i;
                g_config_modules[i].in_use = 1;
                g_config_modules[i].entry_count = 0;
                strncpy(g_config_modules[i].module, module, KWCC_CONFIG_MAX_KEY_LEN - 1);
                g_config_modules[i].module[KWCC_CONFIG_MAX_KEY_LEN - 1] = '\0';
                break;
            }
        }
    }
    if (mod_idx < 0) return;

    kwcc_config_module_t *mod = &g_config_modules[mod_idx];

    for (int i = 0; i < mod->entry_count; i++) {
        if (strcmp(mod->entries[i].key, key) == 0) {
            strncpy(mod->entries[i].value, value, KWCC_CONFIG_MAX_VALUE_LEN - 1);
            mod->entries[i].value[KWCC_CONFIG_MAX_VALUE_LEN - 1] = '\0';
            return;
        }
    }

    if (mod->entry_count < 16) {
        int idx = mod->entry_count++;
        strncpy(mod->entries[idx].key, key, KWCC_CONFIG_MAX_KEY_LEN - 1);
        mod->entries[idx].key[KWCC_CONFIG_MAX_KEY_LEN - 1] = '\0';
        strncpy(mod->entries[idx].value, value, KWCC_CONFIG_MAX_VALUE_LEN - 1);
        mod->entries[idx].value[KWCC_CONFIG_MAX_VALUE_LEN - 1] = '\0';
    }
}

const char *kwcc_config_get(const char *module, const char *key, const char *default_value) {
    if (!module || !key) return default_value;

    for (int i = 0; i < KWCC_CONFIG_MAX_MODULES; i++) {
        if (g_config_modules[i].in_use &&
            strcmp(g_config_modules[i].module, module) == 0) {
            kwcc_config_module_t *mod = &g_config_modules[i];
            for (int j = 0; j < mod->entry_count; j++) {
                if (strcmp(mod->entries[j].key, key) == 0) {
                    return mod->entries[j].value;
                }
            }
        }
    }
    return default_value;
}

/* ── Public API ─────────────────────────────────────────────── */

void kwcc_init(void) {
    mu_init(&g_mu);
}

void kwcc_free(void) {
}

JSContext *kwcc_create_js(void) {
    void *mem_buf = malloc(KWCC_MEM_SIZE);
    JSContext *ctx = JS_NewContext(mem_buf, KWCC_MEM_SIZE, &js_stdlib);
    if (!ctx) {
        log_fatal("kwcc: JS_NewContext failed (not enough memory?)");
        return NULL;
    }
    return ctx;
}

void kwcc_destroy_js(JSContext *ctx) {
    if (ctx) {
        JS_FreeContext(ctx);
    }
}

void kwcc_process_js(JSContext *ctx, const char *js_text) {
    if (!js_text) return;
    g_frame_counter++;
    kwcc_begin_frame();

    mu_begin(&g_mu);

    JSValue result = JS_Eval(ctx, js_text, strlen(js_text), "<main.js>", 0);
    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        JSCStringBuf buf;
        const char *s = JS_ToCString(ctx, exc, &buf);
        log_error("JS: %s", s ? s : "(none)");
    }

    mu_end(&g_mu);
}

mu_Context *kwcc_get_mu(void) {
    return &g_mu;
}
