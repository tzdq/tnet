#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <limits.h>
#include "event/event.h"
#include "event/buffer.h"
#include "event/bufferevent.h"
#include "event/thread.h"
#include "evlog.h"
#include "sys/queue.h"
#include "evmemory.h"
#include "evutil.h"
#include "evthread.h"
#include "evbuffer.h"
#include "bufferevent_internal.h"

static int use_sendfile  = 1;
static int use_mmap = 1;

/* 用户可选择的回调标志. */
#define EVBUFFER_CB_USER_FLAGS	    0xffff
/* 所有内部使用的标志 */
#define EVBUFFER_CB_INTERNAL_FLAGS  0xffff0000
/* 回调使用cb_obsolete函数指针的标志 */
#define EVBUFFER_CB_OBSOLETE	       0x00040000

#define DEFAULT_WRITE_IOVEC 128

//判断链节点是否允许修改数据，被固定就不允许修改
#define CHAIN_PINNED(ch)  (((ch)->flags & EVBUFFER_MEM_PINNED_ANY) != 0)
#define CHAIN_PINNED_R(ch)  (((ch)->flags & EVBUFFER_MEM_PINNED_R) != 0)

//获取可写地址
#define CHAIN_SPACE_PTR(ch) ((ch)->buffer + (ch)->misalign + (ch)->off)

//可写地址的长度
#define CHAIN_SPACE_LEN(ch) ((ch)->flags & EVBUFFER_IMMUTABLE ? 0 : (ch)->buffer_len - ((ch)->misalign + (ch)->off))

static void evbuffer_deferred_callback(struct deferred_cb *cb, void *arg);
static void evbuffer_chain_align(struct evbuffer_chain *chain);
static int evbuffer_chain_should_realign(struct evbuffer_chain *chain, size_t datalen);
static struct evbuffer_chain *evbuffer_expand_singlechain(struct evbuffer *buf, size_t datlen);
static int evbuffer_ptr_memcmp(const struct evbuffer *buf, const struct evbuffer_ptr *pos, const char *mem, size_t len);

//申请和分配内存给链节点chain，size为需要分配的大下
static struct evbuffer_chain *evbuffer_chain_new(size_t size)
{
    struct evbuffer_chain *chain;
    size_t to_alloc;

    if (size > EVBUFFER_CHAIN_MAX - EVBUFFER_CHAIN_SIZE)
        return (NULL);

    /*evbuffer_chain结构体和buffer是一起分配的,也就是说他们是存放在同一块内存中*/
    size += EVBUFFER_CHAIN_SIZE;

    /* 选择合适的大小 */
    if (size < EVBUFFER_CHAIN_MAX / 2) {
        to_alloc = MIN_BUFFER_SIZE;
        while (to_alloc < size) {
            to_alloc <<= 1;
        }
    }
    else {
        to_alloc = size;
    }

    if ((chain = (struct evbuffer_chain *)mm_malloc(to_alloc)) == NULL)
        return (NULL);

    //只需初始化最前面的结构体部分即可
    memset(chain, 0, EVBUFFER_CHAIN_SIZE);

    chain->buffer_len = to_alloc - EVBUFFER_CHAIN_SIZE;

    /* 这样我们可以将缓冲区操作到不同的地址，mmap是必需的。宏的作用就是返回，chain + sizeof(evbuffer_chain) 的内存地址。  其效果就是buffer指向的内存刚好是在evbuffer_chain的后面。*/
    chain->buffer = EVBUFFER_CHAIN_EXTRA(u_char, chain);

    return (chain);
}

//释放当个链节点chain
static inline void evbuffer_chain_free(struct evbuffer_chain *chain)
{
    //如果链块被固定，设置flag为EVBUFFER_DANGLING
    if (CHAIN_PINNED(chain)) {
        chain->flags |= EVBUFFER_DANGLING;
        return;
    }
    //依次判断flag，根绝flag释放链节点
    if (chain->flags & (EVBUFFER_MMAP|EVBUFFER_SENDFILE| EVBUFFER_REFERENCE)) {
        if (chain->flags & EVBUFFER_REFERENCE) {
            struct evbuffer_chain_reference *info = EVBUFFER_CHAIN_EXTRA(struct evbuffer_chain_reference, chain);
            if (info->cleanupfn)
                (*info->cleanupfn)(chain->buffer, chain->buffer_len, info->extra);
        }
        if (chain->flags & EVBUFFER_MMAP) {
			struct evbuffer_chain_fd *info = EVBUFFER_CHAIN_EXTRA(struct evbuffer_chain_fd, chain);
			if (munmap(chain->buffer, chain->buffer_len) == -1)
				event_warn("%s: munmap failed", __func__);
			if (close(info->fd) == -1)
				event_warn("%s: close(%d) failed", __func__, info->fd);
		}
        if (chain->flags & EVBUFFER_SENDFILE) {
			struct evbuffer_chain_fd *info = EVBUFFER_CHAIN_EXTRA(struct evbuffer_chain_fd,chain);
			if (close(info->fd) == -1)
				event_warn("%s: close(%d) failed", __func__, info->fd);
		}
    }

    mm_free(chain);
}

//从chain开始，依次释放它之后的所有链节点
static void evbuffer_free_all_chains(struct evbuffer_chain *chain)
{
    struct evbuffer_chain *next;
    for (; chain; chain = next) {
        next = chain->next;
        evbuffer_chain_free(chain);
    }
}

//判断从链节点chain开始，它之后的所有链节点是否为空
static int evbuffer_chains_all_empty(struct evbuffer_chain *chain)
{
    for (; chain; chain = chain->next) {
        if (chain->off)
            return 0;
    }
    return 1;
}

/*
 * 释放所有尾部所有为空并且未固定的链节点，使用新节点替换，返回指向新节点的指针
 * 内部使用，需要加锁; 调用者必须首先固定buf->last和buf->first，它们可能已经被释放了.
 */
static struct evbuffer_chain **evbuffer_free_trailing_empty_chains(struct evbuffer *buf)
{
    struct evbuffer_chain **ch = buf->last_with_datap;
    while ((*ch) && ((*ch)->off != 0 || CHAIN_PINNED(*ch)))
        ch = &(*ch)->next;
    if (*ch) {
        EVUTIL_ASSERT(evbuffer_chains_all_empty(*ch));
        evbuffer_free_all_chains(*ch);
        *ch = NULL;
    }
    return ch;
}

/* 在“buf”的末尾添加单个链节点chain，根据需要释放尾部的空链节点。 需要锁定，不调度回调。*/
static void evbuffer_chain_insert(struct evbuffer *buf, struct evbuffer_chain *chain)
{
    ASSERT_EVBUFFER_LOCKED(buf);

    /* 新建evbuffer时是把整个evbuffer结构体都赋值0，并有buffer->last_with_datap = &buffer->first;第一次插入链节点，*buf->last_with_datap为NULL */
    if (*buf->last_with_datap == NULL) {
        EVUTIL_ASSERT(buf->last_with_datap == &buf->first);
        EVUTIL_ASSERT(buf->first == NULL);
        buf->first = buf->last = chain;
    }
    else {
        struct evbuffer_chain **ch = buf->last_with_datap;
        /* 在链表中寻找到一个可以使用的evbuffer_chain.可以使用是指该chain没有数据并且可以修改，它可能为*last_with_datap */
        while ((*ch) && ((*ch)->off != 0 || CHAIN_PINNED(*ch)))
            ch = &(*ch)->next;
        if (*ch == NULL) {//在已有的链表中找不到一个满足条件的evbuffer_chain。
            buf->last->next = chain;
            if (chain->off)//要插入的这个chain是有数据的
                buf->last_with_datap = &buf->last->next;//last_with_datap指向的是倒数第二个有数据的chain的next
        } else {
            /* 断言，从这个节点开始，后面的说有节点都是没有数据的,释放从这个节点开始的余下链表节点   */
            EVUTIL_ASSERT(evbuffer_chains_all_empty(*ch));
            evbuffer_free_all_chains(*ch);
            *ch = chain;
        }
        buf->last = chain;
    }
    buf->total_len += chain->off;
}

//在buf中新建一个链块并插入到尾部
static struct evbuffer_chain *evbuffer_chain_insert_new(struct evbuffer *buf, size_t datlen)
{
    struct evbuffer_chain *chain;
    if ((chain = evbuffer_chain_new(datlen)) == NULL)
        return NULL;
    evbuffer_chain_insert(buf, chain);
    return chain;
}

struct evbuffer* evbuffer_new(){
    struct evbuffer *buffer;
    buffer = (struct evbuffer*)mm_calloc(1,sizeof(struct evbuffer));
    if(buffer == NULL)
        return NULL;

    TAILQ_INIT(&buffer->callbacks);
    buffer->refcnt = 1;
    buffer->last_with_datap = &buffer->first;

    return buffer;
}

int evbuffer_set_flags(struct evbuffer *buf, uint64_t flags)
{
    EVBUFFER_LOCK(buf);
    buf->flags |= (uint32_t)flags;
    EVBUFFER_UNLOCK(buf);
    return 0;
}

