/* test_mempool.c — Standalone test for mempool layers 1-3 (pure C, no mquickjs)
 *
 * Tests:
 *   1. init/shutdown
 *   2. alloc + set + get (string, int)
 *   3. const table lookup
 *   4. key_map lookup
 *   5. pool expansion
 *   6. acquire/release/ref_count
 *   7. release
 *   8. get_keys prefix scan
 *   9. TLV build + iter
 *  10. TLV get_path
 *  11. TLV to_json (including escaping)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "src/kwcc_mempool.h"
#include "src/llog.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name, cond) do { \
    if (cond) { tests_passed++; printf("  PASS: %s\n", #name); } \
    else { tests_failed++; printf("  FAIL: %s\n", #name); } \
} while (0)

/* ── TLV pack callback test helper ── */

typedef struct {
    const char *keys[16];
    const char *vals[16];
    uint8_t     types[16];
    int count;
    int idx;
} test_pack_state_t;

static int test_pack_cb(const char **key, const char **value,
                         uint8_t *type, size_t *value_len, void *user_data) {
    test_pack_state_t *st = (test_pack_state_t *)user_data;
    if (st->idx >= st->count) return 0;
    *key = st->keys[st->idx];
    *value = st->vals[st->idx];
    *type = st->types[st->idx];
    *value_len = strlen(st->vals[st->idx]);
    st->idx++;
    return 1;
}

/* TLV iter callback: count entries */
static int test_iter_cb(const char *name, const uint8_t *value,
                         size_t value_len, uint8_t type, void *user_data) {
    int *count = (int *)user_data;
    (*count)++;
    return 1;
}

