#ifndef KWCC_HTTP_H
#define KWCC_HTTP_H

#include <stddef.h>

#define KWCC_HTTP_MAX_REQS 8
#define KWCC_HTTP_INIT_CAP 4096

/* Parsed HTTP response result (read-only for consumers) */
typedef struct {
    int      error;               /* 0 = success, 1 = error */
    int      status;              /* HTTP status code */
    const char *body;             /* pointer into internal buffer (do not free) */
    int      body_len;
} kwcc_http_result_t;

/* Progress info (passed as bus data for http/progress events) */
typedef struct {
    int      loaded;              /* bytes received so far */
    int      total;               /* total bytes from Content-Length, -1 if unknown */
} kwcc_http_progress_t;

void        kwcc_http_init(void);
const char *kwcc_http_request(const char *method,
                              const char *url, const char **headers, int header_count,
                              const char *body, int body_len);
void        kwcc_http_cancel(const char *req_id);
void        kwcc_http_check_progress(void);

/* Look up parsed result by req_id. Returns 0 if found, -1 if not.
 * Result is valid only during the bus event callback (before cleanup). */
int         kwcc_http_get_result(const char *req_id, kwcc_http_result_t *out);

/* Header access API (hides phr_header implementation detail).
 * Valid only during the bus event callback (same lifetime as kwcc_http_get_result). */
int         kwcc_http_result_header_count(const char *req_id);
int         kwcc_http_result_get_header(const char *req_id, int index,
                                        const char **name, size_t *name_len,
                                        const char **value, size_t *value_len);

/* Release request resources by req_id (for ack_cleanup callback).
 * Safe to call after data has been copied into GC heap. */
void        kwcc_http_cleanup_by_req_id(const char *req_id);

#endif
