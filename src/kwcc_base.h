/* kwcc_base.h — common infrastructure (config, constants, utilities)
 *
 * All modules (bus, io, etc.) include this header.
 * Does NOT depend on mquickjs, microui, or nanovg.
 */
#ifndef KWCC_BASE_H
#define KWCC_BASE_H

#include <stddef.h>

/* ── Memory pool compile-time defaults ── */
#ifndef KWCC_CORE_SIZE
#define KWCC_CORE_SIZE  (32 * 1024)
#endif
#ifndef KWCC_APP_SIZE
#define KWCC_APP_SIZE   (256 * 1024)
#endif
#ifndef KWCC_USER_SIZE
#define KWCC_USER_SIZE  (1 * 1024 * 1024)
#endif
#ifndef KWCC_DEBUG
#define KWCC_DEBUG 0
#endif

/* ── Topic validation and sanitization ── */
/* Allowed characters: A-Z a-z 0-9 / _ */
void kwcc_base_topic_sanitize(char *out, size_t out_size, const char *in);
int  kwcc_base_topic_check(const char *topic);

#endif
