#include "event/thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "evlog.h"
#include "evmemory.h"
#include "evutil.h"
#include "evthread.h"

//globals
int evthread_lock_debugging_enabled_ = 0;
unsigned long (*evthread_id_fn_)(void) = NULL;
struct evthread_lock_callbacks evthread_lock_fns_ = {0,0,NULL,NULL,NULL,NULL};
struct evthread_condition_callbacks evthread_cond_fns_ = {0,NULL,NULL,NULL,NULL};

/* Used for debugging */
static struct evthread_lock_callbacks original_lock_fns_ = {0, 0, NULL, NULL, NULL, NULL};
static struct evthread_condition_callbacks original_cond_fns_ = {0, NULL, NULL, NULL, NULL};

void evthread_set_id_callback(unsigned long (*id_fn)(void)) {
    evthread_id_fn_ = id_fn;
}

struct evthread_lock_callbacks* evthread_get_lock_callbacks(){
    return evthread_lock_debugging_enabled_ ? &original_lock_fns_ : &evthread_lock_fns_;
}

struct evthread_condition_callbacks *evthread_get_condition_callbacks() {
    return evthread_lock_debugging_enabled_ ? &original_cond_fns_ : &evthread_cond_fns_;
}

int evthread_set_lock_callbacks(const struct evthread_lock_callbacks *cbs){
    struct evthread_lock_callbacks *target = evthread_get_lock_callbacks();

    //参数为NULL，取消线程锁功能
    if(!cbs){
        if(target->alloc)
            event_warnx("Trying to disable lock functions after they have been set up will probaby not work.");
        memset(target,0, sizeof(evthread_lock_fns_));
        return 0;
    }
    if(target->alloc){
        //传递相同的互斥锁回调函数结构体指针
        if(target->lock_api_version == cbs->lock_api_version &&
           target->supported_locktypes == cbs->supported_locktypes &&
           target->alloc == cbs->alloc &&
           target->free == cbs->free &&
           target->lock == cbs->lock &&
           target->unlock == cbs->unlock){
            return 0;
        }
        event_warnx("Can't change lock callbacks once they have been initialized.");
        return -1;
    }
    if(cbs->free && cbs->alloc && cbs->lock && cbs->unlock){
        memcpy(target,cbs, sizeof(evthread_lock_fns_));
        return event_global_setup_locks(1);
    }
    else return -1;
}

int evthread_set_condition_callbacks(const struct evthread_condition_callbacks * cbs){
    struct evthread_condition_callbacks *target = evthread_get_condition_callbacks();

    //取消条件变量功能
    if(!cbs){
        if(target->alloc_condition)
            event_warnx("Trying to disable condition functions after they have been set up will probaby not work.");
        memset(target,0, sizeof(evthread_cond_fns_));
        return 0;
    }
    //重新设置相同的条件变量回调结构体
    if(target->alloc_condition){
        /* Uh oh; we already had condition callbacks set up.*/
        if (target->condition_api_version == cbs->condition_api_version &&
            target->alloc_condition == cbs->alloc_condition &&
            target->free_condition == cbs->free_condition &&
            target->signal_condition == cbs->signal_condition &&
            target->wait_condition == cbs->wait_condition) {
            /* no change -- allow this. */
            return 0;
        }
        event_warnx("Can't change condition callbacks once they have been initialized.");
        return -1;
    }

    if (cbs->alloc_condition && cbs->free_condition &&
        cbs->signal_condition && cbs->wait_condition) {
        memcpy(target, cbs, sizeof(evthread_cond_fns_));
    }
    //如果是调试锁，那么target返回的就是original_cond_fns_，所以设置时需要重新设置evthread_cond_fns_
    if (evthread_lock_debugging_enabled_) {
        evthread_cond_fns_.alloc_condition = cbs->alloc_condition;
        evthread_cond_fns_.free_condition = cbs->free_condition;
        evthread_cond_fns_.signal_condition = cbs->signal_condition;
    }
    return 0;
}

