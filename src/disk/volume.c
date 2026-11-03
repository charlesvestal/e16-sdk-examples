/*
 * The FAT volume through FatFs: volume.h.
 */
#include "volume.h"

#include <string.h>

#include "blockdev.h"
#include "ff.h"
#include "diskio.h" /* after ff.h, which it needs */
#include "log.h"

static FATFS g_fs;
static FIL g_file;
static bool g_mounted;

const char volume_readme[] =
    "E16 Launcher\n"
    "============\n"
    "\n"
    "This drive appears while the E16 is in Disk mode (the last entry in\n"
    "the launcher menu). Put Lua scripts in the /scripts folder as\n"
    "NAME.lua, then eject the drive (or press encoder 1 on the E16) to\n"
    "leave Disk mode. The launcher reads /scripts again when it leaves.\n"
    "\n"
    "The scene name is the file name without .lua, at most 31 characters.\n"
    "Saved vars and store are kept by that name. A script with the same\n"
    "name as a built-in one replaces it in the menu.\n"
    "\n"
    "Script header\n"
    "-------------\n"
    "\n"
    "Optional lines anywhere in the script, each starting at the beginning\n"
    "of a line (they are Lua comments, so the script runs unchanged):\n"
    "\n"
    "--@scene title=\"My Scene\" pages=\"Seq,Set\" pad_pages=1 settings_page=2\n"
    "\n"
    "  title          the menu entry and default header\n"
    "                 (default: the file name)\n"
    "  pages          page names, comma-separated, at most 12\n"
    "                 (default: from the controls the --@assign lines use)\n"
    "  pad_pages      pages 1..N use the pad controls, ids 1-32 (default 1)\n"
    "  settings_page  the page using the settings controls, ids 33-64\n"
    "                 (default 0: none)\n"
    "\n"
    "--@assign id=1 abbr=\"Len1\" name=\"T1 Length\" l=0 h=127 manual=true g=1\n"
    "\n"
    "  One line per control the script uses, as in the E16's Lua editor.\n"
    "  id is 1-64; only declared ids are wired. abbr (up to 4 characters)\n"
    "  is the control's label until the script sets its own.\n"
    "\n"
    "The built-in scripts in /scripts are examples of both.\n";

/* f_mount() with the drive's error logged. */
static bool mount(void)
{
    FRESULT r = f_mount(&g_fs, "", 1);
    if (r != FR_OK) {
        f_mount(NULL, "", 0);
        g_mounted = false;
        if (r != FR_NO_FILESYSTEM) {
            log_printf("disk: mount failed (FatFs error %d)", (int)r);
        }
        return false;
    }
    g_mounted = true;
    return true;
}

bool volume_mount(void)
{
    if (!mount()) {
        return false;
    }
    static const char *const dirs[] = {VOLUME_SCRIPTS_DIR, VOLUME_APPS_DIR};
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        FRESULT r = f_mkdir(dirs[i]);
        if (r != FR_OK && r != FR_EXIST) {
            log_printf("disk: can't make %s (FatFs error %d)", dirs[i], (int)r);
        }
    }
    return true;
}

void volume_unmount(void)
{
    f_mount(NULL, "", 0);
    g_mounted = false;
}

/*
 * f_setlabel() writes the label into the root directory, which is what
 * FatFs and Windows read; the boot sector keeps f_mkfs's "NO NAME" and a
 * generic "FAT" type string. Put the label (and "FAT16") there too, so
 * nothing that reads the boot sector's copy names the volume "NO NAME".
 */
static bool patch_boot_sector(void)
{
    enum { BS_VOL_LAB = 43, BS_FIL_SYS_TYPE = 54 }; /* FAT12/16 boot sector */
    uint8_t sec[BLOCKDEV_BLOCK_SIZE];
    LBA_t vbr = g_fs.volbase;
    if (disk_read(0, sec, vbr, 1) != RES_OK) {
        return false;
    }
    char lab[11];
    memset(lab, ' ', sizeof lab);
    memcpy(lab, VOLUME_LABEL, sizeof VOLUME_LABEL - 1);
    memcpy(sec + BS_VOL_LAB, lab, sizeof lab);
    if (g_fs.fs_type == FS_FAT16) {
        memcpy(sec + BS_FIL_SYS_TYPE, "FAT16   ", 8);
    }
    return disk_write(0, sec, vbr, 1) == RES_OK && blockdev_flush();
}

