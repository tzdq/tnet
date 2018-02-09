#include <sys/types.h>
#include <errno.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include "event2/listener.h"
#include "event2/util.h"
#include "event2/event.h"
#include "event2/event_struct.h"
#include "evmemory.h"
#include "evutil.h"
#include "evlog.h"
#include "evthread.h"

struct evconnlistener_ops {
    int (*enable)(struct evconnlistener *);
    int (*disable)(struct evconnlistener *);
    void (*destroy)(struct evconnlistener *);
    void (*shutdown)(struct evconnlistener *);
    int (*getfd)(struct evconnlistener *);
    struct event_base *(*getbase)(struct evconnlistener *);
};

struct evconnlistener {
    const struct evconnlistener_ops *ops;
    void *lock;
    evconnlistener_cb cb;
    evconnlistener_errorcb errorcb;
    void *user_data;
    unsigned flags;
    short refcnt;
    int accept4_flags;
    unsigned enabled : 1;
};

struct evconnlistener_event {
    struct evconnlistener base;
    struct event listener;
};

#define LOCK(listener) EVLOCK_LOCK((listener)->lock, 0)
#define UNLOCK(listener) EVLOCK_UNLOCK((listener)->lock, 0)

struct evconnlistener *evconnlistener_new_async(struct event_base *base,
                         evconnlistener_cb cb, void *ptr, unsigned flags, int backlog, int fd);

static int event_listener_enable(struct evconnlistener *);
static int event_listener_disable(struct evconnlistener *);
static void event_listener_destroy(struct evconnlistener *);
static int event_listener_getfd(struct evconnlistener *);
static struct event_base *event_listener_getbase(struct evconnlistener *);

static int listener_decref_and_unlock(struct evconnlistener *listener)
{
    int refcnt = --listener->refcnt;
    if (refcnt == 0) {
        listener->ops->destroy(listener);
        UNLOCK(listener);
        EVTHREAD_FREE_LOCK(listener->lock, EVTHREAD_LOCKTYPE_RECURSIVE);
        mm_free(listener);
        return 1;
    } else {
        UNLOCK(listener);
        return 0;
    }
}

static const struct evconnlistener_ops evconnlistener_event_ops = {
        event_listener_enable,
        event_listener_disable,
        event_listener_destroy,
        NULL, /* shutdown */
        event_listener_getfd,
        event_listener_getbase
};

static void listener_read_cb(int, short, void *);

struct evconnlistener *evconnlistener_new(struct event_base *base, evconnlistener_cb cb,
                                          void *ptr, unsigned flags, int backlog,int fd)
{
    struct evconnlistener_event *lev;

    if (backlog > 0) {
        if (listen(fd, backlog) < 0)
            return NULL;
    } else if (backlog < 0) {
        if (listen(fd, 128) < 0)
            return NULL;
    }

    lev = (struct evconnlistener_event *)mm_calloc(1, sizeof(struct evconnlistener_event));
    if (!lev)
        return NULL;

    lev->base.ops = &evconnlistener_event_ops;
    lev->base.cb = cb;
    lev->base.user_data = ptr;
    lev->base.flags = flags;
    lev->base.refcnt = 1;

    lev->base.accept4_flags = 0;
    if (!(flags & LEV_OPT_LEAVE_SOCKETS_BLOCKING))
        lev->base.accept4_flags |= SOCK_NONBLOCK;
    if (flags & LEV_OPT_CLOSE_ON_EXEC)
        lev->base.accept4_flags |= SOCK_CLOEXEC;

    if (flags & LEV_OPT_THREADSAFE) {
        EVTHREAD_ALLOC_LOCK(lev->base.lock, EVTHREAD_LOCKTYPE_RECURSIVE);
    }

    event_assign(&lev->listener, base, fd, EV_READ|EV_PERSIST, listener_read_cb, lev);

    if (!(flags & LEV_OPT_DISABLED))
        evconnlistener_enable(&lev->base);

    return &lev->base;
}

struct evconnlistener *evconnlistener_new_bind(struct event_base *base, evconnlistener_cb cb,
                        void *ptr, unsigned flags, int backlog, const struct sockaddr *sa, int socklen)
{
    struct evconnlistener *listener;
    int fd;
    int on = 1;
    int family = sa ? sa->sa_family : AF_UNSPEC;
    int socktype = SOCK_STREAM | SOCK_NONBLOCK;

    if (backlog == 0)
        return NULL;

    if (flags & LEV_OPT_CLOSE_ON_EXEC)
        socktype |= SOCK_CLOEXEC;

    fd = evutil_socket(family, socktype, 0);
    if (fd == -1)
        return NULL;

    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (void*)&on, sizeof(on))<0)
        goto err;

    if (flags & LEV_OPT_REUSEABLE) {
        if (evutil_make_listen_socket_reuseable(fd) < 0)
            goto err;
    }

    if (flags & LEV_OPT_REUSEABLE_PORT) {
        if (evutil_make_listen_socket_reuseable_port(fd) < 0)
            goto err;
    }

    if (flags & LEV_OPT_DEFERRED_ACCEPT) {
        if (evutil_make_tcp_listen_socket_deferred(fd) < 0)
            goto err;
    }

    if (sa) {
        if (bind(fd, sa, socklen)<0)
            goto err;
    }

    listener = evconnlistener_new(base, cb, ptr, flags, backlog, fd);
    if (!listener)
        goto err;

    return listener;
