/* bridge.c — mquickjs ↔ microui bridge */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mquickjs/mquickjs.h"
#include "microui/microui.h"
#include "nanovg/nanovg.h"
#include "llog.h"

#include "mquickjs_stubs.h"
#include "mquickjs/mqjs_stdlib.h"

/* Extern: NVG context set in main.m */
extern NVGcontext *vg;

#define BRIDGE_MEM_SIZE (4 * 1024 * 1024)

static mu_Context g_mu;
static mu_Real    g_slider_val = 0.5f;  /* persistent slider state (microui uses ptr as ID) */

/* ── microui text measurement callbacks (real font metrics) ── */

static int mu_text_width(mu_Font font, const char *str, int len) {
    (void)font;
    if (!str || !vg) return len > 0 ? len * 7 : 0;
    if (len < 0) len = (int)strlen(str);
    float bounds[4];
    nvgFontFace(vg, "sans");
    nvgFontSize(vg, 14);
    nvgTextBounds(vg, 0, 0, str, str + len, bounds);
    return (int)(bounds[2] - bounds[0]);
}

static int mu_text_height(mu_Font font) {
    (void)font;
    if (!vg) return 14;
    float bounds[4];
    nvgFontFace(vg, "sans");
    nvgFontSize(vg, 14);
    nvgTextBounds(vg, 0, 0, "Hy", NULL, bounds);
    return (int)(bounds[3] - bounds[1]);
}

/* ── mquickjs UI callback: dispatched by kwcc_ui() ────────────── */

static JSValue js_ui_dispatch(JSContext *ctx, const char *method,
                               int argc, JSValue *argv) {
    (void)argv;
    if (strcmp(method, "beginWindow") == 0) {
        JSCStringBuf buf;
        const char *title = JS_ToCString(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &buf);
        int x = 50, y = 50, w = 400, h = 300;
        int opt = 0;
        if (argc > 1) JS_ToInt32(ctx, &x, argv[1]);
        if (argc > 2) JS_ToInt32(ctx, &y, argv[2]);
        if (argc > 3) JS_ToInt32(ctx, &w, argv[3]);
        if (argc > 4) JS_ToInt32(ctx, &h, argv[4]);
        if (argc > 5) JS_ToInt32(ctx, &opt, argv[5]);
        mu_begin_window_ex(&g_mu, title ? title : "", (mu_Rect){x, y, w, h}, opt);
        return JS_UNDEFINED;
    }
    if (strcmp(method, "endWindow") == 0) {
        mu_end_window(&g_mu);
        return JS_UNDEFINED;
    }
    if (strcmp(method, "beginPanel") == 0) {
        JSCStringBuf buf;
        const char *name = JS_ToCString(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &buf);
        int opt = 0;
        if (argc > 1) JS_ToInt32(ctx, &opt, argv[1]);
        mu_begin_panel_ex(&g_mu, name ? name : "", opt);
        return JS_UNDEFINED;
    }
    if (strcmp(method, "endPanel") == 0) {
        mu_end_panel(&g_mu);
        return JS_UNDEFINED;
    }
    if (strcmp(method, "button") == 0) {
        JSCStringBuf buf;
        const char *text = JS_ToCString(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &buf);
        int res = mu_button_ex(&g_mu, text ? text : "", 0, MU_OPT_ALIGNCENTER);
        return JS_NewBool(res & MU_RES_SUBMIT);
    }
    if (strcmp(method, "label") == 0) {
        JSCStringBuf buf;
        const char *text = JS_ToCString(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &buf);
        mu_label(&g_mu, text ? text : "");
        return JS_UNDEFINED;
    }
    if (strcmp(method, "slider") == 0) {
        double val = g_slider_val, low = 0, high = 1;
        if (argc > 1) JS_ToNumber(ctx, &val, argv[1]);
        if (argc > 2) JS_ToNumber(ctx, &low, argv[2]);
        if (argc > 3) JS_ToNumber(ctx, &high, argv[3]);
        g_slider_val = (mu_Real)val;
        mu_Real flow = (mu_Real)low, fhigh = (mu_Real)high;
        mu_slider_ex(&g_mu, &g_slider_val, flow, fhigh, 0.01f, "%g", MU_OPT_ALIGNCENTER);
        return JS_NewFloat64(ctx, g_slider_val);
    }
    if (strcmp(method, "layoutRow") == 0) {
        int height = 20;
        int widths[MU_MAX_WIDTHS];
        int items = 0;
        if (argc > 0) JS_ToInt32(ctx, &height, argv[0]);
        for (int i = 1; i < argc && i <= MU_MAX_WIDTHS; i++) {
            JS_ToInt32(ctx, &widths[i - 1], argv[i]);
            items = i;
        }
        mu_layout_row(&g_mu, items, items > 0 ? widths : NULL, height);
        return JS_UNDEFINED;
    }
    if (strcmp(method, "setNext") == 0) {
        int x = 0, y = 0, w = 0, h = 0;
        if (argc > 0) JS_ToInt32(ctx, &x, argv[0]);
        if (argc > 1) JS_ToInt32(ctx, &y, argv[1]);
        if (argc > 2) JS_ToInt32(ctx, &w, argv[2]);
        if (argc > 3) JS_ToInt32(ctx, &h, argv[3]);
        mu_layout_set_next(&g_mu, (mu_Rect){x, y, w, h}, 0);
        return JS_UNDEFINED;
    }
    if (strcmp(method, "rect") == 0) {
        int x = 0, y = 0, w = 0, h = 0;
        int r = 50, g = 50, b = 50;
        if (argc > 0) JS_ToInt32(ctx, &x, argv[0]);
        if (argc > 1) JS_ToInt32(ctx, &y, argv[1]);
        if (argc > 2) JS_ToInt32(ctx, &w, argv[2]);
        if (argc > 3) JS_ToInt32(ctx, &h, argv[3]);
        if (argc > 4) JS_ToInt32(ctx, &r, argv[4]);
        if (argc > 5) JS_ToInt32(ctx, &g, argv[5]);
        if (argc > 6) JS_ToInt32(ctx, &b, argv[6]);
        mu_draw_rect(&g_mu, (mu_Rect){x, y, w, h}, (mu_Color){r, g, b, 255});
        return JS_UNDEFINED;
    }
    if (strcmp(method, "display") == 0) {
        /* Draw a display area: dark background + right-aligned white text */
        JSCStringBuf buf;
        const char *text = JS_ToCString(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &buf);
        mu_Rect r = mu_layout_next(&g_mu);
        /* dark background */
        mu_draw_rect(&g_mu, r, (mu_Color){30, 30, 30, 255});
        /* right-aligned white text */
        if (text) {
            int tw = g_mu.text_width(NULL, text, -1);
            int th = g_mu.text_height(NULL);
            int tx = r.x + r.w - tw - 8;  /* 8px right padding */
            int ty = r.y + (r.h - th) / 2;
            mu_draw_text(&g_mu, NULL, text, -1, (mu_Vec2){tx, ty},
                (mu_Color){240, 240, 240, 255});
        }
        return JS_UNDEFINED;
    }
    if (strcmp(method, "textCentered") == 0) {
        JSCStringBuf buf;
        const char *text = JS_ToCString(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &buf);
        mu_Rect r = mu_layout_next(&g_mu);
        mu_draw_control_text(&g_mu, text ? text : "", r, MU_COLOR_TEXT, MU_OPT_ALIGNCENTER);
        return JS_UNDEFINED;
    }
    return JS_UNDEFINED;
}

