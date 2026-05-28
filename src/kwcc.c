/* kwcc.c — kwcc UI engine core (JS ↔ microui ↔ NanoVG bridge) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "mquickjs/mquickjs.h"
#include "microui/microui.h"
#include "nanovg/nanovg.h"
#include "llog.h"
#include "kwcc.h"

#include "jsapi.h"
#include "mquickjs/mqjs_stdlib.h"

#define NANOSVG_IMPLEMENTATION
#include "nanosvg/nanosvg.h"

/* Extern: NVG context set in main.m */
extern NVGcontext *vg;

#define KWCC_MEM_SIZE (4 * 1024 * 1024)

static mu_Context g_mu;
static mu_Real    g_slider_val = 0.5f;  /* persistent slider state (microui uses ptr as ID) */
static const char *g_current_font = NULL; /* current active font name */

/* ── SVG rendering ──────────────────────────────────────────── */

/* ── Extract NVG color from nanosvg color (stored as R|(G<<8)|(B<<16)) ── */
static NVGcolor nsvg_color(unsigned int c) {
    unsigned char r = (c >>  0) & 0xFF;
    unsigned char g = (c >>  8) & 0xFF;
    unsigned char b = (c >> 16) & 0xFF;
    return nvgRGBA(r, g, b, 0xFF);
}

/* ── SVG command queue (filled during JS execution, rendered after nvgBeginFrame) ── */
#define MAX_SVG_QUEUE 8
typedef struct {
    char path[256];
    float x, y, w, h;
    mu_Rect clip;  /* container clip rect for absolute positioning */
} svg_cmd_t;

static svg_cmd_t g_svg_queue[MAX_SVG_QUEUE];
static int g_svg_count = 0;

static void kwcc_queue_svg(const char *path, float x, float y, float w, float h) {
    if (g_svg_count >= MAX_SVG_QUEUE) return;
    svg_cmd_t *cmd = &g_svg_queue[g_svg_count++];
    strncpy(cmd->path, path, sizeof(cmd->path) - 1);
    cmd->path[sizeof(cmd->path) - 1] = '\0';
    cmd->x = x; cmd->y = y; cmd->w = w; cmd->h = h;
    /* Capture current container clip rect for absolute positioning */
    cmd->clip = mu_get_clip_rect(&g_mu);
}

/* ── Draw one parsed SVG shape ── */
static void draw_svg_image(NSVGimage *image, float ox, float oy, float scale) {
    nvgSave(vg);
    nvgTranslate(vg, ox, oy);
    nvgScale(vg, scale, scale);

    for (NSVGshape *shape = image->shapes; shape; shape = shape->next) {
        if (!(shape->flags & NSVG_FLAGS_VISIBLE)) continue;

        for (NSVGpath *p = shape->paths; p; p = p->next) {
            if (!p || p->npts < 2) continue;

            float *pts = p->pts;
            int n = p->npts;

            nvgBeginPath(vg);
            nvgMoveTo(vg, pts[0], pts[1]);
            for (int i = 1; i + 2 < n; i += 3) {
                nvgBezierTo(vg,
                    pts[i*2],     pts[i*2+1],
                    pts[i*2+2],   pts[i*2+3],
                    pts[i*2+4],   pts[i*2+5]);
            }
            if (p->closed) nvgClosePath(vg);

            /* Fill */
            if (shape->fill.type == NSVG_PAINT_COLOR) {
                nvgFillColor(vg, nsvg_color(shape->fill.color));
                nvgFill(vg);
            }

            /* Stroke */
            if (shape->stroke.type == NSVG_PAINT_COLOR && shape->strokeWidth > 0) {
                nvgStrokeColor(vg, nsvg_color(shape->stroke.color));
                nvgStrokeWidth(vg, shape->strokeWidth);
                nvgLineCap(vg, shape->strokeLineCap);
                nvgLineJoin(vg, shape->strokeLineJoin);
                nvgStroke(vg);
            }
        }
    }

    nvgRestore(vg);
}

