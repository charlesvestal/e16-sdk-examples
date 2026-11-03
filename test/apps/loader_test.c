/*
 * Host test for src/apps/loader.c, the .e16app loader.
 *
 *   loader_test [path/to/hello.e16app]
 *
 * (a) Synthetic images built here: a valid one loaded against known
 *     32-bit run addresses (every GOT entry and fixup checked after
 *     rebasing, .bss zeroed), the same through app_load() with real
 *     buffers and through a reader that hands out small pieces, and
 *     broken ones: header fields, CRC, sizes, GOT entries and fixups.
 * (b) The real cross-built app (apps/, built with arm-none-eabi-gcc),
 *     if its path is given and the file exists: loaded against
 *     device-like addresses, then every GOT entry and fixup must point
 *     into the loaded text or data and the entry must be Thumb. (Running
 *     it needs the device.) Without it, (b) is skipped with a note.
 *
 * Prints "ALL PASSED" and exits 0 on success; exits nonzero otherwise.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "loader.h"

static int g_failures;
#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            g_failures++;                                                   \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);            \
            fprintf(stderr, __VA_ARGS__);                                   \
            fprintf(stderr, "\n");                                          \
        }                                                                   \
    } while (0)
#define CHECK_ERR(got, want, what)                                          \
    CHECK((got) == (want), "%s: got %d (%s), want %d (%s)", what, got,     \
          app_strerror(got), want, app_strerror(want))

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

/* ---- synthetic images ---- */

#define T_LINK 0x00000000u
#define D_LINK 0x80000000u
#define T_ADDR 0x20004000u  /* run addresses the stream tests rebase against */
#define D_ADDR 0x10000000u

/* Everything a test may want to break, before the image is serialized. */
typedef struct {
    uint8_t magic[4];
    uint32_t abi, header_size, flags;
    uint32_t text_size, data_size, bss_size, got_offset, got_count, fixup_count;
    uint32_t entry_offset, text_link, data_link;
    char name[E16APP_NAME_MAX];
    uint8_t text[256];
    uint8_t data[256];
    uint32_t fixups[16];
    int bad_crc;
} spec_t;

/*
 * The reference image: 64 bytes of text, 48 of data (a 5-entry GOT at
 * offset 8, then initialized words, two of them pointers), 16 of bss.
 */
static void spec_default(spec_t *s)
{
    memset(s, 0, sizeof *s);
    memcpy(s->magic, E16APP_MAGIC, 4);
    s->abi = E16APP_ABI_VERSION;
    s->header_size = E16APP_HEADER_SIZE;
    s->text_size = 64;
    s->data_size = 48;
    s->bss_size = 16;
    s->got_offset = 8;
    s->got_count = 5;
    s->entry_offset = 4;
    s->text_link = T_LINK;
    s->data_link = D_LINK;
    strcpy(s->name, "synthetic");
    for (int i = 0; i < 64; i++)
        s->text[i] = (uint8_t)(i * 7 + 1);
    for (int i = 0; i < 48; i++)
        s->data[i] = (uint8_t)(0xC0 + i);
    /* GOT, link-time values */
    wr32(s->data + 8, T_LINK + 0x10 + 1);       /* a Thumb function */
    wr32(s->data + 12, T_LINK + 0x30);          /* rodata */
    wr32(s->data + 16, D_LINK + 0x28);          /* a .data variable */
    wr32(s->data + 20, D_LINK + 48 + 8);        /* a .bss variable */
    wr32(s->data + 24, D_LINK + 48 + 16);       /* one past the end of bss */
    /* .data words: two pointers (fixups), one plain number */
    wr32(s->data + 32, T_LINK + 0x20 + 1);      /* int (*fp)(void) = f; */
    wr32(s->data + 36, D_LINK + 0x2C);          /* int *p = &x; */
    wr32(s->data + 40, 0x12345678u);            /* not a pointer: untouched */
    s->fixups[0] = 32;
    s->fixups[1] = 36;
    s->fixup_count = 2;
}

