/*
 * Host test for src/disk/scene_parse.c. Plain C, no Lua -- run directly
 * (not through e16rt-host), like test/disk/blockdev_test.c.
 *
 * Two kinds of checks:
 *  - Unit tests that feed scene_parse() small in-memory "scripts" and
 *    check the parsed scene or the error, covering the defaulting rules
 *    and the edge cases the plan calls out (a bad line, an overlong
 *    name, a duplicate --@scene line, a quoted value with a space, a
 *    line longer than the parser's line buffer, and an arena overflow).
 *  - A comparison against tools/embed_scripts.py's --dump-json for the
 *    five external/oxi-e16-lua scripts (see below for why this is the
 *    comparison to make).
 *
 * Why this comparison: embed_scripts.py's normal output (scenes[] for
 * scripts_gen.c) always takes title/pages/pad_pages/settings_page from
 * the manifest, which wins over --@scene and the defaults -- so it
 * wouldn't exercise scene_parse.c's own defaulting logic at all, and
 * comparing against it would really be testing the manifest, not the
 * parser. --dump-json instead runs the exact same --@scene/--@assign/
 * defaults logic embed_scripts.py's scene_header() implements (see its
 * docstring), with no manifest involved -- the "header-only" parse of
 * each script. That is what scene_parse() computes too, so the two
 * should agree exactly: same assigned ids, same abbrs, and (since none
 * of these five scripts carry a --@scene line) the same defaulted title,
 * pages, pad_pages and settings_page.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scene_parse.h"

static int g_failures;
#define CHECK(cond, ...)                                                                                     \
    do {                                                                                                      \
        if (!(cond)) {                                                                                         \
            g_failures++;                                                                                       \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                \
            fprintf(stderr, __VA_ARGS__);                                                                       \
            fprintf(stderr, "\n");                                                                              \
        }                                                                                                       \
    } while (0)

/* ---- feeding scene_parse() from a block of memory ---- */

typedef struct {
    const char *data;
    size_t len;
    size_t pos;
} mem_reader_t;

static int32_t mem_read(void *ud, uint8_t *buf, size_t buf_len) {
    mem_reader_t *r = ud;
    size_t left = r->len - r->pos;
    if (left == 0) {
        return 0;
    }
    size_t n = left < buf_len ? left : buf_len;
    memcpy(buf, r->data + r->pos, n);
    r->pos += n;
    return (int32_t)n;
}

static int parse_mem(const char *name, const char *src, scene_parsed_t *out,
                      scene_parse_error_t *err) {
    mem_reader_t r = {src, strlen(src), 0};
    return scene_parse(name, mem_read, &r, out, err);
}

/* ---- feeding scene_parse() from a real file, a chunk at a time ---- */

static int32_t file_read(void *ud, uint8_t *buf, size_t buf_len) {
    FILE *f = ud;
    size_t n = fread(buf, 1, buf_len, f);
    if (n == 0 && ferror(f)) {
        return -1;
    }
    return (int32_t)n;
}

static int parse_file(const char *name, const char *path, scene_parsed_t *out,
                       scene_parse_error_t *err) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return -2;
    }
    int rc = scene_parse(name, file_read, f, out, err);
    fclose(f);
    return rc;
}

/* ---- unit tests ---- */

