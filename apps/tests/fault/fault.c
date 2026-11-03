/*
 * A test app that faults on purpose: it logs a line, then writes to an
 * address with nothing behind it (the vendor-specific top of the address
 * space). The launcher's fault handler should note the fault and reset
 * back into the launcher (which logs "last reset was a fault" and "back
 * from app fault (fault)"), never loop back into the app.
 *
 * Not every unmapped address faults on the E16's GD32 (a read of the
 * FSMC range at 0x60000000 just returns), so if the write somehow
 * doesn't, an undefined instruction does.
 */
#include "e16_app.h"

int app_main(const e16_api_t *api)
{
    api->log("about to write to an unmapped address");
    api->delay_ms(50); /* lets the line reach the log drain */
    *(volatile unsigned *)0xFFFFFF00u = 1u;
    api->log("that didn't fault; trying an undefined instruction");
    api->delay_ms(50);
    __builtin_trap();
}