int evbuffer_clear_flags(struct evbuffer *buf, uint64_t flags)
{
    EVBUFFER_LOCK(buf);
    buf->flags &= ~(uint32_t)flags;
    EVBUFFER_UNLOCK(buf);
    return 0;
}

void evbuffer_incref_and_lock(struct evbuffer *buf)
{
    EVBUFFER_LOCK(buf);
    ++buf->refcnt;
}

int evbuffer_defer_callbacks(struct evbuffer *buffer, struct event_base *base)
{
    EVBUFFER_LOCK(buffer);
    buffer->cb_queue = event_base_get_deferred_cb_queue(base);
    buffer->deferred_cbs = 1;
    event_deferred_cb_init(&buffer->deferred, evbuffer_deferred_callback, buffer);
    EVBUFFER_UNLOCK(buffer);
    return 0;
}

int evbuffer_enable_locking(struct evbuffer *buf, void *lock)
{
    if (buf->lock)
        return -1;

    if (!lock) {
        EVTHREAD_ALLOC_LOCK(lock, EVTHREAD_LOCKTYPE_RECURSIVE);
        if (!lock)
            return -1;
        buf->lock = lock;
        buf->own_lock = 1;
    } else {
        buf->lock = lock;
        buf->own_lock = 0;
    }

    return 0;
}

void evbuffer_set_parent(struct evbuffer *buf, struct bufferevent *bev)
{
    EVBUFFER_LOCK(buf);
    buf->parent = bev;
    EVBUFFER_UNLOCK(buf);
}

static void evbuffer_run_callbacks(struct evbuffer *buffer, int running_deferred)
{
    struct evbuffer_cb_entry *cbent, *next;
    struct evbuffer_cb_info info;
    size_t new_size;
    uint32_t mask, masked_val;
    int clear = 1;

    if (running_deferred) {
        mask = EVBUFFER_CB_NODEFER|EVBUFFER_CB_ENABLED;
        masked_val = EVBUFFER_CB_ENABLED;
    } else if (buffer->deferred_cbs) {
        mask = EVBUFFER_CB_NODEFER|EVBUFFER_CB_ENABLED;
        masked_val = EVBUFFER_CB_NODEFER|EVBUFFER_CB_ENABLED;
        clear = 0;
    } else {//一般是这种
        mask = EVBUFFER_CB_ENABLED;
        masked_val = EVBUFFER_CB_ENABLED;
    }

    ASSERT_EVBUFFER_LOCKED(buffer);

    if (TAILQ_EMPTY(&buffer->callbacks)) {//用户没有设置回调函数
        buffer->n_add_for_cb = buffer->n_del_for_cb = 0;
        return;
    }
    //没有添加或者删除数据
    if (buffer->n_add_for_cb == 0 && buffer->n_del_for_cb == 0)
        return;

    new_size = buffer->total_len;
    info.orig_size = new_size + buffer->n_del_for_cb - buffer->n_add_for_cb;
    info.n_added = buffer->n_add_for_cb;
    info.n_deleted = buffer->n_del_for_cb;
    if (clear) {//清零，为下次计算做准备
        buffer->n_add_for_cb = 0;
        buffer->n_del_for_cb = 0;
    }
    //遍历回调函数队列，调用回调函数
    for (cbent = TAILQ_FIRST(&buffer->callbacks); cbent != NULL; cbent = next) {
        next = TAILQ_NEXT(cbent, next);

        if ((cbent->flags & mask) != masked_val)
            continue;

        if ((cbent->flags & EVBUFFER_CB_OBSOLETE))
            cbent->cb.cb_obsolete(buffer, info.orig_size, new_size, cbent->cbarg);
        else
            cbent->cb.cb_func(buffer, &info, cbent->cbarg);
    }
}

void evbuffer_invoke_callbacks(struct evbuffer *buffer)
{
    if (TAILQ_EMPTY(&buffer->callbacks)) {
        buffer->n_add_for_cb = buffer->n_del_for_cb = 0;
        return;
    }

    if (buffer->deferred_cbs) {
        if (buffer->deferred.queued)
            return;
        evbuffer_incref_and_lock(buffer);
        if (buffer->parent)
            bufferevent_incref(buffer->parent);
        EVBUFFER_UNLOCK(buffer);
        event_deferred_cb_schedule(buffer->cb_queue, &buffer->deferred);
    }

    evbuffer_run_callbacks(buffer, 0);
}

static void evbuffer_deferred_callback(struct deferred_cb *cb, void *arg)
{
    struct bufferevent *parent = NULL;
    struct evbuffer *buffer = (struct evbuffer *)arg;

    EVBUFFER_LOCK(buffer);
    parent = buffer->parent;
    evbuffer_run_callbacks(buffer, 1);
    evbuffer_decref_and_unlock(buffer);
    if (parent)
        bufferevent_decref(parent);
}

static void evbuffer_remove_all_callbacks(struct evbuffer *buffer)
{
    struct evbuffer_cb_entry *cbent;

    while ((cbent = TAILQ_FIRST(&buffer->callbacks))) {
        TAILQ_REMOVE(&buffer->callbacks, cbent, next);
        mm_free(cbent);
    }
}

void evbuffer_decref_and_unlock(struct evbuffer *buffer)
{
    struct evbuffer_chain *chain, *next;
    ASSERT_EVBUFFER_LOCKED(buffer);

    EVUTIL_ASSERT(buffer->refcnt > 0);

    if (--buffer->refcnt > 0) {
        EVBUFFER_UNLOCK(buffer);
        return;
    }

    for (chain = buffer->first; chain != NULL; chain = next) {
        next = chain->next;
        evbuffer_chain_free(chain);//依次释放
    }
    evbuffer_remove_all_callbacks(buffer);//移除所有的回调函数
    if (buffer->deferred_cbs)
        event_deferred_cb_cancel(buffer->cb_queue, &buffer->deferred);

    EVBUFFER_UNLOCK(buffer);
    if (buffer->own_lock)
        EVTHREAD_FREE_LOCK(buffer->lock, EVTHREAD_LOCKTYPE_RECURSIVE);
    mm_free(buffer);
}

void evbuffer_free(struct evbuffer *buffer)
{
    EVBUFFER_LOCK(buffer);
    evbuffer_decref_and_unlock(buffer);
}

void evbuffer_lock(struct evbuffer *buf)
{
    EVBUFFER_LOCK(buf);
}

void evbuffer_unlock(struct evbuffer *buf)
{
    EVBUFFER_UNLOCK(buf);
}

size_t evbuffer_get_length(const struct evbuffer *buffer)
{
    size_t result;

    EVBUFFER_LOCK(buffer);

    result = (buffer->total_len);

    EVBUFFER_UNLOCK(buffer);

    return result;
}

size_t evbuffer_get_contiguous_space(const struct evbuffer *buf)
{
    struct evbuffer_chain *chain;
    size_t result;

    EVBUFFER_LOCK(buf);
    chain = buf->first;
    result = (chain != NULL ? chain->off : 0);
    EVBUFFER_UNLOCK(buf);

    return result;
}

static inline int HAS_PINNED_R(struct evbuffer *buf)
{
    return (buf->last && CHAIN_PINNED_R(buf->last));
}

static inline void ZERO_CHAIN(struct evbuffer *dst)
{
    ASSERT_EVBUFFER_LOCKED(dst);
    dst->first = NULL;
    dst->last = NULL;
    dst->last_with_datap = &(dst)->first;
    dst->total_len = 0;
}

/* 通过删除read-pinned链节点将src的内容移动到另一个缓冲区。 第一个pinned链节点被保存在first，最后一个保存在last。 如果src没有read-pinned链节点，则first和last将设置为NULL。*/
static int PRESERVE_PINNED(struct evbuffer *src, struct evbuffer_chain **first, struct evbuffer_chain **last)
{
    struct evbuffer_chain *chain, **pinned;

    ASSERT_EVBUFFER_LOCKED(src);

    //如果src->last不为NULL并且src->last设置EVBUFFER_MEM_PINNED_R，HAS_PINNED_R返回true
    if (!HAS_PINNED_R(src)) {
        *first = *last = NULL;
        return 0;
    }

    pinned = src->last_with_datap;
    if (!CHAIN_PINNED_R(*pinned))
        pinned = &(*pinned)->next;
    EVUTIL_ASSERT(CHAIN_PINNED_R(*pinned));
    chain = *first = *pinned;
    *last = src->last;

    if (chain->off) {
        struct evbuffer_chain *tmp;

        EVUTIL_ASSERT(pinned == src->last_with_datap);
        tmp = evbuffer_chain_new(chain->off);
        if (!tmp)
            return -1;
        memcpy(tmp->buffer, chain->buffer + chain->misalign, chain->off);
        tmp->off = chain->off;
        *src->last_with_datap = tmp;
        src->last = tmp;
        chain->misalign += chain->off;
        chain->off = 0;
    } else {
        src->last = *src->last_with_datap;
        *pinned = NULL;
    }

    return 0;
}

