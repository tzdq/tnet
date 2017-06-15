#ifndef TNET_EVENT_H
#define TNET_EVENT_H

#include <sys/types.h>
#include <sys/time.h>
#include <stdio.h>
#include "util.h"
#include "event_struct.h"

//创建一个event_base对象
struct event_base *event_base_new();

//event_base_loop的标志
#define EVLOOP_ONCE	    0x01    //阻塞直到我们有激活事件，所有激活事件执行回调后退出
#define EVLOOP_NONBLOCK	0x02    //非阻塞：事件就绪，执行高优先级的回调，然后退出

/*
 * 事件循环，成功返回0，有错误发生返回-1，如果因为没有未决/就绪事件退出返回1
 * flags : EVLOOP_ONCE | EVLOOP_NONBLOCK的组合
 */
int event_base_dispatch(struct event_base *);
int event_base_loop(struct event_base *, int);

//销毁一个event_base相关的内存,这个函数不会关闭任何fds或者释放任何作为回调函数的参数传递到event_new的内存
void event_base_free(struct event_base *);

/** 激活事件队列数量的最大值 */
#define EVENT_MAX_PRIORITIES 256

//设置不同事件的优先级，默认情况下，libevent的所有事件具有相同的优先级，但是某些情况下，需要处理一些高优先级的事件，libevent支持严格的优先级队列。
//这个函数应该在event_base_dispatch前使用,不同的优先级其实就是分配多个激活队列，每个激活队列表示一种优先级
int	event_base_priority_init(struct event_base *, int);

//获取激活事件队列的数量
int	event_base_get_npriorities(struct event_base *);

//获取event_base当前使用的IO复用的名字
const char *event_base_get_method(const struct event_base *);

//获取event_base支持的IO复用支持的特性，位掩码
int event_base_get_features(const struct event_base *base);

/*
 * 在指定的时间后退出事件循环
 * 给定定时器到期后的下一个event_base_loop（）迭代将正常完成（处理所有排队的事件），然后再次退出而不阻塞事件。
 * event_base_loop（）的后续调用将正常进行。
 */
int event_base_loopexit(struct event_base *, const struct timeval *);

/*
 * 立即中止活动的event_base_loop（）。
 * event_base_loop（）将在下一个事件完成后中止循环; event_base_loopbreak（）通常从此事件的回调中调用。
 * 此行为类似于“break;” 声明。
 * event_loop（）的后续调用将正常进行。
 */
int event_base_loopbreak(struct event_base *);

//检查事件循环是否被告知通过event_loopbreak（）退出
//在调用event_loopexit（）之后的每个点，该函数将返回true，直到下一次事件循环开始为止
int event_base_got_exit(struct event_base *);

//检查事件循环是否被告知通过event_loopbreak（）立即中止
//在调用event_loopbreak（）之后的每个点，该函数将返回true，直到下一次事件循环开始为止。
int event_base_got_break(struct event_base *);

/*
 * 将'tv'设置为当前时间（由gettimeofday（）返回）
 * 如果存在缓存事件，使用base.tv_cache，反之，则调用gettimeofday（）或clock_gettime（）。
 * 通常，这个值只会在实际处理事件回调时被缓存，如果回调需要很长时间才能执行，这个值可能非常不准确。
 */
int event_base_gettimeofday_cached(struct event_base *base, struct timeval *tv);

//帮助函数
void event_base_dump_events(struct event_base *, FILE *);

/**
  安排一次性活动
  这个函数类似于event_set，但是它只计划回调函数执行一次，而且不需要调用者提供事件结构.

  @param base : event_base的对象
  @param fd : 需要监控的文件描述符，如果不需要设置为-1
  @param events : 监控的事件，可以为EV_READ 、EV_WRITE,  EV_TIMEOUT的组合
  @param callback : 事件触发时的回调函数
  @param arg : 回调函数的参数
  @param timeout : 等待事件的最大时间，NULL使EV_READ / UV_WRITE事件永久等待;NULL使EV_TIMEOUT事件立即成功。

  @return : 成功返回0，有错误发生返回-1
 */
int event_base_once(struct event_base *base,int fd, short events,
                    void (*callback)(int,short,void*),void *arg,const struct timeval *tv);

const struct timeval *event_base_init_common_timeout(struct event_base *base, const struct timeval *duration);

//event设置权限值，如果事件ev已经就绪，设置失败
int	event_priority_set(struct event *, int);

