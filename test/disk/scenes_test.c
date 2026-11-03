/*
 * Host test for the scene provider (src/scenes.c) over the real FAT volume
 * code (volume.c, FatFs, the block device) on a fake flash, with two fake
 * built-ins standing in for scripts_gen.c and the runtime's loads stubbed
 * out (they record what they were given; the streaming load's reader is
 * drained the way lua_load() would). Covers: the list without a volume,
 * unchanged built-in copies (not listed), a replacement (in place,
 * case-insensitive), disk-only scripts (sorted, lowercased names), broken
 * headers (listed, error with line, never loaded), over-long names, the
 * 24-script cap, and the streaming load (the whole file, in pieces of at
 * most 512 bytes), built-ins missing from /scripts written back, and the
 * native apps in /apps. Prints "ALL PASSED" and exits 0 on success.
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
#include "rt.h"
#include "scenes.h"
#include "scripts.h"
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

/* ---- two fake built-ins (scripts.h) ---- */

static const char euclid_src[] = "--@assign id=1 abbr=\"Len1\"\nfunction page.onInit() end\n";
static const char lfo_src[] = "--@assign id=1 abbr=\"Rate\"\nfunction page.onInit() end -- lfo\n";

const rt_scene_t scenes[] = {
    {.name = "euclid", .title = "Euclid", .page_names = {"Pat", "Set"}, .pages = 2, .pad_pages = 1,
     .settings_page = 2},
    {.name = "lfo", .title = "LFOx16", .page_names = {"L1-4"}, .pages = 1, .pad_pages = 1},
};
const char *const sources[] = {euclid_src, lfo_src};
const unsigned source_lens[] = {sizeof euclid_src - 1, sizeof lfo_src - 1};
const int scene_count = 2;

/* ---- the runtime's loads, stubbed ---- */

static const rt_scene_t *g_loaded_scene;
static const char *g_loaded_src; /* rt_load() */
static char g_streamed[64 * 1024];   /* rt_load_reader(): what the reader handed out */
static size_t g_streamed_len, g_max_piece;
static int g_loads, g_reader_loads;

bool rt_load(const rt_scene_t *scene, const char *src, size_t len)
{
    (void)len;
    g_loads++;
    g_loaded_scene = scene;
    g_loaded_src = src;
    return true;
}

bool rt_load_reader(const rt_scene_t *scene, lua_Reader reader, void *ud)
{
    g_reader_loads++;
    g_loaded_scene = scene;
    g_streamed_len = 0;
    g_max_piece = 0;
    for (;;) {
        size_t n = 0;
        const char *p = reader(NULL, ud, &n);
        if (!p || n == 0) {
            break;
        }
        assert(g_streamed_len + n <= sizeof g_streamed);
        memcpy(g_streamed + g_streamed_len, p, n);
        g_streamed_len += n;
        if (n > g_max_piece) {
            g_max_piece = n;
        }
    }
    return true;
}

void rt_unload(void) {}
const char *rt_last_error(void) { return ""; }

/* ---- fake NOR flash (as in fatfs_test.c) ---- */

static uint8_t *fake_flash;

static void fake_read(uint32_t addr, void *buf, size_t len)
{
    memcpy(buf, fake_flash + (addr - VOLUME_FLASH_BASE), len);
}

static void fake_write(uint32_t addr, const void *data, size_t len)
{
    const uint8_t *src = data;
    uint8_t *dst = fake_flash + (addr - VOLUME_FLASH_BASE);
    for (size_t i = 0; i < len; i++) {
        dst[i] &= src[i];
    }
}

static bool fake_erase_sector(uint32_t addr)
{
    memset(fake_flash + (addr - VOLUME_FLASH_BASE), 0xFF, FLASH_SECTOR_SIZE);
    return true;
}

static const flash_ops_t fake_ops = {.read = fake_read, .write = fake_write, .erase_sector = fake_erase_sector};
static uint8_t cache[FLASH_SECTOR_SIZE];

/* ---- helpers ---- */

static bool write_script(const char *file, const char *text)
{
    char path[96];
    snprintf(path, sizeof path, "/scripts/%s", file);
    return volume_write_file(path, text, strlen(text), NULL, 0);
}

/* What the first boot writes for built-in b. */
static bool write_builtin_copy(int b, const char *file)
{
    char path[96], head[160];
    snprintf(path, sizeof path, "/scripts/%s", file);
    size_t n = scenes_builtin_header(b, head, sizeof head);
    return n > 0 && volume_write_file(path, head, n, sources[b], source_lens[b]);
}

static void remove_script(const char *file)
{
    char path[96];
    snprintf(path, sizeof path, "/scripts/%s", file);
    f_unlink(path);
}

