#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include "sys/queue.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include "event2/event.h"
#include "event2/event_struct.h"
#include "event2/util.h"
#include "event_internal.h"
#include "evlog.h"
#include "evutil.h"
#include "evthread.h"
#include "event2/thread.h"
#include "evmap.h"
#include "evsignal.h"
//C++编译时需要包含这个头文件
#ifdef __cplusplus
#include "eviomultiplexing.h"
#endif

extern const struct eventop selectops;
extern const struct eventop pollops;
extern const struct eventop epollops;

/* 支持的IO复用数组 */
static const struct eventop *eventops[] = { &epollops, &pollops, &selectops, NULL };

//不建议使用
struct event_base *event_global_current_base = NULL;
#define current_base event_global_current_base

static int use_monotonic = 0;

static int evthread_notify_base(struct event_base *base);

#define EVENT_BASE_ASSERT_LOCKED(base)	EVLOCK_ASSERT_LOCKED((base)->th_base_lock)

static void event_queue_insert(struct event_base *, struct event *, int );
static void event_queue_remove(struct event_base *, struct event *, int);

/*
 * 公共超时是特殊的超时处理，作为队列而不是在minheap中处理。 如果知道将使用相同的超时值来获得数千个超时事件，这比minheap更有效率。
 * tv_usec是32位长的，因为它不能超过999999，所以只使用这些位中的20个，我们使用最高位作为4位magic num，次8位作为 进入event_base的共同超时的索引。
 */
#define MICROSECONDS_MASK       COMMON_TIMEOUT_MICROSECONDS_MASK
#define COMMON_TIMEOUT_IDX_MASK 0x0ff00000
#define COMMON_TIMEOUT_IDX_SHIFT 20
#define COMMON_TIMEOUT_MASK     0xf0000000
#define COMMON_TIMEOUT_MAGIC    0x50000000

#define COMMON_TIMEOUT_IDX(tv) (((tv)->tv_usec & COMMON_TIMEOUT_IDX_MASK)>>COMMON_TIMEOUT_IDX_SHIFT)

/** 判断tv在base中是否是公共超时 */
static inline int is_common_timeout(const struct timeval *tv, const struct event_base *base)
{
    int idx;
    if ((tv->tv_usec & COMMON_TIMEOUT_MASK) != COMMON_TIMEOUT_MAGIC)
        return 0;
    idx = COMMON_TIMEOUT_IDX(tv);
    return idx < base->n_common_timeouts;
}

/* 为真：tv1和tv2具有相同的公共超时索引，或者两者都不是公共超时。 */
static inline int is_same_common_timeout(const struct timeval *tv1, const struct timeval *tv2)
{
    return (tv1->tv_usec & ~MICROSECONDS_MASK) == (tv2->tv_usec & ~MICROSECONDS_MASK);
}

/** 返回相应的 common_timeout_list，要求tv是公共超时 */
static inline struct common_timeout_list *get_common_timeout_list(struct event_base *base, const struct timeval *tv)
{
    return base->common_timeout_queues[COMMON_TIMEOUT_IDX(tv)];
}

/* 添加一个公共超时事件ev到common_timeout_list中 */
static void insert_common_timeout_inorder(struct common_timeout_list *ctl, struct event *ev)
{
    struct event *e;

    //逆序遍历
    TAILQ_FOREACH_REVERSE(e, &ctl->events, event_list, ev_timeout_pos.ev_next_with_common_timeout) {
       //断言都是公共超时事件
        EVUTIL_ASSERT(is_same_common_timeout(&e->ev_timeout, &ev->ev_timeout));
        //如果新添加的公共超时事件的超时值大于遍历得到的事件的超时值，插入
        if (evutil_timercmp(&ev->ev_timeout, &e->ev_timeout, >=)) {
            TAILQ_INSERT_AFTER(&ctl->events, e, ev, ev_timeout_pos.ev_next_with_common_timeout);
            return;
        }
    }
    //在队列中没有找到比插入事件ev的超时值小的事件，插入到头
    TAILQ_INSERT_HEAD(&ctl->events, ev, ev_timeout_pos.ev_next_with_common_timeout);
}

/* 将公共超时队列中的第一个事件的超时添加到小根堆 */
static void common_timeout_schedule(struct common_timeout_list *ctl, const struct timeval *now, struct event *head)
{
    struct timeval timeout = head->ev_timeout;
    timeout.tv_usec &= MICROSECONDS_MASK;
    event_add_nolock(&ctl->timeout_event, &timeout, 1);
}

//如果我们支持monotonic，第一次调用这个函数的时候，会设置use_monotonic为1,这儿我们默认支持
static void detect_monotonic(){
    struct timespec ts;
    static int use_monotonic_initialized = 0;
    if(use_monotonic_initialized)
        return;

    if(clock_gettime(CLOCK_MONOTONIC,&ts) == 0)
        use_monotonic = 1;
    use_monotonic_initialized = 1;
}
//检查更改挂钟时间为单调时间的时间间隔（单位为秒），设为-1表示永不检查
#define CLOCK_SYNC_INTERVAL -1

//根据base设置tp为当前时间，如果存在tv_cache，使用tv_cache，否则根据gettimeofday或者clock_gettime获取
static int gettime(struct event_base *base, struct timeval *tp)
{
    EVENT_BASE_ASSERT_LOCKED(base);

    if (base->tv_cache.tv_sec) {
        *tp = base->tv_cache;
        return (0);
    }

    if(use_monotonic){
        struct timespec ts;
        if(clock_gettime(CLOCK_MONOTONIC,&ts) == -1)
            return -1;

        tp->tv_sec = ts.tv_sec;
        tp->tv_usec = ts.tv_nsec /1000;//ts.tv_nsec是纳秒，需要转化为微秒
        //更新base中的字段
        if (base->last_updated_clock_diff + CLOCK_SYNC_INTERVAL < tp->tv_sec) {
            struct timeval tv;
            gettimeofday(&tv,NULL);
            evutil_timersub(&tv, tp, &base->tv_clock_diff);
            base->last_updated_clock_diff = tp->tv_sec;
        }

        return 0;
    }

    return gettimeofday(tp, NULL);
}

//清空event_base的时间缓存
static inline void clear_time_cache(struct event_base *base)
{
    base->tv_cache.tv_sec = 0;
}

//使用当前时间更新event_base中的tv_cache
static inline void update_time_cache(struct event_base *base)
{
    base->tv_cache.tv_sec = 0;
    if (!(base->flags & EVENT_BASE_FLAG_NO_CACHE_TIME))
        gettime(base, &base->tv_cache);
}

//获取小根堆中的最小超时时间，设置IO复用的最大等待时间，通过tv_p返回
static int timeout_next(struct event_base *base,struct timeval **tv_p){
    struct timeval now;
    struct event *ev;
    struct timeval *tv = *tv_p;
    int res = 0;

    ev = min_heap_top_(&base->timeheap);
    if(ev == NULL){
        //如果没有基于时间的事件处于活动状态等待I / O
        *tv_p = NULL;
        goto out;
    }

    if(gettime(base,&now) == -1){
        res = -1;
        goto out;
    }

    if(evutil_timercmp(&ev->ev_timeout,&now,<=)){
        evutil_timerclear(tv);
        goto out;
    }

    evutil_timersub(&ev->ev_timeout,&now,tv);

    EVUTIL_ASSERT(tv->tv_sec >= 0);
    EVUTIL_ASSERT(tv->tv_usec >= 0);
    event_debug(("timeout_next: in %d seconds", (int)tv->tv_sec));

out:
    return (res);
}

// 检查小根堆中的定时器事件，将就绪的定时器事件从heap上删除，并插入到激活链表中
static void timeout_process(struct event_base *base){
    struct timeval now;
    struct event *ev;

    //没有定时器事件，直接返回
    if(min_heap_empty_(&base->timeheap)){
        return;
    }

    //获取当前时间
    gettime(base,&now);
    while((ev = min_heap_top_(&base->timeheap))){
        if(evutil_timercmp(&ev->ev_timeout,&now,>))
            break;

        /* IO 队列中删除这个事件 */
        event_del_nolock(ev);

        event_debug(("timeout_process: call %p", ev->ev_callback));

        //添加到激活事件队列
        event_active_nolock(ev, EV_TIMEOUT, 1);
    }
}