static size_t spec_build(const spec_t *s, uint8_t *out)
{
    uint8_t *h = out;
    memset(h, 0, E16APP_HEADER_SIZE);
    memcpy(h, s->magic, 4);
    wr16(h + 4, s->abi);
    wr16(h + 6, s->header_size);
    wr32(h + 8, s->flags);
    wr32(h + 12, s->text_size);
    wr32(h + 16, s->data_size);
    wr32(h + 20, s->bss_size);
    wr32(h + 24, s->got_offset);
    wr32(h + 28, s->got_count);
    wr32(h + 32, s->fixup_count);
    wr32(h + 36, s->entry_offset);
    wr32(h + 40, s->text_link);
    wr32(h + 44, s->data_link);
    memcpy(h + 52, s->name, E16APP_NAME_MAX);
    size_t n = E16APP_HEADER_SIZE;
    uint32_t tsz = s->text_size <= sizeof s->text ? s->text_size : 0;
    uint32_t dsz = s->data_size <= sizeof s->data ? s->data_size : 0;
    uint32_t fcnt = s->fixup_count <= 16 ? s->fixup_count : 0;
    memcpy(out + n, s->text, tsz); n += tsz;
    memcpy(out + n, s->data, dsz); n += dsz;
    for (uint32_t i = 0; i < fcnt; i++, n += 4)
        wr32(out + n, s->fixups[i]);
    uint32_t crc = app_crc32(0, out, n);  /* crc field is still 0 */
    wr32(h + 48, s->bad_crc ? crc ^ 1u : crc);
    return n;
}

/* A reader over a buffer that hands out at most `chunk` bytes per read
 * call (it loops, as a FatFs wrapper would), counting calls. */
typedef struct {
    const uint8_t *p;
    size_t left;
    size_t chunk;
    int calls;
} reader_t;

static int buf_read(void *ud, void *dst, size_t n)
{
    reader_t *r = ud;
    uint8_t *d = dst;
    r->calls++;
    while (n) {
        if (r->left == 0)
            return -1;
        size_t k = n < r->chunk ? n : r->chunk;
        if (k > r->left)
            k = r->left;
        memcpy(d, r->p, k);
        r->p += k; r->left -= k; d += k; n -= k;
    }
    return 0;
}

/* Big enough for any real app (the device's caps are ~100 KB / 64 KB). */
static uint8_t g_text[131072] __attribute__((aligned(8)));
static uint8_t g_data[65536] __attribute__((aligned(8)));

static int load_at(const uint8_t *img, size_t len, size_t text_cap, size_t data_cap,
                   uint32_t taddr, uint32_t daddr, app_loaded_t *out)
{
    reader_t r = { img, len, 7, 0 };
    app_mem_t mem = { g_text, text_cap, taddr, g_data, data_cap, daddr };
    memset(g_text, 0xEE, sizeof g_text);
    memset(g_data, 0xAA, sizeof g_data);
    return app_load_stream(buf_read, &r, &mem, out);
}

static int load_spec(const spec_t *s, app_loaded_t *out)
{
    static uint8_t img[1024];
    size_t n = spec_build(s, img);
    return load_at(img, n, sizeof g_text, sizeof g_data, T_ADDR, D_ADDR, out);
}

static void test_crc(void)
{
    CHECK(app_crc32(0, "123456789", 9) == 0xCBF43926u, "CRC-32 check value");
    uint32_t c = app_crc32(0, "1234", 4);
    CHECK(app_crc32(c, "56789", 5) == 0xCBF43926u, "CRC-32 is incremental");
}

