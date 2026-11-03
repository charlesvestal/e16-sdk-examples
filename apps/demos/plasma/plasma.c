/*
 * Plasma: a fullscreen plasma or tunnel on the 128x64 OLED, ordered-
 * dithered to 1 bit, as fast as it goes, with a rainbow across all 256
 * LEDs.
 *
 *   encoder 1   speed (turn)
 *   encoder 2   palette: smooth, contour bands, inverted, 4 levels (turn)
 *   encoder 3   effect: plasma or tunnel (turn)
 *   Shift held 1 s: back to the launcher (the launcher's own watch)
 *
 * The frame rate is drawn in the top-left corner and logged every 5 s.
 *
 * Everything is integer math (no libc, no libm): a 256-entry sine table,
 * and per-pixel distance and angle tables computed once at start.
 */
#include "e16_app.h"

#define W E16_DISPLAY_WIDTH
#define H E16_DISPLAY_HEIGHT
#define LOG_EVERY_MS 5000u
#define LED_LEVEL 32 /* brightest LED channel */

/* sin, 0..255 for one turn, scaled to 0..255 (128 = zero). */
static const unsigned char sine[256] = {
    128, 131, 134, 137, 140, 144, 147, 150, 153, 156, 159, 162, 165, 168, 171, 174,
    177, 180, 183, 185, 188, 191, 194, 196, 199, 201, 204, 206, 209, 211, 214, 216,
    218, 220, 222, 225, 227, 229, 230, 232, 234, 236, 237, 239, 240, 242, 243, 245,
    246, 247, 248, 249, 250, 251, 252, 252, 253, 254, 254, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 254, 254, 253, 252, 252, 251, 250, 249, 248, 247,
    246, 245, 243, 242, 240, 239, 237, 236, 234, 232, 230, 229, 227, 225, 222, 220,
    218, 216, 214, 211, 209, 206, 204, 201, 199, 196, 194, 191, 188, 185, 183, 180,
    177, 174, 171, 168, 165, 162, 159, 156, 153, 150, 147, 144, 140, 137, 134, 131,
    128, 125, 122, 119, 116, 112, 109, 106, 103, 100,  97,  94,  91,  88,  85,  82,
     79,  76,  73,  71,  68,  65,  62,  60,  57,  55,  52,  50,  47,  45,  42,  40,
     38,  36,  34,  31,  29,  27,  26,  24,  22,  20,  19,  17,  16,  14,  13,  11,
     10,   9,   8,   7,   6,   5,   4,   4,   3,   2,   2,   1,   1,   1,   1,   1,
      0,   1,   1,   1,   1,   1,   2,   2,   3,   4,   4,   5,   6,   7,   8,   9,
     10,  11,  13,  14,  16,  17,  19,  20,  22,  24,  26,  27,  29,  31,  34,  36,
     38,  40,  42,  45,  47,  50,  52,  55,  57,  60,  62,  65,  68,  71,  73,  76,
     79,  82,  85,  88,  91,  94,  97, 100, 103, 106, 109, 112, 116, 119, 122, 125,
};

/* 8x8 ordered-dither thresholds (Bayer), 0..63. */
static const unsigned char bayer[8][8] = {
    {0, 32, 8, 40, 2, 34, 10, 42},  {48, 16, 56, 24, 50, 18, 58, 26},
    {12, 44, 4, 36, 14, 46, 6, 38}, {60, 28, 52, 20, 62, 30, 54, 22},
    {3, 35, 11, 43, 1, 33, 9, 41},  {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47, 7, 39, 13, 45, 5, 37}, {63, 31, 55, 23, 61, 29, 53, 21},
};

/* 3x5 digits and the letters of "fps", column-major, bit 0 at the top. */
static const unsigned char glyphs[][3] = {
    {0x1F, 0x11, 0x1F}, {0x12, 0x1F, 0x10}, {0x1D, 0x15, 0x17}, {0x15, 0x15, 0x1F}, {0x07, 0x04, 0x1F},
    {0x17, 0x15, 0x1D}, {0x1F, 0x15, 0x1D}, {0x01, 0x01, 0x1F}, {0x1F, 0x15, 0x1F}, {0x17, 0x15, 0x1F},
    {0x04, 0x1E, 0x05}, /* f */
    {0x1F, 0x05, 0x07}, /* p */
    {0x16, 0x15, 0x0D}, /* s */
};
#define GLYPH_F 10
#define GLYPH_P 11
#define GLYPH_S 12

