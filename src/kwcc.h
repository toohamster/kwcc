/* kwcc.h — kwcc public API */
#ifndef KWCC_H
#define KWCC_H

#include "mquickjs/mquickjs.h"
#include "microui/microui.h"

void          kwcc_init(void);
void          kwcc_free(void);
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
void          kwcc_render_svg(void);

#endif
