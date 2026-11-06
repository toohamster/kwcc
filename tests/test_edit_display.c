/* test_edit_display.c — 多行展示路径防御测试（vg=NULL harness）
 *
 * 事实边界：行分解/字体度量依赖真实 NVGcontext（g_kwcc_vg），headless 无法
 * 提供 GL 上下文 → 多行展示的行为级验证（空行保留/滚动方向）归 L2 真实 GUI。
 * 本测试验证：vg=NULL 时 textbox 多行路径**不崩溃**（防御生效）+ 字段背景命令仍产出。
 * Run: ./tests/bin/test_edit_display
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nanovg/nanovg.h"
#include "kwcc.h"
#include "microui/microui.h"

NVGcontext *g_kwcc_vg = NULL;   /* harness：无渲染环境 */
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

static const char *SETUP_JS =
    "var $bus = new Object();\n"
    "$bus.handlers = new Object();\n"
    "$bus.on = function() {};\n"
    "$bus.emit = function() {};\n"
    "function onFrame() {\n"
    "    ui.sync('form', 1);\n"
    "    ui.beginWindow('F', 50, 50, 300, 200, 0, 'form/window');\n"
    "    ui.layoutRow(70, -1);\n"
    "    ui.textbox('bio', 'A\\n\\nB\\nCCC', 'form/bio', 1);\n"
    "    ui.endWindow();\n"
    "}\n";

static int count_rect_commands(void) {
    mu_Context *mu = kwcc_get_mu();
    mu_Command *cmd = NULL;
    int n = 0;
    while (mu_next_command(mu, &cmd)) {
        if (cmd->base.type == MU_COMMAND_RECT) n++;
    }
    return n;
}

int main(void) {
    kwcc_mempool_init();
    kwcc_io_init();
    kwcc_http_init();
    JSContext *ctx = kwcc_create_js();
    if (!ctx) return 1;
    kwcc_ui_bus_set_js_ctx(ctx);
    kwcc_ui_init();
    kwcc_register_ui(ctx);
    /* g_kwcc_vg 保持 NULL：防御路径 */

    JSValue r = JS_Eval(ctx, SETUP_JS, strlen(SETUP_JS), "<setup>", JS_EVAL_REPL);
    TEST(setup_ok, !JS_IsException(r));

    /* 多行 textbox 在 vg=NULL 下不崩溃 */
    kwcc_process_js(ctx, "onFrame();");
    TEST(multiline_no_crash_without_vg, 1);   /* 存活即通过 */

    /* 字段背景命令仍产出（bg 绘制无 vg 依赖） */
    int rects = count_rect_commands();
    TEST(field_bg_command_present, rects > 0);

    /* 持续帧存活 */
    kwcc_process_js(ctx, "onFrame();");
    kwcc_process_js(ctx, "onFrame();");
    TEST(survives_frames, 1);

    printf("=== test_edit_display: %d passed, %d failed ===\n",
           tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
