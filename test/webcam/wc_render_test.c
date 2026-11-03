/*
 * Host test for src/webcam/wc_render.c, webcam mode's video synth. Plain
 * C, run directly by test/run_all.sh; prints "ALL PASSED" and exits 0 on
 * success.
 *
 *   wc_render_test              run the tests and the timing report
 *   wc_render_test --ppm DIR    also write one frame per pattern as
 *                               DIR/<pattern>.ppm (for looking at)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "wc_render.h"

static int g_failures;
#define CHECK(cond, ...)                                       \
    do {                                                        \
        if (!(cond)) {                                          \
            g_failures++;                                       \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                       \
            fprintf(stderr, "\n");                              \
        }                                                       \
    } while (0)

static wc_tables_t g_tab;
static wc_palette_t g_pal;
static uint8_t g_frame[WC_H][WC_W * 2] __attribute__((aligned(4)));

static void defaults(wc_settings_t *s)
{
    memset(s, 0, sizeof *s);
    for (int i = 0; i < WC_PARAMS; i++) {
        s->params[i] = wc_param_info[i].def;
    }
    s->clock = 12345;
}

/* A whole frame from settings s, the way webcam.c makes one. */
static wc_line_t g_line;

static void render(const wc_settings_t *s, uint8_t flash)
{
    static wc_view_t v;
    wc_palette_build(&g_pal, &g_tab, s->params, flash);
    wc_view_prepare(&v, &g_tab, s, g_pal.y, g_pal.u, g_pal.v);
    for (int y = 0; y < WC_H; y++) {
        wc_render_line(&v, &g_line, y, g_frame[y]);
    }
}

static int luma(int x, int y)
{
    return g_frame[y][x * 2];
}

/* Every byte of the frame in YUY2's video range. */
static void check_ranges(const char *what)
{
    int bad = 0;
    for (int y = 0; y < WC_H; y++) {
        for (int x = 0; x < WC_W * 2; x++) {
            int b = g_frame[y][x];
            bool is_y = (x & 1) == 0;
            if (is_y ? (b < 16 || b > 235) : (b < 16 || b > 240)) {
                bad++;
            }
        }
    }
    CHECK(bad == 0, "%s: %d bytes out of range", what, bad);
}

static void test_params(void)
{
    CHECK(wc_param_limit(WC_PATTERN, WC_PATTERNS) == 0, "pattern wraps forward");
    CHECK(wc_param_limit(WC_PATTERN, -1) == WC_PATTERNS - 1, "pattern wraps back");
    CHECK(wc_param_limit(WC_SPEED, 99) == 32, "speed clamps");
    CHECK(wc_param_limit(WC_CYCLE, -99) == -16, "cycle clamps low");
    for (int p = 0; p < WC_PARAMS; p++) {
        const wc_param_info_t *i = &wc_param_info[p];
        CHECK(wc_param_from_cc(p, 0) == i->min, "%s: CC 0 -> min", i->name);
        CHECK(wc_param_from_cc(p, 127) == i->max, "%s: CC 127 -> max", i->name);
        CHECK(i->def >= i->min && i->def <= i->max, "%s: default in range", i->name);
        CHECK(strlen(i->name) <= 5, "%s: name too long", i->name);
        char buf[8];
        for (int v = i->min; v <= i->max; v++) {
            const char *t = wc_value_text(p, v, buf);
            CHECK(t && strlen(t) <= 6, "%s %d: value text '%s'", i->name, v, t ? t : "(null)");
        }
    }
    char nb[5];
    CHECK(strcmp(wc_note_name(60, nb), "C4") == 0, "note 60 is %s", nb);
    CHECK(strcmp(wc_note_name(61, nb), "C#4") == 0, "note 61 is %s", nb);
    CHECK(strcmp(wc_note_name(0, nb), "C-1") == 0, "note 0 is %s", nb);
    CHECK(strcmp(wc_note_name(127, nb), "G9") == 0, "note 127 is %s", nb);
}

static void test_tables(void)
{
    CHECK(g_tab.qdist[0][0] == 0, "centre distance %d", g_tab.qdist[0][0]);
    CHECK(g_tab.qdist[WC_QH - 1][WC_QW - 1] == 99, "corner distance %d", g_tab.qdist[WC_QH - 1][WC_QW - 1]);
    CHECK(g_tab.sin64[64] == 63 && g_tab.sin64[192] == -63 && g_tab.sin64[0] == 0, "sine");
    CHECK(g_tab.qangle[0][WC_QW - 1] <= 1 && g_tab.qangle[WC_QH - 1][0] >= 63, "angles");
}

