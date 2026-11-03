/* disk_fatal.h, host build: abort(), like an assert that can't be
 * compiled out. Linked into the host tests (test/disk/blockdev_test.c,
 * test/disk/persist_test.c), never into the firmware. */
#include "disk_fatal.h"
#include "log.h"

#include <stdlib.h>

void disk_fatal(const char *why)
{
    log_printf("disk: fatal: %s", why);
    abort();
}
