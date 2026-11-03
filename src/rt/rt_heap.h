#ifndef RT_HEAP_H
#define RT_HEAP_H
#include <stddef.h>

/* A lua_Alloc over one or more memory pools (TLSF). Add pools before the first allocation. */
void rt_heap_init(void *first_pool, size_t size);
void rt_heap_add_pool(void *pool, size_t size);
void *rt_heap_lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize);
size_t rt_heap_total(void);   /* bytes managed */
size_t rt_heap_used(void);    /* bytes in use, block overhead included */

#endif