static inline void RESTORE_PINNED(struct evbuffer *src, struct evbuffer_chain *pinned, struct evbuffer_chain *last)
{
    ASSERT_EVBUFFER_LOCKED(src);

    if (!pinned) {
        ZERO_CHAIN(src);
        return;
    }

    src->first = pinned;
    src->last = last;
    src->last_with_datap = &src->first;
    src->total_len = 0;
}

static inline void COPY_CHAIN(struct evbuffer *dst, struct evbuffer *src)
{
    ASSERT_EVBUFFER_LOCKED(dst);
    ASSERT_EVBUFFER_LOCKED(src);
    dst->first = src->first;
    if (src->last_with_datap == &src->first)
        dst->last_with_datap = &dst->first;
    else
        dst->last_with_datap = src->last_with_datap;
    dst->last = src->last;
    dst->total_len = src->total_len;
}

static void APPEND_CHAIN(struct evbuffer *dst, struct evbuffer *src)
{
    ASSERT_EVBUFFER_LOCKED(dst);
    ASSERT_EVBUFFER_LOCKED(src);
    dst->last->next = src->first;
    if (src->last_with_datap == &src->first)
        dst->last_with_datap = &dst->last->next;
    else
        dst->last_with_datap = src->last_with_datap;
    dst->last = src->last;
    dst->total_len += src->total_len;
}

static void PREPEND_CHAIN(struct evbuffer *dst, struct evbuffer *src)
{
    ASSERT_EVBUFFER_LOCKED(dst);
    ASSERT_EVBUFFER_LOCKED(src);
    src->last->next = dst->first;
    dst->first = src->first;
    dst->total_len += src->total_len;
    if (*dst->last_with_datap == NULL) {
        if (src->last_with_datap == &(src)->first)
            dst->last_with_datap = &dst->first;
        else
            dst->last_with_datap = src->last_with_datap;
    }
    else if (dst->last_with_datap == &dst->first) {
        dst->last_with_datap = &src->last->next;
    }
}

int evbuffer_add_buffer(struct evbuffer *outbuf, struct evbuffer *inbuf)
{
    struct evbuffer_chain *pinned, *last;
    size_t in_total_len, out_total_len;
    int result = 0;

    EVBUFFER_LOCK2(inbuf, outbuf);
    in_total_len = inbuf->total_len;
    out_total_len = outbuf->total_len;

    //如果输入buffer没有数据或者输入buffer等于输出buffer直接返回
    if (in_total_len == 0 || outbuf == inbuf)
        goto done;

    //如果输出buffer禁止追加或者输入buffer禁止从头部开始修改数据
    if (outbuf->freeze_end || inbuf->freeze_start) {
        result = -1;
        goto done;
    }

    if (PRESERVE_PINNED(inbuf, &pinned, &last) < 0) {
        result = -1;
        goto done;
    }

    if (out_total_len == 0) {
        /* outbuf开始时可能有一个空链; 释放它 */
        evbuffer_free_all_chains(outbuf->first);
        COPY_CHAIN(outbuf, inbuf);
    } else {
        APPEND_CHAIN(outbuf, inbuf);
    }

    RESTORE_PINNED(inbuf, pinned, last);

    inbuf->n_del_for_cb += in_total_len;
    outbuf->n_add_for_cb += in_total_len;

    //唤醒回调
    evbuffer_invoke_callbacks(inbuf);
    evbuffer_invoke_callbacks(outbuf);

done:
    EVBUFFER_UNLOCK2(inbuf, outbuf);
    return result;
}

#define EVBUFFER_CHAIN_MAX_AUTO_SIZE 4096

int evbuffer_add(struct evbuffer *buf, const void *data_in, size_t datlen)
{
    struct evbuffer_chain *chain, *tmp;
    const unsigned char *data = (const unsigned char *)data_in;
    size_t remain, to_alloc;
    int result = -1;

    EVBUFFER_LOCK(buf);

    //禁止追加数据
    if (buf->freeze_end) {
        goto done;
    }
    /* 防止buf->total_len溢出 */
    if (datlen > EV_SIZE_MAX - buf->total_len) {
        goto done;
    }

    chain = buf->last;

    /* 如果evbuffer没有链节点分配，分配一个足够大的链节点,直接把新建的evbuffer_chain插入到链表中。*/
    if (chain == NULL) {
        chain = evbuffer_chain_new(datlen);
        if (!chain)
            goto done;
        evbuffer_chain_insert(buf, chain);
    }
    //判断是否是只读链节点
    if ((chain->flags & EVBUFFER_IMMUTABLE) == 0) {

        EVUTIL_ASSERT(chain->misalign >= 0 && (uint64_t)chain->misalign <= EVBUFFER_CHAIN_MAX);
        remain = chain->buffer_len - (size_t)chain->misalign - chain->off;//当前chain可用的空间字节数
        if (remain >= datlen) {
            /* 当前chain节点中有足够的空间容纳所有数据*/
            memcpy(chain->buffer + chain->misalign + chain->off, data, datlen);
            chain->off += datlen;
            buf->total_len += datlen;
            buf->n_add_for_cb += datlen;
            goto out;
        } else if (!CHAIN_PINNED(chain) && evbuffer_chain_should_realign(chain, datlen)) {
            /* 该evbuffer_chain可以修改 && 通过调整后，也可以放得下本次要插入的数据  */
            evbuffer_chain_align(chain);//调整

            memcpy(chain->buffer + chain->off, data, datlen);
            chain->off += datlen;
            buf->total_len += datlen;
            buf->n_add_for_cb += datlen;
            goto out;
        }
    } else {
        /* 不能写任何数据到最后一个链节点 */
        remain = 0;
    }

    /* 当chain是只读链节点或者chain放不下本次要插入的数据时才会执行下面代码，此时需要添加一个新的链节点 */
    /* 当最后evbuffer_chain的缓冲区小于等于2048时，那么新建的evbuffer_chain的  大小将是最后一个节点缓冲区的2倍。但是最后的大小还是由要插入的数据决定。虽然to_alloc最后的值可能为datlen。但在evbuffer_chain_new中，实际分配的内存大小必然是512的倍数。*/
    to_alloc = chain->buffer_len;
    if (to_alloc <= EVBUFFER_CHAIN_MAX_AUTO_SIZE/2)
        to_alloc <<= 1;
    if (datlen > to_alloc)
        to_alloc = datlen;
    tmp = evbuffer_chain_new(to_alloc);
    if (tmp == NULL)
        goto done;

    //链表最后那个节点还是可以放下一些数据的。那么就先填满链表最后那个节点
    if (remain) {
        memcpy(chain->buffer + chain->misalign + chain->off, data, remain);
        chain->off += remain;
        buf->total_len += remain;
        buf->n_add_for_cb += remain;
    }

    data += remain;
    datlen -= remain;

    memcpy(tmp->buffer, data, datlen);
    tmp->off = datlen;
    evbuffer_chain_insert(buf, tmp);
    buf->n_add_for_cb += datlen;

out:
    evbuffer_invoke_callbacks(buf);
    result = 0;
done:
    EVBUFFER_UNLOCK(buf);
    return result;
}

/** Helper: realigns the memory in chain->buffer so that misalign is 0. */
static void evbuffer_chain_align(struct evbuffer_chain *chain)
{
    EVUTIL_ASSERT(!(chain->flags & EVBUFFER_IMMUTABLE));
    EVUTIL_ASSERT(!(chain->flags & EVBUFFER_MEM_PINNED_ANY));
    memmove(chain->buffer, chain->buffer + chain->misalign, chain->off);
    chain->misalign = 0;
}

#define MAX_TO_COPY_IN_EXPAND 4096
#define MAX_TO_REALIGN_IN_EXPAND 2048

/** 如果我们应该重新排列chain以适合数据的datalen字节，则返回true。 */
static int evbuffer_chain_should_realign(struct evbuffer_chain *chain, size_t datlen)
{
    return chain->buffer_len - chain->off >= datlen &&
           (chain->off < chain->buffer_len / 2) &&
           (chain->off <= MAX_TO_REALIGN_IN_EXPAND);
}

/* 扩展evbuffer的可用空间大小至少为datlen，全部在一个节点中，返回那个节点，如果最后一个节点有数据但是不能存放datlen长度的数据，如果数据量较小，则把这个小数据节点的数据移动到一个足够大的数据连接点，并且释放小数据节点*/
static struct evbuffer_chain *evbuffer_expand_singlechain(struct evbuffer *buf, size_t datlen)
{
    struct evbuffer_chain *chain, **chainp;
    struct evbuffer_chain *result = NULL;
    ASSERT_EVBUFFER_LOCKED(buf);

    chainp = buf->last_with_datap;

    //如果*chainp不为NULL并且没有可用空间，选择它的下一个节点
    if (*chainp && CHAIN_SPACE_LEN(*chainp) == 0)
        chainp = &(*chainp)->next;

