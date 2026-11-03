/* SPDX-License-Identifier: GPL-3.0-only */
/* x0x-lite sequencer (X0X's seq/sequencer, cut down to one kit and one 303): runs in the audio
 * callback, sample accurate.
 *
 * The engine renders a block like this:
 *     while (n) {
 *         k = seq_until_event(&seq, n);       // samples until the next due event (>= 1)
 *         render k samples;
 *         seq_advance(&seq, k, &sink);        // moves time, fires the events now due
 *         n -= k;
 *     }
 * so a step lands on its exact sample, and engine code never sees time.
 *
 * Two parts, each with its own step counter (the lengths may differ): the 808 kit (SEQ_KIT)
 * and the 303 (SEQ_BASS), always 16ths. Both play the same pattern; the kit's length defines
 * the bar: a cued pattern starts when it wraps (both parts from step 1).
 *
 * Clock: internal (BPM) or external (24 ppqn MIDI clock, fed with seq_ext_clock()).
 * Swing delays every second 16th (0 = straight, 100 = 75 %).
 *
 * 303 semantics (as the TB-303 and Open303's sequencer):
 *   NOTE  plays; the gate falls at half the step, unless the step slides (BS_SLIDE)
 *         or the next step is a TIE, in which case it is held through.
 *   TIE   continues the sounding note (no retrigger); same gate rule.
 *   slide a NOTE following a step with BS_SLIDE glides from the held note
 *         (note_on with slide = 1, envelopes not retriggered).
 *   REST  releases.
 *
 * Main flow -> callback: the requests (start, stop, continue, external clocks) are counters
 * only the main flow writes and the callback compares with its own copies, so neither ever
 * loses one to the other's read-modify-write. bpm, swing, mute, cue, xpose and the flags are
 * single words the callback reads as they are. */
#pragma once
#include <stdint.h>
#include "pattern.h"

#define SEQ_FS 48000u
#define SEQ_KIT 0
#define SEQ_BASS 1
#define SEQ_NPARTS 2
#define SEQ_MUTE_BASS X_NDRUM          /* mute bit of the 303; drum track t is bit t */

typedef struct {
    int (*drum)(void *ctx, int voice, float vel);
    void (*bass_on)(void *ctx, int note, int accent, int slide);
    void (*bass_off)(void *ctx);
    void (*midi)(void *ctx, uint8_t status);          /* clock and transport out (one byte) */
    void (*step)(void *ctx, int part, int step);      /* UI playhead (optional) */
    void *ctx;
} seq_sink_t;

typedef struct {                      /* one part's runtime */
    uint8_t pos;                      /* current step */
    uint8_t started;                  /* the first step has fired since start */
    uint8_t sounding;                 /* 303: a note is held */
    uint8_t held_note;                /* 303: bit 7 = it slides on */
    uint32_t to_next;                 /* samples to the next step (Q8 fraction) */
    uint32_t to_off;                  /* 303: samples to the gate falling, Q8; 0 = none */
    uint16_t bars;                    /* wraps since the pattern started (auto-mutate) */
    uint8_t ext_ticks;                /* external clock: ticks into this step */
} seq_track_t;

typedef struct {
    pattern_t *pat;                   /* X_NPAT patterns */
    volatile uint8_t playing;         /* 1 = running (the callback's) */
    volatile uint8_t ppat;            /* the pattern playing */
    volatile uint8_t pcue;            /* the cued pattern (0xFF = none) */
    volatile uint8_t ext;             /* 1 = following an external clock right now */
    volatile uint8_t send_clock;      /* 1 = emit MIDI clock + transport when internal */
    volatile uint8_t swing;           /* 0..100 */
    volatile int8_t xpose;            /* the 303's transpose, semitones */
    volatile uint32_t mute;           /* bit t = drum track t, SEQ_MUTE_BASS = the 303 */
    volatile float bpm;               /* internal tempo (main writes; read per step) */
    float ext_bpm;                    /* tempo measured from the external clock */
    seq_track_t t[SEQ_NPARTS];
    uint32_t clk_to_next;             /* internal: samples to the next MIDI clock, Q8 */
    uint32_t ext_since;               /* samples since the last external clock (tempo estimate) */
    uint32_t ext_period;              /* smoothed samples per external clock, Q8 */
    uint32_t mut_rng;                 /* the auto-mutate random stream */
    volatile uint32_t mutated;        /* auto-mutations so far (the UI redraws) */
    /* requests: main writes the *_req counters, the callback keeps the *_done copies */
    volatile uint32_t start_req, stop_req, cont_req, ext_req;
    uint32_t start_done, stop_done, cont_done, ext_done;
} seq_t;

void seq_init(seq_t *s, pattern_t *patterns);
uint32_t seq_until_event(seq_t *s, uint32_t n);
void seq_advance(seq_t *s, uint32_t k, const seq_sink_t *o);

/* main flow side */
void seq_start(seq_t *s);             /* from step 1 of the current (or cued) pattern */
void seq_stop(seq_t *s);
void seq_continue(seq_t *s);
void seq_ext_clock(seq_t *s);         /* one 24-ppqn tick (MIDI 0xF8) */
void seq_cue(seq_t *s, int pattern);  /* at the end of the bar (at once, if stopped) */

/* one 16th in samples Q8 (no swing) at the current tempo */
uint32_t seq_step_q8(const seq_t *s);
float seq_tempo(const seq_t *s);      /* the tempo in effect (internal or measured) */
int seq_part_len(const seq_t *s, int part);
