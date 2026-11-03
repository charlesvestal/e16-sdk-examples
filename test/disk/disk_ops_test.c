/*
 * Host test for src/disk/disk_ops.c: the bounded retries, the Disk-mode
 * bursts and Disk mode's idle write-back, over the real block device,
 * erase-rate budget and FAT volume on a fake NOR flash and a fake clock.
 *
 *   - The first boot's format of non-blank flash takes all of the normal
 *     budget's 16-erase burst (no margin at all, so it now runs as a
 *     bounded Disk-mode burst); disk_format_burst() formats even with the
 *     bucket spent, well inside DISK_FORMAT_MAX_ERASES.
 *   - A flush refused by the budget is retried until the budget allows
 *     it; a verify failure is not retried.
 *   - The idle write-back flushes a dirty cache only once the computer
 *     has been quiet for DISK_IDLE_FLUSH_MS.
 *
 * Prints "ALL PASSED" and exits 0 on success, like the other C host tests.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blockdev.h"
#include "disk_ops.h"
#include "flash_budget.h"
#include "flash_ops.h"
#include "volume.h"

static int g_failures;
#define CHECK(cond, ...)                                                                           \
    do {                                                                                            \
        if (!(cond)) {                                                                              \
            g_failures++;                                                                           \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                    \
            fprintf(stderr, __VA_ARGS__);                                                           \
            fprintf(stderr, "\n");                                                                  \
        }                                                                                           \
    } while (0)

#define FAKE_BASE VOLUME_FLASH_BASE
#define FAKE_SIZE VOLUME_FLASH_SIZE

static uint8_t *fake_flash;
static uint32_t fake_erases;
static bool fake_writes_lost; /* the flash "takes" no writes: verify fails */

static void fake_read(uint32_t addr, void *buf, size_t len)
{
    assert(addr >= FAKE_BASE && (uint64_t)addr + len <= (uint64_t)FAKE_BASE + FAKE_SIZE);
    memcpy(buf, fake_flash + (addr - FAKE_BASE), len);
}

static void fake_write(uint32_t addr, const void *data, size_t len)
{
    assert(addr >= FAKE_BASE && (uint64_t)addr + len <= (uint64_t)FAKE_BASE + FAKE_SIZE);
    if (fake_writes_lost) {
        return;
    }
    const uint8_t *src = data;
    uint8_t *dst = fake_flash + (addr - FAKE_BASE);
    for (size_t i = 0; i < len; i++) {
        dst[i] = (uint8_t)(dst[i] & src[i]);
    }
}

static bool fake_erase_sector(uint32_t addr)
{
    memset(fake_flash + (addr - FAKE_BASE), 0xFF, FLASH_SECTOR_SIZE);
    fake_erases++;
    return true;
}

static const flash_ops_t fake_ops = {fake_read, fake_write, fake_erase_sector};

static uint32_t g_now;
static uint32_t now_ms(void)
{
    return ++g_now; /* moves on 1 ms per reading, so retries end */
}

static uint8_t g_cache[FLASH_SECTOR_SIZE];

static void setup(uint8_t fill)
{
    memset(fake_flash, fill, FAKE_SIZE);
    fake_erases = 0;
    fake_writes_lost = false;
    g_now = 1000;
    flash_budget_init(&fake_ops, now_ms);
    blockdev_init(&flash_budget_ops, FAKE_BASE, FAKE_SIZE, g_cache);
}

/* Erases unused sectors at the top of the volume until the budget
 * refuses (different sectors, so it's the bucket that runs dry, not one
 * sector's cap). */
static void spend_budget(void)
{
    for (uint32_t i = 0; i < 64u; i++) {
        if (!flash_budget_ops.erase_sector(FAKE_BASE + FAKE_SIZE - (i + 1u) * FLASH_SECTOR_SIZE)) {
            break;
        }
    }
}

static void test_format(void)
{
    /* The old way: the normal budget, a full bucket, flash that isn't
     * blank (as on a device whose stock-era contents are still there). */
    setup(0x00);
    bool ok = volume_format() && blockdev_flush();
    uint32_t normal = fake_erases;
    printf("note: formatting non-blank flash takes %u erases (normal burst %u)\n", (unsigned)normal,
           (unsigned)FLASH_BUDGET_NORMAL_BURST);
    CHECK(ok, "it does just fit the normal burst");
    CHECK(normal >= FLASH_BUDGET_NORMAL_BURST - 2u, "...with no real margin (%u of %u)", (unsigned)normal,
          (unsigned)FLASH_BUDGET_NORMAL_BURST);

    /* Now: a bounded Disk-mode burst, even with the bucket spent. */
    setup(0x00);
    spend_budget();
    uint32_t e0 = fake_erases;
    CHECK(disk_format_burst(), "disk_format_burst() with the budget spent");
    uint32_t used = fake_erases - e0;
    CHECK(used <= DISK_FORMAT_MAX_ERASES / 2u, "the format keeps half its %u-erase limit spare (used %u)",
          (unsigned)DISK_FORMAT_MAX_ERASES, (unsigned)used);
    CHECK(flash_budget_mode() == FLASH_BUDGET_NORMAL, "back in the normal budget afterwards");
    CHECK(volume_count_scripts() == 0, "the volume is there, /scripts empty");
    volume_unmount();
}

