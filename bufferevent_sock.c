#include <sys/types.h>
#include <sys/time.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "event/util.h"
#include "event/bufferevent.h"
#include "event/buffer.h"
#include "event/event.h"
#include "evlog.h"
#include "evmemory.h"
#include "bufferevent_internal.h"
#include "evutil.h"

static int be_socket_enable(struct bufferevent *, short);
static int be_socket_disable(struct bufferevent *, short);
static void be_socket_destruct(struct bufferevent *);
static int be_socket_adj_timeouts(struct bufferevent *);
static int be_socket_flush(struct bufferevent *, short, enum bufferevent_flush_mode);
static int be_socket_ctrl(struct bufferevent *, enum bufferevent_ctrl_op, union bufferevent_ctrl_data *);
static void be_socket_setfd(struct bufferevent *, int);

const struct bufferevent_ops bufferevent_ops_socket = {
        "socket",
        offsetof(struct bufferevent_private, bev),
        be_socket_enable,
        be_socket_disable,
        be_socket_destruct,
        be_socket_adj_timeouts,
        be_socket_flush,
        be_socket_ctrl,
};

/*
 当socket有数据可读时，Libevent就会监听到，然后调用bufferevent_readcb函数处理。该函数会调用evbuffer_read函数，把数据从socket fd中读取到evbuffer中。然后再调用用户在bufferevent_setcb函数中设置的读事件回调函数。所以，当用户的读事件回调函数被调用时，数据已经在evbuffer中了，用户拿来就用，无需调用read这类会阻塞的函数。
*/
static void bufferevent_readcb(int fd, short event, void *arg)
{
    struct bufferevent *bufev = arg;
    struct bufferevent_private *bufev_p = EVUTIL_UPCAST(bufev, struct bufferevent_private, bev);
    struct evbuffer *input;
    int res = 0;
    short what = BEV_EVENT_READING;
    ssize_t howmuch = -1, readmax=-1;

    bufferevent_incref_and_lock(bufev);

    //超时事件，如果event == EV_TIMEOUT | EV_READ，我们可以安全地忽略超时，因为读取已经发生
    if (event == EV_TIMEOUT) {
        what |= BEV_EVENT_TIMEOUT;
        goto error;
    }

    input = bufev->input;

    /* 如果我们配置了一个高水位，那么我们需要判断读取的数据是否会超过高水位.*/
    if (bufev->wm_read.high != 0) {
        howmuch = bufev->wm_read.high - evbuffer_get_length(input);//当前缓冲区中距离高水位的字节数，小于等于0，停止读
        if (howmuch <= 0) {
            bufferevent_suspend_read(bufev,BEV_SUSPEND_WM);
            goto done;
        }
    }

    //用户最大可读字节，16384字节，即16K。
    readmax = 16384;
    if (howmuch < 0 || howmuch > readmax) /* 使用-1来代替"unlimited"*/
        howmuch = readmax;
    if (bufev_p->read_suspended)
        goto done;

    evbuffer_unfreeze(input, 0);//解冻，使得可以在input的后面追加数据
    res = evbuffer_read(input, fd, (int)howmuch);//从fd读取数据
    evbuffer_freeze(input, 0);

    if (res == -1) {//发送错误，不是 EINTER/EAGAIN 这两个可以重试的错误，此时，应该报告给用户
        int err = errno;
        if (EVUTIL_ERR_RW_RETRIABLE(err))
            goto reschedule;
        what |= BEV_EVENT_ERROR;
    }
    else if (res == 0) {//断开了连接
        what |= BEV_EVENT_EOF;
    }

    if (res <= 0)
        goto error;

    /* //evbuffer的数据量大于低水位值,调用用户设置的读回调 */
    if (evbuffer_get_length(input) >= bufev->wm_read.low)
        bufferevent_run_readcb(bufev);

    goto done;

reschedule:
    goto done;

error:
    bufferevent_disable(bufev, EV_READ);//把监听可读事件的event从event_base的事件队列中删除掉.event_del
    bufferevent_run_eventcb(bufev, what);//调用用户设置的错误处理函数

done:
    bufferevent_decref_and_unlock(bufev);//减少引用计数并解锁
}

