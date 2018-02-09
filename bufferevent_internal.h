#ifndef TNET_BUFFEREVENT_INTERNAL_H
#define TNET_BUFFEREVENT_INTERNAL_H

#include "event2/util.h"
#include "defer.h"
#include "evthread.h"
#include "event2/thread.h"
#include "event2/bufferevent.h"

/** bufferevent公共部分 */
struct bufferevent_private {
    /** 底层的bufferevent结构 */
    struct bufferevent bev;

    /** 设置input evbuffer的高水位时，需要一个evbuffer回调函数配合工作 */
    struct evbuffer_cb_entry *read_watermarks_cb;

    /* 锁是Libevent自动分配的，还是用户分配的,设置为1，释放bufferevent时需要释放锁 */
    unsigned own_lock : 1;

    /*如果我们延迟回调并且读事件回调待处理*/
    unsigned readcb_pending : 1;

    /*如果我们延迟回调并且写事件回调待处理*/
    unsigned writecb_pending : 1;

    /**这个socket是否处理正在连接服务器状态 . */
    unsigned connecting : 1;

    /** 标志连接被拒绝  */
    unsigned connection_refused : 1;

    /*如果我们延迟回调并且事件回调待处理，设置为待处理的事件*/
    short eventcb_pending;

    /** 如果设置，读操作将暂停，直到一个或多个条件结束。 参阅上面的BEV_SUSPEND_ *标志. */
    uint16_t read_suspended;

    /** 如果设置，写操作将暂停，直到一个或多个条件结束。 参阅上面的BEV_SUSPEND_ *标志. */
    uint16_t write_suspended;

    /* 如果我们有延迟回调并且事件回调待处理，则设置为当前套接字errno。*/
    int errno_pending;

    /** 用于bufferevent_socket_connect_hostname的DNS错误码 */
    int dns_error;

    /** 延迟回调函数 */
    struct deferred_cb deferred;

    /** bufferevent的构造选项 */
    enum bufferevent_options options;

    /** 当前这个bufferevent的引用计数 */
    int refcnt;

    /** 锁变量，inbuf和outbuf共享.如果为NULL，锁不可用 */
    void *lock;
};

/** 控制回调的可能操作。 */
enum bufferevent_ctrl_op {
    BEV_CTRL_SET_FD,
    BEV_CTRL_GET_FD,
    BEV_CTRL_GET_UNDERLYING,
    BEV_CTRL_CANCEL_ALL
};

/** 控制回调的可能数据类型 */
union bufferevent_ctrl_data {
    void *ptr;
    int fd;
};

struct bufferevent_ops {
    const char *type;//类型名称

    off_t mem_offset;//成员bev的偏移量

    //启动。将event加入到event_base中
    int (*enable)(struct bufferevent *, short);

    //关闭。将event从event_base中删除
    int (*disable)(struct bufferevent *, short);
    //销毁
    void (*destruct)(struct bufferevent *);
    //调整event的超时值
    int (*adj_timeouts)(struct bufferevent *);
    /** Called to flush data. */
    int (*flush)(struct bufferevent *, short, enum bufferevent_flush_mode);
    //获取成员的值。具体看实现
    int (*ctrl)(struct bufferevent *, enum bufferevent_ctrl_op, union bufferevent_ctrl_data *);
};

/* 这些标志是我们可能拒绝启用读写的原因。*/
/* On a all bufferevents, for reading: used when we have read up to the watermark value.*/
#define BEV_SUSPEND_WM 0x01

extern const struct bufferevent_ops bufferevent_ops_socket;

/** 初始化bufferevent的公共数据结构部分 */
int bufferevent_init_common(struct bufferevent_private *, struct event_base *,
                            const struct bufferevent_ops *,    enum bufferevent_options options);

/** 设置bufferevent上的锁。 如果设置了锁定，使用，否则，使用新的锁。 */
int bufferevent_enable_locking(struct bufferevent *bufev, void *lock);

/** 删除在bufev的引用计数，必要时释放，否则解锁它。 如果它释放了bufferevent，则返回1。 */
int bufferevent_decref_and_unlock(struct bufferevent *bufev);

/** 锁定bufev并增加其引用计数，并解锁。 */
void bufferevent_incref_and_lock(struct bufferevent *bufev);

/** 增加对bufev的引用计数 */
void bufferevent_incref(struct bufferevent *bufev);

/** 减少对bufev的引用计数。 如果它释放了bufferevent，则返回1。*/
int bufferevent_decref(struct bufferevent *bufev);

/** 添加事件 */
int bufferevent_add_event(struct event *ev, const struct timeval *tv);

//读写事件挂起/接触挂起，挂起后将停止读写，直到挂起接触，读写挂起与高水位有关
void bufferevent_suspend_read(struct bufferevent *bufev, uint16_t what);
void bufferevent_unsuspend_read(struct bufferevent *bufev, uint16_t what);
void bufferevent_suspend_write(struct bufferevent *bufev, uint16_t what);
void bufferevent_unsuspend_write(struct bufferevent *bufev, uint16_t what);

/*如果回调延迟执行并且我们有读/写/错误回调函数，读/写/错误回调处于待处理状态，延迟执行，否则执行readcb/writecb/eventcb*/
void bufferevent_run_readcb(struct bufferevent *bufev);
void bufferevent_run_writecb(struct bufferevent *bufev);
void bufferevent_run_eventcb(struct bufferevent *bufev, short what);

#define BEV_UPCAST(b) EVUTIL_UPCAST((b), struct bufferevent_private, bev)

/** 加锁 */
#define BEV_LOCK(b) do {						\
		struct bufferevent_private *locking =  BEV_UPCAST(b);	\
		EVLOCK_LOCK(locking->lock, 0);				\
	} while (0)

/** 解锁 */
#define BEV_UNLOCK(b) do {						\
		struct bufferevent_private *locking =  BEV_UPCAST(b);	\
		EVLOCK_UNLOCK(locking->lock, 0);			\
	} while (0)

#endif //TNET_BUFFEREVENT_INTERNAL_H
