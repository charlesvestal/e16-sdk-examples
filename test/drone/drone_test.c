/*
 * Host test for apps/drone/core (the drone synth's DSP). Plain C, run by
 * test/run_all.sh; prints "ALL PASSED" and exits 0 on success.
 *
 *   drone_test                 the tests and a timing report
 *   drone_test --wav out.wav   also renders a 10 s demo (48 kHz, 16-bit
 *                              mono) with a few macro moves
 *
 * docs/audio/drone-demo.wav is made with --wav.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "drone.h"
#include "drone_save.h"

#define FS 48000
#define BLOCK 32 /* the launcher's E16_AUDIO_BLOCK */

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

static drone_t g_d; /* ~33 KB: not on the stack */

typedef struct {
    double sum2;
    float peak;
    long n, bad, nonfinite;
} stats_t;

static void account(stats_t *st, const float *x, int n)
{
    for (int i = 0; i < n; i++) {
        float v = x[i];
        if (!isfinite(v)) {
            st->nonfinite++;
            continue;
        }
        if (v > 1.0f || v < -1.0f) {
            st->bad++;
        }
        float a = fabsf(v);
        if (a > st->peak) {
            st->peak = a;
        }
        st->sum2 += (double)v * v;
        st->n++;
    }
}

static double dbfs(double x)
{
    return x > 0 ? 20.0 * log10(x) : -999.0;
}

static void render_seconds(drone_t *d, double seconds, stats_t *st, float *keep, long keep_cap, long *kept)
{
    float buf[BLOCK];
    long blocks = (long)(seconds * FS / BLOCK);
    for (long b = 0; b < blocks; b++) {
        drone_render(d, buf, BLOCK);
        if (st) {
            account(st, buf, BLOCK);
        }
        if (keep) {
            for (int i = 0; i < BLOCK && *kept < keep_cap; i++) {
                keep[(*kept)++] = buf[i];
            }
        }
    }
}

/* The default patch: bounded, audible, not too hot. */
static void test_default(void)
{
    drone_init(&g_d, FS, 1);
    stats_t st = {0};
    render_seconds(&g_d, 30.0, &st, NULL, 0, NULL);
    double rms = sqrt(st.sum2 / (double)st.n);
    printf("default patch, 30 s: peak %.1f dBFS, RMS %.1f dBFS\n", dbfs(st.peak), dbfs(rms));
    CHECK(st.nonfinite == 0 && st.bad == 0, "bounded (%ld non-finite, %ld out of range)", st.nonfinite, st.bad);
    CHECK(dbfs(rms) > -30.0 && dbfs(rms) < -6.0, "RMS %.1f dBFS in -30..-6", dbfs(rms));
    CHECK(st.peak > 0.1f && st.peak <= 1.0f, "peak %.3f", st.peak);
}

/* Random macros, toggles and notes for 10 minutes of audio: never a
 * non-finite or out-of-range sample. */
static void test_fuzz(void)
{
    drone_init(&g_d, FS, 7);
    srand(12345);
    stats_t st = {0};
    float buf[BLOCK];
    long blocks = 600L * FS / BLOCK;
    for (long b = 0; b < blocks; b++) {
        if (b % 375 == 0) { /* every 0.25 s */
            int n = 1 + rand() % 4;
            for (int k = 0; k < n; k++) {
                int p = rand() % DP_COUNT;
                const drone_param_info_t *pi = &drone_params[p];
                int v = rand() % 5 == 0 ? (rand() % 2 ? pi->hi : pi->lo) : pi->lo + rand() % (pi->hi - pi->lo + 1);
                drone_set(&g_d, p, v);
            }
            if (rand() % 8 == 0) {
                drone_set_flag(&g_d, rand() % DF_COUNT, rand() % 2);
            }
            if (rand() % 6 == 0) {
                drone_note(&g_d, rand() % 128, rand() % 128);
            }
            if (rand() % 50 == 0) {
                drone_set(&g_d, DP_LEVEL, 127); /* make sure loud corners get visited */
                drone_set(&g_d, DP_DRIVE, 127);
                drone_set(&g_d, DP_RESO, 127);
            }
        }
        drone_render(&g_d, buf, BLOCK);
        account(&st, buf, BLOCK);
    }
    printf("fuzz, 600 s: peak %.2f dBFS, RMS %.1f dBFS\n", dbfs(st.peak), dbfs(sqrt(st.sum2 / (double)st.n)));
    CHECK(st.nonfinite == 0, "%ld non-finite samples", st.nonfinite);
    CHECK(st.bad == 0, "%ld samples outside [-1, 1]", st.bad);
    CHECK(isfinite(g_d.ic1) && isfinite(g_d.ic2) && isfinite(g_d.lp[0]) && isfinite(g_d.env), "states finite");
}

