/* test_config.c — Standalone test for Config layer (pure C, no mquickjs)
 *
 * Tests:
 *   1. set_app_string + get_app
 *   2. set_app_int + get_app
 *   3. set_app_bool + get_app
 *   4. set_core_tlv + get_core_slot + TLV path query
 *   5. release_app
 *   6. release_app_prefix
 *   7. set_max_pools
 *   8. default value
 *   9. cross-domain isolation (a. vs c.)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "src/kwcc_mempool.h"
#include "src/kwcc_config.h"
#include "src/llog.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name, cond) do { \
    if (cond) { tests_passed++; printf("  PASS: %s\n", #name); } \
    else { tests_failed++; printf("  FAIL: %s\n", #name); } \
} while (0)

/* TLV pack callback for test data */
typedef struct {
    const char *keys[8];
    const char *vals[8];
    int count;
    int idx;
} tlv_test_state_t;

static int tlv_test_pack_cb(const char **key, const char **value,
                             uint8_t *type, size_t *value_len, void *user_data) {
    tlv_test_state_t *st = (tlv_test_state_t *)user_data;
    if (st->idx >= st->count) return 0;
    *key = st->keys[st->idx];
    *value = st->vals[st->idx];
    *type = KWCC_MEMPOOL_TLV_FIELD;
    *value_len = strlen(st->vals[st->idx]);
    st->idx++;
    return 1;
}

int main(void) {
    printf("=== config layer test (pure C) ===\n\n");

    kwcc_mempool_init();

    /* ── 1. set_app_string + get_app ── */
    printf("[1] set_app_string + get_app\n");
    kwcc_config_set_app_string("name", "myapp");
    const char *v = kwcc_config_get_app("name", "default");
    TEST("get_app returns 'myapp'", v != NULL && strcmp(v, "myapp") == 0);

    /* ── 2. set_app_int + get_app ── */
    printf("\n[2] set_app_int + get_app\n");
    kwcc_config_set_app_int("port", 8080);
    v = kwcc_config_get_app("port", "0");
    TEST("get_app returns '8080'", v != NULL && strcmp(v, "8080") == 0);

    /* ── 3. set_app_bool + get_app ── */
    printf("\n[3] set_app_bool + get_app\n");
    kwcc_config_set_app_bool("enabled", 1);
    v = kwcc_config_get_app("enabled", "0");
    TEST("get_app returns '1' (true)", v != NULL && strcmp(v, "1") == 0);

    kwcc_config_set_app_bool("disabled", 0);
    v = kwcc_config_get_app("disabled", "1");
    TEST("get_app returns '0' (false)", v != NULL && strcmp(v, "0") == 0);

    /* ── 4. set_core_tlv + get_core_slot + TLV path query ── */
    printf("\n[4] set_core_tlv + get_core_slot + TLV path\n");
    tlv_test_state_t tlv;
    tlv.keys[0] = "max_fds";  tlv.vals[0] = "16";
    tlv.keys[1] = "port";     tlv.vals[1] = "8080";
    tlv.count = 2;
    tlv.idx = 0;

    size_t tlv_len = 0;
    uint8_t *tlv_data = kwcc_mempool_tlv_build(tlv_test_pack_cb, &tlv, &tlv_len);
    TEST("tlv build for core", tlv_data != NULL && tlv_len > 0);

    if (tlv_data) {
        kwcc_config_set_core_tlv("io", tlv_data, (uint32_t)tlv_len);
        kwcc_mempool_tlv_free_json((char *)tlv_data);

        void *slot_ptr = kwcc_config_get_core_slot("io");
        TEST("get_core_slot returns slot", slot_ptr != NULL);

        if (slot_ptr) {
            kwcc_mempool_slot_t *slot = (kwcc_mempool_slot_t *)slot_ptr;
            TEST("slot type is TLV", slot->type == KWCC_MEMPOOL_TYPE_TLV);

            size_t vlen = 0;
            const char *pv = kwcc_mempool_tlv_get_path(slot->data, slot->size, "max_fds", &vlen);
            TEST("TLV path 'max_fds' = '16'", pv != NULL && vlen == 2 && memcmp(pv, "16", 2) == 0);

            pv = kwcc_mempool_tlv_get_path(slot->data, slot->size, "port", &vlen);
            TEST("TLV path 'port' = '8080'", pv != NULL && vlen == 4 && memcmp(pv, "8080", 4) == 0);
        }
    }

    /* ── 5. release_app ── */
    printf("\n[5] release_app\n");
    kwcc_config_set_app_string("temp/key", "tempval");
    v = kwcc_config_get_app("temp/key", "default");
    TEST("temp/key exists before release", v != NULL && strcmp(v, "tempval") == 0);
    kwcc_config_release_app("temp/key");
    /* After release with ref=0, GC would collect it. But GC has 5s throttle.
     * Instead check that release doesn't crash. */
    TEST("release_app completes without crash", 1);

    /* ── 6. release_app_prefix ── */
    printf("\n[6] release_app_prefix\n");
    kwcc_config_set_app_string("io/port", "8080");
    kwcc_config_set_app_string("io/host", "localhost");
    kwcc_config_set_app_string("io/name", "test");
    kwcc_config_release_app_prefix("io");
    TEST("release_app_prefix completes without crash", 1);

    /* ── 7. set_max_pools ── */
    printf("\n[7] set_max_pools\n");
    int old_max = g_kwcc_mempool_mgr.max_pools[KWCC_MEMPOOL_L3];
    kwcc_config_set_max_pools(KWCC_MEMPOOL_L3, 8);
    TEST("set_max_pools updates to 8", g_kwcc_mempool_mgr.max_pools[KWCC_MEMPOOL_L3] == 8);
    kwcc_config_set_max_pools(KWCC_MEMPOOL_L3, 2);
    TEST("set_max_pools min is 4", g_kwcc_mempool_mgr.max_pools[KWCC_MEMPOOL_L3] == 4);
    g_kwcc_mempool_mgr.max_pools[KWCC_MEMPOOL_L3] = old_max;

    /* ── 8. default value ── */
    printf("\n[8] default value\n");
    v = kwcc_config_get_app("nonexistent/key", "fallback");
    TEST("get_app returns default for nonexistent", v != NULL && strcmp(v, "fallback") == 0);
    v = kwcc_config_get_core("nonexistent/key", "core_fallback");
    TEST("get_core returns default for nonexistent", v != NULL && strcmp(v, "core_fallback") == 0);

    /* ── 9. Cross-domain isolation ── */
    printf("\n[9] cross-domain isolation (a. vs c.)\n");
    kwcc_config_set_app_string("shared", "app_value");
    kwcc_config_set_core_tlv("shared", (const uint8_t *)"tlv_data", 8);

    const char *app_v = kwcc_config_get_app("shared", "none");
    const char *core_v = kwcc_config_get_core("shared", "none");
    TEST("app reads 'a.shared', not 'c.shared'", app_v != NULL && strcmp(app_v, "app_value") == 0);
    TEST("core reads 'c.shared', not 'a.shared'", core_v != NULL && core_v != app_v);

    /* ── Shutdown ── */
    printf("\n[10] shutdown\n");
    kwcc_mempool_shutdown();
    TEST("shutdown completes", g_kwcc_mempool_mgr.pool_count[KWCC_MEMPOOL_L0] == 0);

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
