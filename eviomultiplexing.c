#include "eviomultiplexing.h"
#include <unistd.h>
#include <signal.h>
#include "event2/thread.h"

#define check_selectop(sop) do { (void) sop; } while (0)

int select_resize(struct selectop *sop, int fdsz)
{
    fd_set *readset_in = NULL;
    fd_set *writeset_in = NULL;

    if (sop->event_readset_in)
        check_selectop(sop);

    //调整输入读写事件集合的大小，调整失败时不会释放以前的内存，这儿主要是增加，不存在缩小
    if ((readset_in = (fd_set *)mm_realloc(sop->event_readset_in, fdsz)) == NULL)
        goto error;
    sop->event_readset_in = readset_in;
    if ((writeset_in = (fd_set *)mm_realloc(sop->event_writeset_in, fdsz)) == NULL) {
        goto error;
    }
    sop->event_writeset_in = writeset_in;
    sop->resize_out_sets = 1;//设置需要调整输出读写事件集合的标志

    //初始化新增的空间
    memset((char *)sop->event_readset_in + sop->event_fdsz, 0, fdsz - sop->event_fdsz);
    memset((char *)sop->event_writeset_in + sop->event_fdsz, 0, fdsz - sop->event_fdsz);

    sop->event_fdsz = fdsz;
    check_selectop(sop);

    return (0);

    error:
    event_warn("malloc");
    return (-1);
}

void select_free_selectop(struct selectop *sop)
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

void *select_init(struct event_base *base){
    struct selectop *sop;

    if (!(sop = (struct selectop *)mm_calloc(1, sizeof(struct selectop))))
        return (NULL);

    if (select_resize(sop, SELECT_ALLOC_SIZE(32 + 1))) {
        select_free_selectop(sop);
        return (NULL);
    }

    evsig_init(base);

    return (sop);
}

