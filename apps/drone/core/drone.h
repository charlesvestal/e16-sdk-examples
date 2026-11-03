#ifndef DRONE_H
#define DRONE_H
#include <stdint.h>

/*
 * Drone: an evolving drone synth, the DSP of apps/drone. Portable C, no
 * api calls, no libc, no libm: host-tested by test/drone/drone_test.c.
 *
 *   6 oscillators (sine -> triangle -> saw morph, PolyBLEP saw) on a
 *   chord over the root, each detuned by a fixed spread plus its own slow
 *   random drift, with slowly breathing levels
 *   + a sine sub an octave down + "air" (high-passed noise)
 *   -> drive (soft saturation) -> resonant multimode filter (TPT state
 *   variable: low-pass -> band-pass -> high-pass morph) with a slow LFO
 *   and random drift on the cutoff
 *   -> "space" (a 4-line feedback delay network with input diffusion and
 *   damping: a small, dark reverb; held, it freezes)
 *   -> level -> limiter (peak follower) -> soft clip: always within +-1.
 *
 * "Evolve" wanders the macros themselves (cutoff, morph, resonance,
 * detune, space, voice levels, and at the top of its range the voices'
 * octaves) over tens of seconds; "hold" freezes the wandering.
 *
 * Threads: drone_render() runs in the audio callback (an interrupt);
 * drone_set(), drone_note() and the other setters run in the app's main
 * flow and only write single 32-bit words, which the renderer picks up at
 * its next block. Rendering works in blocks of DRONE_CTRL samples (the
 * control rate: parameters, LFOs, pitch), any n.
 */

#define DRONE_VOICES 6
#define DRONE_CTRL 32          /* samples per control-rate step */

/* The macros (the 16 encoders, in order), values 0-127 except as noted. */
typedef enum {
    DP_ROOT,        /* MIDI note 24-84 */
    DP_CHORD,       /* 0 .. DRONE_CHORDS-1 */
    DP_DETUNE,      /* fixed spread between the voices, 0 .. 40 cents */
    DP_MORPH,       /* sine .. triangle .. saw */
    DP_CUTOFF,      /* 30 Hz .. 12 kHz */
    DP_RESO,        /* Q 0.5 .. ~16 */
    DP_MODE,        /* low-pass .. band-pass .. high-pass */
    DP_DRIVE,       /* saturation before the filter */
    DP_DRIFT_RATE,  /* LFO and drift speed, ~0.01 .. 2 Hz */
    DP_DRIFT_DEPTH, /* pitch drift (cents) and cutoff LFO depth */
    DP_EVOLVE,      /* how far the macros wander */
    DP_LEVEL,
    DP_SPACE,       /* reverb mix */
    DP_FEEDBACK,    /* reverb decay */
    DP_SUB,         /* sine an octave below the root */
    DP_AIR,         /* noise */
    DP_COUNT
} drone_param_t;

#define DRONE_CHORDS 12
#define DRONE_ROOT_MIN 24
#define DRONE_ROOT_MAX 84

typedef struct {
    const char *name;   /* short, for the screen */
    int8_t lo, hi;      /* value range */
    int8_t def;         /* default */
    uint8_t cc;         /* MIDI CC that sets it (0 = none) */
} drone_param_info_t;

extern const drone_param_info_t drone_params[DP_COUNT];
extern const char *const drone_chord_names[DRONE_CHORDS];

/* Toggles (drone_set_flag). */
typedef enum {
    DF_HOLD,        /* freeze the evolve walks */
    DF_FREEZE,      /* space: infinite hold, no new input */
    DF_MUTE,        /* fade the output out (and back in) */
    DF_OPEN,        /* open voicing: alternate voices an octave up */
    DF_COUNT
} drone_flag_t;

/* Scope capture (for the screen): drone_scope_arm() asks for
 * DRONE_SCOPE_N samples, every `decimate`-th output sample from a rising
 * zero crossing on; drone_scope_ready() is true once they're in. */
#define DRONE_SCOPE_N 128

typedef struct {
    float phase, inc;       /* oscillator, cycles */
    float cents;            /* current drift offset */
    float drift_target;     /* random target, cents (-1..1 scaled) */
    float level, level_target;
    float oct, oct_target;  /* evolve's octave hop, semitones (glides) */
} drone_voice_t;

/* A slow random walk: glides towards a new random target every so often. */
typedef struct {
    float value, target;
    float t;                /* 0..1 through the current segment */
} drone_walk_t;