/*
 * 当我们确实要写入数据时，才监听可写事件。我们调用bufferevent_write写入数据时，Libevent才会把监听可写事件的那个event注册到event_base中。当Libevent把数据都写入到fd的缓冲区后，Libevent又会把这个event从event_base中删除。
 *
 * bufferevent_writecb还需要判断socket fd是不是已经连接上服务器了。因为这个socket fd是非阻塞的，所以调用connect时，可能还没连接上就返回了。对于非阻塞socket fd，一般是通过判断这个socket是否可写，从而得知这个socket是否已经连接上服务器。如果可写，那么它就已经成功连接上服务器了。
*/
static void bufferevent_writecb(int fd, short event, void *arg)
{
    struct bufferevent *bufev = arg;
    struct bufferevent_private *bufev_p = EVUTIL_UPCAST(bufev, struct bufferevent_private, bev);
    int res = 0;
    short what = BEV_EVENT_WRITING;
    int connected = 0;
    ssize_t atmost = -1;

    bufferevent_incref_and_lock(bufev);

    if (event == EV_TIMEOUT) {
        what |= BEV_EVENT_TIMEOUT;
        goto error;
    }

    if (bufev_p->connecting) {
        int c = evutil_socket_finished_connecting(fd);
        /* 如果连接被立即拒绝，我们需要假冒错误 */
        if (bufev_p->connection_refused) {
            bufev_p->connection_refused = 0;
            c = -1;
        }

        if (c == 0)
            goto done;

        bufev_p->connecting = 0;
        if (c < 0) {
            event_del(&bufev->ev_write);
            event_del(&bufev->ev_read);
            bufferevent_run_eventcb(bufev, BEV_EVENT_ERROR);
            goto done;
        }
        else {
            connected = 1;
            bufferevent_run_eventcb(bufev, BEV_EVENT_CONNECTED);
            if (!(bufev->enabled & EV_WRITE) || bufev_p->write_suspended) {
                event_del(&bufev->ev_write);
                goto done;
            }
        }
    }

    //用户最大可写字节数，atmost将返回16384(16K)
    atmost = 16384;

    if (bufev_p->write_suspended)
        goto done;

    //如果evbuffer有数据可以写到sockfd中
    if (evbuffer_get_length(bufev->output)) {
        evbuffer_unfreeze(bufev->output, 1);//解冻链表头
        /* 将output这个evbuffer的数据写到socket fd 的缓冲区中,会把已经写到socket fd缓冲区的数据，从evbuffer中删除 */
        res = evbuffer_write_atmost(bufev->output, fd, atmost);
        evbuffer_freeze(bufev->output, 1);
        if (res == -1) {
            int err = errno;
            if (EVUTIL_ERR_RW_RETRIABLE(err))
                goto reschedule;
            what |= BEV_EVENT_ERROR;
        }
        else if (res == 0) {
            what |= BEV_EVENT_EOF;
        }
        if (res <= 0)
            goto error;
    }

    //如果把写缓冲区的数据都写完成了。为了防止event_base不断地触发可写事件，此时要把这个监听可写的event删除。
    //前面的atmost限制了一次最大的可写数据。如果还没写完所有的数据那么就不能delete这个event，而是要继续监听可写事件，知道把所有的数据都写到socket fd中。
    if (evbuffer_get_length(bufev->output) == 0) {
        event_del(&bufev->ev_write);
    }

    /* 如果我们的缓冲区已满或低于低水位，则调用用户回调。*/
    if ((res || !connected) && evbuffer_get_length(bufev->output) <= bufev->wm_write.low) {
        bufferevent_run_writecb(bufev);
    }

    goto done;

reschedule:
    if (evbuffer_get_length(bufev->output) == 0) {
        event_del(&bufev->ev_write);
    }
    goto done;

error:
    bufferevent_disable(bufev, EV_WRITE);
    bufferevent_run_eventcb(bufev, what);

done:
    bufferevent_decref_and_unlock(bufev);
}

