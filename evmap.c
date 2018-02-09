#include <sys/types.h>
#include <sys/time.h>
#include "sys/queue.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include "event_internal.h"
#include "evmap.h"
#include "evmemory.h"
#include "event2/event_struct.h"

//evmap_io列表的入口：给定fd上所有想要read或者write的事件，以及事件的数目
struct evmap_io {
    struct event_list events;
    uint16_t nread;
    uint16_t nwrite;
};

//evmap_signal列表的入口：信号触发时所有事件都想知道
struct evmap_signal {
    struct event_list events;
};

//从map中获取第slot个元素的数据并保存在x中，元素的类型为type，如果不存在第slot个元素的入口，返回NULL，无边界检查，
#define GET_SIGNAL_SLOT(x, map, slot, type)	(x) = (struct type *)((map)->entries[slot])

//作用同GET_SIGNAL_SLOT，如果不存在第slot个元素的入口，通过ctor构造初始化并返回
#define GET_SIGNAL_SLOT_AND_CTOR(x, map, slot, type, ctor, fdinfo_len)	\
	do {								\
		if ((map)->entries[slot] == NULL) {			\
			(map)->entries[slot] =				\
			    mm_calloc(1,sizeof(struct type)+fdinfo_len); \
			if (EVUTIL_UNLIKELY((map)->entries[slot] == NULL)) \
				return (-1);				\
			(ctor)((struct type *)(map)->entries[slot]);	\
		}							\
		(x) = (struct type *)((map)->entries[slot]);		\
	} while (0)


#define GET_IO_SLOT(x,map,slot,type) GET_SIGNAL_SLOT(x,map,slot,type)

#define GET_IO_SLOT_AND_CTOR(x,map,slot,type,ctor,fdinfo_len)	\
	GET_SIGNAL_SLOT_AND_CTOR(x,map,slot,type,ctor,fdinfo_len)

/*  扩展map的大下，直到它足够存储槽slot所在的数据，倍增法，msize表示结构体中entry的内存大小*/
static int evmap_make_space(struct event_signal_map *map, int slot, int msize)
{
    if (map->nentries <= slot) {
        int nentries = map->nentries ? map->nentries : 32;
        void **tmp;

        while (nentries <= slot)
            nentries <<= 1;

        tmp = (void **)mm_realloc(map->entries, nentries * msize);
        if (tmp == NULL)
            return (-1);

        memset(&tmp[map->nentries], 0, (nentries - map->nentries) * msize);

        map->nentries = nentries;
        map->entries = tmp;
    }

    return (0);
}

void evmap_io_initmap(struct event_io_map* ctx)
{
    evmap_signal_initmap(ctx);
}

void evmap_io_clear(struct event_io_map* ctx)
{
    evmap_signal_clear(ctx);
}

/** 结构体evmap_io初始化 */
static void evmap_io_init(struct evmap_io *entry)
{
    TAILQ_INIT(&entry->events);
    entry->nread = 0;
    entry->nwrite = 0;
}

//有错误发生时返回-1，如果事件后端没有发生任何改变，成功返回0，有处理并且成功返回1
int evmap_io_add(struct event_base *base, int fd, struct event *ev)
{
    const struct eventop *evsel = base->evsel;
    struct event_io_map *io = &base->io;
    struct evmap_io *ctx = NULL;
    int nread, nwrite, retval = 0;
    short res = 0, old = 0;
    struct event *old_ev;

    EVUTIL_ASSERT(fd == ev->ev_fd);

    if (fd < 0)
        return 0;

    if (fd >= io->nentries) {
        if (evmap_make_space(io, fd, sizeof(struct evmap_io *)) == -1)
            return (-1);
    }

    GET_IO_SLOT_AND_CTOR(ctx, io, fd, evmap_io, evmap_io_init, evsel->fdinfo_len);

    nread = ctx->nread;
    nwrite = ctx->nwrite;

    if (nread)
        old |= EV_READ;
    if (nwrite)
        old |= EV_WRITE;

    //读写事件，并且没有添加到IO复用中
    if (ev->ev_events & EV_READ) {
        if (++nread == 1)
            res |= EV_READ;
    }
    if (ev->ev_events & EV_WRITE) {
        if (++nwrite == 1)
            res |= EV_WRITE;
    }

    if (EVUTIL_UNLIKELY(nread > 0xffff || nwrite > 0xffff)) {
        event_warnx("Too many events reading or writing on fd %d", (int)fd);
        return -1;
    }
    if (EVENT_DEBUG_MODE_IS_ON() &&
        (old_ev = TAILQ_FIRST(&ctx->events)) &&
        (old_ev->ev_events & EV_ET) != (ev->ev_events & EV_ET)) {
        event_warnx("Tried to mix edge-triggered and non-edge-triggered events on fd %d", (int)fd);
        return -1;
    }

    //添加到IO复用
    if (res) {
        void *extra = ((char*)ctx) + sizeof(struct evmap_io);
        //不能混用ET和LT
        if (evsel->add(base, ev->ev_fd, old, (ev->ev_events & EV_ET) | res, extra) == -1)
            return (-1);
        retval = 1;
    }

    ctx->nread = (uint16_t) nread;
    ctx->nwrite = (uint16_t) nwrite;
    TAILQ_INSERT_TAIL(&ctx->events, ev, EV_IO_NEXT);

    return (retval);
}