/* The filter's per-sample coefficients at a control step's ends:
 * a1..a3 the TPT state-variable filter's, and the mode mix as
 * y = cx * x + c1 * band + c2 * low (low-pass, k * band-pass and
 * high-pass = x - k * band - low, weighted). */
typedef struct {
    float a1, a2, a3, cx, c1, c2;
} drone_filt_t;

#define DRONE_FDN_LINES 4
#define DRONE_FDN_LEN0 1123
#define DRONE_FDN_LEN1 1601
#define DRONE_FDN_LEN2 2069
#define DRONE_FDN_LEN3 2777
#define DRONE_AP_LEN0 142
#define DRONE_AP_LEN1 379

typedef struct {
    float fs;

    /* written by the main flow (single words) */
    volatile int32_t p[DP_COUNT];
    volatile int32_t flag[DF_COUNT];
    volatile int32_t swell_seq, swell_vel;

    /* control rate */
    uint32_t rng;
    float root;             /* gliding root, semitones */
    float lfo_phase;
    float swell;
    int32_t swell_seen;
    float mute_gain;
    drone_voice_t v[DRONE_VOICES];
    float sub_phase, sub_inc;
    drone_walk_t walk[6];   /* evolve: cutoff, morph, reso, detune, space, octave */
    float cut_drift_target, cut_drift;

    /* per-block values the sample loop uses */
    float drive, drive_comp;
    float morph;            /* 0..2 */
    float vgain[DRONE_VOICES];
    float sub_gain, air_gain;
    float wet, dry, fb, damp, in_gain;
    float out_gain;

    /* the filter's g and k at the last step; the level, smoothed per sample */
    float g_s, k_s, out_s;
    drone_filt_t f0, f1;    /* this control step's start and end */

    /* filter state */
    float ic1, ic2;
    float air_lp;

    /* space */
    float ap0[DRONE_AP_LEN0], ap1[DRONE_AP_LEN1];
    int ap_i0, ap_i1;
    float l0[DRONE_FDN_LEN0], l1[DRONE_FDN_LEN1], l2[DRONE_FDN_LEN2], l3[DRONE_FDN_LEN3];
    int li[DRONE_FDN_LINES];
    float lp[DRONE_FDN_LINES];

    /* limiter */
    float env;

    /* where the control step is (samples left in it) */
    int ctrl_left;

    /* scope */
    volatile int32_t scope_state;   /* 0 idle, 1 armed, 2 capturing, 3 ready */
    volatile int32_t scope_decimate;
    int scope_n, scope_cnt;
    float scope_prev;
    int8_t scope[DRONE_SCOPE_N];

    /* live values for the screen and rings (renderer writes) */
    volatile float live_cutoff_hz;
    volatile float live_morph;      /* 0..2 */
    volatile float live_space;      /* 0..1 */
    volatile float live_peak;       /* output peak, decaying */
    volatile float live_voice_level[DRONE_VOICES];
    volatile float live_voice_cents[DRONE_VOICES];
    volatile float live_root;       /* semitones (gliding) */
} drone_t;

void drone_init(drone_t *d, float fs, uint32_t seed);

/* Main flow: a macro (clamped to its range), a toggle. */
void drone_set(drone_t *d, int param, int value);
int drone_get(const drone_t *d, int param);
void drone_set_flag(drone_t *d, int flag, int on);
int drone_get_flag(const drone_t *d, int flag);

/* Main flow: a MIDI note: the root follows it (gliding), and velocity
 * swells brightness and level for a few seconds. */
void drone_note(drone_t *d, int note, int velocity);

/* Audio: n samples into out, each within [-1, 1]. */
void drone_render(drone_t *d, float *out, int n);

/* Scope (main flow). */
void drone_scope_arm(drone_t *d, int decimate);
int drone_scope_ready(const drone_t *d);
const int8_t *drone_scope_data(const drone_t *d);

/* Helpers the app shares. */
float drone_exp2(float x);                 /* 2^x, ~1e-5 relative */
float drone_note_hz(float note);           /* MIDI note to Hz */
float drone_cutoff_hz(int value);          /* DP_CUTOFF's value to Hz */
float drone_detune_cents(int value);       /* DP_DETUNE's value to cents */
float drone_drift_hz(int value);           /* DP_DRIFT_RATE's value to Hz */

#endif
