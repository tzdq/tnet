#include <sys/types.h>
#include <sys/time.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include "event2/util.h"
#include "event2/buffer.h"
#include "event2/bufferevent.h"
#include "event2/event.h"
#include "evlog.h"
#include "evmemory.h"
#include "bufferevent_internal.h"
#include "evbuffer.h"
#include "evutil.h"

static void bufferevent_cancel_all(struct bufferevent *bev);


static void bufferevent_run_deferred_callbacks_unlocked(struct deferred_cb *_, void *arg)
{
    struct bufferevent_private *bufev_private = (struct bufferevent_private *)arg;
    struct bufferevent *bufev = &bufev_private->bev;

    BEV_LOCK(bufev);
#define UNLOCKED(stmt) do { BEV_UNLOCK(bufev); stmt; BEV_LOCK(bufev); } while(0)

    if ((bufev_private->eventcb_pending & BEV_EVENT_CONNECTED) && bufev->errorcb) {
        //"connected"发生在任何读写之前，先发送它
        bufferevent_event_cb errorcb = bufev->errorcb;
        void *cbarg = bufev->cbarg;
        bufev_private->eventcb_pending &= ~BEV_EVENT_CONNECTED;
        UNLOCKED(errorcb(bufev, BEV_EVENT_CONNECTED, cbarg));
    }
    if (bufev_private->readcb_pending && bufev->readcb) {
        bufferevent_data_cb readcb = bufev->readcb;
        void *cbarg = bufev->cbarg;
        bufev_private->readcb_pending = 0;
        UNLOCKED(readcb(bufev, cbarg));
    }
    if (bufev_private->writecb_pending && bufev->writecb) {
        bufferevent_data_cb writecb = bufev->writecb;
        void *cbarg = bufev->cbarg;
        bufev_private->writecb_pending = 0;
        UNLOCKED(writecb(bufev, cbarg));
    }
    if (bufev_private->eventcb_pending && bufev->errorcb) {
        bufferevent_event_cb errorcb = bufev->errorcb;
        void *cbarg = bufev->cbarg;
        short what = bufev_private->eventcb_pending;
        int err = bufev_private->errno_pending;
        bufev_private->eventcb_pending = 0;
        bufev_private->errno_pending = 0;
        errno = err;
        UNLOCKED(errorcb(bufev,what,cbarg));
    }
    bufferevent_decref_and_unlock(bufev);
#undef UNLOCKED
}

static void bufferevent_run_deferred_callbacks_locked(struct deferred_cb *_, void *arg)
{
    struct bufferevent_private *bufev_private = (struct bufferevent_private *)arg;
    struct bufferevent *bufev = &bufev_private->bev;

    BEV_LOCK(bufev);
    if ((bufev_private->eventcb_pending & BEV_EVENT_CONNECTED) && bufev->errorcb) {
        //"connected"发生在任何读写之前，先发送它
        bufev_private->eventcb_pending &= ~BEV_EVENT_CONNECTED;
        bufev->errorcb(bufev, BEV_EVENT_CONNECTED, bufev->cbarg);
    }
    if (bufev_private->readcb_pending && bufev->readcb) {
        bufev_private->readcb_pending = 0;
        bufev->readcb(bufev, bufev->cbarg);
    }
    if (bufev_private->writecb_pending && bufev->writecb) {
        bufev_private->writecb_pending = 0;
        bufev->writecb(bufev, bufev->cbarg);
    }
    if (bufev_private->eventcb_pending && bufev->errorcb) {
        short what = bufev_private->eventcb_pending;
        int err = bufev_private->errno_pending;
        bufev_private->eventcb_pending = 0;
        bufev_private->errno_pending = 0;
        errno = err;
        bufev->errorcb(bufev, what, bufev->cbarg);
    }
    bufferevent_decref_and_unlock(bufev);
}

/* 输入缓冲区的高水位回调函数 */
static void bufferevent_inbuf_cb(struct evbuffer *buf, const struct evbuffer_cb_info *cbinfo, void *arg)
{
    struct bufferevent *bufev = (struct bufferevent *)arg;
    size_t size;

    size = evbuffer_get_length(buf);

    if (size >= bufev->wm_read.high)
        bufferevent_suspend_read(bufev,BEV_SUSPEND_WM);
    else
        bufferevent_unsuspend_read(bufev,BEV_SUSPEND_WM);
}