static void test_defaults_pad_only(void) {
    scene_parsed_t out;
    scene_parse_error_t err;
    const char *src = "-- a script with no --@scene line\n"
                       "--@assign id=1 abbr=\"A\"\n"
                       "--@assign id=2 abbr=\"B\"\n"
                       "local x = 1\n";
    CHECK(parse_mem("euclid", src, &out, &err) == 0, "defaults: parse failed: %s", err.msg);
    CHECK(!strcmp(out.scene.name, "euclid"), "defaults: name, got %s", out.scene.name);
    CHECK(!strcmp(out.scene.title, "euclid"), "defaults: title falls back to name, got %s",
          out.scene.title);
    CHECK(out.scene.pages == 1, "defaults: one page, got %u", out.scene.pages);
    CHECK(!strcmp(out.scene.page_names[0], "P1"), "defaults: page 1 name, got %s",
          out.scene.page_names[0]);
    CHECK(out.scene.pad_pages == 1, "defaults: pad_pages, got %u", out.scene.pad_pages);
    CHECK(out.scene.settings_page == 0, "defaults: no settings page, got %u",
          out.scene.settings_page);
    CHECK((out.scene.assigned[0] & 0x03) == 0x03, "defaults: ids 1,2 assigned");
    CHECK(!strcmp(out.scene.abbr[0], "A"), "defaults: abbr 1");
    CHECK(!strcmp(out.scene.abbr[1], "B"), "defaults: abbr 2");
}

static void test_defaults_with_settings_id(void) {
    scene_parsed_t out;
    scene_parse_error_t err;
    const char *src = "--@assign id=1 abbr=\"A\"\n"
                       "--@assign id=33 abbr=\"BPM\"\n";
    CHECK(parse_mem("thing", src, &out, &err) == 0, "defaults+settings: parse failed: %s", err.msg);
    CHECK(out.scene.pages == 2, "defaults+settings: two pages, got %u", out.scene.pages);
    CHECK(!strcmp(out.scene.page_names[0], "P1"), "defaults+settings: page 1 name");
    CHECK(!strcmp(out.scene.page_names[1], "Set"), "defaults+settings: page 2 name");
    CHECK(out.scene.pad_pages == 1, "defaults+settings: pad_pages, got %u", out.scene.pad_pages);
    CHECK(out.scene.settings_page == 2, "defaults+settings: settings page, got %u",
          out.scene.settings_page);
}

static void test_scene_line_and_quoted_spaces(void) {
    scene_parsed_t out;
    scene_parse_error_t err;
    const char *src = "--@scene title=\"My Great Scene\" pages=\"Pat,Set\" pad_pages=1 settings_page=2\n"
                       "--@assign id=1 abbr=\"Foo Bar\"\n";
    CHECK(parse_mem("thing", src, &out, &err) == 0, "--@scene: parse failed: %s", err.msg);
    CHECK(!strcmp(out.scene.title, "My Great Scene"), "--@scene: quoted title with spaces, got %s",
          out.scene.title);
    CHECK(out.scene.pages == 2, "--@scene: pages=, got %u", out.scene.pages);
    CHECK(!strcmp(out.scene.page_names[0], "Pat"), "--@scene: page 1 name");
    CHECK(!strcmp(out.scene.page_names[1], "Set"), "--@scene: page 2 name");
    CHECK(out.scene.pad_pages == 1, "--@scene: pad_pages=");
    CHECK(out.scene.settings_page == 2, "--@scene: settings_page=");
    /* "Foo Bar" (a quoted value containing a space) is truncated to 4
     * bytes, same as embed_scripts.py's abbr[:4]. */
    CHECK(!strcmp(out.scene.abbr[0], "Foo "), "--@scene: abbr with a space truncated, got %s",
          out.scene.abbr[0]);
}

static void test_malformed_line(void) {
    scene_parsed_t out;
    scene_parse_error_t err;
    const char *src = "-- header\n"
                       "--@assign abbr=\"X\"\n"; /* no id= */
    CHECK(parse_mem("thing", src, &out, &err) == -1, "malformed: expected an error");
    CHECK(err.line == 2, "malformed: line number, got %d", err.line);
    CHECK(strstr(err.msg, "id=") != NULL, "malformed: message mentions id=, got %s", err.msg);
}

static void test_bad_assign_id(void) {
    scene_parsed_t out;
    scene_parse_error_t err;
    const char *src = "--@assign id=99 abbr=\"X\"\n";
    CHECK(parse_mem("thing", src, &out, &err) == -1, "bad id: expected an error");
    CHECK(err.line == 1, "bad id: line number, got %d", err.line);
}