void kwcc_render_svg(void) {
    if (!vg) return;

    for (int i = 0; i < g_svg_count; i++) {
        svg_cmd_t *cmd = &g_svg_queue[i];

        NSVGimage *image = nsvgParseFromFile(cmd->path, "px", 96.0f);
        if (!image) {
            log_error("svg: cannot parse '%s'", cmd->path);
            continue;
        }

        float svg_w = image->width > 0 ? image->width : 1;
        float svg_h = image->height > 0 ? image->height : 1;

        float sx = cmd->w / svg_w;
        float sy = cmd->h / svg_h;
        float scale = (sx < sy) ? sx : sy;

        float sw = svg_w * scale;
        float sh = svg_h * scale;
        /* cmd.x/cmd.y are relative to layout body; offset by clip rect for screen space */
        float ox = cmd->clip.x + cmd->x + (cmd->w - sw) / 2.0f;
        float oy = cmd->clip.y + cmd->y + (cmd->h - sh) / 2.0f;

        /* Set clip to container body area */
        nvgSave(vg);
        nvgScissor(vg, cmd->clip.x, cmd->clip.y, cmd->clip.w, cmd->clip.h);

        draw_svg_image(image, ox, oy, scale);

        nvgRestore(vg);
        nsvgDelete(image);
    }

    /* Clear queue for next frame */
    g_svg_count = 0;
}

/* ── Forward declaration ──────────────────────────────────── */
void kwcc_load_font_dir(const char *dir_path);

/* ── microui text measurement callbacks (real font metrics) ── */

static int mu_text_width(mu_Font font, const char *str, int len) {
    (void)font;
    if (!str || !vg) return len > 0 ? len * 7 : 0;
    if (len < 0) len = (int)strlen(str);
    float bounds[4];
    const char *fname = g_current_font ? g_current_font : "sans";
    nvgFontFace(vg, fname);
    nvgFontSize(vg, 14);
    nvgTextBounds(vg, 0, 0, str, str + len, bounds);
    return (int)(bounds[2] - bounds[0]);
}

static int mu_text_height(mu_Font font) {
    (void)font;
    if (!vg) return 14;
    float bounds[4];
    const char *fname = g_current_font ? g_current_font : "sans";
    nvgFontFace(vg, fname);
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
    if (strcmp(method, "loadFont") == 0) {
        JSCStringBuf name_buf, path_buf;
        const char *name = JS_ToCString(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &name_buf);
        const char *path = JS_ToCString(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &path_buf);
        if (name && path) {
            if (nvgCreateFont(vg, name, path) >= 0) {
                g_current_font = strdup(name);
                log_info("font loaded: %s (%s)", name, path);
            } else {
                log_error("nvgCreateFont failed: %s (%s)", name, path);
            }
        }
        return JS_UNDEFINED;
    }
    if (strcmp(method, "setFont") == 0) {
        JSCStringBuf name_buf;
        const char *name = JS_ToCString(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &name_buf);
        if (name) {
            if (g_current_font && g_current_font != name) {
                free((void *)g_current_font);
            }
            g_current_font = strdup(name);
            log_info("font set to: %s", name);
        }
        return JS_UNDEFINED;
    }
    if (strcmp(method, "loadFontDir") == 0) {
        JSCStringBuf path_buf;
        const char *dir = JS_ToCString(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &path_buf);
        if (dir) {
            kwcc_load_font_dir(dir);
        }
        return JS_UNDEFINED;
    }
    if (strcmp(method, "svg") == 0) {
        JSCStringBuf path_buf;
        JSValue arg = argc > 0 ? argv[0] : JS_UNDEFINED;
        if (JS_IsUndefined(arg)) return JS_UNDEFINED;
        const char *js_path = JS_ToCString(ctx, arg, &path_buf);
        if (!js_path) return JS_UNDEFINED;
        /* Copy to local buffer since JS_ToCString pointer may be unstable */
        char path[256];
        strncpy(path, js_path, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
        int ix = 0, iy = 0, iw = 100, ih = 100;
        if (argc > 1) JS_ToInt32(ctx, &ix, argv[1]);
        if (argc > 2) JS_ToInt32(ctx, &iy, argv[2]);
        if (argc > 3) JS_ToInt32(ctx, &iw, argv[3]);
        if (argc > 4) JS_ToInt32(ctx, &ih, argv[4]);
        kwcc_queue_svg(path, (float)ix, (float)iy, (float)iw, (float)ih);
        return JS_UNDEFINED;
    }
    return JS_UNDEFINED;
}

/* ── Load all .ttf/.otf fonts from a directory ──────────────── */

/* Keywords for Simplified Chinese CJK fonts */
static int is_cjk_hint(const char *name) {
    const char *hints[] = {
        "noto sans sc", "noto serif sc", "pingfang sc",
        "heiti sc", "songti sc", "hiragino sans gb",
        "simhei", "simsun", "microsoft yahei", "fangsong",
        "kaiti sc", "思源黑体", "思源宋体", "苹方",
        "微软雅黑", "宋体", "黑体", "仿宋", "楷体",
        "wqy", "wenquanyi",
    };
    char lower[256];
    int len = (int)strlen(name);
    if (len >= 256) len = 255;
    for (int i = 0; i < len; i++) lower[i] = (name[i] >= 'A' && name[i] <= 'Z') ? name[i] + 32 : name[i];
    lower[len] = '\0';
    for (size_t i = 0; i < sizeof(hints) / sizeof(hints[0]); i++) {
        if (strstr(lower, hints[i])) return 1;
    }
    return 0;
}

void kwcc_load_font_dir(const char *dir_path) {
    DIR *dir = opendir(dir_path);
    if (!dir) { log_error("font dir: cannot open %s", dir_path); return; }

    struct dirent *ent;
    int loaded = 0, cjk_set = 0;

    while ((ent = readdir(dir)) != NULL) {
        char *ext = strrchr(ent->d_name, '.');
        if (!ext) continue;
        if (strcmp(ext, ".ttf") != 0 && strcmp(ext, ".otf") != 0 &&
            strcmp(ext, ".TTF") != 0 && strcmp(ext, ".OTF") != 0) continue;

        /* Build font name: strip extension */
        char font_name[256];
        strncpy(font_name, ent->d_name, sizeof(font_name) - 1);
        font_name[sizeof(font_name) - 1] = '\0';
        char *dot = strrchr(font_name, '.');
        if (dot) *dot = '\0';

        /* Build full path */
        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, ent->d_name);

        if (nvgCreateFont(vg, font_name, full_path) >= 0) {
            log_info("font loaded: %s (%s)", font_name, full_path);
            loaded++;
            /* Auto-pick CJK font as default */
            if (!cjk_set && is_cjk_hint(font_name)) {
                if (g_current_font) free((void *)g_current_font);
                g_current_font = strdup(font_name);
                cjk_set = 1;
                log_info("auto-selected CJK font: %s", font_name);
            }
        }
    }
    closedir(dir);
    if (loaded == 0) {
        log_warn("font dir %s: no fonts loaded", dir_path);
    } else {
        log_info("loaded %d fonts from %s%s", loaded, dir_path,
            cjk_set ? " (CJK auto-selected)" : "");
    }
}