static void test_valid(void)
{
    spec_t s;
    spec_default(&s);
    app_loaded_t a;
    int err = load_spec(&s, &a);
    CHECK_ERR(err, APP_OK, "valid image");
    if (err != APP_OK)
        return;

    CHECK(memcmp(g_text, s.text, 64) == 0, "text copied");
    CHECK(g_text[64] == 0xEE, "nothing written past text");
    CHECK(a.entry == ((T_ADDR + 4u) | 1u), "entry 0x%08x", a.entry);
    CHECK(a.r9 == D_ADDR + 8u, "r9 0x%08x", a.r9);
    CHECK(a.text_addr == T_ADDR && a.text_size == 64, "text range");
    CHECK(a.data_addr == D_ADDR && a.data_size == 48 && a.bss_size == 16, "data range");
    CHECK(a.got_count == 5 && a.fixup_count == 2, "counts");
    CHECK(strcmp(a.name, "synthetic") == 0, "name %s", a.name);

    /* GOT rebased by range. */
    CHECK(rd32(g_data + 8) == T_ADDR + 0x11, "GOT[0] text fn 0x%08x", rd32(g_data + 8));
    CHECK(rd32(g_data + 12) == T_ADDR + 0x30, "GOT[1] rodata");
    CHECK(rd32(g_data + 16) == D_ADDR + 0x28, "GOT[2] data");
    CHECK(rd32(g_data + 20) == D_ADDR + 56, "GOT[3] bss");
    CHECK(rd32(g_data + 24) == D_ADDR + 64, "GOT[4] end of bss");
    /* Fixups rebased; everything else verbatim. */
    CHECK(rd32(g_data + 32) == T_ADDR + 0x21, "fixup fn ptr 0x%08x", rd32(g_data + 32));
    CHECK(rd32(g_data + 36) == D_ADDR + 0x2C, "fixup data ptr");
    CHECK(rd32(g_data + 40) == 0x12345678u, "plain word untouched");
    CHECK(memcmp(g_data, s.data, 8) == 0 && memcmp(g_data + 28, s.data + 28, 4) == 0 &&
          memcmp(g_data + 44, s.data + 44, 4) == 0, "non-pointer data copied verbatim");
    /* bss zeroed, nothing past it touched. */
    int bss_zero = 1;
    for (int i = 48; i < 64; i++)
        bss_zero &= g_data[i] == 0;
    CHECK(bss_zero, "bss zeroed");
    CHECK(g_data[64] == 0xAA, "nothing written past bss");

    /* No GOT, no fixups, no bss, no data: still loads. */
    spec_default(&s);
    s.data_size = 0; s.got_offset = 0; s.got_count = 0; s.fixup_count = 0; s.bss_size = 0;
    CHECK_ERR(load_spec(&s, &a), APP_OK, "text-only image");
    CHECK(a.r9 == D_ADDR, "r9 with an empty GOT");

    /* Exact fit in the caps. */
    spec_default(&s);
    static uint8_t img[1024];
    size_t n = spec_build(&s, img);
    CHECK_ERR(load_at(img, n, 64, 64, T_ADDR, D_ADDR, &a), APP_OK, "exact-fit caps");
}

static void test_app_load_real_pointers(void)
{
    spec_t s;
    spec_default(&s);
    static uint8_t img[1024];
    size_t n = spec_build(&s, img);
    app_loaded_t a;
    int err = app_load(img, n, g_text, sizeof g_text, g_data, sizeof g_data, &a);
    CHECK_ERR(err, APP_OK, "app_load");
    uint32_t t = (uint32_t)(uintptr_t)g_text, d = (uint32_t)(uintptr_t)g_data;
    CHECK(a.text_addr == t && a.data_addr == d, "app_load uses the buffers' addresses");
    CHECK(rd32(g_data + 8) == t + 0x11 && rd32(g_data + 20) == d + 56, "GOT vs real buffers");
    CHECK(rd32(g_data + 36) == d + 0x2C, "fixup vs real buffers");

    /* A reader handing out one byte at a time gives the same result. */
    uint8_t text1[64], data1[64];
    memcpy(text1, g_text, 64); memcpy(data1, g_data, 64);
    reader_t r = { img, n, 1, 0 };
    app_mem_t mem = { g_text, sizeof g_text, t, g_data, sizeof g_data, d };
    memset(g_data, 0x55, sizeof g_data);
    err = app_load_stream(buf_read, &r, &mem, &a);
    CHECK_ERR(err, APP_OK, "byte-at-a-time stream");
    CHECK(memcmp(text1, g_text, 64) == 0 && memcmp(data1, g_data, 64) == 0,
          "stream and in-memory loads agree");
    CHECK(r.calls == 4, "stream reads header, text, data, fixups: %d calls", r.calls);

    CHECK_ERR(app_load(img, n - 1, g_text, sizeof g_text, g_data, sizeof g_data, &a),
              APP_ERR_TRUNCATED, "truncated by 1");
    CHECK_ERR(app_load(img, 40, g_text, sizeof g_text, g_data, sizeof g_data, &a),
              APP_ERR_TRUNCATED, "shorter than a header");
    CHECK_ERR(app_load(img, n + 1, g_text, sizeof g_text, g_data, sizeof g_data, &a),
              APP_ERR_LENGTH, "trailing byte");
    CHECK_ERR(app_load(NULL, n, g_text, sizeof g_text, g_data, sizeof g_data, &a),
              APP_ERR_ARGS, "NULL image");
    CHECK_ERR(app_load(img, n, g_text + 4, sizeof g_text - 4, g_data, sizeof g_data, &a),
              APP_ERR_ALIGN, "misaligned text buffer");
}