err:
    close(fd);
    return NULL;
}

void evconnlistener_free(struct evconnlistener *lev)
{
    LOCK(lev);
    lev->cb = NULL;
    lev->errorcb = NULL;
    if (lev->ops->shutdown)
        lev->ops->shutdown(lev);
    listener_decref_and_unlock(lev);
}

static void event_listener_destroy(struct evconnlistener *lev)
{
    struct evconnlistener_event *lev_e = EVUTIL_UPCAST(lev, struct evconnlistener_event, base);

    event_del(&lev_e->listener);
    if (lev->flags & LEV_OPT_CLOSE_ON_FREE)
        close(event_get_fd(&lev_e->listener));
    event_debug_unassign(&lev_e->listener);
}

int evconnlistener_enable(struct evconnlistener *lev)
{
    int r;
    LOCK(lev);
    lev->enabled = 1;
    if (lev->cb)
        r = lev->ops->enable(lev);
    else
        r = 0;
    UNLOCK(lev);
    return r;
}

int evconnlistener_disable(struct evconnlistener *lev)
{
    int r;
    LOCK(lev);
    lev->enabled = 0;
    r = lev->ops->disable(lev);
    UNLOCK(lev);
    return r;
}

static int event_listener_enable(struct evconnlistener *lev)
{
    struct evconnlistener_event *lev_e = EVUTIL_UPCAST(lev, struct evconnlistener_event, base);
    return event_add(&lev_e->listener, NULL);
}

static int event_listener_disable(struct evconnlistener *lev)
{
    struct evconnlistener_event *lev_e = EVUTIL_UPCAST(lev, struct evconnlistener_event, base);
    return event_del(&lev_e->listener);
}

int evconnlistener_get_fd(struct evconnlistener *lev)
{
    int fd;
    LOCK(lev);
    fd = lev->ops->getfd(lev);
    UNLOCK(lev);
    return fd;
}

static int event_listener_getfd(struct evconnlistener *lev)
{
    struct evconnlistener_event *lev_e = EVUTIL_UPCAST(lev, struct evconnlistener_event, base);
    return event_get_fd(&lev_e->listener);
}

struct event_base *evconnlistener_get_base(struct evconnlistener *lev)
{
    struct event_base *base;
    LOCK(lev);
    base = lev->ops->getbase(lev);
    UNLOCK(lev);
    return base;
}

static struct event_base *event_listener_getbase(struct evconnlistener *lev)
{
    struct evconnlistener_event *lev_e = EVUTIL_UPCAST(lev, struct evconnlistener_event, base);
    return event_get_base(&lev_e->listener);
}

void evconnlistener_set_cb(struct evconnlistener *lev, evconnlistener_cb cb, void *arg)
{
    int enable = 0;
    LOCK(lev);
    if (lev->enabled && !lev->cb)
        enable = 1;
    lev->cb = cb;
    lev->user_data = arg;
    if (enable)
        evconnlistener_enable(lev);
    UNLOCK(lev);
}

void evconnlistener_set_error_cb(struct evconnlistener *lev, evconnlistener_errorcb errorcb)
{
    LOCK(lev);
    lev->errorcb = errorcb;
    UNLOCK(lev);
}

static void listener_read_cb(int fd, short what, void *p)
{
    struct evconnlistener *lev = (struct evconnlistener *)p;
    int err;
    evconnlistener_cb cb;
    evconnlistener_errorcb errorcb;
    void *user_data;
    LOCK(lev);
    while (1) {
        struct sockaddr_storage ss;
        socklen_t socklen = sizeof(ss);
        int new_fd = evutil_accept(fd, (struct sockaddr*)&ss, &socklen, lev->accept4_flags);
        if (new_fd < 0)
            break;
        if (socklen == 0) {
            close(new_fd);
            continue;
        }

        if (lev->cb == NULL) {
            close(new_fd);
            UNLOCK(lev);
            return;
        }
        ++lev->refcnt;
        cb = lev->cb;
        user_data = lev->user_data;
        UNLOCK(lev);
        cb(lev, new_fd, (struct sockaddr*)&ss, (int)socklen, user_data);
        LOCK(lev);
        if (lev->refcnt == 1) {
            int freed = listener_decref_and_unlock(lev);
            EVUTIL_ASSERT(freed);

            close(new_fd);
            return;
        }
        --lev->refcnt;
    }
    err = errno;
    if (EVUTIL_ERR_ACCEPT_RETRIABLE(err)) {
        UNLOCK(lev);
        return;
    }
    if (lev->errorcb != NULL) {
        ++lev->refcnt;
        errorcb = lev->errorcb;
        user_data = lev->user_data;
        UNLOCK(lev);
        errorcb(lev, user_data);
        LOCK(lev);
        listener_decref_and_unlock(lev);
    } else {
        event_sock_warn(fd, "Error from accept() call");
        UNLOCK(lev);
    }
}