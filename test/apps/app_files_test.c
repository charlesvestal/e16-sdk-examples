/*
 * Host test for the app api's file writes (src/apps/app_files.c) over the
 * real FAT volume, block device and erase-rate budget, on a fake NOR flash
 * (11.5 MB at the volume's address, in RAM) and a fake clock.
 *
 * The scenario this guards against: an app rewrites the same small file
 * a few seconds apart. The second write used to be refused an erase by
 * the normal budget's per-sector cap partway through -- after FatFs had
 * truncated the file -- the flush on the way out failed under the same
 * budget, and the reset that followed dropped the cache: a 0-byte file.
 * Now every write either goes through whole or is refused (E16_EBUSY)
 * before anything changes, and after a simulated reset (a fresh block
 * device cache over the same flash, remounted) the file always holds the
 * last write that succeeded -- never 0 bytes.
 *
 * Also: a refused write erases nothing; the exit flush works under the
 * Disk-mode budget even with the normal one spent; and on a fragmented
 * volume a 64 KB write stays well inside APP_FILE_WRITE_OP_MAX (the
 * worst case is printed).
 *
 * Prints "ALL PASSED" and exits 0 on success, like the other C host tests.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_files.h"
#include "blockdev.h"
#include "e16_api.h"
#include "ff.h"
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

/* ---- fake NOR flash and clock ---- */

#define FAKE_BASE VOLUME_FLASH_BASE
#define FAKE_SIZE VOLUME_FLASH_SIZE

static uint8_t *fake_flash;
static uint32_t fake_erases;

static void fake_read(uint32_t addr, void *buf, size_t len)
{
    assert(addr >= FAKE_BASE && (uint64_t)addr + len <= (uint64_t)FAKE_BASE + FAKE_SIZE);
    memcpy(buf, fake_flash + (addr - FAKE_BASE), len);
}

static void fake_write(uint32_t addr, const void *data, size_t len)
{
    assert(addr >= FAKE_BASE && (uint64_t)addr + len <= (uint64_t)FAKE_BASE + FAKE_SIZE);
    const uint8_t *src = data;
    uint8_t *dst = fake_flash + (addr - FAKE_BASE);
    for (size_t i = 0; i < len; i++) {
        dst[i] = (uint8_t)(dst[i] & src[i]);
    }
}

static bool fake_erase_sector(uint32_t addr)
{
    assert(addr % FLASH_SECTOR_SIZE == 0);
    memset(fake_flash + (addr - FAKE_BASE), 0xFF, FLASH_SECTOR_SIZE);
    fake_erases++;
    return true;
}

static const flash_ops_t fake_ops = {fake_read, fake_write, fake_erase_sector};

/* Every reading moves the clock on 1 ms, so the bounded retries end. */
static uint32_t g_now;
static uint32_t now_ms(void)
{
    return ++g_now;
}

static uint8_t g_cache[FLASH_SECTOR_SIZE];

/* A fresh volume (formatted without a budget), then the device's setup: a
 * full normal budget over the fake flash, the block device on top. */
static void fresh_volume(void)
{
    memset(fake_flash, 0xFF, FAKE_SIZE);
    blockdev_init(&fake_ops, FAKE_BASE, FAKE_SIZE, g_cache);
    bool ok = volume_format() && blockdev_flush();
    assert(ok);
    (void)ok;
    g_now = 100000;
    flash_budget_init(&fake_ops, now_ms);
    blockdev_init(&flash_budget_ops, FAKE_BASE, FAKE_SIZE, g_cache);
    ok = volume_mount();
    assert(ok);
    g_now += 100000; /* the bucket is full */
}

/* A reset: the cache (and whatever was only in it) is gone; the next boot
 * mounts what's on flash. */
static void simulated_reset(void)
{
    static uint8_t other_cache[FLASH_SECTOR_SIZE];
    volume_unmount();
    memset(g_cache, 0xA5, sizeof g_cache);
    blockdev_init(&fake_ops, FAKE_BASE, FAKE_SIZE, other_cache);
    bool ok = volume_mount();
    CHECK(ok, "remount after the reset");
}