static void test_retry(void)
{
    setup(0xFF);
    uint8_t blk[BLOCKDEV_BLOCK_SIZE];
    memset(blk, 0x11, sizeof blk);
    spend_budget();
    CHECK(blockdev_write(10, 1, blk), "a write into the cache");
    CHECK(!blockdev_flush(), "the spent budget refuses the flush");
    CHECK(blockdev_last_error() == BLOCKDEV_ERR_ERASE, "as a refused erase");
    uint32_t t0 = g_now;
    CHECK(disk_flush_retry(), "retried until the budget refills");
    CHECK(g_now - t0 >= 900u && g_now - t0 < DISK_RETRY_MS, "which takes about a second (%u ms)",
          (unsigned)(g_now - t0));

    /* A sector the flash won't take: not retried. */
    CHECK(blockdev_write(20, 1, blk), "another write into the cache");
    g_now += 100000;
    fake_writes_lost = true;
    t0 = g_now;
    CHECK(!disk_flush_retry(), "a verify failure fails");
    CHECK(blockdev_last_error() == BLOCKDEV_ERR_VERIFY, "as a verify failure");
    CHECK(g_now - t0 < 50u, "without retrying for %u ms (%u ms)", (unsigned)DISK_RETRY_MS, (unsigned)(g_now - t0));
    uint8_t other[BLOCKDEV_BLOCK_SIZE];
    t0 = g_now;
    CHECK(!disk_rw_retry(false, 5000, other), "a read needing that flush fails the same way");
    CHECK(g_now - t0 < 50u, "also without retrying");
    fake_writes_lost = false;
    CHECK(disk_flush_retry(), "and goes through once the flash takes writes again");

    /* disk_flush_burst(): the Disk-mode budget, then the mode as it was. */
    spend_budget();
    CHECK(blockdev_write(30, 1, blk), "dirty again");
    CHECK(disk_flush_burst(), "disk_flush_burst()");
    CHECK(flash_budget_mode() == FLASH_BUDGET_NORMAL, "back in NORMAL");
    flash_budget_set_mode(FLASH_BUDGET_DISK);
    CHECK(blockdev_write(40, 1, blk), "dirty in Disk mode");
    CHECK(disk_flush_burst(), "disk_flush_burst() in Disk mode");
    CHECK(flash_budget_mode() == FLASH_BUDGET_DISK, "stays in DISK");
}

static void test_idle_writeback(void)
{
    setup(0xFF);
    flash_budget_set_mode(FLASH_BUDGET_DISK);
    uint8_t blk[BLOCKDEV_BLOCK_SIZE];
    memset(blk, 0x22, sizeof blk);
    CHECK(!disk_idle_writeback(5000, 0), "nothing dirty: nothing to do");
    CHECK(blockdev_write(100, 1, blk), "the computer writes a block");
    uint32_t last_io = 10000;
    CHECK(!disk_idle_writeback(last_io + DISK_IDLE_FLUSH_MS - 1u, last_io), "not before it's been quiet");
    CHECK(blockdev_dirty(), "still only in the cache");
    CHECK(disk_idle_writeback(last_io + DISK_IDLE_FLUSH_MS, last_io), "written back once it has");
    CHECK(!blockdev_dirty(), "the cache is clean");
    uint8_t back[BLOCKDEV_BLOCK_SIZE];
    fake_read(FAKE_BASE + 100u * BLOCKDEV_BLOCK_SIZE, back, sizeof back);
    CHECK(memcmp(back, blk, sizeof blk) == 0, "and the block is on flash (a reset now loses nothing)");
}

int main(void)
{
    fake_flash = malloc(FAKE_SIZE);
    assert(fake_flash);
    test_format();
    test_retry();
    test_idle_writeback();
    free(fake_flash);
    if (g_failures) {
        printf("%d FAILED\n", g_failures);
        return 1;
    }
    printf("ALL PASSED\n");
    return 0;
}
