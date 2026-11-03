#ifndef CCM_H
#define CCM_H

/*
 * CCM_BSS puts a static buffer in the 64 KB core-coupled RAM instead of
 * SRAM, leaving SRAM to the stack, the Lua heap's SRAM pool and whatever
 * a DMA engine has to reach (the CCM is on the CPU's data bus only: the
 * LED driver's DMA buffers, for instance, must stay in SRAM). The display
 * and the SPI flash are written by polling, and USB copies through its
 * own FIFO, so their buffers on our side can live here.
 *
 * .ccmram is NOLOAD: startup code neither zeroes nor initialises it, and
 * a soft reset (a reflash, the dev "reset" SysEx) leaves the previous
 * run's bytes in place. Only use CCM_BSS for a buffer whose owner fills
 * it (or never reads past what it has written, e.g. a ring with its
 * indices in ordinary .bss) before reading it.
 *
 * port_e16.c sizes the Lua heap's CCM pool to what's left of the CCM;
 * moving a buffer here means shrinking that pool by as much (the linker
 * fails the build if .ccmram overflows).
 *
 * On the host (tests) it's empty: plain .bss.
 */
#if defined(__arm__)
#define CCM_BSS __attribute__((section(".ccmram")))
#else
#define CCM_BSS
#endif

#endif
