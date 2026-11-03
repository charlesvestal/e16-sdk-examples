/*
 * The scene provider: scenes.h.
 */
#include "scenes.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "log.h"
#include "rt.h"
#include "scene_parse.h"
#include "scripts.h"
#include "volume.h"
#include "ccm.h"

#include "lauxlib.h"

/* The built-ins' override table is sized for this many (the manifest has
 * five); more are listed but can't be replaced from disk. */
#define BUILTIN_MAX 16

#define F_BROKEN 0x01
#define F_NAME_LONG 0x02 /* file name over RT_SCENE_NAME_MAX: never opened */
#define F_SAME 0x04      /* an unchanged copy of the built-in of its name: not listed */

typedef struct {
    char name[RT_SCENE_NAME_MAX + 1]; /* lowercased stem (truncated if F_NAME_LONG) */
    char title[SCENES_TITLE_MAX + 1];
    int8_t builtin;                   /* the built-in it replaces, or -1 */
    uint8_t flags;
} disk_entry_t;

/* Disk scripts, sorted by name. */
static disk_entry_t g_disk[SCENES_DISK_MAX] CCM_BSS;
static int g_disk_n;
static int g_skipped;

/* Menu order: built-in i shows g_disk[g_override[i]] if that's >= 0; the
 * disk-only scripts follow, g_extra[0..g_extra_n). */
static int8_t g_override[BUILTIN_MAX];
static int8_t g_extra[SCENES_DISK_MAX];
static int g_extra_n;

/* The header of the disk script being scanned, or running. */
static scene_parsed_t g_parsed CCM_BSS;

/* The streaming load's read buffer. */
#define READ_BUF 512
static char g_read_buf[READ_BUF] CCM_BSS;

/* Native apps (the .e16app files in /apps), sorted by name, case-insensitively. */
typedef struct {
    char stem[SCENES_APP_STEM_MAX + 1];
    char title[SCENES_TITLE_MAX + 1];
} app_entry_t;
static app_entry_t g_apps[SCENES_APPS_MAX] CCM_BSS;
static int g_app_n;
static int g_apps_skipped;

/* Why the last scene_open() failed, if not in the runtime. */
static char g_error[128] CCM_BSS;

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

static bool same_name(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        if (lower(*a) != lower(*b)) {
            return false;
        }
    }
    return *a == *b;
}

/* ---- the scan ---- */

/* volume_list_scripts() callback: keeps the first SCENES_DISK_MAX names
 * alphabetically, in order (insertion sort; the directory is small). */
static void collect(void *ud, const char *stem, size_t len)
{
    (void)ud;
    disk_entry_t e;
    memset(&e, 0, sizeof e);
    e.builtin = -1;
    if (len > RT_SCENE_NAME_MAX) {
        e.flags = F_BROKEN | F_NAME_LONG;
        len = RT_SCENE_NAME_MAX;
    }
    for (size_t i = 0; i < len; i++) {
        e.name[i] = lower(stem[i]);
    }

    int pos = g_disk_n;
    while (pos > 0 && strcmp(g_disk[pos - 1].name, e.name) > 0) {
        pos--;
    }
    if (pos >= SCENES_DISK_MAX) {
        log_printf("disk: %s.lua skipped (over %d scripts)", e.name, SCENES_DISK_MAX);
        g_skipped++;
        return;
    }
    if (g_disk_n == SCENES_DISK_MAX) {
        log_printf("disk: %s.lua skipped (over %d scripts)", g_disk[SCENES_DISK_MAX - 1].name, SCENES_DISK_MAX);
        g_skipped++;
        g_disk_n--;
    }
    memmove(&g_disk[pos + 1], &g_disk[pos], (size_t)(g_disk_n - pos) * sizeof g_disk[0]);
    g_disk[pos] = e;
    g_disk_n++;
}

/* ASCII case-insensitive strcmp. */
static int compare_nocase(const char *a, const char *b)
{
    for (; *a && lower(*a) == lower(*b); a++, b++) {
    }
    return (unsigned char)lower(*a) - (unsigned char)lower(*b);
}

/* volume_list() callback for /apps: the first SCENES_APPS_MAX names
 * alphabetically, in order. */
