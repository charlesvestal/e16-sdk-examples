#ifndef APPS_LOADER_H
#define APPS_LOADER_H
#include <stddef.h>
#include <stdint.h>
#include "e16_app_format.h"

/*
 * The .e16app loader (format: e16_app_format.h). Portable C, no SDK
 * includes: it validates an image, copies text and data into the buffers
 * it's given, zeroes .bss and rebases the GOT and the data fixups. It
 * doesn't run anything: the caller sets r9 = out->r9 and calls out->entry
 * with the API table in r0 (that part is device-only assembly).
 *
 * Two ways in:
 *
 *   app_load()         the whole image is in memory.
 *   app_load_stream()  the image comes from a reader callback, in order:
 *                      header, text, data, fixups. Each section is read
 *                      straight into its destination and the fixups in
 *                      small chunks, so loading from a FatFs file needs no
 *                      buffer for the image (the device path: f_read into
 *                      the destination buffers).
 *
 * The CRC is checked once everything has been read. The destination
 * buffers are scratch until app_load*() returns APP_OK: on any error their
 * contents are undefined and must not be run.
 *
 * Run addresses: on the device the code runs where it was copied, so
 * app_load() uses the buffers' own addresses. app_load_stream() takes the
 * run addresses separately (app_mem_t), which lets a host test load into
 * ordinary buffers while rebasing against 32-bit addresses.
 */

typedef enum {
    APP_OK = 0,
    APP_ERR_ARGS = -1,       /* NULL pointer or similar caller error */
    APP_ERR_TRUNCATED = -2,  /* image ends early (or the reader failed) */
    APP_ERR_MAGIC = -3,      /* not an .e16app */
    APP_ERR_ABI = -4,        /* built for another ABI version */
    APP_ERR_HEADER = -5,     /* header fields inconsistent */
    APP_ERR_TEXT_SIZE = -6,  /* text doesn't fit text_cap */
    APP_ERR_DATA_SIZE = -7,  /* data + bss don't fit data_cap */
    APP_ERR_LENGTH = -8,     /* image length doesn't match the header */
    APP_ERR_CRC = -9,        /* checksum mismatch: corrupt image */
    APP_ERR_RELOC = -10,     /* GOT entry or fixup outside the app */
    APP_ERR_ALIGN = -11,     /* destination not 8-byte aligned */
} app_err_t;

/* A short English message for a code from this module (never NULL). */
const char *app_strerror(int err);

typedef struct {
    uint32_t entry;          /* run address of the entry point, Thumb bit set */
    uint32_t r9;             /* run address of the GOT origin */
    uint32_t text_addr;      /* run addresses and sizes as loaded */
    uint32_t text_size;
    uint32_t data_addr;
    uint32_t data_size;
    uint32_t bss_size;       /* zeroed, right after data */
    uint32_t got_count;
    uint32_t fixup_count;
    char name[E16APP_NAME_MAX];
} app_loaded_t;

/*
 * Parse and validate a header from its E16APP_HEADER_SIZE bytes (the menu
 * can use this to list apps without loading them). Checks everything that
 * doesn't need the payload or the destination; returns APP_OK or an error.
 */
int app_parse_header(const uint8_t *bytes, size_t len, e16app_header_t *out);

/* Load an in-memory image. len must be exactly the image's size. */
int app_load(const uint8_t *image, size_t len,
             uint8_t *text_dst, size_t text_cap,
             uint8_t *data_dst, size_t data_cap,
             app_loaded_t *out);

/* Read exactly n bytes into buf; return 0, or nonzero on error / EOF. */
typedef int (*app_read_fn)(void *ud, void *buf, size_t n);

typedef struct {
    uint8_t *text;           /* where the text is copied */
    size_t text_cap;
    uint32_t text_addr;      /* where it runs (== text on the device) */
    uint8_t *data;           /* where data + bss go */
    size_t data_cap;
    uint32_t data_addr;      /* where they live (== data on the device) */
} app_mem_t;

int app_load_stream(app_read_fn rd, void *ud, const app_mem_t *mem,
                    app_loaded_t *out);

/* The whole image's size in bytes, from a parsed header. */
uint64_t app_image_size(const e16app_header_t *h);

/*
 * Check an image without loading it, reading it through rd in small
 * chunks (no destination buffers needed): the header, the sizes against
 * the caps, the length (file_len must be exactly the image's size) and
 * the CRC. Fills *out with the header (valid once the header parsed).
 * The menu runs this before resetting into app mode, so a corrupt or
 * oversized image is refused right there.
 */
int app_verify_stream(app_read_fn rd, void *ud, uint64_t file_len, size_t text_cap, size_t data_cap,
                      e16app_header_t *out);

/* CRC-32 (IEEE, zlib-compatible). Start with crc = 0. */
uint32_t app_crc32(uint32_t crc, const void *buf, size_t n);

#endif
