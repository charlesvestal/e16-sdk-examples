#ifndef BLOCKDEV_H
#define BLOCKDEV_H
#include <stdbool.h>
#include <stdint.h>

#include "flash_ops.h"

/*
 * 512-byte block reads/writes over a region of flash_ops_t, through a
 * single 4 KB write-back cache (one flash sector): blockdev.c has the
 * design notes.
 */
#define BLOCKDEV_BLOCK_SIZE 512u

/* base and size must both be multiples of FLASH_SECTOR_SIZE. cache4k must
 * point at FLASH_SECTOR_SIZE (4096) bytes that stay valid and aren't
 * touched by anything else for as long as the block device is in use --
 * the caller supplies it so it can be carved out of whatever RAM is free
 * (device SRAM, a test's heap, ...). Resets erase_count and leaves the
 * cache empty (not dirty). */
void blockdev_init(const flash_ops_t *ops, uint32_t base, uint32_t size, uint8_t *cache4k);

/* Capacity in 512-byte blocks (size / BLOCKDEV_BLOCK_SIZE). */
uint32_t blockdev_blocks(void);

/* false if [lba, lba + count) runs outside the device, or a flush the read
 * needed to do first failed; the read touches nothing in that case. */
bool blockdev_read(uint32_t lba, uint32_t count, void *buf);

/* false for the same reasons as blockdev_read(), or if flushing a
 * different, dirty cached sector to make room failed (see blockdev_flush()).
 * On success the write lands in the cache and is visible to blockdev_read()
 * right away, whether or not it has reached flash yet. */
bool blockdev_write(uint32_t lba, uint32_t count, const void *buf);

/* Writes out the cached sector if it's dirty: erase, write 4 KB, then read
 * it back to verify. Returns false (and leaves the sector marked dirty) if
 * the verify doesn't match; true if there was nothing to do. Skips the
 * erase and write entirely if the cached sector already matches what's on
 * flash. */
bool blockdev_flush(void);

/* True if the cached sector holds writes not yet on flash. */
bool blockdev_dirty(void);

/* Why the last failed call here failed (unchanged by calls that succeed).
 * BLOCKDEV_ERR_ERASE is the one worth retrying: a sector erase was refused
 * (by the erase-rate budget, flash_budget.h), the cache is still dirty and
 * nothing was written. A verify failure means the flash didn't take what
 * was written; retrying straight away won't help. */
typedef enum {
    BLOCKDEV_OK,
    BLOCKDEV_ERR_RANGE,  /* lba/count outside the device */
    BLOCKDEV_ERR_ERASE,  /* an erase was refused */
    BLOCKDEV_ERR_VERIFY, /* the read-back after a write didn't match */
} blockdev_error_t;
blockdev_error_t blockdev_last_error(void);

/* Sectors erased since blockdev_init(), for tests and bench logging. */
uint32_t blockdev_erase_count(void);

#endif