    /* chain现在指向第一个可写链节点,可能为最后一个有数据的节点，也可能为最后一个有数据的节点的下一个节点*/
    chain = *chainp;

    //chain为空或者这个chain不可修改，插入一个新的节点
    if (chain == NULL || (chain->flags & (EVBUFFER_IMMUTABLE|EVBUFFER_MEM_PINNED_ANY))) {
        goto insert_new;
    }

    /* 如果chain的可用空间大于datlen，直接使用chain */
    if (CHAIN_SPACE_LEN(chain) >= datlen) {
        result = chain;
        goto ok;
    }

    //如果chain没有任何数据，添加一个新的节点去替换它
    if (chain->off == 0) {
        goto insert_new;
    }

    /* 如果chain里面的错位空间加上剩余空间可以满足要求，调整*/
    if (evbuffer_chain_should_realign(chain, datlen)) {
        evbuffer_chain_align(chain);
        result = chain;
        goto ok;
    }

    /* 如果当前chain的可用空间小于buffer_len/8,或者off大于4096或者off+datalen会溢出*/
    if (CHAIN_SPACE_LEN(chain) < chain->buffer_len / 8 || chain->off > MAX_TO_COPY_IN_EXPAND ||
        (datlen < EVBUFFER_CHAIN_MAX && EVBUFFER_CHAIN_MAX - datlen >= chain->off)) {
        /* 不值得调整这个节点的大小，判断下一个节点是否可用，如果不可以插入一个新的*/
        if (chain->next && CHAIN_SPACE_LEN(chain->next) >= datlen) {
            result = chain->next;
            goto ok;
        } else {
            goto insert_new;
        }
    }
    else {//直接调整大小，由于本chain的数据量比较小，所以把这个chain的数据迁移到另外一个 节点
        size_t length = chain->off + datlen;
        struct evbuffer_chain *tmp = evbuffer_chain_new(length);
        if (tmp == NULL)
            goto err;

        tmp->off = chain->off;
        memcpy(tmp->buffer, chain->buffer + chain->misalign, chain->off);
        EVUTIL_ASSERT(*chainp == chain);
        result = *chainp = tmp;

        if (buf->last == chain)
            buf->last = tmp;

        tmp->next = chain->next;
        evbuffer_chain_free(chain);
        goto ok;
    }

insert_new:
    result = evbuffer_chain_insert_new(buf, datlen);
    if (!result)
        goto err;
ok:
    EVUTIL_ASSERT(result);
    EVUTIL_ASSERT(CHAIN_SPACE_LEN(result) >= datlen);
err:
    return result;
}

int evbuffer_expand_fast(struct evbuffer *buf, size_t datlen, int n)
{
    struct evbuffer_chain *chain = buf->last, *tmp, *next;
    size_t avail;
    int used;

    ASSERT_EVBUFFER_LOCKED(buf);
    EVUTIL_ASSERT(n >= 2);//n必须大于等于2

    //最后一个节点是不可用的,直接新建一个足够容量的节点
    if (chain == NULL || (chain->flags & EVBUFFER_IMMUTABLE)) {
        chain = evbuffer_chain_new(datlen);
        if (chain == NULL)
            return (-1);

        evbuffer_chain_insert(buf, chain);
        return (0);
    }

    used = 0; /* 使用的节点数 */
    avail = 0; /* 可用字节数 */
    for (chain = *buf->last_with_datap; chain; chain = chain->next) {
        if (chain->off) {//最后一个有数据的节点的可用空间也是要被使用
            size_t space = (size_t) CHAIN_SPACE_LEN(chain);//当前有数据的节点中的可用空间
            EVUTIL_ASSERT(chain == *buf->last_with_datap);
            if (space) {
                avail += space;
                ++used;
            }
        }
        else {//链表中off为0的空buffer统统使用
            chain->misalign = 0;
            avail += chain->buffer_len;
            ++used;
        }
        if (avail >= datlen) {//链表中的节点的可用空间已经足够了
            return (0);
        }
        if (used == n)//到达了最大可以忍受的节点数
            break;
    }

    /* 前面的for循环，如果找够了空闲空间，那么是直接return。所以运行到这里时，就说明还没找到空闲空间。一般是因为链表后面的off等于0的节点已经被用完了都还不能满足datlen  */
    if (used < n) {
        EVUTIL_ASSERT(chain == NULL);
        tmp = evbuffer_chain_new(datlen - avail);//申请一个足够大的evbuffer_chain，把空间补足
        if (tmp == NULL)
            return (-1);

        buf->last->next = tmp;
        buf->last = tmp;
        return (0);
    }
    else {
        /*used == n: 把后面的n个节点都用了还是不够datlen空间,统统删除n-1个节点然后新建一个足够大的evbuffer_chain。*/
        int rmv_all = 0; //用来标志该链表的所有节点都是off为0的。在这种情况下，将删除所有的节点
        chain = *buf->last_with_datap;
        if (!chain->off) {
            EVUTIL_ASSERT(chain == buf->first);
            rmv_all = 1;
            avail = 0;
        }
        else {
            avail = (size_t) CHAIN_SPACE_LEN(chain);//最后一个有数据的chain的可用空间的大小。这个空间是可以用上的
            chain = chain->next;
        }

        //释放空节点，然后新建一个足够大的节点
        for (; chain; chain = next) {
            next = chain->next;
            EVUTIL_ASSERT(chain->off == 0);
            evbuffer_chain_free(chain);
        }
        EVUTIL_ASSERT(datlen >= avail);
        tmp = evbuffer_chain_new(datlen - avail);
        if (tmp == NULL) {
            if (rmv_all) {//这种情况下，该链表就根本没有节点了
                ZERO_CHAIN(buf);//相当于初始化evbuffer的链表
            }
            else {
                buf->last = *buf->last_with_datap;
                (*buf->last_with_datap)->next = NULL;
            }
            return (-1);
        }

        if (rmv_all) {//这种情况下，该链表就只有一个节点了
            buf->first = buf->last = tmp;
            buf->last_with_datap = &buf->first;
        }
        else {
            (*buf->last_with_datap)->next = tmp;
            buf->last = tmp;
        }
        return (0);
    }
}

int evbuffer_expand(struct evbuffer *buf, size_t datlen)
{
    struct evbuffer_chain *chain;

    EVBUFFER_LOCK(buf);
    chain = evbuffer_expand_singlechain(buf, datlen);
    EVBUFFER_UNLOCK(buf);
    return chain ? 0 : -1;
}

int evbuffer_remove(struct evbuffer *buf, void *data_out, size_t datlen)
{
    ssize_t n;
    EVBUFFER_LOCK(buf);
    n = evbuffer_copyout(buf, data_out, datlen);
    if (n > 0) {
        if (evbuffer_drain(buf, n)<0)
            n = -1;
    }
    EVBUFFER_UNLOCK(buf);
    return (int)n;
}

int evbuffer_drain(struct evbuffer *buf, size_t len)
{
    struct evbuffer_chain *chain, *next;
    size_t remaining, old_len;
    int result = 0;

    EVBUFFER_LOCK(buf);
    old_len = buf->total_len;

    if (old_len == 0)
        goto done;

    if (buf->freeze_start) {
        result = -1;
        goto done;
    }

    //如果长度大于buffer的总字节数并且buf没有读锁定，释放所有节点
    if (len >= old_len && !HAS_PINNED_R(buf)) {
        len = old_len;
        for (chain = buf->first; chain != NULL; chain = next) {
            next = chain->next;
            evbuffer_chain_free(chain);
        }

        ZERO_CHAIN(buf);
    }
    else {
        if (len >= old_len)
            len = old_len;

        buf->total_len -= len;
        remaining = len;
        for (chain = buf->first; remaining >= chain->off; chain = next) {
            next = chain->next;
            remaining -= chain->off;

            if (chain == *buf->last_with_datap) {//已经删除到最后一个有数据的evbuffer_chain了
                buf->last_with_datap = &buf->first;
            }
            if (&chain->next == buf->last_with_datap)//删除到倒数第二个有数据的evbuffer_chain
                buf->last_with_datap = &buf->first;

            if (CHAIN_PINNED_R(chain)) {//这个chain被固定了，不能删除 ，后面的evbuffer_chain也是固定的
                EVUTIL_ASSERT(remaining == 0);
                chain->misalign += chain->off;
                chain->off = 0;
                break;
            }
            else
                evbuffer_chain_free(chain);
        }

        buf->first = chain;
        if (chain) {
            EVUTIL_ASSERT(remaining <= chain->off);
            chain->misalign += remaining;
            chain->off -= remaining;
        }
    }

    buf->n_del_for_cb += len;
    evbuffer_invoke_callbacks(buf);//调用回调

done:
    EVBUFFER_UNLOCK(buf);
    return result;
}