static void test_overlong_name(void) {
    scene_parsed_t out;
    scene_parse_error_t err;
    char name[RT_SCENE_NAME_MAX + 2];
    memset(name, 'a', sizeof name - 1);
    name[sizeof name - 1] = '\0'; /* RT_SCENE_NAME_MAX + 1 bytes, one over the limit */
    CHECK(parse_mem(name, "--@assign id=1\n", &out, &err) == -1, "overlong name: expected an error");
    CHECK(err.line == 0, "overlong name: not tied to a line, got %d", err.line);
}

static void test_duplicate_scene(void) {
    scene_parsed_t out;
    scene_parse_error_t err;
    const char *src = "--@scene title=\"A\"\n"
                       "--@assign id=1\n"
                       "--@scene title=\"B\"\n";
    CHECK(parse_mem("thing", src, &out, &err) == -1, "duplicate --@scene: expected an error");
    CHECK(err.line == 3, "duplicate --@scene: line number, got %d", err.line);
    CHECK(strstr(err.msg, "uplicate") != NULL, "duplicate --@scene: message, got %s", err.msg);
}

static void test_overlong_line_is_skipped_not_fatal(void) {
    scene_parsed_t out;
    scene_parse_error_t err;
    /* A comment line far longer than the parser's line buffer, followed
     * by a normal --@assign line: the overlong line must not desync the
     * reader or corrupt what follows. */
    char long_comment[600];
    memset(long_comment, 'x', sizeof long_comment - 1);
    long_comment[sizeof long_comment - 1] = '\0';
    char src[900];
    snprintf(src, sizeof src, "-- %s\n--@assign id=5 abbr=\"OK\"\n", long_comment);
    CHECK(parse_mem("thing", src, &out, &err) == 0, "overlong non-directive line: parse failed: %s",
          err.msg);
    CHECK((out.scene.assigned[0] & 0x10) != 0, "overlong non-directive line: id 5 still assigned");
    CHECK(!strcmp(out.scene.abbr[4], "OK"), "overlong non-directive line: abbr still read");

    /* An overlong line that *is* a --@assign/--@scene line: truncated,
     * but must still not desync the reader -- the line after it parses
     * normally either way. */
    char long_assign[600];
    memset(long_assign, 'y', sizeof long_assign - 1);
    long_assign[sizeof long_assign - 1] = '\0';
    char src2[900];
    snprintf(src2, sizeof src2, "--@assign id=1 junk=\"%s\"\n--@assign id=6 abbr=\"OK2\"\n", long_assign);
    scene_parsed_t out2;
    scene_parse_error_t err2;
    int rc = parse_mem("thing", src2, &out2, &err2);
    CHECK(rc == 0, "overlong directive line: parse failed: %s", err2.msg);
    if (rc == 0) {
        CHECK((out2.scene.assigned[0] & 0x20) != 0, "overlong directive line: id 6 after it still assigned");
        CHECK(!strcmp(out2.scene.abbr[5], "OK2"), "overlong directive line: abbr after it still read");
    }
}

static void test_arena_overflow(void) {
    scene_parsed_t out;
    scene_parse_error_t err;
    /* No single line can overflow the arena on its own (SCENE_LINE_MAX
     * is well under SCENE_PARSE_ARENA_SIZE) -- a script has to work at
     * it across many lines: a long title plus an abbr on every one of
     * the 64 possible --@assign ids. 200-byte title + name ~ 207 bytes,
     * plus 64 * 5 bytes of truncated abbr = 320, comes to ~527: over
     * the 512-byte arena. */
    char src[64 * 32 + 256];
    size_t n = 0;
    n += (size_t)sprintf(src + n, "--@scene title=\"");
    for (int i = 0; i < 200; i++) {
        src[n++] = 'z';
    }
    n += (size_t)sprintf(src + n, "\"\n");
    for (int id = 1; id <= 64; id++) {
        n += (size_t)sprintf(src + n, "--@assign id=%d abbr=\"WXYZ\"\n", id);
    }
    src[n] = '\0';
    int rc = parse_mem("thing", src, &out, &err);
    CHECK(rc == -1, "arena overflow: expected an error, got %d", rc);
    CHECK(strstr(err.msg, "arena") != NULL, "arena overflow: message, got %s", err.msg);
}