static void collect_app(void *ud, const char *stem, size_t len)
{
    (void)ud;
    if (len > SCENES_APP_STEM_MAX) {
        log_printf("disk: %.*s.e16app skipped (name over %d characters)", (int)len, stem, SCENES_APP_STEM_MAX);
        g_apps_skipped++;
        return;
    }
    app_entry_t e;
    memset(&e, 0, sizeof e);
    memcpy(e.stem, stem, len);
    memcpy(e.title, stem, len < SCENES_TITLE_MAX ? len : SCENES_TITLE_MAX);
    int pos = g_app_n;
    while (pos > 0 && compare_nocase(g_apps[pos - 1].stem, e.stem) > 0) {
        pos--;
    }
    if (pos >= SCENES_APPS_MAX) {
        log_printf("disk: %s.e16app skipped (over %d apps)", e.stem, SCENES_APPS_MAX);
        g_apps_skipped++;
        return;
    }
    if (g_app_n == SCENES_APPS_MAX) {
        log_printf("disk: %s.e16app skipped (over %d apps)", g_apps[SCENES_APPS_MAX - 1].stem, SCENES_APPS_MAX);
        g_apps_skipped++;
        g_app_n--;
    }
    memmove(&g_apps[pos + 1], &g_apps[pos], (size_t)(g_app_n - pos) * sizeof g_apps[0]);
    g_apps[pos] = e;
    g_app_n++;
}

static void scan_apps(void)
{
    g_app_n = 0;
    g_apps_skipped = 0;
    if (volume_list(VOLUME_APPS_DIR, ".e16app", collect_app, NULL) < 0) {
        return; /* no /apps: no apps */
    }
    log_printf("disk: %d apps", g_app_n);
}

