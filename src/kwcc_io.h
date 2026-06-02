/* kwcc_io.h — I/O Reactor (Layer 1) */
#ifndef KWCC_IO_H
#define KWCC_IO_H

#include <sys/select.h>

#define KWCC_IO_MAX_FDS 128  /* compile-time upper bound (memory: 128*24 = 3KB) */

typedef void (*kwcc_io_callback_t)(int fd, void *user_data);

typedef struct {
    int                fd;
    kwcc_io_callback_t callback;
    void              *user_data;
    int                in_use;
} kwcc_io_slot_t;

void kwcc_io_init(void);
void kwcc_io_register(int fd, kwcc_io_callback_t cb, void *user_data);
void kwcc_io_unregister(int fd);
void kwcc_io_poll_once(void);  /* 每帧调用，timeout=0 非阻塞 */

#endif
