#ifndef E16_APP_FORMAT_H
#define E16_APP_FORMAT_H
#include <stdint.h>

/*
 * The .e16app image: a native app, built position-independent, that the
 * launcher copies into RAM and runs there. Nothing is ever written to the
 * E16's internal flash.
 *
 * Layout, all little-endian:
 *
 *   e16app_header_t           header_size bytes (sizeof(e16app_header_t))
 *   text                      text_size bytes: code, then read-only data
 *   data                      data_size bytes: the GOT, then .data
 *   fixups                    fixup_count x uint32_t
 *
 * .bss (bss_size bytes) is not stored: the loader zeroes it right after
 * data. The run-time data area is data_size + bss_size bytes.
 *
 * Addresses. The app is linked with its text at text_link_base and its
 * data at data_link_base (apps/sdk/app.ld: 0x00000000 and 0x80000000).
 * Code never holds an absolute address: it reaches every global through
 * the GOT, which r9 points at (-msingle-pic-base -mpic-register=r9
 * -mno-pic-data-is-text-relative), and calls are PC-relative. So only
 * two kinds of word in the image hold link-time addresses, and the
 * loader rebases both:
 *
 *   - the got_count GOT entries at data offset got_offset;
 *   - the words listed in the fixup table (data offsets, 4-aligned):
 *     initialized pointers in .data, e.g. `const char *names[] = {...}`
 *     or `void (*fn)(void) = f;` (the compiler puts those in .data.rel.*,
 *     which app.ld places in data).
 *
 * Each such word is rebased by range: a value in
 * [text_link_base, text_link_base + text_size] moves with the text, one in
 * [data_link_base, data_link_base + data_size + bss_size] moves with the
 * data (both ends inclusive, for one-past-the-end pointers); any other
 * value rejects the image. tools/mkapp.py makes the same check at build
 * time and refuses any relocation the loader couldn't redo.
 *
 * r9 = data base + got_offset (the GOT origin: app.ld puts
 * _GLOBAL_OFFSET_TABLE_ at the start of the GOT). Entry = text base +
 * entry_offset, with the Thumb bit set; it is called as
 * void entry(const e16_api_t *api) and never returns (apps/sdk/crt0.c
 * leaves through api->exit()).
 *
 * crc32 is the standard CRC-32 (IEEE 802.3, zlib's crc32) over the header
 * with its crc32 field set to 0, followed by the whole payload (text, data,
 * fixups).
 */

#define E16APP_MAGIC        "E16A"
#define E16APP_MAGIC_LEN    4
#define E16APP_ABI_VERSION  1u
#define E16APP_NAME_MAX     24  /* field size; NUL-terminated, so <= 23 chars */

/* No flags are defined yet; the loader rejects any it doesn't know. */
#define E16APP_FLAGS_KNOWN  0u

typedef struct __attribute__((packed)) {
    char     magic[E16APP_MAGIC_LEN];  /*  0: "E16A", no NUL */
    uint16_t abi_version;              /*  4: E16APP_ABI_VERSION */
    uint16_t header_size;              /*  6: sizeof(e16app_header_t) = 76 */
    uint32_t flags;                    /*  8: 0 */
    uint32_t text_size;                /* 12: code + rodata, multiple of 4 */
    uint32_t data_size;                /* 16: GOT + .data, multiple of 4 */
    uint32_t bss_size;                 /* 20: multiple of 4 */
    uint32_t got_offset;               /* 24: byte offset of the GOT in data */
    uint32_t got_count;                /* 28: GOT entries (32-bit words) */
    uint32_t fixup_count;              /* 32: entries in the fixup table */
    uint32_t entry_offset;             /* 36: entry point offset in text, Thumb bit clear */
    uint32_t text_link_base;           /* 40: link-time address of text */
    uint32_t data_link_base;           /* 44: link-time address of data */
    uint32_t crc32;                    /* 48: see above */
    char     name[E16APP_NAME_MAX];    /* 52: NUL-terminated, NUL-padded */
} e16app_header_t;                     /* 76 bytes */

#define E16APP_HEADER_SIZE 76u

_Static_assert(sizeof(e16app_header_t) == E16APP_HEADER_SIZE, "e16app header layout");

#endif
