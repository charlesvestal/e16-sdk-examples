/*
 * Drone's DSP: drone.h.
 *
 * Cost: everything that changes slowly (pitches, the filter's
 * coefficients, LFOs, drift, evolve, levels) is worked out once per
 * DRONE_CTRL samples and interpolated across them where a step could be
 * heard; the per-sample loop is six phase accumulators, the waveform
 * morph, one state-variable filter, two allpasses and a 4-line FDN. No
 * libm: 2^x is a polynomial, tan() a Pade approximant, sin() a folded
 * odd polynomial.
 *
 * Denormals: the filter and the reverb feed back, and decay towards zero
 * when the input stops (a held space, muted output). A constant 1e-20
 * added to their inputs keeps their states far from the denormal range
 * (it's -400 dB: inaudible, and a low-pass passes it as DC, which the
 * output never shows at any meaningful level).
 */
#include "drone.h"

#define PI 3.14159265f
#define ANTI_DENORMAL 1e-20f

const drone_param_info_t drone_params[DP_COUNT] = {
    [DP_ROOT] = {"Root", DRONE_ROOT_MIN, DRONE_ROOT_MAX, 38, 0},
    [DP_CHORD] = {"Chord", 0, DRONE_CHORDS - 1, 2, 20},
    [DP_DETUNE] = {"Detune", 0, 127, 40, 94},
    [DP_MORPH] = {"Wave", 0, 127, 70, 70},
    [DP_CUTOFF] = {"Cutoff", 0, 127, 70, 74},
    [DP_RESO] = {"Reso", 0, 127, 40, 71},
    [DP_MODE] = {"Mode", 0, 127, 0, 21},
    [DP_DRIVE] = {"Drive", 0, 127, 30, 22},
    [DP_DRIFT_RATE] = {"Drift rate", 0, 127, 50, 76},
    [DP_DRIFT_DEPTH] = {"Drift", 0, 127, 50, 77},
    [DP_EVOLVE] = {"Evolve", 0, 127, 50, 1},
    [DP_LEVEL] = {"Level", 0, 127, 100, 7},
    [DP_SPACE] = {"Space", 0, 127, 70, 91},
    [DP_FEEDBACK] = {"Decay", 0, 127, 90, 23},
    [DP_SUB] = {"Sub", 0, 127, 40, 24},
    [DP_AIR] = {"Air", 0, 127, 15, 25},
};

const char *const drone_chord_names[DRONE_CHORDS] = {
    "unison", "octaves", "fifths", "quartal", "sus2", "sus4",
    "minor", "major", "minor 7", "major 7", "minor 9", "cluster",
};

static const int8_t chords[DRONE_CHORDS][DRONE_VOICES] = {
    {0, 0, 0, 0, 0, 0},    {0, 12, 0, 12, -12, 24}, {0, 7, 12, 19, 0, 7},    {0, 5, 10, 12, 17, 22},
    {0, 2, 7, 12, 14, 19}, {0, 5, 7, 12, 17, 19},   {0, 3, 7, 12, 15, 19},   {0, 4, 7, 12, 16, 19},
    {0, 3, 7, 10, 12, 15}, {0, 4, 7, 11, 12, 16},   {0, 3, 7, 10, 14, 19},   {0, 2, 3, 7, 12, 14},
};

/* Each voice's share of the detune spread, and its level. */
static const float spread[DRONE_VOICES] = {0.0f, 1.0f, -1.0f, 0.55f, -0.55f, 0.3f};
static const float weight[DRONE_VOICES] = {1.0f, 0.85f, 0.85f, 0.7f, 0.7f, 0.6f};
#define VOICE_SUM_GAIN 0.2f

/* ---- math ---- */