/* 处理活动信号事件时调用的“闭包”功能 */
static inline void event_signal_closure(struct event_base *base, struct event *ev)
{
    short ncalls;
    int should_break;

    /* Allows deletes to work */
    ncalls = ev->ev_.ev_signal.ev_ncalls;
    if (ncalls != 0)
        ev->ev_.ev_signal.ev_pncalls = &ncalls;
    EVBASE_RELEASE_LOCK(base, th_base_lock);
    while (ncalls) {
        ncalls--;
        ev->ev_.ev_signal.ev_ncalls = ncalls;
        if (ncalls == 0)
            ev->ev_.ev_signal.ev_pncalls = NULL;
        (*ev->ev_callback)(ev->ev_fd, ev->ev_res, ev->ev_arg);

        EVBASE_ACQUIRE_LOCK(base, th_base_lock);
        should_break = base->event_break;
        EVBASE_RELEASE_LOCK(base, th_base_lock);

        if (should_break) {
            if (ncalls != 0)
                ev->ev_.ev_signal.ev_pncalls = NULL;
            return;
        }
    }
}

/* 当我们激活一个永久事件时调用关闭函数。 */
static inline void event_persist_closure(struct event_base *base, struct event *ev)
{
    void (*evcb_callback)(int, short, void *);

    int evcb_fd;
    short evcb_res;
    void *evcb_arg;

    /* 如果我们有一个超时，重新安排永久事件。 */
    if (ev->ev_.ev_io.ev_timeout.tv_sec || ev->ev_.ev_io.ev_timeout.tv_usec) {
        struct timeval run_at, relative_to, delay, now;
        uint32_t usec_mask = 0;
        EVUTIL_ASSERT(is_same_common_timeout(&ev->ev_timeout, &ev->ev_.ev_io.ev_timeout));
        gettime(base, &now);
        if (is_common_timeout(&ev->ev_timeout, base)) {
            delay = ev->ev_.ev_io.ev_timeout;
            usec_mask = delay.tv_usec & ~MICROSECONDS_MASK;
            delay.tv_usec &= MICROSECONDS_MASK;
            if (ev->ev_res & EV_TIMEOUT) {
                relative_to = ev->ev_timeout;
                relative_to.tv_usec &= MICROSECONDS_MASK;
            } else {
                relative_to = now;
            }
        }
        else {
            delay = ev->ev_.ev_io.ev_timeout;
            if (ev->ev_res & EV_TIMEOUT) {
                relative_to = ev->ev_timeout;
            } else {
                relative_to = now;
            }
        }
        evutil_timeradd(&relative_to, &delay, &run_at);
        if (evutil_timercmp(&run_at, &now, <)) {
            evutil_timeradd(&now, &delay, &run_at);
        }
        run_at.tv_usec |= usec_mask;
        event_add_nolock(ev, &run_at, 1);
    }

    evcb_callback = ev->ev_callback;
    evcb_fd = ev->ev_fd;
    evcb_res = ev->ev_res;
    evcb_arg = ev->ev_arg;

    // 解锁
    EVBASE_RELEASE_LOCK(base, th_base_lock);

    // 执行回调
    (evcb_callback)(evcb_fd, evcb_res, evcb_arg);
}

int event_base_set(struct event_base *base, struct event *ev)
{
    /* 只有未初始化的事件才能被关联到不同的event_base */
    if (ev->ev_flags != EVLIST_INIT)
        return (-1);

    ev->ev_base = base;
    ev->ev_pri = base->nactivequeues/2;

    return (0);
}

const char **event_get_supported_methods(void)
{
    static const char **methods = NULL;
    const struct eventop **method;
    const char **tmp;
    int i = 0, k;

    /* 计数 */
    for (method = &eventops[0]; *method != NULL; ++method) {
        ++i;
    }

    /* 分配内存*/
    tmp = (const char**)mm_calloc((i + 1), sizeof(char *));
    if (tmp == NULL)
        return (NULL);

    /* 存储数据 */
    for (k = 0, i = 0; eventops[k] != NULL; ++k) {
        tmp[i++] = eventops[k]->name;
    }
    tmp[i] = NULL;

    if (methods != NULL)
        mm_free((char**)methods);

    methods = tmp;

    return (methods);
}

//在一个单一的激活事件队列中处理所有事件，释放锁，这个函数当它被唤醒时需要持有锁
static int event_process_active_single_queue(struct event_base *base,struct event_list *activeq){
    struct event *ev;
    int count = 0;

    EVUTIL_ASSERT(activeq != NULL);

    for(ev = TAILQ_FIRST(activeq);ev;ev = TAILQ_FIRST(activeq)){
        //判断是否是永久事件
        if(ev->ev_events & EV_PERSIST)
            event_queue_remove(base,ev,EVLIST_ACTIVE);
        else
            event_del_nolock(ev);

        if(!(ev->ev_flags & EVLIST_INTERNAL)) {//非内部使用的事件
            ++count;
        }

        event_debug(("event_process_active: event: %p, %s%scall %p",
                ev,
                ev->ev_res & EV_READ ? "EV_READ " : " ",
                ev->ev_res & EV_WRITE ? "EV_WRITE " : " ",
                ev->ev_callback));

        base->current_event = ev;
        base->current_event_waiters = 0;

        switch(ev->ev_closure){
            case EV_CLOSURE_SIGNAL:
                event_signal_closure(base,ev);
                break;
            case EV_CLOSURE_PERSIST:
                event_persist_closure(base, ev);
                break;
            default:
            case EV_CLOSURE_NONE:
                EVBASE_RELEASE_LOCK(base, th_base_lock);
                (*ev->ev_callback)(ev->ev_fd, ev->ev_res, ev->ev_arg);
                break;
        }
        EVBASE_ACQUIRE_LOCK(base, th_base_lock);
        base->current_event = NULL;
        if (base->current_event_waiters) {
            base->current_event_waiters = 0;
            EVTHREAD_COND_BROADCAST(base->current_event_cond);
        }

        if (base->event_break)
            return -1;
        if (base->event_continue)
            break;
    }
    return count;
}

//在queue中处理最多MAX_DEFERRED个deferred_cb条目。 如果breakptr设置为1，停止。
//开始我们需要在queue上持有锁; 我们处理每个deferred_cb释放锁。
static int event_process_deferred_callbacks(struct deferred_cb_queue *queue, int *breakptr)
{
    int count = 0;
    struct deferred_cb *cb;

#define MAX_DEFERRED 16
    while ((cb = TAILQ_FIRST(&queue->deferred_cb_list))) {
        cb->queued = 0;
        TAILQ_REMOVE(&queue->deferred_cb_list, cb, cb_next);
        --queue->active_count;
        UNLOCK_DEFERRED_QUEUE(queue);

        cb->cb(cb, cb->arg);

        LOCK_DEFERRED_QUEUE(queue);
        if (*breakptr)
            return -1;
        if (++count == MAX_DEFERRED)
            break;
    }
#undef MAX_DEFERRED
    return count;
}

//告诉当前正在运行event_loop的线程（如果有的话）需要在其dispatch()中停止等待（如果有），并处理所有活动事件和延迟回调（如果有的话）。
static int evthread_notify_base(struct event_base *base)
{
    EVENT_BASE_ASSERT_LOCKED(base);
    if (!base->th_notify_fn)
        return -1;
    if (base->is_notify_pending)
        return 0;
    base->is_notify_pending = 1;
    return base->th_notify_fn(base);
}

/** 延迟回调函数队列：唤醒event_base. */
static void notify_base_cbq_callback(struct deferred_cb_queue *cb, void *baseptr)
{
    struct event_base *base = (struct event_base*)baseptr;
    if (EVBASE_NEED_NOTIFY(base))
        evthread_notify_base(base);
}


