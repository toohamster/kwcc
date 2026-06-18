/* kwcc_ui.h — UI module (microui + nanovg bridge) */
#ifndef KWCC_UI_H
#define KWCC_UI_H

#include "mquickjs/mquickjs.h"
#include "microui/microui.h"
#include "kwcc_base.h"
#include <stdint.h>

/* ── SVG cache (extern, set in kwcc_ui.c, read in main.m) ── */
#define KWCC_UI_SVG_CACHE_SIZE 128

typedef struct NSVGimage NSVGimage;

typedef struct {
    uint32_t     hash;
    size_t       content_len;
    NSVGimage   *image;
    int          frame_id;
    int          in_use;
} kwcc_ui_svg_cache_t;

extern kwcc_ui_svg_cache_t g_kwcc_ui_svg_cache[KWCC_UI_SVG_CACHE_SIZE];
extern int         g_kwcc_ui_frame_counter;

void kwcc_ui_init(void);
void kwcc_ui_free(void);
void kwcc_register_ui(JSContext *ctx);
void kwcc_process_js(JSContext *ctx, const char *js_text);
mu_Context *kwcc_get_mu(void);

void kwcc_load_font_dir(const char *dir_path);
const char *kwcc_get_font(void);

void kwcc_input_mousemove(int x, int y);
void kwcc_input_mousedown(int x, int y, int btn);
void kwcc_input_mouseup(int x, int y, int btn);
void kwcc_input_scroll(int x, int y);
void kwcc_input_text(const char *text);

#endif
