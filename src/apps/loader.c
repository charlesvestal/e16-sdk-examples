#include "loader.h"

#include <string.h>

const char *app_strerror(int err)
{
    switch (err) {
    case APP_OK:            return "ok";
    case APP_ERR_ARGS:      return "bad arguments";
    case APP_ERR_TRUNCATED: return "image truncated";
    case APP_ERR_MAGIC:     return "not an .e16app";
    case APP_ERR_ABI:       return "wrong ABI version";
    case APP_ERR_HEADER:    return "bad header";
    case APP_ERR_TEXT_SIZE: return "code too big";
    case APP_ERR_DATA_SIZE: return "data too big";
    case APP_ERR_LENGTH:    return "image size mismatch";
    case APP_ERR_CRC:       return "checksum mismatch";
    case APP_ERR_RELOC:     return "bad relocation";
    case APP_ERR_ALIGN:     return "misaligned load address";
    default:                return "unknown error";
    }
}

/* ---- CRC-32, a nibble at a time (64-byte table) ---- */

static const uint32_t crc_nibble[16] = {
    0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu,
    0x76dc4190u, 0x6b6b51f4u, 0x4db26158u, 0x5005713cu,
    0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu,
    0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu,
};

uint32_t app_crc32(uint32_t crc, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ crc_nibble[crc & 15];
        crc = (crc >> 4) ^ crc_nibble[crc & 15];
    }
    return ~crc;
}

/* ---- header ---- */

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* [base, base + size] doesn't wrap past 2^32 (end inclusive). */
static int range_ok(uint32_t base, uint64_t size)
{
    return (uint64_t)base + size <= 0xffffffffull;
}

int app_parse_header(const uint8_t *b, size_t len, e16app_header_t *h)
{
    if (!b || !h)
        return APP_ERR_ARGS;
    if (len < E16APP_HEADER_SIZE)
        return APP_ERR_TRUNCATED;
    if (memcmp(b, E16APP_MAGIC, E16APP_MAGIC_LEN) != 0)
        return APP_ERR_MAGIC;

    memcpy(h->magic, b, E16APP_MAGIC_LEN);
    h->abi_version    = (uint16_t)rd16(b + 4);
    h->header_size    = (uint16_t)rd16(b + 6);
    h->flags          = rd32(b + 8);
    h->text_size      = rd32(b + 12);
    h->data_size      = rd32(b + 16);
    h->bss_size       = rd32(b + 20);
    h->got_offset     = rd32(b + 24);
    h->got_count      = rd32(b + 28);
    h->fixup_count    = rd32(b + 32);
    h->entry_offset   = rd32(b + 36);
    h->text_link_base = rd32(b + 40);
    h->data_link_base = rd32(b + 44);
    h->crc32          = rd32(b + 48);
    memcpy(h->name, b + 52, E16APP_NAME_MAX);

    if (h->abi_version != E16APP_ABI_VERSION)
        return APP_ERR_ABI;
    if (h->header_size != E16APP_HEADER_SIZE)
        return APP_ERR_HEADER;
    if (h->flags & ~E16APP_FLAGS_KNOWN)
        return APP_ERR_HEADER;

    /* Name: non-empty and NUL-terminated within the field. */
    if (h->name[0] == '\0' || memchr(h->name, '\0', E16APP_NAME_MAX) == NULL)
        return APP_ERR_HEADER;

    /* Sizes and offsets: word multiples, inside their sections. */
    if (h->text_size == 0 || (h->text_size | h->data_size | h->bss_size | h->got_offset) & 3u)
        return APP_ERR_HEADER;
    if (h->entry_offset >= h->text_size || (h->entry_offset & 1u))
        return APP_ERR_HEADER;
    if ((uint64_t)h->got_offset + (uint64_t)h->got_count * 4u > h->data_size)
        return APP_ERR_HEADER;
    if ((uint64_t)h->fixup_count * 4u > h->data_size)
        return APP_ERR_HEADER;

    /* Link-time ranges: no wrap, no overlap (otherwise a word's range
     * wouldn't say which section it belongs to). */
    uint64_t data_total = (uint64_t)h->data_size + h->bss_size;
    if (!range_ok(h->text_link_base, h->text_size) || !range_ok(h->data_link_base, data_total))
        return APP_ERR_HEADER;
    if ((uint64_t)h->text_link_base <= (uint64_t)h->data_link_base + data_total &&
        (uint64_t)h->data_link_base <= (uint64_t)h->text_link_base + h->text_size)
        return APP_ERR_HEADER;
    return APP_OK;
}

