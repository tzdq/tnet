/*
 * 网络通信中接受和发送数据的缓冲区
 * evbuffer可用于将数据发送到网络之前准备数据，或者从网络读取数据。
 * evbuffers尽可能避免内存副本，evbuffers用于传递数据，而不会产生复制数据的开销。
 * 使用evbuffer_new()分配一个新的evbuffer，使用evbuffer_free()来释放它。大多数用户将通过bufferevent接口使用evbuffers。使用bufferevent_get_input()/bufferevent_get_output()访问bufferevent的evbuffers。

 * 使用evbuffers有几个准则。
    如果已经知道多次调用evbuffer_add()添加的数据量大小，则首先使用evbuffer_expand()来手动分配足够的内存是有意义的。
    - evbuffer_add_buffer()将一个缓冲区的内容添加到另一个缓冲区，而不会引起任何不必要的内存副本。
    - evbuffer_add()和evbuffer_add_buffer()混合使用就会在缓冲区中使用分散的内存。
    - 避免将数据复制到缓冲区和从缓冲区中复制：写入缓冲区时使用evbuffer_reserve_space()/ evbuffer_commit_space()可以跳过复制步骤，从缓冲区读取时使用evbuffer_peek()可以跳过复制步骤。

 * evbuffers使用内存块的链表来表示，指针指向链中的第一个和最后一个块。

 * 由于evbuffer的内容可以存储在多个不同的内存块中，因此无法直接访问。相反，evbuffer_pullup（）可以用来强制指定数量的字节连续。如果数据跨多个块分割，这将导致内存重新分配和内存副本。但是，如果不要求内存是连续的，则使用evbuffer_peek（）更有效。
*/

#ifndef TNET_BUFFER_H
#define TNET_BUFFER_H

#include <stdarg.h>
#include <sys/types.h>
#include <sys/uio.h>
#include "util.h"

/*
 * 指向evbuffer内的一个位置。在反复搜索缓冲区时使用。
 * 调用任何修改或重新打包缓冲区内容的函数可能会使该缓冲区的所有evbuffer_ptrs失效。 除了evbuffer_ptr_set之外，不要修改这些值。
 * evbuffer_ptr可以表示从缓冲区开始到缓冲区结束之后的位置的任何位置。
 */
struct evbuffer_ptr {
    ssize_t pos;//总偏移量，相对于数据的开始位置
    struct {
        void *chain;//指明是哪个evbuffer_chain
        size_t pos_in_chain;//在evbuffer_chain中的偏移量,实际偏移量为：chain->buffer+ chain->misalign + pos_in_chain。
    } internal;
};

/* 为一个新的buffer分配内存 */
struct evbuffer *evbuffer_new(void);

/* 释放给定buffer的内存*/
void evbuffer_free(struct evbuffer *buf);

/** 启用对evbuffer的锁定，保证线程安全，如果lock为空，创建一个。启用锁定时，将在调用回调时保持锁定。 如果不小心，可能会导致死锁。*/
int evbuffer_enable_locking(struct evbuffer *buf, void *lock);

/** 对evbuffer加锁 */
void evbuffer_lock(struct evbuffer *buf);

/** 对evbuffer解锁*/
void evbuffer_unlock(struct evbuffer *buf);

#define EVBUFFER_FLAG_DRAINS_TO_FD 1

/* 通过添加更多的内容来更改为evbuffer设置的标志。*/
int evbuffer_set_flags(struct evbuffer *buf, uint64_t flags);

/* 通过删除一些内容来更改为evbuffer设置的标志。*/
int evbuffer_clear_flags(struct evbuffer *buf, uint64_t flags);

/** 获取evbuffer中存储的总字节数*/
size_t evbuffer_get_length(const struct evbuffer *buf);

/** 返回当前第一个块中的字节数*/
size_t evbuffer_get_contiguous_space(const struct evbuffer *buf);

/* 扩展evbuffer的可用空间 ，扩展到至少datlen*/
int evbuffer_expand(struct evbuffer *buf, size_t datlen);

int evbuffer_freeze(struct evbuffer *buf, int at_front);
int evbuffer_unfreeze(struct evbuffer *buf, int at_front);

/** 添加数据到evbuffer中 */
int evbuffer_add(struct evbuffer *buf, const void *data, size_t datlen);

