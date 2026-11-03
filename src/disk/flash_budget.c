/*
 * Erase-rate limiting over a flash_ops_t (flash_budget.h has the policy
 * numbers and the rationale). Pure integer arithmetic (no floats, to
 * match this codebase's -Wfloat-conversion/-Wdouble-promotion firmware
 * build): the token bucket is kept in milli-erases (1 erase = 1000
 * units) so a sub-1-erase-per-second refill rate (NORMAL mode) doesn't
 * need a fraction.
 */
#include "flash_budget.h"

#include <string.h>

#include "log.h"

#define TOKEN_SCALE 1000u /* 1 erase == this many bucket units */

/* NORMAL: ~1 erase/s sustained, burst 16; DISK: ~40/s sustained, burst
 * 256 (disk_mode.c's FAT-table traffic while the host copies files). */
#define NORMAL_RATE_PER_MS 1u    /* TOKEN_SCALE/1000 = 1 unit/ms == 1 erase/s */
#define NORMAL_BURST (FLASH_BUDGET_NORMAL_BURST * TOKEN_SCALE)
#define DISK_RATE_PER_MS 40u     /* 40*TOKEN_SCALE/1000 = 40 units/ms == 40 erases/s */
#define DISK_BURST (256u * TOKEN_SCALE)

/* Per-sector cap, NORMAL mode only: at most this many erases of the same
 * sector within a rolling window, tracked for up to this many distinct
 * sectors at once (older/less-recently-touched entries are recycled if
 * a new sector needs tracking and the table's full -- fails open, which
 * is safe: the global bucket above still caps the overall rate either
 * way). */
#define PER_SECTOR_MAX 4u
#define PER_SECTOR_WINDOW_MS 60000u
#define SECTOR_TRACK_N 8

typedef struct {
    uint32_t addr;
    uint32_t times[PER_SECTOR_MAX];
    uint8_t count;    /* how many of times[] are in use (ring, oldest first) */
    uint8_t next;     /* ring index the next timestamp overwrites */
    uint32_t touched; /* last time this entry was used, for recycling */
    bool valid;
} sector_track_t;

static const flash_ops_t *g_real;
static uint32_t (*g_now_ms)(void);
static flash_budget_mode_t g_mode;

/* The bucket level, in TOKEN_SCALE units. Signed: an operation started
 * with flash_budget_op_begin() can take it below zero (into debt), which
 * the refill pays back first. */
static int32_t g_tokens;
static uint32_t g_last_refill; /* g_now_ms() as of the last refill */
static bool g_initialized;

static sector_track_t g_sectors[SECTOR_TRACK_N];

/* flash_budget_op_begin(): erases still allowed in the operation in
 * progress (0: none in progress). */
static uint32_t g_op_left;
static bool g_op_active;

static uint32_t g_deferred_since_log; /* refused erases since the last summary line */
static uint32_t g_last_log_ms;
static bool g_have_last_log;

static void refill(uint32_t now)
{
    uint32_t elapsed = now - g_last_refill; /* unsigned: wraps correctly */
    int64_t rate = (g_mode == FLASH_BUDGET_DISK) ? DISK_RATE_PER_MS : NORMAL_RATE_PER_MS;
    int64_t burst = (g_mode == FLASH_BUDGET_DISK) ? DISK_BURST : NORMAL_BURST;
    /* 64-bit, so no gap is long enough to overflow; the bucket clamps to
     * burst anyway. */
    int64_t grown = (int64_t)g_tokens + (int64_t)elapsed * rate;
    if (grown > burst) {
        grown = burst;
    }
    /* A bucket over the current burst (just switched DISK -> NORMAL)
     * clamps down too. */
    g_tokens = (int32_t)grown;
    g_last_refill = now;
}

/* Finds addr's tracking entry, or claims one: an empty slot if the table
 * isn't full, else the least-recently-touched entry (recycled -- see
 * flash_budget.h on why that's safe). */
static sector_track_t *sector_find_or_claim(uint32_t addr, uint32_t now)
{
    sector_track_t *victim = NULL;
    uint32_t victim_age = 0;
    for (int i = 0; i < SECTOR_TRACK_N; i++) {
        if (g_sectors[i].valid && g_sectors[i].addr == addr) {
            return &g_sectors[i];
        }
        if (!g_sectors[i].valid) {
            victim = &g_sectors[i];
            victim_age = UINT32_MAX;
            continue;
        }
        if (victim == NULL || victim->valid) {
            uint32_t age = now - g_sectors[i].touched;
            if (victim == NULL || age > victim_age) {
                victim = &g_sectors[i];
                victim_age = age;
            }
        }
    }
    memset(victim, 0, sizeof *victim);
    victim->valid = true;
    victim->addr = addr;
    return victim;
}

/* How many times addr was erased within the window (NORMAL mode's
 * per-sector cap counts these; 0 if it isn't tracked). */
static uint8_t sector_in_window(const sector_track_t *t, uint32_t now)
{
    uint8_t in_window = 0;
    for (uint8_t j = 0; j < t->count; j++) {
        if (now - t->times[j] <= PER_SECTOR_WINDOW_MS) {
            in_window++;
        }
    }
    return in_window;
}