static void bufferevent_socket_outbuf_cb(struct evbuffer *buf, const struct evbuffer_cb_info *cbinfo, void *arg)
{
    struct bufferevent *bufev = arg;
    struct bufferevent_private *bufev_p = EVUTIL_UPCAST(bufev, struct bufferevent_private, bev);

    if (cbinfo->n_added && (bufev->enabled & EV_WRITE)
        &&!event_pending(&bufev->ev_write, EV_WRITE, NULL) && !bufev_p->write_suspended) {
        /* 把数据添加到缓冲区，我们想写，我们当前没有在写。 所以，开始写。 */
        if (bufferevent_add_event(&bufev->ev_write, &bufev->timeout_write) == -1) {
        }
    }
}

struct bufferevent *bufferevent_socket_new(struct event_base *base, int fd, int options)
{
    struct bufferevent_private *bufev_p;
    struct bufferevent *bufev;

    if ((bufev_p = mm_calloc(1, sizeof(struct bufferevent_private)))== NULL)
        return NULL;

    if (bufferevent_init_common(bufev_p, base, &bufferevent_ops_socket, options) < 0) {
        mm_free(bufev_p);
        return NULL;
    }
    bufev = &bufev_p->bev;
    //设置将evbuffer的数据向fd传
    evbuffer_set_flags(bufev->output, EVBUFFER_FLAG_DRAINS_TO_FD);

    /* 读写事件设置值,fd与event相关联。同一个fd关联两个event */
    event_assign(&bufev->ev_read, bufev->ev_base, fd, EV_READ|EV_PERSIST, bufferevent_readcb, bufev);
    event_assign(&bufev->ev_write, bufev->ev_base, fd, EV_WRITE|EV_PERSIST, bufferevent_writecb, bufev);

    /*设置evbuffer的回调函数，使得外界给写缓冲区添加数据时，能触发写操作,回调对于写事件的监听很重要的 */
    evbuffer_add_cb(bufev->output, bufferevent_socket_outbuf_cb, bufev);

    /*冻结读缓冲区的尾部，未解冻之前不能往读缓冲区追加数据(不能从socket fd中读取数据)  */
    evbuffer_freeze(bufev->input, 0);

    /* 冻结写缓冲区的头部，未解冻之前不能把写缓冲区的头部数据删除(不能把数据写到socket fd ) */
    evbuffer_freeze(bufev->output, 1);

    return bufev;
}

int bufferevent_socket_connect(struct bufferevent *bev, struct sockaddr *sa, int socklen)
{
    struct bufferevent_private *bufev_p = EVUTIL_UPCAST(bev, struct bufferevent_private, bev);

    int fd;
    int r = 0;
    int result=-1;
    int ownfd = 0;

    bufferevent_incref_and_lock(bev);

    if (!bufev_p)
        goto done;

    fd = bufferevent_getfd(bev);
    if (fd < 0) {
        if (!sa)
            goto done;
        fd = socket(sa->sa_family, SOCK_STREAM, 0);
        if (fd < 0)
            goto done;
        if (evutil_make_socket_nonblocking(fd)<0)
            goto done;
        ownfd = 1;
    }
    if (sa) {
        r = evutil_socket_connect(&fd, sa, socklen);
        if (r < 0)
            goto freesock;
    }
    bufferevent_setfd(bev, fd);
    if (r == 0) {
        if (! be_socket_enable(bev, EV_WRITE)) {
            bufev_p->connecting = 1;
            result = 0;
            goto done;
        }
    }
    else if (r == 1) {
        result = 0;
        bufev_p->connecting = 1;
        event_active(&bev->ev_write, EV_WRITE, 1);
    }
    else {
        bufev_p->connection_refused = 1;
        bufev_p->connecting = 1;
        result = 0;
        event_active(&bev->ev_write, EV_WRITE, 1);
    }

    goto done;

freesock:
    bufferevent_run_eventcb(bev, BEV_EVENT_ERROR);
    if (ownfd)
        close(fd);
done:
    bufferevent_decref_and_unlock(bev);
    return result;
}


int bufferevent_priority_set(struct bufferevent *bufev, int priority)
{
    int r = -1;

    BEV_LOCK(bufev);
    if (bufev->be_ops != &bufferevent_ops_socket)
        goto done;

    if (event_priority_set(&bufev->ev_read, priority) == -1)
        goto done;
    if (event_priority_set(&bufev->ev_write, priority) == -1)
        goto done;

    r = 0;
    done:
    BEV_UNLOCK(bufev);
    return r;
}

