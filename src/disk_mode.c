/*
 * The FAT volume on the device, and Disk mode: disk_mode.h.
 */
#include "disk_mode.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "storage.h"
#include "system.h"
#include "tusb.h"
#include "usb_msc.h"

#include "blockdev.h"
#include "disk_ops.h"
#include "flash_budget.h"
#include "log.h"
#include "persist_slots.h"
#include "scenes.h"
#include "scripts.h"
#include "volume.h"
#include "mirror.h" /* E16_MIRROR */
#if E16_MIRROR
#include "mirror_page.h"
#endif

_Static_assert(VOLUME_FLASH_END == PERSIST_RESERVE_BASE, "the volume ends where the persistence reserve begins");
_Static_assert(VOLUME_FLASH_SIZE % FLASH_SECTOR_SIZE == 0, "the volume is whole flash sectors");

#define BLOCK_SIZE BLOCKDEV_BLOCK_SIZE

/* The block device's one-sector write-back cache. In SRAM, not CCM: app
 * mode reads and writes the volume too, and gives the app all of the CCM
 * (mem_layout.h). Shared with app mode (disk_mode.h). */
uint8_t disk_block_cache[FLASH_SECTOR_SIZE] __attribute__((aligned(4)));
static bool g_available;

/* ---- USB mass storage (USB service interrupt) ---- */

/* True while the computer may use the medium: only in Disk mode. */
static volatile bool g_present;
static volatile bool g_ejected;

/*
 * The one request handed from the interrupt to the main loop. TinyUSB
 * makes no new read/write call until usb_msc_io_done() answers this
 * one, so there's never more than one. g_pending is set last in the
 * interrupt and cleared by the main loop before it calls io_done (which
 * can run the next callback right away). All of it volatile, so neither
 * side's compiler can move a request field's store or load across
 * g_pending's (single-core Cortex-M4: no hardware reordering to fence).
 */
static volatile bool g_pending;
static volatile bool g_req_write;
static volatile uint32_t g_req_lba, g_req_offset, g_req_len;
static void *volatile g_req_buf;

static bool msc_ready(void)
{
    return g_present;
}

static int32_t msc_request(bool write, uint32_t lba, uint32_t offset, void *buf, uint32_t len)
{
    if (!g_present || g_pending || offset >= BLOCK_SIZE) {
        return USB_MSC_ERROR;
    }
    if (offset + len > BLOCK_SIZE) {
        len = BLOCK_SIZE - offset;
    }
    g_req_write = write;
    g_req_lba = lba;
    g_req_offset = offset;
    g_req_len = len;
    g_req_buf = buf;
    g_pending = true;
    return USB_MSC_ASYNC;
}

static int32_t msc_read(uint32_t lba, uint32_t offset, void *buf, uint32_t len)
{
    return msc_request(false, lba, offset, buf, len);
}

static int32_t msc_write(uint32_t lba, uint32_t offset, const void *buf, uint32_t len)
{
    /* The buffer is TinyUSB's; it's only read from for a write. */
    return msc_request(true, lba, offset, (void *)(uintptr_t)buf, len);
}

/* START STOP UNIT with eject: hide the medium right away (or macOS mounts
 * it again); the main loop then leaves Disk mode. */
static void msc_eject(void)
{
    g_present = false;
    g_ejected = true;
}

static const usb_msc_ops_t msc_ops = {
    .block_count = VOLUME_FLASH_SIZE / BLOCK_SIZE,
    .block_size = BLOCK_SIZE,
    .ready = msc_ready,
    .read = msc_read,
    .write = msc_write,
    .eject = msc_eject,
};

bool disk_mode_usb_start(void)
{
    return usb_msc_start(&msc_ops);
}

/* ---- main loop ---- */

/*
 * Noticing that the computer let go of the volume. With the lock refused
 * (the SDK's usb_msc.c refuses PREVENT MEDIUM REMOVAL), macOS treats the
 * drive as manual-eject: ejecting it unmounts and stops there, with no
 * START STOP UNIT, so msc_eject() never runs. What it does do is mark the
 * volume in use on mount and not in use on unmount, in FAT16's
 * clean-shutdown bit (bit 15 of FAT entry 1; set = clean): every write of
 * the first FAT sector while mounted has it clear, the last one on
 * unmount has it set. So a write of that sector with the bit set, after
 * one with it clear in this visit, means the computer is unmounting the
 * volume; once it has gone quiet (it still writes the second FAT copy
 * after the first, and the eject fails if the medium vanishes under it),
 * Disk mode leaves as if ejected. Requiring the clear write first keeps a
 * disk check before mounting (which may mark a volume left dirty as
 * clean) from counting. A host that never clears the bit just doesn't
 * trigger this; encoder 1 still leaves.
 */
