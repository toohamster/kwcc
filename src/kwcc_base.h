/* kwcc_base.h — common infrastructure (config, constants, utilities)
 *
 * All modules (bus, io, etc.) include this header.
 * Does NOT depend on mquickjs, microui, or nanovg.
 */
#ifndef KWCC_BASE_H
#define KWCC_BASE_H

#include <stddef.h>
#include <stdint.h>

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

/* ── String utilities ── */

typedef struct {
    const char *val;   /* null-terminated C string (always malloc'd) */
    size_t      len;   /* string length (excluding '\0') */
} kwcc_base_str_t;

/* Create a null-terminated string from data[0..len-1] (always mallocs) */
kwcc_base_str_t kwcc_base_str_new(const char *data, size_t len);

/* Free the internal malloc'd buffer */
void kwcc_base_str_free(kwcc_base_str_t *s);

/* Type conversion helpers — return def if val is NULL */
const char *kwcc_base_str_cstr(const kwcc_base_str_t *s, const char *def);
int32_t     kwcc_base_str_int(const kwcc_base_str_t *s, int32_t def);
double      kwcc_base_str_double(const kwcc_base_str_t *s, double def);

/* ── Defer cleanup — function-scoped resource auto-cleanup ── */

typedef void (*kwcc_base_defer_cleanup_fn)(void *);

typedef struct kwcc_base_defer_cleanup_node {
    void                                        *ptr;
    kwcc_base_defer_cleanup_fn                   fn;
    struct kwcc_base_defer_cleanup_node         *next;
} kwcc_base_defer_cleanup_node_t;

typedef struct {
    kwcc_base_defer_cleanup_node_t *head;
} kwcc_base_defer_cleanup_t;

/* malloc instance, head=NULL */
kwcc_base_defer_cleanup_t *kwcc_base_defer_cleanup_create(void);

/* head-insert a (ptr, fn) node, O(1) */
void kwcc_base_defer_cleanup_push(kwcc_base_defer_cleanup_t *dc, void *ptr, kwcc_base_defer_cleanup_fn fn);

/* walk list: exec fn(ptr) + free nodes + free dc itself */
void kwcc_base_defer_cleanup_run(kwcc_base_defer_cleanup_t *dc);

#endif