/* ── Public API ───────────────────────────────────────────────── */

void kwcc_init(void) {
    mu_init(&g_mu);
    g_mu.text_width  = mu_text_width;
    g_mu.text_height = mu_text_height;
}

void kwcc_free(void) {
}

JSContext *kwcc_create_js(void) {
    void *mem_buf = malloc(KWCC_MEM_SIZE);
    JSContext *ctx = JS_NewContext(mem_buf, KWCC_MEM_SIZE, &js_stdlib);
    if (!ctx) {
        log_fatal("kwcc: JS_NewContext failed (not enough memory?)");
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
        "ui.textCentered = function(t) { kwcc_ui('textCentered',t); };\n"
        "ui.loadFont = function(n,p) { kwcc_ui('loadFont',n,p); };\n"
        "ui.setFont = function(n) { kwcc_ui('setFont',n); };\n"
        "ui.loadFontDir = function(d) { kwcc_ui('loadFontDir',d); };\n"
        "ui.svg = function(p,x,y,w,h) { kwcc_ui('svg',p,x||0,y||0,w||100,h||100); };\n";
    JSValue meth_result = JS_Eval(ctx, methods_js, strlen(methods_js), "<kwcc>", 0);
    if (JS_IsException(meth_result)) {
        JSValue exc = JS_GetException(ctx);
        JSCStringBuf buf;
        const char *s = JS_ToCString(ctx, exc, &buf);
        log_error("kwcc JS_Eval: %s", s ? s : "(none)");
    }

    return ctx;
}

void kwcc_destroy_js(JSContext *ctx) {
    if (ctx) {
        JS_FreeContext(ctx);
    }
}

void kwcc_process_js(JSContext *ctx, const char *js_text) {
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

mu_Context *kwcc_get_mu(void) {
    return &g_mu;
}

const char *kwcc_get_font(void) {
    return g_current_font ? g_current_font : "sans";
}

void kwcc_input_mousemove(int x, int y) {
    mu_input_mousemove(&g_mu, x, y);
}

void kwcc_input_mousedown(int x, int y, int btn) {
    mu_input_mousedown(&g_mu, x, y, btn);
}

void kwcc_input_mouseup(int x, int y, int btn) {
    mu_input_mouseup(&g_mu, x, y, btn);
}

void kwcc_input_scroll(int x, int y) {
    mu_input_scroll(&g_mu, x, y);
}

void kwcc_input_text(const char *text) {
    mu_input_text(&g_mu, text);
}
