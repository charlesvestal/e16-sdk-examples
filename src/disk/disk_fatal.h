#ifndef DISK_FATAL_H
#define DISK_FATAL_H

/*
 * A bug that would read or write flash outside where a caller knows it's
 * allowed (blockdev.c's [DISK_BASE, DISK_END), persist_slots.c's
 * persistence reserve) means something's corrupted -- the stock firmware's
 * data, the FAT volume, or the persistence reserve -- and isn't safe to
 * limp on from. Logging and calling this is each caller's response
 * (require_in_bounds() in both files); plain abort() would do on the host,
 * but on the device it just hangs with no USB and nothing to see why, by
 * construction: these checks exist for exactly the case where no debugger
 * is attached.
 *
 * disk_fatal_host.c (host build): logs (log_printf) and abort()s, like an
 * assert that can't be compiled out.
 * disk_fatal_e16.c (device build): logs and reboots into the bootloader
 * (system_enter_bootloader()), the escape persist.c has used since phase
 * 1 -- it stays reachable afterwards instead of hanging.
 */
void disk_fatal(const char *why) __attribute__((noreturn));

#endif
