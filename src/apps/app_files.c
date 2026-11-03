/*
 * The app api's file writes over the FAT volume: app_files.h.
 */
#include "app_files.h"

#include "blockdev.h"
#include "disk_ops.h"
#include "e16_api.h"
#include "flash_budget.h"
#include "log.h"
#include "volume.h"

#define SECTOR 4096u

int app_files_write(const char *dir, const char *path, const void *buf, uint32_t len)
{
    if (len > E16_FILE_WRITE_MAX) {
        return E16_EARGS;
    }
    uint32_t need = APP_FILE_WRITE_ERASES + (len + SECTOR - 1u) / SECTOR;
    if (need > FLASH_BUDGET_NORMAL_BURST) {
        need = FLASH_BUDGET_NORMAL_BURST;
    }
    if (!flash_budget_available(need)) {
        return E16_EBUSY;
    }
    flash_budget_op_begin(APP_FILE_WRITE_OP_MAX);
    uint32_t e0 = blockdev_erase_count();
    bool ok = volume_mkdir(dir) && volume_rewrite_file(path, buf, len) && blockdev_flush();
    uint32_t used = blockdev_erase_count() - e0;
    flash_budget_op_end();
    if (!ok) {
        log_printf("app: writing %s failed (%lu erases)", path, (unsigned long)used);
        return E16_EIO;
    }
    return 0;
}

bool app_files_flush(void)
{
    return disk_flush_burst();
}
