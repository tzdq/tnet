#ifndef TNET_BUFFEREVENT_H
#define TNET_BUFFEREVENT_H

#include <sys/types.h>
#include <sys/time.h>
#include "util.h"
#include "event_struct.h"

struct event_base;
struct evbuffer;
struct sockaddr;

/** 这些标志作为参数传递给bufferevent的事件回调。*/
#define BEV_EVENT_READING	0x01	 /** 读时出错 */
#define BEV_EVENT_WRITING	0x02	 /** 写时出错 */
#define BEV_EVENT_EOF		0x10	 /** 到达文件末尾eof */
#define BEV_EVENT_ERROR	0x20     /** 遇到不可恢复的错误 */
#define BEV_EVENT_TIMEOUT	0x40	 /** 达到用户指定的超时 */
#define BEV_EVENT_CONNECTED	0x80 /** 连接操作完成。 */

struct bufferevent;
/* 对于bufferevent的读或写回调。当新数据到达输入缓冲区时并且可读数据的数量超出低水位线(默认为0)时读回调被触发。
   如果写缓冲区已满或低于其低水位线，则触发写回调。*/
typedef void (*bufferevent_data_cb)(struct bufferevent *bev, void *ctx);

/* 一个bufferevent的事件/错误回调。如果遇到EOF条件或其他不可恢复的错误，触发事件回调。*/
typedef void (*bufferevent_event_cb)(struct bufferevent *bev, short what, void *ctx);

struct event_watermark {
    size_t low;
    size_t high;
};

struct bufferevent {
    struct event_base *ev_base;

    //操作结构体，成员有一些函数指针。类似struct eventop结构体
    const struct bufferevent_ops *be_ops;

    struct event ev_read;//读事件event
    struct event ev_write;//写事件event

    struct evbuffer *input;//读缓冲区

    struct evbuffer *output; //写缓冲区

    struct event_watermark wm_read;//读水位
    struct event_watermark wm_write;//写水位

    bufferevent_data_cb readcb;//可读时的回调函数指针
    bufferevent_data_cb writecb;//可写时的回调函数指针
    bufferevent_event_cb errorcb;//错误发生时的回调函数指针
    void *cbarg;//回调函数的参数

    struct timeval timeout_read;//读事件event的超时值
    struct timeval timeout_write;//写事件event的超时值

    short enabled;//当前启用的事件：目前支持EV_READ和EV_WRITE。
};

/** 创建bufferevent时可以指定的选项 */
enum bufferevent_options {
    /** 如果设置，当bufferevent被释放时我们关闭底层文件描述符/ bufferevent /xx。 */
    BEV_OPT_CLOSE_ON_FREE = (1<<0),

    /** 如果设置，并且启用线程，在这个bufferevent上的操作受锁保护（线程安全） */
    BEV_OPT_THREADSAFE = (1<<1),

    /** 如果设置, 回调在事件循环中延迟运行 */
    BEV_OPT_DEFER_CALLBACKS = (1<<2),

    /* 如果设置，回调执行时没有锁在bufferevent上，这个操作需要BEV_OPT_DEFER_CALLBACKS被设置。*/
    BEV_OPT_UNLOCK_CALLBACKS = (1<<3)
};

/** 在现有套接字上创建一个新的套接字bufferevent，options是BEV_OPT_*  */
struct bufferevent *bufferevent_socket_new(struct event_base *base, int fd, int options);

int bufferevent_socket_connect(struct bufferevent *, struct sockaddr *, int);

//struct evdns_base;
//int bufferevent_socket_connect_hostname(struct bufferevent *, struct evdns_base *, int, const char *, int);

/**获取dns错误码*/
int bufferevent_socket_get_dns_error(struct bufferevent *bev);

/** 为一个特定的event_base分配一个bufferevent。*/
int bufferevent_base_set(struct event_base *base, struct bufferevent *bufev);

/** 返回一个bufferevent使用的event_base*/
struct event_base *bufferevent_get_base(struct bufferevent *bev);

/**  设置bufferevent的优先级，只支持socket bufferevent */
int bufferevent_priority_set(struct bufferevent *bufev, int pri);

/** 取消分配与bufferevent结构相关联的存储。*/
void bufferevent_free(struct bufferevent *bufev);

/** 修改bufferevent的回调函数*/
void bufferevent_setcb(struct bufferevent *bufev, bufferevent_data_cb readcb, bufferevent_data_cb writecb,
                       bufferevent_event_cb eventcb, void *cbarg);

/** 更改bufferevent关联的文件描述符。 不支持所有bufferevent类型。*/
int bufferevent_setfd(struct bufferevent *bufev, int fd);

/**  获取bufferevent关联的文件描述符，如果没有文件描述符关联，返回-1*/
int bufferevent_getfd(struct bufferevent *bufev);

/** 返回与bufferevent相关联的底层bufferevent（如果bufferevent是一个包装器），或者如果没有底层bufferevent则返回NULL。*/
struct bufferevent *bufferevent_get_underlying(struct bufferevent *bufev);

/** 向bufferevent中写入数据*/
int bufferevent_write(struct bufferevent *bufev, const void *data, size_t size);

/* 将数据从evbuffer写入缓冲区*/
int bufferevent_write_buffer(struct bufferevent *bufev, struct evbuffer *buf);

/**从bufferevent中读取数据*/
size_t bufferevent_read(struct bufferevent *bufev, void *data, size_t size);

/* 从bufferevent中读取数据存入到evbuffer，避免内存拷贝*/
int bufferevent_read_buffer(struct bufferevent *bufev, struct evbuffer *buf);

/**返回读写缓冲区，用户不能在缓冲区上设置回调*/
struct evbuffer *bufferevent_get_input(struct bufferevent *bufev);
struct evbuffer *bufferevent_get_output(struct bufferevent *bufev);

/** 启用bufferevent(添加事件)*/
int bufferevent_enable(struct bufferevent *bufev, short event);

/** 禁用bufferevent(删除事件)*/
int bufferevent_disable(struct bufferevent *bufev, short event);

/** 返回在给定的bufferevent上启用的事件。*/
short bufferevent_get_enabled(struct bufferevent *bufev);

/** 设置读写事件的超时 */
int bufferevent_set_timeouts(struct bufferevent *bufev, const struct timeval *timeout_read, const struct timeval *timeout_write);

/* 设置读写事件的水位。在输入时，缓冲区中至少存在低水位字节的数据，bufferevent调用用户读回调。 如果读取缓冲区数据超出了高水位，则bufferevent停止从网络读取。 在输出时，只要缓冲数据低于低水印，就会调用用户写回调。 写入此bufev的过滤器将尝试在高水印允许时不要向该缓冲区写入更多字节，除非是flush。
 */
void bufferevent_setwatermark(struct bufferevent *bufev, short events, size_t lowmark, size_t highmark);

/** 加锁，需要多线程支持*/
void bufferevent_lock(struct bufferevent *bufev);

/** 解锁，需要多线程支持*/
void bufferevent_unlock(struct bufferevent *bufev);

/** 传入过滤器的标志，让他们知道如何处理传入的数据。*/
enum bufferevent_flush_mode {
    BEV_NORMAL = 0,   /** usually set when processing data */
    BEV_FLUSH = 1,    /** 检查所有发送的数据. */
    BEV_FINISHED = 2 /** encountered EOF on read or done sending data */
};

int bufferevent_flush(struct bufferevent *bufev, short iotype, enum bufferevent_flush_mode mode);

#endif //TNET_BUFFEREVENT_H
