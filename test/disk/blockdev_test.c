/*
 * Host test for src/disk/blockdev.c: a fake flash_ops_t over a RAM array
 * that behaves like NOR (erase -> 0xFF, write only ANDs bits in), plus a
 * reference model of "what the block device should read right now" kept in
 * lockstep with every write. No Lua: plain C, run directly (not through
 * e16rt-host). Prints "ALL PASSED" and exits 0 on success, like the Lua
 * specs test/run_all.sh already runs; exits nonzero otherwise.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blockdev.h"
#include "flash_ops.h"

/* Smaller than the real FAT region (11.5 MB), but still many sectors, and
 * offset-mapped like it (base != 0) to catch an un-based address slipping
 * through. */
#define FAKE_BASE 0x400000u
#define FAKE_SIZE (256u * 1024u)
#define FAKE_BLOCKS (FAKE_SIZE / BLOCKDEV_BLOCK_SIZE)
#define FAKE_SECTORS (FAKE_SIZE / FLASH_SECTOR_SIZE)

static int g_failures;
#define CHECK(cond, ...)                                                                                   \
    do {                                                                                                    \
        if (!(cond)) {                                                                                      \
            g_failures++;                                                                                   \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                             \
            fprintf(stderr, __VA_ARGS__);                                                                    \
            fprintf(stderr, "\n");                                                                           \
        }                                                                                                    \
    } while (0)

/* ---- fake flash: a RAM array that enforces NOR write semantics ---- */

static uint8_t fake_flash[FAKE_SIZE];
static uint32_t fake_erase_count;
static int fake_write_violations; /* a write asked to set a 0 bit to 1 */
static bool fake_corrupt_next_write; /* simulates a flash write that silently didn't take */

static void fake_offset_ok(uint32_t addr, size_t len)
{
    assert(addr >= FAKE_BASE);
    assert((uint64_t)addr + len <= (uint64_t)FAKE_BASE + FAKE_SIZE);
}

static void fake_read(uint32_t addr, void *buf, size_t len)
{
    fake_offset_ok(addr, len);
    memcpy(buf, fake_flash + (addr - FAKE_BASE), len);
}

static void fake_write(uint32_t addr, const void *data, size_t len)
{
    fake_offset_ok(addr, len);
    const uint8_t *src = data;
    uint8_t *dst = fake_flash + (addr - FAKE_BASE);
    for (size_t i = 0; i < len; i++) {
        if ((uint8_t)(src[i] & (uint8_t)~dst[i]) != 0) {
            /* src wants a bit high where flash is already low: NOR can't
             * do that without an erase first. */
            fake_write_violations++;
        }
        dst[i] = (uint8_t)(dst[i] & src[i]);
    }
    if (fake_corrupt_next_write && len > 0) {
        /* Simulate a write that silently didn't take: flip a bit back on
         * (only valid right after an erase, so this doesn't itself trip a
         * write-violation check) so blockdev's own read-back verify sees a
         * mismatch. One-shot. */
        dst[0] ^= 0x01;
        fake_corrupt_next_write = false;
    }
}

static bool fake_erase_sector(uint32_t addr)
{
    fake_offset_ok(addr, FLASH_SECTOR_SIZE);
    assert((addr - FAKE_BASE) % FLASH_SECTOR_SIZE == 0);
    memset(fake_flash + (addr - FAKE_BASE), 0xFF, FLASH_SECTOR_SIZE);
    fake_erase_count++;
    return true;
}

static const flash_ops_t fake_ops = {
    .read = fake_read,
    .write = fake_write,
    .erase_sector = fake_erase_sector,
};

static uint8_t g_cache[FLASH_SECTOR_SIZE];

static void fake_flash_reset(void)
{
    memset(fake_flash, 0xFF, FAKE_SIZE);
    fake_erase_count = 0;
    fake_write_violations = 0;
    blockdev_init(&fake_ops, FAKE_BASE, FAKE_SIZE, g_cache);
}

