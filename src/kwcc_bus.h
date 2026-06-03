/* kwcc_bus.h — C→JS message bus bridge */
#ifndef KWCC_BUS_H
#define KWCC_BUS_H

#include "mquickjs/mquickjs.h"

void kwcc_dispatch_event(JSContext *ctx, const char *topic, const char *action);
void kwcc_bind_topic(int id, const char *topic);
void kwcc_bus_begin_frame(void);  /* reset topic map per frame */

#endif
