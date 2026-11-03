#ifndef APP_FILES_H
#define APP_FILES_H
#include <stdbool.h>
#include <stdint.h>

/*
 * The flash side of the app api's file_write() (src/apps/api.c) and of
 * app mode's way out (app_mode.c): portable over the FAT volume, the
 * block device and the erase-rate budget, so it's host-tested
 * (test/apps/app_files_test.c) on a fake flash and a fake clock.
 *
 * Why it's careful. Writing a file rewrites the FAT (two copies) and the
 * directory sector, through a one-sector block cache, so it needs several
 * erases, some of them of the same sector. If the budget refused one
 * partway, the file would be left half-written -- and the old way of
 * writing (truncate, then write: FatFs's FA_CREATE_ALWAYS) left a 0-byte
 * file after a reset. So a write starts only if the budget can cover its
 * worst case (flash_budget_available()), and then runs as one operation
 * the budget doesn't cut short (flash_budget_op_begin()); otherwise it's
 * refused with E16_EBUSY before FatFs is touched. And it writes in place
 * (volume_rewrite_file()): the file keeps its clusters, so rewriting it
 * at the same length erases only its data sectors, not the FAT or the
 * directory.
 *
 * The numbers, measured on test/apps/app_files_test.c's fake flash: a
 * small file's first write (making /apps/<name> too) takes 8 erases, 3
 * of them of the folder's directory sector; a same-length rewrite, one
 * or two (its data). With the free space fragmented, a 64 KB write took
 * up to 40 (16 of them the data itself). So a write needs
 * APP_FILE_WRITE_ERASES plus one per 4 KB of data in the bucket (at most
 * its 16-erase burst) to start, and may use up to APP_FILE_WRITE_OP_MAX,
 * more than twice the worst seen. The per-sector cap (4 erases of any
 * one sector a minute) then limits rewriting one file to about 4 times
 * a minute.
 */
#define APP_FILE_WRITE_ERASES 8u
#define APP_FILE_WRITE_OP_MAX 96u

/* Writes path (under dir, which is made if missing) with len bytes and
 * puts it on flash. 0, or E16_EBUSY (nothing touched), E16_EARGS (len
 * over E16_FILE_WRITE_MAX) or E16_EIO. */
int app_files_write(const char *dir, const char *path, const void *buf, uint32_t len);

/* The block device's cache onto flash before a reset (the app's exit,
 * the launch into app mode): under the Disk-mode budget, retried for a
 * bounded time (disk_flush_burst()). False if it couldn't. */
bool app_files_flush(void);

#endif
