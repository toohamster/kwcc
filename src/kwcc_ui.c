/* kwcc_ui.c — UI module (microui + nanovg bridge) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "nanovg/nanovg.h"
#include "microui/microui.h"
#include "llog.h"
#include "kwcc_ui.h"
#include "kwcc_base.h"
#include "kwcc_js.h"
#include "kwcc_ui_bus.h"
#include "kwcc_edit.h"

#include "nanosvg/nanosvg.h"

/* Extern: NVG context set in main.m */
extern NVGcontext *g_kwcc_vg;

/* ── Owned microui context ─────────────────────────────────── */

mu_Context g_kwcc_mu;

/* ── Persistent UI state ─────────────────────────────────────── */

static mu_Real    g_kwcc_ui_slider_val = 0.5f;
static const char *g_kwcc_ui_current_font = NULL;


/* Window barrier */
#define KWCC_UI_MAX_WIN_DEPTH 32
typedef struct {
    char key[64];
    int  visible;
} mod_state_t;

#define KWCC_UI_MAX_MODULES 32
static mod_state_t g_kwcc_ui_sync_table[KWCC_UI_MAX_MODULES];
static int         g_kwcc_ui_sync_count = 0;
static const char *g_kwcc_ui_current_mod_key = NULL;
static int         g_kwcc_ui_win_intercepted[KWCC_UI_MAX_WIN_DEPTH];
static int         g_kwcc_ui_win_top = 0;

/* Window close topic map: title → topic（每帧重建，kwcc_begin_frame 重置）。
 * beginWindow 每帧 upsert 绑定；on_window_close 按 title 反查 topic 再 dispatch。 */
#define KWCC_UI_MAX_WINDOWS 16   /* microui 容器池 48，真实应用 <10 窗口；溢出 log_warn */
typedef struct {
    char title[64];
    char topic[128];
} kwcc_ui_win_topic_t;
static kwcc_ui_win_topic_t g_kwcc_ui_win_topic_map[KWCC_UI_MAX_WINDOWS];
static int                 g_kwcc_ui_win_topic_count = 0;

/* JS context — for close callback */
static JSContext *g_kwcc_ui_js_ctx = NULL;

void kwcc_set_js_context(JSContext *ctx) {
    g_kwcc_ui_js_ctx = ctx;
}

/* ── Config helpers ─────────────────────────────────────────── */

static void kwcc_sync_module(const char *key, int visible) {
    g_kwcc_ui_current_mod_key = key;
    for (int i = 0; i < g_kwcc_ui_sync_count; i++) {
        if (strcmp(g_kwcc_ui_sync_table[i].key, key) == 0) {
            g_kwcc_ui_sync_table[i].visible = visible;
            return;
        }
    }
    if (g_kwcc_ui_sync_count < KWCC_UI_MAX_MODULES) {
        strncpy(g_kwcc_ui_sync_table[g_kwcc_ui_sync_count].key, key, 63);
        g_kwcc_ui_sync_table[g_kwcc_ui_sync_count].key[63] = '\0';
        g_kwcc_ui_sync_table[g_kwcc_ui_sync_count].visible = visible;
        g_kwcc_ui_sync_count++;
    }
}

static int kwcc_get_current_visibility(void) {
    if (!g_kwcc_ui_current_mod_key) return 1;
    for (int i = 0; i < g_kwcc_ui_sync_count; i++) {
        if (strcmp(g_kwcc_ui_sync_table[i].key, g_kwcc_ui_current_mod_key) == 0) {
            return g_kwcc_ui_sync_table[i].visible;
        }
    }
    return 1;
}

/* 挡板补全（方案 A）：最内层窗口被拦截时，其 body 的所有 widget 调用必须吞掉。
 * 等价于原生 microui 范式中调用方检查 begin_window_ex 返回值后跳过 body——
 * kwcc 契约是 "view 不需要判断"，此检查由 C 层挡板代做。
 * 不吞的后果：widget 落到空 container/clip/layout 栈 → microui expect → abort。 */
static int kwcc_ui_in_intercepted_window(void) {
    return g_kwcc_ui_win_top > 0 && g_kwcc_ui_win_intercepted[g_kwcc_ui_win_top - 1];
}

/* multiline 展示态滚动（per-label）：内容超出字段高时 wheel 查看。
 * 无原生滚动条视觉（thumb 绘制属后续增强）；offset 语义 = 已下移查看量 */
