/*
 * Host test for src/disk/flash_budget.c: a fake underlying flash_ops_t
 * (erase always "succeeds" and just counts) plus an injected fake clock,
 * so the token bucket's timing can be driven exactly rather than through
 * real sleeps. Plain C, run directly (not through e16rt-host). Prints
 * "ALL PASSED" and exits 0 on success; exits nonzero otherwise.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "flash_budget.h"

static int g_failures;
#define CHECK(cond, ...)                                                                                    \
    do {                                                                                                     \
        if (!(cond)) {                                                                                       \
            g_failures++;                                                                                    \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                              \
            fprintf(stderr, __VA_ARGS__);                                                                     \
            fprintf(stderr, "\n");                                                                            \
        }                                                                                                     \
    } while (0)

/* ---- fake underlying flash and fake clock ---- */

static uint32_t fake_erase_count;
static bool fake_erase_fails; /* simulates the real flash itself refusing */

static void fake_read(uint32_t addr, void *buf, size_t len)
{
    (void)addr;
    memset(buf, 0, len);
}

static void fake_write(uint32_t addr, const void *buf, size_t len)
{
    (void)addr;
    (void)buf;
    (void)len;
}

static bool fake_erase_sector(uint32_t addr)
{
    (void)addr;
    if (fake_erase_fails) {
        return false;
    }
    fake_erase_count++;
    return true;
}

static const flash_ops_t fake_ops = {
    .read = fake_read,
    .write = fake_write,
    .erase_sector = fake_erase_sector,
};

static uint32_t g_now;

static uint32_t fake_now_ms(void)
{
    return g_now;
}

static void reset(void)
{
    fake_erase_count = 0;
    fake_erase_fails = false;
    g_now = 0;
    flash_budget_init(&fake_ops, fake_now_ms);
}

/* Erases addr through the budget, advancing the fake clock by dt_ms
 * first (0 for "right now"). */
static bool try_erase_after(uint32_t dt_ms, uint32_t addr)
{
    g_now += dt_ms;
    return flash_budget_ops.erase_sector(addr);
}

/* ---- tests ---- */

/* NORMAL mode starts with a full burst (16), so the first 16 erases in a
 * row (no time passing) all succeed; the 17th is refused. */
static void test_burst(void)
{
    reset();
    for (int i = 0; i < 16; i++) {
        CHECK(try_erase_after(0, 0x1000 + (uint32_t)i * 0x1000), "burst erase %d/16 succeeds", i + 1);
    }
    CHECK(!try_erase_after(0, 0x20000), "the 17th erase in the same instant is refused");
    CHECK(fake_erase_count == 16, "exactly 16 erases actually happened, got %u", fake_erase_count);
}

/* After the burst is spent, NORMAL mode refills at ~1 erase/s: waiting
 * 1000 ms earns back exactly one token. */
static void test_sustained_rate(void)
{
    reset();
    for (int i = 0; i < 16; i++) {
        CHECK(try_erase_after(0, 0x1000 + (uint32_t)i * 0x1000), "spend the burst, %d/16", i + 1);
    }
    CHECK(!try_erase_after(0, 0x30000), "burst spent: immediately refused");
    CHECK(!try_erase_after(500, 0x30000), "half a second isn't enough for another token");
    CHECK(try_erase_after(500, 0x30000), "a full second (500+500 ms) earns one token back");
    CHECK(!try_erase_after(0, 0x40000), "that token is spent again: refused right away");

    /* 10 seconds at the sustained rate should allow about 10 more
     * erases, each needing a different sector so the per-sector cap
     * (tested separately below) doesn't also refuse them. */
    int allowed = 0;
    for (int i = 0; i < 10; i++) {
        if (try_erase_after(1000, 0x100000 + (uint32_t)i * 0x1000)) {
            allowed++;
        }
    }
    CHECK(allowed >= 9 && allowed <= 10, "~1 erase/s sustained over 10 s, got %d/10", allowed);
}

/* A single sector can erase at most 4 times within a rolling minute in
 * NORMAL mode, even with global budget to spare. */
static void test_per_sector_cap(void)
{
    reset();
    uint32_t addr = 0x50000;
    /* Space these out so the global rate never refuses first (1/s
     * sustained, well under one erase every 2 s). */
    CHECK(try_erase_after(0, addr), "sector erase 1/4");
    CHECK(try_erase_after(2000, addr), "sector erase 2/4");
    CHECK(try_erase_after(2000, addr), "sector erase 3/4");
    CHECK(try_erase_after(2000, addr), "sector erase 4/4");
    CHECK(!try_erase_after(2000, addr), "the 5th erase of the same sector within a minute is refused");

    /* A *different* sector isn't affected by the first one's cap. */
    CHECK(try_erase_after(0, addr + 0x1000), "a different sector isn't capped by the first one's history");

    /* Once the oldest of the 4 falls out of the 60 s window, the sector
     * can erase again. The window is inclusive (<=60000 ms still
     * counts), so this needs to land strictly past 60000 ms after the
     * first of the 4 (at t=0) to free it up. */
    CHECK(!try_erase_after(45000, addr), "still within the minute of the first erase: still refused");
    /* Now at t=53000; the first erase (t=0) needs t>60000 to drop out. */
    CHECK(try_erase_after(10000, addr), "60+ s after the first of the 4: refused slot freed up");
}

/* Disk mode: a much higher sustained rate/burst, and no per-sector cap
 * at all (disk_mode.c's FAT-table traffic while the host copies files). */