float drone_exp2(float x)
{
    if (x < -60.0f) {
        x = -60.0f;
    } else if (x > 60.0f) {
        x = 60.0f;
    }
    int i = (int)x;
    if ((float)i > x) {
        i--; /* floor */
    }
    float f = x - (float)i;
    /* 2^f on [0, 1): Taylor to f^6, within 2e-5 */
    float p = 1.0f +
              f * (0.69314718f +
                   f * (0.24022651f + f * (0.05550411f + f * (0.00961813f + f * (0.00133336f + f * 0.00015404f)))));
    union {
        float f;
        uint32_t u;
    } e;
    e.u = (uint32_t)(i + 127) << 23;
    return p * e.f;
}

float drone_note_hz(float note)
{
    return 440.0f * drone_exp2((note - 69.0f) * (1.0f / 12.0f));
}

/* sin(2 pi t), t in [0, 1): folded to a quarter wave, odd polynomial to
 * x^9 (error < 4e-6). */
static inline float sin_turn(float t)
{
    float u = t - 0.5f; /* sin(2 pi t) = -sin(2 pi u) */
    if (u > 0.25f) {
        u = 0.5f - u;
    } else if (u < -0.25f) {
        u = -0.5f - u;
    }
    float x = u * (2.0f * PI), x2 = x * x;
    float s = x * (1.0f + x2 * (-1.0f / 6.0f + x2 * (1.0f / 120.0f + x2 * (-1.0f / 5040.0f + x2 * (1.0f / 362880.0f)))));
    return -s;
}

/* tanh-like soft clip, |result| < 1 for any x (NaN aside): x(27 + x^2) /
 * (27 + 9x^2), which reaches 1 at |x| = 3, clamped there. */