ssize_t evbuffer_copyout(struct evbuffer *buf, void *data_out, size_t datlen)
{
    struct evbuffer_chain *chain;
    char *data = (char *)data_out;
    size_t nread;
    ssize_t result = 0;

    EVBUFFER_LOCK(buf);

    chain = buf->first;

    if (datlen >= buf->total_len)
        datlen = buf->total_len;

    if (datlen == 0)
        goto done;

    if (buf->freeze_start) {
        result = -1;
        goto done;
    }

    nread = datlen;

    while (datlen && datlen >= chain->off) {
        memcpy(data, chain->buffer + chain->misalign, chain->off);
        data += chain->off;
        datlen -= chain->off;

        chain = chain->next;
        EVUTIL_ASSERT(chain || datlen==0);
    }

    if (datlen) {
        EVUTIL_ASSERT(chain);
        EVUTIL_ASSERT(datlen <= chain->off);
        memcpy(data, chain->buffer + chain->misalign, datlen);
    }

    result = nread;
done:
    EVBUFFER_UNLOCK(buf);
    return result;
}

static int advance_last_with_data(struct evbuffer *buf)
{
    int n = 0;
    ASSERT_EVBUFFER_LOCKED(buf);

    if (!*buf->last_with_datap)
        return 0;

    while ((*buf->last_with_datap)->next && (*buf->last_with_datap)->next->off) {
        buf->last_with_datap = &(*buf->last_with_datap)->next;
        ++n;
    }
    return n;
}

int evbuffer_remove_buffer(struct evbuffer *src, struct evbuffer *dst, size_t datlen)
{
    struct evbuffer_chain *chain, *previous;
    size_t nread = 0;
    int result;

    EVBUFFER_LOCK2(src, dst);

    chain = previous = src->first;

    if (datlen == 0 || dst == src) {
        result = 0;
        goto done;
    }

    //dst禁止尾部追加，src禁止从头部修改数据
    if (dst->freeze_end || src->freeze_start) {
        result = -1;
        goto done;
    }

    /* 如果datlen大于src->total_len,直接把src添加到dst*/
    if (datlen >= src->total_len) {
        datlen = src->total_len;
        evbuffer_add_buffer(dst, src);
        result = (int)datlen;
        goto done;
    }

    /* 尽可能的删除节点的数据*/
    while (chain->off <= datlen) {
        /* 我们无法从src中删除最后一个有数据的节点，除非我们删除所有链*/
        EVUTIL_ASSERT(chain != *src->last_with_datap);
        nread += chain->off;
        datlen -= chain->off;
        previous = chain;
        if (src->last_with_datap == &chain->next)
            src->last_with_datap = &src->first;
        chain = chain->next;
    }

    //把整块数据添加的节点数据添加到dst
    if (nread) {
        /* we can remove the chain */
        struct evbuffer_chain **chp;
        chp = evbuffer_free_trailing_empty_chains(dst);

        if (dst->first == NULL) {
            dst->first = src->first;
        } else {
            *chp = src->first;
        }
        dst->last = previous;
        previous->next = NULL;
        src->first = chain;
        advance_last_with_data(dst);//调整dst的最后一个有数据的节点的指向

        dst->total_len += nread;
        dst->n_add_for_cb += nread;
    }

    //添加剩余的数据
    evbuffer_add(dst, chain->buffer + chain->misalign, datlen);
    chain->misalign += datlen;
    chain->off -= datlen;
    nread += datlen;

    src->total_len -= nread;
    src->n_del_for_cb += nread;

    if (nread) {
        evbuffer_invoke_callbacks(dst);
        evbuffer_invoke_callbacks(src);
    }
    result = (int)nread;

done:
    EVBUFFER_UNLOCK2(src, dst);
    return result;
}

int evbuffer_add_printf(struct evbuffer *buf, const char *fmt, ...)
{
    int res = -1;
    va_list ap;

    va_start(ap, fmt);
    res = evbuffer_add_vprintf(buf, fmt, ap);
    va_end(ap);

    return (res);
}

int evbuffer_add_vprintf(struct evbuffer *buf, const char *fmt, va_list ap)
{
    char *buffer;
    size_t space;
    int sz, result = -1;
    va_list aq;
    struct evbuffer_chain *chain;

    EVBUFFER_LOCK(buf);

    if (buf->freeze_end) {
        goto done;
    }

    /* make sure that at least some space is available */
    if ((chain = evbuffer_expand_singlechain(buf, 64)) == NULL)
        goto done;

    for (;;) {
        buffer = (char*) CHAIN_SPACE_PTR(chain);
        space = (size_t) CHAIN_SPACE_LEN(chain);

        va_copy(aq, ap);

        sz = evutil_vsnprintf(buffer, space, fmt, aq);

        va_end(aq);

        if (sz < 0)
            goto done;
        if (INT_MAX >= EVBUFFER_CHAIN_MAX && (size_t)sz >= EVBUFFER_CHAIN_MAX)
            goto done;
        if ((size_t)sz < space) {
            chain->off += sz;
            buf->total_len += sz;
            buf->n_add_for_cb += sz;

            advance_last_with_data(buf);
            evbuffer_invoke_callbacks(buf);
            result = sz;
            goto done;
        }
        if ((chain = evbuffer_expand_singlechain(buf, sz + 1)) == NULL)
            goto done;
    }

done:
    EVBUFFER_UNLOCK(buf);
    return result;
}

int evbuffer_freeze(struct evbuffer *buffer, int start)
{
    EVBUFFER_LOCK(buffer);
    if (start)
        buffer->freeze_start = 1;
    else
        buffer->freeze_end = 1;
    EVBUFFER_UNLOCK(buffer);
    return 0;
}

int evbuffer_unfreeze(struct evbuffer *buffer, int start)
{
    EVBUFFER_LOCK(buffer);
    if (start)
        buffer->freeze_start = 0;
    else
        buffer->freeze_end = 0;
    EVBUFFER_UNLOCK(buffer);
    return 0;
}

int evbuffer_prepend(struct evbuffer *buf, const void *data, size_t datlen)
{
    struct evbuffer_chain *chain, *tmp;
    int result = -1;

    EVBUFFER_LOCK(buf);

    //禁止在头部添加数据，直接返回
    if (buf->freeze_start) {
        goto done;
    }
    //防止溢出
    if (datlen > EV_SIZE_MAX - buf->total_len) {
        goto done;
    }

    chain = buf->first;

    //该buffer中没有节点，直接新增一个节点
    if (chain == NULL) {
        chain = evbuffer_chain_new(datlen);
        if (!chain)
            goto done;
        evbuffer_chain_insert(buf, chain);
    }

    /* 节点不是只读节点，可以修改数据 */
    if ((chain->flags & EVBUFFER_IMMUTABLE) == 0) {
        EVUTIL_ASSERT(chain->misalign >= 0 && (uint64_t)chain->misalign <= EVBUFFER_CHAIN_MAX);

        /* 如果这个链是空的，我们可以把它当作开始处为空，而不是结束处为空 */
        if (chain->off == 0)
            chain->misalign = chain->buffer_len;

        /*一开始chain->off等于0，之后调用evbuffer_prepend插入一些数据(还没填满这个chain),之后再次调用evbuffer_prepend插入一些  数据。这样就能分别进入下面的if else了*/
        if ((size_t)chain->misalign >= datlen) {
            /* 有足够的空间容纳 */
            memcpy(chain->buffer + chain->misalign - datlen, data, datlen);
            chain->off += datlen;
            chain->misalign -= datlen;
            buf->total_len += datlen;
            buf->n_add_for_cb += datlen;
            goto out;
        }
        else if (chain->misalign) {
            /* 只能容纳部分数据，从头开始用完所有空间 */
            memcpy(chain->buffer, (char*)data + datlen - chain->misalign, (size_t)chain->misalign);
            chain->off += (size_t)chain->misalign;
            buf->total_len += (size_t)chain->misalign;
            buf->n_add_for_cb += (size_t)chain->misalign;
            datlen -= (size_t)chain->misalign;
            chain->misalign = 0;
        }
    }

    /* 还有数据没有添加，需要增加一个节点*/
    if ((tmp = evbuffer_chain_new(datlen)) == NULL)
        goto done;
    buf->first = tmp;
    if (buf->last_with_datap == &buf->first)
        buf->last_with_datap = &tmp->next;

    tmp->next = chain;

    tmp->off = datlen;
    EVUTIL_ASSERT(datlen <= tmp->buffer_len);
    tmp->misalign = tmp->buffer_len - datlen;

    memcpy(tmp->buffer + tmp->misalign, data, datlen);
    buf->total_len += datlen;
    buf->n_add_for_cb += (size_t)chain->misalign;

out:
    evbuffer_invoke_callbacks(buf);
    result = 0;
done:
    EVBUFFER_UNLOCK(buf);
    return result;
}