int bufferevent_decref_and_unlock(struct bufferevent *bufev)
{
    struct bufferevent_private *bufev_private = EVUTIL_UPCAST(bufev, struct bufferevent_private, bev);
    struct bufferevent *underlying;

    EVUTIL_ASSERT(bufev_private->refcnt > 0);

    //如果--后引用计数还不为0，解锁，直接返回
    if (--bufev_private->refcnt) {
        BEV_UNLOCK(bufev);
        return 0;
    }

    underlying = bufferevent_get_underlying(bufev);

    /* 引用计数为0，释放内存 */
    if (bufev->be_ops->destruct)
        bufev->be_ops->destruct(bufev);

    /* 释放缓冲区*/
    evbuffer_free(bufev->input);
    evbuffer_free(bufev->output);

    event_debug_unassign(&bufev->ev_read);
    event_debug_unassign(&bufev->ev_write);

    //如果存在锁，释放锁
    BEV_UNLOCK(bufev);
    if (bufev_private->own_lock)
        EVTHREAD_FREE_LOCK(bufev_private->lock, EVTHREAD_LOCKTYPE_RECURSIVE);

    /* 释放内存. */
    mm_free(((char*)bufev) - bufev->be_ops->mem_offset);

    /* 释放底层引用计数*/
    if (underlying)
        bufferevent_decref(underlying);

    return 1;
}

void bufferevent_incref_and_lock(struct bufferevent *bufev)
{
    struct bufferevent_private *bufev_private = BEV_UPCAST(bufev);
    BEV_LOCK(bufev);
    ++bufev_private->refcnt;
}

int bufferevent_decref(struct bufferevent *bufev)
{
    BEV_LOCK(bufev);
    return bufferevent_decref_and_unlock(bufev);
}

void bufferevent_incref(struct bufferevent *bufev)
{
    struct bufferevent_private *bufev_private = EVUTIL_UPCAST(bufev, struct bufferevent_private, bev);

    BEV_LOCK(bufev);
    ++bufev_private->refcnt;
    BEV_UNLOCK(bufev);
}


int bufferevent_init_common(struct bufferevent_private *bufev_private, struct event_base *base,
                        const struct bufferevent_ops *ops, enum bufferevent_options options)
{
    struct bufferevent *bufev = &bufev_private->bev;

    //分配输入缓冲区
    if (!bufev->input) {
        if ((bufev->input = evbuffer_new()) == NULL)
            return -1;
    }

    //分配输出缓冲区
    if (!bufev->output) {
        if ((bufev->output = evbuffer_new()) == NULL) {
            evbuffer_free(bufev->input);
            return -1;
        }
    }

    bufev_private->refcnt = 1;
    bufev->ev_base = base;

    /* 默认情况下,读和写event都是不支持超时的 */
    evutil_timerclear(&bufev->timeout_read);
    evutil_timerclear(&bufev->timeout_write);

    bufev->be_ops = ops;

    //可写是默认支持的
    bufev->enabled = EV_WRITE;

    if (options & BEV_OPT_THREADSAFE) {
        if (bufferevent_enable_locking(bufev, NULL) < 0) {
            /* cleanup */
            evbuffer_free(bufev->input);
            evbuffer_free(bufev->output);
            bufev->input = NULL;
            bufev->output = NULL;
            return -1;
        }
    }

    //延迟回调相关
    if ((options & (BEV_OPT_DEFER_CALLBACKS|BEV_OPT_UNLOCK_CALLBACKS)) == BEV_OPT_UNLOCK_CALLBACKS) {
        event_warnx("UNLOCK_CALLBACKS requires DEFER_CALLBACKS");
        return -1;
    }
    if (options & BEV_OPT_DEFER_CALLBACKS) {
        if (options & BEV_OPT_UNLOCK_CALLBACKS)
            event_deferred_cb_init(&bufev_private->deferred,bufferevent_run_deferred_callbacks_unlocked,
                                   bufev_private);
        else
            event_deferred_cb_init(&bufev_private->deferred, bufferevent_run_deferred_callbacks_locked,
                                   bufev_private);
    }

    bufev_private->options = options;

    evbuffer_set_parent(bufev->input, bufev);
    evbuffer_set_parent(bufev->output, bufev);

    return 0;
}

