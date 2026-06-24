/* kwcc_base.c — common infrastructure utilities */
#include <stdio.h>
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