#define FAT16_CLEAN_BIT 0x8000u
#define UNMOUNT_QUIET_MS 1000u
static uint32_t g_fat_lba = UINT32_MAX;
static bool g_host_mounted;   /* saw the clean bit cleared this visit */
static bool g_host_unmounted; /* then saw it set again */
static uint32_t g_last_io_ms; /* when the last read or write finished */

static void watch_fat_write(uint32_t lba, const uint8_t *block)
{
    /* Entry 0 is the media byte and 0xFF; entry 1 holds the bit. */
    if (lba != g_fat_lba || block[0] != 0xF8 || block[1] != 0xFF) {
        return;
    }
    bool clean = (block[3] << 8 | block[2]) & FAT16_CLEAN_BIT;
    if (!clean) {
        g_host_mounted = true;
    } else if (g_host_mounted) {
        g_host_unmounted = true;
    }
}

/* Per-visit figures, logged on leaving. */
static uint32_t g_bytes_read, g_bytes_written;
static uint32_t g_slowest_ms; /* longest single request: the main loop's worst stall */
static uint32_t g_erases_at_enter;

/* Block reads and writes retry an erase the flash budget refused, for a
 * bounded time (disk_ops.h): Disk mode runs the much more generous DISK
 * budget, so that's rare, but it turns a transient refusal under abusive
 * host traffic into a short delay instead of a failed host write. A
 * verify failure fails the request straight away. */
/* Does the request the interrupt handed over, if any. */
static void service(void)
{
    if (!g_pending) {
        return;
    }
    bool write = g_req_write;
    uint32_t lba = g_req_lba, off = g_req_offset, len = g_req_len;
    uint8_t *buf = g_req_buf;
    uint32_t t0 = system_millis();

    bool ok;
    if (off == 0 && len == BLOCK_SIZE) {
        ok = disk_rw_retry(write, lba, buf);
        if (ok && write) {
            watch_fat_write(lba, buf);
        }
    } else {
        /* Part of a block (never seen with 512-byte blocks, but allowed). */
        uint8_t tmp[BLOCK_SIZE];
        ok = disk_rw_retry(false, lba, tmp);
        if (ok && write) {
            memcpy(tmp + off, buf, len);
            ok = disk_rw_retry(true, lba, tmp);
        } else if (ok) {
            memcpy(buf, tmp + off, len);
        }
    }

    uint32_t dt = system_millis() - t0;
    if (dt > g_slowest_ms) {
        g_slowest_ms = dt;
    }
    if (ok) {
        if (write) {
            g_bytes_written += len;
        } else {
            g_bytes_read += len;
        }
    } else {
        log_printf("disk: %s of block %lu failed", write ? "write" : "read", (unsigned long)lba);
    }
    g_last_io_ms = system_millis();
    g_pending = false;
    usb_msc_io_done(ok ? (int32_t)len : USB_MSC_ERROR);
}

/*
 * The built-in scripts /scripts doesn't have yet, each with a --@scene line
 * on top: all of them on the first boot, and any added to the firmware (or
 * deleted from the disk) since. Writing a few files in a row rewrites the
 * same directory and FAT sectors several times, more than the normal
 * erase budget's per-sector cap allows within a minute (flash_budget.h),
 * so this bounded burst runs under the Disk-mode budget.
 */
#if E16_MIRROR
/* The mirror's web page (tools/mirror/index.html, embedded by the build:
 * mirror_page.h) at the root of the disk, so a computer has it at hand:
 * written when it's missing or differs from this firmware's copy. */
static void write_mirror_page(void)
{
    if (volume_same_file(MIRROR_PAGE_PATH, mirror_page, mirror_page_len)) {
        return;
    }
    if (volume_rewrite_file(MIRROR_PAGE_PATH, mirror_page, mirror_page_len)) {
        log_printf("disk: wrote %s (%u bytes)", MIRROR_PAGE_PATH, (unsigned)mirror_page_len);
    }
}
#endif

