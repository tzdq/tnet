#include <stdint.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <sys/time.h>
#include "sys/queue.h"
#include <sys/epoll.h>
#include <signal.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include "event_internal.h"
#include "evsignal.h"
#include "event/thread.h"
#include "evthread.h"
#include "evlog.h"
#include "evmap.h"

struct epollop {
	struct epoll_event *events;
	int nevents;
	int epfd;
};

static void *epoll_init(struct event_base *);
static int   epoll_dispatch(struct event_base *, struct timeval *);
static void  epoll_dealloc(struct event_base *);
static int   epoll_add(struct event_base *base, int fd, short old, short events, void *p);
static int   epoll_del(struct event_base *base, int fd, short old, short events, void *p);

const struct eventop epollops = {
	"epoll",
	epoll_init,
	epoll_add,
	epoll_del,
	epoll_dispatch,
	epoll_dealloc,
	1, /* need reinit */
	EV_FEATURE_ET|EV_FEATURE_O1,
	0
};

#define INITIAL_NEVENT 32
#define MAX_NEVENT 4096

/*
 * Linux内核版本2.6.24.4前，epoll不能处理大于(LONG_MAX - 999ULL)/HZ的超时值，HZ最大值为1000
 * LONG_MAX最大值为1<<31-1,所以最大支持的msec为2147482 = (1<<31-1 - 999)/1000,四舍五入到2147s
 */
#define MAX_EPOLL_TIMEOUT_MSEC (35*60*1000)


static const char *change_to_string(int change)
{
	change &= (EV_CHANGE_ADD|EV_CHANGE_DEL);
	if (change == EV_CHANGE_ADD) {
		return "add";
	} else if (change == EV_CHANGE_DEL) {
		return "del";
	} else if (change == 0) {
		return "none";
	} else {
		return "???";
	}
}

static const char *epoll_op_to_string(int op)
{
	return op == EPOLL_CTL_ADD ? "ADD":
	    op == EPOLL_CTL_DEL ? "DEL":
	    op == EPOLL_CTL_MOD ? "MOD":
	    "???";
}

static char* print_change(int op,uint32_t events,const struct event_change *ch,const char *status){
	static char buf[512] = {0};
	//#define print_change(op, events, ch, status)
	evutil_snprintf(buf, sizeof(buf),"Epoll %s(%d) on fd %d %s.Old events were %d;"
							"read change was %d (%s);write change was %d (%s);"
			,epoll_op_to_string(op),events,ch->fd,status,ch->old_events
			,ch->read_change,change_to_string(ch->read_change)
			,ch->write_change,change_to_string(ch->write_change));
	return buf;
}

static int epoll_apply_one_change(struct event_base *base, struct epollop *epollop, const struct event_change *ch)
{
	struct epoll_event epev;
	int op, events = 0;

	if((ch->read_change & EV_CHANGE_ADD) || (ch->write_change & EV_CHANGE_ADD)){
		//如果我们添加任何东西，我们将做一个ADD或者MOD
		events = 0;
		op = EPOLL_CTL_ADD;
		if(ch->read_change & EV_CHANGE_ADD)
			events |= EPOLLIN;
		else if(ch->read_change & EV_CHANGE_DEL)
			;
		else if(ch->old_events & EV_READ)
			events |= EPOLLIN;

		if(ch->write_change & EV_CHANGE_ADD)
			events |= EPOLLOUT;
		else if(ch->write_change & EV_CHANGE_DEL)
			;
		else if(ch->old_events & EV_WRITE)
			events |= EPOLLOUT;

		if ((ch->read_change|ch->write_change) & EV_ET)
			events |= EPOLLET;

		//如果MOD失败，尝试ADD，如果ADD失败，尝试MOD。如果fd被关闭并重新启动，MOD会出错；如果fd被dup重新创建，和以前的文件一样，ADD会出错。
		if(ch->old_events){
			op = EPOLL_CTL_MOD;
		}
	}
	else if((ch->read_change & EV_CHANGE_DEL) || (ch->write_change & EV_CHANGE_DEL)){
		op = EPOLL_CTL_DEL;
		if (ch->read_change & EV_CHANGE_DEL) {
			if (ch->write_change & EV_CHANGE_DEL){
				events = EPOLLIN | EPOLLOUT;
			}
			else if (ch->old_events & EV_WRITE) {
				events = EPOLLOUT;
				op = EPOLL_CTL_MOD;
			}
			else {
				events = EPOLLIN;
			}
		}
		else if (ch->write_change & EV_CHANGE_DEL) {
			if (ch->old_events & EV_READ) {
				events = EPOLLIN;
				op = EPOLL_CTL_MOD;
			} else {
				events = EPOLLOUT;
			}
		}
	}

	if(!events)
		return 0;

	memset(&epev,0, sizeof(epev));
	epev.data.fd = ch->fd;
	epev.events = events;

	if(epoll_ctl(epollop->epfd,op,ch->fd,&epev) == 0){
		event_debug((print_change(op, epev.events, ch, "okay")));
		return 0;
	}

	switch (op) {
	case EPOLL_CTL_MOD:
		if (errno == ENOENT) {
			 /* 如果一个MOD操作失败，错误码为ENOENT，那么fd可能被关闭并重新打开，我们应该尝试ADD操作 */
			if (epoll_ctl(epollop->epfd, EPOLL_CTL_ADD, ch->fd, &epev) == -1) {
				event_warn("Epoll MOD(%d) on %d retried as ADD; that failed too", (int)epev.events, ch->fd);
				return -1;
			}
			else {
				event_debug(("Epoll MOD(%d) on %d retried as ADD; succeeded.", (int)epev.events, ch->fd));
				return 0;
			}
		}
		break;
	case EPOLL_CTL_ADD:
		if (errno == EEXIST) {
			/* 如果ADD操作失败，并且错误码为EEXIST，那么操作是多余的，使用dup *（）将相同的文件复制到同一个fd中，提供相同的内容，而不是一个新的。 对于第二种情况，我们必须用MOD重试 */
			if (epoll_ctl(epollop->epfd, EPOLL_CTL_MOD, ch->fd, &epev) == -1) {
				event_warn("Epoll ADD(%d) on %d retried as MOD; that failed too", (int)epev.events, ch->fd);
				return -1;
			}
			else {
				event_debug(("Epoll ADD(%d) on %d retried as MOD; succeeded.", (int)epev.events, ch->fd));
				return 0;
			}
		}
		break;
	case EPOLL_CTL_DEL:
		if (errno == ENOENT || errno == EBADF || errno == EPERM) {
			/* 如果删除失败，错误码为这些，没有关系：我们我们在epoll_dispatch之前关闭了fd */
			event_debug(("Epoll DEL(%d) on fd %d gave %s: DEL was unnecessary.",
					(int)epev.events, ch->fd, strerror(errno)));
			return 0;
		}
		break;
	default:
		break;
	}

	event_warn(print_change(op, epev.events, ch, "failed"));
	return -1;
}