void bufferevent_setcb(struct bufferevent *bufev, bufferevent_data_cb readcb, bufferevent_data_cb writecb,
                  bufferevent_event_cb eventcb, void *cbarg)
{
    BEV_LOCK(bufev);

    bufev->readcb = readcb;
    bufev->writecb = writecb;
    bufev->errorcb = eventcb;

    bufev->cbarg = cbarg;
    BEV_UNLOCK(bufev);
}

int bufferevent_set_timeouts(struct bufferevent *bufev, const struct timeval *tv_read,
                             const struct timeval *tv_write)
{
    int r = 0;
    BEV_LOCK(bufev);
    if (tv_read) {
        bufev->timeout_read = *tv_read;
    }
    else {
        evutil_timerclear(&bufev->timeout_read);
    }
    if (tv_write) {
        bufev->timeout_write = *tv_write;
    }
    else {
        evutil_timerclear(&bufev->timeout_write);
    }

    if (bufev->be_ops->adj_timeouts)
        r = bufev->be_ops->adj_timeouts(bufev);
    BEV_UNLOCK(bufev);

    return r;
}

void bufferevent_setwatermark(struct bufferevent *bufev, short events, size_t lowmark, size_t highmark)
{
    struct bufferevent_private *bufev_private = EVUTIL_UPCAST(bufev, struct bufferevent_private, bev);

    BEV_LOCK(bufev);
    if (events & EV_WRITE) {
        bufev->wm_write.low = lowmark;
        bufev->wm_write.high = highmark;
    }

    if (events & EV_READ) {
        bufev->wm_read.low = lowmark;
        bufev->wm_read.high = highmark;

        if (highmark) {//设置高水位
            if (bufev_private->read_watermarks_cb == NULL) {//还没设置高水位的回调函数
                bufev_private->read_watermarks_cb = evbuffer_add_cb(bufev->input, bufferevent_inbuf_cb, bufev);
            }
            evbuffer_cb_set_flags(bufev->input, bufev_private->read_watermarks_cb,
                                  EVBUFFER_CB_ENABLED|EVBUFFER_CB_NODEFER);

            //设置(修改)高水位时，evbuffer的数据量已经超过了水位值，需要挂起读，反之，不需要
            if (evbuffer_get_length(bufev->input) >= highmark)
                bufferevent_suspend_read(bufev,BEV_SUSPEND_WM);
            else if (evbuffer_get_length(bufev->input) < highmark)
                bufferevent_unsuspend_read(bufev,BEV_SUSPEND_WM);
        }
        else {
            //高水位值等于0，那么就要取消挂起读事件，取消挂起操作是幂等的
            if (bufev_private->read_watermarks_cb)
                evbuffer_cb_clear_flags(bufev->input, bufev_private->read_watermarks_cb, EVBUFFER_CB_ENABLED);
            bufferevent_unsuspend_read(bufev,BEV_SUSPEND_WM);
        }
    }
    BEV_UNLOCK(bufev);
}


int bufferevent_enable_locking(struct bufferevent *bufev, void *lock)
{
    struct bufferevent *underlying;

    if (BEV_UPCAST(bufev)->lock)
        return -1;
    underlying = bufferevent_get_underlying(bufev);

    if (!lock && underlying && BEV_UPCAST(underlying)->lock) {
        lock = BEV_UPCAST(underlying)->lock;
        BEV_UPCAST(bufev)->lock = lock;
        BEV_UPCAST(bufev)->own_lock = 0;
    } else if (!lock) {
        EVTHREAD_ALLOC_LOCK(lock, EVTHREAD_LOCKTYPE_RECURSIVE);
        if (!lock)
            return -1;
        BEV_UPCAST(bufev)->lock = lock;
        BEV_UPCAST(bufev)->own_lock = 1;
    } else {
        BEV_UPCAST(bufev)->lock = lock;
        BEV_UPCAST(bufev)->own_lock = 0;
    }
    evbuffer_enable_locking(bufev->input, lock);
    evbuffer_enable_locking(bufev->output, lock);

    if (underlying && !BEV_UPCAST(underlying)->lock)
        bufferevent_enable_locking(underlying, lock);

    return 0;
}

struct bufferevent *bufferevent_get_underlying(struct bufferevent *bev)
{
    union bufferevent_ctrl_data d;
    int res = -1;
    d.ptr = NULL;
    BEV_LOCK(bev);
    if (bev->be_ops->ctrl)
        res = bev->be_ops->ctrl(bev, BEV_CTRL_GET_UNDERLYING, &d);
    BEV_UNLOCK(bev);
    return (res < 0) ? NULL : (struct bufferevent *)d.ptr;
}

