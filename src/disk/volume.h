#ifndef VOLUME_H
#define VOLUME_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The FAT volume on the block device (blockdev.h), through FatFs
 * (external/fatfs, configured by src/disk/ffconf.h). Portable: the device
 * (src/disk_mode.c) and the host test (test/disk/fatfs_test.c) both use it
 * over whatever flash the block device was initialized on.
 *
 * Layout: an MBR with one FAT16 partition, label "E16" (so macOS mounts it
 * as /Volumes/E16), 2 KB clusters, the data area aligned to the 4 KB flash
 * sectors. Scripts live in /scripts; README.TXT at the root explains the
 * script header format.
 *
 * Only one FATFS (and the FIL this file uses) exists, in static RAM here;
 * FatFs is only ever called from the main loop.
 */

/* Where the volume lives on the E16's 16 MB SPI NOR: above the stock
 * firmware's data (0x000000-0x3FFFFF), below the persistence reserve
 * (persist_slots.h, 0xF80000-0xFFFFFF). 11.5 MB. */
#define VOLUME_FLASH_BASE 0x400000u
#define VOLUME_FLASH_END 0xF80000u
#define VOLUME_FLASH_SIZE (VOLUME_FLASH_END - VOLUME_FLASH_BASE)

#define VOLUME_LABEL "E16"
#define VOLUME_SCRIPTS_DIR "/scripts"
#define VOLUME_APPS_DIR "/apps" /* native apps, *.e16app (src/apps) */

/* Mounts the volume (rereading it from the block device, so call it again
 * after something else -- the computer, in Disk mode -- has written to
 * it). Makes /scripts and /apps if they're missing. False if there's no valid FAT
 * volume (or a disk error); the volume is then unmounted. */
bool volume_mount(void);

/* Formats the block device (FAT16, label "E16"), mounts it, and creates
 * /scripts and README.TXT. Everything that was on it is lost. */
bool volume_format(void);

/* Unmounts. Anything FatFs had buffered is already on the block device
 * (every write here closes its file), and blockdev_flush() puts that on
 * flash. */
void volume_unmount(void);

/* Writes path (replacing it if it exists) as head followed by body (either
 * may be NULL with length 0), then closes it. False on any FatFs error,
 * logged. */
bool volume_write_file(const char *path, const void *head, size_t head_len, const void *body,
                       size_t body_len);

/* Writes path with len bytes over what it held, in place: the clusters
 * it already has are reused, and only the difference in length touches
 * the FAT (a rewrite of the same length rewrites only its data sectors;
 * the directory entry comes out byte-for-byte the same). Makes the file
 * if it's missing. False on any FatFs error, logged. */
bool volume_rewrite_file(const char *path, const void *data, size_t len);

/* The number of .lua files in /scripts (case-insensitive extension),
 * skipping directories, hidden and system files and names starting with
 * '.' (macOS's ._ AppleDouble files). -1 if /scripts can't be read. */
int volume_count_scripts(void);

/* The longest file name (without ".lua") FatFs hands back: FF_MAX_LFN
 * (ffconf.h) minus the extension. */
#define VOLUME_STEM_MAX 60

/* volume_count_scripts(), also calling fn with each script's file name
 * without ".lua" (as stored: FAT keeps the case it was written with, but
 * looks names up case-insensitively), in directory order. The name is
 * only valid during the call. Don't open or write files from fn. */
typedef void (*volume_script_fn)(void *ud, const char *stem, size_t len);
int volume_list_scripts(volume_script_fn fn, void *ud);

/* Reading one script, /scripts/<stem>.lua (the stem matched
 * case-insensitively, as FAT does). One file at a time: it shares the
 * one FIL volume_write_file() uses. volume_read_script() returns the
 * bytes read (0 at the end) or -1 on an error; it reads straight into
 * buf, through FatFs's one sector window (FF_FS_TINY), so it needs
 * little stack -- the Lua parser calls it from deep in its recursion
 * (rt_load_reader()). */
bool volume_open_script(const char *stem);
int32_t volume_read_script(void *buf, size_t len);
void volume_close_script(void);

/* Any directory: volume_list_scripts() for the files in dir_path ending
 * in ext (case-insensitive), fn getting each name without ext. */
int volume_list(const char *dir_path, const char *ext, volume_script_fn fn, void *ud);

/* True if path names a file or directory (FAT's case-insensitive lookup). */
bool volume_exists(const char *path);

/* volume_exists() telling "not there" apart from "couldn't look" (no
 * volume, a disk error). */
typedef enum { VOLUME_EXISTS, VOLUME_MISSING, VOLUME_LOOKUP_ERROR } volume_lookup_t;
volume_lookup_t volume_lookup(const char *path);

/* True while a volume is mounted (volume_mount()/_format() succeeded and
 * nothing has unmounted it since). */
bool volume_mounted(void);

/* Makes a directory; true if it exists afterwards. */
bool volume_mkdir(const char *path);

/* Reading any file by path, through the same one FIL as the scripts (so
 * one file at a time, and not while a script is open): open (the error is
 * logged), its size, read (bytes read, 0 at the end, -1 on an error),
 * close. */
bool volume_open(const char *path);
uint32_t volume_open_size(void);
int32_t volume_read(void *buf, size_t len);
void volume_close(void);

/* True if path exists and holds exactly len bytes of data (quietly false
 * if it's missing). Uses the same FIL as the above. */
bool volume_same_file(const char *path, const void *data, size_t len);

/* The block (LBA) of the mounted volume's first FAT sector, or
 * UINT32_MAX if nothing is mounted. Disk mode watches the computer's
 * writes to it (disk_mode.c). */
uint32_t volume_fat_lba(void);

/* The README.TXT text written by volume_format() (for tests). */
extern const char volume_readme[];

#endif
