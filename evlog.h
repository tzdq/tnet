/*
 * Libevent在默认情况下，会将这些日志信息输出到终端上。
 * Libevent允许用户定制自己的日志回调函数。所有的日志函数在最后输出信息时，都会调用日志回调函数的。
 * 定制回调函数就像设置自己信号处理函数那样，设置一个日志回调函数。
 * 当有日志时，Libevent库就会调用这个日志回调函数
 */

#ifndef TNET_LOG_INTERNAL_H
#define TNET_LOG_INTERNAL_H

#include "event2/util.h"

/*
 * 主要针对GNU
 * __attribute__((format(printf, a, b)))作用：提示编译器，对函数的调用需要像printf一样，用对应的format字符串来check可变参数的数据类型
 * void print(int,char * format,...)__attribute__((format(printf, 2, 3))):表示print的入参第二个参数是format字符串，第三个参数是可变参数
 *
 * __attribute__((noreturn))作用：来生成优化的编译代码，调用了__attribute__(noreturn)的函数后，控制不会再返回caller
 */

#define EV_CHECK_FMT(a,b) __attribute__((format(printf, a, b)))
#define EV_NORETURN __attribute__((noreturn))

#define EVENT_ERR_ABORT_ ((int)0xdeaddead)

void event_err(int eval,const char *fmt,...)EV_CHECK_FMT(2,3) EV_NORETURN;
void event_warn(const char *fmt, ...) EV_CHECK_FMT(1,2);
void event_sock_err(int eval, int sock, const char *fmt, ...) EV_CHECK_FMT(3,4) EV_NORETURN;
void event_sock_warn(int sock, const char *fmt, ...) EV_CHECK_FMT(2,3);
void event_errx(int eval, const char *fmt, ...) EV_CHECK_FMT(2,3) EV_NORETURN;
void event_warnx(const char *fmt, ...) EV_CHECK_FMT(1,2);
void event_msgx(const char *fmt, ...) EV_CHECK_FMT(1,2);
void event_debugx_(const char *fmt, ...) EV_CHECK_FMT(1,2);

void event_logv(int severity,const char* errstr,const char *fmt,va_list ap)EV_CHECK_FMT(3,0);

//#define event_debug(x) event_debugx_ x
#define event_debug(x) 0
#undef EV_CHECK_FMT

#endif //TNET_LOG_INTERNAL_H
