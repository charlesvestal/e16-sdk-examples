/*
 * Host test for the FAT volume (src/disk/volume.c, src/disk/diskio.c and
 * FatFs) over the block device (src/disk/blockdev.c) and a fake NOR flash
 * the size of the real one (11.5 MB at 0x400000, in RAM). Covers: no
 * volume on blank flash, formatting (FAT16, 2 KB clusters, label "E16" in
 * both places, /scripts, README.TXT), writing and reading back files,
 * listing /scripts and counting its scripts, and a remount after a
 * simulated reboot (a fresh block device cache over the same flash).
 * Prints "ALL PASSED" and exits 0 on success, like the other C host tests.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blockdev.h"
#include "ff.h"
#include "flash_ops.h"
#include "volume.h"

static int g_failures;
#define CHECK(cond, ...)                                                                           \
    do {                                                                                            \
        if (!(cond)) {                                                                              \
            g_failures++;                                                                           \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                    \
            fprintf(stderr, __VA_ARGS__);                                                           \
            fprintf(stderr, "\n");                                                                  \
        }                                                                                           \
    } while (0)

/* ---- fake NOR flash over the volume's real address range ---- */

#define FAKE_BASE VOLUME_FLASH_BASE
#define FAKE_SIZE VOLUME_FLASH_SIZE

static uint8_t *fake_flash;
static int fake_write_violations;

static void fake_offset_ok(uint32_t addr, size_t len)
{
    assert(addr >= FAKE_BASE);
    assert((uint64_t)addr + len <= (uint64_t)FAKE_BASE + FAKE_SIZE);
}

static void fake_read(uint32_t addr, void *buf, size_t len)
{
    fake_offset_ok(addr, len);
    memcpy(buf, fake_flash + (addr - FAKE_BASE), len);
}

static void fake_write(uint32_t addr, const void *data, size_t len)
{
    fake_offset_ok(addr, len);
    const uint8_t *src = data;
    uint8_t *dst = fake_flash + (addr - FAKE_BASE);
    for (size_t i = 0; i < len; i++) {
        if ((uint8_t)(src[i] & (uint8_t)~dst[i]) != 0) {
            fake_write_violations++;
        }
        dst[i] = (uint8_t)(dst[i] & src[i]);
    }
}

static bool fake_erase_sector(uint32_t addr)
{
    assert(addr % FLASH_SECTOR_SIZE == 0);
    fake_offset_ok(addr, FLASH_SECTOR_SIZE);
    memset(fake_flash + (addr - FAKE_BASE), 0xFF, FLASH_SECTOR_SIZE);
    return true;
}

static const flash_ops_t fake_ops = {
    .read = fake_read,
    .write = fake_write,
    .erase_sector = fake_erase_sector,
};

static uint8_t cache[FLASH_SECTOR_SIZE];

/* ---- helpers ---- */

/* Reads path into buf (cap bytes); returns its length, or -1. */
static long read_file(const char *path, void *buf, size_t cap)
{
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) {
        return -1;
    }
    UINT n = 0;
    FRESULT r = f_read(&f, buf, (UINT)cap, &n);
    f_close(&f);
    return r == FR_OK ? (long)n : -1;
}

static bool listed(const char *dirpath, const char *name)
{
    DIR d;
    FILINFO fi;
    bool found = false;
    if (f_opendir(&d, dirpath) != FR_OK) {
        return false;
    }
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        if (strcmp(fi.fname, name) == 0) {
            found = true;
        }
    }
    f_closedir(&d);
    return found;
}

static const char script_head[] = "--@scene title=\"X\" pages=\"Main\" pad_pages=1 settings_page=0\n";
static const char script_body[] = "function page.onInit()\n  print(\"hello from disk\")\nend\n";

#define BIG_LEN (100u * 1024u + 123u)

static void fill_big(uint8_t *p)
{
    for (uint32_t i = 0; i < BIG_LEN; i++) {
        p[i] = (uint8_t)(i * 7u + (i >> 9));
    }
}

/* ---- tests ---- */

static void test_blank_has_no_volume(void)
{
    CHECK(!volume_mount(), "blank flash mounts nothing");
    CHECK(volume_count_scripts() == -1, "no script count without a volume");
}

