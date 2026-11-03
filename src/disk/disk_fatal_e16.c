/* disk_fatal.h, device build: log and reboot into the bootloader, rather
 * than abort()'s hang-with-no-USB -- the same escape persist.c has used
 * since phase 1 (system_enter_bootloader()). Linked into the firmware
 * only (CMakeLists.txt's DISK_SOURCES), never the host build. */
#include "disk_fatal.h"
#include "log.h"
#include "system.h"

void disk_fatal(const char *why)
{
    log_printf("disk: fatal: %s", why);
    system_enter_bootloader();
}