static void *epoll_init(struct event_base *base)
{
	int epfd = -1;
	struct epollop *epollop;

	/* 尝试使用epoll_create1 */
	if((epfd = epoll_create1(EPOLL_CLOEXEC)) == -1){
		if(errno != ENOSYS)
			event_warn("epoll_create1");
		return NULL;
	}

	if (!(epollop = mm_calloc(1, sizeof(struct epollop)))) {
		close(epfd);
		return (NULL);
	}

	epollop->epfd = epfd;

	/* 初始化字段 */
	epollop->events = mm_calloc(INITIAL_NEVENT, sizeof(struct epoll_event));
	if (epollop->events == NULL) {
		mm_free(epollop);
		close(epfd);
		return (NULL);
	}
	epollop->nevents = INITIAL_NEVENT;

	evsig_init(base);

	return (epollop);
}

static int epoll_add(struct event_base *base, int fd, short old, short events, void *p)
{
	struct event_change ch;
	ch.fd = fd;
	ch.old_events = old;
	ch.read_change = ch.write_change = 0;
	if (events & EV_WRITE)
		ch.write_change = EV_CHANGE_ADD | (events & EV_ET);
	if (events & EV_READ)
		ch.read_change = EV_CHANGE_ADD | (events & EV_ET);

	return epoll_apply_one_change(base, base->evbase, &ch);
}

static int epoll_del(struct event_base *base, int fd, short old, short events, void *p)
{
	struct event_change ch;
	ch.fd = fd;
	ch.old_events = old;
	ch.read_change = ch.write_change = 0;
	if (events & EV_WRITE)
		ch.write_change = EV_CHANGE_DEL;
	if (events & EV_READ)
		ch.read_change = EV_CHANGE_DEL;

	return epoll_apply_one_change(base, base->evbase, &ch);
}

static int epoll_dispatch(struct event_base *base, struct timeval *tv)
{
	struct epollop *epollop = base->evbase;
	struct epoll_event *events = epollop->events;
	int  res;
	long timeout = -1;

	if (tv != NULL) {
		timeout = evutil_tv_to_msec(tv);//设置正确的超时值
		if (timeout < 0 || timeout > MAX_EPOLL_TIMEOUT_MSEC) {
			timeout = MAX_EPOLL_TIMEOUT_MSEC;
		}
	}

	EVBASE_RELEASE_LOCK(base, th_base_lock);

	res = epoll_wait(epollop->epfd, events, epollop->nevents, timeout);

	EVBASE_ACQUIRE_LOCK(base, th_base_lock);

	if (res == -1) {
		if (errno != EINTR) {
			event_warn("epoll_wait");
			return (-1);
		}

		return (0);
	}

	event_debug(("%s: epoll_wait reports %d", __func__, res));
	EVUTIL_ASSERT(res <= epollop->nevents);

	for (int i = 0; i < res; i++) {
		int what = events[i].events;
		short ev = 0;

		if (what & (EPOLLHUP|EPOLLERR)) {
			ev = EV_READ | EV_WRITE;
		}
		else
		{
			if (what & EPOLLIN)
				ev |= EV_READ;
			if (what & EPOLLOUT)
				ev |= EV_WRITE;
		}

		if (!ev)
			continue;

		evmap_io_active(base, events[i].data.fd, ev | EV_ET);
	}

    //这次使用了所有空间，扩容
	if (res == epollop->nevents && epollop->nevents < MAX_NEVENT) {
		int new_nevents = epollop->nevents * 2;
		struct epoll_event *new_events;

		new_events = mm_realloc(epollop->events, new_nevents * sizeof(struct epoll_event));
		if (new_events) {
			epollop->events = new_events;
			epollop->nevents = new_nevents;
		}
	}

	return (0);
}

static void epoll_dealloc(struct event_base *base)
{
	struct epollop *epollop = base->evbase;

	evsig_dealloc(base);
	if (epollop->events)
		mm_free(epollop->events);
	if (epollop->epfd >= 0)
		close(epollop->epfd);

	memset(epollop, 0, sizeof(struct epollop));
	mm_free(epollop);
}
