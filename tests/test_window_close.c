/* test_window_close.c — Gate-0 reproduction: window X close → program death
 *
 * Reproduces the production path exactly:
 *   kwcc_input_mousemove/mousedown (same entry as main.m event routing)
 *   → kwcc_process_js frame loop (begin_frame → mu_begin → JS_Eval → mu_end)
 *   → mu_begin_window_ex close hook → dispatch → JS handler
 *   → next frames exercise the visibility-intercept path
 *
 * Facts this test is built on (all from source, no guesses):
 *   - microui.c mu_begin_window_ex: titlebar tr = rect with h=title_height;
 *     close btn r = (tr.x + tr.w - tr.h, tr.y, tr.h, tr.h)
 *   - mu_update_control: hover requires mouseover && !mouse_down (needs a
 *     prior frame with mouse moved but button up); focus set on hover+pressed;
 *     hook fires when mouse_pressed == LEFT && id == focus (same frame)
 *   - mu_end resets mouse_pressed each frame
 *   - kwcc_process_js: begin_frame → mu_begin → JS_Eval → mu_end
 *
 * Input sequence (derived from the above facts, v2 — v1 missed the click:
 * hover_root comes from the PREVIOUS frame's next_hover_root via mu_begin,
 * and hover requires a frame with mouse over + button UP; pressing one frame
 * after the move leaves mouse_down=1 so hover can never be established):
 *   frame 1: no input            → window container created
 *   after 1: mousemove(298,62)   → frame 2: next_hover_root = window
 *   frame 2 (clean):             → frame 3: hover_root set; hover = close id
 *   after 3: mousedown(298,62)   → frame 4: focus + pressed → hook fires
 *   after 4: mouseup(298,62)     → frames 5..64: intercept path
 *
 * Run under lldb (build_pitfalls.md methodology):
 *   lldb -s /tmp/lldb_script_close.txt -- ./tests/bin/test_window_close
 *
 * Test conventions follow testing_methodology.md:
 *   standard TEST macro, tests/bin/ output, self-contained test points,
 *   JS_EVAL_RETVAL probes, incrementing callback counter
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nanovg/nanovg.h"
#include "kwcc.h"
#include "microui/microui.h"

/* main.m normally provides these; test links without main.m */
NVGcontext *g_kwcc_vg = NULL;

/* nanosvg implementation (NANOSVG_IMPLEMENTATION) lives in main.m; SVG path
 * is not exercised by this test — provide link stubs */
typedef struct NSVGimage NSVGimage;
NSVGimage *nsvgParseFromFile(const char *filename, const char *units, float dpi) {
    (void)filename; (void)units; (void)dpi; return NULL;
}
NSVGimage *nsvgParse(char *input, const char *units, float dpi) {
    (void)input; (void)units; (void)dpi; return NULL;
}
void nsvgDelete(NSVGimage *image) { (void)image; }

