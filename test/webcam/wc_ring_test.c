/*
 * Host test for src/webcam/wc_ring.c, webcam mode's render-ahead
 * bookkeeping. Plain C, run directly by test/run_all.sh; prints "ALL
 * PASSED" and exits 0 on success.
 *
 * A model of the two contexts: the consumer (USB's interrupt: frames of
 * 120 lines, sometimes cut short, sometimes after a pause) and the
 * producer (the renderer: claim, render over several steps during which
 * the consumer may preempt it, commit), interleaved at random. The model
 * records what each ring slot really holds -- which frame and line, made
 * from which version of which view, and whether a view was replaced or
 * the slot handed out while it was being written -- and checks every line
 * the consumer sends from the ring against the frame and line it asked
 * for.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wc_ring.h"

static int g_failures;
#define CHECK(cond, ...)                                       \
    do {                                                        \
        if (!(cond)) {                                          \
            g_failures++;                                       \
            if (g_failures < 20) {                              \
                fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
                fprintf(stderr, __VA_ARGS__);                   \
                fprintf(stderr, "\n");                          \
            }                                                   \
        }                                                       \
    } while (0)

#define H WC_RING_H_LINES

/* ---- The model ---- */

static wc_ring_t r;
static uint32_t now_ms;
static uint32_t view_ver[2];     /* bumped whenever a view is (re)made */
static uint32_t view_for[2];     /* the frame each view was last made for */

typedef struct {
    uint32_t gen, y, ver;
    bool torn;                   /* its view was replaced while it was rendered */
    bool valid;
} content_t;
static content_t slots[WC_RING_AHEAD];

/* The producer's line in progress. */
static bool busy;
static uint32_t p_gen, p_y, p_ver;
static int p_slot, p_left;

/* The consumer. */
static bool in_frame;
static uint32_t c_y;
static uint32_t frames, sent_ring, sent_inline, commits, refused;

static void make_view(uint32_t gen)
{
    view_ver[gen & 1u]++;
    view_for[gen & 1u] = gen;
    wc_ring_view_made(&r, gen, now_ms);
}

static void reset_model(void)
{
    wc_ring_reset(&r);
    memset(slots, 0, sizeof slots);
    memset(view_ver, 0, sizeof view_ver);
    view_for[0] = view_for[1] = WC_RING_NO_TAG;
    busy = in_frame = false;
    frames = sent_ring = sent_inline = commits = refused = 0;
}

static void consumer_frame(void)
{
    bool need;
    uint32_t gen = wc_ring_frame(&r, now_ms, &need);
    if (need) {
        make_view(gen);
    }
    CHECK(view_for[gen & 1u] == gen, "frame %u starts without its view", (unsigned)gen);
    in_frame = true;
    c_y = 0;
    frames++;
}

/* Sends line c_y of the current frame: from the ring if it says so, which must then be exactly that line. */
static void consumer_line(void)
{
    uint32_t gen = r.gen;
    int slot = wc_ring_take(&r, c_y);
    if (slot >= 0) {
        const content_t *c = &slots[slot];
        CHECK(slot == (int)(c_y % WC_RING_AHEAD), "slot %d for line %u", slot, (unsigned)c_y);
        CHECK(!(busy && p_slot == slot), "frame %u line %u: sent from a slot being written", (unsigned)gen,
              (unsigned)c_y);
        CHECK(c->valid && c->gen == gen && c->y == c_y, "frame %u line %u: the ring held frame %u line %u",
              (unsigned)gen, (unsigned)c_y, (unsigned)c->gen, (unsigned)c->y);
        CHECK(!c->torn, "frame %u line %u: rendered while its view was replaced", (unsigned)gen, (unsigned)c_y);
        CHECK(c->ver == view_ver[gen & 1u] && view_for[gen & 1u] == gen,
              "frame %u line %u: made from another view than the frame's", (unsigned)gen, (unsigned)c_y);
        sent_ring++;
    } else {
        CHECK(view_for[gen & 1u] == gen, "frame %u line %u: inline without the frame's view", (unsigned)gen,
              (unsigned)c_y);
        sent_inline++;
    }
    wc_ring_sent(&r, c_y);
    if (++c_y == H) {
        in_frame = false;
    }
}

/* One step of the producer: claim, a step of rendering, or commit. */
static void producer_step(int render_steps)
{
    if (!busy) {
        uint32_t gen, y;
        int slot;
        bool need;
        if (!wc_ring_claim(&r, &gen, &y, &slot, &need)) {
            return;
        }
        if (need) {
            make_view(gen);
        }
        CHECK(view_for[gen & 1u] == gen, "claimed frame %u without its view", (unsigned)gen);
        CHECK(r.tag[slot] == WC_RING_NO_TAG, "claimed slot %d still tagged", slot);
        busy = true;
        p_gen = gen;
        p_y = y;
        p_slot = slot;
        p_ver = view_ver[gen & 1u];
        p_left = render_steps;
        return;
    }
    if (--p_left > 0) {
        return;
    }
    content_t *c = &slots[p_slot];
    c->gen = p_gen;
    c->y = p_y;
    c->ver = p_ver;
    c->torn = view_ver[p_gen & 1u] != p_ver;
    c->valid = true;
    busy = false;
    if (wc_ring_commit(&r, p_gen, p_y)) {
        commits++;
        CHECK(!c->torn, "frame %u line %u committed though its view was replaced", (unsigned)p_gen,
              (unsigned)p_y);
    } else {
        refused++;
    }
}

/* ---- Scenarios ---- */