static void test_disk_mode(void)
{
    reset();
    flash_budget_set_mode(FLASH_BUDGET_DISK);
    /* Let the bucket refill to Disk mode's full burst before testing it
     * (tokens carry over across a mode switch, at whatever level NORMAL
     * mode left them; this just gives the global budget enough headroom
     * that the per-sector check below isn't confused with it running
     * dry). At 40 units/ms, 10 s is far more than enough to fill 256. */
    try_erase_after(10000, 0x5F000);

    /* The same sector, erased far more than 4 times with no delay
     * between some of them, must not be refused by a per-sector cap
     * that doesn't apply in this mode (though the global budget still
     * can refuse once its own burst is spent). */
    uint32_t addr = 0x60000;
    int allowed = 0;
    for (int i = 0; i < 20; i++) {
        if (try_erase_after(0, addr)) {
            allowed++;
        }
    }
    CHECK(allowed == 20, "20 instant erases of the same sector all succeed in Disk mode, got %d/20", allowed);

    /* The global burst (256) does still cap something unreasonable: ask
     * for far more than that with no time passing. */
    int total_allowed = allowed;
    for (int i = 0; i < 300; i++) {
        if (try_erase_after(0, addr + (uint32_t)i * 0x1000)) {
            total_allowed++;
        }
    }
    CHECK(total_allowed <= 256, "Disk mode's burst (256) still caps something absurd, got %d", total_allowed);
}

/* Switching back to NORMAL mode after Disk mode re-applies the smaller
 * burst and the per-sector cap. */
static void test_mode_switch_back(void)
{
    reset();
    flash_budget_set_mode(FLASH_BUDGET_DISK);
    for (int i = 0; i < 20; i++) {
        try_erase_after(0, 0x70000 + (uint32_t)i * 0x1000);
    }
    flash_budget_set_mode(FLASH_BUDGET_NORMAL);

    /* However many tokens carried over, NORMAL's 16-erase burst ceiling
     * applies again (not DISK's 256), and the per-sector cap is back. */
    uint32_t addr = 0x80000;
    int allowed = 0;
    for (int i = 0; i < 20; i++) {
        if (try_erase_after(0, addr)) {
            allowed++;
        }
    }
    CHECK(allowed <= 4, "back in NORMAL, the per-sector cap (4) applies again to one sector, got %d",
          allowed);
}

/* The real flash itself refusing an erase (e.g. a hardware fault) must
 * not be charged against the budget -- there's nothing to charge, and a
 * caller retrying right away shouldn't be penalized by this layer too. */
static void test_real_flash_refusal_not_charged(void)
{
    reset();
    fake_erase_fails = true;
    CHECK(!try_erase_after(0, 0x90000), "the underlying flash refuses");
    fake_erase_fails = false;
    CHECK(try_erase_after(0, 0x90000), "a token is still available right after (nothing was charged)");
}

/* flash_budget_available(): the bucket and, in NORMAL mode, the
 * per-sector cap -- without changing anything. */
static void test_available(void)
{
    reset();
    CHECK(flash_budget_available(16), "a full bucket covers 16");
    CHECK(!flash_budget_available(17), "but not 17");
    for (uint32_t i = 0; i < 10; i++) {
        try_erase_after(0, 0x1000u * (i + 1u));
    }
    CHECK(flash_budget_available(6) && !flash_budget_available(7), "6 left after 10 erases");
    CHECK(fake_erase_count == 10, "asking erased nothing");
    g_now += 1000;
    CHECK(flash_budget_available(7), "a second later, 7");

    reset();
    for (int i = 0; i < 4; i++) {
        try_erase_after(0, 0x5000);
    }
    CHECK(!flash_budget_available(1), "a sector at its cap: no");
    g_now += 61000;
    CHECK(flash_budget_available(1), "until the window has passed");
    flash_budget_set_mode(FLASH_BUDGET_DISK);
    for (int i = 0; i < 4; i++) {
        try_erase_after(0, 0x6000);
    }
    CHECK(flash_budget_available(1), "DISK mode has no per-sector cap to be at");
}

/* flash_budget_op_begin(): up to its limit, no per-sector cap and no
 * empty bucket; charged all the same, so the debt is paid back before
 * the next yes. */
static void test_op(void)
{
    reset();
    flash_budget_op_begin(30);
    int ok = 0;
    for (int i = 0; i < 30; i++) {
        ok += try_erase_after(0, 0x7000) ? 1 : 0; /* one sector, 30 times */
    }
    CHECK(ok == 30, "30 erases of one sector inside an op of 30 (got %d)", ok);
    CHECK(!try_erase_after(0, 0x8000), "the 31st is budgeted as usual: refused (bucket in debt)");
    flash_budget_op_end();
    CHECK(!try_erase_after(0, 0x9000), "after the op: still refused");
    CHECK(!flash_budget_available(1), "and nothing available");
    g_now += 14000;
    CHECK(!flash_budget_available(1), "14 s later the debt of 14 isn't paid off yet");
    g_now += 61000;
    CHECK(flash_budget_available(16), "75 s later the bucket is full again (debt paid, sector window over)");
    CHECK(try_erase_after(0, 0xA000), "and erases go through");
}

int main(void)
{
    test_burst();
    test_sustained_rate();
    test_per_sector_cap();
    test_disk_mode();
    test_mode_switch_back();
    test_real_flash_refusal_not_charged();
    test_available();
    test_op();

    if (g_failures == 0) {
        printf("ALL PASSED\n");
        return 0;
    }
    printf("%d FAILURES\n", g_failures);
    return 1;
}