static void test_palettes(void)
{
    wc_settings_t s;
    defaults(&s);
    for (int pal = 0; pal < WC_PALETTES; pal++) {
        for (int c = 0; c <= 16; c += 8) {
            for (int b = -8; b <= 8; b += 8) {
                for (int flash = 0; flash <= 255; flash += 255) {
                    s.params[WC_PALETTE] = (int16_t)pal;
                    s.params[WC_CONTRAST] = (int16_t)c;
                    s.params[WC_BRIGHT] = (int16_t)b;
                    wc_palette_build(&g_pal, &g_tab, s.params, (uint8_t)flash);
                    int bad = 0;
                    for (int i = 0; i < 256; i++) {
                        bad += g_pal.y[i] < 16 || g_pal.y[i] > 235 || g_pal.u[i] < 16 || g_pal.u[i] > 240 ||
                               g_pal.v[i] < 16 || g_pal.v[i] > 240;
                    }
                    CHECK(bad == 0, "palette %d contrast %d bright %d flash %d: %d out of range", pal, c, b, flash,
                          bad);
                }
            }
        }
    }
    /* A full flash is white. */
    defaults(&s);
    wc_palette_build(&g_pal, &g_tab, s.params, 255);
    CHECK(g_pal.y[0] == 235 && g_pal.y[100] == 235, "flash 255 is white (%d)", g_pal.y[0]);
    /* Posterize: at most that many colours. */
    for (int bands = 2; bands <= 16; bands++) {
        s.params[WC_POSTERIZE] = (int16_t)bands;
        wc_palette_build(&g_pal, &g_tab, s.params, 0);
        int n = 0; /* runs of one colour: the bands */
        for (int i = 0; i < 256; i++) {
            if (i == 0 || g_pal.r[i] != g_pal.r[i - 1] || g_pal.g[i] != g_pal.g[i - 1] || g_pal.b[i] != g_pal.b[i - 1]) {
                n++;
            }
        }
        CHECK(n <= bands, "posterize %d: %d runs of colour", bands, n);
    }
}

/* wc_render_line() (the interrupt's) and wc_sample() (the OLED's and the
 * LEDs') agree on every pixel's luma, outside the text. */
static void check_matches_sample(const wc_settings_t *s, const char *what)
{
    static wc_view_t v;
    wc_view_prepare(&v, &g_tab, s, g_pal.y, g_pal.u, g_pal.v);
    int bad = 0;
    int k_odd = v.scan_k ? v.scan_k : 256;
    for (int y = 0; y < WC_H; y++) {
        if (s->text.on && y >= WC_TEXT_Y && y < WC_TEXT_Y + 16) {
            continue;
        }
        for (int x = 0; x < WC_W; x++) {
            uint8_t shade;
            uint8_t i = wc_sample(&v, x, y, &shade);
            int k = (y & 1) ? k_odd : 256;
            int kk = shade * k >> 8;
            int l = g_pal.y[i];
            if (kk < 255) {
                l = 16 + ((l - 16) * kk >> 8);
            }
            bad += l != luma(x, y);
        }
    }
    CHECK(bad == 0, "%s: %d pixels differ from wc_sample()", what, bad);
}

static void test_every_combination_in_range(void)
{
    wc_settings_t s;
    defaults(&s);
    int frames = 0;
    for (int a = 0; a < WC_PATTERNS; a++) {
        for (int b = 0; b < WC_PATTERNS; b++) {
            for (int mode = 0; mode < WC_MIX_MODES; mode++) {
                for (int sym = 0; sym < WC_SYMS; sym++) {
                    defaults(&s);
                    s.params[WC_PATTERN] = (int16_t)a;
                    s.params[WC_PATTERN_B] = (int16_t)b;
                    s.params[WC_MIX] = 8;
                    s.params[WC_MIX_MODE] = (int16_t)mode;
                    s.params[WC_SYMMETRY] = (int16_t)sym;
                    s.params[WC_ZOOM] = (int16_t)(1 + (a + b + mode + sym) % 8);
                    s.params[WC_WARP] = (int16_t)((a * 3 + sym) % 17);
                    s.params[WC_SCANLINES] = (int16_t)(mode % 4);
                    s.params[WC_TEXT] = WC_TEXT_E16;
                    wc_text_set(&s.text, "PLASMA+RINGS");
                    s.clock = (uint32_t)(a * 7777 + b * 333 + mode * 55 + sym);
                    render(&s, 0);
                    char what[64];
                    snprintf(what, sizeof what, "A %d B %d mode %d sym %d", a, b, mode, sym);
                    check_ranges(what);
                    check_matches_sample(&s, what);
                    frames++;
                }
            }
        }
    }
    printf("range check: %d frames\n", frames);
}