static void test_header(void)
{
    spec_t s;
    app_loaded_t a;
#define BAD(field_stmt, want, what) \
    do { spec_default(&s); field_stmt; CHECK_ERR(load_spec(&s, &a), want, what); } while (0)

    BAD(s.magic[3] = 'B', APP_ERR_MAGIC, "magic");
    BAD(s.abi = 2, APP_ERR_ABI, "ABI version 2");
    BAD(s.abi = 0, APP_ERR_ABI, "ABI version 0");
    BAD(s.header_size = 80, APP_ERR_HEADER, "header_size");
    BAD(s.flags = 1, APP_ERR_HEADER, "unknown flag");
    BAD(s.name[0] = '\0', APP_ERR_HEADER, "empty name");
    BAD(memset(s.name, 'x', E16APP_NAME_MAX), APP_ERR_HEADER, "unterminated name");
    BAD(s.text_size = 0, APP_ERR_HEADER, "no text");
    BAD(s.text_size = 62, APP_ERR_HEADER, "text not a word multiple");
    BAD(s.data_size = 46, APP_ERR_HEADER, "data not a word multiple");
    BAD(s.bss_size = 3, APP_ERR_HEADER, "bss not a word multiple");
    BAD(s.got_offset = 6, APP_ERR_HEADER, "GOT not aligned");
    BAD(s.got_count = 11, APP_ERR_HEADER, "GOT past data");
    BAD((s.got_offset = 0xFFFFFFFCu, s.got_count = 2), APP_ERR_HEADER, "GOT offset wraps");
    BAD(s.got_count = 0x40000001u, APP_ERR_HEADER, "GOT count overflows");
    BAD(s.fixup_count = 13, APP_ERR_HEADER, "more fixups than data words");
    BAD(s.entry_offset = 64, APP_ERR_HEADER, "entry past text");
    BAD(s.entry_offset = 5, APP_ERR_HEADER, "entry odd (Thumb bit belongs to the loader)");
    BAD(s.data_link = T_LINK + 32, APP_ERR_HEADER, "data link range inside text");
    BAD(s.data_link = T_LINK + 64, APP_ERR_HEADER, "link ranges touch");
    BAD(s.text_link = 0xFFFFFFE0u, APP_ERR_HEADER, "text link range wraps");
    BAD(s.bss_size = 0xFFFFFF00u, APP_ERR_HEADER, "data link range wraps");
#undef BAD

    uint8_t hb[E16APP_HEADER_SIZE];
    e16app_header_t h;
    spec_default(&s);
    static uint8_t img[1024];
    spec_build(&s, img);
    memcpy(hb, img, sizeof hb);
    CHECK_ERR(app_parse_header(hb, sizeof hb, &h), APP_OK, "parse header");
    CHECK(h.text_size == 64 && h.got_offset == 8 && strcmp(h.name, "synthetic") == 0,
          "parsed fields");
    CHECK_ERR(app_parse_header(hb, sizeof hb - 1, &h), APP_ERR_TRUNCATED, "short header");
}

