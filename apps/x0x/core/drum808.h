/* SPDX-License-Identifier: GPL-3.0-only */
/* x0x-lite 808 kit: seven of X0X's 808 voices (dsp/drum808, itself a port of the 8W8 TR-808
 * engine by Charles Vestal and contributors, GPL-3.0), for the E16.
 *
 * Same circuit models, pot ranges, defaults, kit trims, velocity law and hat choke as X0X's
 * drum808. What is NOT here, to fit the E16 app (and why):
 *  - The tracks: bass drum, snare, low tom, clap, closed and open hat, cowbell. The mid / high
 *    toms, the congas, rim shot / claves, maracas and cymbal are gone (code and CPU).
 *  - Per-sound drive / distortion, the reverb and delay sends and the pan: no FX bus here.
 *  - The float-float arithmetic X0X used to match 8W8's double results bit for bit at trigger
 *    time: the pitches and pot curves use fastmath's exp2 / pow (rel 1e-5), which no one hears.
 *  - The metal bank's six squares are plain (no polyBLEP) and their drift / jitter is applied to
 *    the phase increment when it changes (every 256 samples, and at each square's wrap) rather
 *    than every sample: the bank was most of the hats' and cowbell's cost on the E16 (measured,
 *    docs/x0x.md). What aliases is inharmonic metal under the hats' 2.7-6 kHz high-passes.
 *  - The op-amp saturation (bass drum, tom) is a shorter series where it is nearly linear.
 *  - The rate: 48 kHz. Every coefficient X0X precomputed for 44.1 kHz is converted (a decay
 *    multiplier c becomes c^(44100/48000); a one-pole step k becomes 1 - (1-k)^(44100/48000)),
 *    and the 1 ms trigger pulse and the clap's burst spacing are counted at 48 kHz.
 *
 * Mono, C99, float, no libm, no allocation. Idle voices cost nothing; the shared metal bank
 * (six Schmitt squares feeding CH, OH and CB) runs only while one of those is sounding.
 *
 * Threading: drum808_trigger / drum808_set are called between render blocks (x0x-lite: all
 * from the audio callback; x0x.c queues the main flow's changes). */
#pragma once
#include <stdint.h>
#include "x0x_param.h"

enum { D8_BD, D8_SD, D8_LT, D8_CP, D8_CH, D8_OH, D8_CB, D8_NUM };

/* per-track pots */
enum { D8P_LEVEL, D8P_TUNE, D8P_DECAY, D8P_X1, D8P_X2, D8P_NUM };   /* X1/X2: BD tone/attack, SD tone/snappy */

#define D8_SR 48000.0f

/* ---- DSP primitives ---------------------------------------------------- */
/* Biquads are transposed direct form II with the denominator stored as its
 * distance from (1 - z^-1)^2: a1 = -2 + e1, a2 = 1 + e2 (X0X: float keeps the low
 * resonances' coefficients exact this way). */
typedef struct { float b0, e1, e2, z1, z2; } d8_bp_t;               /* constant-peak bandpass */
typedef struct { float n0, n1, n2, e1, e2, z1, z2; } d8_bq_t;       /* general biquad */
typedef struct { float a, z, y; } d8_hp1_t;                         /* one-pole highpass */
typedef struct { uint32_t s1, s2, s3; } d8_rng_t;                   /* SuperCollider taus88 */

typedef struct {
    float f0, loop_g, forward, tune_trim, atk_amt, atk_env, accent_v;
    float fb, tone_c, tone_z, dc_z, sigh_env, shp;
    d8_bp_t bt;
    int32_t gate, coef_age, quiet;
    uint8_t active;
} d8_bd_t;

typedef struct {
    float mix1, mix2, accent_v, noise_env, noise_decay, shp;
    d8_bp_t bt1, bt2;
    d8_bq_t noise_hp, out_hp;
    d8_rng_t rng;
    int32_t gate, quiet;
    uint8_t active;
} d8_sd_t;

