/* kwcc_bus.c — C→JS message bus bridge */
#include <stdio.h>
#include <string.h>
#include "mquickjs/mquickjs.h"
#include "llog.h"
#include "kwcc_bus.h"

/* ── Topic map ─────────────────────────────────────────────── */

#define TOPIC_MAP_SIZE 256
static struct { int id; char topic[128]; } g_topic_map[TOPIC_MAP_SIZE];
static int g_topic_map_count = 0;

/* ── Bind: register ID → topic mapping ─────────────────────── */

void kwcc_bind_topic(int id, const char *topic) {
    if (g_topic_map_count < TOPIC_MAP_SIZE && topic) {
        g_topic_map[g_topic_map_count].id = id;
        strncpy(g_topic_map[g_topic_map_count].topic, topic, 127);
        g_topic_map[g_topic_map_count].topic[127] = '\0';
        g_topic_map_count++;
    }
}

/* ── Per-frame reset ───────────────────────────────────────── */

void kwcc_bus_begin_frame(void) {
    g_topic_map_count = 0;
}

/* ── Event dispatch: C → JS via $bus.emit ──────────────────── */

void kwcc_dispatch_event(JSContext *ctx, const char *topic, const char *action) {
    char t[256], a[128], buf[512];
    int tj = 0;
    for (int i = 0; topic[i] && tj < 254; i++) {
        char c = topic[i];
        if (c == '\\' || c == '\'') t[tj++] = '\\';
        t[tj++] = c;
    }
    t[tj] = '\0';
    int aj = 0;
    for (int i = 0; action[i] && aj < 126; i++) {
        char c = action[i];
        if (c == '\\' || c == '\'') a[aj++] = '\\';
        a[aj++] = c;
    }
    a[aj] = '\0';
    snprintf(buf, sizeof(buf), "$bus.emit('%s', '%s', new Object());", t, a);
    JS_Eval(ctx, buf, strlen(buf), "<dispatch>", 0);
}
