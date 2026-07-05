/* test_config_js.c — Phase 5/6 complete integration tests
 *
 * Covers all 15 test points from Phase 6 spec:
 *   1.  appSetInt → 常量表匹配
 *   2.  appSetString → 正常存储
 *   3.  appSetBool(true) → 常量表引用
 *   4.  appSetBool(false) → 常量表引用
 *   5.  appSetTlv flat → TLV 存储
 *   6.  appSetTlv nested → TLV 存储
 *   7.  appGetTlv(path) flat → 返回值
 *   8.  appGetTlv(path) nested → 返回值
 *   9.  appGetTlv(no path) → JSON 字符串
 *   10. appGet(key, default) → 返回字符串
 *   11. appGet(nonexist, default) → 返回 default
 *   12. appRelease → 释放单个 key
 *   13. appReleasePrefix → 释放前缀所有
 *   14. coreSetTlv → 存 TLV 到 c. 前缀
 *   15. kwcc_mempool_get("c.xxx") → TLV 路径查询（C 模块直接读）
 *   16. setMaxPools → 运行时调整
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mquickjs/mquickjs.h"
#include "mquickjs/mquickjs_priv.h"
#include "src/kwcc_mempool.h"
#include "src/kwcc_config.h"
#include "src/kwcc_js.h"

extern const JSSTDLibraryDef js_stdlib;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name, cond) do { \
    if (cond) { tests_passed++; printf("  PASS: %s\n", #name); } \
    else { tests_failed++; printf("  FAIL: %s\n", #name); } \
} while (0)

static const char* js_str(JSContext *ctx, JSValue v) {
    static __thread char buf[256];
    JSCStringBuf cbuf;
    const char *s = JS_ToCString(ctx, v, &cbuf);
    if (!s) return "(null)";
    snprintf(buf, sizeof(buf), "%s", s);
    return buf;
}

int main(void) {
    printf("=== Phase 5/6 $config complete integration tests ===\n\n");
    kwcc_mempool_init();

    void *mem_buf = malloc(4 * 1024 * 1024);
    JSContext *ctx = JS_NewContext(mem_buf, 4 * 1024 * 1024, &js_stdlib);
    if (!ctx) { printf("JS_NewContext failed\n"); return 1; }

    /* ── 1. appSetInt → 常量表匹配 ── */
    printf("[1] appSetInt → const table\n");
    JSValue s1[2];
    s1[0] = JS_NewString(ctx, "test/int_val");
    s1[1] = JS_NewInt32(ctx, 42);
    JSValue r1 = kwcc_js_config_set_app_int(ctx, NULL, 2, s1);
    TEST("set_app_int(42) no exception", !JS_IsException(r1));

    kwcc_mempool_slot_t *slot_int = kwcc_config_get_app_slot("test/int_val");
    TEST("slot_int exists", slot_int != NULL);
    TEST("slot_int type is INT32", slot_int && slot_int->type == KWCC_MEMPOOL_TYPE_INT32);

    JSValue g1[2];
    g1[0] = JS_NewString(ctx, "test/int_val");
    g1[1] = JS_NewString(ctx, "0");
    JSValue gr1 = kwcc_js_config_get_app(ctx, NULL, 2, g1);
    TEST("get_app returns '42'", strcmp(js_str(ctx, gr1), "42") == 0);

    /* ── 2. appSetString → 正常存储 ── */
    printf("[2] appSetString\n");
    JSValue s2[2];
    s2[0] = JS_NewString(ctx, "test/name");
    s2[1] = JS_NewString(ctx, "myapp");
    kwcc_js_config_set_app_string(ctx, NULL, 2, s2);

    const char *v = kwcc_config_get_app("test/name", "default");
    TEST("get_app returns 'myapp'", v != NULL && strcmp(v, "myapp") == 0);

    /* ── 3. appSetBool(true) → 常量表 ── */
    printf("[3] appSetBool(true) → const table\n");
    JSValue s3[2];
    s3[0] = JS_NewString(ctx, "test/enabled");
    s3[1] = JS_NewInt32(ctx, 1); /* JS_TRUE */
    kwcc_js_config_set_app_bool(ctx, NULL, 2, s3);

    kwcc_mempool_slot_t *slot_bool = kwcc_config_get_app_slot("test/enabled");
    TEST("bool slot type is CONST (true)", slot_bool && slot_bool->type == KWCC_MEMPOOL_TYPE_CONST);

    /* ── 4. appSetBool(false) → 常量表 ── */
    printf("[4] appSetBool(false) → const table\n");
    JSValue s4[2];
    s4[0] = JS_NewString(ctx, "test/disabled");
    s4[1] = JS_NewInt32(ctx, 0);
    kwcc_js_config_set_app_bool(ctx, NULL, 2, s4);

    const char *bool_v = kwcc_config_get_app("test/disabled", "1");
    TEST("get_app(false) returns '0'", bool_v != NULL && strcmp(bool_v, "0") == 0);

    /* ── 5. appSetTlv flat ── */
    printf("[5] appSetTlv flat\n");
    JSValue obj5 = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj5, "port", JS_NewString(ctx, "8080"));
    JS_SetPropertyStr(ctx, obj5, "host", JS_NewString(ctx, "localhost"));
    JSValue s5[2];
    s5[0] = JS_NewString(ctx, "test/tlv_flat");
    s5[1] = obj5;
    JSValue sr5 = kwcc_js_config_set_app_tlv(ctx, NULL, 2, s5);
    TEST("set_app_tlv(flat) no exception", !JS_IsException(sr5));

    kwcc_mempool_slot_t *slot5 = kwcc_config_get_app_slot("test/tlv_flat");
    TEST("tlv_flat slot exists", slot5 != NULL && slot5->data != NULL);
    TEST("tlv_flat type is TLV", slot5 && slot5->type == KWCC_MEMPOOL_TYPE_TLV);

    /* ── 6. appSetTlv nested ── */
    printf("[6] appSetTlv nested\n");
    JSValue inner = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, inner, "user", JS_NewString(ctx, "v1"));
    JS_SetPropertyStr(ctx, inner, "enabled", JS_NewString(ctx, "true"));
    JSValue obj6 = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj6, "timeout", inner);
    JS_SetPropertyStr(ctx, obj6, "version", JS_NewString(ctx, "2.0"));
    JSValue s6[2];
    s6[0] = JS_NewString(ctx, "test/tlv_nested");
    s6[1] = obj6;
    JSValue sr6 = kwcc_js_config_set_app_tlv(ctx, NULL, 2, s6);
    TEST("set_app_tlv(nested) no exception", !JS_IsException(sr6));

    kwcc_mempool_slot_t *slot6 = kwcc_config_get_app_slot("test/tlv_nested");
    TEST("tlv_nested slot exists", slot6 != NULL && slot6->data != NULL);
    TEST("tlv_nested type is TLV", slot6 && slot6->type == KWCC_MEMPOOL_TYPE_TLV);

    /* ── 7. appGetTlv(path) flat ── */
    printf("[7] appGetTlv(path) flat\n");
    JSValue g7[2];
    g7[0] = JS_NewString(ctx, "test/tlv_flat");
    g7[1] = JS_NewString(ctx, "port");
    JSValue gr7 = kwcc_js_config_get_app_tlv_path(ctx, NULL, 2, g7);
    TEST("get_tlv_path('port') returns '8080'", strcmp(js_str(ctx, gr7), "8080") == 0);

    /* ── 8. appGetTlv(path) nested ── */
    printf("[8] appGetTlv(path) nested\n");
    JSValue g8[2];
    g8[0] = JS_NewString(ctx, "test/tlv_nested");
    g8[1] = JS_NewString(ctx, "timeout/user");
    JSValue gr8 = kwcc_js_config_get_app_tlv_path(ctx, NULL, 2, g8);
    TEST("get_tlv_path('timeout/user') returns 'v1'", strcmp(js_str(ctx, gr8), "v1") == 0);

    /* ── 9. appGetTlv(no path) → JSON ── */
    printf("[9] appGetTlv(no path) → JSON\n");
    JSValue g9[1];
    g9[0] = JS_NewString(ctx, "test/tlv_flat");
    JSValue gr9 = kwcc_js_config_get_app_tlv_json(ctx, NULL, 1, g9);
    const char *json9 = js_str(ctx, gr9);
    TEST("get_tlv_json starts with {", json9[0] == '{');
    TEST("get_tlv_json contains 'port'", strstr(json9, "port") != NULL);

    /* ── 10. appGet(key) → 返回值 ── */
    printf("[10] appGet(key)\n");
    JSValue g10[2];
    g10[0] = JS_NewString(ctx, "test/name");
    g10[1] = JS_NewString(ctx, "default");
    JSValue gr10 = kwcc_js_config_get_app(ctx, NULL, 2, g10);
    TEST("get_app('name') returns 'myapp'", strcmp(js_str(ctx, gr10), "myapp") == 0);

    /* ── 11. appGet(nonexist, default) → 返回 default ── */
    printf("[11] appGet(nonexist, default)\n");
    JSValue g11[2];
    g11[0] = JS_NewString(ctx, "test/nonexistent");
    g11[1] = JS_NewString(ctx, "fallback");
    JSValue gr11 = kwcc_js_config_get_app(ctx, NULL, 2, g11);
    TEST("get_app(nonexist) returns 'fallback'", strcmp(js_str(ctx, gr11), "fallback") == 0);

    /* ── 12. appRelease → 释放单个 key ── */
    printf("[12] appRelease\n");
    JSValue g12[1];
    g12[0] = JS_NewString(ctx, "test/int_val");
    JSValue gr12 = kwcc_js_config_release_app(ctx, NULL, 1, g12);
    TEST("appRelease no exception", !JS_IsException(gr12));
    /* ref_count-- 后 slot 还在（GC 未运行），只验证不崩溃 */

    /* ── 13. appReleasePrefix → 释放前缀所有 ── */
    printf("[13] appReleasePrefix\n");
    /* Create prefixed keys */
    JSValue sp13[2];
    sp13[0] = JS_NewString(ctx, "prefix/key1");
    sp13[1] = JS_NewString(ctx, "val1");
    kwcc_js_config_set_app_string(ctx, NULL, 2, sp13);
    JSValue sp13b[2];
    sp13b[0] = JS_NewString(ctx, "prefix/key2");
    sp13b[1] = JS_NewString(ctx, "val2");
    kwcc_js_config_set_app_string(ctx, NULL, 2, sp13b);

    JSValue g13[1];
    g13[0] = JS_NewString(ctx, "prefix");
    JSValue gr13 = kwcc_js_config_release_app_prefix(ctx, NULL, 1, g13);
    TEST("appReleasePrefix no exception", !JS_IsException(gr13));

    /* ── 14. coreSetTlv → 存 TLV 到 c. 前缀 ── */
    printf("[14] coreSetTlv\n");
    JSValue obj14 = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj14, "max_fds", JS_NewString(ctx, "16"));
    JS_SetPropertyStr(ctx, obj14, "port", JS_NewString(ctx, "8080"));
    JSValue s14[2];
    s14[0] = JS_NewString(ctx, "test/io");
    s14[1] = obj14;
    JSValue sr14 = kwcc_js_config_set_core_tlv(ctx, NULL, 2, s14);
    TEST("core_set_tlv no exception", !JS_IsException(sr14));

    /* ── 15. kwcc_mempool_get("c.xxx") → TLV 路径查询（C 模块直接读）── */
    printf("[15] C module reads Core TLV via mempool\n");
    kwcc_mempool_slot_t *slot_core = kwcc_mempool_get("c.test/io");
    TEST("core slot exists", slot_core != NULL && slot_core->data != NULL);
    TEST("core slot type is TLV", slot_core && slot_core->type == KWCC_MEMPOOL_TYPE_TLV);
    if (slot_core && slot_core->data) {
        size_t vlen = 0;
        const char *val = kwcc_mempool_tlv_get_path(slot_core->data, slot_core->size, "max_fds", &vlen, NULL);
        TEST("tlv_get_path('max_fds') returns '16'", val != NULL && vlen == 2 && memcmp(val, "16", 2) == 0);
    }

    /* ── 16. setMaxPools ── */
    printf("[16] setMaxPools\n");
    JSValue s16[2];
    s16[0] = JS_NewString(ctx, "l5");
    s16[1] = JS_NewInt32(ctx, 4);
    JSValue sr16 = kwcc_js_config_set_max_pools(ctx, NULL, 2, s16);
    TEST("setMaxPools('l5', 4) no exception", !JS_IsException(sr16));

    /* ── Summary ── */
    printf("\n=== %d passed, %d failed, %d total ===\n", tests_passed, tests_failed, tests_passed + tests_failed);

    JS_FreeContext(ctx);
    free(mem_buf);
    kwcc_mempool_shutdown();
    return (tests_failed == 0) ? 0 : 1;
}