/* The file's size, and whether it holds exactly want[0..want_len). */
static bool file_is(const char *path, const uint8_t *want, uint32_t want_len, uint32_t *size)
{
    static uint8_t got[E16_FILE_WRITE_MAX + 1];
    *size = 0;
    if (!volume_open(path)) {
        return false;
    }
    *size = volume_open_size();
    int32_t n = volume_read(got, sizeof got);
    volume_close();
    return n == (int32_t)want_len && memcmp(got, want, want_len) == 0;
}

#define DIR "/apps/demo"
#define PATH DIR "/state.bin"

/* The same 200-byte file rewritten every 5 s (the case that used to end
 * as a 0-byte file). */
static void test_rewrites_5s_apart(bool flush_on_exit)
{
    fresh_volume();
    static uint8_t buf[200], last_ok[200];
    bool have_ok = false;
    int ok_count = 0, busy_count = 0;
    for (int i = 0; i < 8; i++) {
        memset(buf, 'a' + i, sizeof buf);
        uint32_t e0 = fake_erases;
        int r = app_files_write(DIR, PATH, buf, sizeof buf);
        CHECK(r == 0 || r == E16_EBUSY, "write %d: 0 or E16_EBUSY (got %d)", i, r);
        if (r == 0) {
            memcpy(last_ok, buf, sizeof buf);
            have_ok = true;
            ok_count++;
        } else if (r == E16_EBUSY) {
            busy_count++;
            CHECK(fake_erases == e0, "a refused write erases nothing (%u)", (unsigned)(fake_erases - e0));
        }
        CHECK(!blockdev_dirty(), "write %d left nothing only in the cache", i);
        g_now += 5000;
    }
    CHECK(ok_count >= 2, "the first two writes, 5 s apart, both go through (%d ok)", ok_count);
    CHECK(busy_count > 0, "eight in 40 s run into the per-sector cap");
    if (flush_on_exit) {
        CHECK(app_files_flush(), "the exit flush");
    }
    simulated_reset();
    uint32_t size = 0;
    CHECK(have_ok && file_is(PATH, last_ok, sizeof last_ok, &size),
          "after the reset the file holds the last good write (size %u)", (unsigned)size);
    CHECK(size != 0, "never a 0-byte file");
    printf("note: 8 rewrites 5 s apart%s: %d written, %d refused (E16_EBUSY)\n",
           flush_on_exit ? "" : " (no exit flush)", ok_count, busy_count);
}

/* Back-to-back writes run the bucket dry: the refused ones change
 * nothing, and the file is the last good one. */
static void test_busy_leaves_file_intact(void)
{
    fresh_volume();
    static uint8_t buf[300], last_ok[300];
    int busy = 0;
    for (int i = 0; i < 6; i++) {
        memset(buf, '0' + i, sizeof buf);
        int r = app_files_write(DIR, PATH, buf, sizeof buf);
        if (r == 0) {
            memcpy(last_ok, buf, sizeof buf);
        } else {
            CHECK(r == E16_EBUSY, "only E16_EBUSY when refused (got %d)", r);
            busy++;
        }
    }
    CHECK(busy > 0, "six writes in a row run into the budget");
    simulated_reset();
    uint32_t size;
    CHECK(file_is(PATH, last_ok, sizeof last_ok, &size), "a refused write left the last good copy (size %u)",
          (unsigned)size);
}

/* The exit flush must work even when the normal budget is spent and the
 * cache is dirty (say, an app's write failed partway for another
 * reason). */
static void test_exit_flush_with_spent_budget(void)
{
    fresh_volume();
    for (uint32_t i = 0; i < 64u; i++) { /* spend the bucket on unused sectors */
        if (!flash_budget_ops.erase_sector(FAKE_BASE + FAKE_SIZE - (i + 1u) * FLASH_SECTOR_SIZE)) {
            break;
        }
    }
    uint8_t blk[BLOCKDEV_BLOCK_SIZE];
    memset(blk, 0x5A, sizeof blk);
    CHECK(blockdev_write(4000, 1, blk), "dirty the cache");
    CHECK(!blockdev_flush(), "the normal budget refuses the flush now");
    CHECK(app_files_flush(), "the exit flush (Disk-mode budget, retried) still writes it");
    CHECK(flash_budget_mode() == FLASH_BUDGET_NORMAL, "and the normal budget is back");
    uint8_t back[BLOCKDEV_BLOCK_SIZE];
    fake_read(FAKE_BASE + 4000u * BLOCKDEV_BLOCK_SIZE, back, sizeof back);
    CHECK(memcmp(back, blk, sizeof blk) == 0, "it's on flash");
}

