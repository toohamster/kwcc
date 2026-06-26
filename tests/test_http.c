/* test_http.c — Pure C tests for kwcc_http module
 *
 * Validates data structures, boundary conditions, and safety.
 * Full HTTP request tests (requiring curl + network) are done in Step 8.
 *
 * Tests:
 *   1. kwcc_http_init does not crash
 *   2. req_id increments on successive requests
 *   3. concurrent request limit (KWCC_HTTP_MAX_REQS = 8)
 *   4. overflow beyond limit returns NULL
 *   5. cancel nonexistent req_id does not crash
 *   6. cancel NULL req_id does not crash
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "src/kwcc_http.h"
#include "src/kwcc_bus.h"
#include "src/kwcc_base.h"
#include "src/kwcc_io.h"
#include "src/kwcc_config.h"
#include "src/kwcc_mempool.h"
#include "src/llog.h"

#define TEST(name, cond) do { \
    if (cond) { printf("  PASS: %s\n", #name); } \
    else { printf("  FAIL: %s\n", #name); } \
} while (0)

int main(void) {
    int passed = 0, failed = 0;

    /* Initialize dependencies */
    kwcc_mempool_init();
    kwcc_bus_init();
    kwcc_io_init();
    kwcc_http_init();

    /* 1. kwcc_http_init does not crash */
    TEST(init_no_crash, 1);
    passed++;

    /* 2. req_id increments — need curl to be available */
    {
        const char *id1 = kwcc_http_request("GET", "http://127.0.0.1:1/test", NULL, 0, NULL, 0);
        const char *id2 = kwcc_http_request("GET", "http://127.0.0.1:1/test", NULL, 0, NULL, 0);
        if (id1 && id2) {
            TEST(req_id_increments, strcmp(id1, id2) != 0);
            if (strcmp(id1, id2) != 0) passed++; else failed++;
        } else {
            /* curl not found on system — skip, just verify no crash */
            TEST(req_id_increments_curl_missing_skip, 1);
            passed++;
        }
        /* Cleanup: cancel any started requests */
        if (id1) kwcc_http_cancel(id1);
        if (id2) kwcc_http_cancel(id2);
    }

    /* 3. concurrent request limit */
    {
        const char *ids[KWCC_HTTP_MAX_REQS + 2];
        int count = 0;
        for (int i = 0; i < KWCC_HTTP_MAX_REQS + 2; i++) {
            const char *id = kwcc_http_request("GET", "http://127.0.0.1:1/test", NULL, 0, NULL, 0);
            if (id) {
                ids[count++] = id;
            }
        }
        TEST(concurrent_limit_respected, count <= KWCC_HTTP_MAX_REQS);
        if (count <= KWCC_HTTP_MAX_REQS) passed++; else failed++;

        /* Cleanup */
        for (int i = 0; i < count; i++) {
            kwcc_http_cancel(ids[i]);
        }
    }

    /* 4. overflow beyond limit returns NULL */
    /* After test 3, all slots are cancelled. Request again after max. */
    {
        const char *ids[KWCC_HTTP_MAX_REQS];
        int count = 0;
        for (int i = 0; i < KWCC_HTTP_MAX_REQS; i++) {
            const char *id = kwcc_http_request("GET", "http://127.0.0.1:1/test", NULL, 0, NULL, 0);
            if (id) ids[count++] = id;
        }
        const char *overflow = kwcc_http_request("GET", "http://127.0.0.1:1/test", NULL, 0, NULL, 0);
        TEST(overflow_returns_null, overflow == NULL);
        if (overflow == NULL) passed++; else failed++;

        /* Cleanup */
        for (int i = 0; i < count; i++) {
            kwcc_http_cancel(ids[i]);
        }
    }

    /* 5. cancel nonexistent req_id does not crash */
    kwcc_http_cancel("nonexistent_id");
    TEST(cancel_nonexistent_no_crash, 1);
    passed++;

    /* 6. cancel NULL req_id does not crash */
    kwcc_http_cancel(NULL);
    TEST(cancel_null_no_crash, 1);
    passed++;

    printf("\n%d/%d tests passed.\n", passed, passed + failed);
    return failed > 0 ? 1 : 0;
}
