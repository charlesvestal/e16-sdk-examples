/*
 * A test app for app mode's internal-flash guard (src/apps/app_mode.c's
 * MPU setup): it writes the first unlock key to the flash controller's
 * key register, FLASH->KEYR at 0x40023C04. With the guard in place that
 * store faults (MemManage), and the launcher comes back with "last reset
 * was a fault at PC=..." and "back from app flashpoke (fault)".
 *
 * Harmless even without the guard: unlocking needs a second key, which
 * this never writes, so the flash stays locked; the app then logs that
 * the guard is missing and exits with code 1.
 */
#include "e16_app.h"

#define FLASH_KEYR 0x40023C04u
#define FLASH_KEY1 0x45670123u

int app_main(const e16_api_t *api)
{
    api->log("writing the flash controller's key register");
    api->delay_ms(1500); /* lets USB come up and the line reach the log */
    *(volatile unsigned *)FLASH_KEYR = FLASH_KEY1;
    api->log("no fault: the internal-flash guard is missing");
    api->delay_ms(50);
    return 1;
}