static void test_format(void)
{
    CHECK(volume_format(), "format succeeds");

    DWORD free_clst = 0;
    FATFS *fs = NULL;
    CHECK(f_getfree("", &free_clst, &fs) == FR_OK && fs, "f_getfree after format");
    if (fs) {
        CHECK(fs->fs_type == FS_FAT16, "FAT16 (got type %d)", fs->fs_type);
        CHECK(fs->csize == 4, "2 KB clusters (got %u sectors)", (unsigned)fs->csize);
        CHECK(fs->n_fatent - 2 > 4085 && fs->n_fatent - 2 < 65525, "cluster count %lu in FAT16 range",
              (unsigned long)(fs->n_fatent - 2));
        CHECK(fs->database % (FLASH_SECTOR_SIZE / BLOCKDEV_BLOCK_SIZE) == 0,
              "data area aligned to a flash sector (sector %lu)", (unsigned long)fs->database);
        CHECK(fs->volbase != 0, "partitioned (MBR), volume at sector %lu", (unsigned long)fs->volbase);
        printf("volume: %lu clusters of 2 KB, %lu free, data at sector %lu\n",
               (unsigned long)(fs->n_fatent - 2), (unsigned long)free_clst, (unsigned long)fs->database);

        /* The label in the boot sector (FAT16's BS_VolLab at 43, type at 54). */
        uint8_t vbr[BLOCKDEV_BLOCK_SIZE];
        CHECK(blockdev_read((uint32_t)fs->volbase, 1, vbr), "read the boot sector");
        CHECK(memcmp(vbr + 43, "E16        ", 11) == 0, "boot sector label is E16");
        CHECK(memcmp(vbr + 54, "FAT16   ", 8) == 0, "boot sector says FAT16");
        CHECK(vbr[510] == 0x55 && vbr[511] == 0xAA, "boot sector signature");

        /* Disk mode watches FAT entry 1's clean-shutdown bit (disk_mode.c):
         * the first FAT sector starts F8 FF, then entry 1 with bit 15 set. */
        uint8_t fat[BLOCKDEV_BLOCK_SIZE];
        CHECK(volume_fat_lba() == (uint32_t)fs->fatbase, "volume_fat_lba is the first FAT sector");
        CHECK(blockdev_read(volume_fat_lba(), 1, fat), "read the first FAT sector");
        CHECK(fat[0] == 0xF8 && fat[1] == 0xFF && (fat[3] & 0x80), "FAT starts F8 FF, entry 1 marked clean");
    }

    char label[24] = {0};
    DWORD vsn = 0;
    CHECK(f_getlabel("", label, &vsn) == FR_OK && strcmp(label, "E16") == 0, "root label is E16 (got '%s')",
          label);

    FILINFO fi;
    CHECK(f_stat(VOLUME_SCRIPTS_DIR, &fi) == FR_OK && (fi.fattrib & AM_DIR), "/scripts is a directory");

    static char buf[4096];
    long n = read_file("/README.TXT", buf, sizeof buf);
    CHECK(n == (long)strlen(volume_readme) && memcmp(buf, volume_readme, (size_t)n) == 0,
          "README.TXT holds the readme (%ld bytes)", n);
    CHECK(strstr(volume_readme, "--@scene") && strstr(volume_readme, "--@assign"),
          "README documents both header lines");
    CHECK(volume_count_scripts() == 0, "no scripts yet");
}

