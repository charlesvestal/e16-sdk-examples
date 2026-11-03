#ifndef FLASH_OPS_H
#define FLASH_OPS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The flash primitives blockdev.c and persist_slots.c need, so they can run
 * unchanged over the device's SPI NOR (flash_e16.c, storage.h; on the
 * device wrapped by flash_budget.c's erase-rate limiter) or a host-side
 * fake (test/disk/blockdev_test.c, test/disk/persist_test.c). Same rules
 * as storage.h: write() only turns 1 bits into 0 (call erase_sector()
 * first to get back to all-1s), and erase_sector() always erases exactly
 * one 4 KB-aligned sector -- or refuses to (false, nothing erased) when
 * an erase-rate budget (flash_budget.h) is over budget. A caller must
 * treat false as "try again later", never as a fatal error: it never
 * indicates corruption, only that this erase didn't happen yet.
 */
#define FLASH_SECTOR_SIZE 4096u

typedef struct {
    void (*read)(uint32_t addr, void *buf, size_t len);
    void (*write)(uint32_t addr, const void *buf, size_t len);
    bool (*erase_sector)(uint32_t addr); /* addr must be 4 KB-aligned; false = refused, nothing erased */
} flash_ops_t;

#endif