static void test_crc_and_size(void)
{
    spec_t s;
    app_loaded_t a;
    static uint8_t img[1024];

    spec_default(&s);
    s.bad_crc = 1;
    CHECK_ERR(load_spec(&s, &a), APP_ERR_CRC, "wrong CRC field");

    /* One flipped bit anywhere after the header, or in a header field the
     * validation can't catch (the name), fails the CRC. */
    spec_default(&s);
    size_t n = spec_build(&s, img);
    size_t spots[] = { 56, E16APP_HEADER_SIZE + 3, E16APP_HEADER_SIZE + 64 + 40, n - 1 };
    for (size_t i = 0; i < sizeof spots / sizeof spots[0]; i++) {
        img[spots[i]] ^= 0x10;
        CHECK_ERR(load_at(img, n, sizeof g_text, sizeof g_data, T_ADDR, D_ADDR, &a),
                  APP_ERR_CRC, "flipped bit");
        img[spots[i]] ^= 0x10;
    }
    CHECK_ERR(load_at(img, n, sizeof g_text, sizeof g_data, T_ADDR, D_ADDR, &a), APP_OK,
              "restored image");

    /* Oversize against the caps. */
    CHECK_ERR(load_at(img, n, 60, sizeof g_data, T_ADDR, D_ADDR, &a), APP_ERR_TEXT_SIZE,
              "text cap");
    CHECK_ERR(load_at(img, n, sizeof g_text, 63, T_ADDR, D_ADDR, &a), APP_ERR_DATA_SIZE,
              "data + bss cap");
    spec_default(&s);
    s.bss_size = 0x7FFFFF00u;  /* huge bss: fine as a header, too big to load */
    CHECK_ERR(load_spec(&s, &a), APP_ERR_DATA_SIZE, "huge bss");

    /* Truncated streams. */
    CHECK_ERR(load_at(img, n - 1, sizeof g_text, sizeof g_data, T_ADDR, D_ADDR, &a),
              APP_ERR_TRUNCATED, "stream short in fixups");
    CHECK_ERR(load_at(img, E16APP_HEADER_SIZE + 10, sizeof g_text, sizeof g_data, T_ADDR,
                      D_ADDR, &a), APP_ERR_TRUNCATED, "stream short in text");
    CHECK_ERR(load_at(img, 10, sizeof g_text, sizeof g_data, T_ADDR, D_ADDR, &a),
              APP_ERR_TRUNCATED, "stream short in header");

    /* Run addresses must be 8-aligned. */
    CHECK_ERR(load_at(img, n, sizeof g_text, sizeof g_data, T_ADDR + 4, D_ADDR, &a),
              APP_ERR_ALIGN, "text run address");
    CHECK_ERR(load_at(img, n, sizeof g_text, sizeof g_data, T_ADDR, D_ADDR + 2, &a),
              APP_ERR_ALIGN, "data run address");
    CHECK_ERR(load_at(img, n, sizeof g_text, sizeof g_data, 0xFFFFFFE0u, D_ADDR, &a),
              APP_ERR_ARGS, "text run range wraps");
}

static void test_reloc(void)
{
    spec_t s;
    app_loaded_t a;
#define BAD(stmt, what) \
    do { spec_default(&s); stmt; CHECK_ERR(load_spec(&s, &a), APP_ERR_RELOC, what); } while (0)

    BAD(wr32(s.data + 16, 0x40000000u), "GOT entry outside the app");
    BAD(wr32(s.data + 16, T_LINK + 65), "GOT entry just past text");
    BAD(wr32(s.data + 16, D_LINK + 65), "GOT entry just past bss");
    BAD(wr32(s.data + 16, D_LINK - 4), "GOT entry just before data");
    BAD(wr32(s.data + 36, 0x12345678u), "fixup word not an app address");
    BAD(s.fixups[1] = 34, "fixup offset unaligned");
    BAD(s.fixups[1] = 48, "fixup offset past data");
    BAD(s.fixups[1] = 0xFFFFFFFCu, "fixup offset wraps");
    BAD((s.fixups[0] = 36, s.fixups[1] = 32), "fixups out of order");
    BAD(s.fixups[1] = 32, "duplicate fixup (would rebase twice)");
    BAD((s.fixups[0] = 12, wr32(s.data + 12, T_LINK)), "fixup inside the GOT");
#undef BAD

    /* Both ends of each range are inclusive. */
    spec_default(&s);
    wr32(s.data + 12, T_LINK + 64);
    wr32(s.data + 16, D_LINK);
    CHECK_ERR(load_spec(&s, &a), APP_OK, "range ends");
    CHECK(rd32(g_data + 12) == T_ADDR + 64 && rd32(g_data + 16) == D_ADDR, "range ends rebased");

    /* Link bases other than app.ld's work the same. */
    spec_default(&s);
    s.text_link = 0x1000;
    s.data_link = 0x9000;
    wr32(s.data + 8, 0x1000 + 0x11);
    wr32(s.data + 12, 0x1000 + 0x30);
    wr32(s.data + 16, 0x9000 + 0x28);
    wr32(s.data + 20, 0x9000 + 56);
    wr32(s.data + 24, 0x9000 + 64);
    wr32(s.data + 32, 0x1000 + 0x21);
    wr32(s.data + 36, 0x9000 + 0x2C);
    CHECK_ERR(load_spec(&s, &a), APP_OK, "other link bases");
    CHECK(rd32(g_data + 8) == T_ADDR + 0x11 && rd32(g_data + 36) == D_ADDR + 0x2C,
          "other link bases rebased");
}

