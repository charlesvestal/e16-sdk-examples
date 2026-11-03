/*
 * SRAM addresses for mem_layout.h's two modes.
 */
#include "mem_layout.h"
#include "stack_guard.h"

/* Linker symbols (the SDK's e16_app.ld): the end of .data + .bss, and the
 * initial stack pointer. Arrays of unknown bound: only their addresses
 * mean anything. */
extern uint8_t _ebss[];
extern uint8_t _estack[];

uint8_t *mem_sram_free_base(void)
{
    return (uint8_t *)(((uintptr_t)_ebss + 7u) & ~(uintptr_t)7u);
}

uint8_t *mem_sram_top(void)
{
    return _estack;
}

uint8_t *mem_normal_heap_base(void)
{
    return mem_sram_free_base() + LUA_SRAM_POOL_BYTES;
}

uint8_t *mem_app_heap_base(void)
{
    return _estack - APP_STACK_BYTES - STACK_GUARD_SIZE - STACK_GUARD_NEWLIB_HEAP;
}
