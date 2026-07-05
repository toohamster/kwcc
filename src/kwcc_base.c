/* kwcc_base.c — common infrastructure utilities */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kwcc_base.h"
#include "llog.h"

/* ── Topic sanitization ── */

void kwcc_base_topic_sanitize(char *out, size_t out_size, const char *in) {
    size_t len = strlen(in);
    int keep_star = (len >= 2 && in[len-2] == '/' && in[len-1] == '*');

    int j = 0;
    size_t limit = keep_star ? len - 2 : len;
    for (size_t i = 0; i < limit && j < (int)out_size - 3; i++) {
        char c = in[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '/' || c == '_') {
            out[j++] = c;
        }
    }
    if (keep_star) {
        out[j++] = '/';
        out[j++] = '*';
    }
    out[j] = '\0';
}

int kwcc_base_topic_check(const char *topic) {
    if (topic[0] == '\0') return 0;
    if (strcmp(topic, "/*") == 0) return 1;

    size_t len = strlen(topic);
    if (len >= 2 && topic[len-2] == '/' && topic[len-1] == '*') {
        len -= 2;
    }
    for (size_t i = 0; i < len; i++) {
        if (topic[i] != '/') return 1;
    }
    return 0;
}

/* ── String utilities ── */

kwcc_base_str_t kwcc_base_str_new(const char *data, size_t len) {
    kwcc_base_str_t s = { NULL, 0 };
    if (!data || len == 0) return s;
    char *buf = (char *)malloc(len + 1);
    if (!buf) return s;
    memcpy(buf, data, len);
    buf[len] = '\0';
    s.val = buf;
    s.len = len;
    return s;
}

void kwcc_base_str_free(kwcc_base_str_t *s) {
    if (!s) return;
    if (s->val) {
        free((void *)s->val);
        s->val = NULL;
    }
    s->len = 0;
}

const char *kwcc_base_str_cstr(const kwcc_base_str_t *s, const char *def) {
    if (!s || !s->val) return def;
    return s->val;
}

int32_t kwcc_base_str_int(const kwcc_base_str_t *s, int32_t def) {
    if (!s || !s->val) return def;
    return (int32_t)strtol(s->val, NULL, 10);
}

double kwcc_base_str_double(const kwcc_base_str_t *s, double def) {
    if (!s || !s->val) return def;
    return strtod(s->val, NULL);
}

/* ── Defer cleanup ── */

kwcc_base_defer_cleanup_t *kwcc_base_defer_cleanup_create(void) {
    kwcc_base_defer_cleanup_t *dc = (kwcc_base_defer_cleanup_t *)malloc(sizeof(kwcc_base_defer_cleanup_t));
    if (!dc) return NULL;
    dc->head = NULL;
    return dc;
}

void kwcc_base_defer_cleanup_push(kwcc_base_defer_cleanup_t *dc, void *ptr, kwcc_base_defer_cleanup_fn fn) {
    if (!dc) return;
    kwcc_base_defer_cleanup_node_t *node = (kwcc_base_defer_cleanup_node_t *)malloc(sizeof(kwcc_base_defer_cleanup_node_t));
    if (!node) return;
    node->ptr  = ptr;
    node->fn   = fn;
    node->next = dc->head;
    dc->head   = node;
}

void kwcc_base_defer_cleanup_run(kwcc_base_defer_cleanup_t *dc) {
    if (!dc) return;
    kwcc_base_defer_cleanup_node_t *node = dc->head;
    while (node) {
        kwcc_base_defer_cleanup_node_t *next = node->next;
        if (node->fn) node->fn(node->ptr);
        free(node);
        node = next;
    }
    free(dc);
}
