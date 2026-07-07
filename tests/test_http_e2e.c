/* test_http_e2e.c — C end-to-end test for kwcc_http
 *
 * Sends real HTTP requests via fork+pipe+curl, polls I/O reactor,
 * reads parsed results via bus callback.
 * Requires: /usr/bin/curl + network access
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "src/kwcc_base.h"
#include "src/kwcc_bus.h"
#include "src/kwcc_config.h"
#include "src/kwcc_http.h"
#include "src/kwcc_io.h"
#include "src/kwcc_mempool.h"
#include "src/llog.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name, cond) do { \
    if (cond) { tests_passed++; printf("  PASS: %s\n", #name); } \
    else { tests_failed++; printf("  FAIL: %s\n", #name); } \
} while (0)

/* ── Shared state for bus callbacks ── */

typedef struct {
    int      got_end;
    int      got_error;
    int      status;
    int      body_len;
    int      header_count;
    char     req_id[64];
} test_result_t;

static test_result_t g_results[8];
static int g_result_count = 0;

static void on_http_event(const char *topic, const void *data, size_t len, void *user_data) {
    (void)data; (void)len; (void)user_data;

    if (strncmp(topic, "http/", 5) != 0) return;
    const char *rest = topic + 5;

    int is_end = (strncmp(rest, "end/", 4) == 0);
    int is_error = (strncmp(rest, "error/", 6) == 0);
    if (!is_end && !is_error) return;

    const char *id = is_end ? rest + 4 : rest + 6;

    kwcc_http_result_t result;
    if (kwcc_http_get_result(id, &result) == 0 && g_result_count < 8) {
        int hcount = kwcc_http_result_header_count(id);
        test_result_t *r = &g_results[g_result_count++];
        r->got_end = is_end;
        r->got_error = is_error;
        r->status = result.status;
        r->body_len = result.body_len;
        r->header_count = hcount;
        strncpy(r->req_id, id, sizeof(r->req_id) - 1);
        r->req_id[sizeof(r->req_id) - 1] = '\0';

        printf("  [bus] %s: status=%d body_len=%d headers=%d error=%d\n",
               topic, result.status, result.body_len, hcount, result.error);

        /* Print body preview for debugging */
        if (result.body && result.body_len > 0) {
            int preview = result.body_len < 120 ? result.body_len : 120;
            printf("    body: %.*s\n", preview, result.body);
        }

        for (int i = 0; i < hcount && i < 5; i++) {
            const char *hname = NULL, *hvalue = NULL;
            size_t nlen = 0, vlen = 0;
            if (kwcc_http_result_get_header(id, i, &hname, &nlen, &hvalue, &vlen) == 0) {
                printf("    header[%d]: %.*s = %.*s\n", i, (int)nlen, hname, (int)vlen, hvalue);
            }
        }
        /* Do NOT call cleanup_by_req_id here — on_read will cleanup after dispatch_end returns */
    }
}

/* ── Helper: poll until result arrives or timeout ── */

static test_result_t *poll_until_result(const char *req_id, int timeout_sec) {
    time_t start = time(NULL);
    int prev_count = g_result_count;
    printf("  [poll] waiting for req_id=%s, prev_count=%d, timeout=%ds\n", req_id, prev_count, timeout_sec);
    while ((int)(time(NULL) - start) < timeout_sec) {
        kwcc_io_poll_once();
        kwcc_http_check_progress();
        if (g_result_count > prev_count) {
            printf("  [poll] new result detected: g_result_count=%d\n", g_result_count);
            for (int i = prev_count; i < g_result_count; i++) {
                if (strcmp(g_results[i].req_id, req_id) == 0) {
                    return &g_results[i];
                }
            }
        }
        usleep(10000);
    }
    printf("  [poll] TIMEOUT for req_id=%s, g_result_count=%d\n", req_id, g_result_count);
    return NULL;
}

/* ── Tests ── */

static void test_get_success(void) {
    printf("--- test_get_success ---\n");
    const char *rid = kwcc_http_request("GET", "https://httpbin.org/get", NULL, 0, NULL, 0);
    if (!rid) {
        printf("  SKIP: curl not available or request failed\n");
        return;
    }
    char req_id[64];
    strncpy(req_id, rid, sizeof(req_id) - 1);
    req_id[sizeof(req_id) - 1] = '\0';
    test_result_t *r = poll_until_result(req_id, 15);
    if (!r) { TEST(get_success_timeout, 0); return; }
    TEST(get_status_200, r->status == 200);
    TEST(get_body_nonempty, r->body_len > 0);
    TEST(get_headers_present, r->header_count > 0);
    TEST(get_is_end_not_error, r->got_end && !r->got_error);
}

static void test_post_success(void) {
    printf("--- test_post_success ---\n");
    const char *body = "hello=world";
    const char *rid = kwcc_http_request("POST", "https://httpbin.org/post", NULL, 0, body, (int)strlen(body));
    if (!rid) {
        printf("  SKIP: request failed\n");
        return;
    }
    char req_id[64];
    strncpy(req_id, rid, sizeof(req_id) - 1);
    req_id[sizeof(req_id) - 1] = '\0';
    test_result_t *r = poll_until_result(req_id, 15);
    if (!r) { TEST(post_success_timeout, 0); return; }
    TEST(post_status_200, r->status == 200);
    TEST(post_body_nonempty, r->body_len > 0);
}

static void test_get_ip(void) {
    printf("--- test_get_ip ---\n");
    const char *rid = kwcc_http_request("GET", "https://httpbin.org/ip", NULL, 0, NULL, 0);
    if (!rid) {
        printf("  SKIP: curl not available or request failed\n");
        return;
    }
    char req_id[64];
    strncpy(req_id, rid, sizeof(req_id) - 1);
    req_id[sizeof(req_id) - 1] = '\0';
    test_result_t *r = poll_until_result(req_id, 15);
    if (!r) { TEST(get_ip_timeout, 0); return; }
    TEST(get_ip_status_200, r->status == 200);
    TEST(get_ip_body_nonempty, r->body_len > 0);
    TEST(get_ip_headers_present, r->header_count > 0);
    TEST(get_ip_is_end, r->got_end && !r->got_error);
}

static void test_cancel_request(void) {
    printf("--- test_cancel_request ---\n");
    const char *rid = kwcc_http_request("GET", "https://httpbin.org/delay/5", NULL, 0, NULL, 0);
    if (!rid) {
        printf("  SKIP: request failed\n");
        return;
    }
    char req_id[64];
    strncpy(req_id, rid, sizeof(req_id) - 1);
    req_id[sizeof(req_id) - 1] = '\0';
    kwcc_http_cancel(req_id);
    TEST(cancel_no_crash, 1);
}

/* ── Main ── */

int main(void) {
    printf("kwcc_http end-to-end tests:\n\n");

    kwcc_mempool_init();
    kwcc_bus_init();
    kwcc_io_init();
    kwcc_http_init();

    /* Subscribe to all HTTP events: "http/" matches http/end/*, http/error/*, etc. */
    kwcc_bus_subscribe("http/", on_http_event, NULL);

    test_get_success();
    test_get_ip();
    test_post_success();
    test_cancel_request();

    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
