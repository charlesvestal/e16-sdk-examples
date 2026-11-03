#ifndef FLASH_E16_H
#define FLASH_E16_H
#include "flash_ops.h"

/*
 * flash_ops_t over the E16 SDK's storage.h (the external SPI NOR). Device
 * build only -- the host build has its own fake (test/disk/blockdev_test.c).
 * storage_init() is the caller's job (persist_init() already calls it for
 * the same chip); this forwards to storage_read/write/erase, refusing
 * (disk_fatal()) any address in the stock firmware's data below 0x400000.
 */
extern const flash_ops_t flash_e16_ops;

#endif
