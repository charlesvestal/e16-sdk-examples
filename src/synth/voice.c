/*
 * 303-style mono voice. See voice.h for the interface.
 *
 * Design notes
 * ------------
 * Everything runs per sample, with no control-rate blocks, so the cost of
 * a sample is the same whether voice_render() is called with n = 1 (one
 * sample per timer interrupt) or with a block. Every float literal is
 * single precision. The only libm calls (expf, exp2f) are in voice_param()
 * and voice_init(), never in voice_render().
 *
 * Oscillator: a phase accumulator in [0, 1) with PolyBLEP corrections at the
 * discontinuities (the saw's wrap; the square's two edges). The pitch is a
 * float note number. While a glide is running it moves linearly in
 * semitones, so a slide takes exactly the glide time, and the phase
 * increment is recomputed every sample with a fast exp2 (fast_exp2 below,
 * ~0.01% error, 0.2 cents).
 *
 * Filter: a 4-pole ladder of TPT (trapezoidal, zero-delay-feedback)
 * one-poles. The feedback loop is solved linearly in one step:
 *
 *   y4 = G^4 u + S,  S = (1-G)(G^3 s0 + G^2 s1 + G s2 + s3)
 *   u  = x - k y4    =>  u = (x - k S) / (1 + k G^4)
 *
 * then u goes through a cubic soft clip before it enters the stages. The
 * stages run on the clipped u, so their state is always consistent and
 * bounded: the filter stays stable at any k, and above k = 4 it
 * self-oscillates at an amplitude the clip sets. G = g / (1 + g) with
 * g = tan(pi fc / fs) from a [5/4] Pade approximation, folded so that G
 * costs a single division (G = N / (D + N) with tan = N / D). fc is clamped
 * to 0.42 fs, where the Pade is still within 0.1% and well short of its
 * pole. Resonance's bass loss is partly made up (x (1 + k/2)), as some
 * ladder designs do.
 *
 * Envelopes: the filter envelope jumps to 1 on a trigger (through a 0.3 ms
 * smoother) and decays exponentially; the amp envelope ramps up linearly in
 * 1 ms, decays slowly while the gate is held and falls with a 1.5 ms time
 * constant on release (-60 dB in ~10 ms). Once the release is inaudible the
 * voice goes idle: the filter state is cleared and render outputs zeros at
 * almost no cost.
 *
 * Cost per sample (the active path, worst case: square + glide): about 100
 * float operations, 2 divisions (4 within dt of a PolyBLEP edge), ~6
 * float<->int conversions and ~15 compare/branches. arm-none-eabi-gcc -O2
 * makes voice_render() ~360 instructions in all, of which ~200 run on the
 * active path. On a Cortex-M4F (1-cycle VADD/VMUL, 3-cycle VFMA, 14-cycle
 * VDIV, 2-cycle VLDR, 2-4 cycles per taken branch) that's roughly 300-450
 * cycles, i.e. ~6-9% of 168 MHz at 32 kHz (budget: 1500 cycles, 28%).
 * Measured on the device (DWT_CYCCNT, port_e16_audio.c's "audio: load"
 * line, the whole interrupt handler): ~390 cycles per sample on average
 * while an acid line plays (idle gaps included), ~480 with a note held
 * throughout, 770 at most -- ~8-10% of the CPU at 32 kHz. At 48 kHz (the
 * rate since USB audio), with the output gain and the USB ring push in the
 * same handler: ~430 cycles average for an acid line, ~520 with a note
 * held, ~810 at most -- 13-16% of the CPU, 24% peak.
 */
#include "voice.h"

#include <math.h>
#include <string.h>

enum { VOICE_AMP_IDLE, VOICE_AMP_ATTACK, VOICE_AMP_DECAY, VOICE_AMP_RELEASE };

#define PI_F 3.14159265f
#define CUT_MIN_HZ 50.0f
#define CUT_RANGE_OCT 7.5f      /* 50 Hz .. ~9 kHz */
#define ENV_MAX_OCT 5.0f
#define ACCENT_OCT 1.5f         /* extra filter envelope on an accent */
#define ACCENT_DECAY_S 0.2f     /* accented notes decay at most this long */
#define K_MAX 4.2f              /* self-oscillation starts at 4 */
#define FC_MAX_FRAC 0.42f       /* of fs */
#define DRIVE 0.8f              /* oscillator level into the filter */
#define OUT_GAIN 0.8f           /* master level at volume 127 */
#define AMP_NORMAL 0.7f
#define AMP_ACCENT 1.0f
#define IDLE_LEVEL 1e-4f        /* amp below this after release: idle */

