#ifndef TNET_DEFER_INTERNAL_H
#define TNET_DEFER_INTERNAL_H

#include "sys/queue.h"

struct deferred_cb;

typedef void (*deferred_cb_fn)(struct deferred_cb *,void *);

//deferred_cb是一个回调函数：可以计划作为event_base的event_loop的一部分运行，而不是立刻运行
//event_base的活动事件中的所有事件处理完后，需要调用一次延迟回调函数
struct deferred_cb {
    TAILQ_ENTRY (deferred_cb) cb_next;
    unsigned queued : 1;//如果deferred_cb正在event_base中等待，设置为true
    deferred_cb_fn cb;//函数指针：回调执行时需要执行的函数

    void *arg;//回调函数的参数
};

//deferred_cb_queue是我们可以添加并运行的deferred_cb的列表
struct deferred_cb_queue {
    void *lock;//互斥锁
    int active_count;/** 队列中条目deferred_cb的数量 */

    //从另一个线程添加到队列时调用的通知函数。
    void (*notify_fn)(struct deferred_cb_queue *, void *);
    void *notify_arg;

    TAILQ_HEAD (deferred_cb_list, deferred_cb) deferred_cb_list;
};

//初始化延迟回调函数
void event_deferred_cb_init(struct deferred_cb *,deferred_cb_fn,void *);

//取消一个延迟回调函数如果它已经在一个event_base的计划中
void event_deferred_cb_cancel(struct deferred_cb_queue *,struct deferred_cb*);

//激活一个延迟回调函数如果它不在一个event_base的计划中
void event_deferred_cb_schedule(struct deferred_cb_queue *,struct deferred_cb*);

#define LOCK_DEFERRED_QUEUE(q) EVLOCK_LOCK((q)->lock,0)
#define UNLOCK_DEFERRED_QUEUE(q) EVLOCK_UNLOCK((q)->lock,0)

//延迟回调函数队列初始化
void event_deferred_cb_queue_init(struct deferred_cb_queue *);

//获取event_base的延迟回调函数队列
struct deferred_cb_queue *event_base_get_deferred_cb_queue(struct event_base *);
#endif //TNET_DEFER_INTERNAL_H
