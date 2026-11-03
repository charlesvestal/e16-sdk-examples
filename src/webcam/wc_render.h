#ifndef WC_RENDER_H
#define WC_RENDER_H
#include <stdbool.h>
#include <stdint.h>

/*
 * Webcam mode's video synth (src/webcam/webcam.h is the device side): a
 * 160x120 picture made a line at a time, with no frame buffer, from
 * tables and two palettes. Portable C (host-tested by
 * test/webcam/wc_render_test.c); grown from the SDK's examples/usb_cam.
 *
 *   wc_tables_init()   once, in the main loop: sine, the distance and angle
 *                      of each pixel of one quarter of the picture, the
 *                      tunnel's depth (~10 KB; the caller provides the memory)
 *   wc_palette_build() in the main loop, whenever a colour setting changes:
 *                      256 colours as YUV (for the video) and RGB (the LEDs),
 *                      with hue, palette type, contrast, brightness,
 *                      posterize and a flash already applied
 *   wc_view_prepare()  at each frame's start: a copy of the settings and the
 *                      per-frame tables (the plasma's waves, the warp)
 *   wc_render_line()   one line as YUY2: lookups, adds and a few multiplies
 *                      per pixel, a pass at a time (80-170 us on the E16)
 *   wc_sample()        one pixel's colour, for the OLED mirror and the LEDs
 *
 * Parameters are small integers (wc_param_info[]): the encoders step them
 * by one, MIDI CCs set them across their range.
 */

#define WC_W 160
#define WC_H 120
#define WC_QW (WC_W / 2)
#define WC_QH (WC_H / 2)

enum {
    WC_PATTERN,   /* 1  pattern A */
    WC_HUE,       /* 2  palette rotation */
    WC_SPEED,     /* 3  animation speed */
    WC_ZOOM,      /* 4  pattern scale */
    WC_PATTERN_B, /* 5  pattern B, mixed in by MIX */
    WC_MIX,       /* 6  how much of B: 0 none, 16 all */
    WC_MIX_MODE,  /* 7  how A and B combine */
    WC_SYMMETRY,  /* 8  off, mirror, quad, kaleidoscope */
    WC_PALETTE,   /* 9  rainbow, fire, ice, neon, mono */
    WC_CYCLE,     /* 10 colour cycling speed (negative: backwards) */
    WC_POSTERIZE, /* 11 off, or that many colour bands */
    WC_CONTRAST,  /* 12 8 = normal */
    WC_BRIGHT,    /* 13 0 = normal */
    WC_WARP,      /* 14 sideways wobble per line */
    WC_TEXT,      /* 15 text overlay: off, names, MIDI note, "E16" */
    WC_SCANLINES, /* 16 darken every other line */
    WC_PARAMS
};

enum { WC_PLASMA, WC_TUNNEL, WC_RINGS, WC_BARS, WC_SPIRAL, WC_CHECKER, WC_PATTERNS };
enum { WC_MIX_BLEND, WC_MIX_ADD, WC_MIX_XOR, WC_MIX_WIPE, WC_MIX_MODES };
enum { WC_SYM_OFF, WC_SYM_MIRROR, WC_SYM_QUAD, WC_SYM_KALEIDO, WC_SYMS };
enum { WC_PAL_RAINBOW, WC_PAL_FIRE, WC_PAL_ICE, WC_PAL_NEON, WC_PAL_MONO, WC_PALETTES };
enum { WC_TEXT_OFF, WC_TEXT_NAME, WC_TEXT_NOTE, WC_TEXT_E16, WC_TEXTS };

typedef struct {
    const char *name; /* 5 characters at most: the OLED and the log */
    int16_t min, max, def;
    bool wraps; /* turning past the end comes round to the start */
} wc_param_info_t;

extern const wc_param_info_t wc_param_info[WC_PARAMS];

/* A parameter's value as text, e.g. "PLASMA", "XOR", "12". buf: 8 bytes. */
const char *wc_value_text(int param, int value, char *buf);

