#include "thread_pool.h"
#include "tp_atomic.h"

#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>

#define OK_VALUE 0x80000000

enum {
    THREAD_POOL_ERROR,
    THREAD_POOL_INIT_FAIL,
    THREAD_POOL_CREATE_THREAD_FAIL,
};

enum {
    THREAD_TYPE_INPOOL,             /* 线程池常驻线程 */
    THREAD_TYPE_TEMP,               /* 临时线程 */
};

enum {
    THREAD_STATUS_IDLE,
    THREAD_STATUS_RUNNING,
};

enum {
    THREAD_TASK_NORMAL,             /* 通过thread_task_create()创建的任务 */
    THREAD_TASK_NO_RESULT,          /* 通过thread_pool_new_task()创建的任务*/
};

enum {
    THREAD_TASK_STATUS_READY,
    THREAD_TASK_STATUS_INQUEUE,
    THREAD_TASK_STATUS_RUNNING,
    THREAD_TASK_STATUS_DONE,
};

struct thread_pool_event {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
};

typedef struct thread_pool_event thread_pool_event_t;

static int thread_pool_event_init(thread_pool_event_t * event)
{
    if (pthread_mutex_init(&event->mutex, NULL) != 0)
        return -1;
    if (pthread_cond_init(&event->cond, NULL) != 0) {
        pthread_mutex_destroy(&event->mutex);
        return -1;
    }
    return 0;
}

static void thread_pool_event_destory(thread_pool_event_t * event)
{
    pthread_mutex_destroy(&event->mutex);
    pthread_cond_destroy(&event->cond);
}

static int thread_pool_event_lock(thread_pool_event_t * event)
{
    return pthread_mutex_lock(&event->mutex);
}

static int thread_pool_event_unlock(thread_pool_event_t * event)
{
    return pthread_mutex_unlock(&event->mutex);
}

static int thread_pool_event_wait(thread_pool_event_t * event)
{
    return pthread_cond_wait(&event->cond, &event->mutex);
}

static int thread_pool_event_trywait(thread_pool_event_t * event, int timeout)
{
    struct timespec abstime;
    abstime.tv_sec = time(NULL) + timeout;
    abstime.tv_nsec = 0;
    return pthread_cond_timedwait(&event->cond, &event->mutex, &abstime);
}

static int thread_pool_event_notify(thread_pool_event_t * event)
{
    return pthread_cond_signal(&event->cond);
}

struct thread_task {
    int task_id;
    task_func func;
    void * arg;
    int task_type;
    int task_status;
    void * result;
    struct thread_task * next;
    thread_pool_event_t event;
};

struct thread_pool {
    int th_num;                                 /* 常驻线程数量 */
    int th_real_num;                            /* 实际线程总数 */

    int pool_status;
    struct thread_struct * threads;             /* 线程数据结构数组 */

    thread_task_t * task_queue;                 /* 任务队列 */
    atomic_t task_total;                        /* 收到的任务总数 */
    atomic_t task_running;                      /* 运行中的任务数量 */
    atomic_t task_queued;                       /* 队列中的任务数量 */
    atomic_t task_done;                         /* 完成的任务总数 */
    atomic_t task_error;                        /* 出错的任务数量 */

    pthread_t manager;                          /* 管理线程 */
    thread_pool_event_t event_queue;            /* 通知管理线程有任务到来的事件 */
    thread_pool_event_t event_alldone;          /* 所有任务完成时的事件通知 */

    struct epoll_event * events_epoll;
};

struct thread_struct {
    pthread_t thread_id;
    struct thread_pool * pool;
    int thread_type;
    int thread_status;
    int eventfds[2];
};

static thread_task_t * _thread_task_create(task_func func, void * arg, int type)
{
    thread_task_t * task;
    if ((task = malloc(sizeof(thread_task_t))) == NULL)
        return NULL;

    if (type == THREAD_TASK_NORMAL) {
        if (thread_pool_event_init(&task->event) != 0) {
            free(task);
            return NULL;
        }
    }

    task->func = func;
    task->arg = arg;
    task->task_type = type;
    task->task_status = THREAD_TASK_STATUS_READY;
    task->result = NULL;
    task->next = NULL;
    return task;
}

thread_task_t * thread_task_create(task_func func, void * arg)
{
    return _thread_task_create(func, arg, THREAD_TASK_NORMAL);
}

int thread_task_wait(thread_task_t * task)
{
    /* 没有放入线程池队列？ */
    if (task->task_status == THREAD_TASK_STATUS_READY)
        return -1;

    thread_pool_event_lock(&task->event);
    while (task->task_status != THREAD_TASK_STATUS_DONE)
        thread_pool_event_wait(&task->event);
    thread_pool_event_unlock(&task->event);
    return 0;
}

