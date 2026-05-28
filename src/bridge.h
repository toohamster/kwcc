#ifndef BRIDGE_H
#define BRIDGE_H

#include "mquickjs/mquickjs.h"
#include "microui/microui.h"

void          bridge_init(void);
void          bridge_free(void);
JSContext    *bridge_create_js(void);
void          bridge_destroy_js(JSContext *ctx);
void          bridge_process_js(JSContext *ctx, const char *js_text);
mu_Context   *bridge_get_mu(void);
void          bridge_input_mousemove(int x, int y);
void          bridge_input_mousedown(int x, int y, int btn);
void          bridge_input_mouseup(int x, int y, int btn);
void          bridge_input_scroll(int x, int y);
void          bridge_input_text(const char *text);
const char   *bridge_get_font(void);
void          bridge_load_font_dir(const char *dir_path);

#endif