/* ---- relocation ---- */

typedef struct {
    const e16app_header_t *h;
    uint32_t text_addr, data_addr;
} rebase_t;

/* Rebase one link-time address; APP_ERR_RELOC if it isn't inside the app. */
static int rebase(const rebase_t *r, uint32_t v, uint32_t *out)
{
    const e16app_header_t *h = r->h;
    if (v >= h->text_link_base && v - h->text_link_base <= h->text_size) {
        *out = r->text_addr + (v - h->text_link_base);
        return APP_OK;
    }
    if (v >= h->data_link_base &&
        (uint64_t)(v - h->data_link_base) <= (uint64_t)h->data_size + h->bss_size) {
        *out = r->data_addr + (v - h->data_link_base);
        return APP_OK;
    }
    return APP_ERR_RELOC;
}

static int rebase_word(const rebase_t *r, uint8_t *p)
{
    uint32_t v;
    int err = rebase(r, rd32(p), &v);
    if (err == APP_OK)
        wr32(p, v);
    return err;
}

/* ---- loading ---- */

#define FIXUP_CHUNK 32  /* fixups read per reader call (128 bytes of stack) */

int app_load_stream(app_read_fn rd, void *ud, const app_mem_t *mem, app_loaded_t *out)
{
    if (!rd || !mem || !out || !mem->text || !mem->data)
        return APP_ERR_ARGS;
    if ((mem->text_addr | mem->data_addr) & 7u)
        return APP_ERR_ALIGN;

    uint8_t hb[E16APP_HEADER_SIZE];
    if (rd(ud, hb, sizeof hb) != 0)
        return APP_ERR_TRUNCATED;
    e16app_header_t h;
    int err = app_parse_header(hb, sizeof hb, &h);
    if (err != APP_OK)
        return err;

    if (h.text_size > mem->text_cap)
        return APP_ERR_TEXT_SIZE;
    if ((uint64_t)h.data_size + h.bss_size > mem->data_cap)
        return APP_ERR_DATA_SIZE;
    /* The run ranges mustn't wrap either. */
    if (!range_ok(mem->text_addr, h.text_size) ||
        !range_ok(mem->data_addr, (uint64_t)h.data_size + h.bss_size))
        return APP_ERR_ARGS;

    /* CRC covers the header with its crc32 field zeroed. */
    wr32(hb + 48, 0);
    uint32_t crc = app_crc32(0, hb, sizeof hb);

    if (rd(ud, mem->text, h.text_size) != 0)
        return APP_ERR_TRUNCATED;
    crc = app_crc32(crc, mem->text, h.text_size);
    if (rd(ud, mem->data, h.data_size) != 0)
        return APP_ERR_TRUNCATED;
    crc = app_crc32(crc, mem->data, h.data_size);

    rebase_t r = { &h, mem->text_addr, mem->data_addr };

    /*
     * Fixups are applied as they're read (there's nowhere to keep them),
     * which is fine: nothing runs unless the CRC below matches. Offsets
     * must be strictly increasing (so none is rebased twice), aligned,
     * inside .data and outside the GOT (the GOT is rebased separately).
     */
    uint32_t got_lo = h.got_offset, got_hi = h.got_offset + h.got_count * 4u;
    uint64_t next_min = 0;
    uint32_t left = h.fixup_count;
    int reloc_err = APP_OK;
    while (left) {
        uint8_t buf[FIXUP_CHUNK * 4];
        uint32_t n = left < FIXUP_CHUNK ? left : FIXUP_CHUNK;
        if (rd(ud, buf, n * 4u) != 0)
            return APP_ERR_TRUNCATED;
        crc = app_crc32(crc, buf, n * 4u);
        for (uint32_t i = 0; i < n && reloc_err == APP_OK; i++) {
            uint32_t off = rd32(buf + 4 * i);
            if (off < next_min || (off & 3u) || (uint64_t)off + 4u > h.data_size ||
                (off >= got_lo && off < got_hi)) {
                reloc_err = APP_ERR_RELOC;
                break;
            }
            next_min = (uint64_t)off + 4u;
            reloc_err = rebase_word(&r, mem->data + off);
        }
        left -= n;
    }

    if (crc != h.crc32)
        return APP_ERR_CRC;
    if (reloc_err != APP_OK)
        return reloc_err;

    for (uint32_t i = 0; i < h.got_count; i++) {
        err = rebase_word(&r, mem->data + h.got_offset + 4u * i);
        if (err != APP_OK)
            return err;
    }

    memset(mem->data + h.data_size, 0, h.bss_size);

    memset(out, 0, sizeof *out);
    out->entry = (mem->text_addr + h.entry_offset) | 1u;
    out->r9 = mem->data_addr + h.got_offset;
    out->text_addr = mem->text_addr;
    out->text_size = h.text_size;
    out->data_addr = mem->data_addr;
    out->data_size = h.data_size;
    out->bss_size = h.bss_size;
    out->got_count = h.got_count;
    out->fixup_count = h.fixup_count;
    memcpy(out->name, h.name, E16APP_NAME_MAX);
    return APP_OK;
}