static int thread_task_notify(thread_task_t * task)
{
    thread_pool_event_lock(&task->event);
    task->task_status = THREAD_TASK_STATUS_DONE;
    thread_pool_event_notify(&task->event);
    thread_pool_event_unlock(&task->event);
    return 0;
}

void thread_task_destroy(thread_task_t * task, int free_result)
{
    if (task->task_type == THREAD_TASK_NORMAL)
        thread_pool_event_destory(&task->event);
    if (free_result)
        free(task->result);
    free(task);
}

void * thread_task_get_result(thread_task_t * task)
{
    return task->result;
}

static void thread_pool_signal_alldone(struct thread_pool * pool)
{
    thread_pool_event_lock(&pool->event_alldone);
    thread_pool_event_notify(&pool->event_alldone);
    thread_pool_event_unlock(&pool->event_alldone);
}

static void * thread_pool_handler(void * vthread)
{
    struct thread_struct * thread = vthread;
    thread_task_t * task = NULL;
    struct thread_pool * pool = thread->pool;
    uint64_t ok = OK_VALUE, task_addr = 0;
    while (1) {
        if (read(thread->eventfds[1], &task_addr, sizeof(uint64_t)) != sizeof(uint64_t)) {
            atomic_inc(&pool->task_error);
            continue;
        }

        task = (thread_task_t *)task_addr;
        thread->thread_status = THREAD_STATUS_RUNNING;
        task->task_status = THREAD_TASK_STATUS_RUNNING;
        atomic_inc(&pool->task_running);
        task->result = task->func(task->arg);

        atomic_dec(&pool->task_running);
        atomic_inc(&pool->task_done);
        thread->thread_status = THREAD_STATUS_IDLE;

        if (task->task_type == THREAD_TASK_NO_RESULT)
            thread_task_destroy(task, 0);
        else if (task->task_type == THREAD_TASK_NORMAL)
            thread_task_notify(task);

        if (atomic_read(&pool->task_done) == atomic_read(&pool->task_total))
            thread_pool_signal_alldone(pool);

        if (write(thread->eventfds[0], &ok, sizeof(uint64_t)) != sizeof(uint64_t))
            atomic_inc(&pool->task_error);

    }
    return NULL;
}

static struct thread_struct * thread_pool_get_thread_by_0fd(struct thread_pool * pool, int fd)
{
    for (int i = 0; i < pool->th_num; i++) {
        struct thread_struct * thread = &pool->threads[i];
        if (thread->eventfds[0] == fd)
            return thread;
    }
    return NULL;
}

static void * thread_pool_manager(void * vpool)
{
    struct thread_pool * pool = vpool;
    int epollfd = epoll_create(1);
    struct epoll_event event;

    for (int i = 0; i < pool->th_num; i++) {
        struct thread_struct * thread = &pool->threads[i];
        thread->thread_type = THREAD_TYPE_INPOOL;
        thread->thread_status = THREAD_STATUS_IDLE;
        thread->pool = pool;
        thread->eventfds[0] = eventfd(OK_VALUE, 0);
        thread->eventfds[1] = eventfd(0, 0);
        event.events = EPOLLIN;
        event.data.fd = thread->eventfds[0];

        int epoll_ctl_ret = epoll_ctl(epollfd, EPOLL_CTL_ADD, thread->eventfds[0], &event);
        int pthread_create_ret = pthread_create(&thread->thread_id, NULL, thread_pool_handler, thread);
        if (epoll_ctl_ret != 0 || pthread_create_ret != 0) {
            for (int j = 0; j < i; j++)
                pthread_cancel(pool->threads[i].thread_id);
            pool->pool_status = THREAD_POOL_CREATE_THREAD_FAIL;
            return NULL;
        }
    }
    uint64_t ok = 0, task_addr = 0;
    while (1) {
        thread_task_t * taskqueue, *tmptask;
        thread_pool_event_lock(&pool->event_queue);
        while (pool->task_queue == NULL)
            thread_pool_event_wait(&pool->event_queue);
        taskqueue = pool->task_queue;
        pool->task_queue = NULL;
        thread_pool_event_unlock(&pool->event_queue);

        while (taskqueue != NULL) {
            int fdn = epoll_wait(epollfd, pool->events_epoll, pool->th_num, -1);
            for (int i = 0; i < fdn && taskqueue != NULL; i++) {
                struct epoll_event * e = &pool->events_epoll[i];
                if (e->events & EPOLLIN) {
                    atomic_dec(&pool->task_queued);
                    int fd = e->data.fd;
                    read(fd, &ok, sizeof(uint64_t));
                    assert(ok == OK_VALUE);

                    struct thread_struct * thread;
                    thread = thread_pool_get_thread_by_0fd(pool, fd);
                    /* 这里不能直接在for里用taskqueue = taskqueue->next
                     * 因为在执行这句语句之前可能执行任务的线程就被调度并且执行完毕
                     * 将这个task结构释放掉了，这时next就会成为无效指针
                     * 所以需要一个临时变量
                     */
                    tmptask = taskqueue->next;
                    task_addr = (uint64_t)taskqueue;
                    if (write(thread->eventfds[1], &task_addr, sizeof(uint64_t)) != sizeof(uint64_t))
                        atomic_inc(&pool->task_error);

                    taskqueue = tmptask;
                }
            }
        }
    }
    return NULL;
}

