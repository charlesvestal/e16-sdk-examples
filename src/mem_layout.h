#ifndef MEM_LAYOUT_H
#define MEM_LAYOUT_H

/*
 * Where everything lives in RAM, in both of the launcher's modes. The one
 * place these numbers are defined: port_e16.c sizes the Lua pools from
 * them, src/apps/app_mode.c loads apps into them, src/boot_records.h puts
 * the reset-surviving records at the top of the CCM, and
 * tools/check_mem_layout.py checks the linked firmware against them after
 * every build (it reads the plain-number #defines below, so keep those
 * plain numbers).
 *
 * CCM, 64 KB at 0x10000000 (CPU data bus only: no DMA, no code):
 *
 *   normal mode   .ccmram: the launcher's non-DMA buffers (CCM_BSS,
 *                 ccm.h) and the Lua heap's CCM pool (port_e16.c)
 *   app mode      the app's GOT, .data and .bss (APP_DATA_BASE/SIZE):
 *                 none of the launcher's .ccmram buffers are used
 *   both          the top CCM_RECORDS_SIZE bytes: records that survive a
 *                 reset (the fault record, the "run this app" record),
 *                 at fixed addresses outside every linker section
 *
 * .ccmram must end at or below CCM_RECORDS_BASE. The linker only knows
 * the whole 64 KB, so tools/check_mem_layout.py fails the build if any
 * CCM symbol reaches the records.
 *
 * SRAM, 128 KB at 0x20000000 (the last word is the bootloader's flag):
 *
 *   [.data/.bss: the resident drivers and the launcher's statics]
 *   then, from _ebss rounded up to 8:
 *   normal mode   [Lua SRAM pool, LUA_SRAM_POOL_BYTES][newlib heap]
 *                 [stack canary][stack, down from _estack]
 *   app mode      [app-mode buffers][app text: code + rodata]
 *                 [newlib heap][stack canary][app stack, APP_STACK_BYTES]
 *
 * The newlib heap and the canary (stack_guard.h) always sit right below
 * the stack, so in both modes only the stack can reach the canary.
 */

#define CCM_BASE 0x10000000u
#define CCM_SIZE 65536u
#define CCM_RECORDS_SIZE 256u
#define CCM_RECORDS_BASE (CCM_BASE + CCM_SIZE - CCM_RECORDS_SIZE)

/* App mode: the app's data + bss. */
#define APP_DATA_BASE CCM_BASE
#define APP_DATA_SIZE (CCM_RECORDS_BASE - CCM_BASE)

/* Normal mode: the .ccmram statics other than the Lua pool, rounded up.
 * The pool gets the rest of the CCM below the records. Grow this (and
 * the pool shrinks) when adding a CCM_BSS buffer; the build check says
 * when it's too small. */
#define CCM_STATIC_BYTES 10752u
#define LUA_CCM_POOL_BYTES (CCM_SIZE - CCM_RECORDS_SIZE - CCM_STATIC_BYTES)

/* Normal mode: the Lua heap's SRAM pool, right after _ebss. Sized to leave
 * real stack headroom above it (port_e16.c has the numbers). */
#define LUA_SRAM_POOL_BYTES 66224u

/* App mode: the app's stack (the main stack, shared with interrupts),
 * at the top of SRAM. */
#define APP_STACK_BYTES 8192u

/* The least normal-mode stack (above the canary) the build accepts. */
#define MIN_STACK_BYTES 24576u

#if defined(__arm__)
#include <stdint.h>
/* SRAM addresses from the linker (mem_layout.c). */
uint8_t *mem_sram_free_base(void); /* _ebss rounded up to 8 */
uint8_t *mem_sram_top(void);       /* _estack */
/* stack_guard_init()'s base in each mode (the newlib heap, then the canary). */
uint8_t *mem_normal_heap_base(void);
uint8_t *mem_app_heap_base(void);
#endif

#endif
