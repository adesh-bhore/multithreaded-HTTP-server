#ifndef DONE_QUEUE_H
#define DONE_QUEUE_H

#include <pthread.h>
#include "server.h"   // for connection_t

// A simple mutex-protected FIFO of finished connection pointers.
// Worker threads push onto the tail.
// The main epoll loop pops from the head after eventfd wakes it.

#define DONE_QUEUE_CAPACITY 4096  // power of 2 — sized for max concurrent conns

typedef struct {
    connection_t   *slots[DONE_QUEUE_CAPACITY];
    int             head;
    int             tail;
    int             count;
    pthread_mutex_t lock;
} done_queue_t;

void  done_queue_init(done_queue_t *q);
int   done_queue_push(done_queue_t *q, connection_t *conn); // returns 0 ok, -1 full
connection_t *done_queue_pop(done_queue_t *q);              // returns NULL if empty
void  done_queue_destroy(done_queue_t *q);

#endif // DONE_QUEUE_H