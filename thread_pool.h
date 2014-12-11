#ifndef THREAD_POOL_H_
#define THREAD_POOL_H_

#define THREAD_NUM_CPU      -1

struct thread_pool;
struct thread_task;

typedef struct thread_task thread_task_t;

typedef void * (*task_func)(void *);

/**
 * 创建一个线程池
 * @thread_num: 线程池内线程的数目，传入THREAD_NUM_CPU则默认为
 *      处理器核心数量
 */
struct thread_pool * thread_pool_create(int thread_num);

/**
 * 将一个创建好的任务放入线程池队列中
 * @pool：线程池
 * @task：事先创建的任务
 */
int thread_pool_queue_task(struct thread_pool * pool, thread_task_t * task);

/**
 * 使用func(arg)创建任务并放入队列中，由线程池分配和释放任务结构
 * 任务完成后不能获取任务的返回值
 * @pool：线程池
 * @func：要执行的函数
 * @arg：传给函数的参数
 */
int thread_pool_new_task(struct thread_pool * pool, task_func func, void * arg);

/**
 * 等待所有线程池中的任务完成，包括已经在执行的和在队列中的
 * @pool：线程池
 */
void thread_pool_wait_all(struct thread_pool * pool);

/**
 * 释放一个线程池占用的资源，调用这个函数会阻塞直到所有线程池创建的线程退出
 * 并且会回收所有在队列中的thread_task_t结构，任何其他的对任务结构的指针
 * 都会成为悬空指针。
 * 这个函数会有内存泄露问题，因为已经在执行中的thread_task_t可能没有被释放
 * 一般情况下不应该调用这个函数，还是让程序结束时操作系统来回收资源。
 * @pool：线程池
 */
void thread_pool_destory(struct thread_pool * pool);

/**
 * 创建一个线程池任务，返回的thread_task_t可以用于等待该任务完成后获取任务
 * 的返回值。
 * @func:要执行的函数
 * @arg：传给函数的参数
 */
thread_task_t * thread_task_create(task_func func, void * arg);

/**
 * 释放任务结构占用的资源。
 * @task：要释放的任务
 * @free_result：不为0则释放任务完成后返回的结果指针
 */
void thread_task_destroy(thread_task_t * task, int free_result);

/**
 * 等待一个放入线程池队列的任务执行完毕，成功返回0，若task没有进入队列返回-1
 * @task：要等待的任务
 */
int thread_task_wait(thread_task_t * task);

/**
 * 任务执行完毕后获得函数的返回值，调用此函数前应调用thread_task_wait等待
 * 任务执行完毕，否则结果NULL。
 * @task：要获取结果的任务
 */
void * thread_task_get_result(thread_task_t * task);

#endif /* THREAD_POOL_H_ */
