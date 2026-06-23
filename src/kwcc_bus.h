/* kwcc_bus.h — Generic C Pub/Sub event bus (zero external dependencies) */
#ifndef KWCC_BUS_H
#define KWCC_BUS_H

#include <stddef.h>
#include <stdint.h>

typedef uint64_t kwcc_bus_sub_id_t;
typedef void (*kwcc_bus_cb_t)(const char *topic, const void *data, size_t len, void *user_data);

void              kwcc_bus_init(void);
kwcc_bus_sub_id_t kwcc_bus_subscribe(const char *topic, kwcc_bus_cb_t cb, void *user_data);
void              kwcc_bus_unsubscribe(kwcc_bus_sub_id_t id);
void              kwcc_bus_publish(const char *topic, const void *data, size_t len);

#endif
