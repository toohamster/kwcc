/* kwcc.h — kwcc public API */
#ifndef KWCC_H
#define KWCC_H

#include <stdint.h>
#include "mquickjs/mquickjs.h"
#include "microui/microui.h"

/* ── SVG cache (shared with main.m for rendering) ── */
#define SVG_CACHE_SIZE 128

/* Forward declare — NSVGimage defined in nanosvg.h (included in main.m) */
typedef struct NSVGimage NSVGimage;

typedef struct {
    uint32_t     hash;
    size_t       content_len;
    NSVGimage   *image;
    int          frame_id;
    int          in_use;
} svg_cache_t;

extern svg_cache_t g_svg_cache[SVG_CACHE_SIZE];
extern int         g_svg_cache_next;
extern int         g_frame_counter;

void          kwcc_init(void);
void          kwcc_free(void);
void          kwcc_begin_frame(void);
JSContext    *kwcc_create_js(void);
void          kwcc_destroy_js(JSContext *ctx);
void          kwcc_process_js(JSContext *ctx, const char *js_text);
mu_Context   *kwcc_get_mu(void);
const char   *kwcc_get_font(void);
void          kwcc_load_font_dir(const char *dir_path);
void          kwcc_input_mousemove(int x, int y);
void          kwcc_input_mousedown(int x, int y, int btn);
void          kwcc_input_mouseup(int x, int y, int btn);
void          kwcc_input_scroll(int x, int y);
void          kwcc_input_text(const char *text);

#endif
