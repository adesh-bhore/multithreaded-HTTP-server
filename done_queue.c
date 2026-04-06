#include "done_queue.h"
#include <string.h>
#include <stdio.h>

void done_queue_init(done_queue_t *q)
{
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->lock, NULL);
}

int done_queue_push(done_queue_t *q, connection_t *conn)
{
    pthread_mutex_lock(&q->lock);

    if (q->count >= DONE_QUEUE_CAPACITY)
    {
        pthread_mutex_unlock(&q->lock);
        fprintf(stderr, "done_queue full — dropping connection\n");
        return -1;
    }

    q->slots[q->tail] = conn;
    q->tail = (q->tail + 1) % DONE_QUEUE_CAPACITY;
    q->count++;

    pthread_mutex_unlock(&q->lock);
    return 0;
}

connection_t *done_queue_pop(done_queue_t *q)
{
    pthread_mutex_lock(&q->lock);

    if (q->count == 0)
    {
        pthread_mutex_unlock(&q->lock);
        return NULL;
    }

    connection_t *conn = q->slots[q->head];
    q->head  = (q->head + 1) % DONE_QUEUE_CAPACITY;
    q->count--;

    pthread_mutex_unlock(&q->lock);
    return conn;
}

void done_queue_destroy(done_queue_t *q)
{
    pthread_mutex_destroy(&q->lock);
}