#define SOKOL_GLCORE
#define SOKOL_IMPL
#include "sokol/sokol_app.h"
#include "sokol/sokol_gfx.h"
#include "sokol/sokol_glue.h"
#include "sokol/sokol_log.h"

#define NANOVG_GL3_IMPLEMENTATION
#include "nanovg/nanovg.h"
#include "nanovg/nanovg_gl.h"

#include "bridge.h"
#include "microui/microui.h"
#include "llog.h"

NVGcontext *vg = NULL;
static JSContext  *js_ctx = NULL;
static const char *js_text = NULL;
static FILE       *log_fp = NULL;

static const char *load_file(const char *path) {
    static char buf[65536];
    FILE *f = fopen(path, "rb");
    if (!f) { log_error("cannot open %s", path); return NULL; }
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static void render_mu_commands(void) {
    mu_Context *mu = bridge_get_mu();
    mu_Command *cmd = NULL;
    int count = 0;

    while (mu_next_command(mu, &cmd)) {
        count++;
        switch (cmd->base.type) {
        case MU_COMMAND_CLIP:
            {
                mu_ClipCommand *c = (mu_ClipCommand *)cmd;
                if (c->rect.w == 0 && c->rect.h == 0) {
                    nvgResetScissor(vg);
                } else {
                    nvgScissor(vg, c->rect.x, c->rect.y, c->rect.w, c->rect.h);
                }
            }
            break;
        case MU_COMMAND_RECT:
            {
                mu_RectCommand *c = (mu_RectCommand *)cmd;
                nvgBeginPath(vg);
                nvgRect(vg, c->rect.x, c->rect.y, c->rect.w, c->rect.h);
                nvgFillColor(vg, nvgRGBA(c->color.r, c->color.g, c->color.b, c->color.a));
                nvgFill(vg);
            }
            break;
        case MU_COMMAND_TEXT:
            {
                mu_TextCommand *c = (mu_TextCommand *)cmd;
                nvgBeginPath(vg);
                nvgFontFace(vg, "sans");
                nvgFontSize(vg, 14);
                nvgFillColor(vg, nvgRGBA(c->color.r, c->color.g, c->color.b, c->color.a));
                nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
                nvgText(vg, c->pos.x, c->pos.y, c->str, NULL);
            }
            break;
        case MU_COMMAND_ICON:
            {
                mu_IconCommand *c = (mu_IconCommand *)cmd;
                mu_Color col = c->color;
                float s = 1.5f;  /* stroke width */
                float m = c->rect.w * 0.25f;  /* margin */
                /* draw X for close icon */
                nvgBeginPath(vg);
                nvgStrokeColor(vg, nvgRGBA(col.r, col.g, col.b, col.a));
                nvgStrokeWidth(vg, s);
                nvgMoveTo(vg, c->rect.x + m, c->rect.y + m);
                nvgLineTo(vg, c->rect.x + c->rect.w - m, c->rect.y + c->rect.h - m);
                nvgMoveTo(vg, c->rect.x + c->rect.w - m, c->rect.y + m);
                nvgLineTo(vg, c->rect.x + m, c->rect.y + c->rect.h - m);
                nvgStroke(vg);
            }
            break;
        }
    }
    static int first = 1;
    if (first) { log_info("render: %d commands", count); first = 0; }
}

static void init(void) {
    log_fp = fopen("kwcc.log", "w");
    if (log_fp) {
        log_add_fp(log_fp, LOG_TRACE);
    }
    log_info("=== kwcc started ===");

    sg_setup(&(sg_desc){
        .environment = sglue_environment(),
        .logger.func = slog_func,
    });

    vg = nvgCreateGL3(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
    nvgCreateFont(vg, "sans", "assets/Roboto-Regular.ttf");

    bridge_init();
    js_ctx = bridge_create_js();
    js_text = load_file("app/main.js");
}

static void frame(void) {
    int w = sapp_width();
    int h = sapp_height();

    bridge_process_js(js_ctx, js_text);

    sg_begin_pass(&(sg_pass){
        .action = {
            .colors[0] = {
                .load_action = SG_LOADACTION_CLEAR,
                .clear_value = { .r = 0.15f, .g = 0.15f, .b = 0.18f, .a = 1.0f },
            },
        },
        .swapchain = sglue_swapchain(),
    });

    nvgBeginFrame(vg, (float)w, (float)h, 1.0f);
    render_mu_commands();
    nvgEndFrame(vg);

    sg_end_pass();
    sg_commit();
}

static void cleanup(void) {
    log_info("=== kwcc exiting ===");
    if (log_fp) { fflush(log_fp); fclose(log_fp); log_fp = NULL; }
    if (vg) { nvgDeleteGL3(vg); vg = NULL; }
    bridge_destroy_js(js_ctx);
    bridge_free();
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
        bridge_input_mousemove(mx, my);
        break;
    case SAPP_EVENTTYPE_MOUSE_DOWN:
        bridge_input_mousedown(mx, my, btn);
        break;
    case SAPP_EVENTTYPE_MOUSE_UP:
        bridge_input_mouseup(mx, my, btn);
        break;
    case SAPP_EVENTTYPE_MOUSE_SCROLL:
        bridge_input_scroll((int)ev->scroll_x, (int)ev->scroll_y);
        break;
    case SAPP_EVENTTYPE_CHAR:
        {
            char buf[8];
            int n = 0;
            if (ev->char_code < 128) { buf[0] = (char)ev->char_code; buf[1] = 0; n = 1; }
            if (n) bridge_input_text(buf);
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