static const char *const palette_names[] = {"smooth", "bands", "inverted", "4 levels"};
static const char *const effect_names[] = {"plasma", "tunnel"};

/* Per-pixel tables, filled at start (in .bss, which the loader zeroes). */
static unsigned char dist[H][W];  /* distance from the centre, 0..72 */
static unsigned char depth[H][W]; /* tunnel depth: 2048 / distance */
static unsigned char angle[H][W]; /* 0..255 around the centre */

static int speed = 4;   /* 0..16 */
static int palette;     /* 0..3 */
static int effect;      /* 0 plasma, 1 tunnel */

static unsigned isqrt(unsigned v)
{
    unsigned r = 0, bit = 1u << 30;
    while (bit > v)
        bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

/* atan2 as 0..255 per turn, piecewise linear per octant (good to a
 * couple of steps, plenty for a tunnel). */
static unsigned atan2_8(int y, int x)
{
    int ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
    if (ax == 0 && ay == 0)
        return 0;
    unsigned a = ax >= ay ? (unsigned)(ay * 32 / ax) : 64u - (unsigned)(ax * 32 / ay); /* 0..64: first quadrant */
    if (x < 0)
        a = 128u - a;
    if (y < 0)
        a = 256u - a;
    return a & 255u;
}

static void make_tables(void)
{
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int dx = x - W / 2, dy = y - H / 2;
            unsigned d = isqrt((unsigned)(dx * dx + dy * dy));
            dist[y][x] = (unsigned char)d;
            depth[y][x] = (unsigned char)(2048u / (d + 4u));
            angle[y][x] = (unsigned char)atan2_8(dy, dx);
        }
    }
}

static unsigned shade(unsigned v)
{
    switch (palette) {
    case 1: return (v & 32u) ? 255u : 0u;
    case 2: return 255u - v;
    case 3: return v & 0xC0u;
    default: return v;
    }
}

/* One frame into the framebuffer: 8 rows (a page) at a time, a byte per
 * column. */
static void render(unsigned char *fb, unsigned t)
{
    unsigned t1 = t, t2 = t * 3u / 2u, t3 = t / 2u, t4 = t * 2u;
    for (int page = 0; page < H / 8; page++) {
        for (int x = 0; x < W; x++) {
            unsigned char byte = 0;
            for (int b = 0; b < 8; b++) {
                int y = page * 8 + b;
                unsigned v;
                if (effect == 0) {
                    v = sine[(x * 3 + t1) & 255u] + sine[(y * 5 + t2) & 255u] +
                        sine[((x + y) * 2 + t3) & 255u] + sine[(dist[y][x] * 6 + t4) & 255u];
                    v >>= 2;
                } else {
                    unsigned u = angle[y][x] + t3, w = depth[y][x] + t1;
                    v = sine[(u * 2) & 255u] / 2u + sine[(w * 4) & 255u] / 2u;
                    /* darker towards the centre */
                    v = v * (dist[y][x] < 24 ? dist[y][x] : 24) / 24u;
                }
                if ((shade(v & 255u) >> 2) > bayer[y & 7][x & 7])
                    byte |= (unsigned char)(1u << b);
            }
            fb[page * W + x] = byte;
        }
    }
}

static void put_pixel(unsigned char *fb, int x, int y, int on)
{
    if (x < 0 || x >= W || y < 0 || y >= H)
        return;
    unsigned char m = (unsigned char)(1u << (y & 7));
    if (on)
        fb[(y >> 3) * W + x] |= m;
    else
        fb[(y >> 3) * W + x] &= (unsigned char)~m;
}