#define KWCC_UI_EDIT_SCROLL_MAX 16   /* 同屏多行字段个位数；满时 warn 不静默 */
typedef struct {
    char label[64];
    int  offset;
} kwcc_ui_edit_scroll_t;
static kwcc_ui_edit_scroll_t g_kwcc_ui_edit_scroll[KWCC_UI_EDIT_SCROLL_MAX];
static int g_kwcc_ui_edit_scroll_count = 0;

static int kwcc_ui_edit_scroll_get(const char *label) {
    for (int i = 0; i < g_kwcc_ui_edit_scroll_count; i++) {
        if (strcmp(g_kwcc_ui_edit_scroll[i].label, label) == 0) {
            return g_kwcc_ui_edit_scroll[i].offset;
        }
    }
    return 0;
}

static void kwcc_ui_edit_scroll_set(const char *label, int offset) {
    for (int i = 0; i < g_kwcc_ui_edit_scroll_count; i++) {
        if (strcmp(g_kwcc_ui_edit_scroll[i].label, label) == 0) {
            g_kwcc_ui_edit_scroll[i].offset = offset;
            return;
        }
    }
    if (g_kwcc_ui_edit_scroll_count < KWCC_UI_EDIT_SCROLL_MAX) {
        kwcc_ui_edit_scroll_t *e = &g_kwcc_ui_edit_scroll[g_kwcc_ui_edit_scroll_count++];
        snprintf(e->label, sizeof(e->label), "%s", label);
        e->offset = offset;
    } else {
        log_warn("ui: edit scroll map full (%d), scroll for '%s' ignored",
                 KWCC_UI_EDIT_SCROLL_MAX, label);
    }
}

/* ── Per-frame begin (UI state only; bus reset handled by kwcc_bus) ── */

void kwcc_begin_frame(void) {
    kwcc_ui_bus_begin_frame();
    g_kwcc_ui_sync_count = 0;
    g_kwcc_ui_win_top = 0;
    g_kwcc_ui_win_topic_count = 0;
    kwcc_edit_begin_frame();   /* 编辑中字段缺报 → 自动 commit（窗口关闭/模块隐藏） */
}

static void kwcc_on_window_close(mu_Context *ctx, const char *title) {
    (void)ctx;
    for (int i = 0; i < g_kwcc_ui_win_topic_count; i++) {
        if (strcmp(g_kwcc_ui_win_topic_map[i].title, title) == 0) {
            log_info("on_window_close: '%s' -> topic '%s'", title,
                     g_kwcc_ui_win_topic_map[i].topic);
            kwcc_ui_bus_dispatch_event(g_kwcc_ui_win_topic_map[i].topic, "close");
            return;
        }
    }
    log_warn("on_window_close: no topic bound for window '%s', dispatching title", title);
    kwcc_ui_bus_dispatch_event(title, "close");
}

/* ── SVG cache ─────────────────────────────────────────────── */

kwcc_ui_svg_cache_t g_kwcc_ui_svg_cache[KWCC_UI_SVG_CACHE_SIZE];
int         g_kwcc_ui_svg_cache_next = 0;
int         g_kwcc_ui_frame_counter  = 0;

static uint32_t kwcc_ui_fnv1a(const char *s) {
    uint32_t h = 2166136261;
    while (*s) { h ^= *s++; h *= 16777619; }
    return h;
}

