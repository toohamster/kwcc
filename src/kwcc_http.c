/* kwcc_http.c — HTTP Process Engine (Layer 2)
 *
 * Fork independent curl process, read response via non-blocking pipe,
 * parse with picohttpparser, dispatch via kwcc_bus_publish.
 *
 * No dependency on mquickjs / JSContext / microui / NanoVG / Sokol.
 */
#include "kwcc_http.h"
#include "kwcc_io.h"
#include "kwcc_bus.h"
#include "kwcc_base.h"
#include "kwcc_config.h"
#include "picohttpparser/picohttpparser.h"
#include "llog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>

/* ── Data structures ────────────────────────────────────────── */

typedef struct {
    char     req_id[64];
    char     method[16];
    char     url[1024];
    char    *body;
    int      body_len;
    char    *headers[16];
    int      header_count;
    pid_t    pid;
    int      pipe_read_fd;
    char    *response_buf;
    int      response_cap;
    int      response_len;
    int      total_size;
    int      http_status;
    int      last_dispatched;
    int      in_use;
    /* Parsed result for JS bridge (valid between dispatch_end and cleanup) */
    int      result_error;         /* 0 = success, 1 = error */
    int      result_status;        /* HTTP status code */
    const char *result_body;       /* pointer into response_buf (do not free) */
    int      result_body_len;
    struct phr_header result_headers[64];
    size_t   result_num_headers;
} kwcc_http_req_t;

static kwcc_http_req_t g_kwcc_http_reqs[KWCC_HTTP_MAX_REQS];
static int g_kwcc_http_next_seq = 0;

/* ── Forward declarations ───────────────────────────────────── */

static void kwcc_http_on_read(int fd, void *user_data);
static void kwcc_http_dispatch_end(kwcc_http_req_t *req, int error,
                                   int status, const char *body, int body_len,
                                   struct phr_header *headers, size_t num_headers);
static void kwcc_http_cleanup(kwcc_http_req_t *req);
static kwcc_http_req_t *kwcc_http_req_find(const char *req_id);

/* ── Public API ─────────────────────────────────────────────── */

void kwcc_http_init(void) {
    memset(g_kwcc_http_reqs, 0, sizeof(g_kwcc_http_reqs));
}

