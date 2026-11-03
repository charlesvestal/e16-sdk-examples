#ifndef FLASH_BUDGET_H
#define FLASH_BUDGET_H
#include <stdbool.h>
#include <stdint.h>

#include "flash_ops.h"

/*
 * A flash_ops_t wrapper that rate-limits sector erases, so that no bug or
 * runaway script can wear out the SPI NOR by erasing it continuously.
 * OXI's hard rule is about the *internal* MCU flash (tools/check_no_flash_
 * writes.sh enforces that no code that writes it is linked in at all);
 * this is the softer promise about the external SPI flash both the
 * persistence reserve (persist_slots.c) and the FAT volume (blockdev.c)
 * live on: writes are fine, but "rewriting flash a ton, super fast" isn't.
 *
 * A token bucket gates erase_sector() only -- read() and write() always
 * pass straight through, unrated (only erasing wears NOR; the write
 * budget is what writing without erasing first already constrains, one
 * bit at a time, which is self-limiting). Two modes (flash_budget_set_
 * mode()):
 *
 *   FLASH_BUDGET_NORMAL   ~1 erase/s sustained, burst up to 16; on top of
 *                         that, any single sector is capped at 4 erases
 *                         per rolling minute (a small table remembers the
 *                         last few distinct sectors erased and when).
 *                         This is the mode persistence and Disk mode's
 *                         block device both run in day to day.
 *   FLASH_BUDGET_DISK     ~40 erases/s sustained, burst up to 256, and no
 *                         per-sector cap -- entered while Disk mode is
 *                         active (disk_mode_enter()/_leave()): the host
 *                         computer is deliberately rewriting FAT tables
 *                         and directory sectors often while copying
 *                         files, which is legitimate, fast, bursty
 *                         traffic the normal budget would wrongly choke.
 *
 * now_ms is injected rather than calling system_millis() directly, so
 * this is host-testable with a fake clock (test/disk/flash_budget_test.c)
 * -- the same reason blockdev.c and persist_slots.c take a flash_ops_t
 * instead of calling storage.h.
 */
/* NORMAL mode's burst, in erases: the most flash_budget_available() can
 * ever say yes to in NORMAL mode. */
#define FLASH_BUDGET_NORMAL_BURST 16u

typedef enum {
    FLASH_BUDGET_NORMAL,
    FLASH_BUDGET_DISK,
} flash_budget_mode_t;

/* Resets all budget/deferral state. ops is the real flash underneath
 * (flash_e16_ops on the device, a fake in tests); now_ms() returns the
 * current time in milliseconds on whatever clock the caller likes (must
 * be monotonic; wraparound at UINT32_MAX is handled the same way
 * elsewhere in this codebase, via unsigned subtraction). Starts in
 * FLASH_BUDGET_NORMAL. */
void flash_budget_init(const flash_ops_t *ops, uint32_t (*now_ms)(void));

void flash_budget_set_mode(flash_budget_mode_t mode);
flash_budget_mode_t flash_budget_mode(void);

/* The clock flash_budget_init() was given (0 before that): the bounded
 * retries in disk_ops.c time themselves with it. */
uint32_t flash_budget_now_ms(void);

/* The wrapped ops: read/write pass through to the real ops unchanged;
 * erase_sector() is rate-limited (see above) and returns false, without
 * erasing, when over budget. This is what persist_slots_init() and
 * blockdev_init() are handed on the device instead of flash_e16_ops
 * directly. */
extern const flash_ops_t flash_budget_ops;

/*
 * For one operation that needs several erases and mustn't be cut short
 * partway -- an app's file write (src/apps/app_files.c): by the time
 * FatFs needs its last erases it has already truncated the old file, so
 * a refusal there loses the old copy and the new one both.
 *
 * flash_budget_available(n): true if n erases could be paid from the
 * bucket right now and, in NORMAL mode, no recently erased sector is at
 * its per-sector cap. Changes nothing.
 *
 * flash_budget_op_begin(max): from here to flash_budget_op_end(), up to
 * max erases go through without the per-sector cap and even with the
 * bucket empty: they're charged all the same, and the bucket goes into
 * debt, which the refill pays back before flash_budget_available() says
 * yes again. Erases past max are budgeted as usual (and so may be
 * refused). Ask flash_budget_available() first: the operation is only
 * as rate-limited as the caller's check makes it. The sustained rate is
 * still the bucket's (~1 erase/s in NORMAL mode), but within one
 * operation a sector can be erased more than the per-sector cap allows
 * (FatFs rewrites the FAT and the directory sector a few times per file
 * write), so the cap bounds how often such operations start, not each
 * erase inside them.
 */
bool flash_budget_available(uint32_t erases);
void flash_budget_op_begin(uint32_t max_erases);
void flash_budget_op_end(void);

/* Call periodically (main.c's loop) so a run of deferrals gets logged as
 * one summary line ("flash: N erases deferred in the last minute")
 * rather than one line per refused erase -- throttled to at most once a
 * minute, and only when at least one erase was actually deferred since
 * the last time this logged. */
void flash_budget_poll(uint32_t now_ms_value);

#endif