int bufferevent_add_event(struct event *ev, const struct timeval *tv)
{
    if (tv->tv_sec == 0 && tv->tv_usec == 0)
        return event_add(ev, NULL);
    else
        return event_add(ev, tv);
}

int bufferevent_enable(struct bufferevent *bufev, short event)
{
    struct bufferevent_private *bufev_private = EVUTIL_UPCAST(bufev, struct bufferevent_private, bev);

    short impl_events = event;
    int r = 0;

    bufferevent_incref_and_lock(bufev);

    if (bufev_private->read_suspended)//挂起了读，此时不能监听读事件
        impl_events &= ~EV_READ;
    if (bufev_private->write_suspended)//挂起了写，此时不能监听写事件
    impl_events &= ~EV_WRITE;

    bufev->enabled |= event;

    //调用对应类型的enbale函数。因为不同类型的bufferevent有不同的enable函数
    if (impl_events && bufev->be_ops->enable(bufev, impl_events) < 0)
        r = -1;

    bufferevent_decref_and_unlock(bufev);
    return r;
}

int bufferevent_disable(struct bufferevent *bufev, short event)
{
    int r = 0;

    BEV_LOCK(bufev);
    bufev->enabled &= ~event;

    if (bufev->be_ops->disable(bufev, event) < 0)
        r = -1;

    BEV_UNLOCK(bufev);
    return r;
}

short bufferevent_get_enabled(struct bufferevent *bufev)
{
    short r;
    BEV_LOCK(bufev);
    r = bufev->enabled;
    BEV_UNLOCK(bufev);
    return r;
}

void bufferevent_lock(struct bufferevent *bev)
{
    bufferevent_incref_and_lock(bev);
}

void bufferevent_unlock(struct bufferevent *bev)
{
    bufferevent_decref_and_unlock(bev);
}

void bufferevent_suspend_read(struct bufferevent *bufev, uint16_t what)
{
    struct bufferevent_private *bufev_private = EVUTIL_UPCAST(bufev, struct bufferevent_private, bev);
    BEV_LOCK(bufev);
    if (!bufev_private->read_suspended)
        bufev->be_ops->disable(bufev, EV_READ);
    bufev_private->read_suspended |= what;
    BEV_UNLOCK(bufev);
}

void bufferevent_unsuspend_read(struct bufferevent *bufev, uint16_t what)
{
    struct bufferevent_private *bufev_private = EVUTIL_UPCAST(bufev, struct bufferevent_private, bev);
    BEV_LOCK(bufev);
    bufev_private->read_suspended &= ~what;
    if (!bufev_private->read_suspended && (bufev->enabled & EV_READ))
        bufev->be_ops->enable(bufev, EV_READ);
    BEV_UNLOCK(bufev);
}

void bufferevent_suspend_write(struct bufferevent *bufev, uint16_t what)
{
    struct bufferevent_private *bufev_private = EVUTIL_UPCAST(bufev, struct bufferevent_private, bev);
    BEV_LOCK(bufev);
    if (!bufev_private->write_suspended)
        bufev->be_ops->disable(bufev, EV_WRITE);
    bufev_private->write_suspended |= what;
    BEV_UNLOCK(bufev);
}

void bufferevent_unsuspend_write(struct bufferevent *bufev, uint16_t what)
{
    struct bufferevent_private *bufev_private = EVUTIL_UPCAST(bufev, struct bufferevent_private, bev);
    BEV_LOCK(bufev);
    bufev_private->write_suspended &= ~what;
    if (!bufev_private->write_suspended && (bufev->enabled & EV_WRITE))
        bufev->be_ops->enable(bufev, EV_WRITE);
    BEV_UNLOCK(bufev);
}

#define SCHEDULE_DEFERRED(bevp)						\
	do {								\
		bufferevent_incref(&(bevp)->bev);			\
		event_deferred_cb_schedule(event_base_get_deferred_cb_queue((bevp)->bev.ev_base),&(bevp)->deferred);\
	} while (0)