typedef struct {
    float f0, loop_g, accent_v, fwd, pitch_env, skin_env, skin_lp_c, skin_lp_z;
    float strike_lp_z, fb, dc_z, shp;
    d8_bp_t bt;
    d8_rng_t rng;
    int32_t gate, coef_age, quiet;
    uint8_t active;
} d8_tom_t;

typedef struct {
    float tooth, tooth_d, main_level, main_d, main_target, floor_level, floor_d;
    d8_bp_t bp;
    d8_bq_t hp;
    d8_rng_t rng;
    int32_t fired, age, quiet;
    uint8_t active;
} d8_cp_t;

typedef struct {                    /* six Schmitt squares, 0.32 fixed-point phases */
    uint32_t ph[6], inc[6], dt[6];  /* dt: inc with the drift and jitter applied (x0x-lite) */
    float drift[6], jm1[6];         /* jitter is 1 + jm1 */
    d8_rng_t rng;
    int32_t drift_cnt;
    uint8_t pend_wrap;              /* phases that start at >= 1.0 in 8W8 */
} d8_bank_t;

typedef struct { float v, target, up, down; } d8_menv_t;

typedef struct {
    d8_bq_t ska, skb;
    d8_hp1_t dc;
    d8_menv_t env;
    float lp_c, lp[5], d1, d2, out_scale;
    float lin_level, lin_step, lin_target, lin_base;
    int32_t lin_hold, lin_n, quiet;
    uint8_t mode, linear, ramp, active;
} d8_hat_t;

typedef struct {
    d8_bp_t bp;
    d8_hp1_t hp, bla, blb;
    float clank, body, body_d, ratio;
    int32_t quiet;
    uint8_t active;
} d8_cb_t;

/* one output lane per track: the hit's gain, the choke fade, the tail detector */
typedef struct {
    float hit, cg, cstep;
    float pk;                         /* the hit's peak so far; the tail ends 60 dB under it */
    int32_t qn;                       /* samples in a row under that */
    uint32_t serial;                  /* when it was triggered (the voice cap's "oldest") */
} d8_lane_t;

typedef struct drum808 {
    uint8_t pot[D8_NUM][D8P_NUM];
    uint8_t kit_level;                /* 0..127 */
    float potv[D8_NUM][D8P_NUM];      /* engineering values of pot[] */
    float vol;
    /* x0x-lite's CPU guard (drum808_set_guard): at most this many voices at once, the oldest
     * fading out (2 ms) when one more starts; a tail ends this far under the hit's peak */
    int32_t max_voices;
    float tail_k;
    uint32_t serial;

    d8_bd_t bd;
    d8_sd_t sd;
    d8_tom_t lt;
    d8_cp_t cp;
    d8_bank_t bank;
    d8_cb_t cb;
    d8_hat_t ch, oh;
    d8_lane_t lane[D8_NUM];
    /* render scratch, here rather than on the stack (it runs on the app's stack, in the audio
     * interrupt): one voice's chunk, the metal bank's bus and its cowbell pair */
    float buf[32], bus[32], pair[32];
} drum808_t;

void drum808_init(drum808_t *d);                          /* 8W8 defaults */
void drum808_trigger(drum808_t *d, int track, float vel); /* vel 0..1, 1.0 = accent */
/* ADDS this block (mono) into out */
void drum808_render(drum808_t *d, float *out, int n);
int drum808_nparams(int track);
const x0x_param_t *drum808_param(int track, int i);
void drum808_set(drum808_t *d, int track, int i, int value);
int drum808_get(const drum808_t *d, int track, int i);
void drum808_set_kit_level(drum808_t *d, int v);
/* the CPU guard: at most max_voices drum voices at once (1..D8_NUM), tails cut at tail_db
 * under each hit's peak (X0X: 60 dB) */
void drum808_set_guard(drum808_t *d, int max_voices, int tail_db);
/* true while any voice is still sounding */
int drum808_active(const drum808_t *d);
/* the 0..1 velocity of an unaccented 808 hit (8W8: the 4 V trigger floor) */
#define D8_VEL_NORMAL 0.502f