bool volume_format(void)
{
    volume_unmount();
    /* 2 KB clusters: 11.5 MB is then ~5900 clusters, inside FAT16's
     * 4086-65525 (4 KB clusters would make it FAT12). align 0: f_mkfs
     * asks diskio for the erase block (one 4 KB flash sector). */
    static const MKFS_PARM opt = {.fmt = FM_FAT, .n_fat = 2, .align = 0, .n_root = 512, .au_size = 2048};
    uint8_t work[FF_MAX_SS];
    FRESULT r = f_mkfs("", &opt, work, sizeof work);
    if (r != FR_OK) {
        log_printf("disk: format failed (FatFs error %d)", (int)r);
        return false;
    }
    if (!mount()) {
        log_printf("disk: format didn't take");
        return false;
    }
    if (g_fs.fs_type != FS_FAT16) {
        log_printf("disk: formatted as FAT type %d, not FAT16", (int)g_fs.fs_type);
    }
    r = f_setlabel(VOLUME_LABEL);
    if (r != FR_OK) {
        log_printf("disk: can't set the label (FatFs error %d)", (int)r);
    }
    if (!patch_boot_sector()) {
        log_printf("disk: can't label the boot sector");
    }
    r = f_mkdir(VOLUME_SCRIPTS_DIR);
    if (r != FR_OK) {
        log_printf("disk: can't make %s (FatFs error %d)", VOLUME_SCRIPTS_DIR, (int)r);
        return false;
    }
    return volume_write_file("/README.TXT", volume_readme, sizeof volume_readme - 1, NULL, 0);
}

uint32_t volume_fat_lba(void)
{
    return g_mounted ? (uint32_t)g_fs.fatbase : UINT32_MAX;
}

static bool write_all(const void *data, size_t len)
{
    UINT n = 0;
    return len == 0 || (f_write(&g_file, data, (UINT)len, &n) == FR_OK && n == len);
}

bool volume_write_file(const char *path, const void *head, size_t head_len, const void *body,
                       size_t body_len)
{
    if (!g_mounted) {
        return false;
    }
    FRESULT r = f_open(&g_file, path, FA_WRITE | FA_CREATE_ALWAYS);
    if (r != FR_OK) {
        log_printf("disk: can't create %s (FatFs error %d)", path, (int)r);
        return false;
    }
    bool ok = write_all(head, head_len) && write_all(body, body_len);
    r = f_close(&g_file);
    if (!ok || r != FR_OK) {
        log_printf("disk: writing %s failed (FatFs error %d)", path, (int)r);
        return false;
    }
    return true;
}

bool volume_rewrite_file(const char *path, const void *data, size_t len)
{
    if (!g_mounted) {
        return false;
    }
    FRESULT r = f_open(&g_file, path, FA_WRITE | FA_OPEN_ALWAYS);
    if (r != FR_OK) {
        log_printf("disk: can't open %s for writing (FatFs error %d)", path, (int)r);
        return false;
    }
    bool ok = write_all(data, len) && f_truncate(&g_file) == FR_OK;
    r = f_close(&g_file);
    if (!ok || r != FR_OK) {
        log_printf("disk: writing %s failed (FatFs error %d)", path, (int)r);
        return false;
    }
    return true;
}

/* s ends with ext (".lua"), ASCII case-insensitively, with something before it. */
static bool ends_with(const char *s, const char *ext)
{
    size_t n = strlen(s), e = strlen(ext);
    if (n <= e) {
        return false;
    }
    for (size_t i = 0; i < e; i++) {
        if ((s[n - e + i] | 0x20) != (ext[i] | 0x20)) {
            return false;
        }
    }
    return true;
}

