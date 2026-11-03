/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Host test for apps/x0x/core (x0x-lite: the 808 kit, the 303, the
 * sequencer, TB-3PO, the saved state). Plain C, run by test/run_all.sh;
 * prints "ALL PASSED" and exits 0 on success.
 *
 *   x0x_test                 the tests and a timing report
 *   x0x_test --wav out.wav   also renders a 10 s demo (48 kHz, 16-bit mono):
 *                            the factory loop, a cutoff sweep, a new line
 *
 * docs/audio/x0x-demo.wav is made with --wav.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tb3po.h"
#include "x0x.h"
#include "x0x_save.h"

#define FS 48000
#define BLOCK 32 /* the launcher's E16_AUDIO_BLOCK */

static int g_failures;
#define CHECK(cond, ...)                                        \
    do {                                                        \
        if (!(cond)) {                                          \
            g_failures++;                                       \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                       \
            fprintf(stderr, "\n");                              \
        }                                                       \
    } while (0)

static x0x_t g_x, g_y; /* not on the stack */
static uint32_t g_rng = 12345;

static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static int rndi(int n) { return (int)(rnd() % (uint32_t)n); }

/* main-flow side of a block: flush parameters, then render */
static void run_block(x0x_t *x, float *buf)
{
    x0x_flush(x);
    x0x_render(x, buf, BLOCK);
}

/* ---- bounds: a long random run ---- */

static void test_random_run(void)
{
    float buf[BLOCK];
    long nonfinite = 0, over = 0, blocks = (long)(120.0 * FS / BLOCK);
    float peak = 0.0f;
    x0x_init(&g_x);
    seq_start(&g_x.seq);
    for (long b = 0; b < blocks; b++) {
        if (b % 50 == 0) { /* every ~33 ms: something changes */
            int what = rndi(10);
            pattern_t *p = &g_x.pat[g_x.seq.ppat];
            if (what < 3) {
                int t = rndi(X_NTARGETS), n = x0x_nparams(t);
                if (n)
                    x0x_set(&g_x, t, rndi(n), rndi(128));
            } else if (what < 5) {
                pattern_drum_nudge(p, rndi(X_NDRUM), rndi(X_NSTEPS), rndi(31) - 15);
            } else if (what == 5) {
                pattern_bass_nudge(p, rndi(X_NSTEPS), rndi(25) - 12, 12, 96);
                pattern_bass_flags(p, rndi(X_NSTEPS), rndi(3) - 1);
            } else if (what == 6) {
                if (rndi(2))
                    x0x_generate(&g_x, rnd());
                else
                    x0x_mutate(&g_x);
            } else if (what == 7) {
                g_x.seq.bpm = 40.0f + (float)rndi(260);
                g_x.seq.swing = (uint8_t)rndi(101);
            } else if (what == 8) {
                seq_cue(&g_x.seq, rndi(X_NPAT));
                p->drum_len = (uint8_t)(1 + rndi(16));
                p->bass_len = (uint8_t)(1 + rndi(16));
            } else {
                x0x_audition_drum(&g_x, rndi(X_NDRUM), (float)rndi(100) / 99.0f);
                if (rndi(10) == 0)
                    seq_stop(&g_x.seq);
                else if (!g_x.seq.playing)
                    seq_start(&g_x.seq);
            }
        }
        run_block(&g_x, buf);
        for (int i = 0; i < BLOCK; i++) {
            if (!isfinite(buf[i])) {
                nonfinite++;
                continue;
            }
            if (fabsf(buf[i]) > 1.0f)
                over++;
            if (fabsf(buf[i]) > peak)
                peak = fabsf(buf[i]);
        }
    }
    CHECK(nonfinite == 0, "random run: %ld non-finite samples", nonfinite);
    CHECK(over == 0, "random run: %ld samples outside [-1, 1]", over);
    CHECK(peak > 0.05f, "random run makes sound (peak %.3f)", peak);
    printf("random run: 120 s, peak %.3f, no NaN/Inf, within +-1\n", peak);
}

/* ---- the 303's pitch ---- */

/* fundamental by autocorrelation over lags lo..hi (samples), parabolic peak */
static double fundamental(const float *x, int n, int lo, int hi)
{
    double best = -1e30;
    int bl = lo;
    static double r[4096];
    for (int lag = lo; lag <= hi; lag++) {
        double s = 0;
        for (int i = 0; i + lag < n; i++)
            s += (double)x[i] * x[i + lag];
        r[lag - lo] = s;
        if (s > best) {
            best = s;
            bl = lag;
        }
    }
    double p = bl;
    if (bl > lo && bl < hi) {
        double a = r[bl - lo - 1], b = r[bl - lo], c = r[bl - lo + 1];
        double d = a - 2 * b + c;
        if (d != 0)
            p = bl + 0.5 * (a - c) / d;
    }
    return FS / p;
}