int evbuffer_prepend_buffer(struct evbuffer *outbuf, struct evbuffer *inbuf)
{
    struct evbuffer_chain *pinned, *last;
    size_t in_total_len, out_total_len;
    int result = 0;

    EVBUFFER_LOCK2(inbuf, outbuf);

    in_total_len = inbuf->total_len;
    out_total_len = outbuf->total_len;

    if (!in_total_len || inbuf == outbuf)
        goto done;

    if (outbuf->freeze_start || inbuf->freeze_start) {
        result = -1;
        goto done;
    }

    if (PRESERVE_PINNED(inbuf, &pinned, &last) < 0) {
        result = -1;
        goto done;
    }

    if (out_total_len == 0) {
        evbuffer_free_all_chains(outbuf->first);
        COPY_CHAIN(outbuf, inbuf);
    } else {
        PREPEND_CHAIN(outbuf, inbuf);
    }

    RESTORE_PINNED(inbuf, pinned, last);

    inbuf->n_del_for_cb += in_total_len;
    outbuf->n_add_for_cb += in_total_len;

    evbuffer_invoke_callbacks(inbuf);
    evbuffer_invoke_callbacks(outbuf);
done:
    EVBUFFER_UNLOCK2(inbuf, outbuf);
    return result;
}

struct evbuffer_ptr evbuffer_search(struct evbuffer *buffer, const char *what, size_t len, const struct evbuffer_ptr *start)
{
    return evbuffer_search_range(buffer, what, len, start, NULL);
}

struct evbuffer_ptr evbuffer_search_range(struct evbuffer *buffer, const char *what, size_t len,
                      const struct evbuffer_ptr *start, const struct evbuffer_ptr *end)
{
    struct evbuffer_ptr pos;
    struct evbuffer_chain *chain, *last_chain = NULL;
    const unsigned char *p;
    char first;

    EVBUFFER_LOCK(buffer);

    //设置正确的范围，初始化pos
    if (start) {
        memcpy(&pos, start, sizeof(pos));
        chain = (struct evbuffer_chain *)(pos.internal.chain);
    }
    else {//如果没有设置start，从第一个节点开始查找
        pos.pos = 0;
        pos.internal.chain = (void*)buffer->first;
        chain =  buffer->first;
        pos.internal.pos_in_chain = 0;
    }

    if (end)
        last_chain = (struct evbuffer_chain *)end->internal.chain;

    //非法长度
    if (!len || len > EV_SSIZE_MAX)
        goto done;

    first = what[0];

    /* 本函数里面并不考虑到what的数据量比较链表的总数据量还多。但在evbuffer_ptr_memcmp函数中会考虑这个问题。此时该函数直接返回-1。本函数之所以没有考虑这样情况，可能是因为，在[start, end]之间有多少数据是不值得统计的，时间复杂度是O(n)。不是一个简单的buffer->total_len就能获取到的 */
    while (chain) {
        const u_char *start_at = (const u_char *)chain->buffer + chain->misalign + pos.internal.pos_in_chain;
        p = (const u_char*)memchr(start_at, first, chain->off - pos.internal.pos_in_chain);
        if (p) {//first在start_at为首地址的缓冲区中存在
            pos.pos += p - start_at;
            pos.internal.pos_in_chain += p - start_at;//设置偏移
            if (!evbuffer_ptr_memcmp(buffer, &pos, what, len)) {//缓冲区中指定偏移量下比较字符串
                if (end && pos.pos + (ssize_t)len > end->pos)//如果what在缓冲区中存在，但是超过了end的范围
                    goto not_found;
                else
                    goto done;
            }
            ++pos.pos;//未找到，增加偏移量，从一个字节开始比较
            ++pos.internal.pos_in_chain;
            if (pos.internal.pos_in_chain == chain->off) {
                pos.internal.chain = (void*)chain->next;
                chain  = chain->next;
                pos.internal.pos_in_chain = 0;
            }
        }
        else {//当前节点找不到，到下一个节点中去查找，如果，当前节点已经是查找的最后一个节点，直接返回查不到
            if (chain == last_chain)
                goto not_found;
            pos.pos += chain->off - pos.internal.pos_in_chain;
            pos.internal.chain = (void*)chain->next;
            chain = chain->next;
            pos.internal.pos_in_chain = 0;
        }
    }

not_found:
    pos.pos = -1;
    pos.internal.chain = NULL;
done:
    EVBUFFER_UNLOCK(buffer);
    return pos;
}

/** 内存比较函数，类似于memcmp */
static int evbuffer_ptr_memcmp(const struct evbuffer *buf, const struct evbuffer_ptr *pos, const char *mem, size_t len)
{
    struct evbuffer_chain *chain;
    size_t position;
    int r;

    ASSERT_EVBUFFER_LOCKED(buf);

    //排除非法位置
    if (pos->pos < 0 || EV_SIZE_MAX - len < (size_t)pos->pos || pos->pos + len > buf->total_len)
        return -1;

    chain = (struct evbuffer_chain *)pos->internal.chain;
    position = pos->internal.pos_in_chain;
    while (len && chain) {//考虑比较的数据在不同的节点中
        size_t n_comparable;//该evbuffer_chain中可以比较的字符数
        if (len + position > chain->off)
            n_comparable = chain->off - position;
        else
            n_comparable = len;
        r = memcmp(chain->buffer + chain->misalign + position, mem, n_comparable);
        if (r)
            return r;
        mem += n_comparable;
        len -= n_comparable;
        position = 0;
        chain = chain->next;
    }

    return 0;
}

//strspn(s,c): 返回字符串s中第一个不在指定字符串c中出现的字符下标.
static inline int evbuffer_strspn(struct evbuffer_ptr *ptr, const char *chrset)
{
    int count = 0;//用于统计有多少个字符是在指定字符串中出现
    struct evbuffer_chain *chain = (struct evbuffer_chain *)ptr->internal.chain;
    size_t i = ptr->internal.pos_in_chain;

    if (!chain)
        return -1;

    while (1) {//统计从chain开始
        char *buffer = (char *)chain->buffer + chain->misalign;
        for (; i < chain->off; ++i) {
            const char *p = chrset;
            while (*p) {
                if (buffer[i] == *p++)
                    goto next;
            }
            ptr->internal.chain = chain;
            ptr->internal.pos_in_chain = i;
            ptr->pos += count;
            return count;
        next:
            ++count;
        }
        i = 0;

        if (! chain->next) {//chain的下一个节点为null，表示没有比较的数据了，比较完成，所有的字符都出现了
            ptr->internal.chain = chain;
            ptr->internal.pos_in_chain = i;
            ptr->pos += count;
            return count;
        }

        chain = chain->next;
    }
}

/* strchr(s,c):查找字符串s中首次出现字符c的位置。
 * memchr(buf,ch,count): 从buf所指内存区域的前count个字节查找字符ch
 */
static inline ssize_t evbuffer_strchr(struct evbuffer_ptr *it, const char chr)
{
    struct evbuffer_chain *chain = (struct evbuffer_chain *)it->internal.chain;
    size_t i = it->internal.pos_in_chain;
    while (chain != NULL)
    {
        char *buffer = (char *)(chain->buffer + chain->misalign);//缓冲区的首地址
        char *cp = (char*)memchr(buffer+i, chr, chain->off-i);
        if (cp) {//查找到了chr
            it->internal.chain = chain;
            it->internal.pos_in_chain = cp - buffer;
            it->pos += (cp - buffer - i);//cp - (buffer+i)
            return it->pos;
        }
        it->pos += chain->off - i;//如果没有找到，在下一个chain节点中查找
        i = 0;
        chain = chain->next;
    }

    return (-1);
}

//获取对应位置的字符
static inline char evbuffer_getchr(struct evbuffer_ptr *it)
{
    struct evbuffer_chain *chain = (struct evbuffer_chain *)it->internal.chain;
    size_t off = it->internal.pos_in_chain;

    return chain->buffer[chain->misalign + off];
}

char *evbuffer_readln(struct evbuffer *buffer, size_t *n_read_out, enum evbuffer_eol_style eol_style)
{
    struct evbuffer_ptr it;
    char *line;
    size_t n_to_copy=0, extra_drain=0;
    char *result = NULL;

    EVBUFFER_LOCK(buffer);

    if (buffer->freeze_start) {
        goto done;
    }

    it = evbuffer_search_eol(buffer, NULL, &extra_drain, eol_style);
    if (it.pos < 0)
        goto done;
    n_to_copy = it.pos;

    if ((line = (char*)mm_malloc(n_to_copy+1)) == NULL) {
        event_warn("%s: out of memory", __func__);
        goto done;
    }

    evbuffer_remove(buffer, line, n_to_copy);
    line[n_to_copy] = '\0';

    evbuffer_drain(buffer, extra_drain);
    result = line;
done:
    EVBUFFER_UNLOCK(buffer);

    if (n_read_out)
        *n_read_out = result ? n_to_copy : 0;

    return result;
}

static inline char *find_eol_char(char *s, size_t len)
{
#define CHUNK_SZ 128
    char *s_end, *cr, *lf;
    s_end = s+len;
    while (s < s_end) {
        size_t chunk = (s + CHUNK_SZ < s_end) ? CHUNK_SZ : (s_end - s);
        cr = (char*)memchr(s, '\r', chunk);//s中查找‘\r’是否存在
        lf = (char*)memchr(s, '\n', chunk);//s中查找‘\n’是否存在
        if (cr) {
            if (lf && lf < cr)//‘\r’和'\n'都存在，并且'\n'先出现
                return lf;
            return cr;
        }
        else if (lf) {
            return lf;
        }
        s += CHUNK_SZ;
    }

    return NULL;
#undef CHUNK_SZ
}