int volume_list(const char *dir_path, const char *ext, volume_script_fn fn, void *ud)
{
    if (!g_mounted) {
        return -1;
    }
    DIR dir;
    FILINFO fi;
    if (f_opendir(&dir, dir_path) != FR_OK) {
        return -1;
    }
    int count = 0;
    size_t e = strlen(ext);
    while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0] != '\0') {
        if ((fi.fattrib & (AM_DIR | AM_HID | AM_SYS)) || fi.fname[0] == '.') {
            continue;
        }
        if (ends_with(fi.fname, ext)) {
            count++;
            if (fn) {
                size_t n = strlen(fi.fname) - e;
                fi.fname[n] = '\0';
                fn(ud, fi.fname, n);
            }
        }
    }
    f_closedir(&dir);
    return count;
}

int volume_list_scripts(volume_script_fn fn, void *ud)
{
    return volume_list(VOLUME_SCRIPTS_DIR, ".lua", fn, ud);
}

int volume_count_scripts(void)
{
    return volume_list_scripts(NULL, NULL);
}

bool volume_exists(const char *path)
{
    FILINFO fi;
    return g_mounted && f_stat(path, &fi) == FR_OK;
}

volume_lookup_t volume_lookup(const char *path)
{
    if (!g_mounted) {
        return VOLUME_LOOKUP_ERROR;
    }
    FILINFO fi;
    FRESULT r = f_stat(path, &fi);
    if (r == FR_OK) {
        return VOLUME_EXISTS;
    }
    return (r == FR_NO_FILE || r == FR_NO_PATH) ? VOLUME_MISSING : VOLUME_LOOKUP_ERROR;
}

bool volume_mounted(void)
{
    return g_mounted;
}

bool volume_mkdir(const char *path)
{
    if (!g_mounted) {
        return false;
    }
    FRESULT r = f_mkdir(path);
    return r == FR_OK || r == FR_EXIST;
}

bool volume_open(const char *path)
{
    if (!g_mounted) {
        return false;
    }
    FRESULT r = f_open(&g_file, path, FA_READ);
    if (r != FR_OK) {
        log_printf("disk: can't open %s (FatFs error %d)", path, (int)r);
        return false;
    }
    return true;
}

bool volume_same_file(const char *path, const void *data, size_t len)
{
    if (!g_mounted || f_open(&g_file, path, FA_READ) != FR_OK) {
        return false;
    }
    bool same = f_size(&g_file) == len;
    const uint8_t *p = data;
    uint8_t buf[128];
    for (size_t off = 0; same && off < len;) {
        UINT n = 0;
        size_t want = len - off < sizeof buf ? len - off : sizeof buf;
        if (f_read(&g_file, buf, (UINT)want, &n) != FR_OK || n != want || memcmp(buf, p + off, want) != 0) {
            same = false;
        }
        off += want;
    }
    f_close(&g_file);
    return same;
}

uint32_t volume_open_size(void)
{
    return (uint32_t)f_size(&g_file);
}

int32_t volume_read(void *buf, size_t len)
{
    return volume_read_script(buf, len);
}

void volume_close(void)
{
    f_close(&g_file);
}

bool volume_open_script(const char *stem)
{
    if (!g_mounted) {
        return false;
    }
    char path[sizeof VOLUME_SCRIPTS_DIR + VOLUME_STEM_MAX + 5];
    size_t n = strlen(stem);
    if (n > VOLUME_STEM_MAX) {
        return false;
    }
    memcpy(path, VOLUME_SCRIPTS_DIR "/", sizeof VOLUME_SCRIPTS_DIR);
    memcpy(path + sizeof VOLUME_SCRIPTS_DIR, stem, n);
    memcpy(path + sizeof VOLUME_SCRIPTS_DIR + n, ".lua", 5);
    return volume_open(path);
}

int32_t volume_read_script(void *buf, size_t len)
{
    UINT n = 0;
    if (f_read(&g_file, buf, (UINT)len, &n) != FR_OK) {
        return -1;
    }
    return (int32_t)n;
}

void volume_close_script(void)
{
    f_close(&g_file);
}
