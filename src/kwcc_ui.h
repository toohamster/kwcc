/* kwcc_ui.h — UI module (microui + nanovg bridge) */
#ifndef KWCC_UI_H
#define KWCC_UI_H

#include "mquickjs/mquickjs.h"
#include "microui/microui.h"
#include "kwcc_base.h"
#include <stdint.h>

/* ── SVG cache (extern, set in kwcc_ui.c, read in main.m) ── */
#define SVG_CACHE_SIZE 128

typedef struct NSVGimage NSVGimage;

typedef struct {
    uint32_t     hash;
    size_t       content_len;
    NSVGimage   *image;
    int          frame_id;
    int          in_use;
} svg_cache_t;

extern svg_cache_t g_svg_cache[SVG_CACHE_SIZE];
extern int         g_frame_counter;

void kwcc_register_ui(JSContext *ctx);
void kwcc_ui_init(void);  /* set microui text callbacks */

void kwcc_load_font_dir(const char *dir_path);

void kwcc_input_mousemove(int x, int y);
void kwcc_input_mousedown(int x, int y, int btn);
void kwcc_input_mouseup(int x, int y, int btn);
void kwcc_input_scroll(int x, int y);
void kwcc_input_text(const char *text);

#endif
