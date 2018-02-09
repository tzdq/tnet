#ifndef TNET_UTIL_H
#define TNET_UTIL_H

#include <sys/time.h>
#include <sys/types.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <inttypes.h>
#include <netdb.h>
#include <errno.h>
#include <sys/socket.h>
#include <time.h>

//数据类型范围常量定义
#define EV_UINT64_MAX ((((uint64_t)0xffffffffUL) << 32) | 0xffffffffUL)
#define EV_INT64_MAX  ((((int64_t) 0x7fffffffL) << 32) | 0xffffffffL)
#define EV_INT64_MIN  ((-EV_INT64_MAX) - 1)
#define EV_UINT32_MAX ((uint32_t)0xffffffffUL)
#define EV_INT32_MAX  ((int32_t) 0x7fffffffL)
#define EV_INT32_MIN  ((-EV_INT32_MAX) - 1)
#define EV_UINT16_MAX ((uint16_t)0xffffUL)
#define EV_INT16_MAX  ((int16_t) 0x7fffL)
#define EV_INT16_MIN  ((-EV_INT16_MAX) - 1)
#define EV_UINT8_MAX  255
#define EV_INT8_MAX   127
#define EV_INT8_MIN   ((-EV_INT8_MAX) - 1)

#define EV_SIZE_MAX EV_UINT32_MAX
#define EV_SSIZE_MAX EV_INT32_MAX

//格式化相关
int evutil_vsnprintf(char *buf, size_t buflen, const char *format, va_list ap)__attribute__((format(printf, 3, 0)));
int evutil_snprintf(char *buf, size_t buflen, const char *format, ...)__attribute__((format(printf, 3, 4)));

//时间相关
/** Return true iff the tvp is related to uvp according to the relational
 * operator cmp.  Recognized values for cmp are ==, <=, <, >=, and >. */
#define	evutil_timercmp(tvp, uvp, cmp)					\
	(((tvp)->tv_sec == (uvp)->tv_sec) ?				\
	 ((tvp)->tv_usec cmp (uvp)->tv_usec) :				\
	 ((tvp)->tv_sec cmp (uvp)->tv_sec))

#define evutil_timeradd(tvp, uvp, vvp)					\
	do {								\
		(vvp)->tv_sec = (tvp)->tv_sec + (uvp)->tv_sec;		\
		(vvp)->tv_usec = (tvp)->tv_usec + (uvp)->tv_usec;       \
		if ((vvp)->tv_usec >= 1000000) {			\
			(vvp)->tv_sec++;				\
			(vvp)->tv_usec -= 1000000;			\
		}							\
	} while (0)

#define	evutil_timersub(tvp, uvp, vvp)					\
	do {								\
		(vvp)->tv_sec = (tvp)->tv_sec - (uvp)->tv_sec;		\
		(vvp)->tv_usec = (tvp)->tv_usec - (uvp)->tv_usec;	\
		if ((vvp)->tv_usec < 0) {				\
			(vvp)->tv_sec--;				\
			(vvp)->tv_usec += 1000000;			\
		}							\
	} while (0)

#define	evutil_timerclear(tvp)	(tvp)->tv_sec = (tvp)->tv_usec = 0

//socket相关
//closeexec标志打开文件
int evutil_open_closeonexec_(const char *pathname, int flags, unsigned mode);

//创建全双工管道
int evutil_socketpair(int d, int type, int protocol, int sv[2]);

//设置sockfd为非阻塞
int evutil_make_socket_nonblocking(int sock);

//设置监听套接字的地址可复用
int evutil_make_listen_socket_reuseable(int sock);

//设置监听套接字的端口可复用
int evutil_make_listen_socket_reuseable_port(int sock);

//设置套接字的closeexec属性
int evutil_make_socket_closeonexec(int sock);

int evutil_make_tcp_listen_socket_deferred(int sock);
#endif //TNET_UTIL_H