uint64_t app_image_size(const e16app_header_t *h)
{
    return (uint64_t)E16APP_HEADER_SIZE + h->text_size + h->data_size + (uint64_t)h->fixup_count * 4u;
}

#define VERIFY_CHUNK 256  /* bytes read per reader call while verifying */

int app_verify_stream(app_read_fn rd, void *ud, uint64_t file_len, size_t text_cap, size_t data_cap,
                      e16app_header_t *out)
{
    if (!rd || !out)
        return APP_ERR_ARGS;
    uint8_t buf[VERIFY_CHUNK];
    if (file_len < E16APP_HEADER_SIZE || rd(ud, buf, E16APP_HEADER_SIZE) != 0)
        return APP_ERR_TRUNCATED;
    int err = app_parse_header(buf, E16APP_HEADER_SIZE, out);
    if (err != APP_OK)
        return err;
    if (out->text_size > text_cap)
        return APP_ERR_TEXT_SIZE;
    if ((uint64_t)out->data_size + out->bss_size > data_cap)
        return APP_ERR_DATA_SIZE;
    uint64_t want = app_image_size(out);
    if (file_len < want)
        return APP_ERR_TRUNCATED;
    if (file_len != want)
        return APP_ERR_LENGTH;

    wr32(buf + 48, 0);
    uint32_t crc = app_crc32(0, buf, E16APP_HEADER_SIZE);
    for (uint64_t left = want - E16APP_HEADER_SIZE; left;) {
        size_t n = left < VERIFY_CHUNK ? (size_t)left : VERIFY_CHUNK;
        if (rd(ud, buf, n) != 0)
            return APP_ERR_TRUNCATED;
        crc = app_crc32(crc, buf, n);
        left -= n;
    }
    return crc == out->crc32 ? APP_OK : APP_ERR_CRC;
}

typedef struct {
    const uint8_t *p;
    size_t left;
} mem_reader_t;

static int mem_read(void *ud, void *buf, size_t n)
{
    mem_reader_t *m = ud;
    if (n > m->left)
        return -1;
    memcpy(buf, m->p, n);
    m->p += n;
    m->left -= n;
    return 0;
}

int app_load(const uint8_t *image, size_t len,
             uint8_t *text_dst, size_t text_cap,
             uint8_t *data_dst, size_t data_cap,
             app_loaded_t *out)
{
    if (!image)
        return APP_ERR_ARGS;
    e16app_header_t h;
    int err = app_parse_header(image, len, &h);
    if (err != APP_OK)
        return err;
    uint64_t want = (uint64_t)E16APP_HEADER_SIZE + h.text_size + h.data_size +
                    (uint64_t)h.fixup_count * 4u;
    if ((uint64_t)len < want)
        return APP_ERR_TRUNCATED;
    if ((uint64_t)len != want)
        return APP_ERR_LENGTH;

    mem_reader_t m = { image, len };
    app_mem_t mem = {
        text_dst, text_cap, (uint32_t)(uintptr_t)text_dst,
        data_dst, data_cap, (uint32_t)(uintptr_t)data_dst,
    };
    return app_load_stream(mem_read, &m, &mem, out);
}