static void test_symmetry(void)
{
    wc_settings_t s;
    for (int a = 0; a < WC_PATTERNS; a++) {
        defaults(&s);
        s.params[WC_PATTERN] = (int16_t)a;
        s.params[WC_SYMMETRY] = WC_SYM_MIRROR;
        render(&s, 0);
        int bad = 0;
        for (int y = 0; y < WC_H; y++) {
            for (int x = 0; x < WC_W; x++) {
                bad += luma(x, y) != luma(WC_W - 1 - x, y);
            }
        }
        CHECK(bad == 0, "pattern %d mirror: %d pixels differ from their mirror image", a, bad);

        for (int sym = WC_SYM_QUAD; sym <= WC_SYM_KALEIDO; sym++) {
            s.params[WC_SYMMETRY] = (int16_t)sym;
            render(&s, 0);
            bad = 0;
            for (int y = 0; y < WC_H; y++) {
                for (int x = 0; x < WC_W; x++) {
                    bad += luma(x, y) != luma(WC_W - 1 - x, y) || luma(x, y) != luma(x, WC_H - 1 - y);
                }
            }
            CHECK(bad == 0, "pattern %d symmetry %d: %d pixels not four-fold", a, sym, bad);
        }
        /* The kaleidoscope also folds along the diagonals (through the centre). */
        s.params[WC_SYMMETRY] = WC_SYM_KALEIDO;
        render(&s, 0);
        bad = 0;
        for (int d = 0; d < WC_QH; d++) {
            for (int e = 0; e < WC_QH; e++) {
                bad += luma(WC_QW + d, WC_QH + e) != luma(WC_QW + e, WC_QH + d);
            }
        }
        CHECK(bad == 0, "pattern %d kaleidoscope: %d pixels not symmetric about the diagonal", a, bad);
    }
}

static void test_mix(void)
{
    static uint8_t ref[WC_H][WC_W * 2];
    wc_settings_t s;
    for (int a = 0; a < WC_PATTERNS; a++) {
        int b = (a + 2) % WC_PATTERNS;
        /* MIX 0: B makes no difference. */
        defaults(&s);
        s.params[WC_PATTERN] = (int16_t)a;
        s.params[WC_MIX] = 0;
        render(&s, 0);
        memcpy(ref, g_frame, sizeof ref);
        s.params[WC_PATTERN_B] = (int16_t)b;
        s.params[WC_MIX_MODE] = WC_MIX_XOR;
        render(&s, 0);
        CHECK(memcmp(ref, g_frame, sizeof ref) == 0, "pattern %d: MIX 0 still shows B", a);
        /* WIPE at 16 is all B. */
        s.params[WC_PATTERN] = (int16_t)b;
        s.params[WC_MIX] = 0;
        render(&s, 0);
        memcpy(ref, g_frame, sizeof ref);
        s.params[WC_PATTERN] = (int16_t)a;
        s.params[WC_MIX] = 16;
        s.params[WC_MIX_MODE] = WC_MIX_WIPE;
        render(&s, 0);
        CHECK(memcmp(ref, g_frame, sizeof ref) == 0, "pattern %d: WIPE 16 isn't all B (%d)", a, b);
        /* BLEND at 8 differs from both. */
        s.params[WC_MIX] = 8;
        s.params[WC_MIX_MODE] = WC_MIX_BLEND;
        render(&s, 0);
        CHECK(memcmp(ref, g_frame, sizeof ref) != 0, "pattern %d: BLEND 8 is just B", a);
    }
}

