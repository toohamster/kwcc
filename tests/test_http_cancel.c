/* test_http_cancel.c — L1 acceptance for http cancel 逻辑放弃 + 自治 reap + shutdown（方案一）
 *
 * Hermetic: uses a fake "curl" script (sleep 2s then emit a valid HTTP response)
 * so the child process deterministically stays alive (cancel window) and exits
 * naturally (reap window). No network dependency.
 *
 * Covered (adjusted from implementation-plan v2 H series; H8 dropped with the
 * service shell — 方案一 has no is_exiting guard):
 *   H1  cancel keeps slot occupied (in_use=1, deferred cleanup)   → T2
 *   H6  double cancel safe                                        → T3
 *   H4  no end/error dispatch during cancel + reap                → T4/T8
 *   H2  natural child exit → check_progress reaps silently        → T5-T7
 *   H9  shutdown reclaims all (SIGTERM + blocking waitpid)        → T10
 *   H7  request rejected after shutdown                           → T11
 *
 * Slot occupancy is observed via kwcc_http_get_result(): returns 0 while the
 * slot is in_use, -1 after cleanup (memset clears in_use so req_find misses).
 * Run: ./tests/bin/test_http_cancel
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
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

/* bus event counters (subscribe "http/" prefix) */
static int g_end_error_events = 0;
static int g_cancel_events = 0;

static void on_http_event(const char *topic, const void *data, size_t len, void *user_data) {
    (void)data; (void)len; (void)user_data;
    if (strncmp(topic, "http/cancel/", 12) == 0) { g_cancel_events++; return; }
    if (strncmp(topic, "http/end/", 9) == 0 || strncmp(topic, "http/error/", 11) == 0) {
        g_end_error_events++;
    }
}

#define FAKE_SH_PATH "/tmp/kwcc_test_fake_curl.sh"
static const char *FAKE_SH_CONTENT =
    "#!/bin/sh\n"
    "sleep 2\n"
    "echo \"HTTP/1.1 200 OK\"\n"
    "echo \"Content-Length: 2\"\n"
    "echo \"\"\n"
    "echo ok\n";

/* pack callback for kwcc_mempool_tlv_build: bin_path + timeout.
 * Protocol (kwcc_mempool.c:733): cb called repeatedly until it returns 0 —
 * no index param, track position internally (single-use in this test). */
static int pack_http_cfg(const char **key, const char **value,
                         uint8_t *type, size_t *value_len, void *user_data) {
    (void)user_data;
    static int field_idx = 0;
    if (field_idx == 0) {
        field_idx++;
        *key = "bin_path"; *value = FAKE_SH_PATH;
        *type = KWCC_MEMPOOL_TLV_FIELD; *value_len = strlen(*value);
        return 1;
    }
    if (field_idx == 1) {
        field_idx++;
        *key = "timeout"; *value = "10";
        *type = KWCC_MEMPOOL_TLV_FIELD; *value_len = strlen(*value);
        return 1;
    }
    return 0;
}

int main(void) {
    /* init: same C-layer sequence as production minus js/ui */
    kwcc_mempool_init();
    kwcc_io_init();
    kwcc_http_init();
    kwcc_bus_subscribe("http/", on_http_event, NULL);

    /* write fake curl script + make executable */
    FILE *f = fopen(FAKE_SH_PATH, "w");
    TEST(fake_script_written, f != NULL);
    if (!f) return 1;
    fputs(FAKE_SH_CONTENT, f);
    fclose(f);
    chmod(FAKE_SH_PATH, 0755);

    /* set core config: http.bin_path = fake script (hermetic) */
    size_t tlv_len = 0;
    uint8_t *tlv = kwcc_mempool_tlv_build(pack_http_cfg, NULL, &tlv_len);
    TEST(config_tlv_built, tlv != NULL && tlv_len > 0);
    kwcc_config_set_core_tlv("http", tlv, tlv_len);
    free(tlv);

    /* T1: request starts via fake curl */
    const char *rid = kwcc_http_request("GET", "http://fake.invalid/", NULL, 0, NULL, 0);
    TEST(request_started, rid != NULL);

    /* T2 (H1): cancel → slot stays occupied (deferred cleanup, pid kept) */
    kwcc_http_cancel(rid);
    kwcc_http_result_t res;
    TEST(cancel_defers_cleanup_slot_occupied, kwcc_http_get_result(rid, &res) == 0);

    /* T3 (H6): double cancel safe */
    kwcc_http_cancel(rid);
    TEST(double_cancel_safe, kwcc_http_get_result(rid, &res) == 0);

    /* T4 (H4): no end/error dispatched during cancel window */
    TEST(no_end_error_during_cancel, g_end_error_events == 0 && g_cancel_events == 1);

    /* T5-T7 (H2): child exits naturally (fake sleeps 2s) → check_progress reaps silently */
    sleep(3);
    kwcc_http_check_progress();
    TEST(reaped_after_natural_exit, kwcc_http_get_result(rid, &res) == -1);

    /* T8 (H4): reap produced no end/error event */
    TEST(reap_is_silent_no_bus_event, g_end_error_events == 0 && g_cancel_events == 1);

    /* T9: freed slot is reusable */
    const char *rid2 = kwcc_http_request("GET", "http://fake.invalid/", NULL, 0, NULL, 0);
    TEST(freed_slot_reusable, rid2 != NULL);

    /* T10 (H9): shutdown reclaims everything (child of rid2 alive in sleep → SIGTERM) */
    kwcc_http_shutdown();
    TEST(shutdown_reclaims_all, kwcc_http_get_result(rid2, &res) == -1);

    /* T11 (H7): request rejected after shutdown */
    const char *rid3 = kwcc_http_request("GET", "http://fake.invalid/", NULL, 0, NULL, 0);
    TEST(reject_after_shutdown, rid3 == NULL);

    unlink(FAKE_SH_PATH);
    printf("=== test_http_cancel: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
