#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <pthread.h>


#define MAX_THREADS 4

typedef struct task
{
    void (*function)(void *arg);
    void *arg;
    struct task *next;
} task_t;

typedef struct thread_pool
{
    pthread_t threads[MAX_THREADS];

    task_t *head;
    task_t *tail;

    int stop;
    pthread_mutex_t lock;
    pthread_cond_t notify;

} thread_pool_t;

// function prototypes
void thread_pool_init();
void thread_pool_add_task(void (*function)(void *arg), void *arg);
void *worker(void *arg);
void thread_pool_destroy();
void example_task(void *arg);

#endif // THREAD_POOL_H

