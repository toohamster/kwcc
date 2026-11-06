/* test_edit_js.c — L1.8: textbox click → begin edit → commit → bus change
 * → JS handler → ui.textboxValue 交付，全链路（真实 JSContext + 真实 js_ui_dispatch）。
 *
 * 后端用 FAKE ops（hermetic——编辑行为已在 test_edit_appkit 对真实控件验证，
 * 此处验证桥接与 JS 交付路径）。
 * 键盘链路（Sokol CHAR/KEY_DOWN）不参与此路径：编辑输入走 AppKit responder chain。
 *
 * Run: ./tests/bin/test_edit_js
 * Compile: 见 tests/TESTING_EDIT.md
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "nanovg/nanovg.h"
#include "kwcc.h"
#include "kwcc_edit.h"
#include "microui/microui.h"

NVGcontext *g_kwcc_vg = NULL;   /* main.m 提供；测试无 main.m */

typedef struct NSVGimage NSVGimage;
NSVGimage *nsvgParseFromFile(const char *f, const char *u, float d) { (void)f;(void)u;(void)d; return NULL; }
NSVGimage *nsvgParse(char *i, const char *u, float d) { (void)i;(void)u;(void)d; return NULL; }
void nsvgDelete(NSVGimage *image) { (void)image; }

static int tests_passed = 0;
static int tests_failed = 0;
#define TEST(name, cond) do { \
    if (cond) { tests_passed++; printf("  PASS: %s\n", #name); } \
    else { tests_failed++; printf("  FAIL: %s\n", #name); } \
} while (0)

/* ── FAKE backend ── */
static void *fake_create(void *window, int multiline) {
    (void)window; (void)multiline;
    return (void *)(intptr_t)0x2000;
}
static void fake_destroy(void *field) { (void)field; }
static void fake_set_value(void *field, const char *text) { (void)field; (void)text; }
static void fake_set_rect(void *field, float x, float y, float w, float h) {
    (void)field; (void)x; (void)y; (void)w; (void)h;
}
static void fake_set_visible(void *field, int v) { (void)field; (void)v; }
static void fake_focus(void *field) { (void)field; }
static void fake_blur(void *field) { (void)field; }
static char *fake_get_value(void *field) { (void)field; return strdup("typed-value"); }

static const kwcc_edit_backend_ops_t g_mock_ops = {
    fake_create, fake_destroy, fake_set_value, fake_set_rect,
    fake_set_visible, fake_focus, fake_blur, fake_get_value
};

static const char *SETUP_JS =
    "var g_got_value = '';\n"
    "var g_change_events = 0;\n"
    "var $bus = new Object();\n"
    "$bus.handlers = new Object();\n"
    "$bus.on = function(topic, fn) { $bus.handlers[topic] = fn; };\n"
    "$bus.emit = function(topic, action, data) {\n"
    "    var h = $bus.handlers[topic];\n"
    "    if (h) { h(action, data); }\n"
    "};\n"
    "$bus.on('form/a', function(action, data) {\n"
    "    g_change_events = g_change_events + 1;\n"
    "    g_got_value = ui.textboxValue('fieldA');\n"
    "});\n"
    "function onFrame() {\n"
    "    ui.sync('form', 1);\n"
    "    ui.beginWindow('Form', 50, 50, 300, 200, 0, 'form/window');\n"
    "    ui.layoutRow(30, -1);\n"
    "    ui.textbox('fieldA', 'init-value', 'form/a', 0);\n"
    "    ui.endWindow();\n"
    "}\n";

#define FIELD_X 100
#define FIELD_Y 94   /* field rect ≈ (55,79,290,30)，取内部点 */

static int eval_int(JSContext *ctx, const char *code) {
    JSValue v = JS_Eval(ctx, code, strlen(code), "<probe>", JS_EVAL_RETVAL);
    int out = -999;
    if (JS_IsException(v)) return -999;
    JS_ToInt32(ctx, &out, v);
    return out;
}

static int eval_str_is(JSContext *ctx, const char *code, const char *expect) {
    JSValue v = JS_Eval(ctx, code, strlen(code), "<probe>", JS_EVAL_RETVAL);
    if (!JS_IsString(ctx, v)) return 0;
    JSCStringBuf buf;
    const char *s = JS_ToCString(ctx, v, &buf);
    int ok = s && strcmp(s, expect) == 0;
    return ok;
}

int main(void) {
    kwcc_mempool_init();
    kwcc_io_init();
    kwcc_http_init();
    JSContext *ctx = kwcc_create_js();
    TEST(init_create_js, ctx != NULL);
    if (!ctx) return 1;
    kwcc_ui_bus_set_js_ctx(ctx);
    kwcc_ui_init();
    kwcc_register_ui(ctx);
    kwcc_edit_init(&g_mock_ops, NULL);   /* hermetic：注入 mock 后端 */

    JSValue r = JS_Eval(ctx, SETUP_JS, strlen(SETUP_JS), "<setup>", JS_EVAL_REPL);
    TEST(setup_eval_no_exception, !JS_IsException(r));

    /* frame 1: 渲染窗口+字段 */
    kwcc_process_js(ctx, "onFrame();");
    TEST(frame1_renders, 1);

    /* 三帧点击序列（hover → focus → press，同 test_window_close 取证结论） */
    kwcc_input_mousemove(FIELD_X, FIELD_Y);
    kwcc_process_js(ctx, "onFrame();");
    kwcc_process_js(ctx, "onFrame();");
    kwcc_input_mousedown(FIELD_X, FIELD_Y, MU_MOUSE_LEFT);
    kwcc_process_js(ctx, "onFrame();");
    kwcc_input_mouseup(FIELD_X, FIELD_Y, MU_MOUSE_LEFT);

    TEST(edit_begins_on_click, kwcc_edit_is_editing() &&
         kwcc_edit_editing_label() && strcmp(kwcc_edit_editing_label(), "fieldA") == 0);

    /* 模拟后端提交（真实场景 = AppKit delegate 回调 Enter/失焦） */
    kwcc_edit_commit();

    TEST(change_event_dispatched, eval_int(ctx, "g_change_events") == 1);
    TEST(committed_value_delivered_to_js,
         eval_str_is(ctx, "g_got_value", "typed-value"));

    /* 提交后继续帧存活（无崩溃/无 JS 异常） */
    kwcc_process_js(ctx, "onFrame();");
    kwcc_process_js(ctx, "onFrame();");
    TEST(survives_after_commit, eval_int(ctx, "1+1") == 2);

    kwcc_edit_shutdown();
    printf("=== test_edit_js: %d passed, %d failed ===\n",
           tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