/* True if addr may erase again right now under the per-sector cap
 * (NORMAL mode only; always true in DISK mode, see flash_budget.h). Does
 * not record anything -- sector_record() does that once the erase is
 * actually allowed to happen (by both this and the global bucket). */
static bool sector_cap_ok(uint32_t addr, uint32_t now)
{
    if (g_mode == FLASH_BUDGET_DISK) {
        return true;
    }
    for (int i = 0; i < SECTOR_TRACK_N; i++) {
        if (g_sectors[i].valid && g_sectors[i].addr == addr) {
            return sector_in_window(&g_sectors[i], now) < PER_SECTOR_MAX;
        }
    }
    return true; /* never tracked (or evicted): fail open */
}

static void sector_record(uint32_t addr, uint32_t now)
{
    if (g_mode == FLASH_BUDGET_DISK) {
        return; /* no cap to track in Disk mode */
    }
    sector_track_t *sh = sector_find_or_claim(addr, now);
    sh->touched = now;
    sh->times[sh->next] = now;
    sh->next = (uint8_t)((sh->next + 1u) % PER_SECTOR_MAX);
    if (sh->count < PER_SECTOR_MAX) {
        sh->count++;
    }
}

static bool budget_erase_sector(uint32_t addr)
{
    uint32_t now = g_now_ms();
    refill(now);

    /* Inside an operation (flash_budget_op_begin()): up to its limit,
     * neither the per-sector cap nor an empty bucket stops it -- the
     * bucket goes into debt instead. */
    bool op = g_op_active && g_op_left > 0;
    if (!op) {
        if (!sector_cap_ok(addr, now)) {
            g_deferred_since_log++;
            return false;
        }
        if (g_tokens < (int32_t)TOKEN_SCALE) {
            g_deferred_since_log++;
            return false;
        }
    }

    if (!g_real->erase_sector(addr)) {
        return false; /* the real flash itself refused; nothing to charge */
    }
    g_tokens -= (int32_t)TOKEN_SCALE;
    if (op) {
        g_op_left--;
    }
    sector_record(addr, now);
    return true;
}

static void budget_read(uint32_t addr, void *buf, size_t len)
{
    g_real->read(addr, buf, len);
}

static void budget_write(uint32_t addr, const void *buf, size_t len)
{
    g_real->write(addr, buf, len);
}

const flash_ops_t flash_budget_ops = {
    .read = budget_read,
    .write = budget_write,
    .erase_sector = budget_erase_sector,
};

void flash_budget_init(const flash_ops_t *ops, uint32_t (*now_ms)(void))
{
    g_real = ops;
    g_now_ms = now_ms;
    g_mode = FLASH_BUDGET_NORMAL;
    g_tokens = (int32_t)NORMAL_BURST; /* start full: a cold boot shouldn't throttle right away */
    g_op_active = false;
    g_op_left = 0;
    g_last_refill = now_ms();
    memset(g_sectors, 0, sizeof g_sectors);
    g_deferred_since_log = 0;
    g_have_last_log = false;
    g_initialized = true;
}

void flash_budget_set_mode(flash_budget_mode_t mode)
{
    if (!g_initialized) {
        return;
    }
    if (g_mode == mode) {
        return;
    }
    uint32_t now = g_now_ms();
    refill(now); /* settle the old mode's rate/cap before switching */
    g_mode = mode;
    /* Switching to a much larger burst (NORMAL -> DISK) shouldn't hand
     * out tokens it hasn't earned yet, but shouldn't clamp away tokens
     * already saved up either: g_tokens is left as-is, refill() clamps
     * to whichever burst is current from here on. */
}

flash_budget_mode_t flash_budget_mode(void)
{
    return g_mode;
}

uint32_t flash_budget_now_ms(void)
{
    return g_now_ms ? g_now_ms() : 0;
}

bool flash_budget_available(uint32_t erases)
{
    if (!g_initialized) {
        return false;
    }
    uint32_t now = g_now_ms();
    refill(now);
    if ((int64_t)g_tokens < (int64_t)erases * TOKEN_SCALE) {
        return false;
    }
    if (g_mode == FLASH_BUDGET_NORMAL) {
        for (int i = 0; i < SECTOR_TRACK_N; i++) {
            if (g_sectors[i].valid && sector_in_window(&g_sectors[i], now) >= PER_SECTOR_MAX) {
                return false;
            }
        }
    }
    return true;
}

void flash_budget_op_begin(uint32_t max_erases)
{
    g_op_active = true;
    g_op_left = max_erases;
}

void flash_budget_op_end(void)
{
    g_op_active = false;
    g_op_left = 0;
}

void flash_budget_poll(uint32_t now_ms_value)
{
    if (g_deferred_since_log == 0) {
        return;
    }
    if (g_have_last_log && now_ms_value - g_last_log_ms < 60000u) {
        return;
    }
    log_printf("flash: %lu erases deferred in the last minute", (unsigned long)g_deferred_since_log);
    g_deferred_since_log = 0;
    g_last_log_ms = now_ms_value;
    g_have_last_log = true;
}
