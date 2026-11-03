/*
 * Parses a script's header comments into an rt_scene_t: --@assign lines
 * (same syntax and meaning as tools/embed_scripts.py's assigns(), and
 * external/oxi-e16-lua/tools/make_scene.py before it) plus a --@scene
 * line giving the scene's title, pages, pad pages and settings page:
 *
 *   --@scene title="..." pages="Page A,Page B" pad_pages=N settings_page=M
 *   --@assign id=N [abbr="..."] [other_key=value ...]
 *
 * Any key --@assign carries besides id= and abbr= (name=, l=, h=, ...) is
 * read and ignored here, same as embed_scripts.py: those are for
 * external/oxi-e16-lua/tools/make_scene.py, which wires a scene's
 * controls from them.
 *
 * Without a --@scene line, or for any key it omits, the default is:
 * title from the scene's name, one pad page, no settings page, and the
 * page list derived from the --@assign ids actually used -- one page if
 * every id is <= 32, or two ("P1", "Set") if any id is >= 33, since ids
 * 33-64 are the settings-page convention (see make_scene.py).
 *
 * Every --@ line in the file is scanned, not just a leading block: that
 * matches embed_scripts.py, whose regex runs over the whole source, and
 * it's cheap even for the longest script here (TB-3PO, ~16 KB).
 *
 * This file takes nothing but a byte-at-a-time read callback, so it
 * builds for the host and for the device without pulling in FatFs or
 * anything SDK-specific; the device feeds it script bytes read from
 * FatFs a sector or so at a time.
 */
#include "scene_parse.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* One source line, truncated to this many bytes if longer (the longest
 * --@assign line across the five bundled scripts is under 90 bytes, and
 * a --@scene line usually isn't much longer). get_line() keeps reading
 * an overlong line's remainder -- just not storing it -- so the stream
 * stays in sync for the next line either way. */
#define SCENE_LINE_MAX 256

/* read_fn is called with a buffer this size at a time. Independent of
 * SCENE_LINE_MAX; a line can span many chunks. */
#define SCENE_STREAM_CHUNK 64

typedef struct {
    scene_read_fn read_fn;
    void *ud;
    uint8_t chunk[SCENE_STREAM_CHUNK];
    int32_t chunk_len;
    int32_t chunk_pos;
    int eof;
    int error; /* read_fn returned < 0 */
} scene_stream_t;

static int stream_getc(scene_stream_t *s) {
    while (s->chunk_pos >= s->chunk_len) {
        if (s->eof) {
            return -1;
        }
        int32_t n = s->read_fn(s->ud, s->chunk, sizeof s->chunk);
        if (n < 0) {
            s->error = 1;
            s->eof = 1;
            return -1;
        }
        if (n == 0) {
            s->eof = 1;
            return -1;
        }
        s->chunk_len = n;
        s->chunk_pos = 0;
    }
    return s->chunk[s->chunk_pos++];
}

/* Reads one line into buf (NUL-terminated, trailing \n and \r\n both
 * stripped), truncating to buf_size - 1 bytes if it's longer -- the
 * remainder is still read (and discarded) so the next call starts at the
 * following line. Returns the (possibly truncated) line length, or -1 at
 * clean end of file or on a read error (*had_error tells which). */
static int get_line(scene_stream_t *s, char *buf, size_t buf_size, int *had_error) {
    int c = stream_getc(s);
    if (c < 0) {
        *had_error = s->error;
        return -1;
    }
    size_t len = 0;
    while (c >= 0 && c != '\n') {
        if (len + 1 < buf_size) {
            buf[len++] = (char)c;
        }
        c = stream_getc(s);
    }
    if (len > 0 && buf[len - 1] == '\r') {
        len--;
    }
    buf[len] = '\0';
    *had_error = s->error;
    return (int)len;
}

static void set_err(scene_parse_error_t *err, int line, const char *msg) {
    err->line = line;
    size_t n = strlen(msg);
    if (n >= sizeof err->msg) {
        n = sizeof err->msg - 1;
    }
    memcpy(err->msg, msg, n);
    err->msg[n] = '\0';
}

static char *arena_push(scene_parsed_t *out, const char *s, size_t len) {
    if (out->arena_used + len + 1 > sizeof out->arena) {
        return NULL;
    }
    char *dst = out->arena + out->arena_used;
    memcpy(dst, s, len);
    dst[len] = '\0';
    out->arena_used += len + 1;
    return dst;
}

static void copy_trunc(char *dst, size_t dst_size, const char *src, size_t src_len) {
    if (src_len >= dst_size) {
        src_len = dst_size - 1;
    }
    memcpy(dst, src, src_len);
    dst[src_len] = '\0';
}