void bufferevent_run_readcb(struct bufferevent *bufev)
{
    /* Requires that we hold the lock and a reference */
    struct bufferevent_private *p = EVUTIL_UPCAST(bufev, struct bufferevent_private, bev);
    if (bufev->readcb == NULL)
        return;
    if (p->options & BEV_OPT_DEFER_CALLBACKS) {
        p->readcb_pending = 1;
        if (!p->deferred.queued)
            SCHEDULE_DEFERRED(p);
    } else {
        bufev->readcb(bufev, bufev->cbarg);
    }
}

void bufferevent_run_writecb(struct bufferevent *bufev)
{
    struct bufferevent_private *p = EVUTIL_UPCAST(bufev, struct bufferevent_private, bev);
    if (bufev->writecb == NULL)
        return;
    if (p->options & BEV_OPT_DEFER_CALLBACKS) {
        p->writecb_pending = 1;
        if (!p->deferred.queued)
            SCHEDULE_DEFERRED(p);
    } else {
        bufev->writecb(bufev, bufev->cbarg);
    }
}

void bufferevent_run_eventcb(struct bufferevent *bufev, short what)
{
    struct bufferevent_private *p = EVUTIL_UPCAST(bufev, struct bufferevent_private, bev);
    if (bufev->errorcb == NULL)
        return;
    if (p->options & BEV_OPT_DEFER_CALLBACKS) {
        p->eventcb_pending |= what;
        p->errno_pending = errno;
        if (!p->deferred.queued)
            SCHEDULE_DEFERRED(p);
    } else {
        bufev->errorcb(bufev, what, bufev->cbarg);
    }
}

int bufferevent_write(struct bufferevent *bufev, const void *data, size_t size)
{
    if (evbuffer_add(bufev->output, data, size) == -1)
        return (-1);

    return 0;
}

int bufferevent_write_buffer(struct bufferevent *bufev, struct evbuffer *buf)
{
    if (evbuffer_add_buffer(bufev->output, buf) == -1)
        return (-1);

    return 0;
}

size_t bufferevent_read(struct bufferevent *bufev, void *data, size_t size)
{
    return (evbuffer_remove(bufev->input, data, size));
}

int bufferevent_read_buffer(struct bufferevent *bufev, struct evbuffer *buf)
{
    return (evbuffer_add_buffer(buf, bufev->input));
}

struct evbuffer *bufferevent_get_input(struct bufferevent *bufev)
{
    return bufev->input;
}

struct evbuffer *bufferevent_get_output(struct bufferevent *bufev)
{
    return bufev->output;
}

int bufferevent_setfd(struct bufferevent *bev, int fd)
{
    union bufferevent_ctrl_data d;
    int res = -1;
    d.fd = fd;
    BEV_LOCK(bev);
    if (bev->be_ops->ctrl)
        res = bev->be_ops->ctrl(bev, BEV_CTRL_SET_FD, &d);
    BEV_UNLOCK(bev);
    return res;
}

int bufferevent_getfd(struct bufferevent *bev)
{
    union bufferevent_ctrl_data d;
    int res = -1;
    d.fd = -1;
    BEV_LOCK(bev);
    if (bev->be_ops->ctrl)
        res = bev->be_ops->ctrl(bev, BEV_CTRL_GET_FD, &d);
    BEV_UNLOCK(bev);
    return (res<0) ? -1 : d.fd;
}

struct event_base *bufferevent_get_base(struct bufferevent *bufev)
{
    return bufev->ev_base;
}

static void bufferevent_cancel_all(struct bufferevent *bev)
{
    union bufferevent_ctrl_data d;
    memset(&d, 0, sizeof(d));
    BEV_LOCK(bev);
    if (bev->be_ops->ctrl)
        bev->be_ops->ctrl(bev, BEV_CTRL_CANCEL_ALL, &d);
    BEV_UNLOCK(bev);
}


void bufferevent_free(struct bufferevent *bufev)
{
    BEV_LOCK(bufev);
    bufferevent_setcb(bufev, NULL, NULL, NULL, NULL);
    bufferevent_cancel_all(bufev);
    bufferevent_decref_and_unlock(bufev);
}

int bufferevent_flush(struct bufferevent *bufev, short iotype, enum bufferevent_flush_mode mode)
{
    int r = -1;
    BEV_LOCK(bufev);
    if (bufev->be_ops->flush)
        r = bufev->be_ops->flush(bufev, iotype, mode);
    BEV_UNLOCK(bufev);
    return r;
}