//通过环境变量判断name是否不可用
static int event_is_method_disabled(const char *name)
{
    char environment[64] = {0};

    evutil_snprintf(environment, sizeof(environment), "EVENT_NO%s", name);
    for (int i = 8; environment[i] != '\0'; ++i)
        environment[i] = toupper((int)environment[i]);
    return (evutil_getenv(environment) != NULL);
}

//deferred_cb相关的代码
void event_deferred_cb_init(struct deferred_cb *cb,deferred_cb_fn fn,void *arg)
{
    memset(cb,0, sizeof(struct deferred_cb));
    cb->cb = fn;
    cb->arg = arg;
}

void event_deferred_cb_cancel(struct deferred_cb_queue *queue,struct deferred_cb *cb)
{
    if(!queue){
        if(current_base)queue = &current_base->defer_queue;
        else return ;
    }
    LOCK_DEFERRED_QUEUE(queue);
    if(cb->queued){
        TAILQ_REMOVE(&queue->deferred_cb_list,cb,cb_next);
        --queue->active_count;
        cb->queued = 0;
    }
    UNLOCK_DEFERRED_QUEUE(queue);
}

void event_deferred_cb_schedule(struct deferred_cb_queue *queue,struct deferred_cb *cb)
{
    if(!queue){
        if(current_base)queue = &current_base->defer_queue;
        else return;
    }
    LOCK_DEFERRED_QUEUE(queue);
    if(!cb->queued){
        cb->queued = 1;
        TAILQ_INSERT_TAIL(&queue->deferred_cb_list,cb,cb_next);
        ++queue->active_count;
        if(queue->notify_fn)
            queue->notify_fn(queue,queue->notify_arg);
    }
    UNLOCK_DEFERRED_QUEUE(queue);
}

void event_deferred_cb_queue_init(struct deferred_cb_queue *cb)
{
    memset(cb, 0, sizeof(struct deferred_cb_queue));
    TAILQ_INIT(&cb->deferred_cb_list);
}

struct deferred_cb_queue *event_base_get_deferred_cb_queue(struct event_base *base)
{
    return base ? &base->defer_queue : NULL;
}

//event_base相关代码
struct event_base *event_base_new(void)
{
    struct event_base *base = NULL;
    struct event_config *cfg = event_config_new();
    if (cfg) {
        base = event_base_new_with_config(cfg);
        event_config_free(cfg);
    }
    return base;
}

void event_base_free(struct event_base *base)
{
    int n_deleted = 0;
    struct event *ev;

    if(base == NULL && current_base)
        base = current_base;
    if(base == NULL){
        event_warnx("%s: no base to free",__func__);
        return ;
    }

    /* 如果存在线程通知的管道 */
    if(base->th_notify_fd[0] != -1){
        event_del(&base->th_notify);
        close(base->th_notify_fd[0]);
        if(base->th_notify_fd[1] != -1)
            close(base->th_notify_fd[1]);
        base->th_notify_fd[0] = -1;
        base->th_notify_fd[1] = -1;
        event_debug_unassign(&base->th_notify);
    }

    /* 删除所有非内部事件 */
    //删除所有注册事件
    for (ev = TAILQ_FIRST(&base->eventqueue);  ev ; ) {
        struct event *next = TAILQ_NEXT(ev,ev_next);
        if (!(ev->ev_flags & EVLIST_INTERNAL)) {
            event_del(ev);
            ++n_deleted;
        }
        ev = next;
    }
    //删除定时器事件
    while((ev = min_heap_top_(&base->timeheap)) != NULL){
        event_del(ev);
        ++n_deleted;
    }
    //删除公共超时时间队列元素
    for (int i = 0; i < base->n_common_timeouts; ++i) {
        struct common_timeout_list *ctl = base->common_timeout_queues[i];
        event_del(&ctl->timeout_event);
        event_debug_unassign(&ctl->timeout_event);
        for (ev = TAILQ_FIRST(&ctl->events);  ev ; ) {
            struct event *next = TAILQ_NEXT(ev,ev_timeout_pos.ev_next_with_common_timeout);
            if(!(ev->ev_flags & EVLIST_INTERNAL)){
                event_del(ev);
                ++n_deleted;
            }
            ev = next;
        }
        mm_free(ctl);
    }
    //释放内存
    if(base->common_timeout_queues)
        mm_free(base->common_timeout_queues);

    //删除激活事件
    for (int i = 0;i < base->nactivequeues;++i) {
        for (ev = TAILQ_FIRST(&base->activequeues[i]); ev; ) {
            struct event *next = TAILQ_NEXT(ev, ev_active_next);
            if (!(ev->ev_flags & EVLIST_INTERNAL)) {
                event_del(ev);
                ++n_deleted;
            }
            ev = next;
        }
    }

    if (n_deleted)
        event_debug(("%s: %d events were still set in base", __func__, n_deleted));

    //释放IO复用
    if(base->evsel != NULL && base->evsel->dealloc != NULL)
        base->evsel->dealloc(base);
;
    for(int i = 0; i < base->nactivequeues;++i)
        EVUTIL_ASSERT(TAILQ_EMPTY(&base->activequeues[i]));

    EVUTIL_ASSERT(min_heap_empty_(&base->timeheap));
    min_heap_dtor_(&base->timeheap);

    mm_free(base->activequeues);
    EVUTIL_ASSERT(TAILQ_EMPTY(&base->eventqueue));

    evmap_io_clear(&base->io);
    evmap_signal_clear(&base->sigmap);

    EVTHREAD_FREE_LOCK(base->th_base_lock,EVTHREAD_LOCKTYPE_RECURSIVE);
    EVTHREAD_FREE_COND(base->current_event_cond);

    //如果我们释放了current_base，则不会有current_base
    if (base == current_base)
        current_base = NULL;
    mm_free(base);
}

int event_base_priority_init(struct event_base *base,int npriorities){
    int r = -1;

    EVBASE_ACQUIRE_LOCK(base,th_base_lock);
    if(N_ACTIVE_CALLBACKS(base) || npriorities < 1 || npriorities >= EVENT_MAX_PRIORITIES)
        goto err;

    if(npriorities == base->nactivequeues)
        goto ok;

    if(base->nactivequeues){
        mm_free(base->activequeues);
        base->nactivequeues = 0;
    }

    /* Allocate our priority queues */
    base->activequeues = (struct event_list *)mm_calloc(npriorities, sizeof(struct event_list));
    if(base->activequeues == NULL){
        event_warn("%s:calloc",__func__);
        goto err;
    }
    base->nactivequeues = npriorities;

    for (int i = 0; i < base->nactivequeues; ++i) {
        TAILQ_INIT(&base->activequeues[i]);
    }
ok:
    r = 0;
err:
    EVBASE_RELEASE_LOCK(base,th_base_lock);
    return r;
}

int event_base_get_npriorities(struct event_base *base){
    int n;
    if(base == NULL)
        base = current_base;

    EVBASE_ACQUIRE_LOCK(base,th_base_lock);
    n = base->nactivequeues;
    EVBASE_RELEASE_LOCK(base,th_base_lock);

    return n;
}

const char *event_base_get_method(const struct event_base *base)
{
    EVUTIL_ASSERT(base);
    return (base->evsel->name);
}

int event_base_get_features(const struct event_base *base)
{
    return base->evsel->features;
}

int event_base_got_exit(struct event_base *base)
{
    int res;
    EVBASE_ACQUIRE_LOCK(base, th_base_lock);
    res = base->event_gotterm;
    EVBASE_RELEASE_LOCK(base, th_base_lock);
    return res;
}

int event_base_got_break(struct event_base *base)
{
    int res;
    EVBASE_ACQUIRE_LOCK(base, th_base_lock);
    res = base->event_break;
    EVBASE_RELEASE_LOCK(base, th_base_lock);
    return res;
}

/* 如果我们当前监听任何事件返回true */
static int event_haveevents(struct event_base *base)
{
    return (base->virtual_event_count > 0 || base->event_count > 0);
}

//回调函数：用于event_base_loopexit告诉event_base该退出循环了
static void event_loopexit_cb(int fd, short what, void *arg)
{
    struct event_base *base = (struct event_base*)arg;
    base->event_gotterm = 1;
}

