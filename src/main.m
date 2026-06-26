#define SOKOL_GLCORE
#define SOKOL_IMPL
#include "sokol/sokol_app.h"
#include "sokol/sokol_gfx.h"
#include "sokol/sokol_glue.h"
#include "sokol/sokol_log.h"

#define NANOVG_GL3_IMPLEMENTATION
#include "nanovg/nanovg.h"
#include "nanovg/nanovg_gl.h"

#include "kwcc.h"
#include "microui/microui.h"
#include "llog.h"

#define NANOSVG_IMPLEMENTATION
#include "nanosvg/nanosvg.h"

NVGcontext *g_kwcc_vg = NULL;
static JSContext  *g_js_ctx = NULL;
static const char *g_js_text = NULL;
static FILE       *g_log_fp = NULL;

static const char *kwcc_load_file(const char *path) {
    static char buf[65536];
    FILE *f = fopen(path, "rb");
    if (!f) { log_error("cannot open %s", path); return NULL; }
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static void kwcc_render_mu_commands(void) {
    mu_Context *mu = kwcc_get_mu();
    mu_Command *cmd = NULL;
    int count = 0;

    while (mu_next_command(mu, &cmd)) {
        count++;
        switch (cmd->base.type) {
        case MU_COMMAND_CLIP:
            {
                mu_ClipCommand *c = (mu_ClipCommand *)cmd;
                if (c->rect.w == 0 && c->rect.h == 0) {
                    nvgResetScissor(g_kwcc_vg);
                } else {
                    nvgScissor(g_kwcc_vg, c->rect.x, c->rect.y, c->rect.w, c->rect.h);
                }
            }
            break;
        case MU_COMMAND_RECT:
            {
                mu_RectCommand *c = (mu_RectCommand *)cmd;
                nvgBeginPath(g_kwcc_vg);
                nvgRect(g_kwcc_vg, c->rect.x, c->rect.y, c->rect.w, c->rect.h);
                nvgFillColor(g_kwcc_vg, nvgRGBA(c->color.r, c->color.g, c->color.b, c->color.a));
                nvgFill(g_kwcc_vg);
            }
            break;
        case MU_COMMAND_TEXT:
            {
                mu_TextCommand *c = (mu_TextCommand *)cmd;
                nvgBeginPath(g_kwcc_vg);
                nvgFontFace(g_kwcc_vg, kwcc_get_font());
                nvgFontSize(g_kwcc_vg, 14);
                nvgFillColor(g_kwcc_vg, nvgRGBA(c->color.r, c->color.g, c->color.b, c->color.a));
                nvgTextAlign(g_kwcc_vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
                nvgText(g_kwcc_vg, c->pos.x, c->pos.y, c->str, NULL);
            }
            break;
        case MU_COMMAND_ICON:
            {
                mu_IconCommand *c = (mu_IconCommand *)cmd;
                mu_Color col = c->color;
                float s = 1.5f;  /* stroke width */
                float m = c->rect.w * 0.25f;  /* margin */
                /* draw X for close icon */
                nvgBeginPath(g_kwcc_vg);
                nvgStrokeColor(g_kwcc_vg, nvgRGBA(col.r, col.g, col.b, col.a));
                nvgStrokeWidth(g_kwcc_vg, s);
                nvgMoveTo(g_kwcc_vg, c->rect.x + m, c->rect.y + m);
                nvgLineTo(g_kwcc_vg, c->rect.x + c->rect.w - m, c->rect.y + c->rect.h - m);
                nvgMoveTo(g_kwcc_vg, c->rect.x + c->rect.w - m, c->rect.y + m);
                nvgLineTo(g_kwcc_vg, c->rect.x + m, c->rect.y + c->rect.h - m);
                nvgStroke(g_kwcc_vg);
            }
            break;
        case MU_COMMAND_SVG:
            {
                mu_SvgCommand *c = (mu_SvgCommand *)cmd;
                if (c->cache_idx < 0 || c->cache_idx >= KWCC_UI_SVG_CACHE_SIZE) {
                    break;
                }
                NSVGimage *image = g_kwcc_ui_svg_cache[c->cache_idx].image;
                if (!image) break;

                /* mark active this frame */
                g_kwcc_ui_svg_cache[c->cache_idx].frame_id = g_kwcc_ui_frame_counter;

                float svg_w = image->width > 0 ? image->width : 1;
                float svg_h = image->height > 0 ? image->height : 1;
                float sx = c->rect.w / svg_w;
                float sy = c->rect.h / svg_h;
                float scale = (sx < sy) ? sx : sy;
                float sw = svg_w * scale;
                float sh = svg_h * scale;
                float ox = (float)c->rect.x + (c->rect.w - sw) / 2.0f;
                float oy = (float)c->rect.y + (c->rect.h - sh) / 2.0f;

                /* Extract NVG color from nanosvg color (stored as R|(G<<8)|(B<<16)) */
                nvgSave(g_kwcc_vg);
                nvgTranslate(g_kwcc_vg, ox, oy);
                nvgScale(g_kwcc_vg, scale, scale);
                for (NSVGshape *shape = image->shapes; shape; shape = shape->next) {
                    if (!(shape->flags & NSVG_FLAGS_VISIBLE)) continue;
                    for (NSVGpath *p = shape->paths; p; p = p->next) {
                        if (!p || p->npts < 2) continue;
                        float *pts = p->pts;
                        int n = p->npts;
                        nvgBeginPath(g_kwcc_vg);
                        nvgMoveTo(g_kwcc_vg, pts[0], pts[1]);
                        for (int i = 1; i + 2 < n; i += 3) {
                            nvgBezierTo(g_kwcc_vg, pts[i*2], pts[i*2+1], pts[i*2+2], pts[i*2+3], pts[i*2+4], pts[i*2+5]);
                        }
                        if (p->closed) nvgClosePath(g_kwcc_vg);
                        if (shape->fill.type == NSVG_PAINT_COLOR) {
                            unsigned int fc = shape->fill.color;
                            nvgFillColor(g_kwcc_vg, nvgRGBA(fc & 0xFF, (fc >> 8) & 0xFF, (fc >> 16) & 0xFF, 0xFF));
                            nvgFill(g_kwcc_vg);
                        }
                        if (shape->stroke.type == NSVG_PAINT_COLOR && shape->strokeWidth > 0) {
                            unsigned int sc = shape->stroke.color;
                            nvgStrokeColor(g_kwcc_vg, nvgRGBA(sc & 0xFF, (sc >> 8) & 0xFF, (sc >> 16) & 0xFF, 0xFF));
                            nvgStrokeWidth(g_kwcc_vg, shape->strokeWidth);
                            nvgLineCap(g_kwcc_vg, shape->strokeLineCap);
                            nvgLineJoin(g_kwcc_vg, shape->strokeLineJoin);
                            nvgStroke(g_kwcc_vg);
                        }
                    }
                }
                nvgRestore(g_kwcc_vg);
            }
            break;
        }
    }
    static int first = 1;
    if (first) { log_info("render: %d commands", count); first = 0; }
}

static void init(void) {
    g_log_fp = fopen("kwcc.log", "w");
    if (g_log_fp) {
        log_add_fp(g_log_fp, LOG_TRACE);
    }
    log_info("=== kwcc started ===");

    sg_setup(&(sg_desc){
        .environment = sglue_environment(),
        .logger.func = slog_func,
    });

    g_kwcc_vg = nvgCreateGL3(NVG_ANTIALIAS | NVG_STENCIL_STROKES);

    /* Load system fonts for CJK support */
    int font = nvgCreateFontAtIndex(g_kwcc_vg, "sans", "/System/Library/Fonts/PingFang.ttc", 0);
    if (font < 0) {
        log_warn("PingFang.ttc load failed, trying STHeiti");
        font = nvgCreateFontAtIndex(g_kwcc_vg, "sans", "/System/Library/Fonts/STHeiti Medium.ttc", 0);
        if (font < 0) {
            log_warn("STHeiti load failed, fallback to Roboto");
            nvgCreateFont(g_kwcc_vg, "sans", "assets/Roboto-Regular.ttf");
        }
    }

    kwcc_mempool_init();       /* 0. memory pool */
    kwcc_io_init();            /* 1. I/O reactor */
    kwcc_http_init();          /* 2. HTTP process engine */
    g_js_ctx = kwcc_create_js();    /* 3. JSContext + config init */
    kwcc_ui_bus_set_js_ctx(g_js_ctx);  /* set JSContext for UI bus */
    kwcc_ui_init();               /* 2. microui text callbacks */
    kwcc_register_ui(g_js_ctx);     /* 3. UI methods */

    /* Load and eval main.js once during init (register modules, init store/events) */
    g_js_text = kwcc_load_file("app/main.js");
    kwcc_process_js(g_js_ctx, g_js_text);
}

static void frame(void) {
    int w = sapp_width();
    int h = sapp_height();

    /* 1. I/O reactor polling (non-blocking) */
    kwcc_io_poll_once();
    kwcc_http_check_progress();

    /* 2. JS processing + microui rendering */
    kwcc_process_js(g_js_ctx, "onFrame();");

    sg_begin_pass(&(sg_pass){
        .action = {
            .colors[0] = {
                .load_action = SG_LOADACTION_CLEAR,
                .clear_value = { .r = 0.15f, .g = 0.15f, .b = 0.18f, .a = 1.0f },
            },
        },
        .swapchain = sglue_swapchain(),
    });

    nvgBeginFrame(g_kwcc_vg, (float)w, (float)h, 1.0f);
    kwcc_render_mu_commands();
    nvgEndFrame(g_kwcc_vg);

    sg_end_pass();
    sg_commit();
}

static void cleanup(void) {
    log_info("=== kwcc exiting ===");
    if (g_log_fp) { fflush(g_log_fp); fclose(g_log_fp); g_log_fp = NULL; }
    if (g_kwcc_vg) { nvgDeleteGL3(g_kwcc_vg); g_kwcc_vg = NULL; }
    kwcc_destroy_js(g_js_ctx);
    kwcc_ui_free();
    kwcc_mempool_shutdown();
    sg_shutdown();
}

static void input(const sapp_event *ev) {
    float scale = sapp_dpi_scale();
    int mx = (int)(ev->mouse_x * scale);
    int my = (int)(ev->mouse_y * scale);
    int btn = ev->mouse_button == 0 ? MU_MOUSE_LEFT :
              ev->mouse_button == 1 ? MU_MOUSE_RIGHT : MU_MOUSE_MIDDLE;

    switch (ev->type) {
    case SAPP_EVENTTYPE_MOUSE_MOVE:
        kwcc_input_mousemove(mx, my);
        break;
    case SAPP_EVENTTYPE_MOUSE_DOWN:
        kwcc_input_mousedown(mx, my, btn);
        break;
    case SAPP_EVENTTYPE_MOUSE_UP:
        kwcc_input_mouseup(mx, my, btn);
        break;
    case SAPP_EVENTTYPE_MOUSE_SCROLL:
        kwcc_input_scroll((int)ev->scroll_x, (int)ev->scroll_y);
        break;
    case SAPP_EVENTTYPE_CHAR:
        {
            char buf[8];
            int n = 0;
            if (ev->char_code < 128) { buf[0] = (char)ev->char_code; buf[1] = 0; n = 1; }
            if (n) kwcc_input_text(buf);
        }
        break;
    default:
        break;
    }
}

sapp_desc sokol_main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    return (sapp_desc){
        .init_cb     = init,
        .frame_cb    = frame,
        .cleanup_cb  = cleanup,
        .event_cb    = input,
        .width       = 800,
        .height      = 600,
        .window_title = "kwcc",
        .logger.func = slog_func,
    };
}
