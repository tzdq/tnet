#include <sys/types.h>
#include <sys/time.h>
#include <sys/select.h>
#include "sys/queue.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include "event_internal.h"
#include "evsignal.h"
#include "event/event.h"
#include "evthread.h"
#include "evlog.h"
#include "evmap.h"

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

static void *select_init(struct event_base *);
static int select_add(struct event_base *, int, short old, short events, void*);
static int select_del(struct event_base *, int, short old, short events, void*);
static int select_dispatch(struct event_base *, struct timeval *);
static void select_dealloc(struct event_base *);

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

#define check_selectop(sop) do { (void) sop; } while (0)

static int select_resize(struct selectop *sop, int fdsz);
static void select_free_selectop(struct selectop *sop);

static void *select_init(struct event_base *base){
    struct selectop *sop;

    if (!(sop = mm_calloc(1, sizeof(struct selectop))))
        return (NULL);

    if (select_resize(sop, SELECT_ALLOC_SIZE(32 + 1))) {
        select_free_selectop(sop);
        return (NULL);
    }

    evsig_init(base);

    return (sop);
}

static int select_add(struct event_base *base, int fd, short old, short events, void *p){
    struct selectop *sop = base->evbase;
    (void)p;

    //断言不是信号事件
    EVUTIL_ASSERT((events & EV_SIGNAL) == 0);
    check_selectop(sop);
    /* 记录最大的fd，调整fdsz，倍增法 */
    if (sop->event_fds < fd) {
        int fdsz = sop->event_fdsz;

        if (fdsz < (int)sizeof(fd_mask))
            fdsz = (int)sizeof(fd_mask);

        //*nix不用担心溢出的问题，每次都是增加的一倍
        while (fdsz < (int) SELECT_ALLOC_SIZE(fd + 1))
            fdsz *= 2;

        //调整
        if (fdsz != sop->event_fdsz) {
            if (select_resize(sop, fdsz)) {
                check_selectop(sop);
                return (-1);
            }
        }

        sop->event_fds = fd;
    }

    if (events & EV_READ)
        FD_SET(fd, sop->event_readset_in);//加入可读可写集合
    if (events & EV_WRITE)
        FD_SET(fd, sop->event_writeset_in);
    check_selectop(sop);

    return (0);
}

static int select_del(struct event_base *base, int fd, short old, short events, void *p){
    struct selectop *sop = base->evbase;
    (void)p;//只是用于去掉p没有使用的警告

    EVUTIL_ASSERT((events & EV_SIGNAL) == 0);
    check_selectop(sop);

    //fd超出当前监听的最大fd，不需要删除，直接返回
    if (sop->event_fds < fd) {
        check_selectop(sop);
        return (0);
    }

    if (events & EV_READ)
        FD_CLR(fd, sop->event_readset_in);

    if (events & EV_WRITE)
        FD_CLR(fd, sop->event_writeset_in);

    check_selectop(sop);
    return (0);
}

static int select_dispatch(struct event_base *base, struct timeval *tv){
    int res = 0;
    int i,nfds;
    struct selectop *sop = base->evbase;

    check_selectop(sop);
    //如果在select_add中调整过输入读写集合的大小，需要同步调整输出读写集合，并清除标志
    if(sop->resize_out_sets){
        fd_set *readset_out = NULL,*writeset_out = NULL;
        size_t sz = sop->event_fdsz;

        if(!(readset_out = mm_realloc(sop->event_readset_out,sz)))
            return -1;
        sop->event_readset_out = readset_out;

        if(!(writeset_out = mm_realloc(sop->event_writeset_out,sz)))
            return -1;
        sop->event_writeset_out = writeset_out;
        sop->resize_out_sets = 0;
    }

    memcpy(sop->event_readset_out,sop->event_readset_in,sop->event_fdsz);
    memcpy(sop->event_writeset_out,sop->event_writeset_in,sop->event_fdsz);

    nfds = sop->event_fds + 1 ;//

    EVBASE_RELEASE_LOCK(base,th_base_lock);
    res = select(nfds,sop->event_readset_out,sop->event_writeset_out,NULL,tv);
    EVBASE_ACQUIRE_LOCK(base, th_base_lock);

    check_selectop(sop);

    if (res == -1) {//出错，需要额外判断是否是中断
        if (errno != EINTR) {
            event_warn("select");
            return (-1);
        }

        return (0);
    }

    event_debug(("%s: select reports %d", __func__, res));

    check_selectop(sop);
    i = random() % nfds;
    //处理就绪事件
    for (int j = 0; j < nfds; ++j) {
        if (++i >= nfds)
            i = 0;
        res = 0;
        if (FD_ISSET(i, sop->event_readset_out))//可读
            res |= EV_READ;
        if (FD_ISSET(i, sop->event_writeset_out))//可写
            res |= EV_WRITE;

        if (res == 0)//没有任何事件，直接continue
            continue;

        evmap_io_active(base, i, res);
    }
    check_selectop(sop);

    return (0);
}

static void select_dealloc(struct event_base *base){
    evsig_dealloc(base);
    select_free_selectop(base->evbase);
}

static int select_resize(struct selectop *sop, int fdsz)
{
    fd_set *readset_in = NULL;
    fd_set *writeset_in = NULL;

    if (sop->event_readset_in)
        check_selectop(sop);

    //调整输入读写事件集合的大小，调整失败时不会释放以前的内存，这儿主要是增加，不存在缩小
    if ((readset_in = mm_realloc(sop->event_readset_in, fdsz)) == NULL)
        goto error;
    sop->event_readset_in = readset_in;
    if ((writeset_in = mm_realloc(sop->event_writeset_in, fdsz)) == NULL) {
        goto error;
    }
    sop->event_writeset_in = writeset_in;
    sop->resize_out_sets = 1;//设置需要调整输出读写事件集合的标志

    //初始化新增的空间
    memset((char *)sop->event_readset_in + sop->event_fdsz, 0,
           fdsz - sop->event_fdsz);
    memset((char *)sop->event_writeset_in + sop->event_fdsz, 0,
           fdsz - sop->event_fdsz);

    sop->event_fdsz = fdsz;
    check_selectop(sop);

    return (0);

    error:
    event_warn("malloc");
    return (-1);
}

static void select_free_selectop(struct selectop *sop)
{
    if (sop->event_readset_in)
        mm_free(sop->event_readset_in);
    if (sop->event_writeset_in)
        mm_free(sop->event_writeset_in);
    if (sop->event_readset_out)
        mm_free(sop->event_readset_out);
    if (sop->event_writeset_out)
        mm_free(sop->event_writeset_out);

    memset(sop, 0, sizeof(struct selectop));
    mm_free(sop);
}