int event_base_loopexit(struct event_base *base, const struct timeval *tv)
{
    return (event_base_once(base, -1, EV_TIMEOUT, event_loopexit_cb, base, tv));
}

int event_base_loopbreak(struct event_base *base){
    int r = 0;
    if(!base)return -1;

    EVBASE_ACQUIRE_LOCK(base,th_base_lock);
    base->event_break = 1;
    if(EVBASE_NEED_NOTIFY(base))
        r = evthread_notify_base(base);
    else r= 0;
    EVBASE_RELEASE_LOCK(base,th_base_lock);
    return r;
}

//一次性时间的回调函数：唤醒用户回调，释放内存
static void event_once_cb(int fd,short events,void *arg){
    struct event_once *eonce= (struct event_once*)arg;
    (*eonce->cb)(fd,events,eonce->arg);
    event_debug_unassign(&eonce->ev);
    mm_free(eonce);
}

int event_base_once(struct event_base *base,int fd, short events,
    void (*callback)(int,short,void*),void *arg,const struct timeval *tv){
    struct event_once *eonce;
    int res = 0;
    struct timeval etv;

    //不支持信号或永久事件
    if(events &(EV_SIGNAL | EV_PERSIST))
        return -1;

    if((eonce = (struct event_once *)mm_calloc(1,sizeof(struct event_once))) == NULL)
        return -1;
    eonce->arg = arg;
    eonce->cb = callback;

    if((events & (EV_TIMEOUT | EV_SIGNAL | EV_READ | EV_WRITE )) == EV_TIMEOUT){
        if(tv == NULL){
           evutil_timerclear(&etv);
            tv = &etv;
        }
        evtimer_assign(&eonce->ev,base,event_once_cb,eonce);
    }
    else if(events & (EV_READ | EV_WRITE)){
        events &= EV_READ| EV_WRITE;
        event_assign(&eonce->ev,base,fd,events,event_once_cb,eonce);
    }
    else{
        mm_free(eonce);
        return -1;
    }

    if (res == 0)
        res = event_add(&eonce->ev, tv);
    if (res != 0) {
        mm_free(eonce);
        return (res);
    }
    return 0;
}

/*
 * 激活事件存储于激活事件队列（带优先级的队列），priority值越小优先级越高。
 * 按照优先级大小遍历，处理激活事件链表中的所有就绪事件；
 * 低优先级的就绪事件可能得不到及时处理
 */
static int event_process_active(struct event_base *base)
{
    struct event_list *activeq = NULL;
    int  c = 0;

    for (int i = 0; i < base->nactivequeues; ++i) {
        if (TAILQ_FIRST(&base->activequeues[i]) != NULL) {
            base->event_running_priority = i;//按照优先级大小遍历，设置base当前运行的优先级
            activeq = &base->activequeues[i];
            //在特定的优先级的队列中处理激活事件(一个优先级包含一个激活事件队列)
            c = event_process_active_single_queue(base, activeq);
            if (c < 0) {
                goto done;
            } else if (c > 0)//处理真实事件,不要考虑较低优先级的事,如果c==0，我们处理的所有事件都是内部的, 继续。
                break;
        }
    }

    event_process_deferred_callbacks(&base->defer_queue,&base->event_break);

done:
    base->event_running_priority = -1;

    return c;
}

int event_base_dispatch(struct event_base *event_base)
{
    return (event_base_loop(event_base, 0));
}

int event_base_loop(struct event_base *base, int flags){
    const struct eventop *evsel = base->evsel;
    struct timeval tv,*tv_p;
    int res,done,retval = 0;

    EVBASE_ACQUIRE_LOCK(base, th_base_lock);

    if (base->running_loop) {
        event_warnx("%s: event_base already running", __func__);
        EVBASE_RELEASE_LOCK(base, th_base_lock);
        return -1;
    }

    base->running_loop = 1;

    clear_time_cache(base);//清空时间缓存

    if (base->sig.ev_signal_added && base->sig.ev_n_signals_added)
        evsig_set_base(base);//设置信号所属的event_base

    done = 0;

    base->th_owner_id = EVTHREAD_GET_ID();

    base->event_gotterm = base->event_break = 0;
    while(!done){
        base->event_continue = 0;

        //终止循环
        if(base->event_gotterm)break;
        if(base->event_break)break;

        // 根据timer heap中事件的最小超时时间，计算系统I/O demultiplexer的最大等待时间
        tv_p = &tv;
        if(!N_ACTIVE_CALLBACKS(base) && !(flags &EVLOOP_NONBLOCK)){
            timeout_next(base,&tv_p);
        }
        else{
            //如果有就绪事件，我们仅仅轮训新事件不用等待
            evutil_timerclear(&tv);
        }

        //没有注册事件,直接退出
        if (!event_haveevents(base) && !N_ACTIVE_CALLBACKS(base)) {
            event_debug(("%s: no events registered.", __func__));
            retval = 1;
            goto done;
        }

        //更新last wait time，并清空time cache
        gettime(base,&base->event_tv);
        clear_time_cache(base);

        res = evsel->dispatch(base, tv_p);

        if (res == -1) {
            event_debug(("%s: dispatch returned unsuccessfully.", __func__));
            retval = -1;
            goto done;
        }
        // 将time cache赋值为当前系统时间
        update_time_cache(base);

        //检查heap中的timer events，将就绪的timer event从heap上删除，并插入到激活链表中
        timeout_process(base);

        //存在激活事件
        // 寻找最高优先级（priority值越小优先级越高）的激活事件链表，然后处理链表中的所有就绪事件；
        // 因此低优先级的就绪事件可能得不到及时处理
        if (N_ACTIVE_CALLBACKS(base)) {
            int n = event_process_active(base);// 处理激活链表中的就绪event，调用其回调函数执行事件处理
            if ((flags & EVLOOP_ONCE) && N_ACTIVE_CALLBACKS(base) == 0 && n != 0)
                done = 1;
        }
        else if (flags & EVLOOP_NONBLOCK)//非阻塞：事件就绪，执行高优先级的回调，然后退出
            done = 1;
    }
    event_debug(("%s: asked to terminate loop.", __func__));

done :
    // 循环结束，清空时间缓存
    clear_time_cache(base);
    base->running_loop = 0;
    EVBASE_RELEASE_LOCK(base, th_base_lock);

    return (retval);
}

int event_priority_set(struct event *ev, int pri)
{
    if (ev->ev_flags & EVLIST_ACTIVE)
        return (-1);
    if (pri < 0 || pri >= ev->ev_base->nactivequeues)
        return (-1);

    ev->ev_pri = pri;

    return (0);
}

int event_base_gettimeofday_cached(struct event_base *base, struct timeval *tv)
{
    int r;
    if (!base) {
        base = current_base;
        if (!current_base)
            return gettimeofday(tv, NULL);
    }

    EVBASE_ACQUIRE_LOCK(base, th_base_lock);
    if (base->tv_cache.tv_sec == 0) {
        r = gettimeofday(tv, NULL);
    }
    else {
        evutil_timeradd(&base->tv_cache, &base->tv_clock_diff, tv);
        r = 0;
    }
    EVBASE_RELEASE_LOCK(base, th_base_lock);
    return r;
}

#define MAX_COMMON_TIMEOUTS 256
/* 当公共超时队列的超时触发时调用。这意味着（至少）应该运行该队列中的第一个事件，如果有更多事件，则应重新计划超时。 */
static void common_timeout_callback(int fd, short what, void *arg)
{
    struct timeval now;
    struct common_timeout_list *ctl = (struct common_timeout_list *)arg;
    struct event_base *base = ctl->base;
    struct event *ev = NULL;
    EVBASE_ACQUIRE_LOCK(base, th_base_lock);
    gettime(base, &now);
    while (1) {
        ev = TAILQ_FIRST(&ctl->events);
        if (!ev || ev->ev_timeout.tv_sec > now.tv_sec ||
            (ev->ev_timeout.tv_sec == now.tv_sec &&
             (ev->ev_timeout.tv_usec&MICROSECONDS_MASK) > now.tv_usec))
            break;
        event_del_nolock(ev);
        event_active_nolock(ev, EV_TIMEOUT, 1);
    }
    if (ev)
        common_timeout_schedule(ctl, &now, ev);
    EVBASE_RELEASE_LOCK(base, th_base_lock);
}