const char *kwcc_http_request(const char *method,
                              const char *url, const char **headers, int header_count,
                              const char *body, int body_len) {
    /* 1. Find free slot */
    kwcc_http_req_t *req = NULL;
    for (int i = 0; i < KWCC_HTTP_MAX_REQS; i++) {
        if (!g_kwcc_http_reqs[i].in_use) {
            req = &g_kwcc_http_reqs[i];
            break;
        }
    }
    if (!req) {
        log_error("http: max concurrent requests (%d) reached", KWCC_HTTP_MAX_REQS);
        return NULL;
    }

    /* 2. Generate req_id */
    snprintf(req->req_id, sizeof(req->req_id), "req_%d", g_kwcc_http_next_seq++);

    /* 3. Copy parameters */
    strncpy(req->method, method ? method : "GET", sizeof(req->method) - 1);
    req->method[sizeof(req->method) - 1] = '\0';
    strncpy(req->url, url ? url : "", sizeof(req->url) - 1);
    req->url[sizeof(req->url) - 1] = '\0';

    if (body && body_len > 0) {
        req->body = malloc(body_len + 1);
        if (!req->body) {
            log_error("http: malloc body failed");
            memset(req, 0, sizeof(kwcc_http_req_t));
            return NULL;
        }
        memcpy(req->body, body, body_len);
        req->body[body_len] = '\0';
        req->body_len = body_len;
    }

    req->header_count = 0;
    for (int i = 0; i < header_count && i < 16; i++) {
        if (headers[i]) {
            req->headers[i] = strdup(headers[i]);
            req->header_count++;
        }
    }

    /* 4. Read config */
    const char *bin_path = kwcc_config_get_core("http/bin_path", "curl");
    const char *timeout_str = kwcc_config_get_core("http/timeout", "30");

    /* Fix 6: bin_path executable check */
    if (access(bin_path, X_OK) == -1) {
        log_error("http: bin_path '%s' not found or not executable", bin_path);
        /* free allocated memory before returning */
        free(req->body);
        for (int i = 0; i < req->header_count; i++) free(req->headers[i]);
        memset(req, 0, sizeof(kwcc_http_req_t));
        return NULL;
    }

    /* 5. Build curl argv */
    int argc_max = 8 + header_count * 2 + (body ? 2 : 0);
    char **argv = malloc(sizeof(char *) * (argc_max + 1));
    if (!argv) {
        free(req->body);
        for (int i = 0; i < req->header_count; i++) free(req->headers[i]);
        memset(req, 0, sizeof(kwcc_http_req_t));
        return NULL;
    }

    int ai = 0;
    argv[ai++] = (char *)bin_path;
    argv[ai++] = "-s";       /* silent */
    argv[ai++] = "-L";       /* follow redirects (Fix 3) */
    argv[ai++] = "-i";       /* include headers in output */
    argv[ai++] = "-X";
    argv[ai++] = req->method;

    /* --max-time */
    char timeout_arg[32];
    snprintf(timeout_arg, sizeof(timeout_arg), "--max-time %s", timeout_str);
    argv[ai++] = timeout_arg;

    /* headers */
    for (int i = 0; i < req->header_count; i++) {
        argv[ai++] = "-H";
        argv[ai++] = req->headers[i];
    }

    /* body */
    if (req->body) {
        argv[ai++] = "-d";
        argv[ai++] = req->body;
    }

    argv[ai++] = req->url;
    argv[ai] = NULL;

    /* 6. pipe + fork */
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        log_error("http: pipe() failed: %s", strerror(errno));
        free(argv);
        free(req->body);
        for (int i = 0; i < req->header_count; i++) free(req->headers[i]);
        memset(req, 0, sizeof(kwcc_http_req_t));
        return NULL;
    }

    /* Set FD_CLOEXEC on write end so it auto-closes if execvp succeeds */
    fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);

    pid_t pid = fork();
    if (pid < 0) {
        log_error("http: fork() failed: %s", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        free(argv);
        free(req->body);
        for (int i = 0; i < req->header_count; i++) free(req->headers[i]);
        memset(req, 0, sizeof(kwcc_http_req_t));
        return NULL;
    }

    if (pid == 0) {
        /* ── Child process ── */
        close(pipefd[0]);

        /* Fix 10: close all FDs > STDERR_FILENO except pipe write end */
        long max_fd = sysconf(_SC_OPEN_MAX);
        for (int fd = STDERR_FILENO + 1; fd < max_fd; fd++) {
            if (fd != pipefd[1]) close(fd);
        }

        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        execvp(bin_path, argv);
        _exit(1);
    }

    /* ── Parent process ── */
    close(pipefd[1]);
    free(argv);

    /* Set read end non-blocking */
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    /* Allocate initial response buffer */
    req->response_buf = malloc(KWCC_HTTP_INIT_CAP);
    if (!req->response_buf) {
        log_error("http: malloc response_buf failed");
        close(pipefd[0]);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, WNOHANG);
        free(req->body);
        for (int i = 0; i < req->header_count; i++) free(req->headers[i]);
        memset(req, 0, sizeof(kwcc_http_req_t));
        return NULL;
    }
    req->response_cap = KWCC_HTTP_INIT_CAP;
    req->response_len = 0;
    req->last_dispatched = 0;
    req->pid = pid;
    req->pipe_read_fd = pipefd[0];
    req->in_use = 1;

    /* Register with I/O reactor */
    kwcc_io_register(req->pipe_read_fd, kwcc_http_on_read, req);

    log_info("http: started %s %s (req_id=%s, pid=%d)", req->method, req->url, req->req_id, (int)pid);
    return req->req_id;
}

void kwcc_http_cancel(const char *req_id) {
    if (!req_id) return;
    kwcc_http_req_t *req = kwcc_http_req_find(req_id);
    if (!req) return;
    kill(req->pid, SIGTERM);
    /* publish cancel event so JS side can clean up */
    char topic[128];
    char safe[128];
    snprintf(topic, sizeof(topic), "http/cancel/%s", req_id);
    kwcc_base_topic_sanitize(safe, sizeof(safe), topic);
    kwcc_bus_publish(safe, NULL, 0);
    kwcc_http_cleanup(req);
}