int bufferevent_base_set(struct event_base *base, struct bufferevent *bufev)
{
    int res = -1;

    BEV_LOCK(bufev);
    if (bufev->be_ops != &bufferevent_ops_socket)
        goto done;

    bufev->ev_base = base;

    res = event_base_set(base, &bufev->ev_read);
    if (res == -1)
        goto done;

    res = event_base_set(base, &bufev->ev_write);
done:
    BEV_UNLOCK(bufev);
    return res;
}

int bufferevent_socket_get_dns_error(struct bufferevent *bev)
{
    int rv;
    struct bufferevent_private *bev_p = EVUTIL_UPCAST(bev, struct bufferevent_private, bev);

    BEV_LOCK(bev);
    rv = bev_p->dns_error;
    BEV_UNLOCK(bev);

    return rv;
}

static int be_socket_enable(struct bufferevent *bufev, short event)
{
    if (event & EV_READ) {
        if (bufferevent_add_event(&bufev->ev_read,&bufev->timeout_read) == -1)
            return -1;
    }
    if (event & EV_WRITE) {
        if (bufferevent_add_event(&bufev->ev_write,&bufev->timeout_write) == -1)
            return -1;
    }
    return 0;
}

static int be_socket_disable(struct bufferevent *bufev, short event)
{
    struct bufferevent_private *bufev_p = EVUTIL_UPCAST(bufev, struct bufferevent_private, bev);
    if (event & EV_READ) {
        if (event_del(&bufev->ev_read) == -1)
            return -1;
    }
    /* 如果我们尝试连接，实际上不要禁用写。*/
    if ((event & EV_WRITE) && ! bufev_p->connecting) {
        if (event_del(&bufev->ev_write) == -1)
            return -1;
    }
    return 0;
}

static void be_socket_destruct(struct bufferevent *bufev)
{
    struct bufferevent_private *bufev_p = EVUTIL_UPCAST(bufev, struct bufferevent_private, bev);
    int fd;
    EVUTIL_ASSERT(bufev->be_ops == &bufferevent_ops_socket);

    fd = event_get_fd(&bufev->ev_read);

    event_del(&bufev->ev_read);
    event_del(&bufev->ev_write);

    if ((bufev_p->options & BEV_OPT_CLOSE_ON_FREE) && fd >= 0)
        close(fd);
}

static int be_socket_adj_timeouts(struct bufferevent *bufev)
{
    int r = 0;
    if (event_pending(&bufev->ev_read, EV_READ, NULL))
        if (bufferevent_add_event(&bufev->ev_read, &bufev->timeout_read) < 0)
            r = -1;
    if (event_pending(&bufev->ev_write, EV_WRITE, NULL)) {
        if (bufferevent_add_event(&bufev->ev_write, &bufev->timeout_write) < 0)
            r = -1;
    }
    return r;
}

static int be_socket_flush(struct bufferevent *bev, short iotype, enum bufferevent_flush_mode mode)
{
    return 0;
}

static void be_socket_setfd(struct bufferevent *bufev, int fd)
{
    BEV_LOCK(bufev);
    EVUTIL_ASSERT(bufev->be_ops == &bufferevent_ops_socket);

    event_del(&bufev->ev_read);
    event_del(&bufev->ev_write);

    event_assign(&bufev->ev_read, bufev->ev_base, fd, EV_READ|EV_PERSIST, bufferevent_readcb, bufev);
    event_assign(&bufev->ev_write, bufev->ev_base, fd, EV_WRITE|EV_PERSIST, bufferevent_writecb, bufev);

    if (fd >= 0)
        bufferevent_enable(bufev, bufev->enabled);

    BEV_UNLOCK(bufev);
}

static int be_socket_ctrl(struct bufferevent *bev, enum bufferevent_ctrl_op op, union bufferevent_ctrl_data *data)
{
    switch (op) {
        case BEV_CTRL_SET_FD:
            be_socket_setfd(bev, data->fd);
            return 0;
        case BEV_CTRL_GET_FD:
            data->fd = event_get_fd(&bev->ev_read);
            return 0;
        case BEV_CTRL_GET_UNDERLYING:
        case BEV_CTRL_CANCEL_ALL:
        default:
            return -1;
    }
}


