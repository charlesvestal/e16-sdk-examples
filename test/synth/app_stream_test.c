/*
 * Host test for src/synth/app_stream.c: a native app's audio stream
 * between its render callback and the per-sample audio interrupt. Plain
 * C, run by test/run_all.sh; prints "ALL PASSED" and exits 0 on success.
 *
 * The "interrupts" are simulated: a loop calls app_stream_next() once per
 * sample, and the render side (target + callback + rendered) runs when
 * the test says it does -- right away, a few samples later, or not at all.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "app_stream.h"

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

#define BLOCK_CYCLES 112000u /* 168 MHz / 48 kHz * 32 */
#define BUDGET 800u

/* The "callback": a ramp, one step per frame, continuing across calls. */
static float g_next_value;
static void render_ramp(float *out, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        out[i] = g_next_value;
        g_next_value += 1.0f;
    }
}

/* One render-interrupt run: returns 1 if it rendered. */
static int render_once(app_stream_t *s, uint32_t cycles)
{
    float *out = app_stream_render_target(s);
    if (!out) {
        return 0;
    }
    render_ramp(out, APP_STREAM_BLOCK);
    app_stream_rendered(s, cycles, cycles);
    return 1;
}

/* Prompt renders: one block of silence, then the ramp without a gap. */
static void test_prompt(void)
{
    app_stream_t s;
    app_stream_init(&s, BLOCK_CYCLES, BUDGET);
    app_stream_start(&s);
    g_next_value = 1.0f;
    CHECK(render_once(&s, 1000) == 1, "start asks for a render");
    CHECK(render_once(&s, 1000) == 0, "nothing more until a swap");
    int bad = 0;
    float expect = 1.0f;
    for (int i = 0; i < 48000; i++) {
        bool kick = false;
        float v = app_stream_next(&s, &kick);
        if (i < (int)APP_STREAM_BLOCK) {
            bad += v != 0.0f;
        } else {
            bad += v != expect;
            expect += 1.0f;
        }
        CHECK(kick == (i > 0 && i % (int)APP_STREAM_BLOCK == 0), "kick at every block start (sample %d)", i);
        if (kick) {
            render_once(&s, 1000);
        }
    }
    CHECK(bad == 0, "silent block then a continuous ramp (%d wrong samples)", bad);
    CHECK(s.late == 0, "no late blocks (%u)", (unsigned)s.late);
    CHECK(s.calls == 48000u / APP_STREAM_BLOCK, "calls %u", (unsigned)s.calls);
}

/* A render that lands a few samples after the kick (the interrupt was
 * busy, or other interrupts ran first) is still in time. */
static void test_delayed(void)
{
    app_stream_t s;
    app_stream_init(&s, BLOCK_CYCLES, BUDGET);
    app_stream_start(&s);
    g_next_value = 1.0f;
    render_once(&s, 1000);
    int due = -1, bad = 0;
    float expect = 1.0f;
    for (int i = 0; i < 9600; i++) {
        bool kick = false;
        float v = app_stream_next(&s, &kick);
        if (i >= (int)APP_STREAM_BLOCK) {
            bad += v != expect;
            expect += 1.0f;
        }
        if (kick) {
            due = i + 31; /* the last sample before the block is needed */
        }
        if (i == due) {
            CHECK(render_once(&s, 1000) == 1, "delayed render at %d", i);
        }
    }
    CHECK(bad == 0, "delayed renders keep the ramp whole (%d wrong)", bad);
    CHECK(s.late == 0, "late %u", (unsigned)s.late);
}

/* A missed render: counted late, the stale block plays, the stream
 * recovers on the next one. */
static void test_missed(void)
{
    app_stream_t s;
    app_stream_init(&s, BLOCK_CYCLES, BUDGET);
    app_stream_start(&s);
    g_next_value = 1.0f;
    render_once(&s, 1000);
    int kicks = 0; /* 19 swaps in 20 blocks; the 5th goes unrendered */
    for (int i = 0; i < 32 * 20; i++) {
        bool kick = false;
        app_stream_next(&s, &kick);
        if (kick && ++kicks != 5) {
            render_once(&s, 1000);
        }
    }
    CHECK(s.late == 1, "one late block (%u)", (unsigned)s.late);
    /* After the miss, a render is due for the new request only (the
     * missed one is superseded), and rendering resumes normally. */
    CHECK(s.calls == 1u + 18u, "calls %u", (unsigned)s.calls);
    /* And a callback that's always late is late on every block. */
    app_stream_start(&s);
    for (int i = 0; i < 32 * 10; i++) {
        bool kick = false;
        app_stream_next(&s, &kick);
    }
    CHECK(s.late == 9, "never rendered: late on every swap but the first block (%u)", (unsigned)s.late);
}

/* A render preempted by the swap (still writing when its block starts
 * playing) finishes the old request; the new one is still due after. */
