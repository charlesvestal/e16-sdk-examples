#ifndef SYNTH_VOICE_H
#define SYNTH_VOICE_H
#include <stdint.h>

/*
 * A monophonic 303-style acid voice: pure DSP, no hardware, no malloc,
 * float only (the device has a single-precision FPU; see voice.c for the
 * design notes and the per-sample cost).
 *
 *   saw/square oscillator (PolyBLEP) -> 4-pole resonant low-pass (TPT
 *   ladder, soft-clipped feedback, stable into self-oscillation) -> VCA
 *
 * Envelopes: a filter envelope (instant attack, exponential decay set by
 * VOICE_P_DECAY) and an amp envelope (1 ms attack, slower exponential
 * decay while the gate is held, ~10 ms release on note-off).
 *
 * Mono legato like a 303: a note-on while another note is held, with
 * VOICE_P_SLIDE on, glides the pitch over the glide time and doesn't
 * retrigger the envelopes. Velocity >= VOICE_ACCENT_VELOCITY is an accent:
 * more filter envelope (and a shorter filter decay) plus a louder note.
 *
 * Threading: voice_render() is meant to run in the audio interrupt.
 * voice_param() only writes single floats/ints the renderer reads (the
 * renderer smooths cutoff, resonance and volume itself), so a parameter
 * change from the main loop can't tear. Note on/off touch several fields
 * and must be serialized with voice_render() by the caller.
 */

#define VOICE_ACCENT_VELOCITY 100
#define VOICE_HELD_MAX 8

typedef enum {
    VOICE_P_CUTOFF,     /* 0..127: base cutoff, 50 Hz .. ~9 kHz (exponential) */
    VOICE_P_RESONANCE,  /* 0..127: 127 is just past self-oscillation */
    VOICE_P_ENV_MOD,    /* 0..127: filter envelope depth, 0 .. 5 octaves */
    VOICE_P_DECAY,      /* 0..127: filter envelope decay, 60 ms .. 2.5 s (to -60 dB) */
    VOICE_P_WAVEFORM,   /* < 64 saw, >= 64 square */
    VOICE_P_VOLUME,     /* 0..127 */
    VOICE_P_SLIDE,      /* >= 64 on: overlapping notes glide (legato) */
    VOICE_P_GLIDE_TIME, /* 0..127: 2 ms .. 502 ms, quadratic; 44 ~ 60 ms (a 303's) */
    VOICE_P_COUNT
} voice_param_id_t;

typedef struct {
    float fs;
    float pi_over_fs;
    float fc_max_oct;   /* log2 of the highest cutoff, 0.42 fs */
    float smooth;       /* one-pole coefficient for parameter smoothing (~5 ms) */
    uint8_t param[VOICE_P_COUNT];

    /* derived from params (written by voice_param) */
    float cut_oct_target; /* log2(Hz) */
    float k_target;       /* ladder feedback, 0 .. ~4.2 */
    float vol_target;
    float env_depth_oct;
    float fenv_coef;        /* per-sample decay multiplier */
    float fenv_coef_accent;
    float amp_decay_coef;
    int square;
    int slide;
    int glide_samples;

    /* smoothed */
    float cut_oct, k, vol;

    /* oscillator and pitch */
    float phase, dt;
    float note_cur, note_target, note_step;
    int glide_left;

    /* envelopes */
    float fenv_lvl;   /* decaying envelope level */
    float fenv;       /* fenv_lvl smoothed (~0.3 ms) so the attack doesn't click */
    float fenv_att;   /* coefficient for that smoothing */
    float fenv_k;     /* the decay coefficient of the current note */
    float amp, amp_peak, amp_att_step, amp_rel_coef;
    int amp_stage;    /* VOICE_AMP_* in voice.c */
    int accent;

    /* filter state */
    float s[4];

    /* held notes, oldest first; the last one sounds */
    uint8_t held[VOICE_HELD_MAX];
    int nheld;

    /* diagnostics: largest |sample| before the final clip to +-1 */
    float peak_preclip;
} voice_t;

void voice_init(voice_t *v, float sample_rate);
void voice_note_on(voice_t *v, uint8_t note, uint8_t velocity); /* velocity 0 = note off */
void voice_note_off(voice_t *v, uint8_t note);
void voice_all_notes_off(voice_t *v); /* release whatever is sounding */
void voice_param(voice_t *v, voice_param_id_t id, uint8_t value); /* value 0..127 */
void voice_render(voice_t *v, float *out, int n); /* out in [-1, 1] */

/* The glide time VOICE_P_GLIDE_TIME currently maps to, in seconds. */
float voice_glide_time_s(const voice_t *v);
/* True while the voice makes sound (gate held or still releasing). */
int voice_active(const voice_t *v);

#endif