static int is_word(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

/* Scans forward from *pp for the next `key=value` token, same syntax as
 * the regex embed_scripts.py and make_scene.py use
 * (r'(\w+)=("[^"]*"|\S+)'): a run of word characters, '=', then either a
 * double-quoted string (quotes stripped, no escapes, same as the regex)
 * or a run of non-space characters. Advances *pp past the match. Returns
 * 1 with key/val filled (truncated to fit, always NUL-terminated), or 0
 * once no more tokens remain on the line. */
static int next_kv(const char **pp, char *key, size_t key_size, char *val, size_t val_size) {
    const char *p = *pp;
    for (;;) {
        while (*p && !is_word(*p)) {
            p++;
        }
        if (!*p) {
            *pp = p;
            return 0;
        }
        const char *kstart = p;
        while (is_word(*p)) {
            p++;
        }
        if (*p != '=') {
            /* No '=' right after this word: not a match here. A regex
             * engine would retry one character further in; so do we. */
            p = kstart + 1;
            continue;
        }
        copy_trunc(key, key_size, kstart, (size_t)(p - kstart));
        p++; /* skip '=' */
        if (*p == '"') {
            p++;
            const char *vstart = p;
            while (*p && *p != '"') {
                p++;
            }
            copy_trunc(val, val_size, vstart, (size_t)(p - vstart));
            if (*p == '"') {
                p++;
            }
        } else {
            const char *vstart = p;
            while (*p && !isspace((unsigned char)*p)) {
                p++;
            }
            copy_trunc(val, val_size, vstart, (size_t)(p - vstart));
        }
        *pp = p;
        return 1;
    }
}

static int parse_uint(const char *val, int *out_val) {
    if (!*val) {
        return 0;
    }
    char *end;
    long v = strtol(val, &end, 10);
    if (end == val || *end != '\0' || v < 0 || v > 255) {
        return 0;
    }
    *out_val = (int)v;
    return 1;
}

static int split_pages(scene_parsed_t *out, const char *val, int line_no, scene_parse_error_t *err) {
    int n = 0;
    const char *p = val;
    while (*p) {
        const char *start = p;
        while (*p && *p != ',') {
            p++;
        }
        size_t len = (size_t)(p - start);
        if (len > 0) { /* a run between commas that isn't empty */
            if (n >= RT_MAX_PAGES) {
                set_err(err, line_no, "--@scene pages: too many (max 12)");
                return -1;
            }
            char *copy = arena_push(out, start, len);
            if (!copy) {
                set_err(err, line_no, "scene arena full");
                return -1;
            }
            out->scene.page_names[n++] = copy;
        }
        if (*p == ',') {
            p++;
        }
    }
    if (n < 1) {
        set_err(err, line_no, "--@scene pages: need at least one page name");
        return -1;
    }
    return n;
}

static int parse_assign_line(const char *rest, int line_no, scene_parsed_t *out,
                              uint64_t *used_mask, scene_parse_error_t *err) {
    char key[16];
    char val[SCENE_LINE_MAX];
    int have_id = 0;
    long id = 0;
    int have_abbr = 0;
    char abbr[SCENE_LINE_MAX];
    abbr[0] = '\0';
    const char *p = rest;
    while (next_kv(&p, key, sizeof key, val, sizeof val)) {
        if (!strcmp(key, "id")) {
            char *end;
            id = strtol(val, &end, 10);
            if (end == val || *end != '\0') {
                set_err(err, line_no, "--@assign id= is not a number");
                return -1;
            }
            have_id = 1;
        } else if (!strcmp(key, "abbr")) {
            copy_trunc(abbr, sizeof abbr, val, strlen(val));
            have_abbr = 1;
        }
        /* other keys (name=, l=, h=, manual=, g=, p=, ...) are for
         * make_scene.py's control wiring, not the scene list. */
    }
    if (!have_id) {
        set_err(err, line_no, "--@assign with no id=");
        return -1;
    }
    if (id < 1 || id > 64) {
        set_err(err, line_no, "--@assign id= out of range 1-64");
        return -1;
    }
    unsigned bit = (unsigned)(id - 1);
    out->scene.assigned[bit / 8] |= (uint8_t)(1u << (bit % 8));
    *used_mask |= (uint64_t)1 << bit;
    if (have_abbr) {
        /* Truncated to 4 bytes, matching embed_scripts.py's abbr[:4] --
         * the UI's per-control label is that short regardless. */
        size_t alen = strlen(abbr);
        if (alen > 4) {
            alen = 4;
        }
        char *copy = arena_push(out, abbr, alen);
        if (!copy) {
            set_err(err, line_no, "scene arena full");
            return -1;
        }
        out->scene.abbr[bit] = copy;
    }
    return 0;
}

static int parse_scene_line(const char *rest, int line_no, scene_parsed_t *out, int *have_title,
                             int *have_pages, int *have_pad, int *have_settings, int *pad_val,
                             int *settings_val, scene_parse_error_t *err) {
    char key[16];
    char val[SCENE_LINE_MAX];
    const char *p = rest;
    while (next_kv(&p, key, sizeof key, val, sizeof val)) {
        if (!strcmp(key, "title")) {
            char *copy = arena_push(out, val, strlen(val));
            if (!copy) {
                set_err(err, line_no, "scene arena full");
                return -1;
            }
            out->scene.title = copy;
            *have_title = 1;
        } else if (!strcmp(key, "pages")) {
            int n = split_pages(out, val, line_no, err);
            if (n < 0) {
                return -1;
            }
            out->scene.pages = (uint8_t)n;
            *have_pages = 1;
        } else if (!strcmp(key, "pad_pages")) {
            if (!parse_uint(val, pad_val)) {
                set_err(err, line_no, "--@scene pad_pages= is not a number");
                return -1;
            }
            *have_pad = 1;
        } else if (!strcmp(key, "settings_page")) {
            if (!parse_uint(val, settings_val)) {
                set_err(err, line_no, "--@scene settings_page= is not a number");
                return -1;
            }
            *have_settings = 1;
        }
        /* an unknown key is ignored, same as the assign line above */
    }
    return 0;
}

int scene_parse(const char *name, scene_read_fn read_fn, void *ud, scene_parsed_t *out,
                 scene_parse_error_t *err) {
    memset(out, 0, sizeof *out);
    err->line = 0;
    err->msg[0] = '\0';

    size_t name_len = strlen(name);
    if (name_len == 0 || name_len > RT_SCENE_NAME_MAX) {
        set_err(err, 0, "scene name too long");
        return -1;
    }
    char *name_copy = arena_push(out, name, name_len);
    if (!name_copy) {
        set_err(err, 0, "scene arena full");
        return -1;
    }
    out->scene.name = name_copy;

    scene_stream_t s;
    memset(&s, 0, sizeof s);
    s.read_fn = read_fn;
    s.ud = ud;

    int scene_seen = 0;
    int scene_line_no = 0;
    int have_title = 0, have_pages = 0, have_pad = 0, have_settings = 0;
    int pad_val = 0, settings_val = 0;
    uint64_t used_mask = 0;
    int line_no = 0;
    char line[SCENE_LINE_MAX];

    for (;;) {
        int had_error = 0;
        int len = get_line(&s, line, sizeof line, &had_error);
        if (len < 0) {
            if (had_error) {
                set_err(err, 0, "read error");
                return -1;
            }
            break; /* clean end of file */
        }
        line_no++;
        if (len >= 9 && !strncmp(line, "--@assign", 9)) {
            if (parse_assign_line(line + 9, line_no, out, &used_mask, err) != 0) {
                memset(out, 0, sizeof *out);
                return -1;
            }
        } else if (len >= 8 && !strncmp(line, "--@scene", 8)) {
            if (scene_seen) {
                set_err(err, line_no, "duplicate --@scene line");
                memset(out, 0, sizeof *out);
                return -1;
            }
            scene_seen = 1;
            scene_line_no = line_no;
            if (parse_scene_line(line + 8, line_no, out, &have_title, &have_pages, &have_pad,
                                  &have_settings, &pad_val, &settings_val, err) != 0) {
                memset(out, 0, sizeof *out);
                return -1;
            }
        }
        /* any other line, --@ or not, carries no scene information */
    }

    if (!have_title) {
        out->scene.title = out->scene.name;
    }
    if (!have_pages) {
        /* ids 33-64 are the settings-page convention (make_scene.py); a
         * script using one needs a second page to hold it. */
        int needs_settings = (used_mask >> 32) != 0;
        out->scene.page_names[0] = "P1";
        if (needs_settings) {
            out->scene.page_names[1] = "Set";
            out->scene.pages = 2;
        } else {
            out->scene.pages = 1;
        }
    }
    if (!have_pad) {
        pad_val = 1;
    }
    if (!have_settings) {
        settings_val = (!have_pages && (used_mask >> 32) != 0) ? 2 : 0;
    }

    if (pad_val > out->scene.pages || settings_val > out->scene.pages) {
        set_err(err, scene_seen ? scene_line_no : 0, "pad_pages/settings_page out of range");
        memset(out, 0, sizeof *out);
        return -1;
    }
    out->scene.pad_pages = (uint8_t)pad_val;
    out->scene.settings_page = (uint8_t)settings_val;
    return 0;
}
