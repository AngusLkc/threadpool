#include "thread_pool.h"

#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
void * test(void * arg)
{
    printf("%ld\n", (uint64_t)arg);
    return arg;
}

int main()
{
    struct thread_pool * pool = thread_pool_create(THREAD_NUM_CPU);
    thread_task_t * tasks[1000000];
    for (uint64_t i = 0; i < 1000000; i++) {
        tasks[i] = thread_task_create(test, (void *)i);
        thread_pool_queue_task(pool, tasks[i]);
    }
    thread_task_wait(tasks[1]);
    printf("result: %ld\n", (uint64_t)thread_task_get_result(tasks[1]));
    thread_pool_wait_all(pool);
    thread_pool_destory(pool);
    return 0;
}
