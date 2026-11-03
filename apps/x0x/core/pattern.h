/* SPDX-License-Identifier: GPL-3.0-only */
/* x0x-lite pattern model (after X0X's seq/pattern.h): 16 patterns, each one 808 kit part and
 * one 303 part of up to 16 steps.
 *
 * Plain data, no pointers, so a pattern copies with an assignment and the saved state is its
 * bytes. The UI edits these in place from the main flow while the sequencer reads them in the
 * audio callback: every field the sequencer reads is a single byte or an aligned 32-bit word,
 * so a read never sees half a write (a whole-pattern change -- copy, clear, a generated line --
 * can be heard half-done for at most a step, which is musically harmless). */
#pragma once
#include <stdint.h>

#define X_NPAT 16
#define X_NSTEPS 16
#define X_NDRUM 7                    /* drum808.h's tracks, in its order */

/* a drum step: 0 = off, 1..X_LVL_MAX = level; X_LVL_MAX is the accent */
#define X_LVL_MAX 15
#define X_LVL_NORMAL 8               /* a plain (unaccented) 808 hit */

enum { G_REST, G_NOTE, G_TIE };     /* 303 step gate */

/* the 303 steps' notes: C1 .. C6 (the UI's range; a saved state is clamped to it, and TB-3PO's
 * base octave to 1..2 so its lines stay inside: 12 * 3 + 11 + 10 + 24 = 81) */
#define X_NOTE_LO 24
#define X_NOTE_HI 84
#define X_BASE_OCT_MIN 1
#define X_BASE_OCT_MAX 2
/* the 303's live transpose (the encoder, MIDI notes and the saved state), semitones either way */
#define X_XPOSE_MAX 24

/* 303 step: one byte of note, one of flags */
#define BS_GATE_MASK 0x03u
#define BS_ACCENT 0x04u
#define BS_SLIDE 0x08u                /* slide INTO the next step (303 semantics) */
typedef struct {
    uint8_t note;                     /* MIDI note */
    uint8_t flags;                    /* gate | BS_ACCENT | BS_SLIDE */
} bstep_t;

/* TB-3PO settings of one 303 line: the line is reproducible from these and the seed */
typedef struct {
    uint8_t density, accent, slide;   /* probabilities 0..100 % */
    uint8_t oct_range;                /* 1..3 */
    uint8_t root;                     /* 0 = C .. 11 = B */
    uint8_t scale;                    /* tb3po.h SCALES */
    uint8_t base_oct;                 /* octave of the root: note = 12 * (base_oct + 1) + root */
    uint8_t mutate_bars;              /* auto-mutate every N bars, 0 = off */
    uint32_t seed;
} tb3po_cfg_t;

typedef struct {
    uint8_t drum[X_NDRUM][X_NSTEPS];  /* levels, 0 = off */
    bstep_t bass[X_NSTEPS];
    uint8_t drum_len;                 /* 1..16 */
    uint8_t bass_len;                 /* 1..16 */
    uint8_t rsv[2];
    tb3po_cfg_t gen;
} pattern_t;

static inline int bstep_gate(const bstep_t *s) { return s->flags & BS_GATE_MASK; }

/* the 0..1 velocity of a drum level (X_LVL_NORMAL = drum808's unaccented hit, X_LVL_MAX = 1) */
float pattern_level_vel(int level);

/* an empty pattern: 16 steps, the 303 line silent, TB-3PO's defaults with this seed */
void pattern_init(pattern_t *p, uint32_t seed);
/* the factory loop (X0X's, on the 808): kick on the beat, clap on 2 and 4, open hats on the
 * off-beats, accents on 1 and 3, a TB-3PO line on the 303 */
void pattern_factory(pattern_t *p);

/* ---- edits (the UI's; host-tested) ---- */
/* a drum step on (at X_LVL_NORMAL) or off; returns the new level */
int pattern_drum_toggle(pattern_t *p, int track, int step);
/* a drum step's level moved by d (an off step comes on); returns the new level (1..X_LVL_MAX) */
int pattern_drum_nudge(pattern_t *p, int track, int step, int d);
/* a 303 step's gate on / off */
void pattern_bass_toggle(pattern_t *p, int step);
/* a 303 step's note moved by d semitones (clamped to lo..hi); an off step comes on */
void pattern_bass_nudge(pattern_t *p, int step, int d, int lo, int hi);
/* a 303 step's accent / slide: d > 0 steps through none -> accent -> slide -> both, d < 0 back */
void pattern_bass_flags(pattern_t *p, int step, int d);
/* the drums and the 303 line cleared (lengths and TB-3PO settings kept) */
void pattern_clear(pattern_t *p);