/* Every macro at its top (and the space frozen), then its bottom. */
static void test_extremes(void)
{
    drone_init(&g_d, FS, 3);
    for (int p = 0; p < DP_COUNT; p++) {
        drone_set(&g_d, p, drone_params[p].hi);
    }
    drone_set_flag(&g_d, DF_OPEN, 1);
    stats_t st = {0};
    render_seconds(&g_d, 20.0, &st, NULL, 0, NULL);
    drone_set_flag(&g_d, DF_FREEZE, 1);
    render_seconds(&g_d, 20.0, &st, NULL, 0, NULL);
    printf("all macros at max (+ freeze), 40 s: peak %.2f dBFS, RMS %.1f dBFS\n", dbfs(st.peak),
           dbfs(sqrt(st.sum2 / (double)st.n)));
    CHECK(st.nonfinite == 0 && st.bad == 0, "max: bounded (%ld, %ld)", st.nonfinite, st.bad);
    CHECK(st.peak > 0.5f, "max: loud (%.2f)", st.peak);

    for (int p = 0; p < DP_COUNT; p++) {
        drone_set(&g_d, p, drone_params[p].lo);
    }
    drone_set_flag(&g_d, DF_FREEZE, 0);
    render_seconds(&g_d, 5.0, NULL, NULL, 0, NULL); /* the level fades */
    stats_t st0 = {0};
    render_seconds(&g_d, 10.0, &st0, NULL, 0, NULL);
    CHECK(st0.nonfinite == 0 && st0.peak < 1e-6f, "level 0: silent (peak %g)", (double)st0.peak);

    /* mute fades out, and back in */
    drone_init(&g_d, FS, 3);
    render_seconds(&g_d, 2.0, NULL, NULL, 0, NULL);
    drone_set_flag(&g_d, DF_MUTE, 1);
    render_seconds(&g_d, 4.0, NULL, NULL, 0, NULL);
    stats_t sm = {0};
    render_seconds(&g_d, 1.0, &sm, NULL, 0, NULL);
    CHECK(sm.peak < 1e-3f, "muted: quiet (peak %g)", (double)sm.peak);
    drone_set_flag(&g_d, DF_MUTE, 0);
    render_seconds(&g_d, 3.0, NULL, NULL, 0, NULL);
    stats_t su = {0};
    render_seconds(&g_d, 1.0, &su, NULL, 0, NULL);
    CHECK(su.peak > 0.05f, "unmuted: back (peak %g)", (double)su.peak);
}