/* 2^x for |x| < ~120. Integer part into the exponent, cubic minimax for the
 * fraction (max relative error ~1e-4). */
static inline float fast_exp2(float x)
{
    int i = (int)x;
    if (x < (float)i)
        i--;
    float f = x - (float)i;
    float p = 1.0f + f * (0.69606564f + f * (0.22449433f + f * 0.07944023f));
    union { float f; uint32_t u; } e;
    e.u = (uint32_t)(i + 127) << 23;
    return p * e.f;
}

/* Cubic soft clip: identity-ish near 0, flat at +-1 from |x| >= 1.5. */
static inline float soft_clip(float x)
{
    if (x > 1.5f)
        return 1.0f;
    if (x < -1.5f)
        return -1.0f;
    return x - (4.0f / 27.0f) * x * x * x;
}

/* PolyBLEP residual for a unit step at t = 0 (t = phase since the step). */
static inline float poly_blep(float t, float dt)
{
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

static inline float note_dt(const voice_t *v, float note)
{
    float dt = (440.0f / v->fs) * fast_exp2((note - 69.0f) * (1.0f / 12.0f));
    return dt < 0.45f ? dt : 0.45f;
}

static float decay_coef(float fs, float t60_s)
{
    /* per-sample multiplier that falls by 60 dB (ln 1000 = 6.9078) in t60 */
    return expf(-6.9077553f / (t60_s * fs));
}

static float filt_decay_t60(uint8_t d)
{
    /* 60 ms .. 2.5 s, exponential */
    return 0.06f * exp2f((float)d * (5.3808217f / 127.0f)); /* log2(2.5/0.06) */
}

float voice_glide_time_s(const voice_t *v)
{
    float g = (float)v->param[VOICE_P_GLIDE_TIME] * (1.0f / 127.0f);
    return 0.002f + 0.5f * g * g;
}

int voice_active(const voice_t *v)
{
    return v->amp_stage != VOICE_AMP_IDLE;
}

void voice_param(voice_t *v, voice_param_id_t id, uint8_t value)
{
    if ((unsigned)id >= VOICE_P_COUNT)
        return;
    if (value > 127)
        value = 127;
    v->param[id] = value;
    float x = (float)value * (1.0f / 127.0f);
    switch (id) {
    case VOICE_P_CUTOFF:
        v->cut_oct_target = log2f(CUT_MIN_HZ) + x * CUT_RANGE_OCT;
        break;
    case VOICE_P_RESONANCE:
        v->k_target = x * K_MAX;
        break;
    case VOICE_P_ENV_MOD:
        v->env_depth_oct = x * ENV_MAX_OCT;
        break;
    case VOICE_P_DECAY: {
        float t60 = filt_decay_t60(value);
        v->fenv_coef = decay_coef(v->fs, t60);
        v->fenv_coef_accent = decay_coef(v->fs, t60 < ACCENT_DECAY_S ? t60 : ACCENT_DECAY_S);
        /* the amp lasts longer than the filter sweep, like a 303's VCA */
        v->amp_decay_coef = decay_coef(v->fs, 0.3f + 2.0f * t60);
        break;
    }
    case VOICE_P_WAVEFORM:
        v->square = value >= 64;
        break;
    case VOICE_P_VOLUME:
        v->vol_target = x * x * OUT_GAIN;
        break;
    case VOICE_P_SLIDE:
        v->slide = value >= 64;
        break;
    case VOICE_P_GLIDE_TIME:
        v->glide_samples = (int)(voice_glide_time_s(v) * v->fs + 0.5f);
        break;
    default:
        break;
    }
}

void voice_init(voice_t *v, float sample_rate)
{
    memset(v, 0, sizeof *v);
    v->fs = sample_rate;
    v->pi_over_fs = PI_F / sample_rate;
    v->fc_max_oct = log2f(FC_MAX_FRAC * sample_rate);
    v->smooth = 1.0f - expf(-1.0f / (0.005f * sample_rate));
    v->fenv_att = 1.0f - expf(-1.0f / (0.0003f * sample_rate));
    v->amp_att_step = 1.0f / (0.001f * sample_rate);
    v->amp_rel_coef = expf(-1.0f / (0.0015f * sample_rate));
    v->amp_stage = VOICE_AMP_IDLE;

    static const uint8_t defaults[VOICE_P_COUNT] = {
        [VOICE_P_CUTOFF] = 50, [VOICE_P_RESONANCE] = 80, [VOICE_P_ENV_MOD] = 60,
        [VOICE_P_DECAY] = 50,  [VOICE_P_WAVEFORM] = 0,   [VOICE_P_VOLUME] = 100,
        [VOICE_P_SLIDE] = 0,   [VOICE_P_GLIDE_TIME] = 44,
    };
    for (int i = 0; i < VOICE_P_COUNT; i++)
        voice_param(v, (voice_param_id_t)i, defaults[i]);
    v->cut_oct = v->cut_oct_target;
    v->k = v->k_target;
    v->vol = v->vol_target;
    v->note_cur = v->note_target = 60.0f;
    v->dt = note_dt(v, 60.0f);
}

/* Move the pitch to `note`: glide if asked and possible, else jump. */
static void set_pitch(voice_t *v, uint8_t note, int glide)
{
    v->note_target = (float)note;
    if (glide && v->glide_samples > 0) {
        v->note_step = (v->note_target - v->note_cur) / (float)v->glide_samples;
        v->glide_left = v->glide_samples;
    } else {
        v->note_cur = v->note_target;
        v->glide_left = 0;
        v->dt = note_dt(v, v->note_cur);
    }
}

static void held_remove(voice_t *v, uint8_t note)
{
    for (int i = 0; i < v->nheld; i++) {
        if (v->held[i] == note) {
            memmove(&v->held[i], &v->held[i + 1], (size_t)(v->nheld - i - 1));
            v->nheld--;
            return;
        }
    }
}

void voice_note_on(voice_t *v, uint8_t note, uint8_t velocity)
{
    if (velocity == 0) {
        voice_note_off(v, note);
        return;
    }
    note &= 0x7f;
    int legato = v->nheld > 0 && v->slide;
    held_remove(v, note);
    if (v->nheld == VOICE_HELD_MAX) { /* drop the oldest */
        memmove(&v->held[0], &v->held[1], VOICE_HELD_MAX - 1);
        v->nheld--;
    }
    v->held[v->nheld++] = note;

    if (legato) {
        /* 303 slide: pitch glides, envelopes carry on */
        set_pitch(v, note, 1);
        return;
    }
    if (v->amp_stage == VOICE_AMP_IDLE) {
        v->phase = 0.0f;
        v->s[0] = v->s[1] = v->s[2] = v->s[3] = 0.0f;
    }
    set_pitch(v, note, 0);
    v->accent = velocity >= VOICE_ACCENT_VELOCITY;
    v->amp_peak = v->accent ? AMP_ACCENT : AMP_NORMAL;
    v->fenv_k = v->accent ? v->fenv_coef_accent : v->fenv_coef;
    v->fenv_lvl = 1.0f;
    v->amp_stage = VOICE_AMP_ATTACK;
}

void voice_note_off(voice_t *v, uint8_t note)
{
    note &= 0x7f;
    if (v->nheld == 0)
        return;
    int was_top = v->held[v->nheld - 1] == note;
    held_remove(v, note);
    if (!was_top)
        return;
    if (v->nheld > 0)
        set_pitch(v, v->held[v->nheld - 1], v->slide); /* back to the held note, no retrigger */
    else if (v->amp_stage != VOICE_AMP_IDLE)
        v->amp_stage = VOICE_AMP_RELEASE;
}

void voice_all_notes_off(voice_t *v)
{
    v->nheld = 0;
    if (v->amp_stage != VOICE_AMP_IDLE)
        v->amp_stage = VOICE_AMP_RELEASE;
}

void voice_render(voice_t *v, float *out, int n)
{
    /* locals for the hot state; written back at the end */
    float phase = v->phase, dt = v->dt;
    float cut_oct = v->cut_oct, k = v->k, vol = v->vol;
    float fenv = v->fenv, fenv_lvl = v->fenv_lvl, amp = v->amp;
    float s0 = v->s[0], s1 = v->s[1], s2 = v->s[2], s3 = v->s[3];
    float peak = v->peak_preclip;
    const float sm = v->smooth;
    const float fc_max_oct = v->fc_max_oct;

    for (int i = 0; i < n; i++) {
        /* parameter smoothing (cheap enough to run even when idle) */
        cut_oct += (v->cut_oct_target - cut_oct) * sm;
        k += (v->k_target - k) * sm;
        vol += (v->vol_target - vol) * sm;

        /* amp envelope */
        switch (v->amp_stage) {
        case VOICE_AMP_IDLE:
            out[i] = 0.0f;
            continue;
        case VOICE_AMP_ATTACK:
            amp += v->amp_att_step;
            if (amp >= v->amp_peak) {
                amp = v->amp_peak;
                v->amp_stage = VOICE_AMP_DECAY;
            }
            break;
        case VOICE_AMP_DECAY:
            amp *= v->amp_decay_coef;
            break;
        default: /* release */
            amp *= v->amp_rel_coef;
            if (amp < IDLE_LEVEL) {
                amp = 0.0f;
                fenv = fenv_lvl = 0.0f;
                s0 = s1 = s2 = s3 = 0.0f;
                v->amp_stage = VOICE_AMP_IDLE;
                out[i] = 0.0f;
                continue;
            }
            break;
        }

        /* filter envelope */
        fenv_lvl *= v->fenv_k;
        fenv += (fenv_lvl - fenv) * v->fenv_att;

        /* pitch glide */
        if (v->glide_left > 0) {
            if (--v->glide_left == 0)
                v->note_cur = v->note_target;
            else
                v->note_cur += v->note_step;
            dt = note_dt(v, v->note_cur);
        }

        /* oscillator */
        float osc;
        if (v->square) {
            float t2 = phase + 0.5f;
            if (t2 >= 1.0f)
                t2 -= 1.0f;
            osc = (phase < 0.5f ? 1.0f : -1.0f) + poly_blep(phase, dt) - poly_blep(t2, dt);
        } else {
            osc = 2.0f * phase - 1.0f - poly_blep(phase, dt);
        }
        phase += dt;
        if (phase >= 1.0f)
            phase -= 1.0f;

        /* cutoff -> G */
        float depth = v->env_depth_oct + (v->accent ? ACCENT_OCT : 0.0f);
        float oct = cut_oct + depth * fenv;
        if (oct > fc_max_oct)
            oct = fc_max_oct;
        float w = fast_exp2(oct) * v->pi_over_fs;
        float w2 = w * w;
        float num = w * (945.0f + w2 * (-105.0f + w2));
        float den = 945.0f + w2 * (-420.0f + 15.0f * w2);
        float G = num / (den + num);

        /* ladder: solve the feedback, clip, run the stages */
        float G2 = G * G;
        float S = (1.0f - G) * (G2 * G * s0 + G2 * s1 + G * s2 + s3);
        float u = (osc * DRIVE - k * S) / (1.0f + k * G2 * G2);
        u = soft_clip(u);
        float vv, y;
        vv = (u - s0) * G; y = vv + s0; s0 = y + vv;
        vv = (y - s1) * G; y = vv + s1; s1 = y + vv;
        vv = (y - s2) * G; y = vv + s2; s2 = y + vv;
        vv = (y - s3) * G; y = vv + s3; s3 = y + vv;

        float o = y * (1.0f + 0.5f * k) * amp * vol;
        float a = o < 0.0f ? -o : o;
        if (a > peak)
            peak = a;
        if (o > 1.0f)
            o = 1.0f;
        else if (o < -1.0f)
            o = -1.0f;
        out[i] = o;
    }

    v->phase = phase; v->dt = dt;
    v->cut_oct = cut_oct; v->k = k; v->vol = vol;
    v->fenv = fenv; v->fenv_lvl = fenv_lvl; v->amp = amp;
    v->s[0] = s0; v->s[1] = s1; v->s[2] = s2; v->s[3] = s3;
    v->peak_preclip = peak;
}