/* Clamps (or wraps) v into param's range. */
int wc_param_limit(int param, int v);

/* MIDI CC value 0-127 to param's range. */
int wc_param_from_cc(int param, int cc_value);

typedef struct {
    int8_t sin64[256];          /* 63 * sin, one turn in 256 steps */
    uint8_t qdist[WC_QH][WC_QW];  /* distance from the centre, one quarter */
    uint8_t qangle[WC_QH][WC_QW]; /* angle from the centre, 0-64 over the quarter turn */
    uint8_t depth[256];         /* tunnel depth for each distance */
} wc_tables_t;

void wc_tables_init(wc_tables_t *t);

typedef struct {
    uint8_t y[256], u[256], v[256]; /* BT.601 video range */
    uint8_t r[256], g[256], b[256];
} wc_palette_t;

/* flash: 0-255, towards white (a MIDI note's flash). */
void wc_palette_build(wc_palette_t *p, const wc_tables_t *t, const int16_t *params, uint8_t flash);

/* The text overlay: 8 rows of the launcher's font, up to 80 columns, drawn at twice the size. */
#define WC_TEXT_COLS 80
typedef struct {
    uint8_t bits[8][WC_TEXT_COLS / 8]; /* row-major, bit 7 = leftmost */
    bool on;
} wc_text_t;

/* Lays out s in the font (font.h) into the mask; empty s: no overlay. */
void wc_text_set(wc_text_t *m, const char *s);

/* What one frame is made from: the settings, copied in one go. */
typedef struct {
    int16_t params[WC_PARAMS];
    uint32_t clock;  /* animation time, advanced by SPEED */
    uint32_t cycle;  /* colour cycling time, advanced by CYCLE */
    wc_text_t text;
} wc_settings_t;

#define WC_TEXT_X 4
#define WC_TEXT_Y (WC_H - 20)

typedef struct {
    const wc_tables_t *tab;
    const uint8_t *py, *pu, *pv; /* the palette's YUV */
    uint8_t pattern, pattern_b, mix, mix_mode, symmetry, zoom, scan_k;
    uint8_t t;         /* animation phase */
    uint8_t cyc;       /* palette index offset */
    int8_t xwave[WC_W];     /* plasma: the column term */
    int8_t dwave[WC_W + WC_H]; /* plasma: the diagonal term */
    int8_t warp[WC_H];     /* sideways shift of each line */
    wc_text_t text;
} wc_view_t;

void wc_view_prepare(wc_view_t *v, const wc_tables_t *t, const wc_settings_t *s, const uint8_t *py,
                     const uint8_t *pu, const uint8_t *pv);

/* Working space for one line: each pixel's coordinates after the warp and
 * the symmetry fold, and the two patterns' palette indexes and shades. The
 * line is made a pass at a time over these (one pattern's whole line in one
 * loop, no per-pixel branching on the settings), which is what makes it
 * fast enough for the interrupt. */
typedef struct {
    uint8_t qx[WC_W], qy[WC_W]; /* the quarter-picture coordinates (distance from the centre) */
    uint8_t fx[WC_W], fy[WC_W]; /* the screen coordinates the patterns see */
    uint8_t side[WC_W];         /* bit 0: left half, bit 1: top half */
    uint8_t a[WC_W], b[WC_W];   /* palette indexes of pattern A and B */
    uint8_t sa[WC_W], sb[WC_W]; /* their shades, 255 = full brightness */
} wc_line_t;

/* Line y (0 = top) as YUY2: WC_W * 2 bytes, 4-byte aligned. The same
 * pixels as wc_sample() gives, faster. */
void wc_render_line(const wc_view_t *v, wc_line_t *w, int y, uint8_t *yuy2);

/* Pixel x, y: its palette index; *shade is the brightness it's drawn at (0-255). */
uint8_t wc_sample(const wc_view_t *v, int x, int y, uint8_t *shade);

/* "C#4" etc. for MIDI note n (middle C = C4). buf: 5 bytes. */
const char *wc_note_name(int n, char *buf);

#endif