static void test_strerror(void)
{
    for (int e = 0; e >= -11; e--)
        CHECK(app_strerror(e) != NULL && strcmp(app_strerror(e), "unknown error") != 0,
              "strerror(%d)", e);
    CHECK(strcmp(app_strerror(-99), "unknown error") == 0, "strerror(unknown)");
}

/* ---- (b) the real app ---- */

typedef struct {
    const uint8_t *p;
    size_t left;
    size_t max_piece;
} chunk_reader_t;

static int chunk_read(void *ud, void *buf, size_t n)
{
    chunk_reader_t *c = ud;
    if (n > c->left)
        return -1;
    if (n > c->max_piece)
        c->max_piece = n;
    memcpy(buf, c->p, n);
    c->p += n;
    c->left -= n;
    return 0;
}

static int verify_image(const uint8_t *img, size_t n, size_t text_cap, size_t data_cap, e16app_header_t *h,
                        size_t *max_piece)
{
    chunk_reader_t c = { img, n, 0 };
    int err = app_verify_stream(chunk_read, &c, n, text_cap, data_cap, h);
    if (max_piece)
        *max_piece = c.max_piece;
    return err;
}

/* app_verify_stream() (the menu's check before resetting into app mode)
 * on a real image: accepted as is, refused when corrupt, cut short,
 * padded or too big for the caps. */
static void test_verify_real(const uint8_t *img, size_t n)
{
    static uint8_t bad[sizeof g_text + sizeof g_data + 4096];
    e16app_header_t h;
    size_t piece = 0;
    CHECK_ERR(verify_image(img, n, sizeof g_text, sizeof g_data, &h, &piece), APP_OK, "verify: real image");
    CHECK(piece <= 256, "verify reads in small pieces (max %zu)", piece);
    CHECK((uint64_t)n == app_image_size(&h), "app_image_size matches the file");
    if (n + 4 > sizeof bad)
        return;

    /* A flipped byte anywhere past the header: the CRC catches it. */
    size_t spots[] = { E16APP_HEADER_SIZE, E16APP_HEADER_SIZE + h.text_size / 2, n - 1 };
    for (size_t i = 0; i < sizeof spots / sizeof spots[0]; i++) {
        memcpy(bad, img, n);
        bad[spots[i]] ^= 0x01;
        char what[48];
        snprintf(what, sizeof what, "verify: flipped byte at %zu", spots[i]);
        CHECK_ERR(verify_image(bad, n, sizeof g_text, sizeof g_data, &h, NULL), APP_ERR_CRC, what);
    }
    /* In the header's name (covered by the CRC too). */
    memcpy(bad, img, n);
    bad[52 + 1] ^= 0x01;
    CHECK_ERR(verify_image(bad, n, sizeof g_text, sizeof g_data, &h, NULL), APP_ERR_CRC, "verify: header byte");

    memcpy(bad, img, n);
    CHECK_ERR(verify_image(bad, n - 4, sizeof g_text, sizeof g_data, &h, NULL), APP_ERR_TRUNCATED,
              "verify: cut short");
    memset(bad + n, 0, 4);
    CHECK_ERR(verify_image(bad, n + 4, sizeof g_text, sizeof g_data, &h, NULL), APP_ERR_LENGTH,
              "verify: trailing bytes");
    CHECK_ERR(verify_image(img, n, 64, sizeof g_data, &h, NULL), APP_ERR_TEXT_SIZE, "verify: text cap");
    CHECK_ERR(verify_image(img, n, sizeof g_text, 8, &h, NULL), APP_ERR_DATA_SIZE, "verify: data cap");
    CHECK_ERR(verify_image(img, 10, sizeof g_text, sizeof g_data, &h, NULL), APP_ERR_TRUNCATED,
              "verify: no header");
}

