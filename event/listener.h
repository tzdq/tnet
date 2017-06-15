#ifndef TNET_LISTENER_H
#define TNET_LISTENER_H

#include "event.h"

struct sockaddr;
struct evconnlistener;

/* 新连接到来时的回调函数*/
typedef void (*evconnlistener_cb)(struct evconnlistener *, int, struct sockaddr *, int socklen, void *);

/** listener遇到不可重复错误时调用的回调。*/
typedef void (*evconnlistener_errorcb)(struct evconnlistener *, void *);

#define LEV_OPT_LEAVE_SOCKETS_BLOCKING	(1u<<0) /*对传入的socket在传入回调前我们不能设置为非阻塞*/
#define LEV_OPT_CLOSE_ON_FREE		(1u<<1) /** 释放listener时应该关闭底层socket. */
#define LEV_OPT_CLOSE_ON_EXEC		(1u<<2) /** 设置close-on-exec标志 */
#define LEV_OPT_REUSEABLE		(1u<<3) /*套接字已关闭并且我们在同一个端口上再次侦听时应该禁用超时*/
#define LEV_OPT_THREADSAFE	(1u<<4) /** 线程安全 */
#define LEV_OPT_DISABLED		(1u<<5) /** 禁用状态下创建侦听器，使用evconnlistener_enable（）稍后启用它 */
#define LEV_OPT_DEFERRED_ACCEPT (1u<<6) /* listener应该延迟accept()直到数据可用,只能被evconnlistener_new_bind使用 */
#define LEV_OPT_REUSEABLE_PORT  (1u<<7) /** 允许多个线程(进程)绑定同一端口 */

/**
   分配一个新的evconnlistener对象来监听给定fd的TCP连接

   @param base The event base to associate the listener with.
   @param cb A callback to be invoked when a new connection arrives.  If the
      callback is NULL, the listener will be treated as disabled until the
      callback is set.
   @param ptr A user-supplied pointer to give to the callback.
   @param flags Any number of LEV_OPT_* flags
   @param backlog Passed to the listen() call to determine the length of the
      acceptable connection backlog.  Set to -1 for a reasonable default.
      Set to 0 if the socket is already listening.
   @param fd The file descriptor to listen on.  It must be a nonblocking
      file descriptor, and it should already be bound to an appropriate
      port and address.
*/
struct evconnlistener *evconnlistener_new(struct event_base *base, evconnlistener_cb cb, void *ptr,
                                          unsigned flags, int backlog, int fd);

/**
   分配一个新的evconnlistener对象来监听给定地址上的TCP连接。

   @param base The event base to associate the listener with.
   @param cb A callback to be invoked when a new connection arrives. If the
      callback is NULL, the listener will be treated as disabled until the
      callback is set.
   @param ptr A user-supplied pointer to give to the callback.
   @param flags Any number of LEV_OPT_* flags
   @param backlog Passed to the listen() call to determine the length of the
      acceptable connection backlog.  Set to -1 for a reasonable default.
   @param addr The address to listen for connections on.
   @param socklen The length of the address.
 */
struct evconnlistener *evconnlistener_new_bind(struct event_base *base, evconnlistener_cb cb, void *ptr,
        unsigned flags, int backlog, const struct sockaddr *sa, int socklen);

/** Disable and deallocate an evconnlistener.*/
void evconnlistener_free(struct evconnlistener *lev);

/* 重新启用一个已经不可用的evconnlistener */
int evconnlistener_enable(struct evconnlistener *lev);

/**  停止监听evconnlistener上的连接。 */
int evconnlistener_disable(struct evconnlistener *lev);

/** Return an evconnlistener's associated event_base. */
struct event_base *evconnlistener_get_base(struct evconnlistener *lev);

/** Return the socket that an evconnlistner is listening on. */
int evconnlistener_get_fd(struct evconnlistener *lev);

/** Change the callback on the listener to cb and its user_data to arg.*/
void evconnlistener_set_cb(struct evconnlistener *lev, evconnlistener_cb cb, void *arg);

/** Set an evconnlistener's error callback. */
void evconnlistener_set_error_cb(struct evconnlistener *lev, evconnlistener_errorcb errorcb);
#endif //TNET_LISTENER_H
