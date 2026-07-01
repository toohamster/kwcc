/* test_js_ops.c — kwcc_js_ops_t ABI contract tests
 *
 * Verifies each ops function pointer as an internal ABI contract.
 * Also verifies the module-grouped dispatch mechanism.
 *
 * 9 test categories (~60 test points):
 *   1. new_object + set_str_prop + get_str_prop
 *   2. new_int32 + to_int32
 *   3. new_string + new_string_len + to_cstring
 *   4. call_cb (call JS function, pass args)
 *   5. is_function / is_undefined / is_null / is_exception + get_class_id
 *   6. eval + JS_EVAL_REPL / JS_EVAL_RETVAL
 *   7. notify_js + ack_cleanup
 *   8. array_length + array_get
 *   9. dispatch: kwcc_js_call_c + kwcc_js_dispatch_call
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mquickjs/mquickjs.h"
#include "mquickjs/mquickjs_priv.h"
#include "src/kwcc_js.h"
#include "src/kwcc_mempool.h"

extern const JSSTDLibraryDef js_stdlib;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name, cond) do { \
    if (cond) { tests_passed++; printf("  PASS: %s\n", #name); } \
    else { tests_failed++; printf("  FAIL: %s\n", #name); } \
} while (0)

/* ── File-scope test helpers (C does not allow nested function definitions) ── */

/* ack_cleanup test state */
static int g_kwcc_test_ack_called = 0;
static char g_kwcc_test_ack_id[64];

static void kwcc_test_ack_cleanup(const char *id) {
    g_kwcc_test_ack_called = 1;
    if (id) {
        strncpy(g_kwcc_test_ack_id, id, sizeof(g_kwcc_test_ack_id) - 1);
        g_kwcc_test_ack_id[sizeof(g_kwcc_test_ack_id) - 1] = '\0';
    }
}

/* dispatch test: ops-signature handler that stores argc/arg0 in JS globals */
static kwcc_js_val_t test_dispatch_echo(kwcc_js_ops_t *ops, int argc, kwcc_js_val_t *argv) {
    ops->set_str_prop(ops, ops->global_obj, "kwcc_dispatch_argc",
                      ops->new_int32(ops, argc));
    if (argc >= 1) {
        kwcc_js_cstr_buf_t buf;
        const char *s = ops->to_cstring(ops, argv[0], &buf);
        if (s) {
            ops->set_str_prop(ops, ops->global_obj, "kwcc_dispatch_arg0",
                              ops->new_string(ops, s));
        }
    }
    return ops->new_string(ops, "dispatch_ok");
}

/* dispatch test: sum handler — tests argv extraction + to_int32 + return */
static kwcc_js_val_t test_dispatch_sum(kwcc_js_ops_t *ops, int argc, kwcc_js_val_t *argv) {
    if (argc < 2) return ops->new_int32(ops, 0);
    int a = ops->to_int32(ops, argv[0]);
    int b = ops->to_int32(ops, argv[1]);
    return ops->new_int32(ops, a + b);
}

/* dispatch test: alternative echo handler for overwrite test */
static kwcc_js_val_t test_dispatch_echo_v2(kwcc_js_ops_t *ops, int argc, kwcc_js_val_t *argv) {
    (void)argc; (void)argv;
    return ops->new_string(ops, "dispatch_ok_v2");
}

static const kwcc_js_api_t test_dispatch_apis[] = {
    { "echo", test_dispatch_echo },
    { "sum", test_dispatch_sum },
    { NULL, NULL }
};

static kwcc_js_module_t test_dispatch_mod = {
    "testmod",
    NULL,               /* load */
    test_dispatch_apis, /* apis */
    NULL,               /* on_bus_event */
    NULL                /* unload */
};

/* ── Main ── */

