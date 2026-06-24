/* kwcc_bus.c — Generic C Pub/Sub event bus (zero external dependencies)
 *
 * Design: linked list of topic groups, each group has a fixed-size callback array.
 * - subscribe: find/create topic group, add callback entry, return sub_id
 * - unsubscribe: find sub_id by scan, mark inactive
 * - publish: iterate groups, match topic, trigger all active callbacks in group
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "kwcc_bus.h"
#include "kwcc_base.h"
#include "llog.h"

/* ── Callback entry within a topic group ── */

#define KWCC_BUS_GROUP_MAX_CB 16

typedef struct {
    uint64_t        id;
    kwcc_bus_cb_t   cb;
    void           *user_data;
    int             in_use;
} kwcc_bus_cb_entry_t;

/* ── Topic group node ── */

typedef struct kwcc_bus_group {
    char                  *topic;
    kwcc_bus_cb_entry_t    callbacks[KWCC_BUS_GROUP_MAX_CB];
    int                    cb_count;
    struct kwcc_bus_group *next;
} kwcc_bus_group_t;

/* ── Global state ── */

static kwcc_bus_group_t *g_kwcc_bus_head = NULL;
static uint64_t          g_kwcc_bus_next_id = 1;

/* ── Match: exact / wildcard "*" / prefix (ends with '/') ── */

static int kwcc_bus_match_topic(const char *pattern, const char *topic) {
    if (strcmp(pattern, topic) == 0) return 1;
    if (strcmp(pattern, KWCC_BUS_WILDCARD) == 0) return 1;
    size_t plen = strlen(pattern);
    if (plen > 0 && pattern[plen - 1] == '/' && strncmp(pattern, topic, plen) == 0)
        return 1;
    return 0;
}

/* ── Init ── */

void kwcc_bus_init(void) {
    g_kwcc_bus_head = NULL;
    g_kwcc_bus_next_id = 1;
}

/* ── Subscribe: find/create topic group, add callback ── */

kwcc_bus_sub_id_t kwcc_bus_subscribe(const char *topic, kwcc_bus_cb_t cb, void *user_data) {
    if (!topic || !cb) return 0;

    char safe[256];
    kwcc_base_topic_sanitize(safe, sizeof(safe), topic);
    if (!kwcc_base_topic_check(safe)) {
        log_warn("bus: subscribe skipped, invalid topic: '%s'", topic);
        return 0;
    }

    /* Find existing group */
    kwcc_bus_group_t *grp = g_kwcc_bus_head;
    while (grp) {
        if (strcmp(grp->topic, safe) == 0) break;
        grp = grp->next;
    }

    /* Create new group if not found */
    if (!grp) {
        grp = calloc(1, sizeof(*grp));
        if (!grp) { log_error("bus: calloc failed"); return 0; }
        grp->topic = strdup(safe);
        grp->next = g_kwcc_bus_head;
        g_kwcc_bus_head = grp;
    }

    /* Find empty slot in group */
    for (int i = 0; i < KWCC_BUS_GROUP_MAX_CB; i++) {
        if (!grp->callbacks[i].in_use) {
            grp->callbacks[i].id = g_kwcc_bus_next_id;
            grp->callbacks[i].cb = cb;
            grp->callbacks[i].user_data = user_data;
            grp->callbacks[i].in_use = 1;
            grp->cb_count++;
            return g_kwcc_bus_next_id++;
        }
    }

    log_warn("bus: topic group full (topic='%s', max=%d)", topic, KWCC_BUS_GROUP_MAX_CB);
    return 0;
}

/* ── Unsubscribe: find sub_id, mark inactive ── */

void kwcc_bus_unsubscribe(kwcc_bus_sub_id_t id) {
    for (kwcc_bus_group_t *grp = g_kwcc_bus_head; grp; grp = grp->next) {
        for (int i = 0; i < KWCC_BUS_GROUP_MAX_CB; i++) {
            if (grp->callbacks[i].in_use && grp->callbacks[i].id == id) {
                grp->callbacks[i].in_use = 0;
                grp->cb_count--;
                return;
            }
        }
    }
}

/* ── Publish: iterate groups, match, trigger callbacks ── */

void kwcc_bus_publish(const char *topic, const void *data, size_t len) {
    if (!topic) return;

    char safe[256];
    kwcc_base_topic_sanitize(safe, sizeof(safe), topic);
    if (!kwcc_base_topic_check(safe)) {
        log_warn("bus: publish skipped, invalid topic: '%s'", topic);
        return;
    }

    for (kwcc_bus_group_t *grp = g_kwcc_bus_head; grp; grp = grp->next) {
        if (!kwcc_bus_match_topic(grp->topic, safe)) continue;
        for (int i = 0; i < KWCC_BUS_GROUP_MAX_CB; i++) {
            if (grp->callbacks[i].in_use) {
                grp->callbacks[i].cb(safe, data, len, grp->callbacks[i].user_data);
            }
        }
    }
}
