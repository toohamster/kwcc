/* kwcc_ui_bus.c — UI→JS event bridge
 *
 * Extracted from kwcc_bus.c. Handles UI widget topic mapping
 * and dispatches events to JS via $bus.emit().
 */
#include <stdio.h>
#include <string.h>
#include "mquickjs/mquickjs.h"
#include "kwcc_base.h"
#include "llog.h"
#include "kwcc_ui_bus.h"

/* ── Topic map ─────────────────────────────────────────────── */

#define KWCC_UI_TOPIC_MAP_SIZE 256
static struct { int id; char topic[128]; } g_kwcc_ui_topic_map[KWCC_UI_TOPIC_MAP_SIZE];
static int g_kwcc_ui_topic_count = 0;
static JSContext *g_kwcc_ui_bus_js_ctx = NULL;

/* ── JSContext setter ──────────────────────────────────────── */

void kwcc_ui_bus_set_js_ctx(void *ctx) {
    g_kwcc_ui_bus_js_ctx = (JSContext *)ctx;
}

/* ── Per-frame reset ───────────────────────────────────────── */

void kwcc_ui_bus_begin_frame(void) {
    g_kwcc_ui_topic_count = 0;
}

/* ── Bind: register ID → topic mapping ─────────────────────── */

void kwcc_ui_bus_bind_topic(int id, const char *topic) {
    char safe[256];
    kwcc_base_topic_sanitize(safe, sizeof(safe), topic);
    if (!kwcc_base_topic_check(safe)) {
        log_warn("ui_bus: bind_topic skipped, invalid topic: '%s'", topic);
        return;
    }
    if (g_kwcc_ui_topic_count < KWCC_UI_TOPIC_MAP_SIZE) {
        g_kwcc_ui_topic_map[g_kwcc_ui_topic_count].id = id;
        strncpy(g_kwcc_ui_topic_map[g_kwcc_ui_topic_count].topic, safe, 127);
        g_kwcc_ui_topic_map[g_kwcc_ui_topic_count].topic[127] = '\0';
        g_kwcc_ui_topic_count++;
    }
}

/* ── Event dispatch: C → JS via $bus.emit ──────────────────── */

void kwcc_ui_bus_dispatch_event(const char *topic, const char *action) {
    if (!g_kwcc_ui_bus_js_ctx || !topic) return;

    char safe[256];
    kwcc_base_topic_sanitize(safe, sizeof(safe), topic);
    if (!kwcc_base_topic_check(safe)) {
        log_warn("ui_bus: dispatch skipped, invalid topic: '%s'", topic);
        return;
    }

    char t[256], a[128], buf[512];
    int tj = 0;
    for (int i = 0; safe[i] && tj < 254; i++) {
        char c = safe[i];
        if (c == '\\' || c == '\'') t[tj++] = '\\';
        t[tj++] = c;
    }
    t[tj] = '\0';
    int aj = 0;
    if (action) {
        for (int i = 0; action[i] && aj < 126; i++) {
            char c = action[i];
            if (c == '\\' || c == '\'') a[aj++] = '\\';
            a[aj++] = c;
        }
    }
    a[aj] = '\0';
    snprintf(buf, sizeof(buf), "$bus.emit('%s', '%s', new Object());", t, a);
    JS_Eval(g_kwcc_ui_bus_js_ctx, buf, strlen(buf), "<ui_bus_dispatch>", 0);
}