const struct timeval *event_base_init_common_timeout(struct event_base *base, const struct timeval *duration)
{
    struct timeval tv;
    const struct timeval *result=NULL;
    struct common_timeout_list *new_ctl;

    EVBASE_ACQUIRE_LOCK(base, th_base_lock);
    if (duration->tv_usec > 1000000) {
        memcpy(&tv, duration, sizeof(struct timeval));
        if (is_common_timeout(duration, base))
            tv.tv_usec &= MICROSECONDS_MASK;
        tv.tv_sec += tv.tv_usec / 1000000;
        tv.tv_usec %= 1000000;
        duration = &tv;
    }
    for (int i = 0; i < base->n_common_timeouts; ++i) {
        const struct common_timeout_list *ctl = base->common_timeout_queues[i];
        if (duration->tv_sec == ctl->duration.tv_sec &&
            duration->tv_usec == (ctl->duration.tv_usec & MICROSECONDS_MASK)) {
            EVUTIL_ASSERT(is_common_timeout(&ctl->duration, base));
            result = &ctl->duration;
            goto done;
        }
    }
    if (base->n_common_timeouts == MAX_COMMON_TIMEOUTS) {
        event_warnx("%s: Too many common timeouts already in use; we only support %d per event_base"
                , __func__, MAX_COMMON_TIMEOUTS);
        goto done;
    }
    if (base->n_common_timeouts_allocated == base->n_common_timeouts) {
        int n = base->n_common_timeouts < 16 ? 16 : base->n_common_timeouts*2;
        struct common_timeout_list **newqueues = (struct common_timeout_list **)mm_realloc(base->common_timeout_queues, n*sizeof(struct common_timeout_queue *));
        if (!newqueues) {
            event_warn("%s: realloc",__func__);
            goto done;
        }
        base->n_common_timeouts_allocated = n;
        base->common_timeout_queues = newqueues;
    }
    new_ctl = (struct common_timeout_list*)mm_calloc(1, sizeof(struct common_timeout_list));
    if (!new_ctl) {
        event_warn("%s: calloc",__func__);
        goto done;
    }
    TAILQ_INIT(&new_ctl->events);
    new_ctl->duration.tv_sec = duration->tv_sec;
    new_ctl->duration.tv_usec = duration->tv_usec | COMMON_TIMEOUT_MAGIC |
            (base->n_common_timeouts << COMMON_TIMEOUT_IDX_SHIFT);
    evtimer_assign(&new_ctl->timeout_event, base, common_timeout_callback, new_ctl);
    new_ctl->timeout_event.ev_flags |= EVLIST_INTERNAL;
    event_priority_set(&new_ctl->timeout_event, 0);
    new_ctl->base = base;
    base->common_timeout_queues[base->n_common_timeouts++] = new_ctl;
    result = &new_ctl->duration;

done:
    if (result)
        EVUTIL_ASSERT(is_common_timeout(result, base));

    EVBASE_RELEASE_LOCK(base, th_base_lock);
    return result;
}

//帮助函数
void event_base_dump_events(struct event_base *base, FILE *output)
{
    struct event *e;
    fprintf(output, "Inserted events:\n");
    TAILQ_FOREACH(e, &base->eventqueue, ev_next) {
        fprintf(output, "  %p [fd = %d]%s%s%s%s%s\n",
                (void*)e, e->ev_fd,
                (e->ev_events&EV_READ)?" Read":"",
                (e->ev_events&EV_WRITE)?" Write":"",
                (e->ev_events&EV_SIGNAL)?" Signal":"",
                (e->ev_events&EV_TIMEOUT)?" Timeout":"",
                (e->ev_events&EV_PERSIST)?" Persist":"");

    }
    for (int i = 0; i < base->nactivequeues; ++i) {
        if (TAILQ_EMPTY(&base->activequeues[i]))
            continue;
        fprintf(output, "Active events [priority %d]:\n", i);
        TAILQ_FOREACH(e, &base->eventqueue, ev_next) {
            fprintf(output, "  %p [fd  = %d ]%s%s%s%s\n",
                    (void*)e, e->ev_fd,
                    (e->ev_res&EV_READ)?" Read active":"",
                    (e->ev_res&EV_WRITE)?" Write active":"",
                    (e->ev_res&EV_SIGNAL)?" Signal active":"",
                    (e->ev_res&EV_TIMEOUT)?" Timeout active":"");
        }
    }
}

//event_config相关代码
//判断method是否在cfg中被禁用
static int event_config_is_avoided_method(const struct event_config *cfg, const char *method)
{
    struct event_config_entry *entry;

    TAILQ_FOREACH(entry, &cfg->entries, next) {
        if (entry->avoid_method != NULL &&
            strcmp(entry->avoid_method, method) == 0)
            return (1);
    }
    return (0);
}

struct event_config *event_config_new(void)
{
    struct event_config *cfg = (struct event_config *)mm_calloc(1, sizeof(*cfg));

    if (cfg == NULL)
        return (NULL);

    TAILQ_INIT(&cfg->entries);

    return (cfg);
}

static void event_config_entry_free(struct event_config_entry *entry)
{
    if (entry->avoid_method != NULL)
        mm_free((char *)entry->avoid_method);
    mm_free(entry);
}

void event_config_free(struct event_config *cfg)
{
    struct event_config_entry *entry;

    while ((entry = TAILQ_FIRST(&cfg->entries)) != NULL) {
        TAILQ_REMOVE(&cfg->entries, entry, next);
        event_config_entry_free(entry);
    }
    mm_free(cfg);
}

struct event_base *event_base_new_with_config(const struct event_config *cfg){
    struct event_base *base;
    int should_check_environment;

    if((base = (struct event_base*)mm_calloc(1, sizeof(struct event_base))) == NULL){
        event_warn("%s: calloc", __func__);
        return NULL;
    }

    if(cfg)
        base->flags = cfg->flags;

    //是否应该检查环境变量,设置时间相关的参数
    should_check_environment = !(cfg && (cfg->flags & EVENT_BASE_FLAG_IGNORE_ENV));

    //判断是否使用monotonic
    detect_monotonic();
    gettime(base,&base->event_tv);

    //初始化定时器小根堆
    min_heap_ctor_(&base->timeheap);

    base->sig.ev_signal_pair[0] = -1;
    base->sig.ev_signal_pair[1] = -1;
    base->th_notify_fd[0] = -1;
    base->th_notify_fd[1] = -1;

    TAILQ_INIT(&base->eventqueue);

    event_deferred_cb_queue_init(&base->defer_queue);
    base->defer_queue.notify_fn = notify_base_cbq_callback;
    base->defer_queue.notify_arg = base;

    evmap_io_initmap(&base->io);
    evmap_signal_initmap(&base->sigmap);

    base->evbase = NULL;
    for (int i = 0; eventops[i] && !base->evbase ; ++i) {
        if(cfg != NULL){
            //决定后端是否应该被禁止
            if(event_config_is_avoided_method(cfg,eventops[i]->name))continue;
            if((cfg->require_features & eventops[i]->features) != cfg->require_features)continue;
        }

        //环境变量
        if(should_check_environment && event_is_method_disabled(eventops[i]->name))continue;

        base->evsel = eventops[i];
        base->evbase = base->evsel->init(base);
    }

    if(base->evbase == NULL){
        event_warnx("%s: no event mechanism available",__func__);
        base->evsel = NULL;
        event_base_free(base);
        return NULL;
    }

    if(evutil_getenv("EVENT_SHOW_METHOD"))
        event_msgx("tnet using: %s",base->evsel->name);

    //分配一个单一的激活事件队列
    if(event_base_priority_init(base,1) < 0){
        event_base_free(base);
        return NULL;
    }

