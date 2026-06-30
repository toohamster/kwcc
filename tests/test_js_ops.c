/* test_js_ops.c — kwcc_js_ops_t ABI contract tests
 *
 * Verifies each ops function pointer as an internal ABI contract
 * before refactoring HTTP code to use the ops interface.
 *
 * 8 test points from js-bridge-architecture.md Step 3:
 *   1. new_object + set_str_prop + get_str_prop
 *   2. new_int32 + to_int32
 *   3. new_string + to_cstring (short + long)
 *   4. call_cb (call JS function, pass args, get return)
 *   5. is_function / is_undefined / is_null / is_exception
 *   6. eval + JS_EVAL_REPL
 *   7. notify_js + ack_cleanup
 *   8. array_length + array_get
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

/* ── ack_cleanup test helpers ── */

static int g_kwcc_test_ack_called = 0;
static char g_kwcc_test_ack_id[64];

static void kwcc_test_ack_cleanup(const char *id) {
    g_kwcc_test_ack_called = 1;
    if (id) {
        strncpy(g_kwcc_test_ack_id, id, sizeof(g_kwcc_test_ack_id) - 1);
        g_kwcc_test_ack_id[sizeof(g_kwcc_test_ack_id) - 1] = '\0';
    }
}

int main(void) {
    printf("=== kwcc_js_ops_t ABI contract tests ===\n\n");

    /* Setup: create JSContext + init ops + inject $notify */
    kwcc_mempool_init();

    void *mem_buf = malloc(4 * 1024 * 1024);
    JSContext *ctx = JS_NewContext(mem_buf, 4 * 1024 * 1024, &js_stdlib);
    if (!ctx) { printf("JS_NewContext failed\n"); return 1; }

    kwcc_js_ops_init(ctx);
    kwcc_js_ops_t *ops = &g_kwcc_js_ops;

    /* Register $notify (needed for test 7) */
    kwcc_js_register_modules(ops);

    /* ── 1. new_object + set_str_prop + get_str_prop ── */
    printf("[1] new_object + set_str_prop + get_str_prop\n");
    kwcc_js_val_t obj1 = ops->new_object(ops);
    TEST("new_object returns non-exception", !ops->is_exception(obj1));

    kwcc_js_val_t val1 = ops->new_string(ops, "hello");
    ops->set_str_prop(ops, obj1, "greeting", val1);

    kwcc_js_val_t got1 = ops->get_str_prop(ops, obj1, "greeting");
    kwcc_js_cstr_buf_t buf1;
    const char *str1 = ops->to_cstring(ops, got1, &buf1);
    TEST("get_str_prop returns 'hello'", str1 != NULL && strcmp(str1, "hello") == 0);

    /* Non-existent property returns undefined */
    kwcc_js_val_t got1b = ops->get_str_prop(ops, obj1, "nonexistent");
    TEST("get_str_prop(nonexistent) is undefined", ops->is_undefined(got1b));

    /* Overwrite existing property */
    ops->set_str_prop(ops, obj1, "greeting", ops->new_string(ops, "world"));
    kwcc_js_val_t got1c = ops->get_str_prop(ops, obj1, "greeting");
    kwcc_js_cstr_buf_t buf1c;
    const char *str1c = ops->to_cstring(ops, got1c, &buf1c);
    TEST("set_str_prop overwrites existing property", str1c != NULL && strcmp(str1c, "world") == 0);

    /* Set int property on object */
    ops->set_str_prop(ops, obj1, "count", ops->new_int32(ops, 42));
    kwcc_js_val_t got1d = ops->get_str_prop(ops, obj1, "count");
    TEST("set_str_prop with int value", ops->to_int32(ops, got1d) == 42);

    /* Read known global property */
    kwcc_js_val_t json_obj = ops->get_str_prop(ops, ops->global_obj, "JSON");
    TEST("get_str_prop(global, 'JSON') is not undefined", !ops->is_undefined(json_obj));

    /* Multiple objects are independent */
    kwcc_js_val_t obj1b = ops->new_object(ops);
    ops->set_str_prop(ops, obj1b, "x", ops->new_int32(ops, 1));
    kwcc_js_val_t got1e = ops->get_str_prop(ops, obj1b, "x");
    kwcc_js_val_t got1f = ops->get_str_prop(ops, obj1, "x");
    TEST("separate objects are independent", ops->to_int32(ops, got1e) == 1 && ops->is_undefined(got1f));

    /* ── 2. new_int32 + to_int32 ── */
    printf("[2] new_int32 + to_int32\n");
    kwcc_js_val_t int2 = ops->new_int32(ops, 12345);
    int val2 = ops->to_int32(ops, int2);
    TEST("new_int32(12345) + to_int32 = 12345", val2 == 12345);

    kwcc_js_val_t neg2 = ops->new_int32(ops, -99);
    TEST("new_int32(-99) + to_int32 = -99", ops->to_int32(ops, neg2) == -99);

    kwcc_js_val_t zero2 = ops->new_int32(ops, 0);
    TEST("new_int32(0) + to_int32 = 0", ops->to_int32(ops, zero2) == 0);

    /* Boundary values */
    kwcc_js_val_t max2 = ops->new_int32(ops, 2147483647);
    TEST("new_int32(INT32_MAX) roundtrip", ops->to_int32(ops, max2) == 2147483647);

    kwcc_js_val_t min2 = ops->new_int32(ops, -2147483648);
    TEST("new_int32(INT32_MIN) roundtrip", ops->to_int32(ops, min2) == -2147483648);

    /* to_int32 on string value (JS coercion: "123" → 123) */
    ops->eval(ops, "var kwcc_test_str_num = '123';",
              strlen("var kwcc_test_str_num = '123';"), "<test2>", JS_EVAL_REPL);
    kwcc_js_val_t str_num2 = ops->get_str_prop(ops, ops->global_obj, "kwcc_test_str_num");
    TEST("to_int32 on JS string '123' = 123", ops->to_int32(ops, str_num2) == 123);

    /* ── 3. new_string + to_cstring (short + long) ── */
    printf("[3] new_string + to_cstring\n");
    /* Short string (fits in inline buf[5]) */
    kwcc_js_val_t short3 = ops->new_string(ops, "abc");
    kwcc_js_cstr_buf_t sbuf3;
    const char *sstr3 = ops->to_cstring(ops, short3, &sbuf3);
    TEST("short string 'abc' roundtrip", sstr3 != NULL && strcmp(sstr3, "abc") == 0);

    /* Long string (exceeds inline buf[5]) */
    const char *long_text = "This is a much longer string that definitely exceeds the 5-byte inline buffer";
    kwcc_js_val_t long3 = ops->new_string(ops, long_text);
    kwcc_js_cstr_buf_t lbuf3;
    const char *lstr3 = ops->to_cstring(ops, long3, &lbuf3);
    TEST("long string roundtrip", lstr3 != NULL && strcmp(lstr3, long_text) == 0);

    /* new_string_len with explicit length */
    kwcc_js_val_t len3 = ops->new_string_len(ops, "hello world", 5);
    kwcc_js_cstr_buf_t lenbuf3;
    const char *lstr3b = ops->to_cstring(ops, len3, &lenbuf3);
    TEST("new_string_len truncates to 'hello'", lstr3b != NULL && strcmp(lstr3b, "hello") == 0);

    /* Empty string */
    kwcc_js_val_t empty3 = ops->new_string(ops, "");
    kwcc_js_cstr_buf_t ebuf3;
    const char *estr3 = ops->to_cstring(ops, empty3, &ebuf3);
    TEST("empty string roundtrip", estr3 != NULL && strcmp(estr3, "") == 0);

    /* Single char string (fits in buf[5]) */
    kwcc_js_val_t char3 = ops->new_string(ops, "X");
    kwcc_js_cstr_buf_t cbuf3;
    const char *cstr3 = ops->to_cstring(ops, char3, &cbuf3);
    TEST("single char string roundtrip", cstr3 != NULL && strcmp(cstr3, "X") == 0);

    /* Exactly 4 bytes (max inline: 4 UTF-8 + NUL) */
    kwcc_js_val_t four3 = ops->new_string(ops, "abcd");
    kwcc_js_cstr_buf_t fbuf3;
    const char *fstr3 = ops->to_cstring(ops, four3, &fbuf3);
    TEST("4-byte string roundtrip", fstr3 != NULL && strcmp(fstr3, "abcd") == 0);

    /* new_string_len with zero length */
    kwcc_js_val_t zlen3 = ops->new_string_len(ops, "anything", 0);
    kwcc_js_cstr_buf_t zbuf3;
    const char *zstr3 = ops->to_cstring(ops, zlen3, &zbuf3);
    TEST("new_string_len(, 0) = empty string", zstr3 != NULL && strcmp(zstr3, "") == 0);

    /* to_cstring on int value (JS coercion: 42 → "42") */
    kwcc_js_val_t int_str3 = ops->new_int32(ops, 42);
    kwcc_js_cstr_buf_t ibuf3;
    const char *istr3 = ops->to_cstring(ops, int_str3, &ibuf3);
    TEST("to_cstring on int 42 = '42'", istr3 != NULL && strcmp(istr3, "42") == 0);

    /* to_cstring on undefined */
    kwcc_js_cstr_buf_t ubuf3;
    const char *ustr3 = ops->to_cstring(ops, ops->undefined, &ubuf3);
    TEST("to_cstring on undefined = 'undefined'", ustr3 != NULL && strcmp(ustr3, "undefined") == 0);

    /* ── 4. call_cb (call JS function, pass args, get return) ── */
    printf("[4] call_cb\n");
    /* Define a JS function that stores result in a global variable */
    ops->eval(ops,
        "var kwcc_test_stored = null;\n"
        "var kwcc_test_store = function(v) { kwcc_test_stored = v; };",
        strlen("var kwcc_test_stored = null;\n"
               "var kwcc_test_store = function(v) { kwcc_test_stored = v; };"),
        "<test4>", JS_EVAL_REPL);

    kwcc_js_val_t fn4 = ops->get_str_prop(ops, ops->global_obj, "kwcc_test_store");
    TEST("kwcc_test_store is a function", ops->is_function(ops, fn4));

    kwcc_js_val_t args4[1];
    args4[0] = ops->new_string(ops, "stored_value");
    ops->call_cb(ops, fn4, 1, args4);

    kwcc_js_val_t stored4 = ops->get_str_prop(ops, ops->global_obj, "kwcc_test_stored");
    kwcc_js_cstr_buf_t sbuf4;
    const char *sstr4 = ops->to_cstring(ops, stored4, &sbuf4);
    TEST("call_cb stores value via JS function", sstr4 != NULL && strcmp(sstr4, "stored_value") == 0);

    /* Test call_cb with multiple integer args */
    ops->eval(ops,
        "var kwcc_test_sum = null;\n"
        "var kwcc_test_store_sum = function(a, b) { kwcc_test_sum = a + b; };",
        strlen("var kwcc_test_sum = null;\n"
               "var kwcc_test_store_sum = function(a, b) { kwcc_test_sum = a + b; };"),
        "<test4b>", JS_EVAL_REPL);

    kwcc_js_val_t fn4b = ops->get_str_prop(ops, ops->global_obj, "kwcc_test_store_sum");
    kwcc_js_val_t args4b[2];
    args4b[0] = ops->new_int32(ops, 10);
    args4b[1] = ops->new_int32(ops, 20);
    ops->call_cb(ops, fn4b, 2, args4b);

    kwcc_js_val_t sum4 = ops->get_str_prop(ops, ops->global_obj, "kwcc_test_sum");
    TEST("call_cb with 2 int args: 10+20=30", ops->to_int32(ops, sum4) == 30);

    /* call_cb with 0 args */
    ops->eval(ops,
        "var kwcc_test_no_arg_called = 0;\n"
        "var kwcc_test_no_arg = function() { kwcc_test_no_arg_called = 1; };",
        strlen("var kwcc_test_no_arg_called = 0;\n"
               "var kwcc_test_no_arg = function() { kwcc_test_no_arg_called = 1; };"),
        "<test4c>", JS_EVAL_REPL);
    kwcc_js_val_t fn4c = ops->get_str_prop(ops, ops->global_obj, "kwcc_test_no_arg");
    ops->call_cb(ops, fn4c, 0, NULL);
    kwcc_js_val_t called4c = ops->get_str_prop(ops, ops->global_obj, "kwcc_test_no_arg_called");
    TEST("call_cb with 0 args: function executed", ops->to_int32(ops, called4c) == 1);

    /* call_cb with object arg */
    ops->eval(ops,
        "var kwcc_test_obj_arg_result = null;\n"
        "var kwcc_test_obj_arg = function(obj) { kwcc_test_obj_arg_result = obj.key; };",
        strlen("var kwcc_test_obj_arg_result = null;\n"
               "var kwcc_test_obj_arg = function(obj) { kwcc_test_obj_arg_result = obj.key; };"),
        "<test4d>", JS_EVAL_REPL);
    kwcc_js_val_t fn4d = ops->get_str_prop(ops, ops->global_obj, "kwcc_test_obj_arg");
    kwcc_js_val_t obj_arg4 = ops->new_object(ops);
    ops->set_str_prop(ops, obj_arg4, "key", ops->new_string(ops, "value"));
    kwcc_js_val_t args4d[1];
    args4d[0] = obj_arg4;
    ops->call_cb(ops, fn4d, 1, args4d);
    kwcc_js_val_t result4d = ops->get_str_prop(ops, ops->global_obj, "kwcc_test_obj_arg_result");
    kwcc_js_cstr_buf_t rbuf4d;
    const char *rstr4d = ops->to_cstring(ops, result4d, &rbuf4d);
    TEST("call_cb with object arg: reads obj.key", rstr4d != NULL && strcmp(rstr4d, "value") == 0);

    /* call_cb with JS exception: should not crash */
    ops->eval(ops,
        "var kwcc_test_throw = function() { throw new Error('test'); };",
        strlen("var kwcc_test_throw = function() { throw new Error('test'); };"),
        "<test4e>", JS_EVAL_REPL);
    kwcc_js_val_t fn4e = ops->get_str_prop(ops, ops->global_obj, "kwcc_test_throw");
    ops->call_cb(ops, fn4e, 0, NULL);
    TEST("call_cb with throwing function: no crash", 1);

    /* call_cb with 7 args (max for HTTP request handler) */
    ops->eval(ops,
        "var kwcc_test_7arg = null;\n"
        "var kwcc_test_store7 = function(a,b,c,d,e,f,g) {\n"
        "    kwcc_test_7arg = a + b + c + d + e + f + g;\n"
        "};",
        strlen("var kwcc_test_7arg = null;\n"
               "var kwcc_test_store7 = function(a,b,c,d,e,f,g) {\n"
               "    kwcc_test_7arg = a + b + c + d + e + f + g;\n"
               "};"),
        "<test4f>", JS_EVAL_REPL);
    kwcc_js_val_t fn4f = ops->get_str_prop(ops, ops->global_obj, "kwcc_test_store7");
    kwcc_js_val_t args4f[7];
    for (int i = 0; i < 7; i++) args4f[i] = ops->new_int32(ops, i + 1);
    ops->call_cb(ops, fn4f, 7, args4f);
    kwcc_js_val_t result4f = ops->get_str_prop(ops, ops->global_obj, "kwcc_test_7arg");
    TEST("call_cb with 7 args: 1+2+..+7 = 28", ops->to_int32(ops, result4f) == 28);

    /* ── 5. is_function / is_undefined / is_null / is_exception ── */
    printf("[5] type checks\n");
    TEST("undefined is undefined", ops->is_undefined(ops->undefined));
    TEST("null is null", ops->is_null(ops->null));
    TEST("exception is exception", ops->is_exception(ops->exception));

    TEST("undefined is not null", !ops->is_null(ops->undefined));
    TEST("null is not undefined", !ops->is_undefined(ops->null));
    TEST("int32 is not undefined", !ops->is_undefined(ops->new_int32(ops, 1)));

    kwcc_js_val_t fn5 = ops->get_str_prop(ops, ops->global_obj, "kwcc_test_store");
    TEST("JS function is_function true", ops->is_function(ops, fn5));
    TEST("int32 is_function false", !ops->is_function(ops, ops->new_int32(ops, 1)));
    TEST("string is_function false", !ops->is_function(ops, ops->new_string(ops, "hi")));

    /* Object is not function */
    TEST("object is_function false", !ops->is_function(ops, ops->new_object(ops)));

    /* Cross-check: exception is not undefined and not null */
    TEST("exception is not undefined", !ops->is_undefined(ops->exception));
    TEST("exception is not null", !ops->is_null(ops->exception));

    /* get_class_id on known types */
    kwcc_js_val_t obj5 = ops->new_object(ops);
    int cid5 = ops->get_class_id(ops, obj5);
    TEST("get_class_id(object) = JS_CLASS_OBJECT (0)", cid5 == JS_CLASS_OBJECT);

    /* Create array in test 5 scope (kwcc_test_arr not created until test 8) */
    ops->eval(ops, "var kwcc_class_test_arr = [1,2,3];",
              strlen("var kwcc_class_test_arr = [1,2,3];"), "<test5>", JS_EVAL_REPL);
    kwcc_js_val_t arr5 = ops->get_str_prop(ops, ops->global_obj, "kwcc_class_test_arr");
    int cid5b = ops->get_class_id(ops, arr5);
    TEST("get_class_id(array) = JS_CLASS_ARRAY", cid5b == JS_CLASS_ARRAY);

    int cid5c = ops->get_class_id(ops, ops->new_int32(ops, 1));
    TEST("get_class_id(int32) < 0 (non-object)", cid5c < 0);

    /* ── 6. eval + JS_EVAL_REPL ── */
    printf("[6] eval + JS_EVAL_REPL\n");
    const char *code6a = "var kwcc_eval_test_var = 999;";
    kwcc_js_val_t ret6a = ops->eval(ops, code6a, strlen(code6a), "<test6a>", JS_EVAL_REPL);
    TEST("eval creates global var (no exception)", !ops->is_exception(ret6a));

    kwcc_js_val_t eval_var6 = ops->get_str_prop(ops, ops->global_obj, "kwcc_eval_test_var");
    TEST("eval global var = 999", ops->to_int32(ops, eval_var6) == 999);

    /* eval expression via assignment */
    const char *code6c = "var kwcc_eval_result = 40 + 2;";
    ops->eval(ops, code6c, strlen(code6c), "<test6c>", JS_EVAL_REPL);
    kwcc_js_val_t ret6c = ops->get_str_prop(ops, ops->global_obj, "kwcc_eval_result");
    TEST("eval expression assignment = 42", ops->to_int32(ops, ret6c) == 42);

    /* eval syntax error returns exception */
    const char *code6d = "var ;;;invalid;;;";
    kwcc_js_val_t ret6d = ops->eval(ops, code6d, strlen(code6d), "<test6d>", JS_EVAL_REPL);
    TEST("eval syntax error returns exception", ops->is_exception(ret6d));

    /* eval runtime error returns exception */
    const char *code6e = "var kwcc_test_undef; kwcc_test_undef();";
    kwcc_js_val_t ret6e = ops->eval(ops, code6e, strlen(code6e), "<test6e>", JS_EVAL_REPL);
    TEST("eval runtime error returns exception", ops->is_exception(ret6e));

    /* eval creates function that can be called later */
    const char *code6f = "var kwcc_test_fn_eval = function() { return 77; };";
    ops->eval(ops, code6f, strlen(code6f), "<test6f>", JS_EVAL_REPL);
    kwcc_js_val_t fn6f = ops->get_str_prop(ops, ops->global_obj, "kwcc_test_fn_eval");
    TEST("eval creates callable function", ops->is_function(ops, fn6f));

    /* eval can modify existing global */
    ops->eval(ops, "kwcc_eval_test_var = 1111;",
              strlen("kwcc_eval_test_var = 1111;"), "<test6g>", JS_EVAL_REPL);
    kwcc_js_val_t mod6g = ops->get_str_prop(ops, ops->global_obj, "kwcc_eval_test_var");
    TEST("eval modifies existing global var", ops->to_int32(ops, mod6g) == 1111);

    /* ── 7. notify_js + ack_cleanup ── */
    printf("[7] notify_js + ack_cleanup\n");
    /* Register a handler via $notify.on */
    const char *code7 =
        "var kwcc_notify_received = null;\n"
        "var kwcc_notify_id = null;\n"
        "var kwcc_notify_data = null;\n"
        "$notify.on('testtype', function(event, id, data) {\n"
        "    kwcc_notify_received = event;\n"
        "    kwcc_notify_id = id;\n"
        "    kwcc_notify_data = data;\n"
        "});\n";
    ops->eval(ops, code7, strlen(code7), "<test7>", JS_EVAL_REPL);

    /* 7a: notify_js with NULL ack_cleanup */
    kwcc_js_val_t data7 = ops->new_object(ops);
    ops->set_str_prop(ops, data7, "status", ops->new_string(ops, "ok"));
    ops->notify_js(ops, "testtype", "testevent", "req_007", data7, NULL);

    /* Verify handler was invoked */
    kwcc_js_val_t recv7 = ops->get_str_prop(ops, ops->global_obj, "kwcc_notify_received");
    kwcc_js_cstr_buf_t rbuf7;
    const char *rstr7 = ops->to_cstring(ops, recv7, &rbuf7);
    TEST("notify_js delivers event to handler", rstr7 != NULL && strcmp(rstr7, "testevent") == 0);

    kwcc_js_val_t id7 = ops->get_str_prop(ops, ops->global_obj, "kwcc_notify_id");
    kwcc_js_cstr_buf_t ibuf7;
    const char *idstr7 = ops->to_cstring(ops, id7, &ibuf7);
    TEST("notify_js delivers id to handler", idstr7 != NULL && strcmp(idstr7, "req_007") == 0);

    /* Verify data object was passed */
    kwcc_js_val_t dat7 = ops->get_str_prop(ops, ops->global_obj, "kwcc_notify_data");
    kwcc_js_val_t status7 = ops->get_str_prop(ops, dat7, "status");
    kwcc_js_cstr_buf_t dbuf7;
    const char *dstr7 = ops->to_cstring(ops, status7, &dbuf7);
    TEST("notify_js delivers data.status = 'ok'", dstr7 != NULL && strcmp(dstr7, "ok") == 0);

    /* 7b: notify_js with non-NULL ack_cleanup — verify it's called */
    g_kwcc_test_ack_called = 0;
    g_kwcc_test_ack_id[0] = '\0';

    kwcc_js_val_t data7b = ops->new_object(ops);
    ops->set_str_prop(ops, data7b, "msg", ops->new_string(ops, "with_ack"));
    ops->notify_js(ops, "testtype", "acktest", "req_008", data7b, kwcc_test_ack_cleanup);

    TEST("ack_cleanup was called", g_kwcc_test_ack_called == 1);
    TEST("ack_cleanup received correct id", strcmp(g_kwcc_test_ack_id, "req_008") == 0);

    /* Verify handler still received the event (ack_cleanup called before call_cb,
       but data is already in GC heap so handler should still work) */
    kwcc_js_val_t recv7b = ops->get_str_prop(ops, ops->global_obj, "kwcc_notify_received");
    kwcc_js_cstr_buf_t rbuf7b;
    const char *rstr7b = ops->to_cstring(ops, recv7b, &rbuf7b);
    TEST("notify_js with ack_cleanup still delivers event",
         rstr7b != NULL && strcmp(rstr7b, "acktest") == 0);

    /* 7c: unregistered type — handler not called, no crash */
    ops->eval(ops,
        "var kwcc_notify_received_before = kwcc_notify_received;",
        strlen("var kwcc_notify_received_before = kwcc_notify_received;"),
        "<test7c>", JS_EVAL_REPL);
    kwcc_js_val_t data7c = ops->new_object(ops);
    ops->notify_js(ops, "unknowntype", "event", "req_009", data7c, NULL);
    kwcc_js_val_t recv7c = ops->get_str_prop(ops, ops->global_obj, "kwcc_notify_received");
    kwcc_js_val_t before7c = ops->get_str_prop(ops, ops->global_obj, "kwcc_notify_received_before");
    kwcc_js_cstr_buf_t rbuf7c, bbuf7c;
    const char *rstr7c = ops->to_cstring(ops, recv7c, &rbuf7c);
    const char *bstr7c = ops->to_cstring(ops, before7c, &bbuf7c);
    TEST("notify_js with unregistered type: no handler called (no crash)",
         rstr7c != NULL && bstr7c != NULL && strcmp(rstr7c, bstr7c) == 0);

    /* 7d: multiple notify_js calls in sequence */
    ops->notify_js(ops, "testtype", "first", "req_a", ops->new_object(ops), NULL);
    kwcc_js_val_t recv7d = ops->get_str_prop(ops, ops->global_obj, "kwcc_notify_received");
    kwcc_js_cstr_buf_t rbuf7d;
    const char *rstr7d = ops->to_cstring(ops, recv7d, &rbuf7d);
    ops->notify_js(ops, "testtype", "second", "req_b", ops->new_object(ops), NULL);
    kwcc_js_val_t recv7d2 = ops->get_str_prop(ops, ops->global_obj, "kwcc_notify_received");
    kwcc_js_cstr_buf_t rbuf7d2;
    const char *rstr7d2 = ops->to_cstring(ops, recv7d2, &rbuf7d2);
    TEST("notify_js sequential: first then second",
         rstr7d != NULL && strcmp(rstr7d, "first") == 0 &&
         rstr7d2 != NULL && strcmp(rstr7d2, "second") == 0);

    /* 7e: ack_cleanup called before JS handler (order verification) */
    /* We reuse the same handler which stores event. ack_cleanup runs first.
       After notify_js, handler should have run (event = "order_test"). */
    g_kwcc_test_ack_called = 0;
    ops->notify_js(ops, "testtype", "order_test", "req_order", ops->new_object(ops), kwcc_test_ack_cleanup);
    /* If ack_cleanup ran before call_cb, and call_cb ran, handler was invoked */
    kwcc_js_val_t recv7e = ops->get_str_prop(ops, ops->global_obj, "kwcc_notify_received");
    kwcc_js_cstr_buf_t rbuf7e;
    const char *rstr7e = ops->to_cstring(ops, recv7e, &rbuf7e);
    TEST("notify_js: ack_cleanup + handler both ran",
         g_kwcc_test_ack_called == 1 &&
         rstr7e != NULL && strcmp(rstr7e, "order_test") == 0);

    /* 7f: notify_js with empty data object */
    kwcc_js_val_t empty_data7 = ops->new_object(ops);
    ops->notify_js(ops, "testtype", "empty_data", "req_empty", empty_data7, NULL);
    kwcc_js_val_t dat7f = ops->get_str_prop(ops, ops->global_obj, "kwcc_notify_data");
    TEST("notify_js with empty data object: handler received object",
         !ops->is_undefined(dat7f) && !ops->is_exception(dat7f));

    /* ── 8. array_length + array_get ── */
    printf("[8] array_length + array_get\n");
    /* Create array via eval */
    ops->eval(ops, "var kwcc_test_arr = [10, 20, 30];",
              strlen("var kwcc_test_arr = [10, 20, 30];"),
              "<test8>", JS_EVAL_REPL);

    kwcc_js_val_t arr8 = ops->get_str_prop(ops, ops->global_obj, "kwcc_test_arr");
    TEST("array is not undefined", !ops->is_undefined(arr8));

    int len8 = ops->array_length(ops, arr8);
    TEST("array_length = 3", len8 == 3);

    kwcc_js_val_t elem0 = ops->array_get(ops, arr8, 0);
    TEST("array_get(0) = 10", ops->to_int32(ops, elem0) == 10);

    kwcc_js_val_t elem1 = ops->array_get(ops, arr8, 1);
    TEST("array_get(1) = 20", ops->to_int32(ops, elem1) == 20);

    kwcc_js_val_t elem2 = ops->array_get(ops, arr8, 2);
    TEST("array_get(2) = 30", ops->to_int32(ops, elem2) == 30);

    /* Out of bounds returns undefined */
    kwcc_js_val_t elem3 = ops->array_get(ops, arr8, 99);
    TEST("array_get(out of bounds) is undefined", ops->is_undefined(elem3));

    /* Empty array */
    ops->eval(ops, "var kwcc_test_empty_arr = [];",
              strlen("var kwcc_test_empty_arr = [];"), "<test8b>", JS_EVAL_REPL);
    kwcc_js_val_t empty_arr8 = ops->get_str_prop(ops, ops->global_obj, "kwcc_test_empty_arr");
    TEST("empty array length = 0", ops->array_length(ops, empty_arr8) == 0);

    /* String array */
    ops->eval(ops, "var kwcc_test_str_arr = ['foo', 'bar'];",
              strlen("var kwcc_test_str_arr = ['foo', 'bar'];"), "<test8c>", JS_EVAL_REPL);
    kwcc_js_val_t str_arr8 = ops->get_str_prop(ops, ops->global_obj, "kwcc_test_str_arr");
    TEST("string array length = 2", ops->array_length(ops, str_arr8) == 2);
    kwcc_js_val_t sa0 = ops->array_get(ops, str_arr8, 0);
    kwcc_js_cstr_buf_t sabuf0;
    const char *sastr0 = ops->to_cstring(ops, sa0, &sabuf0);
    TEST("string array[0] = 'foo'", sastr0 != NULL && strcmp(sastr0, "foo") == 0);
    kwcc_js_val_t sa1 = ops->array_get(ops, str_arr8, 1);
    kwcc_js_cstr_buf_t sabuf1;
    const char *sastr1 = ops->to_cstring(ops, sa1, &sabuf1);
    TEST("string array[1] = 'bar'", sastr1 != NULL && strcmp(sastr1, "bar") == 0);

    /* Mixed type array */
    ops->eval(ops, "var kwcc_test_mix_arr = [1, 'two', true];",
              strlen("var kwcc_test_mix_arr = [1, 'two', true];"), "<test8d>", JS_EVAL_REPL);
    kwcc_js_val_t mix_arr8 = ops->get_str_prop(ops, ops->global_obj, "kwcc_test_mix_arr");
    TEST("mixed array length = 3", ops->array_length(ops, mix_arr8) == 3);
    TEST("mixed array[0] = 1", ops->to_int32(ops, ops->array_get(ops, mix_arr8, 0)) == 1);
    kwcc_js_cstr_buf_t mbuf1;
    const char *mstr1 = ops->to_cstring(ops, ops->array_get(ops, mix_arr8, 1), &mbuf1);
    TEST("mixed array[1] = 'two'", mstr1 != NULL && strcmp(mstr1, "two") == 0);

    /* array_get(0) on empty array returns undefined */
    TEST("empty array_get(0) is undefined", ops->is_undefined(ops->array_get(ops, empty_arr8, 0)));

    /* ── Summary ── */
    printf("\n=== %d passed, %d failed, %d total ===\n",
           tests_passed, tests_failed, tests_passed + tests_failed);

    JS_FreeContext(ctx);
    free(mem_buf);
    kwcc_mempool_shutdown();
    return (tests_failed == 0) ? 0 : 1;
}