static ssize_t evbuffer_find_eol_char(struct evbuffer_ptr *it)
{
    struct evbuffer_chain *chain = (struct evbuffer_chain *)it->internal.chain;
    size_t i = it->internal.pos_in_chain;
    while (chain != NULL) {
        char *buffer = (char *)chain->buffer + chain->misalign;
        char *cp = find_eol_char(buffer+i, chain->off-i);
        if (cp) {
            it->internal.chain = chain;
            it->internal.pos_in_chain = cp - buffer;
            it->pos += (cp - buffer) - i;
            return it->pos;
        }
        it->pos += chain->off - i;
        i = 0;
        chain = chain->next;
    }

    return (-1);
}

struct evbuffer_ptr evbuffer_search_eol(struct evbuffer *buffer, struct evbuffer_ptr *start,
                                        size_t *eol_len_out, enum evbuffer_eol_style eol_style)
{
    struct evbuffer_ptr it, it2;
    size_t extra_drain = 0;
    int ok = 0;

    EVBUFFER_LOCK(buffer);

    //设置搜索的起始位置
    if (start) {
        memcpy(&it, start, sizeof(it));
    }
    else {
        it.pos = 0;
        it.internal.chain = buffer->first;
        it.internal.pos_in_chain = 0;
    }

    switch (eol_style) {
        case EVBUFFER_EOL_ANY:
            if (evbuffer_find_eol_char(&it) < 0)
                goto done;
            memcpy(&it2, &it, sizeof(it));
            extra_drain = evbuffer_strspn(&it2, "\r\n");
            break;
        case EVBUFFER_EOL_CRLF_STRICT: {
            it = evbuffer_search(buffer, "\r\n", 2, &it);
            if (it.pos < 0)
                goto done;
            extra_drain = 2;
            break;
        }
        case EVBUFFER_EOL_CRLF:
            while (1) {
                if (evbuffer_find_eol_char(&it) < 0)
                    goto done;
                if (evbuffer_getchr(&it) == '\n') {
                    extra_drain = 1;
                    break;
                }
                else if (!evbuffer_ptr_memcmp(buffer, &it, "\r\n", 2)) {
                    extra_drain = 2;
                    break;
                }
                else {
                    if (evbuffer_ptr_set(buffer, &it, 1, EVBUFFER_PTR_ADD)<0)
                        goto done;
                }
            }
            break;
        case EVBUFFER_EOL_LF:
            if (evbuffer_strchr(&it, '\n') < 0)
                goto done;
            extra_drain = 1;
            break;
        default:
            goto done;
    }

    ok = 1;
done:
    EVBUFFER_UNLOCK(buffer);

    if (!ok) {
        it.pos = -1;
    }
    if (eol_len_out)
        *eol_len_out = extra_drain;

    return it;
}

int evbuffer_ptr_set(struct evbuffer *buf, struct evbuffer_ptr *pos, size_t position, enum evbuffer_ptr_how how)
{
    size_t left = position;
    struct evbuffer_chain *chain = NULL;

    EVBUFFER_LOCK(buf);

    //这个switch的作用就是给pos设置新的总偏移量值。
    switch (how) {
        case EVBUFFER_PTR_SET:
            chain = buf->first;//从第一个evbuffer_chain算起
            pos->pos = position;//设置总偏移量
            position = 0;
            break;
        case EVBUFFER_PTR_ADD:
            if (pos->pos < 0 || EV_SIZE_MAX - position < (size_t)pos->pos) {
                EVBUFFER_UNLOCK(buf);
                return -1;
            }
            chain = (struct evbuffer_chain *)pos->internal.chain;//从当前evbuffer_chain算起
            pos->pos += position;//加上相对偏移量
            position = pos->internal.pos_in_chain;
            break;
    }

    EVUTIL_ASSERT(EV_SIZE_MAX - left >= position);//跨多个节点偏移
    while (chain && position + left >= chain->off) {
        left -= chain->off - position;
        chain = chain->next;
        position = 0;
    }
    if (chain) {
        pos->internal.chain = chain;
        pos->internal.pos_in_chain = position + left;
    } else {//跨越所有的节点
        pos->internal.chain = NULL;
        pos->pos = -1;
    }

    EVBUFFER_UNLOCK(buf);

    return chain != NULL ? 0 : -1;
}

int evbuffer_add_reference(struct evbuffer *outbuf, const void *data, size_t datlen,
                           evbuffer_ref_cleanup_cb cleanupfn, void *extra)
{
    struct evbuffer_chain *chain;
    struct evbuffer_chain_reference *info;
    int result = -1;

    chain = evbuffer_chain_new(sizeof(struct evbuffer_chain_reference));
    if (!chain)
        return (-1);
    chain->flags |= EVBUFFER_REFERENCE | EVBUFFER_IMMUTABLE;
    chain->buffer = (u_char *)data;
    chain->buffer_len = datlen;
    chain->off = datlen;

    info = EVBUFFER_CHAIN_EXTRA(struct evbuffer_chain_reference, chain);
    info->cleanupfn = cleanupfn;
    info->extra = extra;

    EVBUFFER_LOCK(outbuf);
    if (outbuf->freeze_end) {
        mm_free(chain);
        goto done;
    }
    evbuffer_chain_insert(outbuf, chain);
    outbuf->n_add_for_cb += datlen;

    evbuffer_invoke_callbacks(outbuf);

    result = 0;
done:
    EVBUFFER_UNLOCK(outbuf);

    return result;
}

int evbuffer_remove_cb_entry(struct evbuffer *buffer, struct evbuffer_cb_entry *ent)
{
    EVBUFFER_LOCK(buffer);
    TAILQ_REMOVE(&buffer->callbacks, ent, next);
    EVBUFFER_UNLOCK(buffer);
    mm_free(ent);
    return 0;
}

struct evbuffer_cb_entry *evbuffer_add_cb(struct evbuffer *buffer, evbuffer_cb_func cb, void *cbarg)
{
    struct evbuffer_cb_entry *e;
    if (! (e = (struct evbuffer_cb_entry *)mm_calloc(1, sizeof(struct evbuffer_cb_entry))))
        return NULL;
    EVBUFFER_LOCK(buffer);
    e->cb.cb_func = cb;
    e->cbarg = cbarg;
    e->flags = EVBUFFER_CB_ENABLED;
    TAILQ_INSERT_HEAD(&buffer->callbacks, e, next);
    EVBUFFER_UNLOCK(buffer);
    return e;
}

int evbuffer_remove_cb(struct evbuffer *buffer, evbuffer_cb_func cb, void *cbarg)
{
    struct evbuffer_cb_entry *cbent;
    int result = -1;
    EVBUFFER_LOCK(buffer);
    TAILQ_FOREACH(cbent, &buffer->callbacks, next) {
        if (cb == cbent->cb.cb_func && cbarg == cbent->cbarg) {
            result = evbuffer_remove_cb_entry(buffer, cbent);
            goto done;
        }
    }
done:
    EVBUFFER_UNLOCK(buffer);
    return result;
}

int evbuffer_cb_set_flags(struct evbuffer *buffer, struct evbuffer_cb_entry *cb, uint32_t flags)
{
    flags &= ~EVBUFFER_CB_INTERNAL_FLAGS;
    EVBUFFER_LOCK(buffer);
    cb->flags |= flags;
    EVBUFFER_UNLOCK(buffer);
    return 0;
}

int evbuffer_cb_clear_flags(struct evbuffer *buffer, struct evbuffer_cb_entry *cb, uint32_t flags)
{
    flags &= ~EVBUFFER_CB_INTERNAL_FLAGS;
    EVBUFFER_LOCK(buffer);
    cb->flags &= ~flags;
    EVBUFFER_UNLOCK(buffer);
    return 0;
}

#define EVBUFFER_MAX_READ	4096

static int get_n_bytes_readable_on_socket(int fd)
{
    int n = EVBUFFER_MAX_READ;
    if (ioctl(fd, FIONREAD, &n) < 0)
        return -1;
    return n;
}

static inline int evbuffer_write_sendfile(struct evbuffer *buffer, int fd, ssize_t howmuch)
{
    struct evbuffer_chain *chain = buffer->first;
    struct evbuffer_chain_fd *info = EVBUFFER_CHAIN_EXTRA(struct evbuffer_chain_fd, chain);

    ssize_t res;
    off_t offset = chain->misalign;


    ASSERT_EVBUFFER_LOCKED(buffer);

    res = sendfile(fd, info->fd, &offset, chain->off);
    if (res == -1 && EVUTIL_ERR_RW_RETRIABLE(errno)) {
        return (0);
    }
    return (res);

}

