#ifndef SCENE_PARSE_H
#define SCENE_PARSE_H
#include <stddef.h>
#include <stdint.h>

#include "rt_scene.h"

/*
 * Parses a disk script's header comments into an owned rt_scene_t: the
 * same rules tools/embed_scripts.py applies to the built-in scripts'
 * --@assign lines, plus the new --@scene line that gives a disk script
 * its title, pages, pad pages and settings page without needing a
 * manifest entry. Portable (no SDK headers, no malloc): the device feeds
 * it script bytes read from FatFs a chunk at a time; the host feeds it a
 * file. See scene_parse.c for the header format and the default rules.
 */

/* All of a parsed scene's strings (name, title, page names, abbrs) live
 * in this arena rather than being individually allocated. Comfortably
 * above anything the five oxi-e16-lua scripts need (their abbrs, name
 * and title together add up to well under half of this) while still
 * small enough that a pathological script -- many ids each with an
 * abbr, plus a long title -- can exhaust it; scene_parse() fails
 * cleanly in that case rather than overrunning it (see test/disk/
 * scene_parse_test.c's arena-overflow case). */
#define SCENE_PARSE_ARENA_SIZE 512

typedef struct {
    rt_scene_t scene;
    char arena[SCENE_PARSE_ARENA_SIZE];
    size_t arena_used;
} scene_parsed_t;

/* Reads up to buf_len bytes of the script into buf. Returns the number of
 * bytes placed in buf (1..buf_len), 0 at end of file, or a negative value
 * on a read error. May return fewer bytes than requested even before EOF
 * (as FatFs's f_read can at a cluster boundary) -- scene_parse() calls
 * this repeatedly until it sees 0 or a negative return. */
typedef int32_t (*scene_read_fn)(void *ud, uint8_t *buf, size_t buf_len);

typedef struct {
    int line;      /* 1-based source line, or 0 for an error not tied to one */
    char msg[80];  /* short message, no trailing punctuation */
} scene_parse_error_t;

/* Parses the header comments of the script read through read_fn/ud into
 * *out (which this zeroes first). `name` is the scene's name (the disk
 * file's name without ".lua", or a built-in's manifest name) -- it is
 * copied into out's arena and checked against RT_SCENE_NAME_MAX; it is
 * not read from the script itself.
 *
 * Returns 0 on success. Returns -1 and fills *err on a parse error (a
 * malformed --@assign/--@scene line, a name or page count out of range,
 * a duplicate --@scene line, or the arena filling up); *out is left
 * zeroed in that case. Returns -1 with err->line == 0 and a message
 * starting "read error" if read_fn itself fails. */
int scene_parse(const char *name, scene_read_fn read_fn, void *ud,
                 scene_parsed_t *out, scene_parse_error_t *err);

#endif
