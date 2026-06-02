/* kwcc.h — kwcc public API */
#ifndef KWCC_H
#define KWCC_H

#include "kwcc_base.h"
#include <stdint.h>
#include "mquickjs/mquickjs.h"
#include "microui/microui.h"

void          kwcc_init(void);
void          kwcc_free(void);
JSContext    *kwcc_create_js(void);
void          kwcc_destroy_js(JSContext *ctx);
void          kwcc_process_js(JSContext *ctx, const char *js_text);
mu_Context   *kwcc_get_mu(void);

#endif