/* ── Public API ───────────────────────────────────────────────── */

void bridge_init(void) {
    mu_init(&g_mu);
    g_mu.text_width  = mu_text_width;
    g_mu.text_height = mu_text_height;
}

void bridge_free(void) {
}

JSContext *bridge_create_js(void) {
    void *mem_buf = malloc(BRIDGE_MEM_SIZE);
    JSContext *ctx = JS_NewContext(mem_buf, BRIDGE_MEM_SIZE, &js_stdlib);
    if (!ctx) {
        log_fatal("JS_NewContext failed (not enough memory?)");
        return NULL;
    }

    kwcc_set_ui_callback(js_ui_dispatch);

    /* Create ui object and methods from C */
    JSValue global_obj = JS_GetGlobalObject(ctx);
    JSValue ui = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, global_obj, "ui", ui);

    /* Note: JS_Eval requires strlen() for input_len, not 0 */
    const char *methods_js =
        "ui.beginWindow = function(t,x,y,w,h,opt) { kwcc_ui('beginWindow',t,x,y,w,h,opt||0); };\n"
        "ui.endWindow = function() { kwcc_ui('endWindow'); };\n"
        "ui.beginPanel = function(n,opt) { kwcc_ui('beginPanel',n,opt||0); };\n"
        "ui.endPanel = function() { kwcc_ui('endPanel'); };\n"
        "ui.button = function(t) { return kwcc_ui('button',t); };\n"
        "ui.label = function(t) { kwcc_ui('label',t); };\n"
        "ui.slider = function(t,v,lo,hi) { return kwcc_ui('slider',t,v,lo,hi); };\n"
        "ui.layoutRow = function(h,w1,w2,w3,w4) { kwcc_ui('layoutRow',h,w1,w2,w3,w4); };\n"
        "ui.setNext = function(x,y,w,h) { kwcc_ui('setNext',x,y,w,h); };\n"
        "ui.rect = function(x,y,w,h,r,g,b) { kwcc_ui('rect',x,y,w,h,r,g,b); };\n"
        "ui.display = function(t) { kwcc_ui('display',t); };\n"
        "ui.textCentered = function(t) { kwcc_ui('textCentered',t); };\n";
    JSValue meth_result = JS_Eval(ctx, methods_js, strlen(methods_js), "<bridge>", 0);
    if (JS_IsException(meth_result)) {
        JSValue exc = JS_GetException(ctx);
        JSCStringBuf buf;
        const char *s = JS_ToCString(ctx, exc, &buf);
        log_error("bridge JS_Eval: %s", s ? s : "(none)");
    }

    return ctx;
}

void bridge_destroy_js(JSContext *ctx) {
    if (ctx) {
        JS_FreeContext(ctx);
    }
}

void bridge_process_js(JSContext *ctx, const char *js_text) {
    if (!js_text) return;

    mu_begin(&g_mu);

    JSValue result = JS_Eval(ctx, js_text, strlen(js_text), "<main.js>", 0);
    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        JSCStringBuf buf;
        const char *s = JS_ToCString(ctx, exc, &buf);
        log_error("JS: %s", s ? s : "(none)");
    }

    mu_end(&g_mu);
}

mu_Context *bridge_get_mu(void) {
    return &g_mu;
}

void bridge_input_mousemove(int x, int y) {
    mu_input_mousemove(&g_mu, x, y);
}

void bridge_input_mousedown(int x, int y, int btn) {
    mu_input_mousedown(&g_mu, x, y, btn);
}

void bridge_input_mouseup(int x, int y, int btn) {
    mu_input_mouseup(&g_mu, x, y, btn);
}

void bridge_input_scroll(int x, int y) {
    mu_input_scroll(&g_mu, x, y);
}

void bridge_input_text(const char *text) {
    mu_input_text(&g_mu, text);
}