static void test_bad_name(void) {
    scene_parsed_t out;
    scene_parse_error_t err;
    CHECK(parse_mem("", "--@assign id=1\n", &out, &err) == -1, "empty name: expected an error");
}

/* ---- comparison against embed_scripts.py --dump-json ---- */

#ifndef OXI_LUA_DIR
#define OXI_LUA_DIR "external/oxi-e16-lua"
#endif
#ifndef SCENE_PARSE_DUMP_JSON
#define SCENE_PARSE_DUMP_JSON "build-host/scene_parse_dump.json"
#endif

static const char *const kScriptFiles[] = {"euclid.lua", "lfo.lua", "chords.lua", "modseq.lua",
                                            "noteseq.lua", "tb3po.lua"};
#define N_SCRIPTS (int)(sizeof kScriptFiles / sizeof kScriptFiles[0])

/* ---- a tiny reader for embed_scripts.py's --dump-json output.
 *
 * Not a general JSON parser: it knows exactly the shape dump_json() in
 * tools/embed_scripts.py writes (a compact array of flat objects with
 * string/int/string-array/int-array/string-map fields) and nothing else.
 * Good enough for a test comparing two things that are both under our
 * control. */

static char *slurp_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

/* Returns a malloc'd copy of the idx-th top-level {...} object in a JSON
 * array, or NULL past the end. Assumes (true here) that no string value
 * contains a brace. */
static char *nth_object(const char *json, int idx) {
    int depth = 0, cur = -1;
    const char *start = NULL;
    for (const char *p = json; *p; p++) {
        if (*p == '{') {
            if (depth == 0) {
                cur++;
                start = p;
            }
            depth++;
        } else if (*p == '}') {
            depth--;
            if (depth == 0 && cur == idx) {
                size_t len = (size_t)(p - start + 1);
                char *out = malloc(len + 1);
                memcpy(out, start, len);
                out[len] = '\0';
                return out;
            }
        }
    }
    return NULL;
}

static int json_str(const char *obj, const char *key, char *out, size_t out_sz) {
    char pat[32];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(obj, pat);
    if (!p) {
        return 0;
    }
    p += strlen(pat);
    const char *q = p;
    while (*q && *q != '"') {
        q++;
    }
    size_t len = (size_t)(q - p);
    if (len >= out_sz) {
        len = out_sz - 1;
    }
    memcpy(out, p, len);
    out[len] = '\0';
    return 1;
}

static int json_int(const char *obj, const char *key, int *out) {
    char pat[32];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(obj, pat);
    if (!p) {
        return 0;
    }
    p += strlen(pat);
    *out = atoi(p);
    return 1;
}

/* Parses a `"key":["a","b",...]` array of strings. */
static int json_str_array(const char *obj, const char *key, char out[][40], int cap, int *n) {
    char pat[32];
    snprintf(pat, sizeof pat, "\"%s\":[", key);
    const char *p = strstr(obj, pat);
    if (!p) {
        return 0;
    }
    p += strlen(pat);
    *n = 0;
    while (*p && *p != ']') {
        if (*p == '"') {
            p++;
            const char *start = p;
            while (*p && *p != '"') {
                p++;
            }
            if (*n < cap) {
                size_t len = (size_t)(p - start);
                if (len >= 40) {
                    len = 39;
                }
                memcpy(out[*n], start, len);
                out[*n][len] = '\0';
                (*n)++;
            }
            if (*p == '"') {
                p++;
            }
        } else {
            p++;
        }
    }
    return 1;
}

