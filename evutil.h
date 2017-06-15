#ifndef TNET_UTIL_INTERNAL_H
#define TNET_UTIL_INTERNAL_H

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include "evlog.h"
#include "event/util.h"

#if EAGAIN == EWOULDBLOCK
#define EVUTIL_ERR_IS_EAGAIN(e) ((e) == EAGAIN)
#else
#define EVUTIL_ERR_IS_EAGAIN(e) ((e) == EAGAIN || (e) == EWOULDBLOCK)
#endif

/* True iff e is an error that means a read/write operation can be retried. */
#define EVUTIL_ERR_RW_RETRIABLE(e) ((e) == EINTR || EVUTIL_ERR_IS_EAGAIN(e))

/* True iff e is an error that means an connect can be retried. */
#define EVUTIL_ERR_CONNECT_RETRIABLE(e)	((e) == EINTR || (e) == EINPROGRESS)

/* True iff e is an error that means a accept can be retried. */
#define EVUTIL_ERR_ACCEPT_RETRIABLE(e) ((e) == EINTR || EVUTIL_ERR_IS_EAGAIN(e) || (e) == ECONNABORTED)

/* True iff e is an error that means the connection was refused */
#define EVUTIL_ERR_CONNECT_REFUSED(e) ((e) == ECONNREFUSED)

/*
 * __builtin_expect(exp,c) : GCC 帮助程序员处理分支预测，优化程序。
 *    参数 exp 为一个整型表达式，这个内建函数的返回值也是这个 exp ， c 为一个编译期常量。
 *    函数的语义是：你期望 exp 表达式的值等于常量 c ，从而 GCC 为你优化程序，将符合这个条件的分支放在合适的地方。
      因为这个函数只提供了整型表达式，所以如果你要优化其他类型的表达式，可以采用指针的形式。

 * likely 和 unlikely 是 gcc 扩展的跟处理器相关的宏：
	#define  likely(x)        __builtin_expect(!!(x), 1)
	#define  unlikely(x)      __builtin_expect(!!(x), 0)
 	引入likely 和 unlikely的目的是增加条件分支预测的准确性，cpu 会提前装载后面的指令，遇到条件转移指令时会提前预测并装载某个分 支的指令。unlikely 表示你可以确认该条件是极少发生的，相反 likely 表示该条件多数情况下会发生。编译器会产生相应的代码来优化 cpu 执行效率。

 *	if(likely(value)) 等价于 if(value)
    if(unlikely(value)) 也等价于 if(value)

    目前Linux中使用_G_BOOLEAN_EXPR(expr) 代替了 !!(expr)
 */
#define EVUTIL_UNLIKELY(p) __builtin_expect(!!(p),0)
#define EVUTIL_LIKELY(p)   __builtin_expect(!!(p),1)

/* Replacement for assert() that calls event_errx on failure. */
#define EVUTIL_ASSERT(cond)						\
	do {								\
		if (EVUTIL_UNLIKELY(!(cond))) {				\
			event_errx(EVENT_ERR_ABORT_,"%s:%d: Assertion %s failed in %s",__FILE__,__LINE__,#cond,__func__);		\
			(void)fprintf(stderr,"%s:%d: Assertion %s failed in %s",__FILE__,__LINE__,#cond,__func__);		\
			abort();					\
		}							\
	} while (0)
#define EVUTIL_FAILURE_CHECK(cond) EVUTIL_UNLIKELY(cond)

#define EV_SOCK_FMT "%d"

/* 如果我们知道给定的指针指向一个结构中的一个字段，则返回一个指向结构本身的指针。 用于实现我们的半拷贝 */
#define EVUTIL_UPCAST(ptr, type, field) ((type *)(((char*)(ptr)) - offsetof(type, field)))

const char *evutil_getenv(const char *name);

//创建管道
int evutil_make_internal_pipe(int fd[2]);
int evutil_socket(int domain, int type, int protocol);
int evutil_accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags);
int evutil_socket_connect(int *fd_ptr, struct sockaddr *sa, int socklen);

/*检查我们调用connect（）的socket是否完成连接。返回1为连接，0为尚未，-1为错误。 在错误的情况下，将当前套接字errno设置为连接操作期间发生的错误。*/
int evutil_socket_finished_connecting(int fd);

/*
 * 主要用于记录Libevent的时间相关的API
 * CLOCK_MONOTONIC是monotonic time，而CLOCK_REALTIME是wall time。
 * monotonic time是单调时间，它指的是系统启动以后流逝的时间，由变量jiffies来记录的。
 * 系统每次启动时jiffies初始化为0，每来一个timer interrupt，jiffies加1，它代表系统启动后流逝的tick数。jiffies一定是单调递增的。
 * wall time是挂钟时间，它指的是现实的时间，由变量xtime来记录的。
 * 系统每次启动时将CMOS上的RTC时间读入xtime，这个值是"自1970-01-01起经历的秒数、本秒中经历的纳秒数"，每来一个timer interrupt，需要去更新xtime。
 *
 * int clock_gettime(clockid_t clk_id, struct timespect *tp);
 * clockid_t用于指定计时时钟的类型，有如下常用值：
 *   CLOCK_REALTIME
     CLOCK_MONOTONIC
     CLOCK_PROCESS_CPUTIME_ID：CPU的每一个进程提供的高精度定时器
     CLOCK_THREAD_CPUTIME_ID： CPU的每一个线程提供的高精度定时器
   timespect用来存储当前的时间
   成功返回0，失败返回-1

 * time:  函数返回自1970年以来的秒数
 * gettimeofday: 返回自1970年以来的秒数和微秒数,易受ntp影响
*/
long evutil_tv_to_msec(const struct timeval *tv);
void evutil_usleep(const struct timeval *tv);

#endif //TNET_UTIL_INTERNAL_H
