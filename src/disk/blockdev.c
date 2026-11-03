/*
 * 512-byte blocks over a flash_ops_t region, through a one-sector (4 KB)
 * write-back cache.
 *
 * The cache holds at most one sector at a time. A read or write first makes
 * sure the sector it touches is cached (load_sector()): if a *different*
 * sector is cached and dirty, that one is flushed first (erase, write,
 * read-back verify), then the new sector is read in. A write only ever
 * touches the cache and sets it dirty; it never erases or writes flash by
 * itself -- only blockdev_flush() (directly, or via load_sector() making
 * room for another sector) does that.
 *
 * flush_cache() skips the erase + write entirely when the cached sector is
 * already byte-for-byte what's on flash (a plain compare, read in 256-byte
 * chunks): the main place this matters is a sector that was only ever read,
 * never written, so it was never actually marked dirty and this never
 * triggers -- but it also covers a write that puts back exactly what was
 * already there.
 *
 * All addresses handed to *ops are absolute (base-relative, not disk- or
 * sector-relative): flash_ops_t and its implementations (storage.h on the
 * device, the fake in test/disk/blockdev_test.c) don't know anything about
 * this module's base/size.
 */
#include "blockdev.h"

#include <assert.h>
#include <string.h>

#include "disk_fatal.h"
#include "log.h"

#define BLOCKS_PER_SECTOR (FLASH_SECTOR_SIZE / BLOCKDEV_BLOCK_SIZE)

static const flash_ops_t *g_ops;
static uint32_t g_base;
static uint32_t g_size;
static uint8_t *g_cache;
static uint32_t g_cache_sector; /* index, relative to g_base, of the cached sector */
static bool g_cache_valid;
static bool g_cache_dirty;
static uint32_t g_erase_count;
static blockdev_error_t g_last_error;

/*
 * Every address blockdev.c hands to *ops is g_base plus an offset derived
 * from a public call's lba/count, which blockdev_read()/blockdev_write()
 * already bounds-check against blockdev_blocks() before any of this runs.
 * So this can't actually fail -- but if the arithmetic above it is ever
 * wrong, writing or erasing the wrong flash address is a silent corruption,
 * not just a bad return value, so this stays active even when assert() is
 * compiled out (NDEBUG, e.g. the firmware's MinSizeRel build). disk_fatal()
 * (disk_fatal.h) is the portable half of persist.c's original pattern: it
 * aborts on the host, and on the device logs and reboots into the
 * bootloader instead of hanging with no USB -- this file is shared with
 * the host build, so it can't call system_enter_bootloader() directly.
 */
static void require_in_bounds(uint32_t addr, uint32_t len)
{
    assert(addr >= g_base && (uint64_t)addr + len <= (uint64_t)g_base + g_size);
    if (addr < g_base || (uint64_t)addr + len > (uint64_t)g_base + g_size) {
        log_printf("blockdev: BUG: address 0x%08lx+%lu outside [0x%08lx, 0x%08lx)",
                   (unsigned long)addr, (unsigned long)len, (unsigned long)g_base,
                   (unsigned long)(g_base + g_size));
        disk_fatal("blockdev: address outside bounds");
    }
}

static bool bounds_ok(uint32_t lba, uint32_t count)
{
    uint64_t end = (uint64_t)lba + (uint64_t)count;
    return end <= (uint64_t)(g_size / BLOCKDEV_BLOCK_SIZE);
}

static uint32_t sector_addr(uint32_t sector_index)
{
    return g_base + sector_index * FLASH_SECTOR_SIZE;
}

/* True if g_cache already matches what's on flash at addr (so a flush can
 * skip erase + write). Reads flash in small chunks rather than needing a
 * whole second 4 KB buffer. */
static bool sector_matches_flash(uint32_t addr)
{
    uint8_t chunk[256];
    _Static_assert(FLASH_SECTOR_SIZE % sizeof chunk == 0, "chunk must evenly divide a sector");
    for (uint32_t off = 0; off < FLASH_SECTOR_SIZE; off += sizeof chunk) {
        g_ops->read(addr + off, chunk, sizeof chunk);
        if (memcmp(chunk, g_cache + off, sizeof chunk) != 0) {
            return false;
        }
    }
    return true;
}

