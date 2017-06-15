#ifndef TNET_EVTHREAD_INTERNAL_H
#define TNET_EVTHREAD_INTERNAL_H

#include "event/thread.h"
#include "evutil.h"

struct event_base;

extern struct evthread_lock_callbacks evthread_lock_fns_;
extern struct evthread_condition_callbacks evthread_cond_fns_;
extern unsigned long (*evthread_id_fn_)(void);
extern int evthread_lock_debugging_enabled_;

//获取当前线程ID，如果线程不可用，返回1
#define EVTHREAD_GET_ID() (evthread_id_fn_ ? evthread_id_fn_() : 1)

//判断线程是否运行在给定event_base,需要加锁
#define EVBASE_IN_THREAD(base) (evthread_id_fn_ == NULL || (base)->th_owner_id == evthread_id_fn_())

//返回true，如果我们需要通知evbase的主线程改变它的状态，因为它当前运行主循环在其它线程，需要加锁
#define EVBASE_NEED_NOTIFY(base) \
    (evthread_id_fn_ != NULL && \
    (base)->running_loop && \
    (base)->th_owner_id != evthread_id_fn_())

//申请内存用于分配锁，存储于lockvar，如果锁不可用，设置lockvar为NULL
#define EVTHREAD_ALLOC_LOCK(lockvar,locktype) \
    ((lockvar) = evthread_lock_fns_.alloc ? evthread_lock_fns_.alloc(locktype): NULL)

//释放锁的内存
#define EVTHREAD_FREE_LOCK(lockvar,locktype) \
    do{  \
        void *lock_tmp_ = (lockvar); \
        if(lock_tmp_ && evthread_lock_fns_.free) \
            evthread_lock_fns_.free(lock_tmp_,(locktype)); \
    }while(0)

//加锁
#define EVLOCK_LOCK(lockvar,mode)	 \
	do { \
		if (lockvar) \
			evthread_lock_fns_.lock(mode, lockvar); \
	} while (0)

//解锁
#define EVLOCK_UNLOCK(lockvar,mode)					\
	do {								\
		if (lockvar)						\
			evthread_lock_fns_.unlock(mode, lockvar);	\
	} while (0)

//lockvar1和lockvar2升序
#define EVLOCK_SORTLOCKS_(lockvar1, lockvar2)				\
	do {								\
		if (lockvar1 && lockvar2 && lockvar1 > lockvar2) {	\
			void *tmp = lockvar1;				\
			lockvar1 = lockvar2;				\
			lockvar2 = tmp;					\
		}							\
	} while (0)

//对evbase进行加锁，针对的是evbase中存在字段名为lockvar
#define EVBASE_ACQUIRE_LOCK(base, lockvar) do {				\
		EVLOCK_LOCK((base)->lockvar, 0);			\
	} while (0)

//evbase解锁，如果它被锁，针对的是evbase中存在字段名为lockvar
#define EVBASE_RELEASE_LOCK(base, lockvar) do {				\
		EVLOCK_UNLOCK((base)->lockvar, 0);			\
	} while (0)

//如果锁调试可用，并且锁不为空，我们假设锁lock已经加锁
#define EVLOCK_ASSERT_LOCKED(lock)					\
	do {								\
		if ((lock) && evthread_lock_debugging_enabled_) {	\
			EVUTIL_ASSERT(evthread_is_debug_lock_held_(lock)); \
		}							\
	} while (0)

//条件变量申请内存
#define EVTHREAD_ALLOC_COND(condvar)					\
	do {								\
		(condvar) = evthread_cond_fns_.alloc_condition ?	\
		    evthread_cond_fns_.alloc_condition(0) : NULL;	\
	} while (0)

//释放条件变量的内存
#define EVTHREAD_FREE_COND(cond)					\
	do {								\
		if (cond)						\
			evthread_cond_fns_.free_condition((cond));	\
	} while (0)

//唤醒单个等待条件变量的线程
#define EVTHREAD_COND_SIGNAL(cond) ((cond) ? evthread_cond_fns_.signal_condition((cond), 0) : 0 )

//唤醒所有等待条件变量的线程
#define EVTHREAD_COND_BROADCAST(cond) ((cond) ? evthread_cond_fns_.signal_condition((cond), 1) : 0 )

//阻塞等待条件变量
#define EVTHREAD_COND_WAIT(cond, lock) ((cond) ? evthread_cond_fns_.wait_condition((cond), (lock), NULL) : 0 )

//设置超时时间等待条件变量，超时返回1
#define EVTHREAD_COND_WAIT_TIMED(cond, lock, tv) ((cond) ? evthread_cond_fns_.wait_condition((cond), (lock), (tv)) : 0 )

//判断锁是否可用
#define EVTHREAD_LOCKING_ENABLED() (evthread_lock_fns_.lock != NULL)

//尝试非阻塞的获取锁lockvar，返回1，如果我们获取到
static inline int EVLOCK_TRY_LOCK_(void *lock);
static inline int EVLOCK_TRY_LOCK_(void *lock) {
    if (lock && evthread_lock_fns_.lock) {
        int r = evthread_lock_fns_.lock(EVTHREAD_TRY, lock);
        return !r;
    }
	else {
        return 1;
    }
}

/** 加锁lock1和lock2。总是以相同的顺序分配锁，所以两个线程用LOCK2锁定两个锁不会死锁。 */
#define EVLOCK_LOCK2(lock1,lock2,mode1,mode2)				\
	do {								\
		void *_lock1_tmplock = (lock1);				\
		void *_lock2_tmplock = (lock2);				\
		EVLOCK_SORTLOCKS_(_lock1_tmplock,_lock2_tmplock);	\
		EVLOCK_LOCK(_lock1_tmplock,mode1);			\
		if (_lock2_tmplock != _lock1_tmplock)			\
			EVLOCK_LOCK(_lock2_tmplock,mode2);		\
	} while (0)

/** 解锁lock1和lock2.  */
#define EVLOCK_UNLOCK2(lock1,lock2,mode1,mode2)				\
	do {								\
		void *_lock1_tmplock = (lock1);				\
		void *_lock2_tmplock = (lock2);				\
		EVLOCK_SORTLOCKS_(_lock1_tmplock,_lock2_tmplock);	\
		if (_lock2_tmplock != _lock1_tmplock)			\
			EVLOCK_UNLOCK(_lock2_tmplock,mode2);		\
		EVLOCK_UNLOCK(_lock1_tmplock,mode1);			\
	} while (0)


int evthread_is_debug_lock_held_(void *lock);
void *evthread_setup_global_lock_(void *lock_, unsigned locktype, int enable_locks);

#define EVTHREAD_SETUP_GLOBAL_LOCK(lockvar, locktype)			\
	do {								\
		lockvar = evthread_setup_global_lock_(lockvar,	(locktype), enable_locks);				\
		if (!lockvar) {						\
			event_warn("Couldn't allocate %s", #lockvar);	\
			return -1;					\
		}							\
	} while (0);

int event_global_setup_locks(const int enable_locks);
int evsig_global_setup_locks(const int enable_locks);

//返回当前evthread_lock_callbacks
struct evthread_lock_callbacks *evthread_get_lock_callbacks(void);

//返回当前evthread_condition_callbacks
struct evthread_condition_callbacks *evthread_get_condition_callbacks(void);

#endif //TNET_EVTHREAD_INTERNAL_H