/* Goertzel power at f Hz over x[0..n). */
static double power_at(const float *x, long n, double f)
{
    double w = 2.0 * M_PI * f / FS, c = 2.0 * cos(w), s1 = 0, s2 = 0;
    for (long i = 0; i < n; i++) {
        double hann = 0.5 - 0.5 * cos(2.0 * M_PI * (double)i / (double)(n - 1));
        double s0 = x[i] * hann + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}

/* A plain patch (unison, no detune/drift/evolve, saw, filter open, no
 * sub/air/space) at A2: the strongest component between 40 and 400 Hz is
 * 110 Hz; at fifths, 110 and 165 Hz (the fifth) are both there; the sub
 * adds 55 Hz. */
static void test_pitch(void)
{
    static float x[FS * 2];
    long kept;
    drone_init(&g_d, FS, 5);
    int plain[][2] = {{DP_ROOT, 45},       {DP_CHORD, 0},       {DP_DETUNE, 0},  {DP_MORPH, 127},
                      {DP_CUTOFF, 127},    {DP_RESO, 0},        {DP_MODE, 0},    {DP_DRIVE, 0},
                      {DP_DRIFT_DEPTH, 0}, {DP_EVOLVE, 0},      {DP_SUB, 0},     {DP_AIR, 0},
                      {DP_SPACE, 0},       {DP_LEVEL, 100}};
    for (size_t i = 0; i < sizeof plain / sizeof plain[0]; i++) {
        drone_set(&g_d, plain[i][0], plain[i][1]);
    }
    render_seconds(&g_d, 2.0, NULL, NULL, 0, NULL); /* glide, filter settle */
    kept = 0;
    render_seconds(&g_d, 1.0, NULL, x, FS, &kept);
    double best_f = 0, best_p = 0;
    for (double f = 40; f <= 400; f += 1.0) {
        double p = power_at(x, kept, f);
        if (p > best_p) {
            best_p = p;
            best_f = f;
        }
    }
    printf("A2 unison saw: strongest 40-400 Hz component at %.0f Hz\n", best_f);
    CHECK(fabs(best_f - 110.0) <= 1.0, "fundamental at 110 Hz (got %.0f)", best_f);
    double p110 = power_at(x, kept, 110), p100 = power_at(x, kept, 100), p220 = power_at(x, kept, 220);
    CHECK(p110 > 100 * p100, "110 Hz stands out of the noise floor (%.3g vs %.3g)", p110, p100);
    /* (the six unison voices start at random phases, so how their
     * harmonics add up varies: only "there, and below the fundamental") */
    CHECK(p220 > 0.01 * p110 && p220 < p110, "saw: a 2nd harmonic (%.3g of %.3g)", p220, p110);

    drone_set(&g_d, DP_CHORD, 2); /* fifths: 0 7 12 19 0 7 */
    drone_set(&g_d, DP_SUB, 127);
    render_seconds(&g_d, 2.0, NULL, NULL, 0, NULL);
    kept = 0;
    render_seconds(&g_d, 1.0, NULL, x, FS, &kept);
    double p55 = power_at(x, kept, 55), p165 = power_at(x, kept, 164.81), p60 = power_at(x, kept, 60);
    p110 = power_at(x, kept, 110);
    CHECK(p55 > 100 * p60, "sub at 55 Hz (%.3g vs %.3g at 60)", p55, p60);
    CHECK(p165 > 100 * power_at(x, kept, 150), "the fifth at 164.8 Hz (%.3g)", p165);
    CHECK(p110 > 100 * p100, "the root still there");

    /* a MIDI note moves the root: E3 (52) -> 164.8 Hz at unison */
    drone_set(&g_d, DP_CHORD, 0);
    drone_set(&g_d, DP_SUB, 0);
    drone_note(&g_d, 52, 100);
    CHECK(drone_get(&g_d, DP_ROOT) == 52, "note sets the root");
    render_seconds(&g_d, 4.0, NULL, NULL, 0, NULL);
    kept = 0;
    render_seconds(&g_d, 1.0, NULL, x, FS, &kept);
    best_f = 0;
    best_p = 0;
    for (double f = 40; f <= 400; f += 0.5) {
        double p = power_at(x, kept, f);
        if (p > best_p) {
            best_p = p;
            best_f = f;
        }
    }
    CHECK(fabs(best_f - 164.8) <= 1.0, "after note 52: %.1f Hz", best_f);
    drone_note(&g_d, 4, 0); /* far below the range: folded up by octaves */
    CHECK(drone_get(&g_d, DP_ROOT) >= DRONE_ROOT_MIN && drone_get(&g_d, DP_ROOT) % 12 == 4, "folded root %d",
          drone_get(&g_d, DP_ROOT));
}

/* exp2 accuracy and the table helpers. */
static void test_math(void)
{
    double worst = 0;
    for (float x = -20.0f; x <= 20.0f; x += 0.01f) {
        double e = fabs(drone_exp2(x) / exp2(x) - 1.0);
        if (e > worst) {
            worst = e;
        }
    }
    CHECK(worst < 3e-5, "exp2 within 3e-5 (%.2g)", worst);
    CHECK(fabsf(drone_note_hz(69.0f) - 440.0f) < 0.01f, "A4");
    CHECK(fabsf(drone_cutoff_hz(0) - 30.0f) < 0.5f && fabsf(drone_cutoff_hz(127) - 12000.0f) < 100.0f,
          "cutoff range %.1f .. %.0f", (double)drone_cutoff_hz(0), (double)drone_cutoff_hz(127));
    drone_init(&g_d, FS, 1);
    drone_set(&g_d, DP_ROOT, 200);
    CHECK(drone_get(&g_d, DP_ROOT) == DRONE_ROOT_MAX, "clamped root");
    drone_set(&g_d, DP_CHORD, -3);
    CHECK(drone_get(&g_d, DP_CHORD) == 0, "clamped chord");
    drone_set(&g_d, 99, 5); /* ignored */
}

/* The scope: armed, it fills once and says so. */
static void test_scope(void)
{
    drone_init(&g_d, FS, 1);
    render_seconds(&g_d, 0.5, NULL, NULL, 0, NULL);
    drone_scope_arm(&g_d, 4);
    float buf[BLOCK];
    int blocks = 0;
    while (!drone_scope_ready(&g_d) && blocks < 1000) {
        drone_render(&g_d, buf, BLOCK);
        blocks++;
    }
    CHECK(drone_scope_ready(&g_d), "scope ready after %d blocks", blocks);
    const int8_t *s = drone_scope_data(&g_d);
    int lo = 127, hi = -128;
    for (int i = 0; i < DRONE_SCOPE_N; i++) {
        lo = s[i] < lo ? s[i] : lo;
        hi = s[i] > hi ? s[i] : hi;
    }
    CHECK(hi - lo > 10, "scope shows a waveform (%d..%d)", lo, hi);
    CHECK(s[0] >= -2, "starts at a rising zero crossing (%d)", s[0]);
}

/* The save schedule: simulate a loop pass every ms and count attempts. */
static int save_attempts(drone_save_t *s, uint32_t from, uint32_t to, uint32_t shift_from, uint32_t shift_to,
                         int ok, int *leaving)
{
    int n = 0;
    for (uint32_t t = from; t < to; t++) {
        uint32_t held = t >= shift_from && t < shift_to ? t - shift_from + 1 : 0;
        drone_save_do_t d = drone_save_poll(s, t, held);
        if (d != DRONE_SAVE_NONE) {
            n++;
            if (d == DRONE_SAVE_LEAVING && leaving) {
                (*leaving)++;
            }
            drone_save_result(s, t, ok);
        }
    }
    return n;
}

static void test_save_schedule(void)
{
    drone_save_t s = {0};
    int lv = 0;
    CHECK(save_attempts(&s, 0, 60000, ~0u, ~0u, 1, NULL) == 0, "nothing changed: no saves");

    /* a change: one save 2 s later, then nothing */
    drone_save_changed(&s, 1000);
    CHECK(drone_save_poll(&s, 2999, 0) == DRONE_SAVE_NONE, "not before 2 s quiet");
    CHECK(save_attempts(&s, 1000, 10000, ~0u, ~0u, 1, NULL) == 1, "one save");
    CHECK(s.try_ms == 3000, "at 3000 (%u)", (unsigned)s.try_ms);

    /* changes every 100 ms for a minute: at most one save per 20 s */
    int n = 0;
    for (uint32_t t = 10000; t < 70000; t++) {
        if (t % 100 == 0) {
            drone_save_changed(&s, t);
        }
        drone_save_do_t d = drone_save_poll(&s, t, 0);
        if (d) {
            n++;
            drone_save_result(&s, t, 1);
        }
    }
    CHECK(n == 0, "never 2 s quiet: no saves (%d)", n);
    CHECK(save_attempts(&s, 70000, 100000, ~0u, ~0u, 1, NULL) == 1, "quiet again: one save");

    /* a write that keeps failing (E16_EBUSY): one attempt per 5 s, not one per pass */
    drone_save_t f = {0};
    drone_save_changed(&f, 0);
    n = save_attempts(&f, 0, 30000, ~0u, ~0u, 0, NULL);
    CHECK(n == 6, "failing first save: retried every 5 s (%d attempts in 30 s)", n);
    CHECK(f.dirty, "still dirty");
    /* then it works: done */
    CHECK(save_attempts(&f, 30000, 60000, ~0u, ~0u, 1, NULL) == 1 && !f.dirty, "then saved once");

    /* Shift held: one attempt per press, even failing, even inside the interval */
    drone_save_t h = {0};
    drone_save_changed(&h, 0);
    save_attempts(&h, 0, 2500, ~0u, ~0u, 1, NULL); /* saved at 2000 */
    drone_save_changed(&h, 3000);
    lv = 0;
    n = save_attempts(&h, 3000, 5000, 3100, 4600, 0, &lv);
    CHECK(n == 1 && lv == 1, "held 1.5 s, failing: one leaving attempt (%d, %d)", n, lv);
    CHECK(h.try_ms == 3100 + DRONE_SAVE_SHIFT_MS - 1, "after 0.4 s held (%u)", (unsigned)h.try_ms);
    lv = 0;
    n = save_attempts(&h, 5000, 5600, 5000, 5600, 1, &lv);
    CHECK(n == 1 && lv == 1 && !h.dirty, "a new press: another attempt, which works (%d, %d)", n, lv);
    lv = 0;
    n = save_attempts(&h, 6000, 7000, 6000, 7000, 1, &lv);
    CHECK(n == 0, "nothing to save: no attempt on Shift (%d)", n);
    CHECK(save_attempts(&h, 0, 300, 0, 300, 1, NULL) == 0, "a short press: none");
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* ns per sample, the default patch and a decaying, frozen-then-silent one
 * (the denormal trap: no slower). */
static void test_timing(void)
{
    float buf[BLOCK];
    drone_init(&g_d, FS, 9);
    long blocks = 20L * FS / BLOCK;
    double t0 = now_s();
    for (long b = 0; b < blocks; b++) {
        drone_render(&g_d, buf, BLOCK);
    }
    double ns = (now_s() - t0) * 1e9 / (double)(blocks * BLOCK);

    drone_set(&g_d, DP_LEVEL, 0);
    drone_set(&g_d, DP_SUB, 0);
    drone_set(&g_d, DP_AIR, 0);
    drone_set_flag(&g_d, DF_MUTE, 1);
    render_seconds(&g_d, 60.0, NULL, NULL, 0, NULL); /* everything decays */
    t0 = now_s();
    for (long b = 0; b < blocks; b++) {
        drone_render(&g_d, buf, BLOCK);
    }
    double ns_quiet = (now_s() - t0) * 1e9 / (double)(blocks * BLOCK);
    printf("timing: %.1f ns/sample (default patch), %.1f ns/sample after 60 s of silence; "
           "%.2f%% of one host core at 48 kHz\n",
           ns, ns_quiet, ns * 48000.0 / 1e7);
    CHECK(ns_quiet < ns * 2.0 + 20.0, "no denormal slowdown (%.1f vs %.1f ns)", ns_quiet, ns);
}

static void put_le(FILE *f, uint32_t v, int bytes)
{
    for (int i = 0; i < bytes; i++) {
        fputc((int)(v >> (8 * i)) & 0xFF, f);
    }
}

/* The demo: the default patch, a few slow macro moves, a MIDI note. */
static int write_demo(const char *path)
{
    const long secs = 10, n = secs * FS;
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return 1;
    }
    fwrite("RIFF", 1, 4, f);
    put_le(f, (uint32_t)(36 + n * 2), 4);
    fwrite("WAVEfmt ", 1, 8, f);
    put_le(f, 16, 4);
    put_le(f, 1, 2);
    put_le(f, 1, 2);
    put_le(f, FS, 4);
    put_le(f, FS * 2, 4);
    put_le(f, 2, 2);
    put_le(f, 16, 2);
    fwrite("data", 1, 4, f);
    put_le(f, (uint32_t)(n * 2), 4);

    drone_init(&g_d, FS, 2026);
    drone_set(&g_d, DP_ROOT, 38);  /* D2 */
    drone_set(&g_d, DP_CHORD, 8);  /* minor 7 */
    drone_set(&g_d, DP_EVOLVE, 80);
    drone_set(&g_d, DP_CUTOFF, 50);
    float buf[BLOCK];
    stats_t st = {0};
    for (long b = 0; b < n / BLOCK; b++) {
        double t = (double)(b * BLOCK) / FS;
        /* cutoff opens over the first 5 s, closes after; resonance up */
        int cut = t < 5.0 ? 50 + (int)(t * 12.0) : 110 - (int)((t - 5.0) * 10.0);
        drone_set(&g_d, DP_CUTOFF, cut);
        drone_set(&g_d, DP_RESO, 40 + (int)(t * 5.0));
        if (b == (long)(6.0 * FS / BLOCK)) {
            drone_note(&g_d, 33, 110); /* down to A1 with a swell */
            drone_set(&g_d, DP_CHORD, 2);
        }
        drone_render(&g_d, buf, BLOCK);
        account(&st, buf, BLOCK);
        for (int i = 0; i < BLOCK; i++) {
            int v = (int)lrintf(buf[i] * 32767.0f);
            put_le(f, (uint32_t)(uint16_t)(int16_t)v, 2);
        }
    }
    fclose(f);
    printf("wrote %s: %ld s, peak %.1f dBFS, RMS %.1f dBFS\n", path, secs, dbfs(st.peak),
           dbfs(sqrt(st.sum2 / (double)st.n)));
    return 0;
}

int main(int argc, char **argv)
{
    test_math();
    test_save_schedule();
    test_default();
    test_pitch();
    test_scope();
    test_extremes();
    test_fuzz();
    test_timing();
    if (argc == 3 && strcmp(argv[1], "--wav") == 0 && write_demo(argv[2]) != 0) {
        g_failures++;
    }
    if (g_failures) {
        printf("%d FAILED\n", g_failures);
        return 1;
    }
    printf("ALL PASSED\n");
    return 0;
}