/* ── Standard assertion macro (testing_methodology.md template) ── */
static int tests_passed = 0;
static int tests_failed = 0;
#define TEST(name, cond) do { \
    if (cond) { tests_passed++; printf("  PASS: %s\n", #name); } \
    else { tests_failed++; printf("  FAIL: %s\n", #name); } \
} while (0)

/* ── Test JS: faithful minimal replica of the production chain ──
 * handler = test.js $bus.on($topics.test.window) → $store.dispatch("test","closeWindow")
 * view    = test_view.js beginWindow(..., topic) + body widgets + endWindow
 * onFrame = main.js auto ui.sync(key, visible) then render
 * Self-contained: this eval creates every global it uses.
 */
static const char *SETUP_JS =
    "var g_close_hit = 0;\n"
    "var $bus = new Object();\n"
    "$bus.handlers = new Object();\n"
    "$bus.on = function(topic, fn) { $bus.handlers[topic] = fn; };\n"
    "$bus.emit = function(topic, action, data) {\n"
    "    var h = $bus.handlers[topic];\n"
    "    if (h) { h(action, data); }\n"
    "};\n"
    "var $store = new Object();\n"
    "$store.state = new Object();\n"
    "$store.state.test = new Object();\n"
    "$store.state.test.count = 0;\n"
    "$store.state.test.visible = 1;\n"
    "$store.dispatch = function(module, action) {\n"
    "    if (module == 'test' && action == 'closeWindow') {\n"
    "        $store.state.test.visible = 0;\n"
    "    }\n"
    "};\n"
    "$bus.on('test/window', function(action, data) {\n"
    "    g_close_hit = g_close_hit + 1;\n"
    "    $store.dispatch('test', 'closeWindow');\n"
    "});\n"
    "function onFrame() {\n"
    "    ui.sync('test', $store.state.test.visible);\n"
    "    ui.beginWindow('Test Counter', 50, 50, 260, 160, 0, 'test/window');\n"
    "    ui.layoutRow(30, -1);\n"
    "    ui.label('Count: 0');\n"
    "    ui.layoutRow(30, 100, 100);\n"
    "    ui.button('+', 'test/btn');\n"
    "    ui.button('Reset', 'test/reset');\n"
    "    ui.endWindow();\n"
    "}\n";

#define TOTAL_FRAMES 64
#define CLOSE_X 298   /* window(50,50,260,160): close r=(286,50,24,24), center */
#define CLOSE_Y 62

/* Probe via JS_EVAL_RETVAL (testing_methodology.md: RETVAL, not REPL+re-read) */
static int eval_int(JSContext *ctx, const char *code) {
    JSValue v = JS_Eval(ctx, code, strlen(code), "<probe>", JS_EVAL_RETVAL);
    int out = -999;
    if (JS_IsException(v)) return -999;
    JS_ToInt32(ctx, &out, v);
    return out;
}

static int count_commands(void) {
    mu_Context *mu = kwcc_get_mu();
    mu_Command *cmd = NULL;
    int n = 0;
    while (mu_next_command(mu, &cmd)) n++;
    return n;
}

int main(void) {
    /* init: exact production order from main.m init() (minus sokol/nvg) */
    kwcc_mempool_init();
    kwcc_io_init();
    kwcc_http_init();
    JSContext *ctx = kwcc_create_js();
    TEST(init_create_js, ctx != NULL);
    if (!ctx) { printf("RESULT FAIL: cannot continue without JSContext\n"); return 1; }
    kwcc_ui_bus_set_js_ctx(ctx);
    kwcc_ui_init();
    kwcc_register_ui(ctx);

    /* setup eval: JS_EVAL_REPL to create globals (production loads main.js once) */
    JSValue r = JS_Eval(ctx, SETUP_JS, strlen(SETUP_JS), "<setup>", JS_EVAL_REPL);
    TEST(setup_eval_no_exception, !JS_IsException(r));

    /* ── frame 1: create window (no input) ── */
    kwcc_process_js(ctx, "onFrame();");
    int cmds_f1 = count_commands();
    printf("  INFO: frame 1 commands=%d (window rendered)\n", cmds_f1);
    TEST(frame1_window_renders, cmds_f1 > 0);

    /* ── after frame 1: move mouse onto close button ── */
    kwcc_input_mousemove(CLOSE_X, CLOSE_Y);

    /* ── frame 2: next_hover_root = window (hover_root updates next frame) ── */
    kwcc_process_js(ctx, "onFrame();");
    printf("  INFO: frame 2 commands=%d (next_hover_root set)\n", count_commands());

    /* ── frame 3: hover_root = window → hover = close id (button still up) ── */
    kwcc_process_js(ctx, "onFrame();");
    printf("  INFO: frame 3 commands=%d (hover should be set on close id)\n",
           count_commands());

    /* ── after frame 3: press → frame 4 must fire hook ── */
    kwcc_input_mousedown(CLOSE_X, CLOSE_Y, MU_MOUSE_LEFT);
    kwcc_process_js(ctx, "onFrame();");
    int cmds_f4 = count_commands();
    int hit = eval_int(ctx, "g_close_hit");
    int vis = eval_int(ctx, "$store.state.test.visible");
    printf("  INFO: frame 4 commands=%d close_hit=%d visible=%d\n", cmds_f4, hit, vis);
    TEST(close_handler_fired_once, hit == 1);
    TEST(visible_set_zero_after_close, vis == 0);
    kwcc_input_mouseup(CLOSE_X, CLOSE_Y, MU_MOUSE_LEFT);

    /* ── frames 5..N: intercept path (visible=0) — death zone under lldb ── */
    int loop_completed = 1;
    for (int f = 5; f <= TOTAL_FRAMES; f++) {
        kwcc_process_js(ctx, "onFrame();");
        int cmds = count_commands();
        if (f <= 6 || f % 10 == 0) {
            printf("  INFO: frame %d commands=%d\n", f, cmds);
        }
        if (f == TOTAL_FRAMES) loop_completed = 1;
    }
    TEST(survived_intercept_frames, loop_completed);

    /* engine still functional after all frames */
    int probe_alive = eval_int(ctx, "1 + 1");
    TEST(engine_eval_alive_after_frames, probe_alive == 2);

    printf("=== test_window_close: %d passed, %d failed ===\n",
           tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
