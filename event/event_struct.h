#ifndef TNET_EVENT_STRUCT_H
#define TNET_EVENT_STRUCT_H

#include <sys/types.h>
#include <sys/time.h>
#include "util.h"

//描述事件当前的状态
#define EVLIST_TIMEOUT  0x01  // event在time堆中
#define EVLIST_INSERTED 0x02  // event在已注册事件链表中
#define EVLIST_SIGNAL   0x04  // 未见使用
#define EVLIST_ACTIVE   0x08  // event在就绪事件链表中
#define EVLIST_INTERNAL 0x10  // 内部使用标记
#define EVLIST_INIT     0x80  // event 已被初始化
#define EVLIST_ALL      0xff  // 所有事件

#ifndef TAILQ_ENTRY
#define EVENT_DEFINED_TQENTRY_
#define TAILQ_ENTRY(type)						\
struct {								\
	struct type *tqe_next;	/* next element */			\
	struct type **tqe_prev;	/* address of previous next element */	\
}
#endif

#ifndef TAILQ_HEAD
#define EVENT_DEFINED_TQHEAD_
#define TAILQ_HEAD(name, type)			\
struct name {					\
	struct type *tqh_first;			\
	struct type **tqh_last;			\
}
#endif

struct event_base;
struct event {
	TAILQ_ENTRY(event) ev_active_next;// 就绪事件队列（激活事件队列）
	TAILQ_ENTRY(event) ev_next;//注册事件队列
    //超时管理
    union {
        TAILQ_ENTRY(event) ev_next_with_common_timeout;//公共超时时间事件队列
        int min_heap_idx;//该event在最小堆上的位置
    }ev_timeout_pos;//仅仅用于定时事件

	int ev_fd;//对于I/O事件，是文件描述符；对于signal事件，是信号值

    struct event_base *ev_base;//event所属的event_base

    union {
        //IO事件
        struct {
			TAILQ_ENTRY(event) ev_io_next;
            struct timeval ev_timeout;
        }ev_io;
        //信号事件
        struct{
			TAILQ_ENTRY(event) ev_signal_next;
            short ev_ncalls;//信号调用ev_callback次数 通常为1
            short *ev_pncalls;//指向ev_nalls ,要么为NULL
        }ev_signal;
    }ev_;//由于是联合体  io事件和信号事件不可共存

    short ev_events;//记录监听的事件类型 EV_READ EVTIMEOUT之类
    short ev_res;//当前激活事件的类型
	short ev_flags;//事件当前的状态
	uint8_t ev_pri;	//优先级，值越小优先级越高
	uint8_t ev_closure;
    struct timeval ev_timeout;//用于定时器,指定定时器的超时值
	void (*ev_callback)(int, short, void *arg);//事件的回调
	void *ev_arg;//回调函数的参数
};

TAILQ_HEAD(event_list,event);

#ifdef EVENT_DEFINED_TQENTRY_
#undef TAILQ_ENTRY
#endif

#ifdef EVENT_DEFINED_TQHEAD_
#undef TAILQ_HEAD
#endif

#endif //TNET_EVENT_STRUCT_H
