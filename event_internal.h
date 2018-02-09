#ifndef TNET_EVENT_INTERNAL_H
#define TNET_EVENT_INTERNAL_H

#include "sys/queue.h"
#include <time.h>
#include "event2/event_struct.h"
#include "evsignal.h"
#include "minheap.h"
#include "evmemory.h"
#include "defer.h"

//用于简化event中变量
#define EV_SIGNAL_NEXT	 ev_.ev_signal.ev_signal_next
#define EV_IO_NEXT	     ev_.ev_io.ev_io_next

//struct event中ev_closure的值定义
/** 常规事件 */
#define EV_CLOSURE_NONE 0
/** 信号事件*/
#define EV_CLOSURE_SIGNAL 1
/** 永久非信号事件*/
#define EV_CLOSURE_PERSIST 2

//event_base的后端结构体,后端也可以理解为所谓的IO复用，这儿也用于信号的处理
struct eventop {
    const char *name;//多路IO复用函数的名字
    void *(*init)(struct event_base *);//IO复用初始化，新创建一个后端结构体并返回，返回的指针通过形参evbase来存储，失败返回NULL
    /*
     * 在给定的fd或者signal上添加/删除事件：events是具体的事件：EV_READ，EV_WRITE，EV_SIGNAL，EV_ET...
     * old 是以前我们添加过的事件；fdinfo是通过fd映射的evmap结构体，大小通过fdinfo指定，在fd是第一次添加时应该设置为0
     */
    int (*add)(struct event_base *, int fd, short old, short events, void *fdinfo);//添加事件
    int (*del)(struct event_base *, int fd, short old, short events, void *fdinfo);//删除事件
    int (*dispatch)(struct event_base *, struct timeval *);//事件循环，查看就绪事件，激活事件的event_active被调用，成功返回0，失败返回-1，
    void (*dealloc)(struct event_base *);//从event_base中清理和释放数据

    int need_reinit;//标志变量：event_base是否需要重新初始化
    enum event_method_feature features;//多路IO的特性
    size_t fdinfo_len;//用于一个或者多个激活事件的fd的额外信息的长度，信息记录在evmap中，作为add和del的入参
};

#define event_io_map event_signal_map

struct event_signal_map{
    void **entries;//执行evmap_io或者evmap_signal的指针数组，二级指针
    int nentries;//entries中可用的项目数
};

//公共超时时间队列
struct common_timeout_list{
    struct event_list events;//当前在队列中等待的事件
    struct timeval duration;//用于指示位置的超时值
    struct event timeout_event;
    struct event_base *base;//超时列表所属的event_base
};

//用于从common timeout中获取实际微秒的掩码
#define COMMON_TIMEOUT_MICROSECONDS_MASK       0x000fffff

struct event_change{
    int fd;//事件event发生改变对应的文件描述符或者信号值
    short old_events;
    //在fd上想要读和写的变化，被设置为如下的宏
    // 如果fd是信号量，read_change被设置为EV_CHANGE_SIGNAL，write_change未使用
    uint8_t read_change;
    uint8_t write_change;
};

//read_change和write_change的标志
/* 如果被设置，添加事件 */
#define EV_CHANGE_ADD     0x01
/* 如果被设置, 删除事件.  不可与EV_CHANGE_ADD一起使用*/
#define EV_CHANGE_DEL     0x02
/* 如果被设置, 事件指的是信号，不是文件描述符. */
#define EV_CHANGE_SIGNAL  EV_SIGNAL
/* 为永久事件设置，当前未被使用. */
#define EV_CHANGE_PERSIST EV_PERSIST
/* 为添加ET事件设置 */
#define EV_CHANGE_ET      EV_ET


//调试模式是否打开的标志
#define EVENT_DEBUG_MODE_IS_ON() (0)