/* 事件的回调函数的格式声明 */
typedef void (*event_callback_fn)(int, short, void *);

//通过信号事件ev获取信号值
#define event_get_signal(ev) ((int)event_get_fd(ev))

//获取事件ev获取信号值或者文件描述符，如果ev上没有socket，返回-1
int event_get_fd(const struct event *ev);

//获取事件ev所属的event_base
struct event_base *event_get_base(const struct event *ev);

//获取事件ev上的具体事件
short event_get_events(const struct event *ev);

//获取事件ev上的回调函数
event_callback_fn event_get_callback(const struct event *ev);

//获取事件ev上的回调函数参数
void *event_get_callback_arg(const struct event *ev);

//获取事件ev的优先级
int event_get_priority(const struct event *ev);

//获取event结构体的大小
size_t event_get_struct_event_size(void);

//获取上述所有参数
void event_get_assignment(const struct event *event, struct event_base **base_out, int *fd_out,
                          short *events_out, event_callback_fn *callback_out, void **arg_out);
/*
 * 获取Libevent支持的所有事件通知机制。
 * 这个函数按照Libevent的优先级返回事件机制:此列表将包括Libevent已编译的所有后端，但是当前OS不一定会支持这些后端。
*/
const char **event_get_supported_methods(void);

/**
 * 当调试模式可用时，通知tnet一个事件应当不再被当做已分配。当调试模式不可用时，什么都不做
 * 这个功能只能被一个未添加的事件调用
 */
void event_debug_unassign(struct event *);

//event_base后端的特性，主要用于event_base后端的配置
enum event_method_feature
{
    /* 支持边缘触发 epoll满足要求*/
    EV_FEATURE_ET = 0x01,

    /* 事件触发的复杂度为O(1),poll和select是O(n),epoll满足*/
    EV_FEATURE_O1 = 0x02,

    /* 支持任意的文件描述符，而不能仅仅支持套接字*/
    EV_FEATURE_FDS = 0x04,
};

//event_base_config的标志,影响event_base的行为
enum event_base_config_flag
{
    /* 不为event_base分配锁，设置此操作导致多线程是非线程安全的*/
    EVENT_BASE_FLAG_NOLOCK = 0x01,

    /* 配置event_base时不检查EVENT_环境变量 */
    EVENT_BASE_FLAG_IGNORE_ENV = 0x02,

    /* Windows专用，启动时设置IOCP调度*/
    EVENT_BASE_FLAG_STARTUP_IOCP = 0x04,

    /* 在执行event_base_loop的时候没有cache时间。该函数的while循环会经常取系统时间，如果cache时间，那么就取cache的。如果没有的话，就只能通过系统提供的函数来获取系统时间*/
    EVENT_BASE_FLAG_NO_CACHE_TIME = 0x08,
};

//分配一个event_config的对象，event_config对象用于改变event_base的行为
struct event_config *event_config_new();

//释放event_config对象
void event_config_free(struct event_config *cfg);

//使用event_config初始化event_base对象
struct event_base *event_base_new_with_config(const struct event_config *);

//设置event_config的属性（成功返回0，失败返回-1）
int event_config_set_num_cpus_hint(struct event_config *cfg, int cpus);
int event_config_require_features(struct event_config *cfg,  int feature);
int event_config_set_flag(struct event_config *cfg, int flag);

//在event_config中设置被禁止的IO复用方法，method是IO复用的名称
int event_config_avoid_method(struct event_config *cfg, const char *method);

//事件的标志
#define EV_TIMEOUT	0x01 // 超时事件
#define EV_READ		0x02 // 读事件
#define EV_WRITE	0x04 // 写事件
#define EV_SIGNAL	0x08 // 信号事件
#define EV_PERSIST	0x10 // 永久事件：激活时不会自动被移除，当超时的永久事件被激活时，其超时时间被重设为0
#define EV_ET		0x20   //ET模式