static void test_animation_and_text(void)
{
    static uint8_t ref[WC_H][WC_W * 2];
    wc_settings_t s;
    for (int a = 0; a < WC_PATTERNS; a++) {
        defaults(&s);
        s.params[WC_PATTERN] = (int16_t)a;
        render(&s, 0);
        memcpy(ref, g_frame, sizeof ref);
        s.clock += 32 * 5; /* five steps of animation phase */
        render(&s, 0);
        CHECK(memcmp(ref, g_frame, sizeof ref) != 0, "pattern %d doesn't move", a);
        /* Colour cycling moves it too, without the clock. */
        s.clock -= 32 * 5;
        s.cycle = 64 * 10;
        render(&s, 0);
        CHECK(memcmp(ref, g_frame, sizeof ref) != 0, "pattern %d doesn't cycle", a);
    }

    /* Text: white pixels in its band, none outside it that weren't there before. */
    defaults(&s);
    s.params[WC_PALETTE] = WC_PAL_FIRE;
    s.params[WC_CONTRAST] = 4; /* nothing in the picture reaches white */
    render(&s, 0);
    memcpy(ref, g_frame, sizeof ref);
    wc_text_set(&s.text, "OXI E16");
    CHECK(s.text.on, "text has ink");
    render(&s, 0);
    int in = 0, out = 0;
    for (int y = 0; y < WC_H; y++) {
        for (int x = 0; x < WC_W; x++) {
            bool white = luma(x, y) == 235;
            bool band = y >= WC_TEXT_Y && y < WC_TEXT_Y + 16;
            in += band && white;
            out += !band && memcmp(&ref[y][x * 2], &g_frame[y][x * 2], 1) != 0;
        }
    }
    CHECK(in > 100, "text: only %d white pixels in its band", in);
    CHECK(out == 0, "text: %d pixels changed outside its band", out);
    wc_text_set(&s.text, "");
    CHECK(!s.text.on, "empty text has no ink");

    /* Scanlines darken odd lines only. */
    defaults(&s);
    render(&s, 0);
    memcpy(ref, g_frame, sizeof ref);
    s.params[WC_SCANLINES] = 3;
    render(&s, 0);
    long even = 0, odd = 0;
    for (int y = 0; y < WC_H; y++) {
        for (int x = 0; x < WC_W; x++) {
            int d = ref[y][x * 2] - g_frame[y][x * 2];
            if (y & 1) {
                odd += d;
            } else {
                even += d != 0;
            }
        }
    }
    CHECK(even == 0, "scanlines changed %ld pixels on even lines", even);
    CHECK(odd > WC_W * WC_H / 2 * 20, "scanlines: odd lines only %ld darker in all", odd);
}

static void timing(void)
{
    wc_settings_t s;
    defaults(&s);
    static wc_view_t v;
    static wc_line_t w;
    const struct {
        const char *what;
        int a, b, mix, sym, warp;
    } cases[] = {
        {"plasma", WC_PLASMA, 0, 0, 0, 0},
        {"tunnel", WC_TUNNEL, 0, 0, 0, 0},
        {"plasma+tunnel blend, kaleido, warp", WC_PLASMA, WC_TUNNEL, 8, WC_SYM_KALEIDO, 8},
    };
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        s.params[WC_PATTERN] = (int16_t)cases[c].a;
        s.params[WC_PATTERN_B] = (int16_t)cases[c].b;
        s.params[WC_MIX] = (int16_t)cases[c].mix;
        s.params[WC_SYMMETRY] = (int16_t)cases[c].sym;
        s.params[WC_WARP] = (int16_t)cases[c].warp;
        wc_palette_build(&g_pal, &g_tab, s.params, 0);
        clock_t t0 = clock();
        int n = 200;
        for (int f = 0; f < n; f++) {
            s.clock += 100;
            wc_view_prepare(&v, &g_tab, &s, g_pal.y, g_pal.u, g_pal.v);
            for (int y = 0; y < WC_H; y++) {
                wc_render_line(&v, &w, y, g_frame[y]);
            }
        }
        double us = (double)(clock() - t0) * 1e6 / CLOCKS_PER_SEC / n;
        printf("timing (host): %s: %.0f us/frame\n", cases[c].what, us);
    }
}

/* Frame as RGB PPM, for looking at. */
static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        return;
    }
    fprintf(f, "P6 %d %d 255\n", WC_W, WC_H);
    for (int y = 0; y < WC_H; y++) {
        for (int x = 0; x < WC_W; x++) {
            int yy = g_frame[y][x * 2] - 16;
            int u = g_frame[y][(x & ~1) * 2 + 1] - 128, v = g_frame[y][(x & ~1) * 2 + 3] - 128;
            int r = (298 * yy + 409 * v + 128) >> 8, g = (298 * yy - 100 * u - 208 * v + 128) >> 8,
                b = (298 * yy + 516 * u + 128) >> 8;
            uint8_t px[3] = {(uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r), (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g),
                             (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b)};
            fwrite(px, 1, 3, f);
        }
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    wc_tables_init(&g_tab);
    test_params();
    test_tables();
    test_palettes();
    test_every_combination_in_range();
    test_symmetry();
    test_mix();
    test_animation_and_text();
    timing();
    if (argc == 3 && strcmp(argv[1], "--ppm") == 0) {
        wc_settings_t s;
        char path[512];
        for (int a = 0; a < WC_PATTERNS; a++) {
            defaults(&s);
            s.params[WC_PATTERN] = (int16_t)a;
            render(&s, 0);
            char buf[8];
            snprintf(path, sizeof path, "%s/%s.ppm", argv[2], wc_value_text(WC_PATTERN, a, buf));
            write_ppm(path);
        }
    }
    if (g_failures) {
        printf("%d FAILED\n", g_failures);
        return 1;
    }
    printf("ALL PASSED\n");
    return 0;
}
