#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <time.h>
#include <sys/stat.h>
#include <ifaddrs.h>
#include "event/util.h"
#include "evutil.h"
#include "evlog.h"
#include "evmemory.h"
#include "evthread.h"

int evutil_snprintf(char *buf, size_t buflen, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int r = evutil_vsnprintf(buf, buflen, format, ap);
    va_end(ap);
    return r;
}

int evutil_vsnprintf(char *buf, size_t buflen, const char *format, va_list ap)
{
    if (!buflen)
        return 0;

    int r = vsnprintf(buf, buflen, format, ap);

    buf[buflen-1] = '\0';
    return r;
}

int evutil_open_closeonexec_(const char *pathname, int flags, unsigned mode)
{
    int fd;

    fd = open(pathname, flags, (mode_t)mode);
    if (fd < 0)
        return -1;

    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

int evutil_socketpair(int family, int type, int protocol, int fd[2])
{
    return socketpair(family, type, protocol, fd);
}

int evutil_make_socket_nonblocking(int fd)
{
    int flags;
    if ((flags = fcntl(fd, F_GETFL, NULL)) < 0) {
        event_warn("fcntl(%d, F_GETFL)", fd);
        return -1;
    }
    if (!(flags & O_NONBLOCK)) {
        if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
            event_warn("fcntl(%d, F_SETFL)", fd);
            return -1;
        }
    }
    return 0;
}


static int evutil_fast_socket_nonblocking(int fd)
{
    if (fcntl(fd, F_SETFL, O_NONBLOCK) == -1) {
        event_warn("fcntl(%d, F_SETFL)", fd);
        return -1;
    }
    return 0;
}

int evutil_make_listen_socket_reuseable(int sock)
{
    int one = 1;
    return setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (void*) &one, (socklen_t)sizeof(one));
}

int evutil_make_listen_socket_reuseable_port(int sock)
{
#if defined(SO_REUSEPORT)
    int one = 1;
	return setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, (void*) &one,(socklen_t)sizeof(one));
#else
    return 0;
#endif
}

int evutil_make_socket_closeonexec(int fd)
{
    int flags;
    if ((flags = fcntl(fd, F_GETFD, NULL)) < 0) {
        event_warn("fcntl(%d, F_GETFD)", fd);
        return -1;
    }
    if (!(flags & FD_CLOEXEC)) {
        if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1) {
            event_warn("fcntl(%d, F_SETFD)", fd);
            return -1;
        }
    }

    return 0;
}

static int evutil_fast_socket_closeonexec(int fd)
{
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) == -1) {
        event_warn("fcntl(%d, F_SETFD)", fd);
        return -1;
    }
    return 0;
}

int evutil_make_tcp_listen_socket_deferred(int sock)
{
#if defined(EVENT__HAVE_NETINET_TCP_H) && defined(TCP_DEFER_ACCEPT)
    int one = 1;

	/* TCP_DEFER_ACCEPT tells the kernel to call defer accept() only after data
	 * has arrived and ready to read */
	return setsockopt(sock, IPPROTO_TCP, TCP_DEFER_ACCEPT, &one,(socklen_t)sizeof(one));
#endif
    return 0;
}

int evutil_make_internal_pipe(int fd[2])
{
    //将第二个套接字设置为非阻塞有点微妙，鉴于我们在写入时忽略任何EAGAIN返回值，并且您不会通过任何方式做一个非阻塞的套接字。 但如果内核给我们EAGAIN，那么不需要再添加任何数据到缓冲区，因为主线程要么已经要唤醒和处理，要么已经唤醒，并在处理过程中
    if (evutil_socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0) {
        if (evutil_fast_socket_nonblocking(fd[0]) < 0 || evutil_fast_socket_nonblocking(fd[1]) < 0 ||
            evutil_fast_socket_closeonexec(fd[0]) < 0 || evutil_fast_socket_closeonexec(fd[1]) < 0)
        {
            close(fd[0]);
            close(fd[1]);
            fd[0] = fd[1] = -1;
            return -1;
        }
        return 0;
    }
    fd[0] = fd[1] = -1;
    return -1;
}

int evutil_socket(int domain, int type, int protocol)
{
    int r;
    r = socket(domain, type, protocol);
    if (r >= 0)
        return r;
    else if ((type & (SOCK_NONBLOCK|SOCK_CLOEXEC)) == 0)
        return -1;
    return r;//type& (SOCK_NONBLOCK|SOCK_CLOEXEC) == 1 ，并且r < 0
}

int evutil_accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags)
{
    int result;
    result = accept(sockfd, addr, addrlen);
    if (result < 0)
        return result;

    if (flags & SOCK_CLOEXEC) {
        if (evutil_fast_socket_closeonexec(result) < 0) {
            close(result);
            return -1;
        }
    }
    if (flags & SOCK_NONBLOCK) {
        if (evutil_fast_socket_nonblocking(result) < 0) {
            close(result);
            return -1;
        }
    }
    return result;
}

static int evutil_issetugid(void)
{
    if (getuid() != geteuid())
		return 1;
    if (getgid() != getegid())
		return 1;
    return 0;
}

const char *evutil_getenv(const char *varname)
{
    if (evutil_issetugid())
        return NULL;

    return getenv(varname);
}

#define MAX_SECONDS_IN_MSEC_LONG (((LONG_MAX) - 999) / 1000)

long evutil_tv_to_msec(const struct timeval *tv){
    if (tv->tv_usec > 1000000 || tv->tv_sec > MAX_SECONDS_IN_MSEC_LONG)
        return -1;

    return (tv->tv_sec * 1000) + ((tv->tv_usec + 999) / 1000);
}

void evutil_usleep(const struct timeval *tv){
    if(!tv)return ;

    struct timespec ts;
    ts.tv_sec = tv->tv_sec;
    ts.tv_nsec = tv->tv_usec*1000;

    nanosleep(&ts, NULL);
}

int evutil_socket_finished_connecting(int fd)
{
    int e;
    socklen_t elen = sizeof(e);

    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (void*)&e, &elen) < 0)
        return -1;

    if (e) {
        if (EVUTIL_ERR_CONNECT_RETRIABLE(e))
            return 0;
        errno = e;
        return -1;
    }

    return 1;
}

int evutil_socket_connect(int *fd_ptr, struct sockaddr *sa, int socklen)
{
    int made_fd = 0;

    if (*fd_ptr < 0) {
        if ((*fd_ptr = socket(sa->sa_family, SOCK_STREAM, 0)) < 0)
            goto err;
        made_fd = 1;
        if (evutil_make_socket_nonblocking(*fd_ptr) < 0) {
            goto err;
        }
    }

    if (connect(*fd_ptr, sa, socklen) < 0) {
        int e = errno;
        if (EVUTIL_ERR_CONNECT_RETRIABLE(e))
            return 0;
        if (EVUTIL_ERR_CONNECT_REFUSED(e))
            return 2;
        goto err;
    } else {
        return 1;
    }

err:
    if (made_fd) {
        close(*fd_ptr);
        *fd_ptr = -1;
    }
    return -1;
}
