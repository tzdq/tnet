#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include "event2/event.h"
#include "evmemory.h"

//内存管理相关的代码
static void *(*mm_malloc_fn)(size_t sz) = NULL;
static void *(*mm_realloc_fn)(void *p, size_t sz) = NULL;
static void (*mm_free_fn)(void *p) = NULL;

void *event_mm_malloc(size_t sz)
{
    if(sz == 0)return NULL;
    if(mm_malloc_fn)return mm_malloc_fn(sz);
    return malloc(sz);
}

void *event_mm_calloc(size_t count, size_t size)
{
    if(count == 0 || size == 0)return NULL;
    if(mm_malloc_fn){
        size_t  sz = count * size;
        void *p = NULL;
        if(count >  EV_SIZE_MAX / size)
            goto error;
        p = mm_malloc_fn(sz);
        if(p)return memset(p,0,sz);
    }
    else{
        void *p = calloc(count,size);
        return p;
    }
    error:
    errno = ENOMEM;
    return NULL;
}

char *event_mm_strdup(const char *str)
{
    if(!str){
        errno = EINVAL;
        return NULL;
    }

    if(mm_malloc_fn){
        size_t ln = strlen(str);
        void *p = NULL;
        if(ln == EV_SIZE_MAX)
            goto error;
        p = mm_malloc_fn(ln+1);
        if(p)return (char *)memcpy(p,str,ln+1);
    }
    else{
        return strdup(str);
    }
    error:
    errno = ENOMEM;
    return NULL;
}

void *event_mm_realloc(void *ptr, size_t sz)
{
    if (mm_realloc_fn)
        return mm_realloc_fn(ptr, sz);
    else
        return realloc(ptr, sz);
}

void event_mm_free(void *ptr)
{
    if (mm_free_fn)
        mm_free_fn(ptr);
    else
        free(ptr);
}

void event_set_mem_functions(void *(*malloc_fn)(size_t sz), void *(*realloc_fn)(void *ptr, size_t sz), void (*free_fn)(void *ptr))
{
    mm_malloc_fn = malloc_fn;
    mm_realloc_fn = realloc_fn;
    mm_free_fn = free_fn;
}