    if(EVTHREAD_LOCKING_ENABLED() && (!cfg || !(cfg->flags & EVENT_BASE_FLAG_NOLOCK))){
        int r;
        EVTHREAD_ALLOC_LOCK(base->th_base_lock, 0);
        EVTHREAD_ALLOC_COND(base->current_event_cond);
        base->defer_queue.lock = base->th_base_lock;
        r = evthread_make_base_notifiable(base);
        if(r < 0){
            event_warnx("%s: Unable to make base notifiable.",__func__);
            event_base_free(base);
            return NULL;
        }
    }

    return base;
}

int event_config_require_features(struct event_config *cfg, int features)
{
    if (!cfg)
        return (-1);
    cfg->require_features = features;
    return (0);
}

int event_config_set_num_cpus_hint(struct event_config *cfg, int cpus)
{
    if (!cfg)
        return (-1);
    cfg->n_cpus_hint = cpus;
    return (0);
}

int event_config_set_flag(struct event_config *cfg, int flag)
{
    if (!cfg)
        return -1;
    cfg->flags |= flag;
    return 0;
}

int event_config_avoid_method(struct event_config *cfg, const char *method)
{
    struct event_config_entry *entry = (struct event_config_entry *)mm_malloc(sizeof(*entry));
    if (entry == NULL)
        return (-1);

    if ((entry->avoid_method = mm_strdup(method)) == NULL) {
        mm_free(entry);
        return (-1);
    }

    TAILQ_INSERT_TAIL(&cfg->entries, entry, next);

    return (0);
}

//event相关的代码
struct event *event_new(struct event_base *base, int fd, short events, void (*cb)(int, short, void *), void *arg)
{
    struct event *ev;
    ev = (struct event *)mm_malloc(sizeof(struct event));
    if (ev == NULL)
        return (NULL);
    if (event_assign(ev, base, fd, events, cb, arg) < 0) {
        mm_free(ev);
        return (NULL);
    }

    return (ev);
}

int event_assign(struct event *ev, struct event_base *base, int fd, short events, void (*callback)(int, short, void *), void *arg)
{
    if (!base)
        base = current_base;

    ev->ev_base = base;
    ev->ev_callback = callback;
    ev->ev_arg = arg;
    ev->ev_fd = fd;
    ev->ev_events = events;
    ev->ev_res = 0;
    ev->ev_flags = EVLIST_INIT;
    ev->ev_.ev_signal.ev_ncalls = 0;
    ev->ev_.ev_signal.ev_pncalls = NULL;

    if (events & EV_SIGNAL) {
        if ((events & (EV_READ|EV_WRITE)) != 0) {
            event_warnx("%s: EV_SIGNAL is not compatible with ""EV_READ, EV_WRITE", __func__);
            return -1;
        }
        ev->ev_closure = EV_CLOSURE_SIGNAL;
    }
    else {
        if (events & EV_PERSIST) {
            evutil_timerclear(&ev->ev_.ev_io.ev_timeout);
            ev->ev_closure = EV_CLOSURE_PERSIST;
        }
        else {
            ev->ev_closure = EV_CLOSURE_NONE;
        }
    }

    min_heap_elem_init_(ev);

    if (base != NULL) {
        /* by default, we put new events into the middle priority */
        ev->ev_pri = base->nactivequeues / 2;
    }

    return 0;
}

int event_add(struct event *ev, const struct timeval *tv)
{
    int res;

    if (EVUTIL_FAILURE_CHECK(!ev->ev_base)) {
        event_warnx("%s: event has no event_base set.", __func__);
        return -1;
    }

    EVBASE_ACQUIRE_LOCK(ev->ev_base, th_base_lock);

    res = event_add_nolock(ev, tv, 0);

    EVBASE_RELEASE_LOCK(ev->ev_base, th_base_lock);

    return (res);
}

/* 添加事件，和event_add类似，除了：
 * 1）它要求我们有锁。
 * 2）如果设置了tv_is_absolute，我们将tv视为绝对时间，不是作为当前时间添加的间隔*/
int event_add_nolock(struct event *ev, const struct timeval *tv, int tv_is_absolute)
{
    struct event_base *base = ev->ev_base;
    int res = 0;
    int notify = 0;

    EVENT_BASE_ASSERT_LOCKED(base);

    event_debug(("event_add: event: %p (fd = %d), %s%s%scall %p",
            ev, ev->ev_fd,
            ev->ev_events & EV_READ ? "EV_READ " : " ",
            ev->ev_events & EV_WRITE ? "EV_WRITE " : " ",
            tv ? "EV_TIMEOUT " : " ",
            ev->ev_callback));

    EVUTIL_ASSERT(!(ev->ev_flags & ~EVLIST_ALL));

    /*
     * 如果tv不为NULL，并且事件不在小根堆中，判断小根堆中分配的内存是否有多余空间，空间不足，倍增法分配空间，分配失败返回-1
     * 如果任何一个步骤错误，不改变任何状态
     */
    if (tv != NULL && !(ev->ev_flags & EVLIST_TIMEOUT)) {
        if (min_heap_reserve_(&base->timeheap, 1 + min_heap_size_(&base->timeheap)) == -1)
            return (-1);  /* ENOMEM == errno */
    }

    /* 如果主线程正在执行一个信号事件的回调函数，我们当前不在主线程中，我们需要等待直到回调函数执行完成 */
    if(base->current_event == ev && (ev->ev_events & EV_SIGNAL) && !EVBASE_IN_THREAD(base)){
        ++base->current_event_waiters;
        EVTHREAD_COND_WAIT(base->current_event_cond, base->th_base_lock);
    }

    //如果是IO事件或者信号事件，并且事件的状态不是已注册或者已就绪
    if ((ev->ev_events & (EV_READ|EV_WRITE|EV_SIGNAL)) && !(ev->ev_flags & (EVLIST_INSERTED|EVLIST_ACTIVE))) {
        if (ev->ev_events & (EV_READ|EV_WRITE))
            res = evmap_io_add(base, ev->ev_fd, ev);
        else if (ev->ev_events & EV_SIGNAL)
            res = evmap_signal_add(base, (int)ev->ev_fd, ev);
        if (res != -1)
            event_queue_insert(base, ev,EVLIST_INSERTED);
        if (res == 1) {
            /* 需要通知主线程 */
            notify = 1;
            res = 0;
        }
    }

    //先前的事件添加成功，改变超时状态
    if (res != -1 && tv != NULL) {
        struct timeval now;
        int common_timeout;

        /* 对于永久超时事件，我们记住超时值，重新添加；如果tv_is_absolute，已经设置 */
        if (ev->ev_closure == EV_CLOSURE_PERSIST && !tv_is_absolute)
            ev->ev_.ev_io.ev_timeout = *tv;

        //如果事件已经插入到小根堆中，并且小根堆的节点数不等于0，需要通知主线程，移除事件ev
        if (ev->ev_flags & EVLIST_TIMEOUT) {
            if (min_heap_elt_is_top_(ev))
                notify = 1;
            event_queue_remove(base, ev, EVLIST_TIMEOUT);
        }

        /* 检查是否由于超时而处于活动状态。 在执行回调之前重新设置超时，将其从活动列表中删除。*/
        if ((ev->ev_flags & EVLIST_ACTIVE) && (ev->ev_res & EV_TIMEOUT)) {
            if (ev->ev_events & EV_SIGNAL) {
                /* 看看我们是否只是在循环中主动执行这个事件*/
                if (ev->ev_.ev_signal.ev_ncalls && ev->ev_.ev_signal.ev_pncalls) {
                    /* Abort loop */
                    *ev->ev_.ev_signal.ev_pncalls = 0;
                }
            }

            event_queue_remove(base, ev, EVLIST_ACTIVE);
        }

        //获取当前时间
        gettime(base, &now);

        common_timeout = is_common_timeout(tv, base);

        if (tv_is_absolute) {
            ev->ev_timeout = *tv;
        }
        else if (common_timeout) {//不是绝对事件，计算超时值
            struct timeval tmp = *tv;
            tmp.tv_usec &= MICROSECONDS_MASK;//低20位采用掩码编码
            evutil_timeradd(&now, &tmp, &ev->ev_timeout);
            ev->ev_timeout.tv_usec |= (tv->tv_usec & ~MICROSECONDS_MASK);
        }
        else {
            evutil_timeradd(&now, tv, &ev->ev_timeout);
        }

        event_debug(("event_add: event %p, timeout in %d seconds %d useconds, call %p",
                ev, (int)tv->tv_sec, (int)tv->tv_usec, ev->ev_callback));
        //插入超时事件
        event_queue_insert(base, ev, EVLIST_TIMEOUT);

        if (common_timeout) {
            struct common_timeout_list *ctl = get_common_timeout_list(base, &ev->ev_timeout);
            if (ev == TAILQ_FIRST(&ctl->events)) {
                common_timeout_schedule(ctl, &now, ev);
            }
        }
        else {
            struct event* top = NULL;
            /*
             * 如果ev插入到timer小根堆中，并且插入到根节点：需要告诉主线程早一点唤醒。
             * 我们仔细检查顶部元素的超时，以处理由于系统暂停引起的时间暂停。
             */
            if (min_heap_elt_is_top_(ev))
                notify = 1;
            else if ((top = min_heap_top_(&base->timeheap)) != NULL && evutil_timercmp(&top->ev_timeout, &now, <))
                notify = 1;
        }
    }

    /* 不在正确的线程，线程通知，唤醒循环 */
    if (res != -1 && notify && EVBASE_NEED_NOTIFY(base))
        evthread_notify_base(base);

    return (res);
}