static int in_range(uint32_t v, uint32_t base, uint32_t size)
{
    return v >= base && v - base <= size;
}

static void test_real_app(const char *path)
{
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) {
        printf("note: %s not found; skipping the real-app check "
               "(build apps/ with arm-none-eabi-gcc)\n", path ? path : "no app path given,");
        return;
    }
    static uint8_t img[sizeof g_text + sizeof g_data + 4096];
    size_t n = fread(img, 1, sizeof img, f);
    fclose(f);
    test_verify_real(img, n);

    /* Device-like addresses: text in SRAM, data in CCM. */
    const uint32_t taddr = 0x20008000u, daddr = 0x10000000u;
    e16app_header_t h;
    int err = app_parse_header(img, n, &h);
    CHECK_ERR(err, APP_OK, "real app header");
    if (err != APP_OK)
        return;
    app_loaded_t a;
    err = load_at(img, n, sizeof g_text, sizeof g_data, taddr, daddr, &a);
    CHECK_ERR(err, APP_OK, "real app load");
    if (err != APP_OK)
        return;
    uint32_t dtot = a.data_size + a.bss_size;
    printf("real app '%s': text %u data %u bss %u GOT %u fixups %u (file %zu bytes)\n",
           a.name, a.text_size, a.data_size, a.bss_size, a.got_count, a.fixup_count, n);

    CHECK(a.entry & 1u, "entry is Thumb");
    CHECK(in_range(a.entry & ~1u, taddr, a.text_size - 1), "entry inside text");
    CHECK(a.r9 == daddr + h.got_offset, "r9 = GOT");
    CHECK(memcmp(g_text, img + E16APP_HEADER_SIZE, a.text_size) == 0, "text copied verbatim");

    int thumb_fns = 0, to_text = 0, to_data = 0;
    for (uint32_t i = 0; i < a.got_count; i++) {
        uint32_t v = rd32(g_data + h.got_offset + 4 * i);
        int t = in_range(v, taddr, a.text_size), d = in_range(v, daddr, dtot);
        CHECK(t || d, "GOT[%u] = 0x%08x outside the loaded app", i, v);
        to_text += t;
        to_data += d;
        thumb_fns += t && (v & 1u);
    }
    const uint8_t *fx = img + E16APP_HEADER_SIZE + a.text_size + a.data_size;
    for (uint32_t i = 0; i < a.fixup_count; i++) {
        uint32_t off = rd32(fx + 4 * i);
        uint32_t v = rd32(g_data + off);
        CHECK(in_range(v, taddr, a.text_size) || in_range(v, daddr, dtot),
              "fixup at data+%u = 0x%08x outside the loaded app", off, v);
    }
    printf("real app GOT: %d -> text (%d Thumb functions), %d -> data\n",
           to_text, thumb_fns, to_data);
    /* hello.c takes function addresses, a rodata table and data/bss
     * globals: each kind must be there. */
    if (strcmp(a.name, "hello") == 0) {
        CHECK(thumb_fns >= 2, "hello: function pointers in the GOT");
        CHECK(to_data >= 2, "hello: data/bss globals in the GOT");
        CHECK(a.fixup_count >= 6, "hello: words[] and ops[] fixups");
        CHECK(a.bss_size > 0, "hello: counts[] in bss");
    }
}

int main(int argc, char **argv)
{
    test_crc();
    test_valid();
    test_app_load_real_pointers();
    test_header();
    test_crc_and_size();
    test_reloc();
    test_strerror();
    if (argc < 2)
        test_real_app(NULL);
    for (int i = 1; i < argc; i++)
        test_real_app(argv[i]);

    if (g_failures == 0) {
        printf("ALL PASSED\n");
        return 0;
    }
    printf("%d FAILURES\n", g_failures);
    return 1;
}