void kwcc_http_check_progress(void) {
    for (int i = 0; i < KWCC_HTTP_MAX_REQS; i++) {
        kwcc_http_req_t *req = &g_kwcc_http_reqs[i];
        if (!req->in_use) continue;

        /* Zombie reclamation: check if child has exited without us noticing */
        if (req->pid > 0) {
            int status;
            pid_t ret = waitpid(req->pid, &status, WNOHANG);
            if (ret > 0) {
                /* Child exited — if we haven't dispatched end yet, do it now */
                if (req->response_len > 0 && req->result_status == 0) {
                    /* Parse what we have and dispatch */
                    const char *p = req->response_buf;
                    size_t remaining = req->response_len;
                    int final_status = 0;
                    struct phr_header final_headers[64];
                    size_t final_num_headers = 64;

                    while (remaining >= 9 && memcmp(p, "HTTP/1.", 7) == 0) {
                        size_t nh = 64;
                        int mv, st;
                        const char *msg;
                        size_t msg_len;
                        int r = phr_parse_response(p, remaining, &mv, &st, &msg, &msg_len,
                                                   final_headers, &nh, 0);
                        if (r == -2 || r <= 0) break;
                        final_status = st;
                        final_num_headers = nh;
                        p += r;
                        remaining -= r;
                    }
                    kwcc_http_dispatch_end(req, 0, final_status, p, (int)remaining,
                                            final_headers, final_num_headers);
                } else if (req->result_status == 0) {
                    /* No data at all — dispatch error */
                    kwcc_http_dispatch_end(req, 1, 0, NULL, 0, NULL, 0);
                }
                kwcc_http_cleanup(req);
                continue;
            }
        }

        /* Progress reporting */
        if (req->response_len != req->last_dispatched) {
            req->last_dispatched = req->response_len;
            kwcc_http_progress_t prog;
            prog.loaded = req->response_len;
            prog.total = req->total_size;
            char topic[128];
            char safe[128];
            snprintf(topic, sizeof(topic), "http/progress/%s", req->req_id);
            kwcc_base_topic_sanitize(safe, sizeof(safe), topic);
            kwcc_bus_publish(safe, &prog, sizeof(prog));
        }
    }
}

/* ── Internal: read callback from I/O reactor ───────────────── */

static void kwcc_http_on_read(int fd, void *user_data) {
    kwcc_http_req_t *req = (kwcc_http_req_t *)user_data;
    char buf[4096];
    ssize_t n;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        /* Fix 1: realloc +4 bytes + trailing '\0' + NULL check */
        if (req->response_len + (int)n + 4 > req->response_cap) {
            int new_cap = (req->response_len + (int)n + 4) * 2;
            char *new_buf = realloc(req->response_buf, new_cap);
            if (!new_buf) {
                log_error("http: realloc failed for %s", req->req_id);
                kwcc_http_dispatch_end(req, 1, 0, NULL, 0, NULL, 0);
                kwcc_http_cleanup(req);
                return;
            }
            req->response_buf = new_buf;
            req->response_cap = new_cap;
        }
        memcpy(req->response_buf + req->response_len, buf, n);
        req->response_len += (int)n;
        req->response_buf[req->response_len] = '\0';

        /* Incremental Content-Length extraction (only if not yet known) */
        if (req->total_size == 0 && req->response_len > 0) {
            /* Look for end of headers ("\r\n\r\n") to parse Content-Length */
            const char *hdr_end = memmem(req->response_buf, req->response_len,
                                         "\r\n\r\n", 4);
            if (hdr_end) {
                /* Parse status line + headers to find Content-Length */
                size_t hdr_len = (hdr_end + 2) - req->response_buf;
                int mv, st;
                const char *msg;
                size_t msg_len;
                struct phr_header hdrs[64];
                size_t nh = 64;
                int r = phr_parse_response(req->response_buf, hdr_len,
                                           &mv, &st, &msg, &msg_len, hdrs, &nh, 0);
                if (r > 0) {
                    for (size_t h = 0; h < nh; h++) {
                        if (hdrs[h].name_len == 14 &&
                            strncasecmp(hdrs[h].name, "Content-Length", 14) == 0) {
                            req->total_size = atoi(hdrs[h].value);
                            break;
                        }
                    }
                }
            }
        }
    }

    if (n == 0) {
        /* EOF: curl exited, parse response */
        const char *p = req->response_buf;
        size_t remaining = req->response_len;
        int ret = 0;
        int final_status = 0;
        struct phr_header final_headers[64];
        size_t final_num_headers = 64;

        /* Fix 4: loop parse to skip intermediate responses (302/301 redirects) */
        while (remaining >= 9 && memcmp(p, "HTTP/1.", 7) == 0) {
            size_t nh = 64;
            int mv, st;
            const char *msg;
            size_t msg_len;
            int r = phr_parse_response(p, remaining, &mv, &st, &msg, &msg_len,
                                       final_headers, &nh, 0);
            if (r == -2) break;   /* Fix 8: incomplete data */
            if (r <= 0) break;    /* -1 = protocol error */
            ret += r;
            final_status = st;
            final_num_headers = nh;
            p += r;
            remaining -= r;
        }

        kwcc_http_dispatch_end(req, 0, final_status, p, (int)remaining,
                                final_headers, final_num_headers);
        kwcc_http_cleanup(req);
        return;
    }

    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        log_error("http: read error for %s: %s", req->req_id, strerror(errno));
        kwcc_http_dispatch_end(req, 1, 0, NULL, 0, NULL, 0);
        kwcc_http_cleanup(req);
    }
}

