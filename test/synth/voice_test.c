/*
 * Host test for src/synth/voice.c (P3-2). Plain C, run directly by
 * test/run_all.sh; prints "ALL PASSED" and exits 0 on success.
 *
 *   voice_test                 run the tests and the timing report
 *   voice_test --wav out.wav   also render the acid demo line to out.wav
 *                              (32 kHz, 16-bit mono) and print the spectral
 *                              centroid along its cutoff sweep
 *
 * docs/audio/acid-demo.wav is made with --wav.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "voice.h"

#define FS 32000

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

/* a voice with the filter wide open and no envelope sweep */
static void open_voice(voice_t *v, int square)
{
    voice_init(v, (float)FS);
    voice_param(v, VOICE_P_CUTOFF, 127);
    voice_param(v, VOICE_P_RESONANCE, 0);
    voice_param(v, VOICE_P_ENV_MOD, 0);
    voice_param(v, VOICE_P_DECAY, 127);
    voice_param(v, VOICE_P_VOLUME, 127);
    voice_param(v, VOICE_P_WAVEFORM, square ? 127 : 0);
}

/* Rising zero crossings in buf[from, to), interpolated; returns the count
 * and fills times[] (in samples). */
static int rising_crossings(const float *buf, int from, int to, double *times, int max)
{
    int n = 0;
    for (int i = from + 1; i < to && n < max; i++) {
        if (buf[i - 1] < 0.0f && buf[i] >= 0.0f) {
            double frac = buf[i - 1] / (double)(buf[i - 1] - buf[i]);
            times[n++] = (i - 1) + frac;
        }
    }
    return n;
}

static double rms(const float *buf, int from, int to)
{
    double acc = 0;
    for (int i = from; i < to; i++)
        acc += (double)buf[i] * buf[i];
    return sqrt(acc / (to - from));
}

/* ---- tests ---- */

static void test_square_period(void)
{
    static float buf[FS];
    static double t[2000];
    voice_t v;
    open_voice(&v, 1);
    voice_note_on(&v, 69, 80);
    voice_render(&v, buf, FS);
    int n = rising_crossings(buf, FS / 10, FS, t, 2000);
    CHECK(n > 100, "square: only %d crossings", n);
    double period = (t[n - 1] - t[0]) / (n - 1);
    double expect = (double)FS / 440.0;
    double err = fabs(period - expect) / expect;
    printf("square 440 Hz: period %.3f samples (expect %.3f), error %.4f%%\n", period, expect, err * 100);
    CHECK(err < 0.01, "square period %.3f vs %.3f", period, expect);
    /* every single period, not just the average */
    for (int i = 1; i < n; i++) {
        double p = t[i] - t[i - 1];
        if (fabs(p - expect) / expect > 0.01) {
            CHECK(0, "square: period %d = %.3f", i, p);
            break;
        }
    }
}

static void test_cutoff_rms(void)
{
    static float buf[FS / 2];
    double prev = 1e9;
    printf("saw 110 Hz RMS vs cutoff:");
    static const int cuts[] = {127, 112, 96, 80, 64, 48, 32, 16, 0};
    for (unsigned ci = 0; ci < sizeof cuts / sizeof cuts[0]; ci++) {
        int c = cuts[ci];
        voice_t v;
        open_voice(&v, 0);
        voice_param(&v, VOICE_P_CUTOFF, (uint8_t)c);
        v.cut_oct = v.cut_oct_target; /* no smoothing ramp from the default */
        voice_note_on(&v, 45, 80);
        voice_render(&v, buf, FS / 2);
        double r = rms(buf, FS / 10, FS * 4 / 10);
        printf(" %d:%.4f", c, r);
        CHECK(r < prev, "RMS not decreasing at cutoff %d (%.5f >= %.5f)", c, r, prev);
        prev = r;
    }
    printf("\n");
    CHECK(prev < 0.05, "cutoff 0 still loud: %.4f", prev);
}

