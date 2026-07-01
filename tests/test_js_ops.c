/* test_js_ops.c — kwcc_js_ops_t ABI contract tests
 *
 * 验证 ops-signature + module-grouped dispatch 机制的正确性
 * 60 个测试点，覆盖 9 个 Section
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

/* ── File-scope test helpers ── */

static int g_kwcc_test_ack_called = 0;
static char g_kwcc_test_ack_id[64];

static void kwcc_test_ack_cleanup(const char *id) {
    g_kwcc_test_ack_called++;
    if (id) {
        strncpy(g_kwcc_test_ack_id, id, sizeof(g_kwcc_test_ack_id) - 1);
        g_kwcc_test_ack_id[sizeof(g_kwcc_test_ack_id) - 1] = '\0';
    }
}

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

static kwcc_js_val_t test_dispatch_sum(kwcc_js_ops_t *ops, int argc, kwcc_js_val_t *argv) {
    if (argc < 2) return ops->new_int32(ops, 0);
    int a = ops->to_int32(ops, argv[0]);
    int b = ops->to_int32(ops, argv[1]);
    return ops->new_int32(ops, a + b);
}

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
    NULL,
    test_dispatch_apis,
    NULL,
    NULL
};

/* ── Main ── */

int main(void) {
    printf("=== kwcc_js_ops_t ABI contract tests ===\n\n");

    kwcc_mempool_init();

    void *mem_buf = malloc(4 * 1024 * 1024);
    JSContext *ctx = JS_NewContext(mem_buf, 4 * 1024 * 1024, &js_stdlib);
    if (!ctx) { printf("JS_NewContext failed\n"); return 1; }

    kwcc_js_ops_init(ctx);
    kwcc_js_ops_t *ops = &g_kwcc_js_ops;

    kwcc_js_register_modules(ops);

    /* ════════════════════════════════════════════════════════════════
     * Section 1: new_object + set_str_prop + get_str_prop (7点)
     * ════════════════════════════════════════════════════════════════ */
    printf("[1] new_object + set_str_prop + get_str_prop\n");

    /* 1a: new_object returns non-exception */
    kwcc_js_val_t obj1 = ops->new_object(ops);
    TEST("1a: new_object returns non-exception", !ops->is_exception(obj1));

    /* 1b: set_str_prop + get_str_prop string roundtrip */
    ops->set_str_prop(ops, obj1, "greeting", ops->new_string(ops, "hello"));
    kwcc_js_val_t got1 = ops->get_str_prop(ops, obj1, "greeting");
    kwcc_js_cstr_buf_t buf1;
    const char *str1 = ops->to_cstring(ops, got1, &buf1);
    TEST("1b: get_str_prop returns 'hello'", str1 != NULL && strcmp(str1, "hello") == 0);

    /* 1c: get_str_prop nonexistent property returns undefined */
    kwcc_js_val_t got1b = ops->get_str_prop(ops, obj1, "nonexistent");
    TEST("1c: get_str_prop(nonexistent) is undefined", ops->is_undefined(got1b));

    /* 1d: set_str_prop overwrites existing property */
    ops->set_str_prop(ops, obj1, "greeting", ops->new_string(ops, "world"));
    kwcc_js_val_t got1c = ops->get_str_prop(ops, obj1, "greeting");
    kwcc_js_cstr_buf_t buf1c;
    TEST("1d: set_str_prop overwrites existing",
         ops->to_cstring(ops, got1c, &buf1c) != NULL &&
         strcmp(ops->to_cstring(ops, got1c, &buf1c), "world") == 0);

    /* 1e: set_str_prop with int value, get + to_int32 */
    ops->set_str_prop(ops, obj1, "count", ops->new_int32(ops, 42));
    kwcc_js_val_t got1d = ops->get_str_prop(ops, obj1, "count");
    TEST("1e: set_str_prop with int value", ops->to_int32(ops, got1d) == 42);

    /* 1f: get_str_prop(global, 'JSON') returns non-undefined */
    kwcc_js_val_t json_obj = ops->get_str_prop(ops, ops->global_obj, "JSON");
    TEST("1f: get_str_prop(global, 'JSON') is not undefined", !ops->is_undefined(json_obj));

    /* 1g: separate objects are independent */
    kwcc_js_val_t obj1b = ops->new_object(ops);
    ops->set_str_prop(ops, obj1b, "x", ops->new_int32(ops, 1));
    kwcc_js_val_t got1e = ops->get_str_prop(ops, obj1b, "x");
    kwcc_js_val_t got1f = ops->get_str_prop(ops, obj1, "x");
    TEST("1g: separate objects are independent",
         ops->to_int32(ops, got1e) == 1 && ops->is_undefined(got1f));

    /* ════════════════════════════════════════════════════════════════
     * Section 2: new_int32 + to_int32 (6点)
     * ════════════════════════════════════════════════════════════════ */
    printf("[2] new_int32 + to_int32\n");

    /* 2a: to_int32(new_int32(0)) == 0 */
    TEST("2a: new_int32(0) roundtrip", ops->to_int32(ops, ops->new_int32(ops, 0)) == 0);

    /* 2b: to_int32(new_int32(-99)) == -99 */
    TEST("2b: new_int32(-99) roundtrip", ops->to_int32(ops, ops->new_int32(ops, -99)) == -99);

    /* 2c: to_int32(new_int32(12345)) == 12345 */
    TEST("2c: new_int32(12345) roundtrip", ops->to_int32(ops, ops->new_int32(ops, 12345)) == 12345);

    /* 2d: to_int32(new_int32(2147483647)) == 2147483647 (INT32_MAX) */
    TEST("2d: new_int32(2147483647) roundtrip",
         ops->to_int32(ops, ops->new_int32(ops, 2147483647)) == 2147483647);

    /* 2e: to_int32(new_int32(-2147483648)) == -2147483648 (INT32_MIN) */
    TEST("2e: new_int32(-2147483648) roundtrip",
         ops->to_int32(ops, ops->new_int32(ops, -2147483648)) == -2147483648);

    /* 2f: eval("'123'", JS_EVAL_RETVAL) returns string, to_int32 == 123 */
    kwcc_js_val_t str_num2 = ops->eval(ops, "'123'", 5, "<test2f>", JS_EVAL_RETVAL);
    TEST("2f: to_int32 on JS string '123' = 123", ops->to_int32(ops, str_num2) == 123);

    /* ════════════════════════════════════════════════════════════════
     * Section 3: new_string + new_string_len + to_cstring (7点)
     * ════════════════════════════════════════════════════════════════ */
    printf("[3] new_string + new_string_len + to_cstring\n");

    /* 3a: to_cstring(new_string("abc")) == "abc" (< 5 字节，内联存储) */
    kwcc_js_cstr_buf_t sbuf3;
    TEST("3a: short string 'abc' roundtrip",
         (ops->to_cstring(ops, ops->new_string(ops, "abc"), &sbuf3) != NULL &&
          strcmp(ops->to_cstring(ops, ops->new_string(ops, "abc"), &sbuf3), "abc") == 0));

    /* 3b: to_cstring(new_string("This is a much longer string...")) returns full string */
    const char *long_text = "This is a much longer string that definitely exceeds the 5-byte inline buffer";
    kwcc_js_cstr_buf_t lbuf3;
    const char *lstr3 = ops->to_cstring(ops, ops->new_string(ops, long_text), &lbuf3);
    TEST("3b: long string roundtrip", lstr3 != NULL && strcmp(lstr3, long_text) == 0);

    /* 3c: to_cstring(new_string_len("hello world", 5)) == "hello" */
    kwcc_js_cstr_buf_t lenbuf3;
    const char *lstr3b = ops->to_cstring(ops, ops->new_string_len(ops, "hello world", 5), &lenbuf3);
    TEST("3c: new_string_len truncates to 'hello'", lstr3b != NULL && strcmp(lstr3b, "hello") == 0);

    /* 3d: to_cstring(new_string("")) == "" */
    kwcc_js_cstr_buf_t ebuf3;
    const char *estr3 = ops->to_cstring(ops, ops->new_string(ops, ""), &ebuf3);
    TEST("3d: empty string roundtrip", estr3 != NULL && strcmp(estr3, "") == 0);

    /* 3e: to_cstring(new_string_len("anything", 0)) == "" */
    kwcc_js_cstr_buf_t zbuf3;
    const char *zstr3 = ops->to_cstring(ops, ops->new_string_len(ops, "anything", 0), &zbuf3);
    TEST("3e: new_string_len(, 0) = empty string", zstr3 != NULL && strcmp(zstr3, "") == 0);

    /* 3f: to_cstring(new_int32(42)) == "42" (JS 强转) */
    kwcc_js_cstr_buf_t ibuf3;
    const char *istr3 = ops->to_cstring(ops, ops->new_int32(ops, 42), &ibuf3);
    TEST("3f: to_cstring on int 42 = '42'", istr3 != NULL && strcmp(istr3, "42") == 0);

    /* 3g: to_cstring(new_string("X")) == "X" */
    kwcc_js_cstr_buf_t cbuf3;
    TEST("3g: single char 'X' roundtrip",
         (ops->to_cstring(ops, ops->new_string(ops, "X"), &cbuf3) != NULL &&
          strcmp(ops->to_cstring(ops, ops->new_string(ops, "X"), &cbuf3), "X") == 0));

    /* ════════════════════════════════════════════════════════════════
     * Section 4: call_cb (6点)
     * call_cb 签名是 void，无法直接获取返回值，必须通过全局变量中转验证
     * ════════════════════════════════════════════════════════════════ */
    printf("[4] call_cb\n");

    /* 4a: call no-arg function, verify execution via global variable */
    ops->eval(ops, "var kwcc_cb_called = 0;", 20, "<test4a>", JS_EVAL_REPL);
    kwcc_js_val_t fn4a = ops->eval(ops,
        "(function() { kwcc_cb_called = 1; })",
        35, "<test4a>", JS_EVAL_RETVAL);
    TEST("4a: call_cb creates function", ops->is_function(ops, fn4a));
    ops->call_cb(ops, fn4a, 0, NULL);
    kwcc_js_val_t called4a = ops->get_str_prop(ops, ops->global_obj, "kwcc_cb_called");
    TEST("4a: call_cb executed function", ops->to_int32(ops, called4a) == 1);

    /* 4b: call function with string argument, verify via global variable */
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

    /* 4c: call function with return value, verify via global variable */
    ops->eval(ops, "var kwcc_cb_ret = '';", 23, "<test4c>", JS_EVAL_REPL);
    kwcc_js_val_t fn4c = ops->eval(ops,
        "(function() { kwcc_cb_ret = 'returned'; })",
        46, "<test4c>", JS_EVAL_RETVAL);
    ops->call_cb(ops, fn4c, 0, NULL);
    kwcc_js_val_t got4c = ops->get_str_prop(ops, ops->global_obj, "kwcc_cb_ret");
    kwcc_js_cstr_buf_t bbuf4c;
    TEST("4c: call_cb function with return value",
         ops->to_cstring(ops, got4c, &bbuf4c) != NULL &&
         strcmp(ops->to_cstring(ops, got4c, &bbuf4c), "returned") == 0);

    /* 4d: is_function on eval-created function == 1 */
    kwcc_js_val_t fn4d = ops->eval(ops, "(function(){})", 16, "<test4d>", JS_EVAL_RETVAL);
    TEST("4d: is_function on function = 1", ops->is_function(ops, fn4d));

    /* 4e: is_function on int == 0 */
    TEST("4e: is_function on int = 0", !ops->is_function(ops, ops->new_int32(ops, 42)));

    /* 4f: call function that throws, verify engine state remains valid */
    kwcc_js_val_t fn4f = ops->eval(ops,
        "(function() { throw new Error('test'); })",
        42, "<test4f>", JS_EVAL_RETVAL);
    ops->call_cb(ops, fn4f, 0, NULL);
    kwcc_js_val_t verify4f = ops->eval(ops, "1 + 1", 5, "<test4f_verify>", JS_EVAL_RETVAL);
    TEST("4f: call_cb on throwing function no crash, engine state valid",
         ops->to_int32(ops, verify4f) == 2);

    /* ════════════════════════════════════════════════════════════════
     * Section 5: 类型判断 + get_class_id (7点)
     * 一致性要求：使用 ops->undefined/ops->null/ops->exception，不使用宏
     * ════════════════════════════════════════════════════════════════ */
    printf("[5] type checks + get_class_id\n");

    /* 5a: is_undefined(ops->undefined) == true */
    TEST("5a: is_undefined(ops->undefined) = true", ops->is_undefined(ops->undefined));

    /* 5b: is_null(ops->null) == true */
    TEST("5b: is_null(ops->null) = true", ops->is_null(ops->null));

    /* 5c: is_exception(ops->exception) == true */
    TEST("5c: is_exception(ops->exception) = true", ops->is_exception(ops->exception));

    /* 5d: is_function on eval-created function == true */
    kwcc_js_val_t fn5d = ops->eval(ops, "(function(){})", 16, "<test5d>", JS_EVAL_RETVAL);
    TEST("5d: is_function on eval function = true", ops->is_function(ops, fn5d));

    /* 5e: get_class_id on eval("[1,2,3]") == JS_CLASS_ARRAY(1) */
    kwcc_js_val_t arr5e = ops->eval(ops, "[1,2,3]", 7, "<test5e>", JS_EVAL_RETVAL);
    TEST("5e: get_class_id on array = JS_CLASS_ARRAY(1)", ops->get_class_id(ops, arr5e) == 1);

    /* 5f: get_class_id on new_object == JS_CLASS_OBJECT(0) */
    kwcc_js_val_t obj5f = ops->new_object(ops);
    TEST("5f: get_class_id on new_object = JS_CLASS_OBJECT(0)", ops->get_class_id(ops, obj5f) == 0);

    /* 5g: get_class_id(ops->undefined) == -1 */
    TEST("5g: get_class_id on ops->undefined = -1", ops->get_class_id(ops, ops->undefined) == -1);

    /* ════════════════════════════════════════════════════════════════
     * Section 6: eval (5点)
     * ════════════════════════════════════════════════════════════════ */
    printf("[6] eval\n");

    /* 6a: JS_EVAL_RETVAL returns expression value */
    kwcc_js_val_t ret6a = ops->eval(ops, "1 + 2", 5, "<test6a>", JS_EVAL_RETVAL);
    TEST("6a: JS_EVAL_RETVAL returns expression value", ops->to_int32(ops, ret6a) == 3);

    /* 6b: JS_EVAL_REPL creates global variable */
    ops->eval(ops, "var kwcc_eval_global = 'hello';", 31, "<test6b>", JS_EVAL_REPL);
    kwcc_js_val_t got6b = ops->get_str_prop(ops, ops->global_obj, "kwcc_eval_global");
    kwcc_js_cstr_buf_t bbuf6b;
    TEST("6b: JS_EVAL_REPL creates global variable",
         ops->to_cstring(ops, got6b, &bbuf6b) != NULL &&
         strcmp(ops->to_cstring(ops, got6b, &bbuf6b), "hello") == 0);

    /* 6c: eval exception code returns is_exception */
    kwcc_js_val_t ret6c = ops->eval(ops, "undefined_var;", 14, "<test6c>", JS_EVAL_RETVAL);
    TEST("6c: eval exception code returns is_exception", ops->is_exception(ret6c));

    /* 6d: eval empty string no crash */
    kwcc_js_val_t ret6d = ops->eval(ops, "", 0, "<test6d>", JS_EVAL_RETVAL);
    TEST("6d: eval empty string no crash", !ops->is_exception(ret6d));

    /* 6e: JS_EVAL_RETVAL returns string */
    kwcc_js_val_t ret6e = ops->eval(ops, "'test_string'", 13, "<test6e>", JS_EVAL_RETVAL);
    kwcc_js_cstr_buf_t bbuf6e;
    TEST("6e: JS_EVAL_RETVAL returns string",
         ops->to_cstring(ops, ret6e, &bbuf6e) != NULL &&
         strcmp(ops->to_cstring(ops, ret6e, &bbuf6e), "test_string") == 0);

    /* ════════════════════════════════════════════════════════════════
     * Section 7: notify_js C 端测试 (5点)
     * 测试目标：验证 ops->notify_js 的 C 端行为，不依赖 JS 端 $notify.on 注册
     * ════════════════════════════════════════════════════════════════ */
    printf("[7] notify_js C-side tests\n");

    /* 7a: notify_js with no handler registered, no crash */
    kwcc_js_val_t data7a = ops->new_object(ops);
    ops->notify_js(ops, "testtype", "event1", "req_001", data7a, NULL);
    TEST("7a: notify_js with no handler no crash", 1);

    /* 7b: notify_js with ack_cleanup, verify cleanup is called */
    g_kwcc_test_ack_called = 0;
    g_kwcc_test_ack_id[0] = '\0';
    kwcc_js_val_t data7b = ops->new_object(ops);
    ops->notify_js(ops, "testtype", "event2", "req_002", data7b, kwcc_test_ack_cleanup);
    TEST("7b: ack_cleanup was called", g_kwcc_test_ack_called == 1);
    TEST("7b: ack_cleanup received correct id", strcmp(g_kwcc_test_ack_id, "req_002") == 0);

    /* 7c: notify_js with unregistered type, no crash */
    kwcc_js_val_t data7c = ops->new_object(ops);
    ops->notify_js(ops, "unknowntype", "event3", "req_003", data7c, NULL);
    TEST("7c: notify_js with unregistered type no crash", 1);

    /* 7d: consecutive notify_js calls with ack_cleanup */
    g_kwcc_test_ack_called = 0;
    kwcc_js_val_t data7d1 = ops->new_object(ops);
    kwcc_js_val_t data7d2 = ops->new_object(ops);
    ops->notify_js(ops, "testtype", "event4", "req_004", data7d1, kwcc_test_ack_cleanup);
    ops->notify_js(ops, "testtype", "event5", "req_005", data7d2, kwcc_test_ack_cleanup);
    TEST("7d: consecutive notify_js calls safe", g_kwcc_test_ack_called == 2);

    /* 7e: notify_js with NULL id, ack_cleanup handles NULL safely */
    g_kwcc_test_ack_called = 0;
    g_kwcc_test_ack_id[0] = '\0';
    kwcc_js_val_t data7e = ops->new_object(ops);
    ops->notify_js(ops, "testtype", "event6", NULL, data7e, kwcc_test_ack_cleanup);
    TEST("7e: notify_js with NULL id no crash", g_kwcc_test_ack_called == 1);

    /* ════════════════════════════════════════════════════════════════
     * Section 8: array_length + array_get (6点)
     * ════════════════════════════════════════════════════════════════ */
    printf("[8] array_length + array_get\n");

    /* 8a: array [10, 20, 30] with length and element access */
    kwcc_js_val_t arr8a = ops->eval(ops, "[10, 20, 30]", 13, "<test8a>", JS_EVAL_RETVAL);
    TEST("8a: array is not undefined", !ops->is_undefined(arr8a));
    TEST("8a: array_length = 3", ops->array_length(ops, arr8a) == 3);
    TEST("8a: array_get(0) = 10", ops->to_int32(ops, ops->array_get(ops, arr8a, 0)) == 10);
    TEST("8a: array_get(1) = 20", ops->to_int32(ops, ops->array_get(ops, arr8a, 1)) == 20);
    TEST("8a: array_get(2) = 30", ops->to_int32(ops, ops->array_get(ops, arr8a, 2)) == 30);

    /* 8b: empty array [] */
    kwcc_js_val_t empty_arr8 = ops->eval(ops, "[]", 2, "<test8b>", JS_EVAL_RETVAL);
    TEST("8b: empty array length = 0", ops->array_length(ops, empty_arr8) == 0);
    TEST("8b: empty array_get(0) is undefined",
         ops->is_undefined(ops->array_get(ops, empty_arr8, 0)));

    /* 8c: string array ['foo', 'bar'] */
    kwcc_js_val_t str_arr8 = ops->eval(ops, "['foo', 'bar']", 15, "<test8c>", JS_EVAL_RETVAL);
    TEST("8c: string array length = 2", ops->array_length(ops, str_arr8) == 2);
    kwcc_js_cstr_buf_t sabuf0;
    TEST("8c: string array[0] = 'foo'",
         ops->to_cstring(ops, ops->array_get(ops, str_arr8, 0), &sabuf0) != NULL &&
         strcmp(ops->to_cstring(ops, ops->array_get(ops, str_arr8, 0), &sabuf0), "foo") == 0);

    /* 8d: out of bounds access */
    TEST("8d: array_get(out of bounds) is undefined",
         ops->is_undefined(ops->array_get(ops, arr8a, 99)));

    /* 8e: mixed type array [1, 'two', true] */
    kwcc_js_val_t mix_arr8 = ops->eval(ops, "[1, 'two', true]", 17, "<test8e>", JS_EVAL_RETVAL);
    TEST("8e: mixed array length = 3", ops->array_length(ops, mix_arr8) == 3);
    TEST("8e: mixed array[0] = 1", ops->to_int32(ops, ops->array_get(ops, mix_arr8, 0)) == 1);
    kwcc_js_cstr_buf_t mbuf1;
    TEST("8e: mixed array[1] = 'two'",
         ops->to_cstring(ops, ops->array_get(ops, mix_arr8, 1), &mbuf1) != NULL &&
         strcmp(ops->to_cstring(ops, ops->array_get(ops, mix_arr8, 1), &mbuf1), "two") == 0);

    /* 8f: get_class_id on array */
    TEST("8f: get_class_id on array = JS_CLASS_ARRAY(1)", ops->get_class_id(ops, arr8a) == 1);

    /* ════════════════════════════════════════════════════════════════
     * Section 9: dispatch: kwcc_js_call_c + kwcc_js_dispatch_call (9点)
     * 前置条件：kwcc_js_register_module(ops, &test_dispatch_mod) 注册 testmod
     * ════════════════════════════════════════════════════════════════ */
    printf("[9] dispatch: kwcc_js_call_c + kwcc_js_dispatch_call\n");

    /* Register test module */
    kwcc_js_register_module(ops, &test_dispatch_mod);

    /* ── JS 层 dispatch（通过 eval 调 kwcc_js_call_c）── */

    /* 9a: basic dispatch with single argument */
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

    /* ── C 端 dispatch（直接调 kwcc_js_dispatch_call）── */

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

    /* ── 重复注册行为 ── */

    /* 9i: duplicate registration overwrites handler */
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

