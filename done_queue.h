#ifndef DONE_QUEUE_H
#define DONE_QUEUE_H

#include <pthread.h>
#include "server.h"

#define DONE_QUEUE_CAPACITY 4096

typedef struct {
    connection_t   *slots[DONE_QUEUE_CAPACITY];
    int             head;
    int             tail;
    int             count;
    pthread_mutex_t lock;
} done_queue_t;

void          done_queue_init(done_queue_t *q);
int           done_queue_push(done_queue_t *q, connection_t *conn);
connection_t *done_queue_pop(done_queue_t *q);
void          done_queue_destroy(done_queue_t *q);

#endif