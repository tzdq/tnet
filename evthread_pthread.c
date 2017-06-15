#include <pthread.h>
#include "event/thread.h"
#include <stdlib.h>
#include "evmemory.h"
#include "evthread.h"

struct event_base;

static pthread_mutexattr_t attr_recursive;

static void* evthread_posix_lock_alloc(unsigned locktype){
    pthread_mutexattr_t *attr = NULL;
    pthread_mutex_t *lock = mm_malloc(sizeof(pthread_mutex_t));
    if(!lock)return NULL;
    if(locktype & EVTHREAD_LOCKTYPE_RECURSIVE)
        attr = &attr_recursive;
    if(pthread_mutex_init(lock,attr)){
        mm_free(lock);
        return NULL;
    }
    return lock;
}

static void evthread_posix_lock_free(void *lock_,unsigned locktype){
    pthread_mutex_t *lock = lock_;
    pthread_mutex_destroy(lock);
    mm_free(lock);
}

static int evthread_posix_lock(unsigned mode,void *lock_){
    pthread_mutex_t *lock = lock_;
    if(mode & EVTHREAD_TRY)
        return pthread_mutex_trylock(lock);
    else return pthread_mutex_lock(lock);
}

static int evthread_posix_unlock(unsigned mode, void *lock_)
{
    pthread_mutex_t *lock = lock_;
    return pthread_mutex_unlock(lock);
}

static unsigned long evthread_posix_get_id(){
    union {
        pthread_t thr;
        unsigned long id;
    }r;
    r.thr = pthread_self();
    return (unsigned long)r.id;
}

static void *evthread_posix_cond_alloc(unsigned condflags)
{
    pthread_cond_t *cond = mm_malloc(sizeof(pthread_cond_t));
    if (!cond)
        return NULL;
    if (pthread_cond_init(cond, NULL)) {
        mm_free(cond);
        return NULL;
    }
    return cond;
}

static void evthread_posix_cond_free(void *cond_)
{
    pthread_cond_t *cond = cond_;
    pthread_cond_destroy(cond);
    mm_free(cond);
}

static int evthread_posix_cond_signal(void *cond_, int broadcast)
{
    pthread_cond_t *cond = cond_;
    int r;
    if (broadcast)
        r = pthread_cond_broadcast(cond);
    else
        r = pthread_cond_signal(cond);
    return r ? -1 : 0;
}

//pthread_cond_timedwait的时间是绝对时间，这里的tv则是等待的时间
static int evthread_posix_cond_wait(void *cond_,void *lock_,const struct timeval *tv){
    int r ;
    pthread_cond_t  *cond = cond_;
    pthread_mutex_t *lock = lock_;
    if(tv){
        struct timeval now,abstime;
        struct timespec ts;
        gettimeofday(&now,NULL);
        evutil_timeradd(&now,tv,&abstime);
        ts.tv_sec = abstime.tv_sec;
        ts.tv_nsec = abstime.tv_usec*1000;
        r = pthread_cond_timedwait(cond,lock,&ts);
        if(r == ETIMEDOUT)return 1;
        else if(r) return -1;
        else return 0;
    }
    else{
        r = pthread_cond_wait(cond,lock);
        return r ? -1 : 0;
    }
}

// 必须在event_base_new函数之前调用,开启多线程，目前只支持递归锁，同时模式只支持EVTHREAD_TRY
//内存分配、日志记录、线程锁这些定制应在使用event、event_base这些结构体之前。因为这些结构体会使用到内存分配、日志记录、线程锁的。
// 定制顺序应该是：内存分配->日志记录->线程锁。
int evthread_use_pthreads(){
    struct evthread_lock_callbacks cbs = {
            EVTHREAD_LOCK_API_VERSION,
            EVTHREAD_LOCKTYPE_RECURSIVE,
            evthread_posix_lock_alloc,
            evthread_posix_lock_free,
            evthread_posix_lock,
            evthread_posix_unlock
    };

    struct evthread_condition_callbacks cond_cbs = {
            EVTHREAD_CONDITION_API_VERSION,
            evthread_posix_cond_alloc,
            evthread_posix_cond_free,
            evthread_posix_cond_signal,
            evthread_posix_cond_wait
    };

    //设置递归锁的属性
    if(pthread_mutexattr_init(&attr_recursive))return -1;
    if(pthread_mutexattr_settype(&attr_recursive,PTHREAD_MUTEX_RECURSIVE))return -1;

    evthread_set_lock_callbacks(&cbs);
    evthread_set_condition_callbacks(&cond_cbs);
    evthread_set_id_callback(evthread_posix_get_id);
    return 0;
}