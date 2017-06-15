/*
 * 如果要定制自己的内存分配函数，开始配置编译Libevent时，不能加入--disable-malloc-replacement选项。默认情况下，是没有这个选项的。
 *
 * 替换内存管理函数影响libevent 随后的所有分配、调整大小和释放内存操作。所以必须保证在调用任何其他libevent函数之前进行定制。否则，Libevent可能用定制的free函数释放C语言 库的malloc函数分配的内存
 * malloc和realloc函数返回的内存块应该具有和C库返回的内存块一样的地址对齐
 * realloc函数应该正确处理realloc(NULL, sz)（也就是当作malloc(sz)处理）
 * realloc函数应该正确处理realloc(ptr, 0)（也就是当作free(ptr)处理）
 * 如果在多个线程中使用libevent，替代的内存管理函数需要是线程安全的
 * 如果要释放由Libevent函数分配的内存，并且已经定制了malloc和realloc函数，那么就应该使用定制的free函数释放。否则将会C语言标准库的free函数释放定制内存分配函数分配的内存，这将发生错误。所以三者要么全部不定制，要么全部定制。
 */

#ifndef TNET_MM_INTERNAL_H
#define TNET_MM_INTERNAL_H

#include <sys/types.h>

void *event_mm_malloc(size_t sz);
void *event_mm_calloc(size_t count, size_t size);
char *event_mm_strdup(const char *str);
void *event_mm_realloc(void *ptr, size_t sz);
void event_mm_free(void *ptr);

#define mm_malloc(sz) event_mm_malloc(sz)
#define mm_calloc(count, size) event_mm_calloc((count), (size))
#define mm_strdup(s) event_mm_strdup(s)
#define mm_realloc(ptr, sz) event_mm_realloc((ptr), (sz))
#define mm_free(ptr) event_mm_free(ptr)

#endif //TNET_MM_INTERNAL_H
