#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include "event/event.h"
#include "event/util.h"
#include "evlog.h"

//日志操作的核心，仅仅只是把日志简单的输出到终端
static void event_log(int severity,const char *msg);
static void event_exit(int errcode)EV_NORETURN;

//全局错误回调函数指针
static event_fatal_cb fatal_fn = NULL;

void event_set_fatal_callback(event_fatal_cb cb){
    fatal_fn = cb;
}

//程序退出操作
static void event_exit(int errcode){
    if(fatal_fn){
        fatal_fn(errcode);
        exit(errcode);
    }
    else if(errcode == EVENT_ERR_ABORT_)
        abort();
    else exit(errcode);
}

//错误日志函数：eval是错误代码
void event_err(int eval,const char *fmt,...){
    va_list ap;
    va_start(ap,fmt);
    event_logv(EVENT_LOG_ERR,strerror(errno),fmt,ap);
    va_end(ap);
    event_exit(eval);
}

//警告日志函数
void event_warn(const char *fmt, ...){
    va_list ap;
    va_start(ap,fmt);
    event_logv(EVENT_LOG_WARN,strerror(errno),fmt,ap);
    va_end(ap);
}

//sock错误日志函数
void event_sock_err(int eval, int sock, const char *fmt, ...){
    va_list ap;
    int err = errno;
    va_start(ap,fmt);
    event_logv(EVENT_LOG_ERR,strerror(err),fmt,ap);
    va_end(ap);
    event_exit(eval);
}

//sock警告日志函数
void event_sock_warn(int sock, const char *fmt, ...){
    va_list ap;
    int err = errno;
    va_start(ap,fmt);
    event_logv(EVENT_LOG_WARN,strerror(err),fmt,ap);
    va_end(ap);
}

//错误日志函数，无错误信息字符串
void event_errx(int eval, const char *fmt, ...) {
    va_list ap;
    va_start(ap,fmt);
    event_logv(EVENT_LOG_ERR,NULL,fmt,ap);
    va_end(ap);
    event_exit(eval);
}

void event_warnx(const char *fmt, ...){
    va_list ap;
    va_start(ap,fmt);
    event_logv(EVENT_LOG_WARN,NULL,fmt,ap);
    va_end(ap);
}

void event_msgx(const char *fmt, ...){
    va_list ap;
    va_start(ap,fmt);
    event_logv(EVENT_LOG_MSG,NULL,fmt,ap);
    va_end(ap);
}

//调试日志函数
void event_debugx_(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    event_logv(EVENT_LOG_DEBUG, NULL, fmt, ap);
    va_end(ap);
}

void event_logv(int severity,const char* errstr,const char *fmt,va_list ap){
    char buf[1024] = {0};
    size_t len = 0;

    if(fmt != NULL)
        evutil_vsnprintf(buf,sizeof(buf),fmt,ap);
    else buf[0] = '\0';

    if(errstr){
        len = strlen(buf);
        if(len < sizeof(buf) - 3){
            evutil_snprintf(buf+len,sizeof(buf) - len,": %s",errstr);
        }
    }

    event_log(severity,buf);
}

//全局日志回调函数指针
static event_log_cb log_fn = NULL;

//设置日志回调函数
void event_set_log_callback(event_log_cb cb)
{
    log_fn = cb;
}

//日志核心处理函数
static void event_log(int severity,const char *msg){
    if(log_fn)
        log_fn(severity,msg);
    else{
        const char* severity_str;
        switch (severity){
            case EVENT_LOG_DEBUG:
                severity_str = "debug";
                break;
            case EVENT_LOG_MSG:
                severity_str = "msg";
                break;
            case EVENT_LOG_ERR:
                severity_str = "err";
                break;
            case EVENT_LOG_WARN:
                severity_str = "warn";
                break;
            default:
                severity_str = "unknown";
                break;
        }
        (void)fprintf(stderr,"[%s] %s\n",severity_str, msg);
    }
}