/* Parses a `"key":[1,2,3]` array of non-negative ints. */
static int json_int_array(const char *obj, const char *key, int *out, int cap, int *n) {
    char pat[32];
    snprintf(pat, sizeof pat, "\"%s\":[", key);
    const char *p = strstr(obj, pat);
    if (!p) {
        return 0;
    }
    p += strlen(pat);
    *n = 0;
    while (*p && *p != ']') {
        if (*p >= '0' && *p <= '9') {
            int v = atoi(p);
            if (*n < cap) {
                out[(*n)++] = v;
            }
            while (*p >= '0' && *p <= '9') {
                p++;
            }
        } else {
            p++;
        }
    }
    return 1;
}

/* Parses a `"abbr":{"1":"Len1","17":"Len1",...}` map; ids_out[i] and
 * vals_out[i] line up. */
static int json_abbr_map(const char *obj, int *ids_out, char vals_out[][8], int cap, int *n) {
    const char *p = strstr(obj, "\"abbr\":{");
    if (!p) {
        return 0;
    }
    p += strlen("\"abbr\":{");
    *n = 0;
    while (*p && *p != '}') {
        if (*p == '"') {
            p++;
            int id = atoi(p);
            while (*p && *p != '"') {
                p++;
            }
            if (*p == '"') {
                p++;
            }
            if (*p == ':' ) {
                p++;
            }
            if (*p == '"') {
                p++;
                const char *start = p;
                while (*p && *p != '"') {
                    p++;
                }
                if (*n < cap) {
                    size_t len = (size_t)(p - start);
                    if (len >= 8) {
                        len = 7;
                    }
                    memcpy(vals_out[*n], start, len);
                    vals_out[*n][len] = '\0';
                    ids_out[*n] = id;
                    (*n)++;
                }
                if (*p == '"') {
                    p++;
                }
            }
        } else {
            p++;
        }
    }
    return 1;
}

static int int_cmp(const void *a, const void *b) {
    return *(const int *)a - *(const int *)b;
}