size_t scenes_builtin_header(int b, char *out, size_t cap)
{
    const rt_scene_t *sc = &scenes[b];
    int n = snprintf(out, cap, "--@scene title=\"%s\" pages=\"", sc->title);
    for (int p = 0; p < sc->pages && n > 0 && (size_t)n < cap; p++) {
        n += snprintf(out + n, cap - (size_t)n, "%s%s", p ? "," : "", sc->page_names[p] ? sc->page_names[p] : "");
    }
    if (n > 0 && (size_t)n < cap) {
        n += snprintf(out + n, cap - (size_t)n, "\" pad_pages=%d settings_page=%d\n", sc->pad_pages,
                      sc->settings_page);
    }
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

/* Compares a disk script, as the header parse streams it, with what the
 * first boot wrote for a built-in: its header line, then its source. */
typedef struct {
    const char *head, *src;
    size_t head_len, src_len, pos;
    bool same;
} compare_t;

static void compare(compare_t *c, const uint8_t *buf, size_t n)
{
    while (c->same && n > 0) {
        const char *want;
        size_t avail;
        if (c->pos < c->head_len) {
            want = c->head + c->pos;
            avail = c->head_len - c->pos;
        } else if (c->pos - c->head_len < c->src_len) {
            want = c->src + (c->pos - c->head_len);
            avail = c->src_len - (c->pos - c->head_len);
        } else {
            c->same = false; /* longer than the built-in */
            break;
        }
        size_t k = n < avail ? n : avail;
        if (memcmp(buf, want, k) != 0) {
            c->same = false;
        }
        buf += k;
        n -= k;
        c->pos += k;
    }
}

/* scene_parse()'s reader; ud is a compare_t, or NULL to just read. */
static int32_t parse_read(void *ud, uint8_t *buf, size_t len)
{
    int32_t n = volume_read_script(buf, len);
    if (ud && n > 0) {
        compare(ud, buf, (size_t)n);
    }
    return n;
}

/* Parses disk entry d's header into g_parsed (comparing the file with
 * cmp's built-in on the way, if cmp isn't NULL). False with g_error set
 * to "<name>.lua:<line>: <message>" if it doesn't parse. */
static bool parse_header(const disk_entry_t *d, compare_t *cmp)
{
    if (d->flags & F_NAME_LONG) {
        snprintf(g_error, sizeof g_error, "%s...: file name over %d characters (without .lua)", d->name,
                 RT_SCENE_NAME_MAX);
        return false;
    }
    if (!volume_open_script(d->name)) {
        snprintf(g_error, sizeof g_error, "%s.lua: can't open", d->name);
        return false;
    }
    scene_parse_error_t err;
    int r = scene_parse(d->name, parse_read, cmp, &g_parsed, &err);
    volume_close_script();
    if (r != 0) {
        if (err.line > 0) {
            snprintf(g_error, sizeof g_error, "%s.lua:%d: %s", d->name, err.line, err.msg);
        } else {
            snprintf(g_error, sizeof g_error, "%s.lua: %s", d->name, err.msg);
        }
        return false;
    }
    return true;
}

void scenes_rescan(void)
{
    /* g_error and g_parsed are in CCM (ccm.h): not zeroed at startup. */
    g_error[0] = '\0';
    memset(&g_parsed, 0, sizeof g_parsed);
    g_disk_n = 0;
    g_skipped = 0;
    g_extra_n = 0;
    g_app_n = 0;
    for (int i = 0; i < BUILTIN_MAX; i++) {
        g_override[i] = -1;
    }
    if (volume_list_scripts(collect, NULL) < 0) {
        log_printf("disk: no scripts (volume not mounted)");
        return;
    }

    int replaced = 0, broken = 0, same = 0;
    for (int d = 0; d < g_disk_n; d++) {
        disk_entry_t *e = &g_disk[d];
        int b = -1;
        for (int k = 0; k < scene_count && k < BUILTIN_MAX; k++) {
            if (same_name(e->name, scenes[k].name)) {
                b = k;
                break;
            }
        }
        char head[160];
        compare_t cmp = {0};
        if (b >= 0) {
            cmp.head = head;
            cmp.head_len = scenes_builtin_header(b, head, sizeof head);
            cmp.src = sources[b];
            cmp.src_len = source_lens[b];
            cmp.same = cmp.head_len > 0;
        }
        if (parse_header(e, b >= 0 ? &cmp : NULL)) {
            size_t n = strlen(g_parsed.scene.title);
            if (n > SCENES_TITLE_MAX) {
                n = SCENES_TITLE_MAX;
            }
            memcpy(e->title, g_parsed.scene.title, n);
            e->title[n] = '\0';
            if (b >= 0 && cmp.same && cmp.pos == cmp.head_len + cmp.src_len) {
                e->flags |= F_SAME;
                same++;
                continue; /* the built-in stays */
            }
        } else {
            e->flags |= F_BROKEN;
            memcpy(e->title, e->name, SCENES_TITLE_MAX);
            e->title[SCENES_TITLE_MAX] = '\0';
            log_printf("disk: broken: %s", g_error);
            broken++;
        }
        if (b >= 0) {
            e->builtin = (int8_t)b;
            g_override[b] = (int8_t)d;
            replaced++;
        } else {
            g_extra[g_extra_n++] = (int8_t)d;
        }
    }
    memset(&g_parsed, 0, sizeof g_parsed);
    log_printf("disk: %d scripts (%d replace built-ins, %d broken; %d unchanged built-in copies)", g_disk_n,
               replaced, broken, same);
    if (g_skipped) {
        log_printf("disk: %d more skipped: only the first %d alphabetically are listed", g_skipped,
                   SCENES_DISK_MAX);
    }
    scan_apps();
}

int scenes_write_missing_builtins(void)
{
    if (volume_count_scripts() < 0) {
        return -1;
    }
    int written = 0;
    for (int b = 0; b < scene_count; b++) {
        char path[sizeof VOLUME_SCRIPTS_DIR + RT_SCENE_NAME_MAX + 8], head[160];
        snprintf(path, sizeof path, "%s/%s.lua", VOLUME_SCRIPTS_DIR, scenes[b].name);
        if (volume_exists(path)) {
            continue; /* FAT's lookup ignores case: "Euclid.lua" counts */
        }
        size_t head_len = scenes_builtin_header(b, head, sizeof head);
        if (head_len > 0 && volume_write_file(path, head, head_len, sources[b], source_lens[b])) {
            written++;
        }
    }
    if (written > 0) {
        log_printf("disk: wrote %d missing built-ins", written);
    }
    return written;
}

bool scenes_name_exists(const char *name)
{
    for (int b = 0; b < scene_count; b++) {
        if (strcmp(scenes[b].name, name) == 0) {
            return true;
        }
    }
    char path[sizeof VOLUME_SCRIPTS_DIR + RT_SCENE_NAME_MAX + 8];
    int n = snprintf(path, sizeof path, "%s/%s.lua", VOLUME_SCRIPTS_DIR, name);
    if (n < 0 || (size_t)n >= sizeof path) {
        return false; /* longer than any scene name */
    }
    /* Anything but a clean "no such file" counts as there: a read error
     * mustn't look like a deleted script. */
    return volume_lookup(path) != VOLUME_MISSING;
}

/* ---- the list ---- */

int scenes_count(void)
{
    return scene_count + g_extra_n + g_app_n;
}

/* Entry i's disk script, or NULL for a built-in shown as itself. */
static const disk_entry_t *disk_entry(int i)
{
    if (i < scene_count) {
        return (i < BUILTIN_MAX && g_override[i] >= 0) ? &g_disk[g_override[i]] : NULL;
    }
    return i - scene_count < g_extra_n ? &g_disk[g_extra[i - scene_count]] : NULL;
}

scene_info_t scene_info(int i)
{
    scene_info_t info = {0};
    if (i < 0 || i >= scenes_count()) {
        info.name = info.title = "";
        return info;
    }
    if (i >= scene_count + g_extra_n) {
        const app_entry_t *a = &g_apps[i - scene_count - g_extra_n];
        info.name = a->stem;
        info.title = a->title;
        info.source = SCENE_APP;
        return info;
    }
    const disk_entry_t *d = disk_entry(i);
    if (!d) {
        info.name = scenes[i].name;
        info.title = scenes[i].title;
        info.source = SCENE_BUILTIN;
        return info;
    }
    info.name = d->name;
    info.title = d->title;
    info.source = SCENE_DISK;
    info.replaces = d->builtin >= 0;
    info.broken = (d->flags & F_BROKEN) != 0;
    return info;
}

/* ---- loading ---- */

/* rt_load_reader()'s reader: the next buffer-full of the open script.
 * Called from inside lua_load(), which runs protected, so a read error
 * can be raised as a load error. */
static const char *lua_read(lua_State *L, void *ud, size_t *size)
{
    const char *name = ud;
    int32_t n = volume_read_script(g_read_buf, sizeof g_read_buf);
    if (n < 0) {
        luaL_error(L, "%s.lua: read error", name);
    }
    *size = n > 0 ? (size_t)n : 0;
    return n > 0 ? g_read_buf : NULL;
}

const rt_scene_t *scene_open(int i)
{
    g_error[0] = '\0';
    if (i < 0 || i >= scenes_count()) {
        snprintf(g_error, sizeof g_error, "no scene %d", i);
        return NULL;
    }
    if (i >= scene_count + g_extra_n) {
        snprintf(g_error, sizeof g_error, "%s is an app, not a script", scene_info(i).name);
        return NULL;
    }
    const disk_entry_t *d = disk_entry(i);
    if (!d) {
        return rt_load(&scenes[i], sources[i], source_lens[i]) ? &scenes[i] : NULL;
    }

    /* Unload whatever ran before g_parsed is overwritten: the runtime
     * still points at the previous disk scene's header otherwise. */
    rt_unload();
    if (!parse_header(d, NULL)) {
        log_printf("%s", g_error);
        return NULL;
    }
    if (!volume_open_script(d->name)) {
        snprintf(g_error, sizeof g_error, "%s.lua: can't open", d->name);
        return NULL;
    }
    bool ok = rt_load_reader(&g_parsed.scene, lua_read, (void *)d->name);
    volume_close_script();
    return ok ? &g_parsed.scene : NULL;
}

/* The provider's own error (a header, or a file that can't be opened),
 * else the runtime's. */
const char *scenes_last_error(void)
{
    return g_error[0] ? g_error : rt_last_error();
}