/* Random interleavings: the renderer gets 10-90% of the time between USB
 * events, renders a line in 1-4 steps; frames are sometimes cut short
 * (the computer stopped and started again) or start after a pause. */
static void test_random(unsigned seed)
{
    srand(seed);
    reset_model();
    now_ms = 1000;
    int share = 10 + rand() % 81; /* the renderer's share of the steps, 10-90% */
    int steps = 1 + rand() % 4;
    for (int i = 0; i < 200000; i++) {
        if (rand() % 100 < share) {
            producer_step(steps);
            continue;
        }
        now_ms += rand() % 2;
        if (!in_frame) {
            if (rand() % 50 == 0) {
                now_ms += 50 + (uint32_t)(rand() % 200); /* a pause, maybe longer than a stale view */
            }
            consumer_frame();
        } else if (rand() % 2000 == 0) {
            now_ms += (uint32_t)(rand() % 300);
            consumer_frame(); /* cut short: a new frame from line 0 */
        } else {
            consumer_line();
        }
    }
    CHECK(frames > 100, "seed %u: only %u frames", seed, (unsigned)frames);
}

/* No renderer at all: every line is rendered inline, none from the ring. */
static void test_underrun(void)
{
    reset_model();
    now_ms = 5000;
    for (int f = 0; f < 5; f++) {
        consumer_frame();
        while (in_frame) {
            consumer_line();
        }
    }
    CHECK(sent_ring == 0 && sent_inline == 5 * H, "underrun: ring %u, inline %u", (unsigned)sent_ring,
          (unsigned)sent_inline);
}

/* A renderer that keeps up: after the first lines, everything comes from the ring. */
static void test_keeps_up(void)
{
    reset_model();
    now_ms = 5000;
    for (int f = 0; f < 10; f++) {
        consumer_frame();
        while (in_frame) {
            for (int k = 0; k < 4; k++) {
                producer_step(1);
            }
            consumer_line();
        }
    }
    CHECK(sent_inline <= 1 && sent_ring >= 10 * H - 1, "keeps up: ring %u, inline %u", (unsigned)sent_ring,
          (unsigned)sent_inline);
    CHECK(refused == 0, "keeps up: %u refused", (unsigned)refused);
}

/* The next frame's first lines rendered ahead, then a pause longer than
 * WC_RING_STALE_MS: that frame is skipped and none of its lines is sent. */
static void test_stale_skip(void)
{
    reset_model();
    now_ms = 5000;
    consumer_frame();
    while (in_frame) {
        for (int k = 0; k < 4; k++) {
            producer_step(1);
        }
        consumer_line();
    }
    uint32_t before = r.gen;
    for (int k = 0; k < 40; k++) {
        producer_step(1); /* fills the ring with the next frame's lines */
    }
    int ahead_tags = 0;
    for (int i = 0; i < WC_RING_AHEAD; i++) {
        ahead_tags += r.tag[i] != WC_RING_NO_TAG && r.tag[i] >> 8 == before + 1u;
    }
    CHECK(ahead_tags == WC_RING_AHEAD, "only %d lines of the next frame ahead", ahead_tags);
    now_ms += WC_RING_STALE_MS + 1u;
    consumer_frame();
    CHECK(r.gen == before + 2u, "stale frame not skipped: gen %u after %u", (unsigned)r.gen, (unsigned)before);
    for (uint32_t y = 0; y < WC_RING_AHEAD; y++) {
        CHECK(wc_ring_take(&r, y) < 0, "line %u of the skipped frame still offered", (unsigned)y);
    }
    while (in_frame) {
        consumer_line();
    }
    /* Not stale: a frame made ahead just now is used, lines and all. */
    for (int k = 0; k < 40; k++) {
        producer_step(1);
    }
    now_ms += 10;
    before = r.gen;
    consumer_frame();
    CHECK(r.gen == before + 1u, "fresh frame skipped");
    CHECK(wc_ring_take(&r, 0) == 0, "fresh frame's line 0 not offered");
}

/* A renderer the consumer keeps overtaking gives the frame up after
 * WC_RING_MAX_DISCARDS refused lines, and starts again with the next. */
static void test_discard_guard(void)
{
    reset_model();
    now_ms = 5000;
    consumer_frame();
    for (int i = 0; i < 40; i++) {
        producer_step(3); /* claim */
        consumer_line();  /* the consumer overtakes it */
        producer_step(3);
        producer_step(3); /* finished: refused */
    }
    CHECK(refused == WC_RING_MAX_DISCARDS, "%u refused, guard at %u", (unsigned)refused,
          (unsigned)WC_RING_MAX_DISCARDS);
    uint32_t g, y;
    int slot;
    bool need;
    CHECK(!wc_ring_claim(&r, &g, &y, &slot, &need), "still claiming in a given-up frame");
    while (in_frame) {
        consumer_line();
    }
    consumer_frame();
    CHECK(wc_ring_claim(&r, &g, &y, &slot, &need) && g == r.gen, "not claiming in the next frame");
}

int main(void)
{
    test_underrun();
    test_keeps_up();
    test_stale_skip();
    test_discard_guard();
    unsigned long ring = 0, inl = 0, ref = 0, fr = 0;
    for (unsigned seed = 1; seed <= 200; seed++) {
        test_random(seed);
        ring += sent_ring;
        inl += sent_inline;
        ref += refused;
        fr += frames;
    }
    printf("random interleavings: 200 runs, %lu frames, lines from the ring %lu, inline %lu, refused %lu\n", fr,
           ring, inl, ref);
    if (g_failures) {
        printf("%d FAILED\n", g_failures);
        return 1;
    }
    printf("ALL PASSED\n");
    return 0;
}