static inline int evbuffer_write_iovec(struct evbuffer *buffer, int fd, ssize_t howmuch) {
    struct iovec iov[DEFAULT_WRITE_IOVEC];
    struct evbuffer_chain *chain = buffer->first;
    int n, i = 0;

    if (howmuch < 0)
        return -1;

    ASSERT_EVBUFFER_LOCKED(buffer);

    while (chain != NULL && i < DEFAULT_WRITE_IOVEC && howmuch) {
        /* 我们无法通过writev写入文件信息 */
        if (chain->flags & EVBUFFER_SENDFILE)
            break;
        iov[i].iov_base = (void *) (chain->buffer + chain->misalign);
        if ((size_t) howmuch >= chain->off) {//跨多个节点
            iov[i++].iov_len = (size_t) chain->off;//注意这儿是i++，iov[i]的值为chain->off
            howmuch -= chain->off;
        }
        else {
            iov[i++].iov_len = (size_t) howmuch;
            break;
        }
        chain = chain->next;
    }
    if (!i)
        return 0;
    n = writev(fd, iov, i);
    return (n);
}

#define NUM_READ_IOVEC 4
int evbuffer_read(struct evbuffer *buf, int fd, int howmuch)
{
    struct evbuffer_chain **chainp;
    int n;
    int result;
    int nvecs, i, remaining;

    EVBUFFER_LOCK(buf);

    if (buf->freeze_end) {
        result = -1;
        goto done;
    }

    n = get_n_bytes_readable_on_socket(fd);
    if (n <= 0 || n > EVBUFFER_MAX_READ)
        n = EVBUFFER_MAX_READ;
    if (howmuch < 0 || howmuch > n)
        howmuch = n;

    /* 预留空间 */
	if (evbuffer_expand_fast(buf, howmuch, NUM_READ_IOVEC) == -1) {
		result = -1;
		goto done;
	}
    else {
		struct iovec vecs[NUM_READ_IOVEC];
		nvecs = evbuffer_read_setup_vecs(buf, howmuch, vecs,NUM_READ_IOVEC, &chainp, 1);

		n = readv(fd, vecs, nvecs);
	}

    if (n == -1) {
        result = -1;
        goto done;
    }
    if (n == 0) {
        result = 0;
        goto done;
    }

    remaining = n;
	for (i = 0; i < nvecs; ++i) {
		size_t space = (size_t) CHAIN_SPACE_LEN(*chainp);
		if (space > EVBUFFER_CHAIN_MAX)
			space = EVBUFFER_CHAIN_MAX;
		if ((ssize_t)space < remaining) {
			(*chainp)->off += space;
			remaining -= (int)space;
		}
        else {
			(*chainp)->off += remaining;
			buf->last_with_datap = chainp;
			break;
		}
		chainp = &(*chainp)->next;
	}
    buf->total_len += n;
    buf->n_add_for_cb += n;

    evbuffer_invoke_callbacks(buf);
    result = n;
done:
    EVBUFFER_UNLOCK(buf);
    return result;
}


int evbuffer_write_atmost(struct evbuffer *buffer, int fd, ssize_t howmuch)
{
    int n = -1;

    EVBUFFER_LOCK(buffer);

    if (buffer->freeze_start) {
        goto done;
    }

    if (howmuch < 0 || (size_t)howmuch > buffer->total_len)
        howmuch = buffer->total_len;

    if (howmuch > 0) {
        struct evbuffer_chain *chain = buffer->first;
		if (chain != NULL && (chain->flags & EVBUFFER_SENDFILE))
			n = evbuffer_write_sendfile(buffer, fd, howmuch);
		else
        {
            n = evbuffer_write_iovec(buffer, fd, howmuch);
        }
    }

    if (n > 0)
        evbuffer_drain(buffer, n);

done:
    EVBUFFER_UNLOCK(buffer);
    return (n);
}

int evbuffer_write(struct evbuffer *buffer, int fd)
{
    return evbuffer_write_atmost(buffer, fd, -1);
}

int evbuffer_add_file(struct evbuffer *outbuf, int fd, off_t offset, off_t length)
{
    struct evbuffer_chain *chain;
	struct evbuffer_chain_fd *info;
    int sendfile_okay = 1;

    int ok = 1;

    if (offset < 0 || length < 0 ||
        ((uint64_t)length > EVBUFFER_CHAIN_MAX) ||
        (uint64_t)offset > (uint64_t)(EVBUFFER_CHAIN_MAX - length))
        return (-1);

    if (use_sendfile) {
		EVBUFFER_LOCK(outbuf);
		sendfile_okay = outbuf->flags & EVBUFFER_FLAG_DRAINS_TO_FD;
		EVBUFFER_UNLOCK(outbuf);
	}

	if (use_sendfile && sendfile_okay) {
		chain = evbuffer_chain_new(sizeof(struct evbuffer_chain_fd));
		if (chain == NULL) {
			event_warn("%s: out of memory", __func__);
			return (-1);
		}

		chain->flags |= EVBUFFER_SENDFILE | EVBUFFER_IMMUTABLE;
		chain->buffer = NULL;	/* no reading possible */
		chain->buffer_len = length + offset;
		chain->off = length;
		chain->misalign = offset;

		info = EVBUFFER_CHAIN_EXTRA(struct evbuffer_chain_fd, chain);
		info->fd = fd;

		EVBUFFER_LOCK(outbuf);
		if (outbuf->freeze_end) {
			mm_free(chain);
			ok = 0;
		} else {
			outbuf->n_add_for_cb += length;
			evbuffer_chain_insert(outbuf, chain);
		}
	}
    else if (use_mmap) {
		void *mapped = mmap(NULL, length + offset, PROT_READ, MAP_FILE | MAP_PRIVATE, fd, 0);
		if (mapped == MAP_FAILED) {
			event_warn("%s: mmap(%d, %d, %zu) failed", __func__, fd, 0, (size_t)(offset + length));
			return (-1);
		}
		chain = evbuffer_chain_new(sizeof(struct evbuffer_chain_fd));
		if (chain == NULL) {
			event_warn("%s: out of memory", __func__);
			munmap(mapped, length);
			return (-1);
		}

		chain->flags |= EVBUFFER_MMAP | EVBUFFER_IMMUTABLE;
		chain->buffer = (u_char*)mapped;
		chain->buffer_len = length + offset;
		chain->off = length + offset;

		info = EVBUFFER_CHAIN_EXTRA(struct evbuffer_chain_fd, chain);
		info->fd = fd;

		EVBUFFER_LOCK(outbuf);
		if (outbuf->freeze_end) {
			info->fd = -1;
			evbuffer_chain_free(chain);
			ok = 0;
		} else {
			outbuf->n_add_for_cb += length;

			evbuffer_chain_insert(outbuf, chain);

			/* 删除不需要的*/
			evbuffer_drain(outbuf, offset);
		}
	}
    else
    {
        struct evbuffer *tmp = evbuffer_new();
        ssize_t read;

        if (tmp == NULL)
            return (-1);

        if (lseek(fd, offset, SEEK_SET) == -1) {
            evbuffer_free(tmp);
            return (-1);
        }

        while (length) {
            ssize_t to_read = length > EV_SSIZE_MAX ? EV_SSIZE_MAX : (ssize_t)length;
            read = evbuffer_read(tmp, fd, to_read);
            if (read == -1) {
                evbuffer_free(tmp);
                return (-1);
            }

            length -= read;
        }

        EVBUFFER_LOCK(outbuf);
        if (outbuf->freeze_end) {
            evbuffer_free(tmp);
            ok = 0;
        }
        else {
            evbuffer_add_buffer(outbuf, tmp);
            evbuffer_free(tmp);

            close(fd);
        }
    }

    if (ok)
        evbuffer_invoke_callbacks(outbuf);
    EVBUFFER_UNLOCK(outbuf);

    return ok ? 0 : -1;
}

int evbuffer_read_setup_vecs(struct evbuffer *buf, ssize_t howmuch, struct iovec *vecs, int n_vecs_avail,
                          struct evbuffer_chain ***chainp, int exact)
{
    struct evbuffer_chain *chain;
    struct evbuffer_chain **firstchainp;
    size_t so_far;
    int i;
    ASSERT_EVBUFFER_LOCKED(buf);

    if (howmuch < 0)
        return -1;

    so_far = 0;//节点可用空间字节数和
    /* 找到第一个可用的节点 */
    firstchainp = buf->last_with_datap;
    if (CHAIN_SPACE_LEN(*firstchainp) == 0) {
        firstchainp = &(*firstchainp)->next;
    }

    chain = *firstchainp;
    for (i = 0; i < n_vecs_avail && so_far < (size_t)howmuch; ++i) {
        size_t avail = (size_t) CHAIN_SPACE_LEN(chain);//当前节点可用空间
        if (avail > (howmuch - so_far) && exact)
            avail = howmuch - so_far;
        vecs[i].iov_base = CHAIN_SPACE_PTR(chain);
        vecs[i].iov_len = avail;
        so_far += avail;
        chain = chain->next;
    }

    *chainp = firstchainp;//记录使用的第一个节点
    return i;//返回使用的节点数
}