static void test_preempted(void)
{
    app_stream_t s;
    app_stream_init(&s, BLOCK_CYCLES, BUDGET);
    app_stream_start(&s);
    float *out = app_stream_render_target(&s); /* request 1 */
    CHECK(out == s.buf[1], "first target is block 1");
    for (int i = 0; i < (int)APP_STREAM_BLOCK + 1; i++) { /* the swap happens meanwhile */
        bool kick = false;
        app_stream_next(&s, &kick);
    }
    CHECK(s.late == 1, "the swap found it unfinished");
    app_stream_rendered(&s, 1000, 1000);
    out = app_stream_render_target(&s);
    CHECK(out == s.buf[0], "the next request (block 0) is due after the slow render");
    app_stream_rendered(&s, 1000, 1000);
    CHECK(app_stream_render_target(&s) == NULL, "then nothing");
}

/* Renders (each taking `cycles`) until one more load window has ended,
 * or the stream stopped. */
static void run_window(app_stream_t *s, uint32_t cycles)
{
    uint32_t w = s->windows;
    for (int guard = 0; guard < 4000 && s->windows == w && s->state == APP_STREAM_RUNNING; guard++) {
        for (uint32_t i = 0; i < APP_STREAM_BLOCK; i++) {
            bool kick = false;
            app_stream_next(s, &kick);
        }
        render_once(s, cycles);
    }
}

static void test_load(void)
{
    app_stream_t s;
    app_stream_init(&s, BLOCK_CYCLES, BUDGET);
    app_stream_start(&s);
    render_once(&s, BLOCK_CYCLES / 2);
    run_window(&s, BLOCK_CYCLES / 2);
    CHECK(s.windows == 1, "one window (%u)", (unsigned)s.windows);
    CHECK(s.last_avg_permille == 500, "50%% load (%u)", (unsigned)s.last_avg_permille);
    CHECK(s.last_peak_permille == 500, "peak (%u)", (unsigned)s.last_peak_permille);

    /* two overloaded seconds, a good one, two more: still running */
    run_window(&s, BLOCK_CYCLES * 9 / 10);
    run_window(&s, BLOCK_CYCLES * 9 / 10);
    CHECK(s.state == APP_STREAM_RUNNING && s.over == 2, "two overloaded windows: running (over %u)", (unsigned)s.over);
    run_window(&s, BLOCK_CYCLES / 10);
    CHECK(s.over == 0, "a good window resets the count");
    run_window(&s, BLOCK_CYCLES * 9 / 10);
    run_window(&s, BLOCK_CYCLES * 9 / 10);
    CHECK(s.state == APP_STREAM_RUNNING, "still running");
    /* a third in a row: stopped, silence, no more renders */
    run_window(&s, BLOCK_CYCLES * 9 / 10);
    CHECK(s.state == APP_STREAM_STOP_OVERLOAD, "three in a row stop it (state %u)", (unsigned)s.state);
    CHECK(s.last_avg_permille == 900, "the stopping window's load (%u)", (unsigned)s.last_avg_permille);
    bool kick = false;
    int loud = 0;
    for (int i = 0; i < 1000; i++) {
        loud += app_stream_next(&s, &kick) != 0.0f;
    }
    CHECK(loud == 0 && !kick, "silent and quiet after the stop");
    CHECK(app_stream_render_target(&s) == NULL, "no render after the stop");

    /* a single very slow call (an app's first-block setup) doesn't stop it */
    app_stream_start(&s);
    render_once(&s, BLOCK_CYCLES * 20);
    run_window(&s, BLOCK_CYCLES / 4);
    CHECK(s.state == APP_STREAM_RUNNING, "one slow call is fine");
    CHECK(s.last_peak_permille == 20000, "peak shows it (%u)", (unsigned)s.last_peak_permille);

    /* over 100 %: stopped as soon as a window's worth of CPU is used up,
     * after one (short) window */
    app_stream_start(&s);
    render_once(&s, BLOCK_CYCLES * 3 / 2);
    run_window(&s, BLOCK_CYCLES * 3 / 2);
    CHECK(s.state == APP_STREAM_STOP_OVERLOAD && s.windows == 1, "150%%: stopped after one window (state %u)",
          (unsigned)s.state);
    CHECK(s.calls == 1000, "150%%: after 1000 calls, a second's worth of CPU (%u)", (unsigned)s.calls);
    CHECK(s.last_avg_permille == 1500, "150%%: load %u", (unsigned)s.last_avg_permille);
    /* 99 % isn't "starving": the three-window rule applies */
    app_stream_start(&s);
    render_once(&s, BLOCK_CYCLES * 99 / 100);
    run_window(&s, BLOCK_CYCLES * 99 / 100);
    CHECK(s.state == APP_STREAM_RUNNING && s.over == 1, "99%%: one overloaded window, still running");

    /* huge cycle counts don't overflow the window sum */
    app_stream_start(&s);
    run_window(&s, 0xF0000000u);
    CHECK(s.windows == 1 && s.last_avg_permille > 1000000u && s.state == APP_STREAM_STOP_OVERLOAD,
          "huge calls: load %u", (unsigned)s.last_avg_permille);
}

/* Runs `windows` load windows with net/gross cycles per call, leaving
 * one block in `skip_every` unrendered (0 = none): late blocks. */
