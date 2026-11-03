#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include "stack_guard.h"

/*
 * newlib's malloc (used inside printf's float conversion, among others) gets
 * memory from _sbrk. The default one from nosys.specs grows from _ebss with
 * no limit, toward the stack. This one hands out only the
 * STACK_GUARD_NEWLIB_HEAP bytes stack_guard.c reserves below the canary
 * (mem_layout.h: where that is depends on the mode), so a large request
 * fails with ENOMEM instead of overwriting the canary and the stack.
 */
void *_sbrk(ptrdiff_t increment)
{
    static uint8_t *brk;
    uint8_t *base = stack_guard_heap_base();
    if (!base) {
        errno = ENOMEM;
        return (void *)-1;
    }
    if (!brk) {
        brk = base;
    }
    uint8_t *limit = base + STACK_GUARD_NEWLIB_HEAP;
    if (increment > limit - brk || brk + increment < base) {
        errno = ENOMEM;
        return (void *)-1;
    }
    uint8_t *prev = brk;
    brk += increment;
    return prev;
}
