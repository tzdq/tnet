#ifndef TNET_EVBUFFER_INTERNAL_H
#define TNET_EVBUFFER_INTERNAL_H

#include "event/util.h"
#include "evutil.h"
#include "defer.h"
#include "sys/queue.h"
#include "event/buffer.h"

/* 回调从不延迟的标志*/
#define EVBUFFER_CB_NODEFER  2

/* chain分配的最小数值*/
#define MIN_BUFFER_SIZE 512

//当evbuffer添加或删除字节时，将调用回调函数
struct evbuffer_cb_entry {
    TAILQ_ENTRY(evbuffer_cb_entry) next;
    /* 调用此回调时调用的回调函数，如果flags被设置为EVBUFFER_CB_OBSOLETE，使用cb_obsolete，否则使用cb_func*/
    union {
        evbuffer_cb_func cb_func;
        evbuffer_cb cb_obsolete;
    } cb;
    /** 回调函数的参数 */
    void *cbarg;
    /** 回调函数的当前标志 */
	uint32_t flags;
};

struct bufferevent;
struct evbuffer_chain;
struct evbuffer {
    struct evbuffer_chain *first; /** 这个缓冲区链中的第一个链元素*/
    struct evbuffer_chain *last;  /** 这个缓冲区链中的最后一个链元素*/

    /*
     * 这是一个二级指针。使用*last_with_datap时，指向的是链表中最后一个有数据的evbuffer_chain。 一开始buffer->last_with_datap = &buffer->first;此时first为NULL。所以当链表没有节点时*last_with_datap为NULL。当只有一个节点时*last_with_datap就是first。
     */
    struct evbuffer_chain **last_with_datap;

    size_t total_len;      /* 存储在所有链中的总字节数。*/
    size_t n_add_for_cb;   /*上次调用回调函数后添加到缓冲区的字节数。 */
    size_t n_del_for_cb;   /* 上次调用回调函数后从缓冲区中删除的字节数。*/

    void *lock;      /* 用于控制对该缓冲区的访问的互斥锁。*/
    unsigned own_lock : 1;/* 当释放evbuffer时，如果需要释放lock，设置为true */

    unsigned freeze_start : 1; /** 如果我们不允许在缓冲区的前面进行更改(drains or prepends)，设置为true */
    unsigned freeze_end : 1;/** 如果我们不允许更改缓冲区的末尾(appends，禁止追加)，设置为true */
    unsigned deferred_cbs : 1;/* 如果evbuffer的回调在缓冲区更改后不会立即被调用，而是从event_base的循环中延迟调用，设置为True。*/

	uint32_t flags; /** 0或者EVBUFFER_FLAG_*  */

    struct deferred_cb_queue *cb_queue;/** 延迟调用回调函数队列. */

    int refcnt;/* evbuffer的引用计数，如果引用计数为0，buffer被销毁 */

    struct deferred_cb deferred;/* 延迟回调的回调函数。*/

    TAILQ_HEAD(evbuffer_cb_queue, evbuffer_cb_entry) callbacks;/* 回调函数的队列*/

    struct bufferevent *parent;/* 这个evbuffer所属的父级bufferevent对象。 如果evbuffer独立，则为NULL。*/
};

#define EVBUFFER_CHAIN_MAX ((size_t)EV_SSIZE_MAX)

/** evbuffer中的一个项目 */
struct evbuffer_chain {
    struct evbuffer_chain *next;/** 指向链中的下一个缓冲区 */
    size_t buffer_len; /** buffer的大小   */
	ssize_t misalign; /** 错开不使用的空间。该成员的值一般等于0   */
    size_t off;/*evbuffer_chain已存数据的字节数,所以要从buffer + misalign + off的位置开始写入数据 */

    /** 需要特殊处理这个链的标志 */
    unsigned flags;
#define EVBUFFER_MMAP		    0x0001	/**< 缓冲区中的内存是共享内存 */
#define EVBUFFER_SENDFILE	    0x0002	/**< 用于sendfile的链 */
#define EVBUFFER_REFERENCE	0x0004	/**< 链具有内存清理函数 */
#define EVBUFFER_IMMUTABLE	0x0008	/**< 只读链 */
    /** 一个链不能被重新分配或释放，或者它的内容被移动，直到链被解除. */
#define EVBUFFER_MEM_PINNED_R	0x0010
#define EVBUFFER_MEM_PINNED_W	0x0020
#define EVBUFFER_MEM_PINNED_ANY (EVBUFFER_MEM_PINNED_R|EVBUFFER_MEM_PINNED_W)
    /** 一条链应该被释放，但不能被释放，直到它被解除固定。 */
#define EVBUFFER_DANGLING	    0x0040

    /** 通常指向属于该缓冲区的读写存储器，作为evbuffer_chain分配的一部分分配。 对于mmap，这可以是只读缓冲区，EVBUFFER_IMMUTABLE将在flags中设置。 对于sendfile，它可能指向NULL */
    unsigned char *buffer;
};

/* 当前用于mmap和sendfile */
struct evbuffer_chain_fd {
	int fd;	/** 与链块关联的fd*/
};

/** 回调用于引用缓冲区; 让我们知道当我们完成它时该怎么办。 */
struct evbuffer_chain_reference {
	evbuffer_ref_cleanup_cb cleanupfn;
	void *extra;
};

#define EVBUFFER_CHAIN_SIZE sizeof(struct evbuffer_chain)

/** 返回一个指针，指向与evbuffer一起分配的额外数据。 */
#define EVBUFFER_CHAIN_EXTRA(t, c) (t *)((struct evbuffer_chain *)(c) + 1)

//断言加锁
#define ASSERT_EVBUFFER_LOCKED(buffer)	EVLOCK_ASSERT_LOCKED((buffer)->lock)

//加锁操作
#define EVBUFFER_LOCK(buffer)						\
	do {								\
		EVLOCK_LOCK((buffer)->lock, 0);				\
	} while (0)

//解锁操作
#define EVBUFFER_UNLOCK(buffer)						\
	do {								\
		EVLOCK_UNLOCK((buffer)->lock, 0);			\
	} while (0)

#define EVBUFFER_LOCK2(buffer1, buffer2)				\
	do {								\
		EVLOCK_LOCK2((buffer1)->lock, (buffer2)->lock, 0, 0);	\
	} while (0)

#define EVBUFFER_UNLOCK2(buffer1, buffer2)				\
	do {								\
		EVLOCK_UNLOCK2((buffer1)->lock, (buffer2)->lock, 0, 0);	\
	} while (0)

/** 增加evbuffer的引用计数，并且加锁 */
void evbuffer_incref_and_lock(struct evbuffer *buf);

/** evbuffer_free的核心函数，要求我们在缓冲区上加锁，解锁并释放缓冲区。 */
void evbuffer_decref_and_unlock(struct evbuffer *buffer);

/* evbuffer调用回调函数 */
void evbuffer_invoke_callbacks(struct evbuffer *buf);

/* 设置buf的父bufferevent对象*/
void evbuffer_set_parent(struct evbuffer *buf, struct bufferevent *bev);

//用最多不超过n个节点就提供datlen大小的空闲空间
int evbuffer_expand_fast(struct evbuffer *buf, size_t datlen, int n);

int evbuffer_read_setup_vecs(struct evbuffer *buf, ssize_t howmuch, struct iovec *vecs, int n_vecs_avail,
						  struct evbuffer_chain ***chainp, int exact);
#endif //TNET_EVBUFFER_INTERNAL_H