/* In place: a same-length rewrite costs only its data sector, and
 * shrinking and growing the file keeps exactly the new bytes. */
static void test_rewrite_sizes(void)
{
    fresh_volume();
    static uint8_t buf[9000];
    static const uint32_t sizes[] = {5000, 5000, 100, 9000, 0, 300};
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        memset(buf, 'A' + (int)i, sizes[i]);
        g_now += 120000;
        uint32_t e0 = fake_erases;
        CHECK(app_files_write(DIR, PATH, buf, sizes[i]) == 0, "write %u bytes", (unsigned)sizes[i]);
        if (i == 1) {
            /* 5000 bytes span two flash sectors; FatFs's one-block
             * window can make the first of them go out twice. A replace
             * (truncate, reallocate) took 6+ here: the FAT copies and the
             * directory sector twice each. */
            CHECK(fake_erases - e0 <= 3u, "a same-length rewrite erases data sectors only (%u)",
                  (unsigned)(fake_erases - e0));
        }
        simulated_reset();
        blockdev_init(&flash_budget_ops, FAKE_BASE, FAKE_SIZE, g_cache);
        volume_mount();
        uint32_t size;
        CHECK(file_is(PATH, buf, sizes[i], &size), "after a reset: exactly the %u new bytes (size %u)",
              (unsigned)sizes[i], (unsigned)size);
    }
}

static void test_too_big(void)
{
    fresh_volume();
    static uint8_t buf[8];
    CHECK(app_files_write(DIR, PATH, buf, E16_FILE_WRITE_MAX + 1u) == E16_EARGS, "over E16_FILE_WRITE_MAX");
}

/* Free space in pieces: many files, every other one deleted. 64 KB
 * writes of changing data then hit chains across FAT sectors. */
static void test_fragmented_worst_case(void)
{
    fresh_volume();
    static uint8_t buf[E16_FILE_WRITE_MAX];
    srand(1);
    flash_budget_set_mode(FLASH_BUDGET_DISK); /* the filler isn't what's measured */
    char p[32];
    for (int i = 0; i < 300; i++) {
        snprintf(p, sizeof p, "/f%d.bin", i);
        volume_write_file(p, buf, (size_t)(rand() % 9000), NULL, 0);
    }
    for (int i = 0; i < 300; i += 2) {
        snprintf(p, sizeof p, "/f%d.bin", i);
        f_unlink(p);
    }
    blockdev_flush();
    flash_budget_set_mode(FLASH_BUDGET_NORMAL);

    uint32_t worst = 0;
    int written = 0;
    for (int i = 0; i < 40; i++) {
        uint32_t len = (uint32_t)(rand() % (int)E16_FILE_WRITE_MAX) + 1u;
        for (uint32_t k = 0; k < len; k++) {
            buf[k] = (uint8_t)rand();
        }
        g_now += 120000; /* let the bucket and the sector caps recover */
        uint32_t e0 = fake_erases;
        int r = app_files_write(DIR, i & 1 ? DIR "/a.bin" : DIR "/b.bin", buf, len);
        CHECK(r == 0, "write %d (%u bytes) after a long pause (got %d)", i, (unsigned)len, r);
        uint32_t used = fake_erases - e0;
        if (used > worst) {
            worst = used;
        }
        written += r == 0;
    }
    printf("note: fragmented volume, %d writes up to 64 KB: worst %u erases (op limit %u)\n", written,
           (unsigned)worst, (unsigned)APP_FILE_WRITE_OP_MAX);
    CHECK(worst <= APP_FILE_WRITE_OP_MAX * 2u / 3u, "the worst case keeps a third of the op limit spare (%u)",
          (unsigned)worst);
}

int main(void)
{
    fake_flash = malloc(FAKE_SIZE);
    assert(fake_flash);
    test_rewrites_5s_apart(true);
    test_rewrites_5s_apart(false);
    test_busy_leaves_file_intact();
    test_exit_flush_with_spent_budget();
    test_rewrite_sizes();
    test_too_big();
    test_fragmented_worst_case();
    free(fake_flash);
    if (g_failures) {
        printf("%d FAILED\n", g_failures);
        return 1;
    }
    printf("ALL PASSED\n");
    return 0;
}
