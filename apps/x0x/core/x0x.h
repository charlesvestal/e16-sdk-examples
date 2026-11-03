/* SPDX-License-Identifier: GPL-3.0-only */
/* x0x-lite engine: the 808 kit, the 303, the sequencer and a small master, in one struct, plus
 * the saved state. Portable C (no api calls, no libm): apps/x0x/app.c runs it on the E16's
 * audio stream, test/x0x/x0x_test.c on the host.
 *
 * Threads: x0x_render() is the audio callback's. Everything else is the main flow's. The main
 * flow edits patterns in place (pattern.h says why that is safe) and changes sound parameters
 * only through x0x_set() (into the shadow copy the UI and the saved state read) and
 * x0x_flush() (onto a single-producer / single-consumer queue the callback drains at the top
 * of each block, so the engines are only ever touched from the callback). Auditions (a drum
 * hit, a 303 note while stopped) go through the same queue. MIDI clock / transport out goes
 * the other way, through a byte ring the main flow drains (x0x_midi_out()). */
#pragma once
#include <stdint.h>
#include "bass303.h"
#include "drum808.h"
#include "pattern.h"
#include "seq.h"

/* parameter targets for x0x_set / x0x_get */
enum { X_T_DRUM0 = 0, X_T_BASS = X_NDRUM, X_T_MIX, X_NTARGETS };
/* the mix's parameters */
enum { X_MIX_KIT, X_MIX_BASS, X_MIX_DRIVE, X_MIX_VOLUME, X_MIX_N };
#define X_MAXP BASS303_NPARAMS
_Static_assert((int)X_MAXP <= 16, "x0x_t.dirty is a 16-bit mask per target");
_Static_assert((int)D8P_NUM <= (int)X_MAXP && (int)X_MIX_N <= (int)X_MAXP, "every target's parameters fit pot[][X_MAXP]");

#define X_CMDQ 64                     /* commands main -> callback (power of 2) */
#define X_MIDIQ 64                    /* MIDI bytes callback -> main (power of 2) */

typedef struct {
    uint8_t kind, a, b, c;
} x0x_cmd_t;

typedef struct {
    pattern_t pat[X_NPAT];
    seq_t seq;
    bass303_t bass;
    drum808_t kit;

    /* the shadow of every sound parameter (main flow's; what the UI shows and the state saves) */
    uint8_t pot[X_NTARGETS][X_MAXP];
    uint16_t dirty[X_NTARGETS];       /* bit i: pot[t][i] not yet sent to the callback */

    /* settings the main flow keeps (saved) */
    uint8_t follow_clock;             /* 1 = MIDI clock / start / stop in drive the sequencer */

    /* the master (callback's) */
    float kit_gain, bass_gain, drive, volume;
    float lim_env;
    float bb[32], kb[32];             /* render scratch: the 303's block, the kit's (not on the stack) */

    /* main -> callback */
    volatile uint8_t guard;           /* the CPU guard's level, 0..X_GUARD_MAX (x0x_guard_step) */
    uint8_t guard_on;                 /* the level the callback applied */
    uint8_t guard_calm;               /* main: calls in a row under X_GUARD_LO */
    x0x_cmd_t cmd[X_CMDQ];
    volatile uint32_t cmd_head;       /* main writes */
    volatile uint32_t cmd_tail;       /* callback writes */
    uint8_t audition_note;            /* the 303 note an audition holds (callback's) */

    /* callback -> main */
    uint8_t mo[X_MIDIQ];
    volatile uint32_t mo_head;        /* callback writes */
    volatile uint32_t mo_tail;        /* main writes */
    volatile uint32_t mo_dropped;
    /* bit t: drum track t fired (X_NDRUM: the 303). The callback ORs bits in; the main flow takes
     * them with an atomic exchange (LDREX/STREX: an interrupt between the two makes the STREX
     * fail and it retries), so no hit is lost. */
    volatile uint32_t hits;
    /* the output's peak since the main flow last cleared it (for its log line). The read and the
     * clear aren't atomic: a block's peak that lands between them is lost, which only makes one
     * 10 s log figure a little low. */
    volatile float peak;
    volatile uint32_t blocks;         /* callback calls */
} x0x_t;

/* everything to its defaults: the factory loop in pattern 1, the factory sound, 125 BPM */
void x0x_init(x0x_t *x);
/* the audio callback: n mono samples */
void x0x_render(x0x_t *x, float *out, int n);

/* sound parameters (target X_T_*, index), shadowed; x0x_flush() sends what changed */
int x0x_nparams(int target);
const x0x_param_t *x0x_param(int target, int i);
int x0x_get(const x0x_t *x, int target, int i);
void x0x_set(x0x_t *x, int target, int i, int value);
void x0x_flush(x0x_t *x);

/* auditions (main flow): a drum hit; a 303 note on (note >= 0) or off (note < 0) */
void x0x_audition_drum(x0x_t *x, int track, float vel);
void x0x_audition_bass(x0x_t *x, int note, int accent);

/* The CPU guard. The audio callback's load depends on how many drum voices ring at once; with
 * all seven and the 303 going it is over what the E16 gives an app, and the launcher would stop
 * the stream. So the main flow feeds it the load the launcher measures (per mille, every 0.5 s)
 * and it raises a level (one a call, two over X_GUARD_JUMP or when blocks were late since the last
 * call) while the load is over X_GUARD_HI, and lowers it after 3 s under X_GUARD_LO. Level 0: all 7 voices, tails cut 60 dB down (X0X's); 1: 5 voices, 50 dB;
 * 2: 4 voices, 40 dB; 3: 3 voices, 40 dB. A voice over the limit fades out in 2 ms (the oldest).
 * Returns the new level (the old one if unchanged). */
#define X_GUARD_MAX 3
#define X_GUARD_HI 550u
#define X_GUARD_LO 400u
#define X_GUARD_JUMP 700u
int x0x_guard_step(x0x_t *x, uint32_t load_permille, int late);

/* the next MIDI byte the sequencer sent (clock, start, stop), or -1 */
int x0x_midi_out(x0x_t *x);

/* the playing pattern's 303 line: a new TB-3PO line (new seed) / a mutation */
void x0x_generate(x0x_t *x, uint32_t entropy);
void x0x_mutate(x0x_t *x);

/* ---- the saved state ---- */
#define X_STATE_MAGIC 0x31583058u     /* "X0X1" */
#define X_STATE_MAX 3072u
/* the state into buf (at most cap bytes); returns its length, or 0 if cap is too small */
uint32_t x0x_state_pack(const x0x_t *x, uint8_t *buf, uint32_t cap);
/* the state from buf (the callback must not be running, or must be stopped from playing it:
 * call before audio starts); returns 0, or -1 (nothing changed) if it isn't a valid state */
int x0x_state_unpack(x0x_t *x, const uint8_t *buf, uint32_t len);
