#include <stdint.h>

#include "stack_guard.h"
#include "log.h"
#include "system.h"

#define GUARD_BYTE 0xA5u

static uint8_t *g_base;

static uint8_t *guard(void)
{
    return g_base + STACK_GUARD_NEWLIB_HEAP;
}

uint8_t *stack_guard_heap_base(void)
{
    return g_base;
}

void stack_guard_init(uint8_t *base)
{
    g_base = base;
    uint8_t *g = guard();
    for (unsigned i = 0; i < STACK_GUARD_SIZE; i++) {
        g[i] = GUARD_BYTE;
    }
    /* Also paint the free stack above the canary (up to a little below
     * the current stack pointer) so stack_guard_free() can report the
     * high-water mark. */
    uint8_t *sp;
    __asm volatile("mov %0, sp" : "=r"(sp));
    uint8_t *top = sp - 64;
    for (uint8_t *p = g + STACK_GUARD_SIZE; p < top; p++) {
        *p = GUARD_BYTE;
    }
}

unsigned stack_guard_free(void)
{
    uint8_t *p = guard() + STACK_GUARD_SIZE;
    unsigned n = 0;
    while (p[n] == GUARD_BYTE) {
        n++;
    }
    return n;
}

bool stack_guard_intact(void)
{
    uint8_t *g = guard();
    for (unsigned i = 0; i < STACK_GUARD_SIZE; i++) {
        if (g[i] != GUARD_BYTE) {
            return false;
        }
    }
    return true;
}

void stack_guard_check(void)
{
    if (!stack_guard_intact()) {
        log_printf("fatal: stack overflow");
        system_enter_bootloader();
    }
}