//有错误发生时返回-1，如果事件后端没有发生任何改变，成功返回0，有处理并且成功返回1
int evmap_io_del(struct event_base *base, int fd, struct event *ev)
{
    const struct eventop *evsel = base->evsel;
    struct event_io_map *io = &base->io;
    struct evmap_io *ctx;
    int nread, nwrite, retval = 0;
    short res = 0, old = 0;

    if (fd < 0)
        return 0;

    EVUTIL_ASSERT(fd == ev->ev_fd);

    if (fd >= io->nentries)
        return (-1);

    GET_IO_SLOT(ctx, io, fd, evmap_io);

    nread = ctx->nread;
    nwrite = ctx->nwrite;

    if (nread)
        old |= EV_READ;
    if (nwrite)
        old |= EV_WRITE;

    if (ev->ev_events & EV_READ) {
        if (--nread == 0)
            res |= EV_READ;
        EVUTIL_ASSERT(nread >= 0);
    }
    if (ev->ev_events & EV_WRITE) {
        if (--nwrite == 0)
            res |= EV_WRITE;
        EVUTIL_ASSERT(nwrite >= 0);
    }

    if (res) {
        void *extra = ((char*)ctx) + sizeof(struct evmap_io);
        if (evsel->del(base, ev->ev_fd, old, res, extra) == -1)
            return -1;
        retval = 1;
    }

    ctx->nread = nread;
    ctx->nwrite = nwrite;
    TAILQ_REMOVE(&ctx->events,ev,EV_IO_NEXT);

    return (retval);
}

void evmap_io_active(struct event_base *base, int fd, short events)
{
    struct event_io_map *io = &base->io;
    struct evmap_io *ctx;
    struct event *ev;

    if (fd < 0 || fd >= io->nentries)
        return;

    GET_IO_SLOT(ctx, io, fd, evmap_io);

    if (NULL == ctx)
        return;
    TAILQ_FOREACH(ev, &ctx->events, EV_IO_NEXT) {
        if (ev->ev_events & events)
            event_active_nolock(ev, ev->ev_events & events, 1);
    }
}

void *evmap_io_get_fdinfo(struct event_io_map *map, int fd)
{
    struct evmap_io *ctx;
    GET_IO_SLOT(ctx, map, fd, evmap_io);
    if (ctx)
        return ((char*)ctx) + sizeof(struct evmap_io);
    else
        return NULL;
}

void evmap_signal_initmap(struct event_signal_map *ctx)
{
    ctx->nentries = 0;
    ctx->entries = NULL;
}

void evmap_signal_clear(struct event_signal_map *ctx)
{
    if (ctx->entries != NULL) {
        for (int i = 0; i < ctx->nentries; ++i) {
            if (ctx->entries[i] != NULL)
                mm_free(ctx->entries[i]);
        }
        mm_free(ctx->entries);
        ctx->entries = NULL;
    }
    ctx->nentries = 0;
}

/** 结构体evmap_signal初始化 */
static void evmap_signal_init(struct evmap_signal *entry)
{
    TAILQ_INIT(&entry->events);
}

int evmap_signal_add(struct event_base *base, int sig, struct event *ev)
{
    const struct eventop *evsel = base->evsigsel;
    struct event_signal_map *map = &base->sigmap;
    struct evmap_signal *ctx = NULL;

    if (sig >= map->nentries) {
        if (evmap_make_space(map, sig, sizeof(struct evmap_signal *)) == -1)
            return (-1);
    }
    GET_SIGNAL_SLOT_AND_CTOR(ctx, map, sig, evmap_signal, evmap_signal_init, base->evsigsel->fdinfo_len);

    if (TAILQ_EMPTY(&ctx->events)) {
        if (evsel->add(base, ev->ev_fd, 0, EV_SIGNAL, NULL) == -1)
            return (-1);
    }

    TAILQ_INSERT_TAIL(&ctx->events, ev, EV_SIGNAL_NEXT);

    return (1);
}

int evmap_signal_del(struct event_base *base, int sig, struct event *ev)
{
    const struct eventop *evsel = base->evsigsel;
    struct event_signal_map *map = &base->sigmap;
    struct evmap_signal *ctx;

    if (sig >= map->nentries)
        return (-1);

    GET_SIGNAL_SLOT(ctx, map, sig, evmap_signal);

    if (TAILQ_FIRST(&ctx->events) == TAILQ_LAST(&ctx->events, event_list)) {
        if (evsel->del(base, ev->ev_fd, 0, EV_SIGNAL, NULL) == -1)
            return (-1);
    }

    TAILQ_REMOVE(&ctx->events, ev, EV_SIGNAL_NEXT);

    return (1);
}

void evmap_signal_active(struct event_base *base, int sig, int ncalls)
{
    struct event_signal_map *map = &base->sigmap;
    struct evmap_signal *ctx;
    struct event *ev;

    if (sig < 0 || sig >= map->nentries)
        return;
    GET_SIGNAL_SLOT(ctx, map, sig, evmap_signal);

    if (!ctx)
        return;
    TAILQ_FOREACH(ev,&ctx->events,EV_SIGNAL_NEXT)
        event_active_nolock(ev, EV_SIGNAL, ncalls);
}