#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

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

thread_pool_t pool;

// function prototypes
void thread_pool_init();
void thread_pool_add_task(void (*function)(void *arg), void *arg);
void *worker(void *arg);
void thread_pool_destroy();
void example_task(void *arg);

// init thread pool
void thread_pool_init()
{
    pool.stop = 0;
    pool.head = NULL;
    pool.tail = NULL;

    pthread_mutex_init(&pool.lock, NULL);
    pthread_cond_init(&pool.notify, NULL);

    for (int i = 0; i < MAX_THREADS; i++)
    {
        pthread_create(&pool.threads[i], NULL, worker, NULL);
    }
}

// add task to thread pool
void thread_pool_add_task(void (*function)(void *arg), void *arg)
{
    task_t *new_task = (task_t *)malloc(sizeof(task_t));
    new_task->function = function;
    new_task->arg = arg;
    new_task->next = NULL;

    pthread_mutex_lock(&pool.lock);

    if (pool.tail == NULL)
    {
        pool.head = pool.tail = new_task;
    }
    else
    {
        pool.tail->next = new_task;
        pool.tail = new_task;
    }

    pthread_cond_signal(&pool.notify);
    pthread_mutex_unlock(&pool.lock);
}

// worker thread function
#include <unistd.h>

void* worker(void *arg) {
    printf("[Thread %lu] Started\n", pthread_self());

    while (1) {
        pthread_mutex_lock(&pool.lock);

        while (pool.head == NULL && !pool.stop) {
            printf("[Thread %lu] Waiting for task...\n", pthread_self());
            pthread_cond_wait(&pool.notify, &pool.lock);
            printf("[Thread %lu] Woke up!\n", pthread_self());
        }

        if (pool.stop) {
            pthread_mutex_unlock(&pool.lock);
            printf("[Thread %lu] Exiting...\n", pthread_self());
            break;
        }

        task_t *task = pool.head;

        if (task) {
            pool.head = task->next;
            if (pool.head == NULL) pool.tail = NULL;
        }

        pthread_mutex_unlock(&pool.lock);

        if (task) {
            printf("[Thread %lu] Executing task...\n", pthread_self());
            task->function(task->arg);
            printf("[Thread %lu] Finished task\n", pthread_self());
            free(task);
        }
    }

    return NULL;
}

// destroy thread pool
void thread_pool_destroy()
{
    pthread_mutex_lock(&pool.lock);
    pool.stop = 1;
    pthread_cond_broadcast(&pool.notify);
    pthread_mutex_unlock(&pool.lock);

    for (int i = 0; i < MAX_THREADS; i++)
    {
        pthread_join(pool.threads[i], NULL);
    }

    // free remaining tasks
    while (pool.head != NULL)
    {
        task_t *task = pool.head;
        pool.head = task->next;
        free(task);
    }

    pthread_mutex_destroy(&pool.lock);
    pthread_cond_destroy(&pool.notify);
}

// example task function
void example_task(void *arg)
{
    int num = *(int*)arg;

    printf("    → Task %d started\n", num);
    sleep(1);  // simulate work
    printf("    → Task %d finished\n", num);

    free(arg);
}

// int main()
// {
//     thread_pool_init();

//     char input;
//     int task_id = 0;

//     printf("Press 'a' to add task, 'q' to quit\n");

//     while ((input = getchar()) != 'q')
//     {
//         if (input == 'a')
//         {
//             int *num = (int *)malloc(sizeof(int));
//             *num = task_id++;
//             thread_pool_add_task(example_task, num);
//             printf("Task %d added\n", *(int *)num);
//         }
//         // Add small delay to avoid input buffer issues
//         usleep(10000);
//     }

//     thread_pool_destroy();
//     return 0;
// }