/* 结构体event_base是Libevent的Reactor */
struct event_base{
    /* 初始化Reactor时选择的一种后端IO复用机制，并记录在如下字段中 */
    const struct eventop *evsel;
    /*指向IO复用机制真正存储的数据，它通过evsel成员的init函数来进行初始化，类似于类和静态函数的关系，evbase是evsel的实例*/
    void *evbase;

    const struct eventop *evsigsel;//指向信号的后端处理机制
    /* 信号事件处理器使用的数据结构，其中封装了一个由socketpair创建的管道。它用于信号处理函数和事件多路分发器之间的通信 */
    struct evsig_info sig;

    int virtual_event_count;//虚拟事件数量
    int event_count;//加入到该event_base的事件总数
    int event_count_active;//该event_base上就绪事件总数

    int event_gotterm;//是否在处理完活动事件队列上剩余的任务之后终止事件循环
    int event_break;//是否立即终止事件循环
    int event_continue;//是否重新启动一个新的事件循环

    int event_running_priority;//当前正在处理的活动事件队列的优先级

    int running_loop;//事件循环是否启动

    /* 活动事件队列数组，索引值越小的队列，优先级越高。高优先级的活动事件队列中的事件处理器将被优先处理 */
    struct event_list *activequeues;
    int nactivequeues;//活动事件队列数组的大小，即该event_base共有nactivequeues个不同优先级的活动事件队列

    /* 注册事件队列：所有被添加到event_base的事件队列，存放IO事件处理器和信号事件处理器*/
    struct event_list eventqueue;

    /** 延迟回调函数的链表，事件循环每次成功处理完一个活动队列中的所有事件之后，调用一次延迟回调函数 */
    struct deferred_cb_queue defer_queue;

    struct common_timeout_list **common_timeout_queues;/* 通用定时器队列 */
    int n_common_timeouts;/* common_timeout_queues项目数使用数目 */
    int n_common_timeouts_allocated;/* common_timeout_queues分配数目 */

    struct event_io_map io;/* 文件描述符和IO事件之间的映射关系表 */
    struct event_signal_map sigmap;  /* 信号值和信号事件之间的映射关系表 */
    struct min_heap timeheap;/* 时间堆 */

    struct timeval tv_cache;//时间缓存
    struct timeval event_tv;/*used to detect when time is running backwards. */
    struct timeval tv_clock_diff;
    time_t last_updated_clock_diff;/* 上次更新tv_clock_diff的时间 */

    unsigned long th_owner_id;
    void *th_base_lock;//互斥锁
    void *current_event_cond;//条件变量
    int current_event_waiters;//等待条件变量而阻塞的线程数量
    struct event *current_event;//当前事件循环正在执行哪个事件处理器的回调函数

    /* 该event_base的一些配置参数 */
    enum event_base_config_flag flags;

    //子线程通知主线程相关的变量
    int is_notify_pending;//为1：如果base中存在未决的通知，不再提示
    int th_notify_fd[2];//类似pipe
    struct event th_notify;//th_notify通知主线程的事件
    int (*th_notify_fn)(struct event_base *base);//唤醒主线程的回调
};

struct event_config_entry {
    TAILQ_ENTRY(event_config_entry) next;
    const char *avoid_method;
};

//event_base的配置结构体
struct event_config {
    TAILQ_HEAD(event_configq, event_config_entry) entries;

    int n_cpus_hint;      //指明CPU的数量 Windows IOCP使用
    enum event_method_feature require_features;
    enum event_base_config_flag flags;
};

//事件只处理一次
struct event_once {
    struct event ev;

    void (*cb)(int, short, void *);
    void *arg;
};

#define N_ACTIVE_CALLBACKS(base)	((base)->event_count_active + (base)->defer_queue.active_count)

int event_add_nolock(struct event *ev,const struct timeval *tv, int tv_is_absolute);
int event_del_nolock(struct event *ev);
void event_active_nolock(struct event *ev, int res, short count);

#endif //TNET_EVENT_INTERNAL_H