static int kwcc_ui_svg_resolve(const char *data, int is_inline) {
    uint32_t hash = kwcc_ui_fnv1a(data);
    size_t   len  = strlen(data);

    for (int i = 0; i < KWCC_UI_SVG_CACHE_SIZE; i++) {
        if (g_kwcc_ui_svg_cache[i].in_use &&
            g_kwcc_ui_svg_cache[i].hash == hash &&
            g_kwcc_ui_svg_cache[i].content_len == len) {
            g_kwcc_ui_svg_cache[i].frame_id = g_kwcc_ui_frame_counter;
            return i;
        }
    }

    NSVGimage *img = NULL;
    if (is_inline) {
        char *buf = strdup(data);
        img = nsvgParse(buf, "px", 96.0f);
        free(buf);
    } else {
        img = nsvgParseFromFile(data, "px", 96.0f);
    }
    if (!img) { log_warn("svg: parse failed (is_inline=%d)", is_inline); return -1; }

    int slot = g_kwcc_ui_svg_cache_next;

    if (g_kwcc_ui_svg_cache[slot].in_use && g_kwcc_ui_svg_cache[slot].frame_id >= g_kwcc_ui_frame_counter) {
        int found = -1;
        for (int i = 0; i < KWCC_UI_SVG_CACHE_SIZE; i++) {
            if (g_kwcc_ui_svg_cache[i].in_use && g_kwcc_ui_svg_cache[i].frame_id < g_kwcc_ui_frame_counter) {
                found = i;
                break;
            }
        }
        if (found >= 0) {
            slot = found;
        } else {
            nsvgDelete(img);
            return -1;
        }
    }

    if (g_kwcc_ui_svg_cache[slot].in_use && g_kwcc_ui_svg_cache[slot].image) {
        nsvgDelete(g_kwcc_ui_svg_cache[slot].image);
    }

    g_kwcc_ui_svg_cache[slot].hash = hash;
    g_kwcc_ui_svg_cache[slot].content_len = len;
    g_kwcc_ui_svg_cache[slot].image = img;
    g_kwcc_ui_svg_cache[slot].frame_id = g_kwcc_ui_frame_counter;
    g_kwcc_ui_svg_cache[slot].in_use = 1;

    g_kwcc_ui_svg_cache_next = (slot + 1) % KWCC_UI_SVG_CACHE_SIZE;
    return slot;
}

static void kwcc_queue_svg(const char *data, int is_inline, float x, float y, float w, float h) {
    mu_Rect clip = mu_get_clip_rect(&g_kwcc_mu);
    int cache_idx = kwcc_ui_svg_resolve(data, is_inline);
    mu_SvgCommand *cmd = (mu_SvgCommand *)mu_push_command(&g_kwcc_mu, MU_COMMAND_SVG, sizeof(mu_SvgCommand));
    cmd->rect.x = (int)(clip.x + x);
    cmd->rect.y = (int)(clip.y + y);
    cmd->rect.w = (int)w;
    cmd->rect.h = (int)h;
    cmd->cache_idx = cache_idx;
}