int event_del(struct event *ev)
{
    int res;

    if (EVUTIL_FAILURE_CHECK(!ev->ev_base)) {
        event_warnx("%s: event has no event_base set.", __func__);
        return -1;
    }

    EVBASE_ACQUIRE_LOCK(ev->ev_base, th_base_lock);

    res = event_del_nolock(ev);

    EVBASE_RELEASE_LOCK(ev->ev_base, th_base_lock);

    return (res);
}

int event_del_nolock(struct event *ev){
    struct event_base *base;
    int res = 0, notify = 0;

    event_debug(("event_del: %p (fd = %d), callback %p", ev, ev->ev_fd, ev->ev_callback));

    /* An event without a base has not been added */
    if (ev->ev_base == NULL)
        return (-1);

    EVENT_BASE_ASSERT_LOCKED(ev->ev_base);

    /*
     * 如果主线程正在执行这个事件ev的回调函数，并且我们不在主线程中，移除这个事件前我们需要等待直到回调执行完成
     * 这样，当该函数返回时，可以安全地释放用户提供的参数。
     */
    base = ev->ev_base;
    if (base->current_event == ev && !EVBASE_IN_THREAD(base)) {
        ++base->current_event_waiters;
        EVTHREAD_COND_WAIT(base->current_event_cond, base->th_base_lock);
    }

    EVUTIL_ASSERT(!(ev->ev_flags & ~EVLIST_ALL));

    /* 看看我们是否只是在循环中主动执行这个事件 */
    if (ev->ev_events & EV_SIGNAL) {
        if (ev->ev_.ev_signal.ev_ncalls && ev->ev_.ev_signal.ev_pncalls) {
            /* Abort loop */
            *ev->ev_.ev_signal.ev_pncalls = 0;
        }
    }

    //已删除的超时事件，我们不需要通知主线程
    if (ev->ev_flags & EVLIST_TIMEOUT) {
        event_queue_remove(base, ev,EVLIST_TIMEOUT);
    }

    if (ev->ev_flags & EVLIST_ACTIVE)
        event_queue_remove(base, ev,EVLIST_ACTIVE);
    else if (ev->ev_flags & EVLIST_INSERTED) {
        event_queue_remove(base, ev,EVLIST_INSERTED);
        if (ev->ev_events & (EV_READ|EV_WRITE))
            res = evmap_io_del(base, ev->ev_fd, ev);
        else
            res = evmap_signal_del(base, (int)ev->ev_fd, ev);
        if (res == 1) {
            /* evmap：我们应该通知主线程 */
            notify = 1;
            res = 0;
        }
    }

    /* 如果我们不在正确的线程, 我们需要唤醒循环 */
    if (res != -1 && notify && EVBASE_NEED_NOTIFY(base))
        evthread_notify_base(base);

    return (res);
}

void event_active(struct event *ev, int res, short ncalls)
{
    if (EVUTIL_FAILURE_CHECK(!ev->ev_base)) {
        event_warnx("%s: event has no event_base set.", __func__);
        return;
    }

    EVBASE_ACQUIRE_LOCK(ev->ev_base, th_base_lock);

    event_active_nolock(ev, res, ncalls);

    EVBASE_RELEASE_LOCK(ev->ev_base, th_base_lock);
}

void event_active_nolock(struct event *ev, int res, short ncalls) {
    struct event_base *base;

    event_debug(("event_active: %p (fd = %d), res %d, callback %p",
            ev, ev->ev_fd, (int) res, ev->ev_callback));

    if (ev->ev_flags & EVLIST_ACTIVE) {
        ev->ev_res |= res;
        return;
    }

    base = ev->ev_base;

    EVENT_BASE_ASSERT_LOCKED(base);

    ev->ev_res = res;

    if (ev->ev_pri < base->event_running_priority)
        base->event_continue = 1;

    if (ev->ev_events & EV_SIGNAL) {
        if (base->current_event == ev && !EVBASE_IN_THREAD(base)) {
            ++base->current_event_waiters;
            EVTHREAD_COND_WAIT(base->current_event_cond, base->th_base_lock);
        }
        ev->ev_.ev_signal.ev_ncalls = ncalls;
        ev->ev_.ev_signal.ev_pncalls  = NULL;
    }

    event_queue_insert(base, ev, EVLIST_ACTIVE);

    if (EVBASE_NEED_NOTIFY(base))
        evthread_notify_base(base);
}

void event_free(struct event *ev)
{
    event_del(ev);
    mm_free(ev);
}

int event_initialized(const struct event *ev)
{
    if (!(ev->ev_flags & EVLIST_INIT))
        return 0;

    return 1;
}

int event_reinit(struct event_base *base)
{
    const struct eventop *evsel;
    int res = 0;
    struct event *ev;
    int was_notifiable = 0;

    EVBASE_ACQUIRE_LOCK(base, th_base_lock);

    evsel = base->evsel;

    /* prevent internal delete */
    if (base->sig.ev_signal_added) {
        //不能调用event_del，应为base还没有被重新初始化
        event_queue_remove(base, &base->sig.ev_signal, EVLIST_INSERTED);
        if (base->sig.ev_signal.ev_flags & EVLIST_ACTIVE)
            event_queue_remove(base, &base->sig.ev_signal, EVLIST_ACTIVE);
        if (base->sig.ev_signal_pair[0] != -1)
            close(base->sig.ev_signal_pair[0]);
        if (base->sig.ev_signal_pair[1] != -1)
            close(base->sig.ev_signal_pair[1]);
        base->sig.ev_signal_added = 0;
    }
    if (base->th_notify_fd[0] != -1) {
        was_notifiable = 1;
        event_queue_remove(base, &base->th_notify, EVLIST_INSERTED);
        if (base->th_notify.ev_flags & EVLIST_ACTIVE)
            event_queue_remove(base, &base->th_notify, EVLIST_ACTIVE);
        base->sig.ev_signal_added = 0;
        close(base->th_notify_fd[0]);
        if (base->th_notify_fd[1] != -1)
            close(base->th_notify_fd[1]);
        base->th_notify_fd[0] = -1;
        base->th_notify_fd[1] = -1;
        event_debug_unassign(&base->th_notify);
    }

    if (base->evsel->dealloc != NULL)
        base->evsel->dealloc(base);
    base->evbase = evsel->init(base);
    if (base->evbase == NULL) {
        event_errx(1, "%s: could not reinitialize event mechanism", __func__);
        res = -1;
        goto done;
    }

    evmap_io_clear(&base->io);
    evmap_signal_clear(&base->sigmap);

    TAILQ_FOREACH(ev, &base->eventqueue, ev_next) {
        if (ev->ev_events & (EV_READ|EV_WRITE)) {
            if (ev == &base->sig.ev_signal) {
                /* 如果遇到ev_signal事件，那只是在eventqueue中，因为添加了一些信号事件，这使得evsig_add重新添加ev_signal。 所以不要双重添加。 */
                continue;
            }
            if (evmap_io_add(base, ev->ev_fd, ev) == -1)
                res = -1;
        } else if (ev->ev_events & EV_SIGNAL) {
            if (evmap_signal_add(base, (int)ev->ev_fd, ev) == -1)
                res = -1;
        }
    }

    if (was_notifiable && res == 0)
        res = evthread_make_base_notifiable(base);

done:
    EVBASE_RELEASE_LOCK(base, th_base_lock);
    return (res);
}

