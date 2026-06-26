#ifndef KWCC_HTTP_H
#define KWCC_HTTP_H

#include <stddef.h>

#define KWCC_HTTP_MAX_REQS 8
#define KWCC_HTTP_INIT_CAP 4096

/* Parsed HTTP response result (read-only for consumers) */
struct phr_header;
typedef struct {
    int      error;               /* 0 = success, 1 = error */
    int      status;              /* HTTP status code */
    const char *body;             /* pointer into internal buffer (do not free) */
    int      body_len;
    const struct phr_header *headers;
    size_t   num_headers;
} kwcc_http_result_t;

void        kwcc_http_init(void);
const char *kwcc_http_request(const char *method,
                              const char *url, const char **headers, int header_count,
                              const char *body, int body_len);
void        kwcc_http_cancel(const char *req_id);
void        kwcc_http_check_progress(void);

/* Look up parsed result by req_id. Returns 0 if found, -1 if not.
 * Result is valid only during the bus event callback (before cleanup). */
int         kwcc_http_get_result(const char *req_id, kwcc_http_result_t *out);

/* Callback registration for async response routing.
 * Stores opaque pointers (e.g. JSValue from kwcc_js.c).
 * The consumer (JS bridge) is responsible for type casting. */
int         kwcc_http_register_callback(const char *req_id,
                                        void *on_end, void *on_error, void *on_progress);

/* Find callback by req_id. Returns 0 if found, -1 if not.
 * Sets *is_in_use=0 after retrieval (one-shot, auto-cleanup). */
int         kwcc_http_find_callback(const char *req_id,
                                    void **on_end, void **on_error, void **on_progress);

/* Clear callback entry by req_id (for cancel/cleanup). */
void        kwcc_http_clear_callback(const char *req_id);

#endif