int main(void) {
    printf("=== kwcc_js_ops_t ABI contract tests ===\n\n");

    /* Setup: create JSContext + init ops + inject $notify */
    kwcc_mempool_init();

    void *mem_buf = malloc(4 * 1024 * 1024);
    JSContext *ctx = JS_NewContext(mem_buf, 4 * 1024 * 1024, &js_stdlib);
    if (!ctx) { printf("JS_NewContext failed\n"); return 1; }

    kwcc_js_ops_init(ctx);
    kwcc_js_ops_t *ops = &g_kwcc_js_ops;

    /* Register $notify + core dispatch entries */
    kwcc_js_register_modules(ops);

    /* ════════════════════════════════════════════════════════════════
     * 1. new_object + set_str_prop + get_str_prop
     * ════════════════════════════════════════════════════════════════ */
    printf("[1] new_object + set_str_prop + get_str_prop\n");
    kwcc_js_val_t obj1 = ops->new_object(ops);
    TEST("1a: new_object returns non-exception", !ops->is_exception(obj1));

    ops->set_str_prop(ops, obj1, "greeting", ops->new_string(ops, "hello"));
    kwcc_js_val_t got1 = ops->get_str_prop(ops, obj1, "greeting");
    kwcc_js_cstr_buf_t buf1;
    const char *str1 = ops->to_cstring(ops, got1, &buf1);
    TEST("1b: get_str_prop returns 'hello'", str1 != NULL && strcmp(str1, "hello") == 0);

    kwcc_js_val_t got1b = ops->get_str_prop(ops, obj1, "nonexistent");
    TEST("1c: get_str_prop(nonexistent) is undefined", ops->is_undefined(got1b));

    ops->set_str_prop(ops, obj1, "greeting", ops->new_string(ops, "world"));
    kwcc_js_val_t got1c = ops->get_str_prop(ops, obj1, "greeting");
    kwcc_js_cstr_buf_t buf1c;
    TEST("1d: set_str_prop overwrites existing",
         ops->to_cstring(ops, got1c, &buf1c) != NULL &&
         strcmp(ops->to_cstring(ops, got1c, &buf1c), "world") == 0);

    ops->set_str_prop(ops, obj1, "count", ops->new_int32(ops, 42));
    kwcc_js_val_t got1d = ops->get_str_prop(ops, obj1, "count");
    TEST("1e: set_str_prop with int value", ops->to_int32(ops, got1d) == 42);

    kwcc_js_val_t json_obj = ops->get_str_prop(ops, ops->global_obj, "JSON");
    TEST("1f: get_str_prop(global, 'JSON') is not undefined", !ops->is_undefined(json_obj));

    kwcc_js_val_t obj1b = ops->new_object(ops);
    ops->set_str_prop(ops, obj1b, "x", ops->new_int32(ops, 1));
    kwcc_js_val_t got1e = ops->get_str_prop(ops, obj1b, "x");
    kwcc_js_val_t got1f = ops->get_str_prop(ops, obj1, "x");
    TEST("1g: separate objects are independent",
         ops->to_int32(ops, got1e) == 1 && ops->is_undefined(got1f));

    /* ════════════════════════════════════════════════════════════════
     * 2. new_int32 + to_int32
     * ════════════════════════════════════════════════════════════════ */
    printf("[2] new_int32 + to_int32\n");
    TEST("2a: new_int32(0) roundtrip", ops->to_int32(ops, ops->new_int32(ops, 0)) == 0);
    TEST("2b: new_int32(-99) roundtrip", ops->to_int32(ops, ops->new_int32(ops, -99)) == -99);
    TEST("2c: new_int32(12345) roundtrip", ops->to_int32(ops, ops->new_int32(ops, 12345)) == 12345);
    TEST("2d: new_int32(INT32_MAX) roundtrip",
         ops->to_int32(ops, ops->new_int32(ops, 2147483647)) == 2147483647);
    TEST("2e: new_int32(INT32_MIN) roundtrip",
         ops->to_int32(ops, ops->new_int32(ops, -2147483648)) == -2147483648);

    kwcc_js_val_t str_num2 = ops->eval(ops, "'123'", 5, "<test2f>", JS_EVAL_RETVAL);
    TEST("2f: to_int32 on JS string '123' = 123", ops->to_int32(ops, str_num2) == 123);

    /* ════════════════════════════════════════════════════════════════
     * 3. new_string + new_string_len + to_cstring
     * ════════════════════════════════════════════════════════════════ */
    printf("[3] new_string + new_string_len + to_cstring\n");

    kwcc_js_cstr_buf_t sbuf3;
    TEST("3a: short string 'abc' roundtrip",
         (ops->to_cstring(ops, ops->new_string(ops, "abc"), &sbuf3) != NULL &&
          strcmp(ops->to_cstring(ops, ops->new_string(ops, "abc"), &sbuf3), "abc") == 0));

    const char *long_text = "This is a much longer string that definitely exceeds the 5-byte inline buffer";
    kwcc_js_cstr_buf_t lbuf3;
    const char *lstr3 = ops->to_cstring(ops, ops->new_string(ops, long_text), &lbuf3);
    TEST("3b: long string roundtrip", lstr3 != NULL && strcmp(lstr3, long_text) == 0);

    kwcc_js_cstr_buf_t lenbuf3;
    const char *lstr3b = ops->to_cstring(ops, ops->new_string_len(ops, "hello world", 5), &lenbuf3);
    TEST("3c: new_string_len truncates to 'hello'", lstr3b != NULL && strcmp(lstr3b, "hello") == 0);

    kwcc_js_cstr_buf_t ebuf3;
    const char *estr3 = ops->to_cstring(ops, ops->new_string(ops, ""), &ebuf3);
    TEST("3d: empty string roundtrip", estr3 != NULL && strcmp(estr3, "") == 0);

    kwcc_js_cstr_buf_t zbuf3;
    const char *zstr3 = ops->to_cstring(ops, ops->new_string_len(ops, "anything", 0), &zbuf3);
    TEST("3e: new_string_len(, 0) = empty string", zstr3 != NULL && strcmp(zstr3, "") == 0);

    kwcc_js_cstr_buf_t ibuf3;
    const char *istr3 = ops->to_cstring(ops, ops->new_int32(ops, 42), &ibuf3);
    TEST("3f: to_cstring on int 42 = '42'", istr3 != NULL && strcmp(istr3, "42") == 0);

    kwcc_js_cstr_buf_t cbuf3;
    TEST("3g: single char 'X' roundtrip",
         (ops->to_cstring(ops, ops->new_string(ops, "X"), &cbuf3) != NULL &&
          strcmp(ops->to_cstring(ops, ops->new_string(ops, "X"), &cbuf3), "X") == 0));

    /* ════════════════════════════════════════════════════════════════
     * 4. call_cb
     * ════════════════════════════════════════════════════════════════ */
    printf("[4] call_cb\n");

    /* 4a: call no-arg function */
    ops->eval(ops, "var kwcc_cb_called = 0;", 20, "<test4a>", JS_EVAL_REPL);
    kwcc_js_val_t fn4a = ops->eval(ops,
        "(function() { kwcc_cb_called = 1; })",
        35, "<test4a>", JS_EVAL_RETVAL);
    TEST("4a: call_cb creates function", ops->is_function(ops, fn4a));
    ops->call_cb(ops, fn4a, 0, NULL);
    kwcc_js_val_t called4a = ops->get_str_prop(ops, ops->global_obj, "kwcc_cb_called");
    TEST("4a: call_cb executed function", ops->to_int32(ops, called4a) == 1);

    /* 4b: call with args */
    ops->eval(ops, "var kwcc_cb_arg = '';", 22, "<test4b>", JS_EVAL_REPL);
    kwcc_js_val_t fn4b = ops->eval(ops,
        "(function(x) { kwcc_cb_arg = x; })",
        38, "<test4b>", JS_EVAL_RETVAL);
    kwcc_js_val_t arg4b = ops->new_string(ops, "test_value");
    ops->call_cb(ops, fn4b, 1, &arg4b);
    kwcc_js_val_t got4b = ops->get_str_prop(ops, ops->global_obj, "kwcc_cb_arg");
    kwcc_js_cstr_buf_t bbuf4b;
    TEST("4b: call_cb passed argument",
         ops->to_cstring(ops, got4b, &bbuf4b) != NULL &&
         strcmp(ops->to_cstring(ops, got4b, &bbuf4b), "test_value") == 0);

    /* 4c: call function with return value */
    ops->eval(ops, "var kwcc_cb_ret = '';", 23, "<test4c>", JS_EVAL_REPL);
    kwcc_js_val_t fn4c = ops->eval(ops,
        "(function() { kwcc_cb_ret = 'returned'; return 'ok'; })",
        58, "<test4c>", JS_EVAL_RETVAL);
    ops->call_cb(ops, fn4c, 0, NULL);
    kwcc_js_val_t got4c = ops->get_str_prop(ops, ops->global_obj, "kwcc_cb_ret");
    kwcc_js_cstr_buf_t bbuf4c;
    TEST("4c: call_cb function with return value",
         ops->to_cstring(ops, got4c, &bbuf4c) != NULL &&
         strcmp(ops->to_cstring(ops, got4c, &bbuf4c), "returned") == 0);

    /* 4d/4e: is_function */
    kwcc_js_val_t fn4d = ops->eval(ops, "(function(){})", 16, "<test4d>", JS_EVAL_RETVAL);
    TEST("4d: is_function on function = 1", ops->is_function(ops, fn4d));
    TEST("4e: is_function on int = 0", !ops->is_function(ops, ops->new_int32(ops, 42)));

    /* 4f: call function that throws — should not crash */
    kwcc_js_val_t fn4f = ops->eval(ops,
        "(function() { throw new Error('test'); })",
        42, "<test4f>", JS_EVAL_RETVAL);
    ops->call_cb(ops, fn4f, 0, NULL);
    TEST("4f: call_cb on throwing function no crash", 1);

    /* ════════════════════════════════════════════════════════════════
     * 5. Type checks + get_class_id
     * ════════════════════════════════════════════════════════════════ */
    printf("[5] type checks + get_class_id\n"); fflush(stdout);

    TEST("5a: is_undefined(JS_UNDEFINED) = true", ops->is_undefined(JS_UNDEFINED));
    fflush(stdout);
    TEST("5b: is_null(JS_NULL) = true", ops->is_null(JS_NULL));
    TEST("5c: is_exception(JS_EXCEPTION) = true", ops->is_exception(JS_EXCEPTION));

    kwcc_js_val_t fn5d = ops->eval(ops, "(function(){})", 16, "<test5d>", JS_EVAL_RETVAL);
    TEST("5d: is_function on eval function = true", ops->is_function(ops, fn5d));

    kwcc_js_val_t arr5e = ops->eval(ops, "[1,2,3]", 7, "<test5e>", JS_EVAL_RETVAL);
    TEST("5e: get_class_id on array = JS_CLASS_ARRAY(1)", ops->get_class_id(ops, arr5e) == 1);

    kwcc_js_val_t obj5f = ops->new_object(ops);
    TEST("5f: get_class_id on new_object = JS_CLASS_OBJECT(0)", ops->get_class_id(ops, obj5f) == 0);

    TEST("5g: get_class_id on undefined = -1", ops->get_class_id(ops, JS_UNDEFINED) == -1);

    /* ════════════════════════════════════════════════════════════════
     * 6. eval
     * ════════════════════════════════════════════════════════════════ */
    printf("[6] eval\n");

    kwcc_js_val_t ret6a = ops->eval(ops, "1 + 2", 5, "<test6a>", JS_EVAL_RETVAL);
    TEST("6a: JS_EVAL_RETVAL returns expression value", ops->to_int32(ops, ret6a) == 3);

    ops->eval(ops, "var kwcc_eval_global = 'hello';", 31, "<test6b>", JS_EVAL_REPL);
    kwcc_js_val_t got6b = ops->get_str_prop(ops, ops->global_obj, "kwcc_eval_global");
    kwcc_js_cstr_buf_t bbuf6b;
    TEST("6b: JS_EVAL_REPL creates global variable",
         ops->to_cstring(ops, got6b, &bbuf6b) != NULL &&
         strcmp(ops->to_cstring(ops, got6b, &bbuf6b), "hello") == 0);

    kwcc_js_val_t ret6c = ops->eval(ops, "undefined_var;", 14, "<test6c>", JS_EVAL_RETVAL);
    TEST("6c: eval exception code returns is_exception", ops->is_exception(ret6c));

    kwcc_js_val_t ret6d = ops->eval(ops, "", 0, "<test6d>", JS_EVAL_RETVAL);
    TEST("6d: eval empty string no crash", !ops->is_exception(ret6d));

    kwcc_js_val_t ret6e = ops->eval(ops, "'test_string'", 13, "<test6e>", JS_EVAL_RETVAL);
    kwcc_js_cstr_buf_t bbuf6e;
    TEST("6e: JS_EVAL_RETVAL returns string",
         ops->to_cstring(ops, ret6e, &bbuf6e) != NULL &&
         strcmp(ops->to_cstring(ops, ret6e, &bbuf6e), "test_string") == 0);

    /* ════════════════════════════════════════════════════════════════
     * 7. notify_js + ack_cleanup
     * ════════════════════════════════════════════════════════════════ */
    printf("[7] notify_js + ack_cleanup\n");

    /* Setup: register $notify handler */
    ops->eval(ops,
        "var kwcc_notify_received = '';"
        "var kwcc_notify_id = '';"
        "var kwcc_notify_data = null;"
        "$notify.on('testtype', function(e) {"
        "  kwcc_notify_received = e.event;"
        "  kwcc_notify_id = e.id;"
        "  kwcc_notify_data = e.data;"
        "});",
        168, "<test7setup>", JS_EVAL_REPL);

    /* 7a: notify delivers event/id/data */
    kwcc_js_val_t data7a = ops->new_object(ops);
    ops->set_str_prop(ops, data7a, "status", ops->new_string(ops, "ok"));
    ops->notify_js(ops, "testtype", "testevent", "req_007", data7a, NULL);
    kwcc_js_val_t recv7a = ops->get_str_prop(ops, ops->global_obj, "kwcc_notify_received");
    kwcc_js_cstr_buf_t rbuf7a;
    TEST("7a: notify_js delivers event",
         ops->to_cstring(ops, recv7a, &rbuf7a) != NULL &&
         strcmp(ops->to_cstring(ops, recv7a, &rbuf7a), "testevent") == 0);

    kwcc_js_val_t id7a = ops->get_str_prop(ops, ops->global_obj, "kwcc_notify_id");
    kwcc_js_cstr_buf_t ibuf7a;
    TEST("7a: notify_js delivers id",
         ops->to_cstring(ops, id7a, &ibuf7a) != NULL &&
         strcmp(ops->to_cstring(ops, id7a, &ibuf7a), "req_007") == 0);

    kwcc_js_val_t dat7a = ops->get_str_prop(ops, ops->global_obj, "kwcc_notify_data");
    kwcc_js_val_t status7a = ops->get_str_prop(ops, dat7a, "status");
    kwcc_js_cstr_buf_t dbuf7a;
    TEST("7a: notify_js delivers data.status = 'ok'",
         ops->to_cstring(ops, status7a, &dbuf7a) != NULL &&
         strcmp(ops->to_cstring(ops, status7a, &dbuf7a), "ok") == 0);

    /* 7b: notify with ack_cleanup */
    g_kwcc_test_ack_called = 0;
    g_kwcc_test_ack_id[0] = '\0';
    kwcc_js_val_t data7b = ops->new_object(ops);
    ops->set_str_prop(ops, data7b, "msg", ops->new_string(ops, "with_ack"));
    ops->notify_js(ops, "testtype", "acktest", "req_008", data7b, kwcc_test_ack_cleanup);
    TEST("7b: ack_cleanup was called", g_kwcc_test_ack_called == 1);
    TEST("7b: ack_cleanup received correct id", strcmp(g_kwcc_test_ack_id, "req_008") == 0);

    /* 7c: unregistered type — no crash */
    kwcc_js_val_t data7c = ops->new_object(ops);
    ops->notify_js(ops, "unknowntype", "event", "req_009", data7c, NULL);
    TEST("7c: notify_js with unregistered type no crash", 1);

    /* 7d: sequential notify */
    ops->notify_js(ops, "testtype", "first", "req_a", ops->new_object(ops), NULL);
    ops->notify_js(ops, "testtype", "second", "req_b", ops->new_object(ops), NULL);
    kwcc_js_val_t recv7d = ops->get_str_prop(ops, ops->global_obj, "kwcc_notify_received");
    kwcc_js_cstr_buf_t rbuf7d;
    TEST("7d: notify_js sequential: last event = 'second'",
         ops->to_cstring(ops, recv7d, &rbuf7d) != NULL &&
         strcmp(ops->to_cstring(ops, recv7d, &rbuf7d), "second") == 0);

    /* 7e: ack_cleanup + handler both run */
    g_kwcc_test_ack_called = 0;
    ops->notify_js(ops, "testtype", "order_test", "req_order", ops->new_object(ops), kwcc_test_ack_cleanup);
    kwcc_js_val_t recv7e = ops->get_str_prop(ops, ops->global_obj, "kwcc_notify_received");
    kwcc_js_cstr_buf_t rbuf7e;
    TEST("7e: notify_js: ack_cleanup + handler both ran",
         g_kwcc_test_ack_called == 1 &&
         ops->to_cstring(ops, recv7e, &rbuf7e) != NULL &&
         strcmp(ops->to_cstring(ops, recv7e, &rbuf7e), "order_test") == 0);

    /* 7f: empty data object */
    kwcc_js_val_t empty_data7 = ops->new_object(ops);
    ops->notify_js(ops, "testtype", "empty_data", "req_empty", empty_data7, NULL);
    kwcc_js_val_t dat7f = ops->get_str_prop(ops, ops->global_obj, "kwcc_notify_data");
    TEST("7f: notify_js with empty data object no crash",
         !ops->is_undefined(dat7f) && !ops->is_exception(dat7f));

    /* 7g: ack_cleanup with NULL id */
    g_kwcc_test_ack_called = 0;
    ops->notify_js(ops, "testtype", "null_id_test", NULL, ops->new_object(ops), kwcc_test_ack_cleanup);
    TEST("7g: notify_js with NULL id no crash", g_kwcc_test_ack_called == 1);

    /* ════════════════════════════════════════════════════════════════
     * 8. array_length + array_get
     * ════════════════════════════════════════════════════════════════ */
    printf("[8] array_length + array_get\n");

    kwcc_js_val_t arr8a = ops->eval(ops, "[10, 20, 30]", 13, "<test8a>", JS_EVAL_RETVAL);
    TEST("8a: array is not undefined", !ops->is_undefined(arr8a));
    TEST("8a: array_length = 3", ops->array_length(ops, arr8a) == 3);
    TEST("8a: array_get(0) = 10", ops->to_int32(ops, ops->array_get(ops, arr8a, 0)) == 10);
    TEST("8a: array_get(1) = 20", ops->to_int32(ops, ops->array_get(ops, arr8a, 1)) == 20);
    TEST("8a: array_get(2) = 30", ops->to_int32(ops, ops->array_get(ops, arr8a, 2)) == 30);
    TEST("8d: array_get(out of bounds) is undefined",
         ops->is_undefined(ops->array_get(ops, arr8a, 99)));

    kwcc_js_val_t empty_arr8 = ops->eval(ops, "[]", 2, "<test8b>", JS_EVAL_RETVAL);
    TEST("8b: empty array length = 0", ops->array_length(ops, empty_arr8) == 0);
    TEST("8b: empty array_get(0) is undefined",
         ops->is_undefined(ops->array_get(ops, empty_arr8, 0)));

    kwcc_js_val_t str_arr8 = ops->eval(ops, "['foo', 'bar']", 15, "<test8c>", JS_EVAL_RETVAL);
    TEST("8c: string array length = 2", ops->array_length(ops, str_arr8) == 2);
    kwcc_js_cstr_buf_t sabuf0;
    TEST("8c: string array[0] = 'foo'",
         ops->to_cstring(ops, ops->array_get(ops, str_arr8, 0), &sabuf0) != NULL &&
         strcmp(ops->to_cstring(ops, ops->array_get(ops, str_arr8, 0), &sabuf0), "foo") == 0);

    kwcc_js_val_t mix_arr8 = ops->eval(ops, "[1, 'two', true]", 17, "<test8e>", JS_EVAL_RETVAL);
    TEST("8e: mixed array length = 3", ops->array_length(ops, mix_arr8) == 3);
    TEST("8e: mixed array[0] = 1", ops->to_int32(ops, ops->array_get(ops, mix_arr8, 0)) == 1);
    kwcc_js_cstr_buf_t mbuf1;
    TEST("8e: mixed array[1] = 'two'",
         ops->to_cstring(ops, ops->array_get(ops, mix_arr8, 1), &mbuf1) != NULL &&
         strcmp(ops->to_cstring(ops, ops->array_get(ops, mix_arr8, 1), &mbuf1), "two") == 0);

    TEST("8f: get_class_id on array = JS_CLASS_ARRAY(1)", ops->get_class_id(ops, arr8a) == 1);

    /* ════════════════════════════════════════════════════════════════
     * 9. Dispatch: kwcc_js_call_c + kwcc_js_dispatch_call
     * ════════════════════════════════════════════════════════════════ */
    printf("[9] dispatch: kwcc_js_call_c + kwcc_js_dispatch_call\n");

    /* Register test module */
    kwcc_js_register_module(ops, &test_dispatch_mod);

    /* 9a: JS dispatch — kwcc_js_call_c('testmod','echo','hello') */
    kwcc_js_val_t ret9a = ops->eval(ops,
        "kwcc_js_call_c('testmod', 'echo', 'hello');",
        44, "<test9a>", JS_EVAL_REPL);
    TEST("9a: dispatch no exception", !ops->is_exception(ret9a));
    kwcc_js_val_t argc9a = ops->get_str_prop(ops, ops->global_obj, "kwcc_dispatch_argc");
    TEST("9a: handler received argc = 1", ops->to_int32(ops, argc9a) == 1);
    kwcc_js_val_t arg09a = ops->get_str_prop(ops, ops->global_obj, "kwcc_dispatch_arg0");
    kwcc_js_cstr_buf_t abuf9a;
    TEST("9a: handler received arg0 = 'hello'",
         ops->to_cstring(ops, arg09a, &abuf9a) != NULL &&
         strcmp(ops->to_cstring(ops, arg09a, &abuf9a), "hello") == 0);

    /* 9b: dispatch return value */
    kwcc_js_val_t ret9b = ops->eval(ops,
        "var kwcc_dispatch_result = kwcc_js_call_c('testmod', 'echo', 'test');",
        71, "<test9b>", JS_EVAL_REPL);
    TEST("9b: dispatch eval no exception", !ops->is_exception(ret9b));
    kwcc_js_val_t result9b = ops->get_str_prop(ops, ops->global_obj, "kwcc_dispatch_result");
    kwcc_js_cstr_buf_t rbuf9b;
    TEST("9b: dispatch return value = 'dispatch_ok'",
         ops->to_cstring(ops, result9b, &rbuf9b) != NULL &&
         strcmp(ops->to_cstring(ops, result9b, &rbuf9b), "dispatch_ok") == 0);

    /* 9c: multi-arg dispatch */
    kwcc_js_val_t ret9c = ops->eval(ops,
        "kwcc_js_call_c('testmod', 'echo', 'a', 'b', 'c');",
        52, "<test9c>", JS_EVAL_REPL);
    TEST("9c: multi-arg dispatch no exception", !ops->is_exception(ret9c));
    kwcc_js_val_t argc9c = ops->get_str_prop(ops, ops->global_obj, "kwcc_dispatch_argc");
    TEST("9c: multi-arg handler received argc = 3", ops->to_int32(ops, argc9c) == 3);

    /* 9d: unknown module */
    kwcc_js_val_t ret9d = ops->eval(ops,
        "var kwcc_unknown_mod = kwcc_js_call_c('nonexistent', 'echo');",
        60, "<test9d>", JS_EVAL_REPL);
    TEST("9d: unknown module no exception", !ops->is_exception(ret9d));
    TEST("9d: unknown module returns undefined",
         ops->is_undefined(ops->get_str_prop(ops, ops->global_obj, "kwcc_unknown_mod")));

    /* 9e: unknown func */
    kwcc_js_val_t ret9e = ops->eval(ops,
        "var kwcc_unknown_fn = kwcc_js_call_c('testmod', 'nonexistent');",
        64, "<test9e>", JS_EVAL_REPL);
    TEST("9e: unknown func no exception", !ops->is_exception(ret9e));
    TEST("9e: unknown func returns undefined",
         ops->is_undefined(ops->get_str_prop(ops, ops->global_obj, "kwcc_unknown_fn")));

    /* 9f: core module dispatch */
    kwcc_js_val_t ret9f = ops->eval(ops,
        "kwcc_js_call_c('core', 'mempool_dump_stats');",
        48, "<test9f>", JS_EVAL_REPL);
    TEST("9f: core/mempool_dump_stats no exception", !ops->is_exception(ret9f));

    /* 9g: C-level dispatch_call — sum handler */
    kwcc_js_val_t sum_args[2] = { ops->new_int32(ops, 10), ops->new_int32(ops, 20) };
    kwcc_js_val_t sum_ret = kwcc_js_dispatch_call("testmod", "sum", 2, sum_args);
    TEST("9g: C-level dispatch_call sum(10,20) = 30", ops->to_int32(ops, sum_ret) == 30);

    /* 9h: C-level dispatch_call — echo handler */
    kwcc_js_val_t echo_args[1] = { ops->new_string(ops, "direct_call") };
    kwcc_js_val_t echo_ret = kwcc_js_dispatch_call("testmod", "echo", 1, echo_args);
    kwcc_js_cstr_buf_t ebuf9h;
    TEST("9h: C-level dispatch_call echo = 'dispatch_ok'",
         ops->to_cstring(ops, echo_ret, &ebuf9h) != NULL &&
         strcmp(ops->to_cstring(ops, echo_ret, &ebuf9h), "dispatch_ok") == 0);

    /* 9i: duplicate registration — overwrite */
    kwcc_js_dispatch_add("testmod", "echo", test_dispatch_echo_v2);
    kwcc_js_val_t dup_args[1] = { ops->new_string(ops, "test") };
    kwcc_js_val_t dup_ret = kwcc_js_dispatch_call("testmod", "echo", 1, dup_args);
    kwcc_js_cstr_buf_t dbuf9i;
    TEST("9i: duplicate registration overwrites handler",
         ops->to_cstring(ops, dup_ret, &dbuf9i) != NULL &&
         strcmp(ops->to_cstring(ops, dup_ret, &dbuf9i), "dispatch_ok_v2") == 0);

    /* ── Summary ── */
    printf("\n=== %d passed, %d failed, %d total ===\n",
           tests_passed, tests_failed, tests_passed + tests_failed);

    JS_FreeContext(ctx);
    free(mem_buf);
    kwcc_mempool_shutdown();
    return (tests_failed == 0) ? 0 : 1;
}
