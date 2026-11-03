/*
 * A test app for app mode's NULL guard (src/apps/app_mode.c's MPU region
 * 2): it writes through a NULL pointer, to NULLPOKE_ADDR (default 0; build
 * with -DNULLPOKE_ADDR=0x100 for a NULL struct's member). With SRAM1
 * remapped at 0 that address aliases the launcher's .data; with the guard
 * in place the store faults (MemManage), and the launcher comes back with
 * "last reset was a fault at PC=..." and "back from app nullpoke (fault)".
 *
 * Without the guard the write lands in the launcher's .data, which app
 * mode doesn't use (the launcher reboots after the app), so the app logs
 * that the guard is missing and exits with code 1.
 */
#include <stdint.h>

#include "e16_app.h"

#ifndef NULLPOKE_ADDR
#define NULLPOKE_ADDR 0u
#endif

int app_main(const e16_api_t *api)
{
    /* Through a volatile, so the compiler can't see the NULL (and turn the
     * store into a trap of its own). */
    volatile uintptr_t addr = NULLPOKE_ADDR;
    api->log("writing through a NULL pointer");
    api->delay_ms(1500); /* lets USB come up and the line reach the log */
    *(volatile unsigned *)addr = 0xDEADBEEFu;
    api->log("no fault: the NULL guard is missing");
    api->delay_ms(50);
    return 1;
}