int main(void) {
    printf("=== mempool test: layer 1-3 (pure C) ===\n\n");

    /* ── 1. Init ── */
    printf("[1] init\n");
    kwcc_mempool_init();
    TEST("initialized", g_kwcc_mempool_mgr.pool_count[KWCC_MEMPOOL_L0] >= 1);
    TEST("L7 initialized", g_kwcc_mempool_mgr.pool_count[KWCC_MEMPOOL_L7] >= 1);

    /* ── 2. Alloc + set + get (string) ── */
    printf("\n[2] alloc + set + get (string)\n");
    kwcc_mempool_slot_t *s1 = kwcc_mempool_alloc(KWCC_MEMPOOL_TYPE_STRING, "test/name", 6, 0);
    TEST("alloc returns slot", s1 != NULL);
    if (s1) {
        const char *val = "hello";
        kwcc_mempool_set(s1, val, 6);
        TEST("set size", s1->size == 6);
        TEST("set content", memcmp(s1->data, "hello", 6) == 0);

        kwcc_mempool_slot_t *s1b = kwcc_mempool_get("test/name");
        TEST("get returns same slot", s1b == s1);
    }

    /* ── 3. Const table ── */
    printf("\n[3] const table\n");
    int32_t one = 1;
    int ci = kwcc_mempool_const_lookup(&one, sizeof(one), KWCC_MEMPOOL_TYPE_INT32);
    /* Index 6 (CONST_TRUE_BOOL) matches before index 10 (CONST_1_INT) because both are INT32=1 */
    TEST("const lookup finds 1 (or true_bool)", ci == KWCC_MEMPOOL_CONST_TRUE_BOOL || ci == KWCC_MEMPOOL_CONST_1_INT);

    const char *true_str = "true";
    ci = kwcc_mempool_const_lookup(true_str, 5, KWCC_MEMPOOL_TYPE_STRING);
    TEST("const lookup finds 'true'", ci == KWCC_MEMPOOL_CONST_TRUE);

    /* ── 4. set with const match ── */
    printf("\n[4] set with const match\n");
    kwcc_mempool_slot_t *s2 = kwcc_mempool_alloc(KWCC_MEMPOOL_TYPE_INT32, "test/one", 4, 0);
    TEST("alloc int slot", s2 != NULL);
    if (s2) {
        int32_t val = 1;
        kwcc_mempool_set(s2, &val, sizeof(val));
        TEST("set 1 → CONST type", s2->type == KWCC_MEMPOOL_TYPE_CONST);
        TEST("set 1 → const table value", *(int32_t *)s2->data == 1);
    }

    /* ── 5. get_str ── */
    printf("\n[5] get_str\n");
    const char *str = kwcc_mempool_get_str("test/name", "default");
    TEST("get_str returns value", str != NULL && strcmp(str, "hello") == 0);
    str = kwcc_mempool_get_str("nonexistent", "default");
    TEST("get_str returns default", str != NULL && strcmp(str, "default") == 0);

    /* ── 6. ref_count initial value = 1 ── */
    printf("\n[6] ref_count initial value\n");
    s1 = kwcc_mempool_get("test/name");
    if (s1) {
        TEST("ref_count starts at 1 (not 0)", s1->ref_count == 1);
        kwcc_mempool_acquire(s1);
        TEST("acquire increments", s1->ref_count == 2);
        kwcc_mempool_release(s1);
        TEST("release decrements", s1->ref_count == 1);
    }

    /* ── 7. Multiple allocs ── */
    printf("\n[7] multiple allocs\n");
    kwcc_mempool_slot_t *s3 = kwcc_mempool_alloc(KWCC_MEMPOOL_TYPE_STRING, "test/key1", 10, 0);
    kwcc_mempool_slot_t *s4 = kwcc_mempool_alloc(KWCC_MEMPOOL_TYPE_STRING, "test/key2", 10, 0);
    kwcc_mempool_slot_t *s5 = kwcc_mempool_alloc(KWCC_MEMPOOL_TYPE_STRING, "test/key3", 10, 0);
    TEST("alloc 3 slots", s3 && s4 && s5);
    if (s3) { kwcc_mempool_set(s3, "val1", 5); }
    if (s4) { kwcc_mempool_set(s4, "val2", 5); }
    if (s5) { kwcc_mempool_set(s5, "val3", 5); }

    /* ── 8. get_keys prefix scan ── */
    printf("\n[8] get_keys prefix scan\n");
    const char *keys[32];
    int n = kwcc_mempool_get_keys("test/key", keys, 32);
    TEST("prefix finds 3 keys", n == 3);

    n = kwcc_mempool_get_keys("nonexist", keys, 32);
    TEST("prefix finds 0 for nonexistent", n == 0);

    /* ── 9. TLV build + iter ── */
    printf("\n[9] TLV build + iter\n");
    test_pack_state_t pack;
    pack.keys[0] = "name";   pack.vals[0] = "myapp";    pack.types[0] = KWCC_MEMPOOL_TLV_FIELD;
    pack.keys[1] = "port";   pack.vals[1] = "8080";     pack.types[1] = KWCC_MEMPOOL_TLV_FIELD;
    pack.keys[2] = "enabled"; pack.vals[2] = "true";    pack.types[2] = KWCC_MEMPOOL_TLV_FIELD;
    pack.count = 3;
    pack.idx = 0;

    size_t tlv_len = 0;
    uint8_t *tlv = kwcc_mempool_tlv_build(test_pack_cb, &pack, &tlv_len);
    TEST("tlv build returns data", tlv != NULL && tlv_len > 0);

    if (tlv) {
        int entry_count = 0;
        int iter_result = kwcc_mempool_tlv_iter(tlv, tlv_len, test_iter_cb, &entry_count);
        TEST("tlv iter finds 3 entries", iter_result == 3 && entry_count == 3);

        /* ── 10. TLV get_path ── */
        printf("\n[10] TLV get_path\n");
        size_t vlen = 0;
        const char *v = kwcc_mempool_tlv_get_path(tlv, tlv_len, "name", &vlen);
        TEST("get_path('name')", v != NULL && vlen == 5 && memcmp(v, "myapp", 5) == 0);

        v = kwcc_mempool_tlv_get_path(tlv, tlv_len, "port", &vlen);
        TEST("get_path('port')", v != NULL && vlen == 4 && memcmp(v, "8080", 4) == 0);

        v = kwcc_mempool_tlv_get_path(tlv, tlv_len, "nonexist", &vlen);
        TEST("get_path('nonexist') returns NULL", v == NULL);

        /* ── 11. TLV to_json ── */
        printf("\n[11] TLV to_json\n");
        size_t json_len = 0;
        char *json = kwcc_mempool_tlv_to_json(tlv, tlv_len, &json_len);
        TEST("tlv to_json returns string", json != NULL && json_len > 0);
        if (json) {
            printf("  JSON: %s\n", json);
            TEST("JSON starts with {", json[0] == '{');
            TEST("JSON ends with }", json[json_len - 1] == '}');
            TEST("JSON contains 'name'", strstr(json, "\"name\":") != NULL);
        }
        kwcc_mempool_tlv_free_json(json);
        kwcc_mempool_tlv_free_json((char *)tlv);
    }

    /* ── 12. TLV to_json with escaping ── */
    printf("\n[12] TLV to_json escaping\n");
    test_pack_state_t esc;
    esc.keys[0] = "msg";    esc.vals[0] = "he said \"hi\""; esc.types[0] = KWCC_MEMPOOL_TLV_FIELD;
    esc.keys[1] = "path";   esc.vals[1] = "c:\\dir\\file";  esc.types[1] = KWCC_MEMPOOL_TLV_FIELD;
    esc.count = 2;
    esc.idx = 0;

    uint8_t *tlv2 = kwcc_mempool_tlv_build(test_pack_cb, &esc, &tlv_len);
    size_t json_len2 = 0;
    TEST("tlv build with special chars", tlv2 != NULL);
    if (tlv2) {
        char *json2 = kwcc_mempool_tlv_to_json(tlv2, tlv_len, &json_len2);
        TEST("tlv to_json with escaping", json2 != NULL);
        if (json2) {
            printf("  JSON: %s\n", json2);
            /* Check that quotes are escaped: \" */
            TEST("JSON escapes quotes", strstr(json2, "\\\"") != NULL);
            /* Check that backslashes are escaped: \\ */
            TEST("JSON escapes backslash", strstr(json2, "\\\\") != NULL);
        }
        kwcc_mempool_tlv_free_json(json2);
        kwcc_mempool_tlv_free_json((char *)tlv2);
    }

    /* ── 13. L7 dynamic alloc ── */
    printf("\n[13] L7 dynamic alloc (>16KB)\n");
    kwcc_mempool_slot_t *s6 = kwcc_mempool_alloc(KWCC_MEMPOOL_TYPE_STRING, "test/large", 20000, 0);
    TEST("alloc >16KB goes to L7", s6 != NULL && s6->pool_type == KWCC_MEMPOOL_L7);
    if (s6) {
        TEST("L7 capacity >= 20000", s6->capacity >= 20000);
    }

    /* ── 14. set_max_pools ── */
    printf("\n[14] set_max_pools\n");
    int old_max = g_kwcc_mempool_mgr.max_pools[KWCC_MEMPOOL_L3];
    kwcc_mempool_set_max_pools(KWCC_MEMPOOL_L3, 8);
    TEST("set_max_pools updates", g_kwcc_mempool_mgr.max_pools[KWCC_MEMPOOL_L3] == 8);
    kwcc_mempool_set_max_pools(KWCC_MEMPOOL_L3, 2);
    TEST("set_max_pools min is 4", g_kwcc_mempool_mgr.max_pools[KWCC_MEMPOOL_L3] == 4);
    g_kwcc_mempool_mgr.max_pools[KWCC_MEMPOOL_L3] = old_max;

    /* ── 15. ref_count + GC ── */
    printf("\n[15] ref_count + GC\n");
    /* alloc → ref=1 → GC should NOT collect */
    kwcc_mempool_alloc(KWCC_MEMPOOL_TYPE_STRING, "test/gc_keep", 20, 0);
    kwcc_mempool_slot_t *s_gc = kwcc_mempool_get("test/gc_keep");
    TEST("alloc ref=1", s_gc != NULL && s_gc->ref_count == 1);
    kwcc_mempool_gc_force();
    s_gc = kwcc_mempool_get("test/gc_keep");
    TEST("GC preserves ref=1 slot", s_gc != NULL);

    /* alloc → release → ref=0 → GC SHOULD collect */
    kwcc_mempool_slot_t *s_rel = kwcc_mempool_alloc(KWCC_MEMPOOL_TYPE_STRING, "test/gc_drop", 20, 0);
    TEST("alloc ref=1", s_rel != NULL && s_rel->ref_count == 1);
    if (s_rel) kwcc_mempool_release(s_rel);
    s_rel = kwcc_mempool_get("test/gc_drop");
    TEST("after release ref=0", s_rel != NULL && s_rel->ref_count == 0);
    kwcc_mempool_gc_force();
    s_rel = kwcc_mempool_get("test/gc_drop");
    TEST("GC collects ref=0 slot", s_rel == NULL);

    /* ── 16. TLV boundary check ── */
    printf("\n[16] TLV boundary check\n");
    /* Nested object path: get_path into non-existent sub-object */
    test_pack_state_t simple;
    simple.keys[0] = "flat";  simple.vals[0] = "value"; simple.types[0] = KWCC_MEMPOOL_TLV_FIELD;
    simple.count = 1;
    simple.idx = 0;
    uint8_t *flat_tlv = kwcc_mempool_tlv_build(test_pack_cb, &simple, &tlv_len);
    TEST("flat TLV builds", flat_tlv != NULL);
    if (flat_tlv) {
        /* Path query on flat TLV for non-existent nested path */
        size_t vlen = 0;
        const char *v = kwcc_mempool_tlv_get_path(flat_tlv, tlv_len, "nested/key", &vlen);
        TEST("get_path returns NULL for non-existent nested path", v == NULL);
        kwcc_mempool_tlv_free_json((char *)flat_tlv);
    }

    /* Truncated TLV data: force error code */
    uint8_t truncated[2] = { KWCC_MEMPOOL_TLV_FIELD, 0x10 };  /* missing length byte + data */
    int iter_result = kwcc_mempool_tlv_iter(truncated, sizeof(truncated), test_iter_cb, NULL);
    TEST("truncated TLV returns error code", iter_result < 0);

    /* ── 17. Shutdown ── */
    printf("\n[17] shutdown\n");
    kwcc_mempool_shutdown();
    TEST("shutdown completes", g_kwcc_mempool_mgr.pool_count[KWCC_MEMPOOL_L0] == 0);
    TEST("L7 usage zeroed", g_kwcc_mempool_l7_used == 0);

    /* ── Summary ── */
    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