static void compare_one_script(const char *dump_json, int idx, const char *file) {
    char name[RT_SCENE_NAME_MAX + 1];
    size_t namelen = strlen(file) - 4; /* strip ".lua" */
    memcpy(name, file, namelen);
    name[namelen] = '\0';

    char path[512];
    snprintf(path, sizeof path, "%s/%s", OXI_LUA_DIR, file);
    scene_parsed_t out;
    scene_parse_error_t err;
    int rc = parse_file(name, path, &out, &err);
    if (rc == -2) {
        CHECK(0, "%s: could not open (run from the repo root, or check OXI_LUA_DIR)", path);
        return;
    }
    CHECK(rc == 0, "%s: C parser failed at line %d: %s", file, err.line, err.msg);
    if (rc != 0) {
        return;
    }

    char *obj = nth_object(dump_json, idx);
    CHECK(obj != NULL, "%s: no JSON object at index %d (check manifest/script order)", file, idx);
    if (!obj) {
        return;
    }

    char py_name[RT_SCENE_NAME_MAX + 1] = "";
    char py_title[256] = "";
    json_str(obj, "name", py_name, sizeof py_name);
    json_str(obj, "title", py_title, sizeof py_title);
    CHECK(!strcmp(py_name, name), "%s: name mismatch: C %s, python %s", file, name, py_name);
    CHECK(!strcmp(py_title, out.scene.title), "%s: title mismatch: C %s, python %s", file,
          out.scene.title, py_title);

    char py_pages[RT_MAX_PAGES][40];
    int py_npages = 0;
    json_str_array(obj, "pages", py_pages, RT_MAX_PAGES, &py_npages);
    CHECK(py_npages == out.scene.pages, "%s: page count mismatch: C %u, python %d", file,
          out.scene.pages, py_npages);
    for (int i = 0; i < py_npages && i < out.scene.pages; i++) {
        CHECK(!strcmp(py_pages[i], out.scene.page_names[i]), "%s: page %d name mismatch: C %s, python %s",
              file, i, out.scene.page_names[i], py_pages[i]);
    }

    int py_pad = -1, py_settings = -1;
    json_int(obj, "pad_pages", &py_pad);
    json_int(obj, "settings_page", &py_settings);
    CHECK(py_pad == out.scene.pad_pages, "%s: pad_pages mismatch: C %u, python %d", file,
          out.scene.pad_pages, py_pad);
    CHECK(py_settings == out.scene.settings_page, "%s: settings_page mismatch: C %u, python %d", file,
          out.scene.settings_page, py_settings);

    int py_ids[64];
    int py_nids = 0;
    json_int_array(obj, "assigned", py_ids, 64, &py_nids);
    qsort(py_ids, (size_t)py_nids, sizeof py_ids[0], int_cmp);
    int c_ids[64];
    int c_nids = 0;
    for (int id = 1; id <= 64; id++) {
        if (out.scene.assigned[(id - 1) / 8] & (1u << ((id - 1) % 8))) {
            c_ids[c_nids++] = id;
        }
    }
    CHECK(c_nids == py_nids, "%s: assigned id count mismatch: C %d, python %d", file, c_nids, py_nids);
    for (int i = 0; i < c_nids && i < py_nids; i++) {
        CHECK(c_ids[i] == py_ids[i], "%s: assigned id %d mismatch: C %d, python %d", file, i, c_ids[i],
              py_ids[i]);
    }

    int abbr_ids[64];
    char abbr_vals[64][8];
    int n_abbr = 0;
    json_abbr_map(obj, abbr_ids, abbr_vals, 64, &n_abbr);
    for (int i = 0; i < n_abbr; i++) {
        int id = abbr_ids[i];
        const char *c_abbr = out.scene.abbr[id - 1];
        CHECK(c_abbr != NULL, "%s: id %d: python has abbr %s, C has none", file, id, abbr_vals[i]);
        if (c_abbr) {
            CHECK(!strcmp(c_abbr, abbr_vals[i]), "%s: id %d: abbr mismatch: C %s, python %s", file, id,
                  c_abbr, abbr_vals[i]);
        }
    }
    /* and the other way: every id the C parser gave an abbr to should be
     * in python's map too. */
    for (int id = 1; id <= 64; id++) {
        if (out.scene.abbr[id - 1]) {
            int found = 0;
            for (int i = 0; i < n_abbr; i++) {
                if (abbr_ids[i] == id) {
                    found = 1;
                }
            }
            CHECK(found, "%s: id %d: C has abbr %s, python has none", file, id, out.scene.abbr[id - 1]);
        }
    }

    free(obj);
}

static void test_against_python(void) {
    char *dump_json = slurp_file(SCENE_PARSE_DUMP_JSON);
    if (!dump_json) {
        CHECK(0, "could not read %s (build it first: "
                 "python3 tools/embed_scripts.py scripts/manifest.txt external/oxi-e16-lua "
                 "/tmp/unused.c --dump-json %s)",
              SCENE_PARSE_DUMP_JSON, SCENE_PARSE_DUMP_JSON);
        return;
    }
    for (int i = 0; i < N_SCRIPTS; i++) {
        compare_one_script(dump_json, i, kScriptFiles[i]);
    }
    free(dump_json);
}

int main(void) {
    test_defaults_pad_only();
    test_defaults_with_settings_id();
    test_scene_line_and_quoted_spaces();
    test_malformed_line();
    test_bad_assign_id();
    test_overlong_name();
    test_duplicate_scene();
    test_overlong_line_is_skipped_not_fatal();
    test_arena_overflow();
    test_bad_name();
    test_against_python();

    if (g_failures == 0) {
        printf("ALL PASSED\n");
        return 0;
    }
    printf("%d FAILURES\n", g_failures);
    return 1;
}