static void test_files(uint8_t *big)
{
    CHECK(volume_write_file("/scripts/x.lua", script_head, sizeof script_head - 1, script_body,
                            sizeof script_body - 1),
          "write /scripts/x.lua");
    static char buf[1024];
    long n = read_file("/scripts/x.lua", buf, sizeof buf);
    size_t want = sizeof script_head - 1 + sizeof script_body - 1;
    CHECK(n == (long)want && memcmp(buf, script_head, sizeof script_head - 1) == 0 &&
              memcmp(buf + sizeof script_head - 1, script_body, sizeof script_body - 1) == 0,
          "x.lua reads back (%ld bytes)", n);

    /* Rewriting replaces rather than appends. */
    CHECK(volume_write_file("/scripts/x.lua", script_head, sizeof script_head - 1, script_body,
                            sizeof script_body - 1),
          "rewrite x.lua");
    CHECK(read_file("/scripts/x.lua", buf, sizeof buf) == (long)want, "rewrite keeps the length");

    /* Only real .lua files count. */
    CHECK(volume_write_file("/scripts/Long Name Script.LUA", "-- y\n", 5, NULL, 0), "write an LFN .LUA");
    CHECK(volume_write_file("/scripts/._x.lua", "\0\5\26\7", 4, NULL, 0), "write an AppleDouble file");
    CHECK(volume_write_file("/scripts/notes.txt", "hi\n", 3, NULL, 0), "write a non-script");
    CHECK(f_mkdir("/scripts/dir.lua") == FR_OK, "make a directory named like a script");
    CHECK(volume_count_scripts() == 2, "two scripts (got %d)", volume_count_scripts());
    CHECK(listed(VOLUME_SCRIPTS_DIR, "x.lua"), "x.lua listed");
    CHECK(listed(VOLUME_SCRIPTS_DIR, "Long Name Script.LUA"), "long name listed as written");
    CHECK(listed("/", "README.TXT") && listed("/", "scripts"), "root lists README.TXT and scripts");

    /* A file spanning many clusters and flash sectors. */
    fill_big(big);
    CHECK(volume_write_file("/big.bin", big, BIG_LEN, NULL, 0), "write a 100 KB file");
    static uint8_t back[BIG_LEN + 16];
    CHECK(read_file("/big.bin", back, sizeof back) == (long)BIG_LEN && memcmp(back, big, BIG_LEN) == 0,
          "100 KB file reads back");
    CHECK(!blockdev_dirty(), "closing a file leaves nothing in the block cache (CTRL_SYNC flushed it)");
}

static void test_remount(const uint8_t *big)
{
    volume_unmount();
    CHECK(blockdev_flush(), "flush");
    CHECK(volume_count_scripts() == -1, "nothing to count while unmounted");
    CHECK(volume_fat_lba() == UINT32_MAX, "no FAT while unmounted");

    /* A reboot: a fresh, empty cache over the same flash. */
    memset(cache, 0x5A, sizeof cache);
    blockdev_init(&fake_ops, FAKE_BASE, FAKE_SIZE, cache);
    CHECK(volume_mount(), "mounts after a reboot");
    CHECK(volume_count_scripts() == 2, "still two scripts");

    static char buf[1024];
    long n = read_file("/scripts/x.lua", buf, sizeof buf);
    CHECK(n > 0 && strstr(buf, "hello from disk") != NULL, "x.lua survives the remount");
    static uint8_t back[BIG_LEN];
    CHECK(read_file("/big.bin", back, sizeof back) == (long)BIG_LEN && memcmp(back, big, BIG_LEN) == 0,
          "100 KB file survives the remount");
    CHECK(f_unlink("/big.bin") == FR_OK && read_file("/big.bin", back, 1) == -1, "delete it");

    /* /scripts comes back if someone deleted it. */
    CHECK(f_unlink("/scripts/x.lua") == FR_OK && f_unlink("/scripts/Long Name Script.LUA") == FR_OK &&
              f_unlink("/scripts/._x.lua") == FR_OK && f_unlink("/scripts/notes.txt") == FR_OK &&
              f_unlink("/scripts/dir.lua") == FR_OK && f_unlink(VOLUME_SCRIPTS_DIR) == FR_OK,
          "empty and remove /scripts");
    CHECK(volume_mount(), "remount");
    FILINFO fi;
    CHECK(f_stat(VOLUME_SCRIPTS_DIR, &fi) == FR_OK && (fi.fattrib & AM_DIR), "mount remade /scripts");
    CHECK(volume_count_scripts() == 0, "empty /scripts");
}

int main(void)
{
    fake_flash = malloc(FAKE_SIZE);
    uint8_t *big = malloc(BIG_LEN);
    assert(fake_flash && big);
    memset(fake_flash, 0xFF, FAKE_SIZE);
    blockdev_init(&fake_ops, FAKE_BASE, FAKE_SIZE, cache);

    test_blank_has_no_volume();
    test_format();
    test_files(big);
    test_remount(big);

    CHECK(fake_write_violations == 0, "no NOR write violations (%d)", fake_write_violations);
    printf("erases since the last init: %lu\n", (unsigned long)blockdev_erase_count());
    free(big);
    free(fake_flash);
    if (g_failures) {
        printf("%d FAILED\n", g_failures);
        return 1;
    }
    printf("ALL PASSED\n");
    return 0;
}
