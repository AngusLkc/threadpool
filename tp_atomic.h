#ifndef TP_ATOMIC_H_
#define TP_ATOMIC_H_

/* C11新加了一个头文件stdatomic.h
 * 这应该是使用原子操作最好的方法
 * 但gcc4.9才包含了该头文件
 * 直接用内核头文件也不明智
 * 所以先自己写个简单的用用
 */

typedef struct {
    volatile int counter;
} atomic_t;

#define ATOMIC_INIT(i)  { (i) }

static inline int atomic_read(const atomic_t * v)
{
    return v->counter;
}

static inline void atomic_set(atomic_t * v, int i)
{
    v->counter = i;
}

static inline int atomic_add_fetch(atomic_t * v, int i)
{
    return __sync_add_and_fetch(&v->counter, i);
}

static inline int atomic_fetch_add(atomic_t * v, int i)
{
    return __sync_fetch_and_add(&v->counter, i);
}

static inline int atomic_sub_fetch(atomic_t * v, int i)
{
    return __sync_sub_and_fetch(&v->counter, i);
}

static inline int atomic_fetch_sub(atomic_t * v, int i)
{
    return __sync_fetch_and_sub(&v->counter, i);
}

static inline void atomic_inc(atomic_t * v)
{
    atomic_fetch_add(v, 1);
}

static inline void atomic_dec(atomic_t * v)
{
    atomic_fetch_sub(v, 1);
}

static inline int atomic_inc_return(atomic_t * v)
{
    return atomic_add_fetch(v, 1);
}

static inline int atomic_dec_return(atomic_t * v)
{
    return atomic_sub_fetch(v, 1);
}

#endif /* TP_ATOMIC_H_ */
