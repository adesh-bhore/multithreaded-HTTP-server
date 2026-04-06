#ifndef SERVER_GLOBALS_H
#define SERVER_GLOBALS_H

#include <stdint.h>
#include "done_queue.h"

// Declared extern here, defined once in main.c
// Both the main loop and every worker thread access these.

extern done_queue_t  g_done_queue;   // finished connections waiting for EPOLLOUT
extern int           g_eventfd;      // eventfd — watched by epoll, written by workers

// Signal the main loop: push conn then call this.
// Writes 1 to g_eventfd — epoll_wait wakes immediately.
static inline void signal_main_loop(void)
{
    uint64_t val = 1;
    // write is async-signal-safe and thread-safe for eventfd
    // Multiple workers writing concurrently is fine — the kernel
    // adds the values, so epoll fires once per write minimum.
    if (write(g_eventfd, &val, sizeof(val)) < 0 && errno != EAGAIN)
        perror("eventfd write");
}

#endif