/* "NNN fps" in a black box at the top-left. */
static void draw_fps(unsigned char *fb, unsigned fps)
{
    int g[10], n = 0;
    char digits[6];
    int nd = 0;
    do {
        digits[nd++] = (char)(fps % 10);
        fps /= 10;
    } while (fps && nd < 5);
    while (nd)
        g[n++] = digits[--nd];
    g[n++] = -1; /* space */
    g[n++] = GLYPH_F;
    g[n++] = GLYPH_P;
    g[n++] = GLYPH_S;
    int width = n * 4 + 1;
    for (int y = 0; y < 7; y++)
        for (int x = 0; x < width; x++)
            put_pixel(fb, x, y, 0);
    for (int i = 0; i < n; i++) {
        if (g[i] < 0)
            continue;
        for (int c = 0; c < 3; c++)
            for (int r = 0; r < 5; r++)
                if (glyphs[g[i]][c] >> r & 1)
                    put_pixel(fb, 1 + i * 4 + c, 1 + r, 1);
    }
}

/* Hue 0..255 to an RGB triple at most LED_LEVEL bright. */
static void hue_rgb(unsigned h, unsigned char *r, unsigned char *g, unsigned char *b)
{
    unsigned region = h / 43u, f = (h - region * 43u) * 6u; /* 0..255 within the region */
    unsigned q = LED_LEVEL * (255u - f) / 255u, p = LED_LEVEL * f / 255u;
    switch (region) {
    case 0: *r = LED_LEVEL; *g = (unsigned char)p; *b = 0; break;
    case 1: *r = (unsigned char)q; *g = LED_LEVEL; *b = 0; break;
    case 2: *r = 0; *g = LED_LEVEL; *b = (unsigned char)p; break;
    case 3: *r = 0; *g = (unsigned char)q; *b = LED_LEVEL; break;
    case 4: *r = (unsigned char)p; *g = 0; *b = LED_LEVEL; break;
    default: *r = LED_LEVEL; *g = 0; *b = (unsigned char)q; break;
    }
}

static void rainbow(const e16_api_t *api, unsigned t)
{
    /* palette 2 (inverted) runs the rainbow backwards, 3 in coarse steps */
    for (int ring = 0; ring < E16_LED_RINGS; ring++) {
        for (int led = 0; led < E16_LEDS_PER_RING; led++) {
            unsigned i = (unsigned)(ring * E16_LEDS_PER_RING + led);
            unsigned h = palette == 2 ? t - i : t + i;
            if (palette == 3)
                h &= 0xE0u;
            unsigned char r, g, b;
            hue_rgb(h & 255u, &r, &g, &b);
            api->leds_set(ring, led, r, g, b);
        }
    }
    api->leds_show();
}

static char *put_str(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    return p;
}

static char *put_uint(char *p, unsigned v)
{
    char tmp[10];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        *p++ = tmp[--n];
    return p;
}

static void log_status(const e16_api_t *api, unsigned fps)
{
    char line[96];
    char *p = put_str(line, "fps ");
    p = put_uint(p, fps);
    p = put_str(p, " (");
    p = put_str(p, effect_names[effect]);
    p = put_str(p, ", speed ");
    p = put_uint(p, (unsigned)speed);
    p = put_str(p, ", palette ");
    p = put_str(p, palette_names[palette]);
    p = put_str(p, ")");
    *p = '\0';
    api->log(line);
}

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

int app_main(const e16_api_t *api)
{
    make_tables();
    unsigned char *fb = api->display_fb();
    api->log("plasma: encoder 1 speed, 2 palette, 3 effect; hold Shift to leave");

    unsigned frames = 0, fps = 0;
    unsigned t_fps = api->millis(), t_log = t_fps;
    unsigned phase = 0; /* 1/16 steps */
    unsigned last = t_fps;
    for (;;) {
        int d = api->enc_read(0);
        if (d)
            speed = clamp(speed + d, 0, 16);
        d = api->enc_read(1);
        if (d)
            palette = (palette + (d > 0 ? 1 : 3)) & 3;
        d = api->enc_read(2);
        if (d)
            effect ^= 1;

        unsigned now = api->millis();
        phase += (now - last) * (unsigned)speed;
        last = now;
        unsigned t = phase / 64u;

        render(fb, t);
        draw_fps(fb, fps);
        api->display_present();
        rainbow(api, t);

        frames++;
        if (now - t_fps >= 1000u) {
            fps = frames * 1000u / (now - t_fps);
            frames = 0;
            t_fps = now;
        }
        if (now - t_log >= LOG_EVERY_MS) {
            t_log = now;
            log_status(api, fps);
        }
    }
}
