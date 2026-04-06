#ifndef SERVER_GLOBALS_H
#define SERVER_GLOBALS_H

#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include "done_queue.h"

extern done_queue_t g_done_queue;
extern int          g_eventfd;

static inline void signal_main_loop(void)
{
    uint64_t val = 1;
    if (write(g_eventfd, &val, sizeof(val)) < 0 && errno != EAGAIN)
        perror("eventfd write");
}

#endif