/** 从evbuffer读取数据并删除读取的字节。如果请求的字节比evbuffer中的可用字节多，我们只提取可用的字节数。*/
int evbuffer_remove(struct evbuffer *buf, void *data, size_t datlen);

/** 从evbuffer读取数据，并保持缓冲区不变。 如果请求的字节比evbuffer中的可用字节多，我们只提取可用的字节数。*/
ssize_t evbuffer_copyout(struct evbuffer *buf, void *data_out, size_t datlen);

/** 从evbuffer的开头删除指定数量的字节数据。*/
int evbuffer_drain(struct evbuffer *buf, size_t len);

/** 添加数据到evbuffer前面*/
int evbuffer_prepend(struct evbuffer *buf, const void *data, size_t size);

/** 将所有数据从一个evbuffer移动到另一个evbuffer。不会发生不必要的内存副本.*/
int evbuffer_add_buffer(struct evbuffer *outbuf, struct evbuffer *inbuf);

/** 从src读取datlen字节的数据到dst，并从src中删除。可以避免复制操作。如果请求的字节数多于src中的可用字节，则src缓冲区将被完全排除。*/
int evbuffer_remove_buffer(struct evbuffer *src, struct evbuffer *dst, size_t datlen);

/** 将src所有数据添加到dst evbuffer的开头。*/
int evbuffer_prepend_buffer(struct evbuffer *dst, struct evbuffer* src);

/**
 * 在缓冲区中查找含有len个字符的字符串what。函数返回包含字符串位置，或者在没有找到字符串时包含-1的evbuffer_ptr结构体。如果提供了start参数，则从指定的位置开始搜索；否则，从开始处进行搜索
 */
struct evbuffer_ptr evbuffer_search(struct evbuffer *buffer, const char *what, size_t len, const struct evbuffer_ptr *start);

/** 和evbuffer_search行为相同，只是它只考虑在end之前出现的what。*/
struct evbuffer_ptr evbuffer_search_range(struct evbuffer *buffer, const char *what, size_t len,
                                          const struct evbuffer_ptr *start, const struct evbuffer_ptr *end);

enum evbuffer_ptr_how {
    EVBUFFER_PTR_SET,//偏移量是一个绝对位置
    EVBUFFER_PTR_ADD//偏移量是一个相对位置
};

/**
  操作buffer中的位置pos。position指明移动的偏移量，how指明该偏移量是绝对偏移量还是相对当前位置的偏移量。成功时函数返回0，失败时返回-1。
*/
int evbuffer_ptr_set(struct evbuffer *buffer, struct evbuffer_ptr *ptr, size_t position, enum evbuffer_ptr_how how);

/* 换行符*/
enum evbuffer_eol_style {
    EVBUFFER_EOL_ANY,
    EVBUFFER_EOL_CRLF,
    EVBUFFER_EOL_CRLF_STRICT,
    EVBUFFER_EOL_LF,
    EVBUFFER_EOL_NUL
};

/**
 * 从evbuffer中读取一行，该行数据会自动加上'\0'结尾。如果n_read_out不是NULL，则它被设置为返回的字符串的字节数。如果没有整行供读取，函数返回空。返回的字符串不包括行结束符。

 * evbuffer_readln()理解5种行结束格式：
    l EVBUFFER_EOL_LF：行尾是单个换行符（\n，ASCII值是0x0A）
    2 EVBUFFER_EOL_CRLF_STRICT：行尾是一个回车符，后随一个换行符（\r\n，ASCII值是0x0D 0x0A）
    3 EVBUFFER_EOL_CRLF：行尾是一个可选的回车，后随一个换行符（\r\n或者\n）。
    4 EVBUFFER_EOL_ANY：行尾是任意数量、任意次序的回车和换行符。这种格式不是特别有用。它的存在主要是为了向后兼容。
    5 EVBUFFER_EOL_NUL：行尾是空字符（0）
 */
char *evbuffer_readln(struct evbuffer *buffer, size_t *n_read_out, enum evbuffer_eol_style eol_style);

/** 像evbuffer_readln()一样检测行结束，但不复制行，而是返回指向行结束符的evbuffer_ptr。如果eol_len_out非空，则它被设置为EOL字符串长度 */
struct evbuffer_ptr evbuffer_search_eol(struct evbuffer *buffer, struct evbuffer_ptr *start, size_t *eol_len_out,
                                        enum evbuffer_eol_style eol_style);