/* ── Internal: dispatch end/error via bus ───────────────────── */

static void kwcc_http_dispatch_end(kwcc_http_req_t *req, int error,
                                   int status, const char *body, int body_len,
                                   struct phr_header *headers, size_t num_headers) {
    /* Store parsed result on req for JS bridge to read via kwcc_http_get_result */
    req->result_error = error;
    req->result_status = status;
    req->result_body = body;
    req->result_body_len = body_len;
    if (headers && num_headers > 0) {
        size_t copy = num_headers > 64 ? 64 : num_headers;
        memcpy(req->result_headers, headers, copy * sizeof(struct phr_header));
        req->result_num_headers = copy;
    } else {
        req->result_num_headers = 0;
    }

    /* Publish bus event — JS bridge extracts req_id from topic,
     * calls kwcc_http_get_result to read parsed data,
     * constructs JSValue response object via C API (Fix 2: no string injection) */
    char topic[128];
    char safe[128];
    snprintf(topic, sizeof(topic), error ? "http/error/%s" : "http/end/%s", req->req_id);
    kwcc_base_topic_sanitize(safe, sizeof(safe), topic);
    kwcc_bus_publish(safe, NULL, 0);

    log_info("http: %s %s completed (status=%d, body_len=%d, error=%d)",
             req->method, req->url, status, body_len, error);
}

/* ── Public: look up parsed result by req_id ────────────────── */

int kwcc_http_get_result(const char *req_id, kwcc_http_result_t *out) {
    if (!req_id || !out) return -1;
    kwcc_http_req_t *req = kwcc_http_req_find(req_id);
    if (!req) return -1;
    out->error = req->result_error;
    out->status = req->result_status;
    out->body = req->result_body;
    out->body_len = req->result_body_len;
    return 0;
}

/* ── Internal: find request by req_id ──────────────────────── */

static kwcc_http_req_t *kwcc_http_req_find(const char *req_id) {
    if (!req_id) return NULL;
    for (int i = 0; i < KWCC_HTTP_MAX_REQS; i++) {
        if (g_kwcc_http_reqs[i].in_use && strcmp(g_kwcc_http_reqs[i].req_id, req_id) == 0)
            return &g_kwcc_http_reqs[i];
    }
    return NULL;
}

/* ── Public: header access API (hides phr_header) ─────────── */

int kwcc_http_result_header_count(const char *req_id) {
    kwcc_http_req_t *req = kwcc_http_req_find(req_id);
    if (!req) return 0;
    return (int)req->result_num_headers;
}

int kwcc_http_result_get_header(const char *req_id, int index,
                                const char **name, size_t *name_len,
                                const char **value, size_t *value_len) {
    kwcc_http_req_t *req = kwcc_http_req_find(req_id);
    if (!req || index < 0 || index >= (int)req->result_num_headers)
        return -1;
    const struct phr_header *h = &req->result_headers[index];
    if (name)      *name      = h->name;
    if (name_len)  *name_len  = h->name_len;
    if (value)     *value     = h->value;
    if (value_len) *value_len = h->value_len;
    return 0;
}

/* ── Public: cleanup by req_id (for ack_cleanup) ──────────── */

void kwcc_http_cleanup_by_req_id(const char *req_id) {
    kwcc_http_req_t *req = kwcc_http_req_find(req_id);
    if (req) kwcc_http_cleanup(req);
}

/* ── Internal: cleanup request ──────────────────────────────── */

static void kwcc_http_cleanup(kwcc_http_req_t *req) {
    /* Unregister from I/O reactor first, then close fd */
    kwcc_io_unregister(req->pipe_read_fd);
    if (req->pipe_read_fd >= 0) {
        close(req->pipe_read_fd);
        req->pipe_read_fd = -1;
    }
    /* Reap child process to prevent zombies (Fix 5) */
    if (req->pid > 0) {
        waitpid(req->pid, NULL, WNOHANG);
        req->pid = 0;
    }
    free(req->response_buf);
    req->response_buf = NULL;
    free(req->body);
    req->body = NULL;
    for (int i = 0; i < req->header_count; i++) {
        free(req->headers[i]);
        req->headers[i] = NULL;
    }
    memset(req, 0, sizeof(kwcc_http_req_t));
}