static uint32_t rng_state = 12345;
static uint32_t rng(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

static void test_fuzz_max_resonance(void)
{
    static float buf[256];
    voice_t v;
    voice_init(&v, (float)FS);
    voice_param(&v, VOICE_P_RESONANCE, 127);
    voice_param(&v, VOICE_P_VOLUME, 127);
    long total = 10L * FS, done = 0;
    int bad = 0;
    double sum2 = 0;
    while (done < total) {
        /* random event */
        uint32_t r = rng();
        switch (r % 6) {
        case 0:
        case 1:
            voice_note_on(&v, (uint8_t)(24 + rng() % 72), (uint8_t)(1 + rng() % 127));
            break;
        case 2:
            voice_note_off(&v, (uint8_t)(24 + rng() % 72));
            break;
        default: {
            voice_param_id_t id = (voice_param_id_t)(rng() % VOICE_P_COUNT);
            if (id == VOICE_P_RESONANCE)
                id = VOICE_P_CUTOFF; /* keep resonance at max */
            uint8_t val = (uint8_t)(rng() % 128);
            if (id == VOICE_P_VOLUME)
                val = (uint8_t)(100 + val % 28);
            voice_param(&v, id, val);
        }
        }
        int len = 1 + (int)(rng() % 256);
        if (done + len > total)
            len = (int)(total - done);
        voice_render(&v, buf, len);
        for (int i = 0; i < len; i++) {
            float x = buf[i];
            if (!isfinite(x) || x > 1.0f || x < -1.0f)
                bad++;
            sum2 += (double)x * x;
        }
        done += len;
    }
    printf("fuzz 10 s at max resonance: %d bad samples, rms %.3f, pre-clip peak %.3f\n", bad,
           sqrt(sum2 / total), v.peak_preclip);
    CHECK(bad == 0, "%d samples NaN/Inf/out of range", bad);
    CHECK(isfinite(v.peak_preclip) && v.peak_preclip < 2.5f, "pre-clip peak %.3f", v.peak_preclip);
    CHECK(sqrt(sum2 / total) > 0.01, "fuzz produced (almost) no sound");
    CHECK(isfinite(v.s[0]) && isfinite(v.s[3]), "filter state not finite");
}

/* Measure where a slide from A2 to A3 lands, from the audio. */
static void test_slide(void)
{
    enum { LEN = FS };
    static float buf[LEN];
    static double t[2000];
    voice_t v;
    open_voice(&v, 0);
    voice_param(&v, VOICE_P_SLIDE, 127);
    voice_param(&v, VOICE_P_GLIDE_TIME, 80);
    double glide = voice_glide_time_s(&v);
    int start = FS / 5;
    voice_note_on(&v, 45, 80); /* 110 Hz */
    voice_render(&v, buf, start);
    float fenv_before = v.fenv_lvl;
    voice_note_on(&v, 57, 80); /* 220 Hz, legato */
    voice_note_off(&v, 45);
    CHECK(v.fenv_lvl == fenv_before, "slide retriggered the filter envelope");
    voice_render(&v, buf + start, LEN - start);

    int n = rising_crossings(buf, start, LEN, t, 2000);
    /* first period that is within 1% of 220 Hz and stays there */
    double target = (double)FS / 220.0, reached = -1;
    for (int i = n - 1; i > 0; i--) {
        double p = t[i] - t[i - 1];
        if (fabs(p - target) / target > 0.01)
            break;
        reached = t[i - 1]; /* period [t[i-1], t[i]] is on pitch */
    }
    double secs = (reached - start) / FS;
    printf("slide 110 -> 220 Hz: on pitch after %.1f ms, glide time %.1f ms (%.1f%%)\n", secs * 1000,
           glide * 1000, 100 * secs / glide);
    CHECK(reached > 0, "slide never reached the target pitch");
    CHECK(secs >= 0.9 * glide && secs <= 1.1 * glide, "slide took %.4f s, glide %.4f s", secs, glide);

    /* without slide the second note retriggers */
    open_voice(&v, 0);
    voice_note_on(&v, 45, 80);
    voice_render(&v, buf, start);
    voice_note_on(&v, 57, 80);
    CHECK(v.fenv_lvl == 1.0f, "a non-slide overlap didn't retrigger");
}

/* Note-off fades in ~10 ms, without a step. */
static void test_release(void)
{
    static float buf[FS / 2];
    voice_t v;
    open_voice(&v, 0);
    voice_note_on(&v, 45, 80);
    voice_render(&v, buf, FS / 4);
    double before = rms(buf, FS / 4 - 640, FS / 4);
    float last = buf[FS / 4 - 1];
    voice_note_off(&v, 45);
    voice_render(&v, buf, FS / 4);
    float maxstep = fabsf(buf[0] - last);
    for (int i = 1; i < 32; i++)
        if (fabsf(buf[i] - buf[i - 1]) > maxstep)
            maxstep = fabsf(buf[i] - buf[i - 1]);
    int idle_at = -1;
    for (int i = 0; i < FS / 4; i++)
        if (!voice_active(&v) && buf[i] == 0.0f) {
            idle_at = i;
            break;
        }
    double after = rms(buf, (int)(0.012 * FS), (int)(0.015 * FS));
    printf("release: rms %.4f -> %.6f 12-15 ms after note-off\n", before, after);
    CHECK(after < before * 0.002, "release too slow: %.6f", after);
    CHECK(!voice_active(&v), "voice still active after release");
    (void)idle_at;
    /* the first samples after note-off move no more than the saw itself does */
    CHECK(maxstep < 0.2f, "release step %.3f", maxstep);
}

/* Accent: louder and brighter than a normal note. */
static void test_accent(void)
{
    static float a[FS / 4], b[FS / 4];
    voice_t v;
    voice_init(&v, (float)FS);
    voice_note_on(&v, 45, 80);
    voice_render(&v, a, FS / 4);
    voice_init(&v, (float)FS);
    voice_note_on(&v, 45, 120);
    voice_render(&v, b, FS / 4);
    double ra = rms(a, 0, FS / 20), rb = rms(b, 0, FS / 20);
    printf("accent: first 50 ms rms %.4f -> %.4f\n", ra, rb);
    CHECK(rb > ra * 1.2, "accent not louder (%.4f vs %.4f)", rb, ra);
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static volatile float sink;

static void timing_report(void)
{
    enum { N = 32768, REPS = 50 };
    static float buf[N];
    voice_t v;
    voice_init(&v, (float)FS);
    voice_param(&v, VOICE_P_RESONANCE, 120);
    voice_param(&v, VOICE_P_WAVEFORM, 127);
    voice_param(&v, VOICE_P_SLIDE, 127);
    voice_param(&v, VOICE_P_GLIDE_TIME, 127);
    voice_param(&v, VOICE_P_DECAY, 127);
    double best = 1e9;
    for (int r = 0; r < REPS; r++) {
        voice_note_on(&v, (uint8_t)(36 + r % 24), 120); /* slides: glide path active */
        double t0 = now_s();
        voice_render(&v, buf, N);
        double t1 = now_s();
        sink += buf[N / 2];
        if (t1 - t0 < best)
            best = t1 - t0;
    }
    /* per-sample render, the way a 1-sample-per-interrupt ISR would call it */
    double best1 = 1e9;
    for (int r = 0; r < 10; r++) {
        voice_note_on(&v, (uint8_t)(36 + r % 24), 120);
        double t0 = now_s();
        for (int i = 0; i < N; i++)
            voice_render(&v, &buf[i], 1);
        double t1 = now_s();
        sink += buf[N / 2];
        if (t1 - t0 < best1)
            best1 = t1 - t0;
    }
    printf("timing: %.2f ns/sample in blocks of %d, %.2f ns/sample one at a time (host, best of runs)\n",
           best * 1e9 / N, N, best1 * 1e9 / N);
}

/* ---- acid demo ---- */

static void put_le(FILE *f, uint32_t x, int bytes)
{
    for (int i = 0; i < bytes; i++)
        fputc((int)((x >> (8 * i)) & 0xff), f);
}

static int write_wav(const char *path, const float *buf, int n)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    fwrite("RIFF", 1, 4, f);
    put_le(f, 36u + 2u * (uint32_t)n, 4);
    fwrite("WAVEfmt ", 1, 8, f);
    put_le(f, 16, 4);
    put_le(f, 1, 2); /* PCM */
    put_le(f, 1, 2); /* mono */
    put_le(f, FS, 4);
    put_le(f, FS * 2, 4);
    put_le(f, 2, 2);
    put_le(f, 16, 2);
    fwrite("data", 1, 4, f);
    put_le(f, 2u * (uint32_t)n, 4);
    for (int i = 0; i < n; i++) {
        float x = buf[i];
        if (x > 1.0f) x = 1.0f;
        if (x < -1.0f) x = -1.0f;
        put_le(f, (uint32_t)(uint16_t)(int16_t)lrintf(x * 32767.0f), 2);
    }
    return fclose(f);
}

/* Spectral centroid (Hz) of a Hann-windowed 1024-sample frame, by a plain DFT. */
static double centroid(const float *x)
{
    enum { M = 1024 };
    double num = 0, den = 0;
    for (int k = 1; k < M / 2; k++) {
        double re = 0, im = 0;
        for (int i = 0; i < M; i++) {
            double w = 0.5 - 0.5 * cos(2 * M_PI * i / M);
            re += w * x[i] * cos(2 * M_PI * k * i / M);
            im -= w * x[i] * sin(2 * M_PI * k * i / M);
        }
        double mag = sqrt(re * re + im * im);
        num += mag * k * (double)FS / M;
        den += mag;
    }
    return den > 0 ? num / den : 0;
}

struct step { int note; int accent; int slide; int rest; };

static void acid_demo(const char *path)
{
    /* 16 steps in A minor, 132 BPM sixteenths; slide = tie into the next step */
    static const struct step pat[16] = {
        {33, 1, 0, 0}, {33, 0, 0, 0}, {45, 0, 1, 0}, {43, 0, 0, 0},
        {33, 0, 0, 0}, {0, 0, 0, 1},  {36, 1, 0, 0}, {33, 0, 1, 0},
        {45, 1, 0, 0}, {33, 0, 0, 0}, {40, 0, 1, 0}, {43, 0, 1, 0},
        {45, 1, 0, 0}, {33, 0, 0, 0}, {48, 0, 0, 0}, {46, 1, 1, 0},
    };
    const int bars = 4;
    const int step_len = (int)(FS * 60.0 / 132.0 / 4.0 + 0.5);
    const int total = bars * 16 * step_len + FS / 2; /* plus a tail */
    float *buf = calloc((size_t)total, sizeof(float));
    if (!buf) {
        CHECK(0, "out of memory");
        return;
    }
    voice_t v;
    voice_init(&v, (float)FS);
    voice_param(&v, VOICE_P_RESONANCE, 110);
    voice_param(&v, VOICE_P_ENV_MOD, 70);
    voice_param(&v, VOICE_P_DECAY, 40);
    voice_param(&v, VOICE_P_SLIDE, 127);
    voice_param(&v, VOICE_P_VOLUME, 120);

    int pos = 0, sounding = -1;
    for (int b = 0; b < bars; b++) {
        if (b == 2)
            voice_param(&v, VOICE_P_WAVEFORM, 127); /* square for the second half */
        for (int s = 0; s < 16; s++) {
            const struct step *st = &pat[s];
            /* cutoff sweep: up over the first three bars, back down in the last */
            double prog = (b * 16 + s) / 64.0;
            int cut = prog < 0.75 ? (int)(10 + prog / 0.75 * 90) : (int)(100 - (prog - 0.75) / 0.25 * 80);
            voice_param(&v, VOICE_P_CUTOFF, (uint8_t)cut);
            if (!st->rest) {
                voice_note_on(&v, (uint8_t)st->note, st->accent ? 120 : 80);
                if (sounding >= 0 && sounding != st->note)
                    voice_note_off(&v, (uint8_t)sounding); /* the tie from a slide step */
                sounding = st->note;
            }
            int gate = st->slide && !st->rest ? step_len : step_len / 2;
            voice_render(&v, buf + pos, gate);
            if (!st->slide && sounding >= 0) {
                voice_note_off(&v, (uint8_t)sounding);
                sounding = -1;
            }
            voice_render(&v, buf + pos + gate, step_len - gate);
            pos += step_len;
        }
    }
    if (sounding >= 0)
        voice_note_off(&v, (uint8_t)sounding);
    voice_render(&v, buf + pos, total - pos);

    printf("acid demo: %.2f s, pre-clip peak %.3f\n", (double)total / FS, v.peak_preclip);
    printf("spectral centroid per half-bar (cutoff sweeps up 3 bars, down 1):\n ");
    for (int h = 0; h < bars * 2; h++) {
        /* average over the half-bar's eight steps, frames a step apart */
        double c = 0;
        for (int s = 0; s < 8; s++)
            c += centroid(buf + (h * 8 + s) * step_len);
        printf(" %.0f", c / 8);
    }
    printf(" Hz\n");
    CHECK(write_wav(path, buf, total) == 0, "can't write %s", path);
    printf("wrote %s\n", path);
    free(buf);
}

int main(int argc, char **argv)
{
    const char *wav = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--wav") && i + 1 < argc)
            wav = argv[++i];
        else {
            fprintf(stderr, "usage: %s [--wav out.wav]\n", argv[0]);
            return 2;
        }
    }
    test_square_period();
    test_cutoff_rms();
    test_fuzz_max_resonance();
    test_slide();
    test_release();
    test_accent();
    timing_report();
    if (wav)
        acid_demo(wav);
    if (g_failures) {
        printf("%d FAILED\n", g_failures);
        return 1;
    }
    printf("ALL PASSED\n");
    return 0;
}
