#ifndef STACK_GUARD_H
#define STACK_GUARD_H
#include <stdbool.h>
#include <stdint.h>

/* Bytes for newlib's malloc (see sbrk.c); the canary follows them. */
#define STACK_GUARD_NEWLIB_HEAP 4096
#define STACK_GUARD_SIZE 256

/*
 * A canary in RAM between newlib's sbrk-based heap (used internally by
 * things like float-to-string conversion, which mallocs a scratch bignum
 * buffer) and the main stack, which grows down from _estack. newlib's
 * heap grows up from `base` via sbrk (capped by sbrk.c at
 * STACK_GUARD_NEWLIB_HEAP); the canary follows it; above that is the
 * stack (mem_layout.h has where `base` is in each mode).
 * stack_guard_init() fills the canary first thing at boot (before
 * anything can call sbrk, which refuses until then);
 * stack_guard_check() verifies it's still intact. If the stack has grown
 * enough to smash it, reboot into the bootloader rather than keep running
 * on corrupted memory.
 */
void stack_guard_init(uint8_t *base);
void stack_guard_check(void);
bool stack_guard_intact(void);

/* newlib's heap: [base, base + STACK_GUARD_NEWLIB_HEAP), or NULL before
 * stack_guard_init(). */
uint8_t *stack_guard_heap_base(void);

/* Stack bytes above the canary never used since stack_guard_init() (the
 * free stack at its high-water mark, approximately). */
unsigned stack_guard_free(void);

#endif
