#include "rt_heap.h"
#include "tlsf.h"
#include "stack_guard.h"

static tlsf_t tlsf;
static size_t total, used;

void rt_heap_init(void *first_pool, size_t size)
{
    tlsf = tlsf_create_with_pool(first_pool, size);
    total = size - tlsf_size();
    used = 0;
}

void rt_heap_add_pool(void *pool, size_t size)
{
    tlsf_add_pool(tlsf, pool, size);
    total += size - tlsf_pool_overhead();
}

void *rt_heap_lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    (void)ud;
    (void)osize;
    stack_guard_check();
    if (nsize == 0) {
        if (ptr) {
            used -= tlsf_block_size(ptr);
            tlsf_free(tlsf, ptr);
        }
        return NULL;
    }
    size_t before = ptr ? tlsf_block_size(ptr) : 0;
    void *p = tlsf_realloc(tlsf, ptr, nsize);
    if (p) {
        used += tlsf_block_size(p) - before;
    }
    return p;
}

/* Upper bound: TLSF's own per-block and per-pool alignment/headers mean the
 * usable bytes are a little less than this. */
size_t rt_heap_total(void) { return total; }
size_t rt_heap_used(void) { return used; }
