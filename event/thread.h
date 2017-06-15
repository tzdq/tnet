#ifndef TNET_THREAD_H
#define TNET_THREAD_H

//锁模式mode，目前只使用了EVTHREAD_TRY
#define EVTHREAD_WRITE 0x04    //仅用于读写锁：为写操作请求或者释放锁
#define EVTHREAD_READ  0x08    //仅用于读写锁：为读操作请求或者释放锁
#define EVTHREAD_TRY   0x10    //仅用于锁定：仅在可以立刻锁定的时候才请求锁定

#define EVTHREAD_LOCK_API_VERSION 1

//锁类型,目前只支持递归锁
//同一个线程可以多次获取同一个递归锁，不会产生死锁。而如果一个线程多次获取同一个非递归锁，则会产生死锁。
#define EVTHREAD_LOCKTYPE_RECURSIVE 1 //递归锁
#define EVTHREAD_LOCKTYPE_READWRITE 2 //读写锁

//线程库锁的数据结构
struct evthread_lock_callbacks{
    int lock_api_version;//当前锁API的版本，设置为EVTHREAD_LOCK_API_VERSION
    unsigned supported_locktypes;//当前锁支持的类型，位字段EVTHREAD_LOCKTYPE_RECURSIVE和EVTHREAD_LOCKTYPE_READWRITE，递归锁是默认的，读写锁未使用
    void *(*alloc)(unsigned locktype);//分配和初始化类型为locktype的锁，失败返回NULL
    void (*free)(void *lock, unsigned locktype);//释放类型为locktype的锁lock
    int (*lock)(unsigned mode,void *lock);//通过模式mode加锁，成功返回0，失败返回非0
    int (*unlock)(unsigned mode,void *lock);//通过模式mode解锁，成功返回0，失败返回非0
};

//设置线程锁的数据结构，如果使用pthread线程库，不推荐使用这个函数，推荐使用evthread_use_pthreads
int evthread_set_lock_callbacks(const struct evthread_lock_callbacks *);

//条件变量的API版本
#define EVTHREAD_CONDITION_API_VERSION 1

struct timeval;

//条件变量的数据结构
struct evthread_condition_callbacks{
    int condition_api_version;//条件变量API的版本，设置为EVTHREAD_CONDITION_API_VERSION
    void *(*alloc_condition)(unsigned condtype);//分配和初始化条件变量，成功返回条件变量，失败返回NULL，当前condtype设置为0
    void (*free_condition)(void *cond);//释放条件变量
    int (*signal_condition)(void *cond, int broadcast);//broadcast设置为1，所有等待的线程被唤醒，成功返回0，失败返回-1，需要锁
    int (*wait_condition)(void *cond, void *lock, const struct timeval *timeout);//等待条件变量，成功返回0，出错返回-1，超时返回1
};

//如果使用pthread线程库，不推荐使用这个函数，推荐使用evthread_use_pthreads
int evthread_set_condition_callbacks(const struct evthread_condition_callbacks *);

//设置确定的线程id
void evthread_set_id_callback(unsigned long (*id_fn)(void));

//使用pthread线程库
int evthread_use_pthreads(void);

//启用调试锁，如果发生锁错误，断言错误退出，需要在分配锁前调用
void evthread_enable_lock_debugging(void);

struct event_base;
//确保其它线程或者信号处理程序唤醒event_base是安全的，不需要手动调用，成功返回0，失败返回-1
int evthread_make_base_notifiable(struct event_base *base);

#endif //TNET_THREAD_H
