# Linux下的C线程池

这是一个linux系统下使用C语言编写的基于pthread的线程池，通过预先创建线程，节省线程创建和销毁的时间来提高性能。由于工作线程和管理线程之间使用`eventfd`机制通信，所以只能在Linux系统下运行，并且`eventfd`占用了不少文件描述符。

## 创建线程池
使用`thread_pool_create()`创建线程池，可以指定线程池内的线程数量。目前这个数量只能在创建时声明，线程池不会动态增加线程数量。传入`THREAD_NUM_CPU`则会创建与CPU核心数量一样的线程数。

    struct thread_pool * thread_pool_create(int thread_num);

## 创建任务并放入线程池队列
有两种方法可以创建任务并将任务放入线程池队列，第一种是显式的调用`thread_task_create()`创建，然后调用`thread_pool_queue_task()`将其放入线程池中：

    thread_task_t * thread_task_create(task_func func, void * arg);
    int thread_pool_queue_task(struct thread_pool * pool, thread_task_t * task);

第二种是隐式的调用`thread_pool_new_task()`创建任务，线程池会负责分配`thread_task_t`结构并将其放入队里中：

    int thread_pool_new_task(struct thread_pool * pool, task_func func, void * arg);

两种方法的区别在于第一种方法调用者可以获得指向`thread_task_t`结构的指针，因此可以对任务做些额外的事情，例如等待任务完成后获取任务函数的返回值，但调用者必须自己负责清理`thread_task_t`结构，方法是调用`thread_task_destroy()`函数。第二种方法则不需要调用者做额外的事情，线程池会负责申请并清理`thread_task_t`结构，但调用者就拿不到任务函数的返回值了，因此第二种方法更适合执行不需要返回值的任务。

目前线程池的队列调度算法是简单的LIFO（栈结构）。

## 等待任务完成
对于使用`thread_task_create()`创建的任务，可以先调用`thread_task_wait()`等待任务完成后，再调用`thread_task_get_result()`获得任务函数的返回值：

    int thread_task_wait(thread_task_t * task);
    void * thread_task_get_result(thread_task_t * task);
    
另外，对于线程池，可以调用`thread_pool_wait_all`等待放入线程池任务队列中所有的任务完成：

    void thread_pool_wait_all(struct thread_pool * pool);
    
## 销毁线程池
通过`thread_pool_destory()`可以销毁一个线程池，这个函数会阻塞直到所有工作线程和管理线程退出。非常不建议在程序中途使用这个函数，可能会造成资源泄漏。如果一定要使用这个函数，请先调用`thread_pool_wait_all()`等待所有队列中的任务执行完毕，这样造成资源泄漏的概率要小的多。

    void thread_pool_destory(struct thread_pool * pool);
    
## 其他说明
这个线程池实现了最基础的功能，我个人认为比较适合于计算密集型的应用，另外，线程池是基于pthread的，所以放入线程池中的任务函数不能调用某些pthread函数，例如`pthread_exit()`，调用了这个函数会导致工作线程退出，接下来会发生什么事情我也不知道了。当然，像`pthread_mutex_t`这类锁的调用，只要实现正常，应该是可以用的。