//debug递归锁数据结构
struct debug_lock{
    unsigned locktype;//锁类型
    unsigned long held_by;//这个锁是被哪个线程所拥有
    //如果使用读写锁，我们需要一个单独的锁去保护count
    int count;//锁的加锁次数
    void *lock;
};

static void* debug_lock_alloc(unsigned locktype){
    struct debug_lock *result = (struct debug_lock *)mm_malloc(sizeof(struct debug_lock));
    if(!result)return NULL;
    if(original_lock_fns_.alloc){//用户设置过自己的线程锁函数 ，用用户定制的线程锁函数分配一个线程锁
        if(!(result->lock = original_lock_fns_.alloc(locktype|EVTHREAD_LOCKTYPE_RECURSIVE))){
            mm_free(result);
            return NULL;
        }
    }
    else{
        result->lock = NULL;
    }
    result->locktype  = locktype;
    result->count     = 0;
    result->held_by   = 0;

    return result;
}

static void debug_lock_free(void *lock_, unsigned locktype) {
    struct debug_lock *lock = (struct debug_lock *)lock_;
    EVUTIL_ASSERT(lock->count == 0);
    EVUTIL_ASSERT(locktype == lock->locktype);
    if (original_lock_fns_.free) {
        original_lock_fns_.free(lock->lock, lock->locktype|EVTHREAD_LOCKTYPE_RECURSIVE);
    }
    lock->lock = NULL;
    lock->count = -100;
    mm_free(lock);
}

static void evthread_debug_lock_mark_locked(unsigned mode, struct debug_lock *lock){
    ++lock->count;
    if(!(lock->locktype & EVTHREAD_LOCKTYPE_RECURSIVE))
        EVUTIL_ASSERT(lock->count == 1);
    if(evthread_id_fn_){
        unsigned long me;
        me = evthread_id_fn_();
        if(lock->count > 1)
            EVUTIL_ASSERT(lock->held_by == me);
        lock->held_by = me;
    }
}

static int debug_lock_lock(unsigned mode ,void *lock_){
    struct debug_lock *lock = (struct debug_lock *)lock_;
    int res = 0;
    if(lock->locktype & EVTHREAD_LOCKTYPE_READWRITE)
        EVUTIL_ASSERT(mode & (EVTHREAD_READ |EVTHREAD_WRITE));
    else
        EVUTIL_ASSERT((mode & (EVTHREAD_READ|EVTHREAD_WRITE)) == 0);
    if(original_lock_fns_.lock)
        res = original_lock_fns_.lock(mode,lock->lock);
    if(!res){
        evthread_debug_lock_mark_locked(mode,lock);
    }
    return res;
}

static void evthread_debug_lock_mark_unlocked(unsigned mode, struct debug_lock *lock)
{
    if (lock->locktype & EVTHREAD_LOCKTYPE_READWRITE)
        EVUTIL_ASSERT(mode & (EVTHREAD_READ|EVTHREAD_WRITE));
    else
        EVUTIL_ASSERT((mode & (EVTHREAD_READ|EVTHREAD_WRITE)) == 0);
    if (evthread_id_fn_) {
        unsigned long me;
        me = evthread_id_fn_();
        EVUTIL_ASSERT(lock->held_by == me);//检测锁的拥有者是否为要解锁的线程
        if (lock->count == 1)
            lock->held_by = 0;
    }
    --lock->count;
    EVUTIL_ASSERT(lock->count >= 0);
}

static int debug_lock_unlock(unsigned mode, void *lock_)
{
    struct debug_lock *lock = (struct debug_lock *)lock_;
    int res = 0;
    evthread_debug_lock_mark_unlocked(mode, lock);
    if (original_lock_fns_.unlock)
        res = original_lock_fns_.unlock(mode, lock->lock);
    return res;
}

