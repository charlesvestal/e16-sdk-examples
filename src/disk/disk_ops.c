/*
 * Bounded retries and budget bursts over the block device: disk_ops.h.
 */
#include "disk_ops.h"

#include "blockdev.h"
#include "flash_budget.h"
#include "volume.h"

static bool retryable(void)
{
    return blockdev_last_error() == BLOCKDEV_ERR_ERASE;
}

bool disk_flush_retry(void)
{
    uint32_t t0 = flash_budget_now_ms();
    for (;;) {
        if (blockdev_flush()) {
            return true;
        }
        if (!retryable() || flash_budget_now_ms() - t0 >= DISK_RETRY_MS) {
            return false;
        }
    }
}

bool disk_rw_retry(bool write, uint32_t lba, void *buf)
{
    uint32_t t0 = flash_budget_now_ms();
    for (;;) {
        if (write ? blockdev_write(lba, 1, buf) : blockdev_read(lba, 1, buf)) {
            return true;
        }
        if (!retryable() || flash_budget_now_ms() - t0 >= DISK_RETRY_MS) {
            return false;
        }
    }
}

bool disk_flush_burst(void)
{
    flash_budget_mode_t was = flash_budget_mode();
    flash_budget_set_mode(FLASH_BUDGET_DISK);
    bool ok = disk_flush_retry();
    flash_budget_set_mode(was);
    return ok;
}

bool disk_format_burst(void)
{
    flash_budget_set_mode(FLASH_BUDGET_DISK);
    flash_budget_op_begin(DISK_FORMAT_MAX_ERASES);
    bool ok = volume_format() && blockdev_flush();
    flash_budget_op_end();
    flash_budget_set_mode(FLASH_BUDGET_NORMAL);
    return ok;
}

bool disk_idle_writeback(uint32_t now_ms, uint32_t last_io_ms)
{
    if (!blockdev_dirty() || now_ms - last_io_ms < DISK_IDLE_FLUSH_MS) {
        return false;
    }
    return blockdev_flush();
}