static bool flush_cache(void)
{
    if (!g_cache_valid || !g_cache_dirty) {
        return true;
    }
    uint32_t addr = sector_addr(g_cache_sector);
    require_in_bounds(addr, FLASH_SECTOR_SIZE);

    if (sector_matches_flash(addr)) {
        g_cache_dirty = false;
        return true;
    }

    if (!g_ops->erase_sector(addr)) {
        /* Refused (by the erase-rate budget, flash_budget.h, in practice):
         * the cache stays dirty, nothing was written, and the caller may
         * retry -- disk_ops.c's flush/read/write retries do, for a bounded
         * time. */
        g_last_error = BLOCKDEV_ERR_ERASE;
        return false;
    }
    g_erase_count++;
    g_ops->write(addr, g_cache, FLASH_SECTOR_SIZE);

    if (!sector_matches_flash(addr)) {
        log_printf("blockdev: write verify failed at 0x%08lx", (unsigned long)addr);
        g_last_error = BLOCKDEV_ERR_VERIFY;
        return false; /* still dirty: a later flush will try again */
    }
    g_cache_dirty = false;
    return true;
}

/* Makes sure sector_index is the cached sector, flushing whatever was
 * cached before (if it was a different, dirty sector) to make room. */
static bool load_sector(uint32_t sector_index)
{
    if (g_cache_valid && g_cache_sector == sector_index) {
        return true;
    }
    if (!flush_cache()) {
        return false;
    }
    uint32_t addr = sector_addr(sector_index);
    require_in_bounds(addr, FLASH_SECTOR_SIZE);
    g_ops->read(addr, g_cache, FLASH_SECTOR_SIZE);
    g_cache_sector = sector_index;
    g_cache_valid = true;
    g_cache_dirty = false;
    return true;
}

void blockdev_init(const flash_ops_t *ops, uint32_t base, uint32_t size, uint8_t *cache4k)
{
    assert(base % FLASH_SECTOR_SIZE == 0);
    assert(size % FLASH_SECTOR_SIZE == 0);
    g_ops = ops;
    g_base = base;
    g_size = size;
    g_cache = cache4k;
    g_cache_sector = 0;
    g_cache_valid = false;
    g_cache_dirty = false;
    g_erase_count = 0;
    g_last_error = BLOCKDEV_OK;
}

uint32_t blockdev_blocks(void)
{
    return g_size / BLOCKDEV_BLOCK_SIZE;
}

bool blockdev_read(uint32_t lba, uint32_t count, void *buf)
{
    if (count == 0) {
        return true;
    }
    if (!bounds_ok(lba, count)) {
        g_last_error = BLOCKDEV_ERR_RANGE;
        return false;
    }
    uint8_t *out = buf;
    uint32_t block = lba;
    uint32_t remaining = count;
    while (remaining > 0) {
        uint32_t sector_index = block / BLOCKS_PER_SECTOR;
        uint32_t offset_in_sector = (block % BLOCKS_PER_SECTOR) * BLOCKDEV_BLOCK_SIZE;
        if (!load_sector(sector_index)) {
            return false;
        }
        uint32_t this_sector = BLOCKS_PER_SECTOR - (block % BLOCKS_PER_SECTOR);
        if (this_sector > remaining) {
            this_sector = remaining;
        }
        memcpy(out, g_cache + offset_in_sector, (size_t)this_sector * BLOCKDEV_BLOCK_SIZE);
        out += (size_t)this_sector * BLOCKDEV_BLOCK_SIZE;
        block += this_sector;
        remaining -= this_sector;
    }
    return true;
}

bool blockdev_write(uint32_t lba, uint32_t count, const void *buf)
{
    if (count == 0) {
        return true;
    }
    if (!bounds_ok(lba, count)) {
        g_last_error = BLOCKDEV_ERR_RANGE;
        return false;
    }
    const uint8_t *in = buf;
    uint32_t block = lba;
    uint32_t remaining = count;
    while (remaining > 0) {
        uint32_t sector_index = block / BLOCKS_PER_SECTOR;
        uint32_t offset_in_sector = (block % BLOCKS_PER_SECTOR) * BLOCKDEV_BLOCK_SIZE;
        if (!load_sector(sector_index)) {
            return false;
        }
        uint32_t this_sector = BLOCKS_PER_SECTOR - (block % BLOCKS_PER_SECTOR);
        if (this_sector > remaining) {
            this_sector = remaining;
        }
        memcpy(g_cache + offset_in_sector, in, (size_t)this_sector * BLOCKDEV_BLOCK_SIZE);
        g_cache_dirty = true;
        in += (size_t)this_sector * BLOCKDEV_BLOCK_SIZE;
        block += this_sector;
        remaining -= this_sector;
    }
    return true;
}

bool blockdev_flush(void)
{
    return flush_cache();
}

bool blockdev_dirty(void)
{
    return g_cache_valid && g_cache_dirty;
}

blockdev_error_t blockdev_last_error(void)
{
    return g_last_error;
}

uint32_t blockdev_erase_count(void)
{
    return g_erase_count;
}
