/* test_config_js.c — test $config C handlers (JSValue → C handler → config → mempool) */
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

int main(void) {
    printf("=== $config C handler tests ===\n\n");

    kwcc_mempool_init();

    void *mem_buf = malloc(4 * 1024 * 1024);
    JSContext *ctx = JS_NewContext(mem_buf, 4 * 1024 * 1024, &js_stdlib);
    if (!ctx) { printf("JS_NewContext failed\n"); return 1; }

    /* ── 1. set_app_tlv + get_app_slot + TLV path query ── */
    printf("[1] set_app_tlv + get_app_slot + tlv_get_path\n");
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "timeout", JS_NewString(ctx, "30"));
    JS_SetPropertyStr(ctx, obj, "enabled", JS_NewString(ctx, "true"));

    JSValue sargs[2];
    sargs[0] = JS_NewString(ctx, "test/tlv");
    sargs[1] = obj;
    JSValue sr = kwcc_js_config_set_app_tlv(ctx, NULL, 2, sargs);
    TEST("set_app_tlv no exception", !JS_IsException(sr));

    kwcc_mempool_slot_t *slot = kwcc_config_get_app_slot("test/tlv");
    TEST("slot exists", slot != NULL && slot->data != NULL);
    TEST("slot type is TLV", slot != NULL && slot->type == KWCC_MEMPOOL_TYPE_TLV);
    if (slot && slot->data) {
        size_t vlen = 0;
        const char *val = kwcc_mempool_tlv_get_path(slot->data, slot->size, "timeout", &vlen);
        TEST("get_path('timeout') returns '30'", val != NULL && vlen == 2 && memcmp(val, "30", 2) == 0);
    }

    /* ── 2. get_app_tlv_json ── */
    printf("[2] get_app_tlv_json\n");
    JSValue gargs[1];
    gargs[0] = JS_NewString(ctx, "test/tlv");
    JSValue jr = kwcc_js_config_get_app_tlv_json(ctx, NULL, 1, gargs);
    TEST("get_app_tlv_json no exception", !JS_IsException(jr));
    JSCStringBuf jbuf;
    const char *json = JS_ToCString(ctx, jr, &jbuf);
    TEST("json starts with {", json != NULL && json[0] == '{');

    /* ── 3. set_app_int via C handler ── */
    printf("[3] kwcc_js_config_set_app_int + kwcc_js_config_get_app\n");
    JSValue iargs[2];
    iargs[0] = JS_NewString(ctx, "test/num");
    iargs[1] = JS_NewInt32(ctx, 42);
    kwcc_js_config_set_app_int(ctx, NULL, 2, iargs);

    JSValue igargs[2];
    igargs[0] = JS_NewString(ctx, "test/num");
    igargs[1] = JS_NewString(ctx, "default");
    JSValue ir = kwcc_js_config_get_app(ctx, NULL, 2, igargs);
    JSCStringBuf ibuf;
    const char *istr = JS_ToCString(ctx, ir, &ibuf);
    TEST("get_app returns '42'", istr != NULL && strcmp(istr, "42") == 0);

    printf("\n=== %d passed, %d failed ===\n", tests_passed, tests_failed);

    JS_FreeContext(ctx);
    free(mem_buf);
    kwcc_mempool_shutdown();
    return (tests_failed == 0) ? 0 : 1;
}