/** 一个通过引用添加到evbuffer的内存的清理函数。 */
typedef void (*evbuffer_ref_cleanup_cb)(const void *data, size_t datalen, void *extra);

/**
  这个函数通过引用向evbuffer末尾添加一段数据。不会进行复制：evbuffer只会存储一个到data处的datlen字节的指针。因此，在evbuffer使用这个指针期间，必须保持指针是有效的。evbuffer会在不再需要这部分数据的时候调用用户提供的cleanupfn函数，带有提供的data指针、datlen值和extra指针参数。函数成功时返回0，失败时返回-1
 */
int evbuffer_add_reference(struct evbuffer *outbuf, const void *data, size_t datlen, evbuffer_ref_cleanup_cb cleanupfn,
                           void *cleanupfn_arg);

/**
  evbuffer_add_file()要求一个打开的可读文件描述符fd（不是套接字）。函数将文件中offset处开始的length字节添加到output末尾。成功时函数返回0，失败时返回-1。
*/
int evbuffer_add_file(struct evbuffer *outbuf, int fd, off_t offset, off_t length);

/** 添加一个格式化的数据到evbuffer的末尾*/
int evbuffer_add_printf(struct evbuffer *buf, const char *fmt, ...)__attribute__((format(printf, 2, 3)));
int evbuffer_add_vprintf(struct evbuffer *buf, const char *fmt, va_list ap)__attribute__((format(printf, 2, 0)));

/* 用于网络IO*/
int evbuffer_write(struct evbuffer *buffer, int fd);
int evbuffer_write_atmost(struct evbuffer *buffer, int fd, ssize_t howmuch);
int evbuffer_read(struct evbuffer *buffer, int fd, int howmuch);

/** 传递给evbuffer_cb_func evbuffer回调函数的数据结构 */
struct evbuffer_cb_info {
    size_t orig_size;/** 最后一次调用回调时，该evbuffer中的字节数。 */
    size_t n_added;/** 上次调用回调后添加的字节数。 */
    size_t n_deleted;/* 从上次调用回调以来删除的字节数。*/
};

/**
    evbuffer添加或删除数据时调用的回调函数。evbuffer可能会一次设置一个或多个回调。 执行它们的顺序是未定义的。
    回调函数可能会添加更多回调，或者从回调列表中删除自己，或者从缓冲区添加或删除数据。它可能不会从列表中删除另一个回调。
    如果回调从缓冲区或另一个缓冲区添加或删除数据，则可能导致回调或其他回调的递归调用。
*/
typedef void (*evbuffer_cb_func)(struct evbuffer *buffer, const struct evbuffer_cb_info *info, void *arg);
typedef void (*evbuffer_cb)(struct evbuffer *buffer, size_t old_len, size_t new_len, void *arg);

struct evbuffer_cb_entry;
/**
 * evbuffer_add_cb()函数为evbuffer添加一个回调函数，返回一个不透明的指针，随后可用于代表这个特定的回调实例。cb参数是将被调用的函数，cbarg是用户提供的将传给这个函数的指针。可以为单个evbuffer设置多个回调，添加新的回调不会移除原来的回调。
 */
struct evbuffer_cb_entry *evbuffer_add_cb(struct evbuffer *buffer, evbuffer_cb_func cb, void *cbarg);
int evbuffer_remove_cb_entry(struct evbuffer *buffer, struct evbuffer_cb_entry *ent);
int evbuffer_remove_cb(struct evbuffer *buffer, evbuffer_cb_func cb, void *cbarg);

/* 如果这个标志没有被设置，回调不可用，并且不能被唤醒*/
#define EVBUFFER_CB_ENABLED 1

int evbuffer_cb_set_flags(struct evbuffer *buffer, struct evbuffer_cb_entry *cb, uint32_t flags);
int evbuffer_cb_clear_flags(struct evbuffer *buffer, struct evbuffer_cb_entry *cb, uint32_t flags);

struct event_base;
/* 强制在evbuffer上运行所有的回调,回调在缓冲区更改后不会立即被调用，从event_base的循环中延迟调用。这可以用于将所有回调序列化到单个执行线程。*/
int evbuffer_defer_callbacks(struct evbuffer *buffer, struct event_base *base);
#endif //TNET_BUFFER_H