static int write_builtins(void)
{
    flash_budget_set_mode(FLASH_BUDGET_DISK);
    int n = scenes_write_missing_builtins();
#if E16_MIRROR
    write_mirror_page();
#endif
    if (!disk_flush_retry()) {
        log_printf("disk: flush failed after writing built-ins");
    }
    flash_budget_set_mode(FLASH_BUDGET_NORMAL);
    return n;
}

void disk_init(void)
{
    if (storage_size() < VOLUME_FLASH_END) {
        log_printf("disk: SPI flash missing or too small; no disk");
        return;
    }
    blockdev_init(&flash_budget_ops, VOLUME_FLASH_BASE, VOLUME_FLASH_SIZE, disk_block_cache);
    if (volume_mount()) {
        g_available = true;
        log_printf("disk: mounted, %d .lua files in %s", volume_count_scripts(), VOLUME_SCRIPTS_DIR);
        write_builtins();
        return;
    }
    /* The first boot's format: as one bounded burst under the Disk-mode
     * budget (disk_format_burst()), since a format of non-blank flash
     * needs the whole of the normal budget's burst. */
    uint32_t t0 = system_millis();
    if (!disk_format_burst()) {
        log_printf("disk: no volume, and formatting failed");
        return;
    }
    g_available = true;
    int n = write_builtins();
    log_printf("disk: formatted, wrote %d scripts (%lu ms)", n, (unsigned long)(system_millis() - t0));
}

bool disk_available(void)
{
    return g_available;
}

void disk_mode_enter(void)
{
    /* The host is about to rewrite FAT tables and directory sectors
     * often, on purpose, while copying files -- a much higher erase
     * budget with no per-sector cap (flash_budget.h), for as long as
     * Disk mode is active. */
    flash_budget_set_mode(FLASH_BUDGET_DISK);
    g_fat_lba = volume_fat_lba();
    g_host_mounted = false;
    g_host_unmounted = false;
    volume_unmount();
    if (!blockdev_flush()) {
        log_printf("disk: flush failed before Disk mode");
    }
    g_bytes_read = g_bytes_written = 0;
    g_slowest_ms = 0;
    g_erases_at_enter = blockdev_erase_count();
    g_ejected = false;
    g_present = true; /* last: the computer may start right away */
    log_printf("disk: mode on, %lu blocks", (unsigned long)msc_ops.block_count);
}

/*
 * Write-back while the computer is idle: the block device holds the last
 * written sector in its cache until something else needs the cache, so
 * a copy that ends without an eject (the cable pulled, the E16 reset)
 * would lose that sector. Once the computer has done nothing for
 * DISK_IDLE_FLUSH_MS, the cache goes onto flash (under the Disk-mode
 * budget, already set).
 */
bool disk_mode_poll(void)
{
    service();
    if (!g_pending && g_present) {
        disk_idle_writeback(system_millis(), g_last_io_ms);
    }
    if (g_host_unmounted && !g_pending && system_millis() - g_last_io_ms >= UNMOUNT_QUIET_MS) {
        g_present = false;
        g_ejected = true;
    }
    return g_ejected;
}

void disk_mode_leave(const char *why)
{
    /* Once this store is done no new request can start (the interrupt
     * checks it first); one that started before it is finished here. */
    g_present = false;
    service();
    bool flushed = disk_flush_burst();
    log_printf("disk: mode off (%s): read %lu KB, wrote %lu KB, %lu erases, slowest request %lu ms%s", why,
               (unsigned long)(g_bytes_read / 1024), (unsigned long)(g_bytes_written / 1024),
               (unsigned long)(blockdev_erase_count() - g_erases_at_enter), (unsigned long)g_slowest_ms,
               flushed ? "" : ", FLUSH FAILED");
    /* Back to the normal, lower budget now that the host isn't actively
     * copying files. */
    flash_budget_set_mode(FLASH_BUDGET_NORMAL);
    if (!volume_mount()) {
        log_printf("disk: no valid volume after Disk mode");
    }
    /* The menu from what /scripts holds now (only the built-ins if the
     * volume didn't mount: nothing on it can be loaded). */
    scenes_rescan();
}

