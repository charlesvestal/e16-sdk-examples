/* flash_ops_t over storage.h, for the block device (the FAT volume) and
 * the persistence reserve on the device (flash_e16.h). */
#include "flash_e16.h"

#include "disk_fatal.h"
#include "log.h"
#include "storage.h"

/*
 * The stock firmware's own data lives in the SPI flash's first 4 MB
 * (0x000000-0x3FFFFF): nothing in this firmware may touch it. Both users
 * of this file already stay above it (blockdev.c's and persist_slots.c's
 * own bounds checks), so this is the one place that floor is enforced
 * for every access, whatever the caller's arithmetic says.
 */
#define OXI_DATA_END 0x400000u

static void require_above_oxi_data(uint32_t addr)
{
    if (addr < OXI_DATA_END) {
        log_printf("flash: BUG: refused access at 0x%06lx (the stock firmware's data)", (unsigned long)addr);
        disk_fatal("flash: access below 0x400000");
    }
}

static void flash_e16_read(uint32_t addr, void *buf, size_t len)
{
    require_above_oxi_data(addr);
    storage_read(addr, buf, len);
}

static void flash_e16_write(uint32_t addr, const void *buf, size_t len)
{
    require_above_oxi_data(addr);
    storage_write(addr, buf, len);
}

static bool flash_e16_erase_sector(uint32_t addr)
{
    require_above_oxi_data(addr);
    storage_erase(addr, FLASH_SECTOR_SIZE);
    return true; /* no rate limiting here -- that's flash_budget.c, wrapped around this */
}

const flash_ops_t flash_e16_ops = {
    .read = flash_e16_read,
    .write = flash_e16_write,
    .erase_sector = flash_e16_erase_sector,
};
