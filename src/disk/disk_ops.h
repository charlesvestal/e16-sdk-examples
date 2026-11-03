#ifndef DISK_OPS_H
#define DISK_OPS_H
#include <stdbool.h>
#include <stdint.h>

/*
 * Bounded retries and budget bursts over the block device (blockdev.h)
 * and the erase-rate budget (flash_budget.h), shared by Disk mode
 * (src/disk_mode.c), app mode (src/apps) and the first boot's format.
 * Portable, and host-tested (test/disk/disk_ops_test.c); timed with the
 * clock flash_budget_init() was given.
 *
 * Only a refused erase (BLOCKDEV_ERR_ERASE: the budget said "not now") is
 * retried. A verify failure fails straight away: the flash didn't take
 * the write, and trying again in a loop won't change that.
 */

/* How long a retry keeps trying before giving up. */
#define DISK_RETRY_MS 3000u

/* After this long with no I/O from the computer, Disk mode writes back a
 * dirty block-device cache on its own (disk_idle_writeback()). */
#define DISK_IDLE_FLUSH_MS 300u

/* Erases the first boot's format may use (disk_format_burst()): a format
 * of non-blank flash takes 16 (the FAT copies, the root directory, the
 * boot sector), measured by disk_ops_test.c, which also checks there's
 * room to spare. */
#define DISK_FORMAT_MAX_ERASES 48u

/* blockdev_flush(), retried while it's refused an erase, for at most
 * DISK_RETRY_MS. */
bool disk_flush_retry(void);

/* One block read or written (blockdev_read()/_write()), retried the same
 * way. */
bool disk_rw_retry(bool write, uint32_t lba, void *buf);

/* disk_flush_retry() under the Disk-mode budget (no per-sector cap, the
 * larger burst; flash_budget.h), then back to whatever mode was set. For
 * a flush that has to happen now: before a reset (into or out of app
 * mode), and leaving Disk mode. */
bool disk_flush_burst(void);

/* volume_format() and the flush after it as one bounded operation under
 * the Disk-mode budget (flash_budget_op_begin(DISK_FORMAT_MAX_ERASES)),
 * so the first boot's format can't be cut short by a budget that's
 * already been spent. Leaves the budget in NORMAL mode. */
bool disk_format_burst(void);

/* Disk mode's write-back: if the cache is dirty and the last I/O ended
 * DISK_IDLE_FLUSH_MS or more before now, flushes it (one attempt: a
 * refused erase is tried again on the next call). True if it flushed
 * something. */
bool disk_idle_writeback(uint32_t now_ms, uint32_t last_io_ms);

#endif
