/*
 * FatFs's disk I/O layer (external/fatfs/source/diskio.h) over the block
 * device (blockdev.h): one drive, 512-byte sectors. Portable: the host
 * tests run it over a fake flash, the device over the SPI NOR.
 *
 * CTRL_SYNC (FatFs calls it from f_sync/f_close/f_mkfs) flushes the block
 * device's 4 KB write-back cache, so a closed file is on flash. The erase
 * block reported to f_mkfs is one 4 KB flash sector, so it aligns the data
 * area to it.
 */
#include "ff.h"
#include "diskio.h"

#include "blockdev.h"
#include "flash_ops.h"

DSTATUS disk_status(BYTE pdrv)
{
    return pdrv == 0 ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    return disk_status(pdrv);
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0) {
        return RES_PARERR;
    }
    return blockdev_read((uint32_t)sector, count, buff) ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0) {
        return RES_PARERR;
    }
    return blockdev_write((uint32_t)sector, count, buff) ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv != 0) {
        return RES_PARERR;
    }
    switch (cmd) {
        case CTRL_SYNC:
            return blockdev_flush() ? RES_OK : RES_ERROR;
        case GET_SECTOR_COUNT:
            *(LBA_t *)buff = blockdev_blocks();
            return RES_OK;
        case GET_SECTOR_SIZE:
            *(WORD *)buff = BLOCKDEV_BLOCK_SIZE;
            return RES_OK;
        case GET_BLOCK_SIZE:
            *(DWORD *)buff = FLASH_SECTOR_SIZE / BLOCKDEV_BLOCK_SIZE;
            return RES_OK;
        default:
            return RES_PARERR;
    }
}