int event_pending(const struct event *ev, short event, struct timeval *tv)
{
    int flags = 0;

    if (EVUTIL_FAILURE_CHECK(ev->ev_base == NULL)) {
        event_warnx("%s: event has no event_base set.", __func__);
        return 0;
    }

    EVBASE_ACQUIRE_LOCK(ev->ev_base, th_base_lock);

    //flags记录用户监听了哪些事件
    if (ev->ev_flags & EVLIST_INSERTED)
        flags |= (ev->ev_events & (EV_READ|EV_WRITE|EV_SIGNAL));
    if (ev->ev_flags & EVLIST_ACTIVE)
        flags |= ev->ev_res;
    if (ev->ev_flags & EVLIST_TIMEOUT)
        flags |= EV_TIMEOUT;

    event &= (EV_TIMEOUT|EV_READ|EV_WRITE|EV_SIGNAL);

    /* 看看我们应该报告超时*/
    if (tv != NULL && (flags & event & EV_TIMEOUT)) {
        struct timeval tmp = ev->ev_timeout;
        tmp.tv_usec &= MICROSECONDS_MASK;
		evutil_timeradd(&ev->ev_base->tv_clock_diff, &tmp, tv);
    }

    EVBASE_RELEASE_LOCK(ev->ev_base, th_base_lock);

    return (flags & event);
}

//插入事件到对应的队列中
static void event_queue_insert(struct event_base *base, struct event *ev, int queue){
    EVENT_BASE_ASSERT_LOCKED(base);

    if(ev->ev_flags & queue){
        /* 就绪事件有可能被插入两次 */
        if (queue & EVLIST_ACTIVE)
            return;

        event_errx(1, "%s: %p(fd = %d) already on queue %x", __func__, ev, ev->ev_fd, queue);
        return;
    }
    //不是内部事件，增加事件数目
    if(~ev->ev_flags & EVLIST_INTERNAL)
        base->event_count++;

    ev->ev_flags |= queue;
    switch(queue){
        case EVLIST_INSERTED:
            TAILQ_INSERT_TAIL(&base->eventqueue, ev, ev_next);
            break;
        case EVLIST_ACTIVE:
            base->event_count_active++;
            TAILQ_INSERT_TAIL(&base->activequeues[ev->ev_pri], ev,ev_active_next);
            break;
        case EVLIST_TIMEOUT: {
            if (is_common_timeout(&ev->ev_timeout, base)) {
                struct common_timeout_list *ctl = get_common_timeout_list(base, &ev->ev_timeout);
                insert_common_timeout_inorder(ctl, ev);
            }
            else
                min_heap_push_(&base->timeheap, ev);
            break;
        }
        default:
            event_errx(1, "%s: unknown queue %x", __func__, queue);
    }
}

//从指定队列中删除事件
static void event_queue_remove(struct event_base *base, struct event *ev, int queue){
    EVENT_BASE_ASSERT_LOCKED(base);

    if (!(ev->ev_flags & queue)) {
        event_errx(1, "%s: %p(fd = %d) not on queue %x", __func__, ev, ev->ev_fd, queue);
        return;
    }

    //不是内部事件，减少事件总数
    if (~ev->ev_flags & EVLIST_INTERNAL)
        base->event_count--;

    ev->ev_flags &= ~queue;
    switch (queue) {
        case EVLIST_INSERTED:
            TAILQ_REMOVE(&base->eventqueue, ev, ev_next);
            break;
        case EVLIST_ACTIVE:
            base->event_count_active--;
            TAILQ_REMOVE(&base->activequeues[ev->ev_pri], ev, ev_active_next);
            break;
        case EVLIST_TIMEOUT:
            if (is_common_timeout(&ev->ev_timeout, base)) {
                struct common_timeout_list *ctl = get_common_timeout_list(base, &ev->ev_timeout);
                TAILQ_REMOVE(&ctl->events, ev, ev_timeout_pos.ev_next_with_common_timeout);
            }
            else {
                min_heap_erase_(&base->timeheap, ev);
            }
            break;
        default:
            event_errx(1, "%s: unknown queue %x", __func__, queue);
    }
}

size_t event_get_struct_event_size(void)
{
    return sizeof(struct event);
}

int event_get_fd(const struct event *ev)
{
    return ev->ev_fd;
}

struct event_base *event_get_base(const struct event *ev)
{
    return ev->ev_base;
}

short event_get_events(const struct event *ev)
{
    return ev->ev_events;
}

event_callback_fn event_get_callback(const struct event *ev)
{
    return ev->ev_callback;
}

void *event_get_callback_arg(const struct event *ev)
{
    return ev->ev_arg;
}

int event_get_priority(const struct event *ev)
{
    return ev->ev_pri;
}

void event_get_assignment(const struct event *event, struct event_base **base_out, int *fd_out, short *events_out, event_callback_fn *callback_out, void **arg_out)
{
    if (base_out)
        *base_out = event->ev_base;
    if (fd_out)
        *fd_out = event->ev_fd;
    if (events_out)
        *events_out = event->ev_events;
    if (callback_out)
        *callback_out = event->ev_callback;
    if (arg_out)
        *arg_out = event->ev_arg;
}

void event_debug_unassign(struct event *ev)
{
    ev->ev_flags &= ~EVLIST_INIT;
}

int event_global_setup_locks(const int enable_locks)
{
    if (evsig_global_setup_locks(enable_locks) < 0)
        return -1;
    return 0;
}

//从其它线程唤醒event_base
static int evthread_notify_base_default(struct event_base *base)
{
    char buf[1] = {0};
    int r;
    buf[0] = (char) 0;
    r = write(base->th_notify_fd[1], buf, 1);

    return (r < 0 && ! EVUTIL_ERR_IS_EAGAIN(errno)) ? -1 : 0;
}

static void evthread_notify_drain_default(int fd, short what, void *arg)
{
    unsigned char buf[1024];
    struct event_base *base = (struct event_base *)arg;
    while (read(fd, (char*)buf, sizeof(buf)) > 0)
        ;

    EVBASE_ACQUIRE_LOCK(base, th_base_lock);
    base->is_notify_pending = 0;
    EVBASE_RELEASE_LOCK(base, th_base_lock);
}

static int evthread_make_base_notifiable_nolock_(struct event_base *base)
{
    void (*cb)(int, short, void *);
    int (*notify)(struct event_base *);

    //已经设置了通知函数
    if (base->th_notify_fn != NULL) {
        return 0;
    }

    if (evutil_make_internal_pipe(base->th_notify_fd) == 0) {
        notify = evthread_notify_base_default;
        cb = evthread_notify_drain_default;
    }
    else {
        return -1;
    }

    base->th_notify_fn = notify;

    /* prepare an event that we can use for wakeup */
    event_assign(&base->th_notify, base, base->th_notify_fd[0], EV_READ|EV_PERSIST, cb, base);

    //我们需要标记为内部事件
    base->th_notify.ev_flags |= EVLIST_INTERNAL;
    event_priority_set(&base->th_notify, 0);

    return event_add_nolock(&base->th_notify, NULL, 0);
}

int evthread_make_base_notifiable(struct event_base *base)
{
    int r;
    if (!base)
        return -1;

    EVBASE_ACQUIRE_LOCK(base, th_base_lock);
    r = evthread_make_base_notifiable_nolock_(base);
    EVBASE_RELEASE_LOCK(base, th_base_lock);
    return r;
}
static void event_free_evsig_globals(void)
{
    evsig_free_globals();
}

static void event_free_globals(void)
{
    event_free_evsig_globals();
}

void libevent_global_shutdown(void)
{
    event_free_globals();
}