static int find(const char *name)
{
    for (int i = 0; i < scenes_count(); i++) {
        if (strcmp(scene_info(i).name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* ---- tests ---- */

static void test_no_volume(void)
{
    scenes_rescan();
    CHECK(scenes_count() == 2, "two built-ins without a volume (%d)", scenes_count());
    scene_info_t i0 = scene_info(0);
    CHECK(strcmp(i0.name, "euclid") == 0 && strcmp(i0.title, "Euclid") == 0 && i0.source == SCENE_BUILTIN,
          "entry 0 is the Euclid built-in");
    CHECK(scene_open(0) == &scenes[0] && g_loaded_src == euclid_src, "a built-in loads with rt_load()");
}

static void test_unchanged_copies(void)
{
    CHECK(volume_format(), "format");
    CHECK(write_builtin_copy(0, "euclid.lua") && write_builtin_copy(1, "lfo.lua"), "write the built-in copies");
    scenes_rescan();
    CHECK(scenes_count() == 2, "unchanged copies add nothing (%d)", scenes_count());
    CHECK(scene_info(0).source == SCENE_BUILTIN && scene_info(1).source == SCENE_BUILTIN,
          "unchanged copies leave the built-ins in place");
    int loads = g_loads;
    CHECK(scene_open(1) == &scenes[1] && g_loads == loads + 1, "and they load from the firmware");
}

static void test_replacement(void)
{
    /* Same as the copy but with 99 instead of nothing: changed. */
    remove_script("euclid.lua");
    CHECK(write_script("EUCLID.LUA",
                       "--@scene title=\"Euclid 99\" pages=\"Pat,Set\" pad_pages=1 settings_page=2\n"
                       "--@assign id=1 abbr=\"Len1\"\nlocal bpm = 99\nfunction page.onInit() end\n"),
          "write a changed EUCLID.LUA");
    scenes_rescan();
    CHECK(scenes_count() == 2, "a replacement doesn't add an entry (%d)", scenes_count());
    scene_info_t i0 = scene_info(0);
    CHECK(i0.source == SCENE_DISK && i0.replaces && !i0.broken, "Euclid is now the disk script, in place");
    CHECK(strcmp(i0.name, "euclid") == 0, "its scene name is lowercased: '%s'", i0.name);
    CHECK(strcmp(i0.title, "Euclid 99") == 0, "its title comes from its --@scene line: '%s'", i0.title);
    const rt_scene_t *sc = scene_open(0);
    CHECK(sc && strcmp(sc->name, "euclid") == 0 && sc->pages == 2 && sc->settings_page == 2,
          "it opens with its parsed header");
    CHECK(sc && sc->abbr[0] && strcmp(sc->abbr[0], "Len1") == 0, "abbr from its --@assign");
    CHECK(g_streamed_len > 0 && strstr(g_streamed, "local bpm = 99") != NULL, "the disk source was streamed");
}

static void test_disk_only_and_broken(void)
{
    CHECK(write_script("Hello.lua", "function page.onInit() print('hi') end\n"), "write Hello.lua");
    CHECK(write_script("bad.lua", "-- broken\n--@assign id=x\nfunction page.onInit() end\n"), "write bad.lua");
    CHECK(write_script("lfo.lua", "--@scene pages=\"\"\nfunction page.onInit() end\n"), "a broken lfo.lua");
    scenes_rescan();
    CHECK(scenes_count() == 4, "2 built-in slots + bad + hello (%d)", scenes_count());
    scene_info_t b = scene_info(2), h = scene_info(3);
    CHECK(strcmp(b.name, "bad") == 0 && strcmp(h.name, "hello") == 0, "disk-only scripts sorted: %s, %s", b.name,
          h.name);
    CHECK(h.source == SCENE_DISK && !h.replaces && !h.broken && strcmp(h.title, "hello") == 0,
          "hello: no --@scene, title is the lowercased name ('%s')", h.title);
    CHECK(b.broken && b.source == SCENE_DISK, "bad.lua is listed as broken");

    int reader_loads = g_reader_loads, loads = g_loads;
    CHECK(scene_open(2) == NULL, "a broken script doesn't open");
    CHECK(strcmp(scenes_last_error(), "bad.lua:2: --@assign id= is not a number") == 0, "its error: '%s'",
          scenes_last_error());
    CHECK(g_reader_loads == reader_loads && g_loads == loads, "and nothing was loaded");

    scene_info_t l = scene_info(1);
    CHECK(l.broken && l.replaces && strcmp(l.name, "lfo") == 0, "a broken lfo.lua still replaces the built-in");
    CHECK(scene_open(1) == NULL && strstr(scenes_last_error(), "lfo.lua:1: ") == scenes_last_error(),
          "and shows its error: '%s'", scenes_last_error());

    CHECK(scene_open(3) != NULL && g_streamed_len == strlen("function page.onInit() print('hi') end\n"),
          "hello opens and streams all of itself");
    remove_script("bad.lua");
    remove_script("lfo.lua");
    remove_script("Hello.lua");
}

static void test_long_name(void)
{
    const char *file = "a_name_that_is_much_longer_than_31_chars.lua";
    CHECK(write_script(file, "function page.onInit() end\n"), "write a long-named script");
    scenes_rescan();
    int i = scenes_count() - 1;
    scene_info_t info = scene_info(i);
    CHECK(info.broken, "an over-long name is listed as broken");
    CHECK(scene_open(i) == NULL && strstr(scenes_last_error(), "over 31 characters") != NULL, "error: '%s'",
          scenes_last_error());
    remove_script(file);
}

static void test_cap(void)
{
    /* Written in reverse order, so the kept set isn't just directory order. */
    for (int k = 29; k >= 0; k--) {
        char file[16];
        snprintf(file, sizeof file, "s%02d.lua", k);
        CHECK(write_script(file, "function page.onInit() end\n"), "write %s", file);
    }
    scenes_rescan();
    /* euclid.lua (the replacement) is a disk script too: it sorts first
     * and takes one of the 24 places, so s00..s22 are kept. */
    CHECK(scenes_count() == 2 + 23, "24 disk scripts listed (%d entries)", scenes_count());
    CHECK(find("s00") == 2 && find("s22") == 24 && find("s23") < 0 && find("s29") < 0,
          "the first ones alphabetically are kept");
    for (int k = 0; k < 30; k++) {
        char file[16];
        snprintf(file, sizeof file, "s%02d.lua", k);
        remove_script(file);
    }
}

static void test_big_streaming(void)
{
    /* ~16 KB, like TB-3PO: read in 512-byte pieces, all of it. */
    static char big[16 * 1024 + 77];
    size_t n = 0;
    n += (size_t)snprintf(big, sizeof big, "--@scene title=\"Big\"\n");
    while (n + 40 < sizeof big - 1) {
        n += (size_t)snprintf(big + n, sizeof big - n, "-- filler line %05zu to make it long\n", n);
    }
    big[n] = '\0';
    CHECK(write_script("big.lua", big), "write big.lua");
    scenes_rescan();
    int i = find("big");
    CHECK(i >= 0 && strcmp(scene_info(i).title, "Big") == 0, "big.lua listed");
    CHECK(i >= 0 && scene_open(i) != NULL, "big.lua opens");
    CHECK(g_streamed_len == n && memcmp(g_streamed, big, n) == 0, "all %zu bytes streamed (%zu)", n, g_streamed_len);
    CHECK(g_max_piece <= 512, "in pieces of at most 512 bytes (%zu)", g_max_piece);
    remove_script("big.lua");
}

/* What /scripts/<file> holds, NUL-terminated (at most cap - 1 bytes). */
static size_t read_script(const char *file, char *out, size_t cap)
{
    char path[96];
    snprintf(path, sizeof path, "/scripts/%s", file);
    size_t n = 0;
    if (volume_open(path)) {
        int32_t k;
        while (n < cap - 1 && (k = volume_read(out + n, cap - 1 - n)) > 0) {
            n += (size_t)k;
        }
        volume_close();
    }
    out[n] = '\0';
    return n;
}

/* Built-ins missing from /scripts are written at boot (disk_init()). */
static void test_missing_builtins(void)
{
    CHECK(volume_format(), "format");
    CHECK(write_builtin_copy(0, "EUCLID.LUA"), "euclid's copy, in another case");
    CHECK(scenes_write_missing_builtins() == 1, "only the missing lfo.lua is written");
    static char got[4096], want[4096];
    char head[160];
    size_t hn = scenes_builtin_header(1, head, sizeof head);
    snprintf(want, sizeof want, "%.*s%s", (int)hn, head, sources[1]);
    CHECK(read_script("lfo.lua", got, sizeof got) == strlen(want) && strcmp(got, want) == 0,
          "it's what the first boot writes (header line, then the source)");
    CHECK(read_script("EUCLID.LUA", got, sizeof got) > 0 && volume_count_scripts() == 2,
          "euclid's own copy is left as it was");
    CHECK(scenes_write_missing_builtins() == 0, "nothing is missing the second time");
    scenes_rescan();
    CHECK(scenes_count() == 2 && scene_info(0).source == SCENE_BUILTIN && scene_info(1).source == SCENE_BUILTIN,
          "written copies don't replace the built-ins");

    remove_script("lfo.lua");
    CHECK(write_script("Lfo.lua", "--@scene title=\"Mine\"\nfunction page.onInit() end\n"), "a changed Lfo.lua");
    CHECK(scenes_write_missing_builtins() == 0, "a changed copy (any case) counts as present");
    scenes_rescan();
    CHECK(scene_info(1).replaces && strcmp(scene_info(1).title, "Mine") == 0, "and still replaces the built-in");
    remove_script("Lfo.lua");
    remove_script("EUCLID.LUA");

    volume_unmount();
    CHECK(scenes_write_missing_builtins() == -1, "-1 without a volume");
    CHECK(volume_mount(), "mount again");
    CHECK(scenes_write_missing_builtins() == 2, "an empty /scripts gets every built-in");
}

static bool write_app(const char *file)
{
    char path[96];
    snprintf(path, sizeof path, "/apps/%s", file);
    return volume_write_file(path, "E16A", 4, NULL, 0);
}

/* Native apps: the .e16app files in /apps, listed after the scripts. */
static void test_apps(void)
{
    CHECK(volume_mount(), "mount (makes /apps)");
    CHECK(volume_exists("/apps"), "/apps exists");
    CHECK(write_app("Plasma.e16app") && write_app("hello.E16APP") && write_app("notes.txt") &&
              write_app(".hidden.e16app") && write_app("a_much_too_long_app_name_for_the_menu.e16app"),
          "write some files into /apps");
    CHECK(volume_mkdir("/apps/plasma"), "an app's own folder");
    CHECK(write_script("zz.lua", "function page.onInit() end\n"), "a disk script");
    scenes_rescan();
    int n = scenes_count();
    CHECK(n == 2 + 1 + 2, "built-ins, the disk script, then two apps (%d)", n);
    scene_info_t a = scene_info(n - 2), b = scene_info(n - 1), z = scene_info(n - 3);
    CHECK(z.source == SCENE_DISK && strcmp(z.name, "zz") == 0, "scripts come first");
    CHECK(a.source == SCENE_APP && strcmp(a.name, "hello") == 0 && strcmp(a.title, "hello") == 0,
          "hello first (case-insensitive order): '%s'", a.name);
    CHECK(b.source == SCENE_APP && strcmp(b.name, "Plasma") == 0, "then Plasma, its case kept: '%s'", b.name);
    CHECK(!a.broken && !a.replaces, "apps aren't broken or replacements");
    int loads = g_loads, reader_loads = g_reader_loads;
    CHECK(scene_open(n - 1) == NULL && strstr(scenes_last_error(), "is an app") != NULL,
          "scene_open() refuses an app: '%s'", scenes_last_error());
    CHECK(g_loads == loads && g_reader_loads == reader_loads, "and loads nothing");
    CHECK(strcmp(scene_info(n).name, "") == 0, "past the end: empty");
    remove_script("zz.lua");
}

/* What the boot's persistence reclaim keeps: built-ins, and scripts in
 * /scripts whatever their case. */
static void test_name_exists(void)
{
    CHECK(write_script("Keeper.LUA", "function page.onInit() end\n"), "write Keeper.LUA");
    CHECK(scenes_name_exists("euclid") && scenes_name_exists("lfo"), "built-ins exist");
    CHECK(scenes_name_exists("keeper"), "a disk script exists by its lowercased name");
    CHECK(!scenes_name_exists("gone"), "a deleted script doesn't");
    CHECK(!scenes_name_exists("0123456789012345678901234567890123456789"), "nor an over-long name");
    remove_script("Keeper.LUA");
    CHECK(!scenes_name_exists("keeper"), "once removed, it doesn't");
    volume_unmount();
    CHECK(scenes_name_exists("keeper"), "without a volume: can't tell, so it counts as there");
    CHECK(volume_mount(), "remount");
}

int main(void)
{
    fake_flash = malloc(VOLUME_FLASH_SIZE);
    assert(fake_flash);
    memset(fake_flash, 0xFF, VOLUME_FLASH_SIZE);
    blockdev_init(&fake_ops, VOLUME_FLASH_BASE, VOLUME_FLASH_SIZE, cache);

    test_no_volume();
    test_unchanged_copies();
    test_replacement();
    test_disk_only_and_broken();
    test_long_name();
    test_cap();
    test_big_streaming();
    test_missing_builtins();
    test_apps();
    test_name_exists();

    free(fake_flash);
    if (g_failures) {
        printf("%d FAILED\n", g_failures);
        return 1;
    }
    printf("ALL PASSED\n");
    return 0;
}