static void run_late(app_stream_t *s, int windows, uint32_t net, uint32_t gross, int skip_every)
{
    uint32_t w = s->windows + (uint32_t)windows;
    for (int b = 0; b < 40000 && s->windows < w && s->state == APP_STREAM_RUNNING; b++) {
        for (uint32_t i = 0; i < APP_STREAM_BLOCK; i++) {
            bool kick = false;
            app_stream_next(s, &kick);
        }
        if (skip_every && b % skip_every == 0) {
            continue;
        }
        float *out = app_stream_render_target(s);
        if (out) {
            render_ramp(out, APP_STREAM_BLOCK);
            app_stream_rendered(s, net, gross);
        }
    }
}

/* The callback's own load under budget, but it plus what preempts it
 * (the voice, USB) doesn't fit: blocks late every window. That's an
 * overload too. */
static void test_late_rule(void)
{
    app_stream_t s;
    app_stream_init(&s, BLOCK_CYCLES, BUDGET);
    app_stream_start(&s);
    render_once(&s, BLOCK_CYCLES / 2);
    /* 10 % of blocks late, net 50 %: three windows, then stopped */
    run_late(&s, 2, BLOCK_CYCLES / 2, BLOCK_CYCLES * 95 / 100, 10);
    CHECK(s.state == APP_STREAM_RUNNING && s.over == 2, "two late windows: running (over %u)", (unsigned)s.over);
    CHECK(s.last_avg_permille == 500, "net load reported, 50%% (%u)", (unsigned)s.last_avg_permille);
    CHECK(s.last_late > 100, "late blocks in the window (%u)", (unsigned)s.last_late);
    run_late(&s, 1, BLOCK_CYCLES / 2, BLOCK_CYCLES * 95 / 100, 10);
    CHECK(s.state == APP_STREAM_STOP_OVERLOAD, "third late window: stopped (state %u)", (unsigned)s.state);

    /* a few late blocks (2 %) don't count */
    app_stream_start(&s);
    render_once(&s, BLOCK_CYCLES / 2);
    run_late(&s, 5, BLOCK_CYCLES / 2, BLOCK_CYCLES / 2, 50);
    CHECK(s.state == APP_STREAM_RUNNING && s.over == 0, "2%% late: fine (over %u, late %u)", (unsigned)s.over,
          (unsigned)s.last_late);

    /* one late window between good ones resets */
    run_late(&s, 2, BLOCK_CYCLES / 2, BLOCK_CYCLES / 2, 10);
    run_late(&s, 1, BLOCK_CYCLES / 2, BLOCK_CYCLES / 2, 0);
    CHECK(s.state == APP_STREAM_RUNNING && s.over == 0, "a clean window resets the count");

    /* gross (wall) time over 100 % with a small net share: starving,
     * stopped after one short window */
    app_stream_start(&s);
    render_once(&s, BLOCK_CYCLES / 4);
    run_late(&s, 1, BLOCK_CYCLES / 4, BLOCK_CYCLES * 3 / 2, 0);
    CHECK(s.state == APP_STREAM_STOP_OVERLOAD && s.windows == 1 && s.calls < 1100,
          "gross 150%%, net 25%%: stopped after one window (state %u, calls %u)", (unsigned)s.state,
          (unsigned)s.calls);
}

static void test_off(void)
{
    app_stream_t s;
    app_stream_init(&s, BLOCK_CYCLES, BUDGET);
    bool kick = false;
    CHECK(app_stream_next(&s, &kick) == 0.0f && !kick, "not started: silence, no kick");
    CHECK(app_stream_render_target(&s) == NULL, "not started: nothing to render");
    app_stream_start(&s);
    g_next_value = 0.5f;
    render_once(&s, 10);
    app_stream_stop(&s);
    for (int i = 0; i < 100; i++) {
        CHECK(app_stream_next(&s, &kick) == 0.0f, "stopped: silence");
    }
    CHECK(!kick, "stopped: no kick");
}

static void test_mix(void)
{
    CHECK(app_stream_mix(0.0f, 0.25f) == 0.25f, "app passes through");
    CHECK(app_stream_mix(0.5f, 0.25f) == 0.75f, "sum");
    CHECK(app_stream_mix(0.9f, 0.9f) == 1.0f, "clamped high");
    CHECK(app_stream_mix(-0.9f, -0.9f) == -1.0f, "clamped low");
    CHECK(app_stream_mix(0.0f, NAN) == 0.0f, "NaN plays as 0");
    CHECK(app_stream_mix(0.5f, NAN) == 0.5f, "NaN leaves the voice");
    CHECK(app_stream_mix(0.0f, INFINITY) == 1.0f, "+inf clamps");
    CHECK(app_stream_mix(0.0f, -INFINITY) == -1.0f, "-inf clamps");
    CHECK(app_stream_mix(0.0f, 1e30f) == 1.0f, "huge clamps");
}

int main(void)
{
    test_prompt();
    test_delayed();
    test_missed();
    test_preempted();
    test_load();
    test_late_rule();
    test_off();
    test_mix();
    if (g_failures) {
        printf("%d FAILED\n", g_failures);
        return 1;
    }
    printf("ALL PASSED\n");
    return 0;
}