static int debug_cond_wait(void *cond_, void *lock_, const struct timeval *tv)
{
    int r;
    struct debug_lock *lock = (struct debug_lock *)lock_;
    EVUTIL_ASSERT(lock);
    EVLOCK_ASSERT_LOCKED(lock_);
    evthread_debug_lock_mark_unlocked(0, lock);
    r = original_cond_fns_.wait_condition(cond_, lock->lock, tv);
    evthread_debug_lock_mark_locked(0, lock);
    return r;
}

void evthread_enable_lock_debugging(void){
    struct evthread_lock_callbacks cbs = {
            EVTHREAD_LOCK_API_VERSION,
            EVTHREAD_LOCKTYPE_RECURSIVE,
            debug_lock_alloc,
            debug_lock_free,
            debug_lock_lock,
            debug_lock_unlock
    };
    if(evthread_lock_debugging_enabled_)return ;

    //把当前用户定制的锁操作复制到_original_lock_fns结构体变量中。
    memcpy(&original_lock_fns_,&evthread_lock_fns_, sizeof(struct evthread_lock_callbacks));

    //将当前的锁操作设置成调试锁操作。但调试锁操作函数内部还是使用_original_lock_fns的锁操作函数
    memcpy(&evthread_lock_fns_,&cbs, sizeof(struct evthread_lock_callbacks));
    memcpy(&original_cond_fns_, &evthread_cond_fns_, sizeof(struct evthread_condition_callbacks));
    evthread_cond_fns_.wait_condition = debug_cond_wait;

    evthread_lock_debugging_enabled_ = 1;

    event_global_setup_locks(0);
}

int evthread_is_debug_lock_held_(void *lock_)
{
    struct debug_lock *lock = (struct debug_lock *)lock_;
    if (! lock->count)
        return 0;
    if (evthread_id_fn_) {
        unsigned long me = evthread_id_fn_();
        if (lock->held_by != me)
            return 0;
    }
    return 1;
}

void* evthread_setup_global_lock_(void *lock_, unsigned locktype, int enable_locks){
    /* there are four cases here:
	   1) we're turning on debugging; locking is not on.
	   2) we're turning on debugging; locking is on.
	   3) we're turning on locking; debugging is not on.
	   4) we're turning on locking; debugging is on.
     */
    if(!enable_locks && original_lock_fns_.alloc == NULL){
        //Case 1:分配一个调试锁
        EVUTIL_ASSERT(lock_ == NULL);
        return debug_lock_alloc(locktype);
    }
    else if(!enable_locks && original_lock_fns_.alloc != NULL){
        //Case 2:锁定一个调试锁
        struct debug_lock *lock;
        EVUTIL_ASSERT(lock_ != NULL);
        //锁类型不为递归锁
        if(!(locktype & EVTHREAD_LOCKTYPE_RECURSIVE)){
            original_lock_fns_.free(lock_,locktype);
            return debug_lock_alloc(locktype);
        }
        lock = (struct debug_lock *)mm_malloc(sizeof(struct debug_lock));
        if(!lock){
            original_lock_fns_.free(lock_,locktype);
            return NULL;
        }
        lock->lock = lock_;
        lock->locktype = locktype;
        lock->count = 0;
        lock->held_by = 0;
        return lock;
    }
    else if(enable_locks && !evthread_lock_debugging_enabled_){
        /* Case 3: 申请一个常规锁 */
        EVUTIL_ASSERT(lock_ == NULL);
        return evthread_lock_fns_.alloc(locktype);
    }
    else{
        /* Case 4: 用真正的锁填充调试锁*/
        struct debug_lock *lock = (struct debug_lock *)(lock_ ? lock_ : debug_lock_alloc(locktype));
        EVUTIL_ASSERT(enable_locks && evthread_lock_debugging_enabled_);
        EVUTIL_ASSERT(lock->locktype == locktype);
        if(!lock->lock){
            lock->lock = original_lock_fns_.alloc(locktype|EVTHREAD_LOCKTYPE_RECURSIVE);
            if(!lock->lock){
                lock->count = -200;
                mm_free(lock);
                return NULL;
            }
        }
        return lock;
    }
}