static inline float rtanh(float x)
{
    if (x > 3.0f) {
        return 1.0f;
    }
    if (x < -3.0f) {
        return -1.0f;
    }
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* The output's last stage: transparent up to 0.6, then a tanh knee to 1. */
static inline float out_clip(float x)
{
    const float knee = 0.6f, room = 0.4f;
    if (x > knee) {
        return knee + room * rtanh((x - knee) * (1.0f / room));
    }
    if (x < -knee) {
        return -knee - room * rtanh((-x - knee) * (1.0f / room));
    }
    return x;
}

static inline float fabs_(float x)
{
    return x < 0.0f ? -x : x;
}

static inline float clampf(float x, float lo, float hi)
{
    return x < lo ? lo : x > hi ? hi : x;
}

static inline uint32_t rnd(drone_t *d)
{
    uint32_t x = d->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    d->rng = x;
    return x;
}

/* uniform in [-1, 1) */
static inline float rndf(drone_t *d)
{
    return (float)(int32_t)rnd(d) * (1.0f / 2147483648.0f);
}

static inline float norm(const drone_t *d, int p)
{
    return (float)d->p[p] * (1.0f / 127.0f);
}

float drone_cutoff_hz(int value)
{
    /* 30 Hz .. 12 kHz: log2 4.9 .. 13.55 */
    return drone_exp2(4.907f + (float)value * (8.644f / 127.0f));
}

float drone_detune_cents(int value)
{
    float v = (float)value * (1.0f / 127.0f);
    return 40.0f * v * v;
}

float drone_drift_hz(int value)
{
    /* 0.01 .. 2 Hz, exponential */
    return 0.01f * drone_exp2((float)value * (7.64f / 127.0f));
}

/* ---- setup and the main flow's setters ---- */

void drone_init(drone_t *d, float fs, uint32_t seed)
{
    uint8_t *b = (uint8_t *)d;
    for (uint32_t i = 0; i < sizeof *d; i++) {
        b[i] = 0;
    }
    d->fs = fs;
    d->rng = seed ? seed : 0x1234567u;
    for (int p = 0; p < DP_COUNT; p++) {
        d->p[p] = drone_params[p].def;
    }
    d->root = (float)d->p[DP_ROOT];
    d->mute_gain = 1.0f;
    for (int i = 0; i < DRONE_VOICES; i++) {
        drone_voice_t *v = &d->v[i];
        v->phase = (float)(rnd(d) >> 8) * (1.0f / 16777216.0f);
        v->level = v->level_target = 1.0f;
    }
    for (int w = 0; w < 6; w++) {
        d->walk[w].t = (float)(rnd(d) >> 8) * (1.0f / 16777216.0f);
    }
    d->g_s = 0.1f;
    d->k_s = 1.0f;
    d->ctrl_left = 0;
}

void drone_set(drone_t *d, int param, int value)
{
    if (param < 0 || param >= DP_COUNT) {
        return;
    }
    const drone_param_info_t *pi = &drone_params[param];
    d->p[param] = value < pi->lo ? pi->lo : value > pi->hi ? pi->hi : value;
}

int drone_get(const drone_t *d, int param)
{
    return param >= 0 && param < DP_COUNT ? (int)d->p[param] : 0;
}

void drone_set_flag(drone_t *d, int flag, int on)
{
    if (flag >= 0 && flag < DF_COUNT) {
        d->flag[flag] = on ? 1 : 0;
    }
}

int drone_get_flag(const drone_t *d, int flag)
{
    return flag >= 0 && flag < DF_COUNT ? (int)d->flag[flag] : 0;
}

void drone_note(drone_t *d, int note, int velocity)
{
    while (note < DRONE_ROOT_MIN) {
        note += 12;
    }
    while (note > DRONE_ROOT_MAX) {
        note -= 12;
    }
    d->p[DP_ROOT] = note;
    if (velocity > 0) {
        d->swell_vel = velocity > 127 ? 127 : velocity;
        d->swell_seq++;
    }
}

void drone_scope_arm(drone_t *d, int decimate)
{
    d->scope_decimate = decimate < 1 ? 1 : decimate;
    d->scope_state = 1;
}

int drone_scope_ready(const drone_t *d)
{
    return d->scope_state == 3;
}

const int8_t *drone_scope_data(const drone_t *d)
{
    return d->scope;
}

/* ---- control rate ---- */

/* Advances a walk by dt seconds at `rate` segments a second. */
static void walk_step(drone_t *d, drone_walk_t *w, float rate, float dt)
{
    w->t += rate * dt;
    if (w->t >= 1.0f) {
        w->t -= 1.0f;
        if (w->t >= 1.0f) {
            w->t = 0.0f;
        }
        w->target = rndf(d);
    }
    /* glide over about a third of a segment */
    float c = clampf(3.0f * rate * dt, 0.0f, 1.0f);
    w->value += (w->target - w->value) * c;
}

static void control(drone_t *d)
{
    const float dt = (float)DRONE_CTRL / d->fs;
    float evolve = norm(d, DP_EVOLVE);
    int hold = d->flag[DF_HOLD];

    /* evolve: six slow walks, depth by evolve; held, they stop */
    if (!hold) {
        static const float rate_mul[6] = {1.0f, 0.77f, 1.31f, 0.59f, 0.89f, 0.43f};
        float base = 0.025f + 0.09f * evolve;
        for (int w = 0; w < 6; w++) {
            float before = d->walk[w].t;
            walk_step(d, &d->walk[w], base * rate_mul[w], dt);
            /* the octave walk's new segments occasionally move a voice
             * an octave (evolve above half) */
            if (w == 5 && d->walk[w].t < before) {
                if (evolve > 0.5f && (float)(rnd(d) >> 8) * (1.0f / 16777216.0f) < (evolve - 0.5f) * 1.6f) {
                    int i = 2 + (int)(rnd(d) % 4u);
                    uint32_t r = rnd(d) % 4u;
                    d->v[i].oct_target = r == 0 ? -12.0f : r == 1 ? 12.0f : 0.0f;
                }
            }
        }
    }
    if (evolve <= 0.5f) {
        for (int i = 0; i < DRONE_VOICES; i++) {
            d->v[i].oct_target = 0.0f;
        }
    }
    float ev_cut = d->walk[0].value * evolve * 1.6f;   /* octaves */
    float ev_morph = d->walk[1].value * evolve * 0.7f;
    float ev_reso = d->walk[2].value * evolve * 0.25f;
    float ev_detune = (d->walk[3].value * 0.5f + 0.5f) * evolve * 12.0f; /* cents, added */
    float ev_space = d->walk[4].value * evolve * 0.25f;

    /* swell: a note's velocity, decaying over ~2.5 s */
    if (d->swell_seq != d->swell_seen) {
        d->swell_seen = d->swell_seq;
        float s = (float)d->swell_vel * (1.0f / 127.0f);
        if (s > d->swell) {
            d->swell = s;
        }
    }
    d->swell *= 1.0f - dt * 0.4f;

    /* drift: per-voice random pitch, the cutoff LFO and its random part */
    float rate = drone_drift_hz(d->p[DP_DRIFT_RATE]);
    float depth = norm(d, DP_DRIFT_DEPTH);
    float drift_cents = 25.0f * depth * depth;
    d->lfo_phase += rate * dt * 0.5f;
    if (d->lfo_phase >= 1.0f) {
        d->lfo_phase -= 1.0f;
    }
    float lfo = sin_turn(d->lfo_phase);
    if ((float)(rnd(d) >> 8) * (1.0f / 16777216.0f) < rate * dt) {
        d->cut_drift_target = rndf(d);
    }
    d->cut_drift += (d->cut_drift_target - d->cut_drift) * clampf(rate * dt * 2.0f, 0.0f, 1.0f);

    /* pitches */
    int chord = d->p[DP_CHORD];
    if (chord < 0 || chord >= DRONE_CHORDS) {
        chord = 0;
    }
    float root_target = (float)d->p[DP_ROOT];
    float glide = clampf(dt / 0.35f, 0.0f, 1.0f);
    float oct_glide = clampf(dt / 2.0f, 0.0f, 1.0f);
    d->root += (root_target - d->root) * glide;
    float detune = drone_detune_cents(d->p[DP_DETUNE]) + ev_detune;
    int open = d->flag[DF_OPEN];
    for (int i = 0; i < DRONE_VOICES; i++) {
        drone_voice_t *v = &d->v[i];
        if ((float)(rnd(d) >> 8) * (1.0f / 16777216.0f) < rate * dt * 1.5f) {
            v->drift_target = rndf(d);
        }
        v->cents += (v->drift_target * drift_cents - v->cents) * clampf(rate * dt * 3.0f, 0.0f, 1.0f);
        v->oct += (v->oct_target - v->oct) * oct_glide;
        float note = d->root + (float)chords[chord][i] + v->oct + (open && (i & 1) ? 12.0f : 0.0f) +
                     (detune * spread[i] + v->cents) * 0.01f;
        float hz = drone_note_hz(note);
        if (hz > d->fs * 0.45f) {
            hz = d->fs * 0.45f;
        }
        v->inc = hz / d->fs;
        /* breathing levels: a slow random target each, deeper with evolve */
        if ((float)(rnd(d) >> 8) * (1.0f / 16777216.0f) < dt * 0.15f) {
            v->level_target = 1.0f - (0.25f + 0.6f * evolve) * (0.5f + 0.5f * rndf(d));
        }
        v->level += (v->level_target - v->level) * clampf(dt * 0.4f, 0.0f, 1.0f);
        d->vgain[i] = weight[i] * v->level * VOICE_SUM_GAIN;
        d->live_voice_level[i] = v->level;
        d->live_voice_cents[i] = v->cents + detune * spread[i];
    }
    d->sub_inc = drone_note_hz(d->root - 12.0f) / d->fs;
    float sub = norm(d, DP_SUB);
    d->sub_gain = 0.45f * sub * sub;
    float air = norm(d, DP_AIR);
    d->air_gain = 0.5f * air * air;

    /* waveform */
    d->morph = clampf(norm(d, DP_MORPH) * 2.0f + ev_morph, 0.0f, 2.0f);

    /* drive: 0.7 .. ~8x into the soft clip, made up by gain^-0.6 */
    float drive_oct = norm(d, DP_DRIVE) * 3.5f;
    d->drive = 0.7f * drone_exp2(drive_oct);
    d->drive_comp = (1.0f / 0.7f) * drone_exp2(-0.6f * drive_oct) * 0.85f;

    /* filter */
    float cut_oct = 4.907f + (float)d->p[DP_CUTOFF] * (8.644f / 127.0f);
    cut_oct += lfo * depth * 1.2f + d->cut_drift * depth * 0.8f + ev_cut + d->swell * 2.0f;
    cut_oct = clampf(cut_oct, 4.32f, 14.0f); /* 20 Hz .. 16 kHz */
    float fc = drone_exp2(cut_oct);
    if (fc > d->fs * 0.35f) {
        fc = d->fs * 0.35f;
    }
    d->live_cutoff_hz = fc;
    float x = PI * fc / d->fs, x2 = x * x;
    float g = x * (15.0f - x2) / (15.0f - 6.0f * x2);
    float reso = clampf(norm(d, DP_RESO) + ev_reso, 0.0f, 1.0f);
    float k = 2.0f * drone_exp2(-5.06f * reso); /* 2 (Q 0.5) .. 0.06 (Q 16) */
    float mode = norm(d, DP_MODE) * 2.0f;

    /* from the previous step's values to these, across the step */
    float gg[2] = {d->g_s, g}, kk[2] = {d->k_s, k};
    d->g_s = g;
    d->k_s = k;
    drone_filt_t *ff[2] = {&d->f0, &d->f1};
    float wl = mode < 1.0f ? 1.0f - mode : 0.0f;
    float wb = mode < 1.0f ? mode : 2.0f - mode;
    float wh = mode > 1.0f ? mode - 1.0f : 0.0f;
    for (int j = 0; j < 2; j++) {
        float a1 = 1.0f / (1.0f + gg[j] * (gg[j] + kk[j]));
        ff[j]->a1 = a1;
        ff[j]->a2 = gg[j] * a1;
        ff[j]->a3 = gg[j] * gg[j] * a1;
        /* wl * low + wb * k * band + wh * (x - k * band - low) */
        ff[j]->cx = wh;
        ff[j]->c1 = (wb - wh) * kk[j];
        ff[j]->c2 = wl - wh;
    }

    /* space */
    float space = clampf(norm(d, DP_SPACE) + ev_space, 0.0f, 1.0f);
    d->live_space = space;
    d->wet = space * 0.9f;
    d->dry = 1.0f - space * 0.45f;
    float fbn = norm(d, DP_FEEDBACK);
    if (d->flag[DF_FREEZE]) {
        d->fb = 1.0f;
        d->damp = 1.0f;
        d->in_gain += (0.0f - d->in_gain) * clampf(dt / 0.05f, 0.0f, 1.0f);
    } else {
        d->fb = 0.35f + 0.635f * fbn;     /* .. 0.985 */
        d->damp = 0.55f - 0.25f * fbn;    /* darker as it gets longer */
        d->in_gain += (0.5f - d->in_gain) * clampf(dt / 0.05f, 0.0f, 1.0f);
    }

    /* level, swell, mute */
    float lvl = norm(d, DP_LEVEL);
    float mute_target = d->flag[DF_MUTE] ? 0.0f : 1.0f;
    d->mute_gain += (mute_target - d->mute_gain) * clampf(dt / 0.4f, 0.0f, 1.0f);
    d->out_gain = 1.6f * lvl * lvl * (1.0f + 0.5f * d->swell) * d->mute_gain;

    d->live_morph = d->morph;
    d->live_root = d->root;
}

/* ---- the sample loop ---- */

static inline float poly_blep(float t, float dt, float inv_dt)
{
    if (t < dt) {
        t *= inv_dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) * inv_dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

static void render_step(drone_t *d, float *out, int n, int at)
{
    const drone_filt_t *f0 = &d->f0, *f1 = &d->f1;
    /* the filter's coefficients, interpolated across the control step;
     * `at` = how far into it this call starts */
    const float inv = 1.0f / (float)DRONE_CTRL;
    float fr = (float)at * inv;
    float a1 = f0->a1 + (f1->a1 - f0->a1) * fr, da1 = (f1->a1 - f0->a1) * inv;
    float a2 = f0->a2 + (f1->a2 - f0->a2) * fr, da2 = (f1->a2 - f0->a2) * inv;
    float a3 = f0->a3 + (f1->a3 - f0->a3) * fr, da3 = (f1->a3 - f0->a3) * inv;
    float cx = f0->cx + (f1->cx - f0->cx) * fr, dcx = (f1->cx - f0->cx) * inv;
    float c1 = f0->c1 + (f1->c1 - f0->c1) * fr, dc1 = (f1->c1 - f0->c1) * inv;
    float c2 = f0->c2 + (f1->c2 - f0->c2) * fr, dc2 = (f1->c2 - f0->c2) * inv;

    float morph = d->morph;
    int saw_side = morph > 1.0f;
    float mw = saw_side ? morph - 1.0f : morph; /* share of the second shape */
    float inc[DRONE_VOICES], inv_inc[DRONE_VOICES], ph[DRONE_VOICES], vg[DRONE_VOICES];
    for (int i = 0; i < DRONE_VOICES; i++) {
        inc[i] = d->v[i].inc;
        inv_inc[i] = 1.0f / inc[i];
        ph[i] = d->v[i].phase;
        vg[i] = d->vgain[i];
    }
    float sub_ph = d->sub_phase, sub_inc = d->sub_inc, sub_g = d->sub_gain;
    float air_g = d->air_gain;
    float drive = d->drive, comp = d->drive_comp;
    float ic1 = d->ic1, ic2 = d->ic2;
    float air_lp = d->air_lp;
    float wet = d->wet, dry = d->dry, fb = d->fb, damp = d->damp, in_gain = d->in_gain;
    float out_g = d->out_s, out_target = d->out_gain;
    float env = d->env;
    float peak = d->live_peak;

    for (int s = 0; s < n; s++) {
        /* oscillators */
        float sum = 0.0f;
        for (int i = 0; i < DRONE_VOICES; i++) {
            float t = ph[i] + inc[i];
            if (t >= 1.0f) {
                t -= 1.0f;
            }
            ph[i] = t;
            /* triangle and sine peak at t = 0.25; the saw's wrap is at 0.5 */
            float tt = t + 0.25f;
            if (tt >= 1.0f) {
                tt -= 1.0f;
            }
            float tri = 1.0f - 4.0f * fabs_(tt - 0.5f);
            float o;
            if (saw_side) {
                float ts = t + 0.5f;
                if (ts >= 1.0f) {
                    ts -= 1.0f;
                }
                float saw = 2.0f * ts - 1.0f - poly_blep(ts, inc[i], inv_inc[i]);
                o = tri + (saw - tri) * mw;
            } else {
                float sn = sin_turn(t);
                o = sn + (tri - sn) * mw;
            }
            sum += o * vg[i];
        }
        sub_ph += sub_inc;
        if (sub_ph >= 1.0f) {
            sub_ph -= 1.0f;
        }
        sum += sin_turn(sub_ph) * sub_g;

        /* air: white noise, high-passed (minus a ~1.5 kHz one-pole) */
        float nz = rndf(d);
        air_lp += (nz - air_lp) * 0.18f;
        float air = (nz - air_lp) * air_g;

        /* drive, then the filter (TPT state variable) */
        float x = rtanh((sum + air * 0.7f) * drive) * comp + ANTI_DENORMAL;
        float v3 = x - ic2;
        float v1 = a1 * ic1 + a2 * v3;
        float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        float y = cx * x + c1 * v1 + c2 * v2;
        a1 += da1;
        a2 += da2;
        a3 += da3;
        cx += dcx;
        c1 += dc1;
        c2 += dc2;
        y += air * 0.15f;

        /* space: two diffusing allpasses into a 4-line FDN */
        float in = y * in_gain + ANTI_DENORMAL;
        float b0 = d->ap0[d->ap_i0];
        float w0 = in + 0.6f * b0;
        float ad = b0 - 0.6f * w0;
        d->ap0[d->ap_i0] = w0;
        if (++d->ap_i0 >= DRONE_AP_LEN0) {
            d->ap_i0 = 0;
        }
        float b1 = d->ap1[d->ap_i1];
        float w1 = ad + 0.6f * b1;
        ad = b1 - 0.6f * w1;
        d->ap1[d->ap_i1] = w1;
        if (++d->ap_i1 >= DRONE_AP_LEN1) {
            d->ap_i1 = 0;
        }

        float r0 = d->l0[d->li[0]], r1 = d->l1[d->li[1]], r2 = d->l2[d->li[2]], r3 = d->l3[d->li[3]];
        float p0 = d->lp[0] + (r0 - d->lp[0]) * damp;
        float p1 = d->lp[1] + (r1 - d->lp[1]) * damp;
        float p2 = d->lp[2] + (r2 - d->lp[2]) * damp;
        float p3 = d->lp[3] + (r3 - d->lp[3]) * damp;
        d->lp[0] = p0;
        d->lp[1] = p1;
        d->lp[2] = p2;
        d->lp[3] = p3;
        float h = 0.5f * (p0 + p1 + p2 + p3); /* Householder: I - (2/4) 1 1^T */
        d->l0[d->li[0]] = ad + fb * (p0 - h);
        d->l1[d->li[1]] = ad + fb * (p1 - h);
        d->l2[d->li[2]] = ad + fb * (p2 - h);
        d->l3[d->li[3]] = ad + fb * (p3 - h);
        if (++d->li[0] >= DRONE_FDN_LEN0) {
            d->li[0] = 0;
        }
        if (++d->li[1] >= DRONE_FDN_LEN1) {
            d->li[1] = 0;
        }
        if (++d->li[2] >= DRONE_FDN_LEN2) {
            d->li[2] = 0;
        }
        if (++d->li[3] >= DRONE_FDN_LEN3) {
            d->li[3] = 0;
        }
        float rev = 0.5f * (p0 - p1 + p2 - p3);

        /* level (smoothed), limiter, soft clip */
        out_g += (out_target - out_g) * 0.002f;
        float o = (y * dry + rev * wet) * out_g;
        float a = fabs_(o);
        env = a > env ? env + (a - env) * 0.02f : env * 0.99995f;
        if (env > 0.9f) {
            o *= 0.9f / env;
        }
        o = out_clip(o);
        out[s] = o;

        float ao = fabs_(o);
        peak = ao > peak ? ao : peak * 0.99997f;

        /* scope */
        int st = d->scope_state;
        if (st == 1) {
            if ((d->scope_prev < 0.0f && o >= 0.0f) || ++d->scope_cnt > 4800) {
                d->scope_state = 2;
                d->scope_n = 0;
                d->scope_cnt = 0;
            }
        }
        if (d->scope_state == 2) {
            if (d->scope_cnt++ % d->scope_decimate == 0) {
                d->scope[d->scope_n++] = o == o ? (int8_t)(o * 127.0f) : 0; /* (o is within +-1, NaN aside) */
                if (d->scope_n >= DRONE_SCOPE_N) {
                    d->scope_state = 3;
                    d->scope_cnt = 0;
                }
            }
        }
        d->scope_prev = o;
    }

    for (int i = 0; i < DRONE_VOICES; i++) {
        d->v[i].phase = ph[i];
    }
    d->sub_phase = sub_ph;
    d->ic1 = ic1;
    d->ic2 = ic2;
    d->air_lp = air_lp;
    d->out_s = out_g;
    d->env = env;
    d->live_peak = peak;
}

void drone_render(drone_t *d, float *out, int n)
{
    while (n > 0) {
        if (d->ctrl_left <= 0) {
            control(d);
            d->ctrl_left = DRONE_CTRL;
        }
        int m = n < d->ctrl_left ? n : d->ctrl_left;
        render_step(d, out, m, DRONE_CTRL - d->ctrl_left);
        d->ctrl_left -= m;
        out += m;
        n -= m;
    }
}
