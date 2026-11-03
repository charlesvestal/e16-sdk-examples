#ifndef DISK_MODE_H
#define DISK_MODE_H
#include <stdbool.h>
#include <stdint.h>

/*
 * The FAT volume on the E16's SPI flash (src/disk/volume.h) and Disk mode,
 * where the computer gets it as a USB drive (the SDK's usb_msc.h).
 *
 * Who owns the volume: the firmware (FatFs, through volume.h) outside Disk
 * mode, the computer inside it -- never both. Outside Disk mode the USB
 * drive reports "medium not present", so the computer has nothing to
 * mount. Entering Disk mode unmounts FatFs and flushes the block device
 * before presenting the medium; leaving it hides the medium first, then
 * flushes and remounts.
 *
 * Concurrency: the USB mass-storage callbacks run in the USB service
 * interrupt (TIM7, priority 6). They never touch the flash or the block
 * device: a read or write is only recorded and answered USB_MSC_ASYNC,
 * and disk_mode_poll() in the main loop does the flash I/O and calls
 * usb_msc_io_done(). So all SPI flash access (this, persistence, the
 * block device) happens in the main loop, and an erase (up to ~400 ms)
 * stalls only the main loop, never USB MIDI. In Disk mode no script runs
 * and FatFs is unmounted, so the requests handed over here are the only
 * flash I/O.
 */

/* The block device's 4 KB cache (in SRAM): disk_init() and app mode's
 * own bring-up (src/apps/app_mode.c) both hand it to blockdev_init(). */
extern uint8_t disk_block_cache[];

/* At boot, after persist_init() (which brings up the flash): mounts the
 * volume, or on the first boot (no valid volume) formats it, and writes
 * the built-in scripts /scripts doesn't have (all of them on the first
 * boot) and README.TXT into it. Logs "disk: mounted, ..." or "disk:
 * formatted, wrote N scripts", and "disk: wrote N missing built-ins". */
void disk_init(void);

/* Adds the drive to USB (usb_functions.c), reporting "medium not
 * present" outside Disk mode. Before midi_init(). */
bool disk_mode_usb_start(void);

/* False if there's no flash big enough for the volume, or it couldn't be
 * mounted or formatted; Disk mode then isn't offered. */
bool disk_available(void);

/* Unmounts FatFs, flushes, and presents the medium to the computer. */
void disk_mode_enter(void);

/* Call every main-loop pass while in Disk mode: does the flash I/O for a
 * request the computer made. True once the computer has ejected the
 * medium (the caller should then disk_mode_leave()). */
bool disk_mode_poll(void);

/* Hides the medium, finishes a request in progress, flushes, remounts
 * FatFs and rebuilds the scene list from /scripts (scenes_rescan()). why
 * ends up in the log ("eject", "encoder 1", ...). */
void disk_mode_leave(const char *why);

#endif
