/* kwcc_ui_bus.h — UI→JS event bridge */
#ifndef KWCC_UI_BUS_H
#define KWCC_UI_BUS_H

void kwcc_ui_bus_set_js_ctx(void *ctx);
void kwcc_ui_bus_begin_frame(void);
void kwcc_ui_bus_bind_topic(int id, const char *topic);
void kwcc_ui_bus_dispatch_event(const char *topic, const char *action);

#endif