static void test_303_pitch(void)
{
    static bass303_t b;
    static float buf[FS];
    struct { int note; double hz; } cases[] = {{45, 110.0}, {33, 55.0}, {57, 220.0}};
    for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        bass303_init(&b);
        bass303_set(&b, BASS303_CUTOFF, 100);
        bass303_note_on(&b, cases[c].note, 0, 0);
        for (int i = 0; i < FS; i += BLOCK)
            bass303_render(&b, buf + i, BLOCK);
        /* the last half second, the note still held */
        double f = fundamental(buf + FS / 2, FS / 2 - 2000, (int)(FS / cases[c].hz * 0.8), (int)(FS / cases[c].hz * 1.25));
        CHECK(fabs(f / cases[c].hz - 1.0) < 0.005, "303 note %d: %.2f Hz, want %.2f", cases[c].note, f, cases[c].hz);
        printf("303 note %d held: fundamental %.2f Hz (want %.2f)\n", cases[c].note, f, cases[c].hz);
    }
}

/* ---- the kick: energy where the 808's is ---- */

static double goertzel(const float *x, int n, double hz)
{
    double w = 2 * M_PI * hz / FS, c = 2 * cos(w), s1 = 0, s2 = 0;
    for (int i = 0; i < n; i++) {
        double s0 = x[i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / n;
}

static void test_kick(void)
{
    static drum808_t d;
    static float buf[FS / 2];
    drum808_init(&d);
    memset(buf, 0, sizeof buf);
    drum808_trigger(&d, D8_BD, 1.0f);
    for (int i = 0; i < FS / 2; i += BLOCK)
        drum808_render(&d, buf + i, BLOCK);
    int pk_i = 0;
    float pk = 0;
    for (int i = 0; i < FS / 2; i++)
        if (fabsf(buf[i]) > pk) {
            pk = fabsf(buf[i]);
            pk_i = i;
        }
    /* the strongest partial over the first 200 ms, 30..400 Hz */
    double best = 0, best_hz = 0;
    for (double hz = 30; hz <= 400; hz += 1) {
        double m = goertzel(buf, FS / 5, hz);
        if (m > best) {
            best = m;
            best_hz = hz;
        }
    }
    double hi = 0;
    for (double hz = 1000; hz <= 8000; hz += 250)
        hi += goertzel(buf, FS / 5, hz);
    double e0 = 0, e1 = 0;
    for (int i = 0; i < FS / 10; i++)
        e0 += (double)buf[i] * buf[i];
    for (int i = FS / 5; i < FS / 5 + FS / 10; i++)
        e1 += (double)buf[i] * buf[i];
    /* 8W8's kick: note 34 (58.3 Hz) at the default tune, its pitch lifted a little by the attack */
    CHECK(best_hz > 45 && best_hz < 75, "kick's strongest partial %.0f Hz (want ~58)", best_hz);
    CHECK(best > 20 * hi, "kick's energy is low (%.3g at %.0f Hz, %.3g summed 1-8 kHz)", best, best_hz, hi);
    CHECK(pk_i < FS / 50, "kick peaks in its first 20 ms (sample %d)", pk_i);
    CHECK(pk > 0.05f && pk < 2.0f, "kick level %.3f", pk);
    CHECK(e1 < e0 * 0.5, "kick decays (%.3g then %.3g)", e0, e1);
    printf("kick: peak %.3f at %.1f ms, strongest partial %.0f Hz, low/high %.0f\n", pk, pk_i * 1000.0 / FS,
           best_hz, best / (hi > 0 ? hi : 1e-30));

    /* every voice sounds and ends */
    for (int t = 0; t < D8_NUM; t++) {
        drum808_init(&d);
        memset(buf, 0, sizeof buf);
        drum808_trigger(&d, t, D8_VEL_NORMAL);
        float p = 0;
        for (int i = 0; i < FS / 2; i += BLOCK)
            drum808_render(&d, buf + i, BLOCK);
        for (int i = 0; i < FS / 2; i++)
            p = fabsf(buf[i]) > p ? fabsf(buf[i]) : p;
        for (int k = 0; k < 20 * FS / BLOCK; k++) {
            float tmp[BLOCK] = {0};
            drum808_render(&d, tmp, BLOCK);
        }
        CHECK(p > 0.01f && p < 2.0f, "drum %d peak %.4f", t, p);
        CHECK(!drum808_active(&d), "drum %d still active after 20 s", t);
    }
}

/* ---- the sequencer ---- */

typedef struct {
    long now;
    long drum_at[256];
    int drum_v[256];
    int nd;
    long on_at[64], off_at[64];
    int on_note[64], on_slide[64], non, noff;
    int clocks, starts, stops;
} rec_t;

static int r_drum(void *c, int v, float vel)
{
    rec_t *r = c;
    (void)vel;
    if (r->nd < 256) {
        r->drum_at[r->nd] = r->now;
        r->drum_v[r->nd++] = v;
    }
    return 1;
}
static void r_on(void *c, int note, int acc, int slide)
{
    rec_t *r = c;
    (void)acc;
    if (r->non < 64) {
        r->on_at[r->non] = r->now;
        r->on_slide[r->non] = slide;
        r->on_note[r->non++] = note;
    }
}
static void r_off(void *c)
{
    rec_t *r = c;
    if (r->noff < 64)
        r->off_at[r->noff++] = r->now;
}
static void r_midi(void *c, uint8_t st)
{
    rec_t *r = c;
    if (st == 0xF8)
        r->clocks++;
    else if (st == 0xFA)
        r->starts++;
    else if (st == 0xFC)
        r->stops++;
}

/* run the sequencer alone for n samples, in blocks, as the engine does */
static void seq_run(seq_t *s, rec_t *r, long n, const seq_sink_t *o)
{
    long done = 0;
    while (done < n) {
        uint32_t left = (uint32_t)(n - done < BLOCK ? n - done : BLOCK);
        while (left) {
            uint32_t k = seq_until_event(s, left);
            r->now += k; /* events fire at the end of the k samples: on sample now */
            seq_advance(s, k, o);
            left -= k;
            done += k;
        }
    }
}

static void test_seq_timing(void)
{
    static pattern_t pat[X_NPAT];
    static seq_t s;
    static rec_t r;
    seq_sink_t o = {r_drum, r_on, r_off, r_midi, 0, &r};
    for (int i = 0; i < X_NPAT; i++)
        pattern_init(&pat[i], 1);
    for (int st = 0; st < X_NSTEPS; st++)
        pat[0].drum[0][st] = X_LVL_NORMAL;
    pat[0].bass[0].flags = G_NOTE;
    pat[0].bass[0].note = 40;

    /* 120 BPM, straight: a 16th is 6000 samples. A start is taken at the end of the block it
     * was asked in (the first step is on that block's last sample, 31); the steps follow on
     * the grid from there. */
    seq_init(&s, pat);
    s.bpm = 120.0f;
    memset(&r, 0, sizeof r);
    r.now = -1; /* the first advance (of k samples) ends on sample k - 1 */
    seq_start(&s);
    seq_run(&s, &r, 6000L * 32 + 40, &o);
    long t0 = r.drum_at[0];
    CHECK(r.nd == 33 && t0 == BLOCK - 1, "120 BPM: %d kicks in 32 steps (want 33), the first at %ld", r.nd, t0);
    int bad = 0;
    for (int i = 0; i < r.nd; i++)
        if (r.drum_at[i] - t0 != (long)i * 6000)
            bad++;
    CHECK(bad == 0, "120 BPM: steps off the 6000-sample grid (second at +%ld)", r.drum_at[1] - t0);
    CHECK(r.non == 3 && r.on_at[1] - t0 == 96000, "303: a note each bar (%d, the second at +%ld)", r.non, r.on_at[1] - t0);
    CHECK(r.noff >= 2 && r.off_at[0] - t0 == 3000, "303: the gate falls at half a step (+%ld)", r.off_at[0] - t0);
    CHECK(r.clocks == 8 * 24 + 1 && r.starts == 1, "clock out: %d clocks in 8 beats, %d starts", r.clocks, r.starts);
    printf("seq: 120 BPM 16ths on the 6000-sample grid, %d MIDI clocks in 8 beats\n", r.clocks);

    /* swing 50: odd 16ths 1500 samples late (6000 / 200 * 50), even ones on the grid */
    seq_init(&s, pat);
    s.bpm = 120.0f;
    s.swing = 50;
    s.send_clock = 0;
    memset(&r, 0, sizeof r);
    r.now = -1;
    seq_start(&s);
    seq_run(&s, &r, 6000L * 16, &o);
    bad = 0;
    t0 = r.drum_at[0];
    for (int i = 0; i < r.nd; i++) {
        long want = (long)i * 6000 + ((i & 1) ? 1500 : 0);
        if (labs(r.drum_at[i] - t0 - want) > 1)
            bad++;
    }
    CHECK(r.nd == 16 && bad == 0, "swing 50: %d steps, %d off (step 1 at +%ld, want 7500)", r.nd, bad, r.drum_at[1] - t0);
    printf("seq: swing 50 puts the odd 16ths %ld samples late\n", r.drum_at[1] - t0 - 6000);

    /* slide: the note after a sliding step glides (no retrigger of the gate) */
    pattern_init(&pat[1], 2);
    pat[1].bass[0].flags = G_NOTE | BS_SLIDE;
    pat[1].bass[0].note = 40;
    pat[1].bass[1].flags = G_NOTE;
    pat[1].bass[1].note = 47;
    seq_init(&s, pat);
    s.ppat = 1;
    s.bpm = 120.0f;
    memset(&r, 0, sizeof r);
    r.now = -1;
    seq_start(&s);
    seq_run(&s, &r, 6000L * 2 + 100, &o);
    CHECK(r.non == 2 && r.on_slide[0] == 0 && r.on_slide[1] == 1 && (r.noff == 0 || r.off_at[0] > r.on_at[1]),
          "slide: %d notes, slides %d %d, the gate held through", r.non, r.on_slide[0], r.on_slide[1]);

    /* a cue waits for the bar's end */
    seq_init(&s, pat);
    s.bpm = 120.0f;
    memset(&r, 0, sizeof r);
    r.now = -1;
    seq_start(&s);
    seq_run(&s, &r, 6000L * 5, &o);
    seq_cue(&s, 1);
    seq_run(&s, &r, 6000L * 10, &o);
    CHECK(s.ppat == 0, "cue: still pattern 1 mid-bar");
    seq_run(&s, &r, 6100L, &o);
    CHECK(s.ppat == 1 && s.t[SEQ_KIT].pos == 0, "cue: pattern 2 from the bar (ppat %d pos %d)", s.ppat, s.t[SEQ_KIT].pos);

    /* transpose */
    seq_init(&s, pat);
    s.bpm = 120.0f;
    s.xpose = 5;
    memset(&r, 0, sizeof r);
    seq_start(&s);
    seq_run(&s, &r, 100, &o);
    CHECK(r.non == 1 && r.on_note[0] == 45, "transpose +5: note %d", r.non ? r.on_note[0] : -1);

    /* mutes */
    seq_init(&s, pat);
    s.bpm = 120.0f;
    s.mute = 1u << 0 | 1u << SEQ_MUTE_BASS;
    memset(&r, 0, sizeof r);
    seq_start(&s);
    seq_run(&s, &r, 6000L * 4, &o);
    CHECK(r.nd == 0 && r.non == 0, "mutes: %d hits, %d notes", r.nd, r.non);

    /* stop / start */
    seq_init(&s, pat);
    memset(&r, 0, sizeof r);
    seq_start(&s);
    seq_run(&s, &r, 1000, &o);
    seq_stop(&s);
    seq_run(&s, &r, 100, &o);
    CHECK(!s.playing && r.stops == 1 && r.starts == 1, "stop: playing %d, starts %d stops %d", s.playing, r.starts, r.stops);
}

static void test_ext_clock(void)
{
    static pattern_t pat[X_NPAT];
    static seq_t s;
    static rec_t r;
    seq_sink_t o = {r_drum, r_on, r_off, r_midi, 0, &r};
    for (int i = 0; i < X_NPAT; i++)
        pattern_init(&pat[i], 1);
    for (int st = 0; st < X_NSTEPS; st++)
        pat[0].drum[0][st] = X_LVL_NORMAL;
    seq_init(&s, pat);
    s.bpm = 90.0f;
    memset(&r, 0, sizeof r);
    /* 100 BPM from outside: a tick every 1200 samples; start, then 24 * 8 ticks */
    seq_start(&s);
    seq_ext_clock(&s);
    seq_run(&s, &r, 1200, &o);
    for (int k = 1; k < 24 * 8; k++) {
        seq_ext_clock(&s);
        seq_run(&s, &r, 1200, &o);
    }
    CHECK(s.ext == 1, "ext clock: following");
    CHECK(fabsf(seq_tempo(&s) - 100.0f) < 0.5f, "ext clock: tempo %.2f, want 100", seq_tempo(&s));
    CHECK(r.nd == 32, "ext clock: %d steps in 8 beats (want 32)", r.nd);
    int bad = 0;
    for (int i = 1; i < r.nd; i++)
        if (r.drum_at[i] - r.drum_at[i - 1] != 7200)
            bad++;
    CHECK(bad == 0, "ext clock: steps every 7200 samples (%d off)", bad);
    /* the clock goes away: back to the internal tempo after 0.5 s */
    seq_run(&s, &r, FS, &o);
    CHECK(s.ext == 0 && s.playing, "ext clock gone: internal again (ext %d playing %d)", s.ext, s.playing);
    printf("ext clock: 100 BPM in, followed; back to the internal tempo 0.5 s after it stops\n");
}

/* ---- pattern edits ---- */

static void test_pattern_edits(void)
{
    static pattern_t p;
    pattern_init(&p, 7);
    CHECK(pattern_drum_toggle(&p, 1, 3) == X_LVL_NORMAL && p.drum[1][3] == X_LVL_NORMAL, "toggle on");
    CHECK(pattern_drum_toggle(&p, 1, 3) == 0, "toggle off");
    CHECK(pattern_drum_nudge(&p, 2, 0, 1) == X_LVL_NORMAL, "nudge up an off step: on at normal");
    CHECK(pattern_drum_nudge(&p, 2, 0, 100) == X_LVL_MAX, "nudge clamps at the accent");
    CHECK(pattern_drum_nudge(&p, 2, 0, -100) == 1, "nudge clamps at 1 (stays on)");
    CHECK(pattern_drum_toggle(&p, 9, 3) == 0 && pattern_drum_toggle(&p, 0, 16) == 0, "out-of-range edits ignored");
    CHECK(fabsf(pattern_level_vel(X_LVL_NORMAL) - D8_VEL_NORMAL) < 1e-6f && pattern_level_vel(X_LVL_MAX) == 1.0f &&
              pattern_level_vel(0) == 0.0f, "levels to velocity");
    p.bass[4].note = 40;
    pattern_bass_toggle(&p, 4);
    CHECK(bstep_gate(&p.bass[4]) == G_NOTE, "303 gate on");
    pattern_bass_nudge(&p, 4, 3, 24, 60);
    CHECK(p.bass[4].note == 43, "303 pitch +3");
    pattern_bass_nudge(&p, 4, 50, 24, 60);
    CHECK(p.bass[4].note == 60, "303 pitch clamps");
    pattern_bass_flags(&p, 4, 1);
    CHECK((p.bass[4].flags & (BS_ACCENT | BS_SLIDE)) == BS_ACCENT, "flags: accent");
    pattern_bass_flags(&p, 4, 1);
    CHECK((p.bass[4].flags & (BS_ACCENT | BS_SLIDE)) == BS_SLIDE, "flags: slide");
    pattern_bass_flags(&p, 4, 1);
    pattern_bass_flags(&p, 4, 1);
    CHECK((p.bass[4].flags & (BS_ACCENT | BS_SLIDE)) == (BS_ACCENT | BS_SLIDE) && bstep_gate(&p.bass[4]) == G_NOTE,
          "flags: both, gate kept");
    pattern_bass_flags(&p, 4, -1);
    CHECK((p.bass[4].flags & (BS_ACCENT | BS_SLIDE)) == BS_SLIDE, "flags: back");
    pattern_bass_toggle(&p, 4);
    CHECK(bstep_gate(&p.bass[4]) == G_REST && (p.bass[4].flags & BS_SLIDE), "303 gate off keeps the flags");
    pattern_bass_nudge(&p, 5, -1, 24, 60);
    CHECK(bstep_gate(&p.bass[5]) == G_NOTE, "turning an off 303 step turns it on");
    pattern_clear(&p);
    int any = 0;
    for (int t = 0; t < X_NDRUM; t++)
        for (int s = 0; s < X_NSTEPS; s++)
            any |= p.drum[t][s];
    for (int s = 0; s < X_NSTEPS; s++)
        any |= bstep_gate(&p.bass[s]);
    CHECK(!any && p.drum_len == 16, "clear");
    pattern_init(&p, 0x3B0u);
    pattern_factory(&p);
    CHECK(p.drum[0][0] == X_LVL_MAX && p.drum[0][4] == X_LVL_NORMAL && p.drum[3][4] && p.drum[5][2] && !p.drum[1][0],
          "factory loop drums");
    any = 0;
    for (int s = 0; s < X_NSTEPS; s++)
        any += bstep_gate(&p.bass[s]) != G_REST;
    CHECK(any >= 6, "factory loop: a 303 line (%d notes)", any);
}

/* ---- TB-3PO ---- */

static void test_tb3po(void)
{
    static pattern_t a, b;
    pattern_init(&a, 0xC0FFEEu);
    pattern_init(&b, 0xC0FFEEu);
    tb3po_generate(&a);
    tb3po_generate(&b);
    CHECK(memcmp(a.bass, b.bass, sizeof a.bass) == 0, "same seed, same line");
    b.gen.seed = 0xC0FFEFu;
    tb3po_generate(&b);
    CHECK(memcmp(a.bass, b.bass, sizeof a.bass) != 0, "another seed, another line");
    /* the reference line for this seed (A minor, density 70 %): a regression check */
    int notes = 0, acc = 0, sl = 0, lo = 127, hi = 0;
    for (int s = 0; s < X_NSTEPS; s++) {
        if (bstep_gate(&a.bass[s]) == G_REST)
            continue;
        notes++;
        acc += (a.bass[s].flags & BS_ACCENT) != 0;
        sl += (a.bass[s].flags & BS_SLIDE) != 0;
        lo = a.bass[s].note < lo ? a.bass[s].note : lo;
        hi = a.bass[s].note > hi ? a.bass[s].note : hi;
    }
    CHECK(notes >= 4 && notes <= 16 && lo >= 33 && hi <= 33 + 24 + 10, "line: %d notes, %d..%d", notes, lo, hi);
    uint32_t r1 = 0, r2 = 0;
    b = a;
    tb3po_mutate(&a, &r1);
    tb3po_mutate(&b, &r2);
    CHECK(memcmp(a.bass, b.bass, sizeof a.bass) == 0 && r1 == r2, "mutate is deterministic too");
    printf("tb3po: seed 0xC0FFEE -> %d notes (%d accents, %d slides), notes %d..%d, reproducible\n", notes, acc,
           sl, lo, hi);
}

/* ---- the saved state ---- */

static void test_state(void)
{
    static uint8_t buf[X_STATE_MAX], buf2[X_STATE_MAX];
    x0x_init(&g_x);
    for (int k = 0; k < 500; k++) {
        pattern_t *p = &g_x.pat[rndi(X_NPAT)];
        pattern_drum_nudge(p, rndi(X_NDRUM), rndi(X_NSTEPS), rndi(9) - 4);
        pattern_bass_nudge(p, rndi(X_NSTEPS), rndi(9) - 4, X_NOTE_LO, X_NOTE_HI);
        pattern_bass_flags(p, rndi(X_NSTEPS), 1);
        p->gen.seed = rnd();
        p->drum_len = (uint8_t)(1 + rndi(16));
        int t = rndi(X_NTARGETS), n = x0x_nparams(t);
        x0x_set(&g_x, t, rndi(n), rndi(128));
    }
    g_x.seq.bpm = 133.7f;
    g_x.seq.swing = 33;
    g_x.seq.xpose = -7;
    g_x.seq.ppat = 5;
    g_x.seq.mute = 0x81;
    g_x.follow_clock = 0;
    uint32_t n = x0x_state_pack(&g_x, buf, sizeof buf);
    CHECK(n > 0 && n <= X_STATE_MAX, "pack: %u bytes", n);
    CHECK(x0x_state_pack(&g_x, buf, 100) == 0, "pack refuses a small buffer");
    x0x_init(&g_y);
    CHECK(x0x_state_unpack(&g_y, buf, n) == 0, "unpack");
    uint32_t n2 = x0x_state_pack(&g_y, buf2, sizeof buf2);
    CHECK(n2 == n && memcmp(buf, buf2, n) == 0, "round trip: the same bytes");
    CHECK(memcmp(g_x.pat, g_y.pat, sizeof g_x.pat) == 0, "round trip: the same patterns");
    CHECK(memcmp(g_x.pot, g_y.pot, sizeof g_x.pot) == 0, "round trip: the same sound");
    CHECK(fabsf(g_y.seq.bpm - 133.7f) < 0.06f && g_y.seq.swing == 33 && g_y.seq.xpose == -7 && g_y.seq.ppat == 5 &&
              g_y.seq.mute == 0x81 && g_y.follow_clock == 0, "round trip: the settings");
    /* the engines got the sound */
    CHECK(bass303_get(&g_y.bass, BASS303_CUTOFF) == g_y.pot[X_T_BASS][BASS303_CUTOFF], "unpack applies the 303's pots");
    buf[200] ^= 0x40;
    x0x_init(&g_y);
    CHECK(x0x_state_unpack(&g_y, buf, n) == -1, "a damaged state is refused");
    CHECK(g_y.seq.bpm == 125.0f, "and changes nothing");
    CHECK(x0x_state_unpack(&g_y, buf, n - 1) == -1, "a short state is refused");
    /* out-of-range values (an older or foreign writer) come back clamped */
    x0x_init(&g_x);
    g_x.pat[0].bass[0].note = 3;
    g_x.pat[0].bass[1].note = 120;
    g_x.pat[0].gen.base_oct = 4;
    g_x.seq.xpose = 100;
    n = x0x_state_pack(&g_x, buf, sizeof buf);
    x0x_init(&g_y);
    CHECK(x0x_state_unpack(&g_y, buf, n) == 0 && g_y.pat[0].bass[0].note == X_NOTE_LO &&
              g_y.pat[0].bass[1].note == X_NOTE_HI && g_y.pat[0].gen.base_oct == X_BASE_OCT_MAX &&
              g_y.seq.xpose == X_XPOSE_MAX, "unpack clamps notes, base octave, transpose");
    printf("state: %u bytes, round trip exact, damage refused\n", n);
}

static void test_save_sched(void)
{
    x0x_save_t s;
    memset(&s, 0, sizeof s);
    CHECK(x0x_save_poll(&s, 1000, 0) == X0X_SAVE_NONE, "nothing to save");
    x0x_save_changed(&s, 1000);
    CHECK(x0x_save_poll(&s, 2500, 0) == X0X_SAVE_NONE, "not before 2 s quiet");
    CHECK(x0x_save_poll(&s, 3000, 0) == X0X_SAVE_NOW, "after 2 s quiet");
    x0x_save_result(&s, 3000, 1);
    x0x_save_changed(&s, 4000);
    CHECK(x0x_save_poll(&s, 10000, 0) == X0X_SAVE_NONE, "at most every 20 s");
    CHECK(x0x_save_poll(&s, 23000, 0) == X0X_SAVE_NOW, "20 s later");
    CHECK(x0x_save_poll(&s, 10000, 600) == X0X_SAVE_LEAVING, "Shift held 0.6 s: once, whatever the interval");
    x0x_save_result(&s, 10000, 0);
    CHECK(x0x_save_poll(&s, 10100, 900) == X0X_SAVE_NONE, "only once per Shift press");
    CHECK(x0x_save_poll(&s, 10000, 300) == X0X_SAVE_NONE, "a quick Shift combination saves nothing");
}

/* ---- the CPU guard ---- */

static void test_guard(void)
{
    static drum808_t d;
    float buf[BLOCK];
    x0x_init(&g_x);
    CHECK(x0x_guard_step(&g_x, 300, 0) == 0, "guard: calm at 30%%");
    CHECK(x0x_guard_step(&g_x, 580, 0) == 1, "guard: up one over 55%%");
    CHECK(x0x_guard_step(&g_x, 750, 0) == 3, "guard: up two over 70%%");
    CHECK(x0x_guard_step(&g_x, 900, 0) == 3, "guard: at most 3");
    for (int k = 0; k < 5; k++)
        CHECK(x0x_guard_step(&g_x, 300, 0) == 3, "guard: holds under 3 s calm (%d)", k);
    CHECK(x0x_guard_step(&g_x, 300, 0) == 2, "guard: down one after 3 s under 40%%");
    CHECK(x0x_guard_step(&g_x, 500, 0) == 2 && x0x_guard_step(&g_x, 300, 0) == 2, "guard: the band between resets the calm");
    CHECK(x0x_guard_step(&g_x, 450, 1) == 3, "guard: late blocks raise it at once");
    /* the cap: all seven at once with at most 3 -> 3 sounding, the oldest faded */
    drum808_init(&d);
    drum808_set_guard(&d, 3, 40);
    for (int t = 0; t < D8_NUM; t++)
        drum808_trigger(&d, t, 1.0f);
    for (int k = 0; k < 4; k++) {
        memset(buf, 0, sizeof buf);
        drum808_render(&d, buf, BLOCK);
    }
    int on = 0;
    for (int t = 0; t < D8_NUM; t++)
        on += d.lane[t].cg > 0.0f && d.lane[t].cstep == 0.0f;
    CHECK(on == 3, "cap 3: %d voices sounding after all seven hit", on);
    CHECK(d.lane[D8_CB].cg > 0.0f && d.lane[D8_OH].cg > 0.0f && d.lane[D8_BD].cg == 0.0f,
          "cap: the newest kept, the oldest gone");
    /* and the engine applies the level */
    x0x_init(&g_x);
    g_x.guard = 2;
    x0x_render(&g_x, buf, BLOCK);
    CHECK(g_x.kit.max_voices == 4, "guard 2 -> 4 voices (%d)", (int)g_x.kit.max_voices);
}

/* ---- the master: loud stays inside +-1 ---- */

static void test_master(void)
{
    float buf[BLOCK];
    float peak = 0;
    x0x_init(&g_x);
    x0x_set(&g_x, X_T_MIX, X_MIX_DRIVE, 127);
    x0x_set(&g_x, X_T_MIX, X_MIX_KIT, 127);
    x0x_set(&g_x, X_T_MIX, X_MIX_BASS, 127);
    x0x_set(&g_x, X_T_MIX, X_MIX_VOLUME, 127);
    for (int t = 0; t < X_NDRUM; t++)
        for (int s = 0; s < X_NSTEPS; s++)
            g_x.pat[0].drum[t][s] = X_LVL_MAX;
    seq_start(&g_x.seq);
    for (int b = 0; b < 5 * FS / BLOCK; b++) {
        run_block(&g_x, buf);
        for (int i = 0; i < BLOCK; i++)
            peak = fabsf(buf[i]) > peak ? fabsf(buf[i]) : peak;
    }
    CHECK(peak <= 1.0f && peak > 0.5f, "everything at full: peak %.3f, inside +-1", peak);
}

/* ---- timing ---- */

static double ns_per_sample(x0x_t *x, double seconds, float *worst_block_us)
{
    float buf[BLOCK];
    long blocks = (long)(seconds * FS / BLOCK);
    struct timespec t0, t1, b0, b1;
    double worst = 0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (long b = 0; b < blocks; b++) {
        clock_gettime(CLOCK_MONOTONIC, &b0);
        run_block(x, buf);
        clock_gettime(CLOCK_MONOTONIC, &b1);
        double us = (b1.tv_sec - b0.tv_sec) * 1e6 + (b1.tv_nsec - b0.tv_nsec) / 1e3;
        if (us > worst)
            worst = us;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (worst_block_us)
        *worst_block_us = (float)worst;
    double ns = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
    return ns / (double)(blocks * BLOCK);
}

static void test_timing(void)
{
    float w1, w2;
    x0x_init(&g_x);
    seq_start(&g_x.seq);
    double factory = ns_per_sample(&g_x, 20.0, &w1);
    /* the worst case: every drum on every step, accented, the 303 on every step */
    x0x_init(&g_x);
    for (int t = 0; t < X_NDRUM; t++)
        for (int s = 0; s < X_NSTEPS; s++)
            g_x.pat[0].drum[t][s] = X_LVL_MAX;
    for (int s = 0; s < X_NSTEPS; s++)
        g_x.pat[0].bass[s].flags = G_NOTE | BS_ACCENT;
    x0x_set(&g_x, X_T_BASS, BASS303_DECAY, 127);
    g_x.seq.bpm = 180.0f;
    seq_start(&g_x.seq);
    double worst = ns_per_sample(&g_x, 20.0, &w2);
    printf("timing: %.1f ns/sample (factory loop), %.1f ns/sample (every drum and the 303 on every step, "
           "180 BPM); slowest block %.1f / %.1f us (a block is %.0f us)\n",
           factory, worst, w1, w2, 1e6 * BLOCK / FS);
}

/* ---- the demo ---- */

static void put16(FILE *f, int v)
{
    fputc(v & 0xFF, f);
    fputc((v >> 8) & 0xFF, f);
}
static void put32le(FILE *f, uint32_t v)
{
    put16(f, (int)(v & 0xFFFF));
    put16(f, (int)(v >> 16));
}

static int write_demo(const char *path)
{
    const long n = 10L * FS;
    FILE *f = fopen(path, "wb");
    float buf[BLOCK];
    if (!f)
        return -1;
    fwrite("RIFF", 1, 4, f);
    put32le(f, (uint32_t)(36 + n * 2));
    fwrite("WAVEfmt ", 1, 8, f);
    put32le(f, 16);
    put16(f, 1);
    put16(f, 1);
    put32le(f, FS);
    put32le(f, FS * 2);
    put16(f, 2);
    put16(f, 16);
    fwrite("data", 1, 4, f);
    put32le(f, (uint32_t)(n * 2));
    x0x_init(&g_x);
    seq_start(&g_x.seq);
    for (long b = 0; b < n / BLOCK; b++) {
        double t = (double)b * BLOCK / FS;
        /* 0-4 s the factory loop; 4-7 s the cutoff swept up and back; at 7 s a closed hat on the
         * 16ths and a new TB-3PO line */
        if (t >= 4.0 && t < 7.0) {
            double u = (t - 4.0) / 3.0;
            x0x_set(&g_x, X_T_BASS, BASS303_CUTOFF, 48 + (int)(60 * sin(M_PI * u)));
        }
        if (b == (long)(7.0 * FS / BLOCK)) {
            for (int s = 0; s < X_NSTEPS; s++)
                if (s % 4 != 2)
                    g_x.pat[0].drum[4][s] = (s % 4 == 0) ? 6 : 4;
            x0x_generate(&g_x, 0x5EEDu);
        }
        run_block(&g_x, buf);
        for (int i = 0; i < BLOCK; i++) {
            int v = (int)lrintf(buf[i] * 32767.0f);
            put16(f, v < -32768 ? -32768 : v > 32767 ? 32767 : v);
        }
    }
    fclose(f);
    printf("wrote %s (10 s)\n", path);
    return 0;
}

/* --stress-state out.bin: a saved state with every drum and the 303 on every step, accented,
 * at 180 BPM (copied to /apps/x0x/state.bin, the device's worst case) */
static int write_stress_state(const char *path)
{
    static uint8_t buf[X_STATE_MAX];
    x0x_init(&g_x);
    for (int t = 0; t < X_NDRUM; t++)
        for (int s = 0; s < X_NSTEPS; s++)
            g_x.pat[0].drum[t][s] = X_LVL_MAX;
    for (int s = 0; s < X_NSTEPS; s++)
        g_x.pat[0].bass[s].flags = G_NOTE | BS_ACCENT;
    x0x_set(&g_x, X_T_BASS, BASS303_DECAY, 127);
    g_x.seq.bpm = 180.0f;
    uint32_t n = x0x_state_pack(&g_x, buf, sizeof buf);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(buf, 1, n, f) != n)
        return -1;
    fclose(f);
    printf("wrote %s (%u bytes)\n", path, n);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--stress-state") == 0)
        return write_stress_state(argv[2]) == 0 ? 0 : 1;
    test_pattern_edits();
    test_tb3po();
    test_seq_timing();
    test_ext_clock();
    test_303_pitch();
    test_kick();
    test_state();
    test_save_sched();
    test_guard();
    test_master();
    test_random_run();
    test_timing();
    if (argc == 3 && strcmp(argv[1], "--wav") == 0 && write_demo(argv[2]) != 0) {
        fprintf(stderr, "can't write %s\n", argv[2]);
        return 1;
    }
    if (g_failures) {
        printf("%d FAILED\n", g_failures);
        return 1;
    }
    printf("ALL PASSED\n");
    return 0;
}