/*
 * 分配一个event，准备添加
 * 如果events是EV_READ、EV_WRITE、EV_READ|EV_WRITE，fd是文件描述符或者socket，监听可写、可读或者可读写状态
 * 如果events是EV_SIGNAL，fd是需要等待的信号量。
 * 如果events不是上述两种情况，event只能通过超时或者使用event_active手动激活触发，fd此时必须为-1
 * 如果events是EV_TIMEOUT， 这种情况在这儿不起作用.
 * 如果events是EV_PERSIST，它导致event_add添加的事件是永久的，直到event_del调用
 * 如果events是EV_ET，它与EV_READ和EV_WRITE兼容，仅由某些后端支持，它告诉libevent使用边缘触发事件
 * 可以让多个事件都在同一个fds上进行监听;但是它们必须都是边缘触发，或者都不是边缘触发。
 *
 * 当事件就绪时，事件循环会调用回调函数(fd,events,arg) 其中events如果为EV_TIMEOUT表示发送超时，EV_ET表示ET事件发生
 */
struct event *event_new(struct event_base *, int, short, event_callback_fn, void *);

/*
 * 准备一个新的已经分配的event结构体去添加（构造event的参数）
 * 对于活动或者未决事件调用此函数是不安全的，会破坏Libevent内部数据结构，使用event_assign更改现有事件时，只有当它不活动或挂起时
 */
int event_assign(struct event *, struct event_base *, int, short, event_callback_fn, void *);

/*
 * 添加事件到未决事件集合
 * 如果事件ev已经存在一个超时时间，调用event_add会用新超时值替换旧的超时值，或者清除事件的超时值(tv为NULL)
 */
int event_add(struct event *ev, const struct timeval *timeout);

/* 从监听的事件集合中移除事件*/
int event_del(struct event *);

/* 使事件就绪，多线程程序中常用于从另一个线程唤醒运行event_base_loop（）的线程。 */
void event_active(struct event *ev, int res, short ncalls);

/* 将事件与不同的event_base关联，要关联的事件当前不能处于活动状态或待处理状态。*/
int event_base_set(struct event_base *, struct event *);

/* 在fork后重新初始化event_base */
int event_reinit(struct event_base *base);

/* 检查特定事件是否正在等待或已安排调度。*/
int event_pending(const struct event *ev, short events, struct timeval *tv);

/* 判断一个事件是否可以初始化。*/
int event_initialized(const struct event *ev);

//释放event_new返回的event内存，如果事件是未决或者就绪，应该先让它变成非未决和非就绪事件
void event_free(struct event *);

//执行一次的定时器事件的别名
#define evtimer_assign(ev, b, cb, arg)  event_assign((ev), (b), -1, 0, (cb), (arg))
#define evtimer_new(b, cb, arg)	       event_new((b), -1, 0, (cb), (arg))
#define evtimer_add(ev, tv)		       event_add((ev), (tv))
#define evtimer_del(ev)			       event_del(ev)
#define evtimer_pending(ev, tv)		   event_pending((ev), EV_TIMEOUT, (tv))
#define evtimer_initialized(ev)		   event_initialized(ev)

//信号事件别名
#define evsignal_add(ev, tv)		            event_add((ev), (tv))
#define evsignal_assign(ev, b, x, cb, arg)	event_assign((ev), (b), (x), EV_SIGNAL|EV_PERSIST, cb, (arg))
#define evsignal_new(b, x, cb, arg)		    event_new((b), (x), EV_SIGNAL|EV_PERSIST, (cb), (arg))
#define evsignal_del(ev)		                event_del(ev)
#define evsignal_pending(ev, tv)	            event_pending((ev), EV_SIGNAL, (tv))
#define evsignal_initialized(ev)	            event_initialized(ev)

//日志级别
#define EVENT_LOG_DEBUG 0
#define EVENT_LOG_MSG   1
#define EVENT_LOG_WARN  2
#define EVENT_LOG_ERR   3

//日志回调函数格式,serverity对应上述的四个宏定义，日志回调函数中不能调用任何Libevent的API
typedef  void(*event_log_cb)(int severity, const char *msg);

//设置日志的回调函数，Libevent的日志默认是输出到终端的，我们可以通过回调函数输出到文件
void event_set_log_callback(event_log_cb cb);

//致命内部错误回调函数格式，默认情况下。出现致命错误Libevent使用exit退出程序。
//在调用这个致命处理函数前都会调用前面的日志记录函数，其级别是_EVENT_LOG_ERR
typedef void (*event_fatal_cb)(int err);

//设置致命内部错误回调函数
void event_set_fatal_callback(event_fatal_cb cb);

//设置内存分配的回调函数
void event_set_mem_functions(void *(*malloc_fn)(size_t sz),
                             void *(*realloc_fn)(void *ptr, size_t sz),
                             void (*free_fn)(void *ptr));
#endif //TNET_EVENT_H
