#include <sys/types.h>
#include <sys/time.h>
#include "sys/queue.h"
#include <poll.h>
#include <signal.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include "event_internal.h"
#include "evsignal.h"
#include "evlog.h"
#include "evmap.h"
#include "event/thread.h"
#include "evthread.h"

struct pollidx {
	int idxplus1;
};

struct pollop {
	int event_count;	/* 当前文件描述符最大可用值，也可以说是event_set的大小，倍增法增长 */
	int nfds;			/* 添加到pollfd中的最大文件描述符 */
	int realloc_copy;	/* 是否需要追加event_set_copy的标志，需要时设置为1 */
	struct pollfd *event_set;
	struct pollfd *event_set_copy;
};

static void *poll_init(struct event_base *);
static int poll_add(struct event_base *, int, short old, short events, void *idx);
static int poll_del(struct event_base *, int, short old, short events, void *idx);
static int poll_dispatch(struct event_base *, struct timeval *);
static void poll_dealloc(struct event_base *);

const struct eventop pollops = {
	"poll",
	poll_init,
	poll_add,
	poll_del,
	poll_dispatch,
	poll_dealloc,
	0, /* 不需要重新初始化 */
	EV_FEATURE_FDS,
	sizeof(struct pollidx),
};

static void *poll_init(struct event_base *base)
{
	struct pollop *pollop;

	if (!(pollop = mm_calloc(1, sizeof(struct pollop))))
		return (NULL);

	evsig_init(base);

	return (pollop);
}


static int poll_dispatch(struct event_base *base, struct timeval *tv)
{
	int res, i, nfds;
	long msec = -1;
	struct pollop *pop = base->evbase;
	struct pollfd *event_set;

	nfds = pop->nfds;

	if (base->th_base_lock) {
		/* If we're using this backend in a multithreaded setting,
		 * then we need to work on a copy of event_set, so that we can
		 * let other threads modify the main event_set while we're
		 * polling. If we're not multithreaded, then we'll skip the
		 * copy step here to save memory and time. */
		if (pop->realloc_copy) {
			struct pollfd *tmp = mm_realloc(pop->event_set_copy,
			    pop->event_count * sizeof(struct pollfd));
			if (tmp == NULL) {
				event_warn("realloc");
				return -1;
			}
			pop->event_set_copy = tmp;
			pop->realloc_copy = 0;
		}
		memcpy(pop->event_set_copy, pop->event_set, sizeof(struct pollfd)*nfds);
		event_set = pop->event_set_copy;
	} else {
		event_set = pop->event_set;
	}

	if (tv != NULL) {
		msec = evutil_tv_to_msec(tv);
		if (msec < 0 || msec > INT_MAX)
			msec = INT_MAX;
	}

	EVBASE_RELEASE_LOCK(base, th_base_lock);

	res = poll(event_set, nfds, msec);

	EVBASE_ACQUIRE_LOCK(base, th_base_lock);

	if (res == -1) {
		if (errno != EINTR) {
			event_warn("poll");
			return (-1);
		}

		return (0);
	}

	event_debug(("%s: poll reports %d", __func__, res));

	if (res == 0 || nfds == 0)
		return (0);

	i = random() % nfds;
	for (int j = 0; j < nfds; j++) {
		int what;
		if (++i == nfds)
			i = 0;
		what = event_set[i].revents;
		if (!what)
			continue;

		res = 0;

		/* If the file gets closed notify */
		if (what & (POLLHUP|POLLERR|POLLNVAL))
			what |= POLLIN|POLLOUT;
		if (what & POLLIN)
			res |= EV_READ;
		if (what & POLLOUT)
			res |= EV_WRITE;
		if (res == 0)
			continue;

		evmap_io_active(base, event_set[i].fd, res);
	}

	return (0);
}

static int poll_add(struct event_base *base, int fd, short old, short events, void *idx_)
{
	struct pollop *pop = base->evbase;
	struct pollfd *pfd = NULL;
	struct pollidx *idx = idx_;
	int i;

    //断言不是信号事件
	EVUTIL_ASSERT((events & EV_SIGNAL) == 0);
	if (!(events & (EV_READ|EV_WRITE)))//不是IO事件，直接退出
		return (0);

    //需要追加pollfd的大小
	if (pop->nfds + 1 >= pop->event_count) {
		struct pollfd *tmp_event_set;
		int tmp_event_count;

		if (pop->event_count < 32)
			tmp_event_count = 32;
		else
			tmp_event_count = pop->event_count * 2;

		/* 需要增加pollfd对象的内存大小 */
		tmp_event_set = mm_realloc(pop->event_set, tmp_event_count * sizeof(struct pollfd));
		if (tmp_event_set == NULL) {
			event_warn("realloc");
			return (-1);
		}
		pop->event_set = tmp_event_set;

		pop->event_count = tmp_event_count;
		pop->realloc_copy = 1;
	}

	i = idx->idxplus1 - 1;

	if (i >= 0) {
		pfd = &pop->event_set[i];
	}
    else {
		i = pop->nfds++;
		pfd = &pop->event_set[i];
		pfd->events = 0;
		pfd->fd = fd;
		idx->idxplus1 = i + 1;
	}

	pfd->revents = 0;
	if (events & EV_WRITE)
		pfd->events |= POLLOUT;
	if (events & EV_READ)
		pfd->events |= POLLIN;

	return (0);
}

static int poll_del(struct event_base *base, int fd, short old, short events, void *idx_)
{
	struct pollop *pop = base->evbase;
	struct pollfd *pfd = NULL;
	struct pollidx *idx = idx_;
	int i;

	EVUTIL_ASSERT((events & EV_SIGNAL) == 0);
	if (!(events & (EV_READ|EV_WRITE)))
		return (0);

	i = idx->idxplus1 - 1;
	if (i < 0)
		return (-1);

	/* Do we still want to read or write? */
	pfd = &pop->event_set[i];
	if (events & EV_READ)
		pfd->events &= ~POLLIN;
	if (events & EV_WRITE)
		pfd->events &= ~POLLOUT;

	if (pfd->events)//fd上有其它事件
		return (0);

	/* 不在关注这个fd */
	idx->idxplus1 = 0;

	--pop->nfds;
	if (i != pop->nfds) {
		/*
		 * Shift the last pollfd down into the now-unoccupied
		 * position.
		 */
		memcpy(&pop->event_set[i], &pop->event_set[pop->nfds],
		       sizeof(struct pollfd));
		idx = evmap_io_get_fdinfo(&base->io, pop->event_set[i].fd);
		EVUTIL_ASSERT(idx);
		EVUTIL_ASSERT(idx->idxplus1 == pop->nfds + 1);
		idx->idxplus1 = i + 1;
	}

	return (0);
}

static void poll_dealloc(struct event_base *base)
{
	struct pollop *pop = base->evbase;

	evsig_dealloc(base);
	if (pop->event_set)
		mm_free(pop->event_set);
	if (pop->event_set_copy)
		mm_free(pop->event_set_copy);

	memset(pop, 0, sizeof(struct pollop));
	mm_free(pop);
}