static int get_cpu_count()
{
    return sysconf(_SC_NPROCESSORS_ONLN);
}

void thread_pool_destory(struct thread_pool * pool)
{
    void * ret;
    for (int i = 0; i < pool->th_num; i++)
        pthread_cancel(pool->threads[i].thread_id);
    for (int i = 0; i < pool->th_num; i++)
        pthread_join(pool->threads[i].thread_id, &ret);

    pthread_cancel(pool->manager);
    pthread_join(pool->manager, &ret);

    free(pool->threads);
    free(pool->events_epoll);
    thread_pool_event_destory(&pool->event_queue);
    thread_pool_event_destory(&pool->event_alldone);

    struct thread_task * task = pool->task_queue;
    while (task) {
        struct thread_task * tmp = task->next;
        thread_task_destroy(task, 0);
        task = tmp;
    }
}

struct thread_pool * thread_pool_create(int thread_num)
{
    assert(sizeof(void *) == 8);
    if (thread_num == THREAD_NUM_CPU)
        thread_num = get_cpu_count();

    struct thread_pool * pool;
    if ((pool = malloc(sizeof(struct thread_pool))) == NULL)
        return NULL;
    pool->th_num = thread_num;
    pool->threads = NULL;
    pool->task_queue = NULL;
    atomic_set(&pool->task_total, 0);
    atomic_set(&pool->task_queued, 0);
    atomic_set(&pool->task_running, 0);
    atomic_set(&pool->task_done, 0);

    pool->threads = calloc(pool->th_num, sizeof(struct thread_struct));
    if (pool->threads == NULL)
        goto err;

    pool->events_epoll = calloc(pool->th_num, sizeof(struct epoll_event));
    if (pool->events_epoll == NULL)
        goto err2;

    if (thread_pool_event_init(&pool->event_queue) != 0)
        goto err3;

    if (thread_pool_event_init(&pool->event_alldone) != 0)
        goto err4;

    if (pthread_create(&pool->manager, NULL, thread_pool_manager, pool) != 0)
        goto err5;

    return pool;

    /*这种写法是不是太逗比了，但是写在每个if里感觉更糟糕 */
err5:
    thread_pool_event_destory(&pool->event_alldone);
err4:
    thread_pool_event_destory(&pool->event_queue);
err3:
    free(pool->events_epoll);
err2:
    free(pool->threads);
err:
    free(pool);
    return NULL;
}

void thread_pool_wait_all(struct thread_pool * pool)
{
    thread_pool_event_lock(&pool->event_alldone);
    while (atomic_read(&pool->task_done) != atomic_read(&pool->task_total))
        thread_pool_event_wait(&pool->event_alldone);
    thread_pool_event_unlock(&pool->event_alldone);
}

static void _thread_pool_queue_task(struct thread_pool * pool, thread_task_t * task)
{
    thread_pool_event_lock(&pool->event_queue);
    task->next = pool->task_queue;
    pool->task_queue = task;
    task->task_status = THREAD_TASK_STATUS_INQUEUE;
    thread_pool_event_notify(&pool->event_queue);
    thread_pool_event_unlock(&pool->event_queue);
    atomic_inc(&pool->task_total);
    atomic_inc(&pool->task_queued);
    task->task_id = atomic_read(&pool->task_total);
}

int thread_pool_new_task(struct thread_pool * pool, task_func func, void * arg)
{
    if (pool == NULL || func == NULL)
        return -1;

    thread_task_t * task = _thread_task_create(func, arg, THREAD_TASK_NO_RESULT);
    if (task == NULL)
        return -1;

    _thread_pool_queue_task(pool, task);
    return 0;
}

int thread_pool_queue_task(struct thread_pool * pool, thread_task_t * task)
{
    _thread_pool_queue_task(pool, task);
    return 0;
}