/* ── Font system ───────────────────────────────────────────── */

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

        char font_name[256];
        strncpy(font_name, ent->d_name, sizeof(font_name) - 1);
        font_name[sizeof(font_name) - 1] = '\0';
        char *dot = strrchr(font_name, '.');
        if (dot) *dot = '\0';

        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, ent->d_name);

        if (nvgCreateFont(g_kwcc_vg, font_name, full_path) >= 0) {
            log_info("font loaded: %s (%s)", font_name, full_path);
            loaded++;
            if (!cjk_set && is_cjk_hint(font_name)) {
                if (g_kwcc_ui_current_font) free((void *)g_kwcc_ui_current_font);
                g_kwcc_ui_current_font = strdup(font_name);
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

const char *kwcc_get_font(void) {
    return g_kwcc_ui_current_font ? g_kwcc_ui_current_font : "sans";
}

/* ── microui text measurement callbacks ────────────────────── */

static int mu_text_width(mu_Font font, const char *str, int len) {
    (void)font;
    if (!str || !g_kwcc_vg) return len > 0 ? len * 7 : 0;
    if (len < 0) len = (int)strlen(str);
    float bounds[4];
    const char *fname = g_kwcc_ui_current_font ? g_kwcc_ui_current_font : "sans";
    nvgFontFace(g_kwcc_vg, fname);
    nvgFontSize(g_kwcc_vg, 14);
    nvgTextBounds(g_kwcc_vg, 0, 0, str, str + len, bounds);
    return (int)(bounds[2] - bounds[0]);
}

static int mu_text_height(mu_Font font) {
    (void)font;
    if (!g_kwcc_vg) return 14;
    float bounds[4];
    const char *fname = g_kwcc_ui_current_font ? g_kwcc_ui_current_font : "sans";
    nvgFontFace(g_kwcc_vg, fname);
    nvgFontSize(g_kwcc_vg, 14);
    nvgTextBounds(g_kwcc_vg, 0, 0, "Hy", NULL, bounds);
    return (int)(bounds[3] - bounds[1]);
}

/* ── UI method dispatch ────────────────────────────────────── */

static JSValue js_ui_dispatch(JSContext *ctx, const char *method,
                               int argc, JSValue *argv) {
    (void)argv;
    if (strcmp(method, "beginWindow") == 0) {
        JSCStringBuf buf, tbuf;
        const char *title = JS_ToCString(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &buf);
        int x = 50, y = 50, w = 400, h = 300;
        int opt = 0;
        if (argc > 1) JS_ToInt32(ctx, &x, argv[1]);
        if (argc > 2) JS_ToInt32(ctx, &y, argv[2]);
        if (argc > 3) JS_ToInt32(ctx, &w, argv[3]);
        if (argc > 4) JS_ToInt32(ctx, &h, argv[4]);
        if (argc > 5) JS_ToInt32(ctx, &opt, argv[5]);

        const char *topic = NULL;
        if (argc > 6) {
            topic = JS_ToCString(ctx, argv[6], &tbuf);
        }

        /* 绑定 title → topic（必须在 mu_begin_window_ex 之前：X 关闭回调在其内部触发）。
         * 按 title upsert，同一帧重复 begin 同一窗口时更新 topic。 */
        if (topic && topic[0] && title) {
            int found = 0;
            for (int i = 0; i < g_kwcc_ui_win_topic_count; i++) {
                if (strcmp(g_kwcc_ui_win_topic_map[i].title, title) == 0) {
                    strncpy(g_kwcc_ui_win_topic_map[i].topic, topic, 127);
                    g_kwcc_ui_win_topic_map[i].topic[127] = '\0';
                    found = 1;
                    break;
                }
            }
            if (!found && g_kwcc_ui_win_topic_count < KWCC_UI_MAX_WINDOWS) {
                kwcc_ui_win_topic_t *e = &g_kwcc_ui_win_topic_map[g_kwcc_ui_win_topic_count++];
                strncpy(e->title, title, 63);
                e->title[63] = '\0';
                strncpy(e->topic, topic, 127);
                e->topic[127] = '\0';
            } else if (!found) {
                log_warn("beginWindow: topic map full (%d), close event for '%s' unbound",
                         KWCC_UI_MAX_WINDOWS, title);
            }
        }

        /* 嵌套窗口：外层被拦截时内层同步拦截（内层 body 与外层一起被挡板吞掉） */
        int visible = kwcc_get_current_visibility() && !kwcc_ui_in_intercepted_window();
        if (g_kwcc_ui_win_top < KWCC_UI_MAX_WIN_DEPTH) {
            if (!visible) {
                g_kwcc_ui_win_intercepted[g_kwcc_ui_win_top] = 1;
                g_kwcc_ui_win_top++;
                return JS_NewBool(0);
            }
            g_kwcc_ui_win_intercepted[g_kwcc_ui_win_top] = 0;
            g_kwcc_ui_win_top++;
        }

        mu_begin_window_ex(&g_kwcc_mu, title ? title : "", (mu_Rect){x, y, w, h}, opt);
        return JS_UNDEFINED;
    }
    if (strcmp(method, "endWindow") == 0) {
        if (g_kwcc_ui_win_top > 0) {
            g_kwcc_ui_win_top--;
            if (!g_kwcc_ui_win_intercepted[g_kwcc_ui_win_top]) {
                mu_end_window(&g_kwcc_mu);
            }
        }
        return JS_UNDEFINED;
    }
    if (strcmp(method, "sync") == 0) {
        JSCStringBuf buf;
        const char *key = JS_ToCString(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &buf);
        int visible = 1;
        if (argc > 1) JS_ToInt32(ctx, &visible, argv[1]);
        if (key) kwcc_sync_module(key, visible);
        return JS_UNDEFINED;
    }
    if (strcmp(method, "beginPanel") == 0) {
        if (kwcc_ui_in_intercepted_window()) { return JS_UNDEFINED; }
        JSCStringBuf buf;
        const char *name = JS_ToCString(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &buf);
        int opt = 0;
        if (argc > 1) JS_ToInt32(ctx, &opt, argv[1]);
        mu_begin_panel_ex(&g_kwcc_mu, name ? name : "", opt);
        return JS_UNDEFINED;
    }
    if (strcmp(method, "endPanel") == 0) {
        if (kwcc_ui_in_intercepted_window()) { return JS_UNDEFINED; }
        mu_end_panel(&g_kwcc_mu);
        return JS_UNDEFINED;
    }
    if (strcmp(method, "button") == 0) {
        if (kwcc_ui_in_intercepted_window()) { return JS_NewBool(0); }
        JSCStringBuf buf, tbuf;
        const char *text = JS_ToCString(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &buf);
        const char *topic = NULL;
        if (argc > 1) {
            topic = JS_ToCString(ctx, argv[1], &tbuf);
        }
        int res = mu_button_ex(&g_kwcc_mu, text ? text : "", 0, MU_OPT_ALIGNCENTER);
        if ((res & MU_RES_SUBMIT) && topic) {
            kwcc_ui_bus_dispatch_event(topic, "click");
        }
        return JS_NewBool(res & MU_RES_SUBMIT);
    }
    if (strcmp(method, "label") == 0) {
        if (kwcc_ui_in_intercepted_window()) { return JS_UNDEFINED; }
        JSCStringBuf buf;
        const char *text = JS_ToCString(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &buf);
        mu_label(&g_kwcc_mu, text ? text : "");
        return JS_UNDEFINED;
    }
    if (strcmp(method, "slider") == 0) {
        if (kwcc_ui_in_intercepted_window()) {
            /* 拦截时不画滑块，值无变化，返回传入值保持返回值契约 */
            double val = g_kwcc_ui_slider_val;
            if (argc > 1) JS_ToNumber(ctx, &val, argv[1]);
            return JS_NewFloat64(ctx, val);
        }
        JSCStringBuf tbuf;
        const char *text = JS_ToCString(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &tbuf);
        double val = g_kwcc_ui_slider_val, low = 0, high = 1;
        const char *topic = NULL;
        JSCStringBuf topic_buf;
        if (argc > 1) JS_ToNumber(ctx, &val, argv[1]);
        if (argc > 2) JS_ToNumber(ctx, &low, argv[2]);
        if (argc > 3) JS_ToNumber(ctx, &high, argv[3]);
        if (argc > 4) {
            topic = JS_ToCString(ctx, argv[4], &topic_buf);
        }
        g_kwcc_ui_slider_val = (mu_Real)val;
        mu_Real flow = (mu_Real)low, fhigh = (mu_Real)high;
        mu_Real prev = g_kwcc_ui_slider_val;
        mu_slider_ex(&g_kwcc_mu, &g_kwcc_ui_slider_val, flow, fhigh, 0.01f, "%g", MU_OPT_ALIGNCENTER);
        if (g_kwcc_ui_slider_val != prev && topic) {
            kwcc_ui_bus_dispatch_event(topic, "change");
        }
        return JS_NewFloat64(ctx, g_kwcc_ui_slider_val);
    }
    if (strcmp(method, "layoutRow") == 0) {
        if (kwcc_ui_in_intercepted_window()) { return JS_UNDEFINED; }
        int height = 20;
        int widths[MU_MAX_WIDTHS];
        int items = 0;
        if (argc > 0) JS_ToInt32(ctx, &height, argv[0]);
        for (int i = 1; i < argc && i <= MU_MAX_WIDTHS; i++) {
            JS_ToInt32(ctx, &widths[i - 1], argv[i]);
            items = i;
        }
        mu_layout_row(&g_kwcc_mu, items, items > 0 ? widths : NULL, height);
        return JS_UNDEFINED;
    }
    if (strcmp(method, "setNext") == 0) {
        if (kwcc_ui_in_intercepted_window()) { return JS_UNDEFINED; }
        int x = 0, y = 0, w = 0, h = 0;
        if (argc > 0) JS_ToInt32(ctx, &x, argv[0]);
        if (argc > 1) JS_ToInt32(ctx, &y, argv[1]);
        if (argc > 2) JS_ToInt32(ctx, &w, argv[2]);
        if (argc > 3) JS_ToInt32(ctx, &h, argv[3]);
        mu_layout_set_next(&g_kwcc_mu, (mu_Rect){x, y, w, h}, 0);
        return JS_UNDEFINED;
    }
    if (strcmp(method, "rect") == 0) {
        if (kwcc_ui_in_intercepted_window()) { return JS_UNDEFINED; }
        int x = 0, y = 0, w = 0, h = 0;
        int r = 50, g = 50, b = 50;
        if (argc > 0) JS_ToInt32(ctx, &x, argv[0]);
        if (argc > 1) JS_ToInt32(ctx, &y, argv[1]);
        if (argc > 2) JS_ToInt32(ctx, &w, argv[2]);
        if (argc > 3) JS_ToInt32(ctx, &h, argv[3]);
        if (argc > 4) JS_ToInt32(ctx, &r, argv[4]);
        if (argc > 5) JS_ToInt32(ctx, &g, argv[5]);
        if (argc > 6) JS_ToInt32(ctx, &b, argv[6]);
        mu_draw_rect(&g_kwcc_mu, (mu_Rect){x, y, w, h}, (mu_Color){r, g, b, 255});
        return JS_UNDEFINED;
    }
    if (strcmp(method, "display") == 0) {
        if (kwcc_ui_in_intercepted_window()) { return JS_UNDEFINED; }
        JSCStringBuf buf;
        const char *text = JS_ToCString(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &buf);
        mu_Rect r = mu_layout_next(&g_kwcc_mu);
        mu_draw_rect(&g_kwcc_mu, r, (mu_Color){30, 30, 30, 255});
        if (text) {
            int tw = g_kwcc_mu.text_width(NULL, text, -1);
            int th = g_kwcc_mu.text_height(NULL);
            int tx = r.x + r.w - tw - 8;
            int ty = r.y + (r.h - th) / 2;
            mu_draw_text(&g_kwcc_mu, NULL, text, -1, (mu_Vec2){tx, ty},
                (mu_Color){240, 240, 240, 255});
        }
        return JS_UNDEFINED;
    }
    if (strcmp(method, "textCentered") == 0) {
        if (kwcc_ui_in_intercepted_window()) { return JS_UNDEFINED; }
        JSCStringBuf buf;
        const char *text = JS_ToCString(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &buf);
        mu_Rect r = mu_layout_next(&g_kwcc_mu);
        mu_draw_control_text(&g_kwcc_mu, text ? text : "", r, MU_COLOR_TEXT, MU_OPT_ALIGNCENTER);
        return JS_UNDEFINED;
    }
    if (strcmp(method, "loadFont") == 0) {
        JSCStringBuf name_buf, path_buf;
        const char *name = JS_ToCString(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &name_buf);
        const char *path = JS_ToCString(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &path_buf);
        if (name && path) {
            if (nvgCreateFont(g_kwcc_vg, name, path) >= 0) {
                g_kwcc_ui_current_font = strdup(name);
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
            if (g_kwcc_ui_current_font && g_kwcc_ui_current_font != name) {
                free((void *)g_kwcc_ui_current_font);
            }
            g_kwcc_ui_current_font = strdup(name);
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
        if (kwcc_ui_in_intercepted_window()) { return JS_UNDEFINED; }
        JSCStringBuf buf;
        JSValue arg = argc > 0 ? argv[0] : JS_UNDEFINED;
        if (JS_IsUndefined(arg)) return JS_UNDEFINED;
        const char *js_data = JS_ToCString(ctx, arg, &buf);
        if (!js_data) return JS_UNDEFINED;

        char data[4096];
        strncpy(data, js_data, sizeof(data) - 1);
        data[sizeof(data) - 1] = '\0';

        int ix = 0, iy = 0, iw = 100, ih = 100;
        if (argc > 1) JS_ToInt32(ctx, &ix, argv[1]);
        if (argc > 2) JS_ToInt32(ctx, &iy, argv[2]);
        if (argc > 3) JS_ToInt32(ctx, &iw, argv[3]);
        if (argc > 4) JS_ToInt32(ctx, &ih, argv[4]);

        int is_inline = (data[0] == '<');
        kwcc_queue_svg(data, is_inline, (float)ix, (float)iy, (float)iw, (float)ih);
        return JS_UNDEFINED;
    }
    if (strcmp(method, "textbox") == 0) {
        if (kwcc_ui_in_intercepted_window()) { return JS_UNDEFINED; }
        JSCStringBuf lbuf, vbuf, tbuf;
        const char *label = JS_ToCString(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &lbuf);
        const char *value = JS_ToCString(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &vbuf);
        const char *topic = JS_ToCString(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, &tbuf);
        int multiline = 0;
        if (argc > 3) JS_ToInt32(ctx, &multiline, argv[3]);
        if (!label) return JS_UNDEFINED;

        mu_Rect r = mu_layout_next(&g_kwcc_mu);
        mu_Id id = mu_get_id(&g_kwcc_mu, label, strlen(label));
        mu_update_control(&g_kwcc_mu, id, r, 0);

        int editing_here = kwcc_edit_is_editing() && kwcc_edit_editing_label() &&
                           strcmp(kwcc_edit_editing_label(), label) == 0;

        /* 绘制：字段背景 + 值文本（编辑态原生控件覆盖在上层，GL 不重复画文本） */
        mu_Color bg = editing_here ? (mu_Color){40, 60, 95, 255}
                                   : (mu_Color){52, 52, 58, 255};
        mu_draw_rect(&g_kwcc_mu, r, bg);
        if (!editing_here) {
            if (multiline && g_kwcc_vg) {
                /* 多行展示：nvgText 是单行绘制（\n 不分行——nvgText→fonsTextIterNext
                 * 无换行处理，fontstash.h:1432-1458 取证；行分解 API 需应用自调）。
                 * 按字段宽自行分行（rows 上限 64：栈 ~2.5KB 可忽略，更长内容需
                 * 滚动容器重构属远期），逐行 push 文本命令，顶对齐；
                 * 内容超高时 wheel 滚动查看（无原生滚动条视觉，边界已注明）。
                 * vg NULL（无渲染环境）→ 跳过文本绘制（字体度量不可用） */
                nvgFontFace(g_kwcc_vg, kwcc_get_font());
                nvgFontSize(g_kwcc_vg, 14);   /* 与渲染端 MU_COMMAND_TEXT 一致 */
                float asc, desc, lh;
                nvgTextMetrics(g_kwcc_vg, &asc, &desc, &lh);
                NVGtextRow rows[64];
                int nrows = nvgTextBreakLines(g_kwcc_vg, value ? value : "", NULL,
                                              (float)(r.w - 10), rows, 64);
                int content_h = (int)(asc - desc + lh * (float)nrows) + 8;
                int max_off = content_h > r.h ? content_h - r.h : 0;
                int off = kwcc_ui_edit_scroll_get(label);
                if (mu_mouse_over(&g_kwcc_mu, r) && g_kwcc_mu.scroll_delta.y != 0) {
                    off -= g_kwcc_mu.scroll_delta.y;   /* scroll_delta 是 mu_Vec2 */
                    if (off < 0) off = 0;
                    if (off > max_off) off = max_off;
                    kwcc_ui_edit_scroll_set(label, off);
                }
                mu_push_clip_rect(&g_kwcc_mu, r);
                for (int i = 0; i < nrows; i++) {
                    float y = r.y + asc + 3 + lh * (float)i - (float)off;
                    if (y + lh < (float)r.y || y > (float)(r.y + r.h)) continue;
                    mu_draw_text(&g_kwcc_mu, NULL, rows[i].start,
                                 (int)(rows[i].end - rows[i].start),
                                 (mu_Vec2){r.x + 5, y},
                                 g_kwcc_mu.style->colors[MU_COLOR_TEXT]);
                }
                mu_pop_clip_rect(&g_kwcc_mu);
            } else {
                mu_draw_control_text(&g_kwcc_mu, value ? value : "", r,
                                     MU_COLOR_TEXT, 0);
            }
        }

        /* 命中 + 可见性（与 clip 相交） */
        int clicked = mu_mouse_over(&g_kwcc_mu, r) &&
                      (g_kwcc_mu.mouse_pressed == MU_MOUSE_LEFT);
        mu_Rect clip = mu_get_clip_rect(&g_kwcc_mu);
        int visible = (r.x < clip.x + clip.w && r.x + r.w > clip.x &&
                       r.y < clip.y + clip.h && r.y + r.h > clip.y);

        kwcc_edit_report_field(label, value ? value : "", topic ? topic : "",
                               multiline, (float)r.x, (float)r.y,
                               (float)r.w, (float)r.h, clicked, visible);
        return JS_UNDEFINED;
    }
    if (strcmp(method, "textboxValue") == 0) {
        JSCStringBuf lbuf;
        const char *label = JS_ToCString(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &lbuf);
        if (!label) return JS_UNDEFINED;
        const char *cl = kwcc_edit_committed_label();
        if (cl && strcmp(cl, label) == 0 && kwcc_edit_committed_value()) {
            return JS_NewString(ctx, kwcc_edit_committed_value());
        }
        return JS_UNDEFINED;
    }
    return JS_UNDEFINED;
}

/* ── Input routing ─────────────────────────────────────────── */

void kwcc_input_mousemove(int x, int y) {
    mu_input_mousemove(&g_kwcc_mu, x, y);
}

void kwcc_input_mousedown(int x, int y, int btn) {
    mu_input_mousedown(&g_kwcc_mu, x, y, btn);
}

void kwcc_input_mouseup(int x, int y, int btn) {
    mu_input_mouseup(&g_kwcc_mu, x, y, btn);
}

void kwcc_input_scroll(int x, int y) {
    mu_input_scroll(&g_kwcc_mu, x, y);
}

void kwcc_input_text(const char *text) {
    mu_input_text(&g_kwcc_mu, text);
}

/* ── Registration ──────────────────────────────────────────── */

void kwcc_ui_init(void) {
    mu_init(&g_kwcc_mu);
    g_kwcc_mu.text_width  = mu_text_width;
    g_kwcc_mu.text_height = mu_text_height;
    g_kwcc_mu.on_window_close = kwcc_on_window_close;
}

void kwcc_ui_free(void) {
    /* Reserved for future UI resource cleanup */
}

void kwcc_process_js(JSContext *ctx, const char *js_text) {
    if (!js_text) return;
    g_kwcc_ui_frame_counter++;
    kwcc_begin_frame();

    mu_begin(&g_kwcc_mu);

    JSValue result = JS_Eval(ctx, js_text, strlen(js_text), "<main.js>", 0);
    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        JSCStringBuf buf;
        const char *s = JS_ToCString(ctx, exc, &buf);
        log_error("JS: %s", s ? s : "(none)");
    }

    mu_end(&g_kwcc_mu);
}

mu_Context *kwcc_get_mu(void) {
    return &g_kwcc_mu;
}

void kwcc_register_ui(JSContext *ctx) {
    kwcc_set_js_context(ctx);

    /* Set microui UI callback */
    kwcc_set_ui_callback(js_ui_dispatch);

    /* Create ui object */
    JSValue global_obj = JS_GetGlobalObject(ctx);
    JSValue ui = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, global_obj, "ui", ui);

    /* JS wrappers */
    const char *methods_js =
        "ui.beginWindow = function(t,x,y,w,h,opt,topic) { kwcc_ui('beginWindow',t,x,y,w,h,opt||0,topic); };\n"
        "ui.endWindow = function() { kwcc_ui('endWindow'); };\n"
        "ui.sync = function(key, visible) { kwcc_ui('sync', key, visible !== undefined ? visible : 1); };\n"
        "ui.beginPanel = function(n,opt) { kwcc_ui('beginPanel',n,opt||0); };\n"
        "ui.endPanel = function() { kwcc_ui('endPanel'); };\n"
        "ui.button = function(t, topic) { kwcc_ui('button', t, topic); };\n"
        "ui.label = function(t) { kwcc_ui('label',t); };\n"
        "ui.slider = function(t,v,lo,hi,topic) { return kwcc_ui('slider',t,v,lo,hi,topic); };\n"
        "ui.layoutRow = function(h,w1,w2,w3,w4) { kwcc_ui('layoutRow',h,w1,w2,w3,w4); };\n"
        "ui.setNext = function(x,y,w,h) { kwcc_ui('setNext',x,y,w,h); };\n"
        "ui.rect = function(x,y,w,h,r,g,b) { kwcc_ui('rect',x,y,w,h,r,g,b); };\n"
        "ui.display = function(t) { kwcc_ui('display',t); };\n"
        "ui.textCentered = function(t) { kwcc_ui('textCentered',t); };\n"
        "ui.loadFont = function(n,p) { kwcc_ui('loadFont',n,p); };\n"
        "ui.setFont = function(n) { kwcc_ui('setFont',n); };\n"
        "ui.loadFontDir = function(d) { kwcc_ui('loadFontDir',d); };\n"
        "ui.svg = function(p,x,y,w,h) { kwcc_ui('svg',p,x||0,y||0,w||100,h||100); };\n"
        "ui.textbox = function(l,v,t,multi) { kwcc_ui('textbox', l, v, t||'', multi?1:0); };\n"
        "ui.textboxValue = function(l) { return kwcc_ui('textboxValue', l); };\n";
    JSValue meth_result = JS_Eval(ctx, methods_js, strlen(methods_js), "<kwcc>", 0);
    if (JS_IsException(meth_result)) {
        JSValue exc = JS_GetException(ctx);
        JSCStringBuf buf;
        const char *s = JS_ToCString(ctx, exc, &buf);
        log_error("kwcc JS_Eval: %s", s ? s : "(none)");
    }
}