int select_add(struct event_base *base, int fd, short old, short events, void *p){
    struct selectop *sop = (struct selectop *)base->evbase;
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

int select_del(struct event_base *base, int fd, short old, short events, void *p){
    struct selectop *sop = (struct selectop *)base->evbase;
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

int select_dispatch(struct event_base *base, struct timeval *tv){
    int res = 0;
    int i,nfds;
    struct selectop *sop = (struct selectop *)base->evbase;

    check_selectop(sop);
    //如果在select_add中调整过输入读写集合的大小，需要同步调整输出读写集合，并清除标志
    if(sop->resize_out_sets){
        fd_set *readset_out = NULL,*writeset_out = NULL;
        size_t sz = sop->event_fdsz;

        if(!(readset_out = (fd_set *)mm_realloc(sop->event_readset_out,sz)))
            return -1;
        sop->event_readset_out = readset_out;

        if(!(writeset_out = (fd_set *)mm_realloc(sop->event_writeset_out,sz)))
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

void select_dealloc(struct event_base *base){
    evsig_dealloc(base);
    select_free_selectop((struct selectop *)base->evbase);
}

void *poll_init(struct event_base *base)
{
    struct pollop *pollop;

    if (!(pollop = (struct pollop *)mm_calloc(1, sizeof(struct pollop))))
        return (NULL);

    evsig_init(base);

    return (pollop);
}

int poll_dispatch(struct event_base *base, struct timeval *tv)
{
    int res, i, nfds;
    long msec = -1;
    struct pollop *pop = (struct pollop *)base->evbase;
    struct pollfd *event_set;

    nfds = pop->nfds;

    if (base->th_base_lock) {
        /* If we're using this backend in a multithreaded setting,
         * then we need to work on a copy of event_set, so that we can
         * let other threads modify the main event_set while we're
         * polling. If we're not multithreaded, then we'll skip the
         * copy step here to save memory and time. */
        if (pop->realloc_copy) {
            struct pollfd *tmp = (struct pollfd *)mm_realloc(pop->event_set_copy,
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

int poll_add(struct event_base *base, int fd, short old, short events, void *idx_)
{
    struct pollop *pop = (struct pollop *)base->evbase;
    struct pollfd *pfd = NULL;
    struct pollidx *idx = (struct pollidx *)idx_;
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
        tmp_event_set = (struct pollfd *)mm_realloc(pop->event_set, tmp_event_count * sizeof(struct pollfd));
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

int poll_del(struct event_base *base, int fd, short old, short events, void *idx_)
{
    struct pollop *pop = (struct pollop *)base->evbase;
    struct pollfd *pfd = NULL;
    struct pollidx *idx = (struct pollidx *)idx_;
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
        idx = (struct pollidx *)evmap_io_get_fdinfo(&base->io, pop->event_set[i].fd);
        EVUTIL_ASSERT(idx);
        EVUTIL_ASSERT(idx->idxplus1 == pop->nfds + 1);
        idx->idxplus1 = i + 1;
    }

    return (0);
}

void poll_dealloc(struct event_base *base)
{
    struct pollop *pop = (struct pollop *)base->evbase;

    evsig_dealloc(base);
    if (pop->event_set)
        mm_free(pop->event_set);
    if (pop->event_set_copy)
        mm_free(pop->event_set_copy);

    memset(pop, 0, sizeof(struct pollop));
    mm_free(pop);
}

#define INITIAL_NEVENT 32
#define MAX_NEVENT 4096

/*
 * Linux内核版本2.6.24.4前，epoll不能处理大于(LONG_MAX - 999ULL)/HZ的超时值，HZ最大值为1000
 * LONG_MAX最大值为1<<31-1,所以最大支持的msec为2147482 = (1<<31-1 - 999)/1000,四舍五入到2147s
 */
#define MAX_EPOLL_TIMEOUT_MSEC (35*60*1000)

const char *epoll_op_to_string(int op)
{
    return op == EPOLL_CTL_ADD ? "ADD":
           op == EPOLL_CTL_DEL ? "DEL":
           op == EPOLL_CTL_MOD ? "MOD":
           "???";
}

const char *change_to_string(int change)
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

char* print_change(int op,uint32_t events,const struct event_change *ch,const char *status)
{
    static char buf[512] = {0};
    //#define print_change(op, events, ch, status)
    evutil_snprintf(buf, sizeof(buf),"Epoll %s(%d) on fd %d %s.Old events were %d;"
                            "read change was %d (%s);write change was %d (%s);"
            ,epoll_op_to_string(op),events,ch->fd,status,ch->old_events
            ,ch->read_change,change_to_string(ch->read_change)
            ,ch->write_change,change_to_string(ch->write_change));
    return buf;
}

int epoll_apply_one_change(struct event_base *base, struct epollop *epollop, const struct event_change *ch)
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

void *epoll_init(struct event_base *base)
{
    int epfd = -1;
    struct epollop *epollop;

    /* 尝试使用epoll_create1 */
    if((epfd = epoll_create1(EPOLL_CLOEXEC)) == -1){
        if(errno != ENOSYS)
            event_warn("epoll_create1");
        return NULL;
    }

    if (!(epollop = (struct epollop *)mm_calloc(1, sizeof(struct epollop)))) {
        close(epfd);
        return (NULL);
    }

    epollop->epfd = epfd;

    /* 初始化字段 */
    epollop->events = (struct epoll_event *)mm_calloc(INITIAL_NEVENT, sizeof(struct epoll_event));
    if (epollop->events == NULL) {
        mm_free(epollop);
        close(epfd);
        return (NULL);
    }
    epollop->nevents = INITIAL_NEVENT;

    evsig_init(base);

    return (epollop);
}

int epoll_add(struct event_base *base, int fd, short old, short events, void *p)
{
    struct event_change ch;
    ch.fd = fd;
    ch.old_events = old;
    ch.read_change = ch.write_change = 0;
    if (events & EV_WRITE)
        ch.write_change = EV_CHANGE_ADD | (events & EV_ET);
    if (events & EV_READ)
        ch.read_change = EV_CHANGE_ADD | (events & EV_ET);

    return epoll_apply_one_change(base, (struct epollop *)base->evbase, &ch);
}

int epoll_del(struct event_base *base, int fd, short old, short events, void *p)
{
    struct event_change ch;
    ch.fd = fd;
    ch.old_events = old;
    ch.read_change = ch.write_change = 0;
    if (events & EV_WRITE)
        ch.write_change = EV_CHANGE_DEL;
    if (events & EV_READ)
        ch.read_change = EV_CHANGE_DEL;

    return epoll_apply_one_change(base, (struct epollop *)base->evbase, &ch);
}

int epoll_dispatch(struct event_base *base, struct timeval *tv)
{
    struct epollop *epollop = (struct epollop *)base->evbase;
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

        new_events = (struct epoll_event *)mm_realloc(epollop->events, new_nevents * sizeof(struct epoll_event));
        if (new_events) {
            epollop->events = new_events;
            epollop->nevents = new_nevents;
        }
    }

    return (0);
}

void epoll_dealloc(struct event_base *base)
{
    struct epollop *epollop = (struct epollop *)base->evbase;

    evsig_dealloc(base);
    if (epollop->events)
        mm_free(epollop->events);
    if (epollop->epfd >= 0)
        close(epollop->epfd);

    memset(epollop, 0, sizeof(struct epollop));
    mm_free(epollop);
}