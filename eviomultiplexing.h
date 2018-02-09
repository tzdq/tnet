//
// Linux 下常见三种IO复用：select poll epoll，默认使用epoll
//

#ifndef TNET_EVIOMULTIPLEXING_H
#define TNET_EVIOMULTIPLEXING_H

#include <sys/select.h>
#include <sys/poll.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <stdint.h>
#include "evmemory.h"
#include "evsignal.h"
#include "event_internal.h"
#include "event2/event.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "evthread.h"
#include "evmap.h"
#include <sys/types.h>
#include <limits.h>
#include <errno.h>
#include "evlog.h"
#include <sys/time.h>
#include <fcntl.h>

//select的相关代码
#ifndef NFDBITS
#define NFDBITS (sizeof(fd_mask)*8)
#endif

#ifndef _howmany
#define _howmany(x,y)	(((x)+((y)-1))/(y))
#endif

#define SELECT_ALLOC_SIZE(n) (_howmany(n,NFDBITS) * sizeof(fd_mask))

struct selectop {
    int event_fds;		/* fd_set中最大的fd */
    int event_fdsz;     /* 当前支持的最大fd，采用倍增法增长 */
    int resize_out_sets; /* 是否需要调整输出读写fd_set的内存空间的标志，如果扩展了输入读写fd_set，设置为1 */
    fd_set *event_readset_in;
    fd_set *event_writeset_in;
    fd_set *event_readset_out;
    fd_set *event_writeset_out;
};

void *select_init(struct event_base *);
int select_add(struct event_base *, int, short old, short events, void*);
int select_del(struct event_base *, int, short old, short events, void*);
int select_dispatch(struct event_base *, struct timeval *);
void select_dealloc(struct event_base *);

const struct eventop selectops = {
        "select",
        select_init,
        select_add,
        select_del,
        select_dispatch,
        select_dealloc,
        0, /* 不需要重新启动. */
        EV_FEATURE_FDS,
        0,
};

//poll 相关代码
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

void *poll_init(struct event_base *);
int poll_add(struct event_base *, int, short old, short events, void *idx);
int poll_del(struct event_base *, int, short old, short events, void *idx);
int poll_dispatch(struct event_base *, struct timeval *);
void poll_dealloc(struct event_base *);

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

//epoll相关代码
struct epollop {
    struct epoll_event *events;
    int nevents;
    int epfd;
};

void *epoll_init(struct event_base *);
int   epoll_dispatch(struct event_base *, struct timeval *);
void  epoll_dealloc(struct event_base *);
int   epoll_add(struct event_base *base, int fd, short old, short events, void *p);
int   epoll_del(struct event_base *base, int fd, short old, short events, void *p);

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

#endif //TNET_EVIOMULTIPLEXING_H
