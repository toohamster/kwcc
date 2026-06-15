/* kwcc_io.c — I/O Reactor (Layer 1)
 *
 * Pure POSIX FD manager using select() with zero timeout.
 * Does not block the render main thread. No HTTP/JS knowledge.
 *
 * Runtime limit: default 16, adjustable via kwcc_config("io", { max_fds: N }).
 * Compile-time upper bound: KWCC_IO_MAX_FDS (128).
 */
#include "kwcc_io.h"
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static kwcc_io_slot_t g_io_slots[KWCC_IO_MAX_FDS];
static int            g_io_max_fds = 16;

void kwcc_io_init(void) {
    memset(g_io_slots, 0, sizeof(g_io_slots));

    /* TODO: restore kwcc_config_get_core("io/max_fds") after config layer rebuild */
    int max = 16;
    if (max < 1) max = 1;
    if (max > KWCC_IO_MAX_FDS) max = KWCC_IO_MAX_FDS;
    g_io_max_fds = max;
}

void kwcc_io_register(int fd, kwcc_io_callback_t cb, void *user_data) {
    for (int i = 0; i < g_io_max_fds; i++) {
        if (!g_io_slots[i].in_use) {
            g_io_slots[i].fd = fd;
            g_io_slots[i].callback = cb;
            g_io_slots[i].user_data = user_data;
            g_io_slots[i].in_use = 1;
            return;
        }
    }
}

void kwcc_io_unregister(int fd) {
    for (int i = 0; i < g_io_max_fds; i++) {
        if (g_io_slots[i].in_use && g_io_slots[i].fd == fd) {
            g_io_slots[i].in_use = 0;
            return;
        }
    }
}

void kwcc_io_poll_once(void) {
    fd_set readfds;
    FD_ZERO(&readfds);

    int max_fd = -1;
    int active = 0;

    for (int i = 0; i < g_io_max_fds; i++) {
        if (g_io_slots[i].in_use) {
            FD_SET(g_io_slots[i].fd, &readfds);
            if (g_io_slots[i].fd > max_fd) {
                max_fd = g_io_slots[i].fd;
            }
            active++;
        }
    }

    if (active == 0) return;

    struct timeval tv = {0, 0};
    int ret = select(max_fd + 1, &readfds, NULL, NULL, &tv);

    /* Fix 7: EINTR protection — signal interrupted (window resize, SIGCHLD, etc.) */
    if (ret < 0) {
        if (errno == EINTR) return;
        return;
    }

    if (ret == 0) return;  /* timeout, no data */

    /* Dispatch callbacks for fds with data */
    for (int i = 0; i < g_io_max_fds; i++) {
        if (g_io_slots[i].in_use && FD_ISSET(g_io_slots[i].fd, &readfds)) {
            kwcc_io_slot_t *slot = &g_io_slots[i];
            slot->callback(slot->fd, slot->user_data);
        }
    }
}