/* ---- a tiny portable PRNG (fixed seed => a reproducible run) ---- */

static uint32_t rng_state;

static void rng_seed(uint32_t seed)
{
    rng_state = seed ? seed : 1;
}

static uint32_t rng_next(void)
{
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

static uint32_t rng_range(uint32_t lo, uint32_t hi_inclusive) /* [lo, hi] */
{
    return lo + rng_next() % (hi_inclusive - lo + 1);
}

/* ---- tests ---- */

static void test_bounds(void)
{
    fake_flash_reset();
    uint8_t buf[16 * BLOCKDEV_BLOCK_SIZE];

    CHECK(blockdev_read(0, 1, buf), "in-range read at lba 0");
    CHECK(blockdev_read(FAKE_BLOCKS - 1, 1, buf), "in-range read at the last block");
    CHECK(!blockdev_read(FAKE_BLOCKS, 1, buf), "read starting one past the end");
    CHECK(!blockdev_read(FAKE_BLOCKS - 1, 2, buf), "read that runs one block past the end");
    CHECK(!blockdev_read(0, FAKE_BLOCKS + 1, buf), "read longer than the whole device");
    CHECK(!blockdev_read(UINT32_MAX - 2, 8, buf), "read whose lba + count overflows uint32_t");

    CHECK(blockdev_write(0, 1, buf), "in-range write at lba 0");
    CHECK(!blockdev_write(FAKE_BLOCKS, 1, buf), "write starting one past the end");
    CHECK(!blockdev_write(FAKE_BLOCKS - 1, 2, buf), "write that runs one block past the end");
    CHECK(!blockdev_write(0, FAKE_BLOCKS + 1, buf), "write longer than the whole device");
    CHECK(!blockdev_write(UINT32_MAX - 2, 8, buf), "write whose lba + count overflows uint32_t");

    CHECK(blockdev_read(0, 0, buf), "a zero-count read is a trivial success");
    CHECK(blockdev_write(0, 0, buf), "a zero-count write is a trivial success");

    CHECK(fake_write_violations == 0, "no NOR write violations during bounds test");
}

/* 8 consecutive single-block writes landing in the same sector should cost
 * exactly one erase, and only once flushed. */
static void test_one_sector_one_erase(void)
{
    fake_flash_reset();
    uint32_t sector_lba = 3 * (FLASH_SECTOR_SIZE / BLOCKDEV_BLOCK_SIZE); /* sector 3, arbitrary */
    uint8_t block[BLOCKDEV_BLOCK_SIZE];

    for (uint32_t i = 0; i < 8; i++) {
        memset(block, (int)(0x10 + i), sizeof block);
        CHECK(blockdev_write(sector_lba + i, 1, block), "write block %u of the sector", i);
    }
    CHECK(blockdev_dirty(), "cache is dirty before flush");
    CHECK(fake_erase_count == 0, "no erase yet before flush");

    CHECK(blockdev_flush(), "flush succeeds");
    CHECK(!blockdev_dirty(), "cache is clean after flush");
    CHECK(fake_erase_count == 1, "exactly one erase for 8 writes in one sector, got %u",
          fake_erase_count);
    CHECK(blockdev_erase_count() == 1, "blockdev_erase_count() agrees, got %u", blockdev_erase_count());

    /* Read back through a freshly initialized blockdev (forces a reload
     * from the fake flash, not the cache) to check the writes landed. */
    blockdev_init(&fake_ops, FAKE_BASE, FAKE_SIZE, g_cache);
    uint8_t readback[8 * BLOCKDEV_BLOCK_SIZE];
    CHECK(blockdev_read(sector_lba, 8, readback), "read back the sector");
    for (uint32_t i = 0; i < 8; i++) {
        uint8_t want = (uint8_t)(0x10 + i);
        for (uint32_t b = 0; b < BLOCKDEV_BLOCK_SIZE; b++) {
            if (readback[i * BLOCKDEV_BLOCK_SIZE + b] != want) {
                CHECK(0, "block %u byte %u: got 0x%02x want 0x%02x", i, b,
                      readback[i * BLOCKDEV_BLOCK_SIZE + b], want);
                break;
            }
        }
    }
}

/* Writing back exactly what's already on flash shouldn't cost an erase. */
static void test_unchanged_sector_no_erase(void)
{
    fake_flash_reset();
    uint32_t sector_lba = 5 * (FLASH_SECTOR_SIZE / BLOCKDEV_BLOCK_SIZE);
    uint8_t sector_buf[FLASH_SECTOR_SIZE];

    /* The sector starts erased (all 0xFF): read it, then write that exact
     * content straight back. */
    CHECK(blockdev_read(sector_lba, FLASH_SECTOR_SIZE / BLOCKDEV_BLOCK_SIZE, sector_buf),
          "read the erased sector");
    for (size_t i = 0; i < sizeof sector_buf; i++) {
        CHECK(sector_buf[i] == 0xFF, "erased sector reads as 0xFF at byte %zu", i);
    }
    CHECK(blockdev_write(sector_lba, FLASH_SECTOR_SIZE / BLOCKDEV_BLOCK_SIZE, sector_buf),
          "write the sector's own content back");
    CHECK(blockdev_dirty(), "cache is marked dirty (a write happened)");
    CHECK(blockdev_flush(), "flush succeeds");
    CHECK(fake_erase_count == 0, "flushing an unchanged sector costs no erase, got %u",
          fake_erase_count);
}

/* A write to a different sector flushes the one that was cached first. */
static void test_cache_crosses_sector(void)
{
    fake_flash_reset();
    uint32_t blocks_per_sector = FLASH_SECTOR_SIZE / BLOCKDEV_BLOCK_SIZE;
    uint8_t a[BLOCKDEV_BLOCK_SIZE], b[BLOCKDEV_BLOCK_SIZE];
    memset(a, 0xAA, sizeof a);
    memset(b, 0xBB, sizeof b);

    CHECK(blockdev_write(0, 1, a), "write into sector 0");
    CHECK(fake_erase_count == 0, "nothing flushed yet");
    CHECK(blockdev_write(blocks_per_sector, 1, b), "write into sector 1, flushing sector 0 first");
    CHECK(fake_erase_count == 1, "sector 0 was flushed (1 erase), got %u", fake_erase_count);
    CHECK(blockdev_dirty(), "sector 1 is now the dirty cached one");

    CHECK(blockdev_flush(), "flush sector 1");
    CHECK(fake_erase_count == 2, "sector 1 flushed too, got %u", fake_erase_count);

    uint8_t readback[BLOCKDEV_BLOCK_SIZE];
    CHECK(blockdev_read(0, 1, readback) && memcmp(readback, a, sizeof a) == 0, "sector 0 kept its write");
    CHECK(blockdev_read(blocks_per_sector, 1, readback) && memcmp(readback, b, sizeof b) == 0,
          "sector 1 kept its write");
}

/* Random mixed-size reads and writes (1-16 blocks, crossing sector
 * boundaries) against a reference array that always holds the "logical"
 * content, including unflushed writes. Flushes periodically to exercise
 * read-after-flush too. */
static void test_random_vs_reference(void)
{
    fake_flash_reset();
    rng_seed(0xC0FFEEu);

    static uint8_t reference[FAKE_SIZE]; /* static: too big for the stack */
    memset(reference, 0xFF, sizeof reference);

    uint8_t scratch[16 * BLOCKDEV_BLOCK_SIZE];
    const int ops = 4000;

    for (int op = 0; op < ops; op++) {
        uint32_t count = rng_range(1, 16);
        uint32_t lba = rng_range(0, FAKE_BLOCKS - 1);
        if (lba + count > FAKE_BLOCKS) {
            count = FAKE_BLOCKS - lba; /* keep it in range; bounds has its own test */
        }
        size_t bytes = (size_t)count * BLOCKDEV_BLOCK_SIZE;

        if (rng_next() % 2 == 0) {
            for (size_t i = 0; i < bytes; i++) {
                scratch[i] = (uint8_t)rng_next();
            }
            CHECK(blockdev_write(lba, count, scratch), "op %d: write lba=%u count=%u", op, lba, count);
            memcpy(reference + (size_t)lba * BLOCKDEV_BLOCK_SIZE, scratch, bytes);
        } else {
            CHECK(blockdev_read(lba, count, scratch), "op %d: read lba=%u count=%u", op, lba, count);
            if (memcmp(scratch, reference + (size_t)lba * BLOCKDEV_BLOCK_SIZE, bytes) != 0) {
                CHECK(0, "op %d: read lba=%u count=%u mismatched the reference model", op, lba, count);
            }
        }

        if (op % 97 == 0) {
            CHECK(blockdev_flush(), "op %d: periodic flush", op);
            /* Read-after-flush: the same slice must still match. */
            CHECK(blockdev_read(lba, count, scratch), "op %d: read-after-flush lba=%u count=%u", op, lba,
                  count);
            if (memcmp(scratch, reference + (size_t)lba * BLOCKDEV_BLOCK_SIZE, bytes) != 0) {
                CHECK(0, "op %d: read-after-flush mismatched the reference model", op);
            }
        }
    }

    CHECK(blockdev_flush(), "final flush");
    uint32_t erases_during_run = blockdev_erase_count();

    /* Full read-back through a fresh blockdev (forces every sector to
     * reload from the fake flash) must match the reference exactly. */
    blockdev_init(&fake_ops, FAKE_BASE, FAKE_SIZE, g_cache);
    static uint8_t full[FAKE_SIZE];
    CHECK(blockdev_read(0, FAKE_BLOCKS, full), "full read-back after the random run");
    CHECK(memcmp(full, reference, sizeof full) == 0, "full read-back matches the reference model");

    CHECK(fake_write_violations == 0, "no NOR write violations during the random run");
    CHECK(erases_during_run > 0, "the random run should have caused some erases, got 0");
    fprintf(stderr, "random_vs_reference: %u erases over %u sectors, %d ops\n", (unsigned)erases_during_run,
            (unsigned)FAKE_SECTORS, ops);
}

static void test_flush_nothing_cached_is_noop(void)
{
    fake_flash_reset();
    CHECK(blockdev_flush(), "flushing with nothing cached succeeds");
    CHECK(fake_erase_count == 0, "flushing nothing erases nothing");
    CHECK(!blockdev_dirty(), "nothing cached, so nothing dirty");
}

/* A flush whose read-back verify doesn't match what was written must
 * report failure and leave the sector marked dirty, instead of quietly
 * accepting a write that didn't actually take. */
static void test_flush_verify_failure_stays_dirty(void)
{
    fake_flash_reset();
    uint8_t block[BLOCKDEV_BLOCK_SIZE];
    memset(block, 0x42, sizeof block);
    CHECK(blockdev_write(0, 1, block), "write a block");

    fake_corrupt_next_write = true;
    CHECK(!blockdev_flush(), "flush reports failure when the write silently didn't take");
    CHECK(blockdev_dirty(), "the cache stays marked dirty after a failed flush");
    CHECK(fake_erase_count == 1, "the erase still happened, got %u", fake_erase_count);

    /* A retry (no corruption this time) succeeds and clears dirty. */
    CHECK(blockdev_flush(), "a clean retry succeeds");
    CHECK(!blockdev_dirty(), "dirty is cleared once the retry's verify matches");
}

int main(void)
{
    test_bounds();
    test_one_sector_one_erase();
    test_unchanged_sector_no_erase();
    test_cache_crosses_sector();
    test_random_vs_reference();
    test_flush_nothing_cached_is_noop();
    test_flush_verify_failure_stays_dirty();

    if (g_failures == 0) {
        printf("ALL PASSED\n");
        return 0;
    }
    printf("%d FAILURES\n", g_failures);
    return 1;
}
