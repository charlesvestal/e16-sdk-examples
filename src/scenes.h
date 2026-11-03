#ifndef SCENES_H
#define SCENES_H
#include <stdbool.h>
#include <stddef.h>

#include "rt_scene.h"

/*
 * The scene provider: what the launcher menu lists and how each entry is
 * loaded. The launcher reads only this, never the embedded scripts table
 * (scripts.h) directly.
 *
 * The list is the built-in scripts, in manifest order, then the disk
 * scripts (the .lua files in /scripts on the FAT volume) that don't
 * replace one, alphabetically. A disk script whose scene name equals a
 * built-in's replaces it in place (same menu position, same saved vars/store, since
 * those are keyed by the scene name).
 *
 * Scene names of disk scripts: the file name without ".lua", lowercased
 * (ASCII). FAT's case is unreliable -- the same file can come back as
 * "Euclid.lua", "EUCLID.LUA" or "euclid.lua" depending on what wrote it
 * and whether it has a long name -- so names compare case-insensitively
 * and the lowercased form is what vars/store are saved under. Without a
 * --@scene title, the menu title is that lowercased name.
 *
 * An unchanged copy of a built-in -- byte for byte what the first boot
 * wrote to /scripts: scenes_builtin_header()'s --@scene line, then the
 * embedded source -- doesn't replace it: the built-in stays, unmarked,
 * and loads from the firmware. Editing the copy makes it a replacement.
 *
 * A disk script whose header doesn't parse (scene_parse.h) is still
 * listed, marked broken: starting it shows the error (scene name, line
 * and message) and runs nothing. A broken script with a built-in's name
 * still replaces the built-in, so the error isn't hidden behind it.
 *
 * Memory: at most SCENES_DISK_MAX disk scripts are listed, the first ones
 * alphabetically; the rest are logged as skipped. Each keeps only its
 * name, menu title and flags (about 50 bytes); the full header (one
 * scene_parsed_t, ~0.85 KB, static) is parsed again when the script
 * starts, and the source is read through a 512-byte buffer as Lua
 * parses it (rt_load_reader()), never held in RAM whole.
 *
 * Only call these from the main loop, with the volume owned by the
 * firmware (not in Disk mode).
 */

#define SCENES_DISK_MAX 24

/* Menu titles are kept to what the menu and the title bar show. */
#define SCENES_TITLE_MAX 15

/* Native apps listed (the first ones alphabetically), and the longest
 * file name (without ".e16app") listed. */
#define SCENES_APPS_MAX 16
#define SCENES_APP_STEM_MAX 31

typedef enum { SCENE_BUILTIN, SCENE_DISK, SCENE_APP } scene_source_t;

/* A SCENE_APP entry is a native app, /apps/<name>.e16app (src/apps):
 * name is the file name without ".e16app" as FAT returns it, title the
 * same cut to SCENES_TITLE_MAX. scene_open() refuses it: the launcher
 * runs it in app mode instead (src/apps/app_mode.h). */
typedef struct {
    const char *name;  /* scene name: what vars/store are saved under */
    const char *title; /* menu title (at most SCENES_TITLE_MAX bytes) */
    scene_source_t source;
    bool replaces;     /* a disk script standing in for the built-in of its name */
    bool broken;       /* header doesn't parse: starting it shows the error */
} scene_info_t;

/* The --@scene line ("--@scene title=... pages=... pad_pages=N
 * settings_page=M\n") that heads built-in b's copy on the volume, so the
 * copy carries what the manifest says. Returns its length, or 0 if it
 * doesn't fit in cap. */
size_t scenes_builtin_header(int b, char *out, size_t cap);

/* Rebuilds the list: the built-ins plus whatever /scripts holds now, then
 * the native apps in /apps (no disk scripts or apps if the volume isn't
 * mounted). At boot, and after Disk mode. Logs "disk: N scripts (M
 * replace built-ins, K broken)", plus how many are unchanged copies of
 * built-ins, and "disk: N apps". */
void scenes_rescan(void);

/* Writes each built-in that /scripts has no file for (any case) as the
 * first boot does: scenes_builtin_header(), then the source. Safe: an
 * unchanged copy never replaces its built-in. Returns how many were
 * written (logging "disk: wrote N missing built-ins" if any), or -1
 * without a volume. */
int scenes_write_missing_builtins(void);

/* True if name is a scene that can still be loaded: a built-in's name,
 * or /scripts/<name>.lua exists (FAT's case-insensitive lookup). A disk
 * error answers true. For persist_reclaim(); call only with the volume
 * mounted. */
bool scenes_name_exists(const char *name);

int scenes_count(void);

/* Entry i, 0 <= i < scenes_count(). The strings stay valid until the
 * next scenes_rescan(). */
scene_info_t scene_info(int i);

/* Loads entry i and runs its page.onInit(): rt_load() for a built-in,
 * rt_load_reader() through FatFs for a disk script. Bind and restore
 * vars/store for scene_info(i).name before calling (rt.h's restore
 * order). Returns the scene (valid while it's loaded, until the next
 * scene_open() or scenes_rescan()), or NULL if it didn't load: then
 * scenes_last_error() says why (a broken header's line and message, a
 * read error, or rt_last_error()'s Lua error). */
const rt_scene_t *scene_open(int i);
const char *scenes_last_error(void);

#endif
