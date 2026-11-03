/* SPDX-License-Identifier: GPL-3.0-only */
/* x0x-lite 808 kit: X0X's drum808 (an 8W8 port), seven voices at 48 kHz; see drum808.h.
 *
 * Every voice below is X0X's of the same name (8W8's sc808_*_circuit.h), statement for
 * statement, with its constants converted to 48 kHz. */
#include "drum808.h"
#include "fastmath.h"

#define SR D8_SR
#define TWO_PI_SR 1.308996939e-4f              /* 2 pi / 48000 */
#define PI_SR 6.544984695e-5f                  /* pi / 48000 */
#define CHUNK 32
#define GATE_N 48                              /* the 1 ms trigger pulse */

/* ---- fixed coefficients: X0X's 44.1 kHz values converted (drum808.h) ---- */
#define D8_BD_ATK_C 9.965337989e-01f           /* exp(-1/(6 ms sr)) */
#define D8_BD_SIGH 1.470095605e-03f
#define D8_BD_DC 8.269052424e-04f
#define D8_TOM_PITCH_C 9.995371442e-01f        /* exp(-1/(45 ms sr)) */
#define D8_TOM_SKIN_C 9.982653951e-01f         /* exp(-1/(12 ms sr)) */
#define D8_TOM_STRIKE_LPC 1.171184851e-02f     /* 1 - exp(-2 pi 90 / sr) */
#define D8_CP_TOOTH_D 9.953810706e-01f
#define D8_CP_MAIN_ATK 6.920387510e-03f
#define D8_MENV_UP 8.159753939e-01f
#define D8_OH_ATK_C 3.466201029e-03f
#define D8_CB_CLANK_D 9.986120752e-01f
#define D8_HAT_LPC_OPEN 5.380544232e-01f
#define D8_HAT_LPC_CLOSED 7.470204911e-01f
#define D8_HP1_A_200 9.741597847e-01f
#define D8_HP1_A_1400 8.325520209e-01f

/* the pulse shaper (R162 4.7k, R163 100k, C40 15n) every circuit voice uses */
#define SH_A 7.338892384e-01f
#define SH_DC 4.489016237e-02f

/* ---- maths ---- */

/* expm1 with full relative accuracy near 0, where 1 - exp(x) is what matters */
static inline __attribute__((always_inline)) float d8_expm1(float x)
{
    if (x > -0.5f && x < 0.5f) {
        float p = 1.0f / 362880.0f;
        p = p * x + 1.0f / 40320.0f;
        p = p * x + 1.0f / 5040.0f;
        p = p * x + 1.0f / 720.0f;
        p = p * x + 1.0f / 120.0f;
        p = p * x + 1.0f / 24.0f;
        p = p * x + 1.0f / 6.0f;
        p = p * x + 0.5f;
        p = p * x + 1.0f;
        return p * x;
    }
    return fm_expf(x) - 1.0f;
}

static float d8_exp(float x) { return (x > -0.5f && x < 0.5f) ? 1.0f + d8_expm1(x) : fm_expf(x); }
static float d8_midicps(float note) { return 440.0f * fm_exp2f((note - 69.0f) * (1.0f / 12.0f)); }
static float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

/* ---- primitives ---- */

static void rng_seed(d8_rng_t *r, uint32_t s)
{
    r->s1 = 1243598713u ^ s; if (r->s1 < 2u) r->s1 = 1243598713u;
    r->s2 = 3093459404u ^ s; if (r->s2 < 8u) r->s2 = 3093459404u;
    r->s3 = 1821928721u ^ s; if (r->s3 < 16u) r->s3 = 1821928721u;
}

static inline __attribute__((always_inline)) float rng_frand2(d8_rng_t *r)
{
    fm_bits_t u;
    r->s1 = ((r->s1 & 0xFFFFFFFEu) << 12) ^ (((r->s1 << 13) ^ r->s1) >> 19);
    r->s2 = ((r->s2 & 0xFFFFFFF8u) << 4) ^ (((r->s2 << 2) ^ r->s2) >> 25);
    r->s3 = ((r->s3 & 0xFFFFFFF0u) << 17) ^ (((r->s3 << 3) ^ r->s3) >> 11);
    u.u = 0x40000000u | ((r->s1 ^ r->s2 ^ r->s3) >> 9);
    return u.f - 3.0f;
}

/* PulseShaper: high shelf, then a diode holding negative swings at one drop */
static inline __attribute__((always_inline)) float shaper(float *st, float gate)
{
    float v;
    *st = gate + SH_A * (*st - gate);
    if (gate == 0.0f && fm_fabsf(*st) < 1e-20f)
        *st = 0.0f;
    v = (gate - *st) + SH_DC * *st;
    if (v < 0.0f)
        v = 0.71f * d8_expm1(v > -20.0f ? v : -20.0f);
    return v;
}

/* tanh, with a shorter series than fastmath's where the op-amps spend most of their time
 * (|x| < 0.5: error under 5e-5) */
static inline __attribute__((always_inline)) float d8_tanh(float x)
{
    float z;
    if (x > 0.5f || x < -0.5f)
        return fm_tanhf(x);
    z = x * x;
    return x * (1.0f + z * (-0.333333333f + z * (0.133333333f + z * -0.0539682540f)));
}

static inline __attribute__((always_inline)) float opamp_clip(float v, float rail, float irail) { return rail * d8_tanh(v * irail); }

/* BridgedT::set: constant-peak bandpass */
static inline __attribute__((always_inline)) void bp_set(d8_bp_t *f, float hz, float q)
{
    float w, s, sh, alpha, inv;
    hz = hz < 20.0f ? 20.0f : (hz > SR * 0.4f ? SR * 0.4f : hz);
    q = q < 0.3f ? 0.3f : (q > 60.0f ? 60.0f : q);
    w = TWO_PI_SR * hz;
    s = fm_sin_pi(w);
    sh = fm_sin_pi(0.5f * w);
    alpha = s / (2.0f * q);
    inv = 1.0f / (1.0f + alpha);
    f->b0 = alpha * inv;
    f->e1 = (2.0f * alpha + 4.0f * sh * sh) * inv;
    f->e2 = -2.0f * alpha * inv;
}

static inline __attribute__((always_inline)) float bp_run(d8_bp_t *f, float x)
{
    float bx = f->b0 * x;
    float y = bx + f->z1;
    f->z1 = (y + y + f->z2) - f->e1 * y;
    f->z2 = (-bx - y) - f->e2 * y;
    return y;
}

static inline __attribute__((always_inline)) float bq_run(d8_bq_t *f, float x)
{
    float y = f->n0 * x + f->z1;
    f->z1 = (f->n1 * x + (y + y + f->z2)) - f->e1 * y;
    f->z2 = (f->n2 * x - y) - f->e2 * y;
    return y;
}

/* SuperCollider HPF (2nd-order Butterworth, prewarped), coefficients only */
static inline __attribute__((always_inline)) void sc_hpf_set(d8_bq_t *f, float hz)
{
    float c = fm_tanf(PI_SR * hz), c2 = c * c, s2c = c * 1.41421356f;
    float a0 = 1.0f / (1.0f + s2c + c2);
    f->n0 = a0; f->n1 = -2.0f * a0; f->n2 = a0;
    f->e1 = 2.0f * a0 * (s2c + 2.0f * c2);
    f->e2 = -2.0f * s2c * a0;
}

/* SKHighpass: RBJ highpass with a gain */
static void sk_hp_set(d8_bq_t *f, float hz, float q, float g)
{
    float w = TWO_PI_SR * hz, s = fm_sin_pi(w), sh = fm_sin_pi(0.5f * w);
    float alpha = s / (2.0f * q), inv = 1.0f / (1.0f + alpha);
    float onepc = 2.0f - 2.0f * sh * sh;          /* 1 + cos w */
    f->n0 = g * onepc * 0.5f * inv;
    f->n1 = -g * onepc * inv;
    f->n2 = f->n0;
    f->e1 = (2.0f * alpha + 4.0f * sh * sh) * inv;
    f->e2 = -2.0f * alpha * inv;
}

/* OnePoleHP: set() also clears the state */
static void hp1_set_a(d8_hp1_t *f, float a) { f->a = a; f->z = 0.0f; f->y = 0.0f; }
static inline __attribute__((always_inline)) float hp1_run(d8_hp1_t *f, float x)
{
    f->y = f->a * (f->y + x - f->z);
    f->z = x;
    return f->y;
}

/* ======================================================================= */
/* the voices                                                              */
/* ======================================================================= */

/* a voice ends after n samples below -90 dB (8W8's 3.2e-5) */
#define D8_QUIET 3.2e-5f
#define QUIET(v, o, n)                                                    \
    do {                                                                  \
        if ((o) > D8_QUIET || (o) < -D8_QUIET) (v)->quiet = 0;            \
        else if (++(v)->quiet > (n)) (v)->active = 0;                     \
    } while (0)

/* ---- bass drum: sc808_bd_circuit.h ---- */
#define BD_Q 2.356f

static inline __attribute__((always_inline)) void bd_reset(d8_bd_t *v)
{
    v->bt.z1 = v->bt.z2 = 0.0f;
    v->shp = 0.0f;
    v->fb = 0.0f;
    v->gate = 0;
    v->atk_env = 0.0f;
    v->sigh_env = 0.0f;
    v->tone_z = 0.0f;
    v->dc_z = 0.0f;
    v->active = 0;
    v->quiet = 0;
    v->coef_age = 0;
    bp_set(&v->bt, v->f0 > 20.0f ? v->f0 : 50.0f, BD_Q);
}

static void bd_trigger(d8_bd_t *v, float hz, float decay_k, float tone, float attack, float accent_v)
{
    float w, raw, k, vr, t_hz;
    v->f0 = hz > 20.0f ? (hz < 400.0f ? hz : 400.0f) : 20.0f;
    /* forward gain 1 + R167 / (2 Reff), Reff = 1 / (w^2 R167 C41 C42) */
    w = 6.28318531f * v->f0;
    raw = 1.0f + w * w * 1.125e-4f;
    v->forward = raw;
    v->tune_trim = 1.210321549e+01f / raw;
    k = clampf(decay_k, 0.0f, 1.0f);
    vr = 500.0e3f * (k * 0.999f + 0.001f);
    v->loop_g = vr / (47.0e3f + vr);
    v->atk_amt = clampf(attack, 0.0f, 1.0f);
    v->accent_v = clampf(accent_v, 1.0f, 20.0f);
    t_hz = 300.0f * fm_powf(20.0f, clampf(tone, 0.0f, 1.0f));
    v->tone_c = -d8_expm1(-TWO_PI_SR * t_hz);
    v->gate = GATE_N;
    v->atk_env = 1.0f;
    v->active = 1;
    v->quiet = 0;
    v->coef_age = 0;
}

static inline __attribute__((always_inline)) float bd_tick(d8_bd_t *v)
{
    float gate = 0.0f, vplus, mag, x, y, click, out;
    if (v->gate > 0) {
        gate = v->accent_v;
        v->gate--;
    }
    vplus = shaper(&v->shp, gate);
    v->atk_env *= D8_BD_ATK_C;
    if (v->atk_env < 1e-6f)
        v->atk_env = 0.0f;
    mag = fm_fabsf(v->fb) * (1.0f / 1.35f);
    v->sigh_env += (mag - v->sigh_env) * D8_BD_SIGH;
    if (--v->coef_age <= 0) {
        float sigh = 1.0f + 0.16f * (v->sigh_env < 1.0f ? v->sigh_env : 1.0f);
        float lift = 1.0f + 2.0f * v->atk_amt * v->atk_env;
        v->coef_age = 16;
        bp_set(&v->bt, v->f0 * sigh * lift, BD_Q * lift);
    }
    x = vplus * v->forward + v->loop_g * opamp_clip(v->fb, 12.0f, 1.0f / 12.0f);
    y = bp_run(&v->bt, x);
    v->fb = y;
    click = vplus * v->atk_amt * 0.16f;
    v->tone_z += ((y + click) - v->tone_z) * v->tone_c;
    v->dc_z += (v->tone_z - v->dc_z) * D8_BD_DC;
    out = v->tone_z - v->dc_z;
    if (!(out > -50.0f && out < 50.0f)) {
        bd_reset(v);
        return 0.0f;
    }
    out *= 1.522f * v->tune_trim;
    QUIET(v, out, 400);
    return out;
}

/* ---- snare: sc808_sd_circuit.h ---- */
#define SD_F1 1.733336385e+02f
#define SD_F2 3.359763482e+02f
#define SD_Q1 1.736290840e+01f
#define SD_Q2 1.066003582e+01f
#define SD_G1 6.039411765e+02f
#define SD_G2 2.282727273e+02f
#define SD_DECAY_DEF 0.850393713f                 /* 108/127 */

static inline __attribute__((always_inline)) void sd_reset(d8_sd_t *v)
{
    v->bt1.z1 = v->bt1.z2 = v->bt2.z1 = v->bt2.z2 = 0.0f;
    v->shp = 0.0f;
    sc_hpf_set(&v->noise_hp, 1760.0f);
    v->noise_hp.z1 = v->noise_hp.z2 = 0.0f;
    sc_hpf_set(&v->out_hp, 30.0f);
    v->out_hp.z1 = v->out_hp.z2 = 0.0f;
    v->gate = 0;
    v->noise_env = 0.0f;
    v->active = 0;
    v->quiet = 0;
}

static void sd_trigger(d8_sd_t *v, float ratio, float decay01, float tone01, float snappy01, float accent_v)
{
    float r = ratio > 0.05f ? (ratio < 8.0f ? ratio : 8.0f) : 0.05f;
    float dq = 0.35f + 1.15f * clampf(decay01, 0.0f, 1.0f);
    float t = clampf(tone01, 0.0f, 1.0f) * (0.5f * FM_PI), dnorm;
    bp_set(&v->bt1, SD_F1 * r, SD_Q1 * dq);
    bp_set(&v->bt2, SD_F2 * r, SD_Q2 * dq);
    v->mix1 = fm_sin_pi(0.5f * FM_PI - t);
    v->mix2 = fm_sin_pi(t);
    v->accent_v = clampf(accent_v, 1.0f, 20.0f);
    v->noise_env = clampf(snappy01, 0.0f, 1.0f) * v->accent_v * 0.25f;
    dnorm = dq / (0.35f + 1.15f * SD_DECAY_DEF);
    v->noise_decay = d8_exp(-1.0f / (0.075f * dnorm * SR));
    v->gate = GATE_N;
    v->active = 1;
    v->quiet = 0;
}

static inline __attribute__((always_inline)) float sd_tick(d8_sd_t *v)
{
    float gate = 0.0f, vplus, s1, s2, shells, n, vca, noise, out;
    if (v->gate > 0) {
        gate = v->accent_v;
        v->gate--;
    }
    vplus = shaper(&v->shp, gate);
    s1 = bp_run(&v->bt1, vplus * SD_G1);
    s2 = bp_run(&v->bt2, vplus * SD_G2);
    shells = opamp_clip(s1 * v->mix1 + s2 * v->mix2, 40.0f, 1.0f / 40.0f);
    v->noise_env *= v->noise_decay;
    n = rng_frand2(&v->rng);
    vca = (n > 0.0f ? n : 0.0f) * v->noise_env;
    noise = bq_run(&v->noise_hp, vca);
    out = shells * 0.1491f + noise * 1.4908f;
    out = bq_run(&v->out_hp, out);
    if (!(out > -50.0f && out < 50.0f)) {
        sd_reset(v);
        return 0.0f;
    }
    QUIET(v, out, 400);
    return out;
}

/* ---- low tom: sc808_tom_circuit.h, tom mode ---- */
#define TOM_Q 1.081763729e+01f
#define TOM_LN100_Q_PI 1.585726294e+01f              /* ln(100) Q / pi */

static inline __attribute__((always_inline)) void tom_reset(d8_tom_t *v)
{
    v->bt.z1 = v->bt.z2 = 0.0f;
    v->skin_lp_z = v->strike_lp_z = 0.0f;
    v->fb = v->dc_z = 0.0f;
    v->gate = 0;
    v->active = 0;
    v->quiet = 0;
}

static void tom_trigger(d8_tom_t *v, float hz, float ring_s, float accent_v)
{
    float t, g;
    v->f0 = hz < 20.0f ? 20.0f : (hz > SR * 0.25f ? SR * 0.25f : hz);
    t = ring_s > 0.02f ? ring_s : 0.02f;
    g = 1.0f - TOM_LN100_Q_PI / (v->f0 * t);
    if (g < 0.0f) g = 0.0f;
    if (g > 0.993f) g = 0.993f;
    v->loop_g = g;
    v->accent_v = accent_v;
    v->gate = GATE_N;
    v->pitch_env = 1.0f;
    v->skin_env = 1.0f;
    v->skin_lp_c = -d8_expm1(-TWO_PI_SR * (v->f0 * 2.0f));
    v->fwd = fm_powf(120.0f / v->f0, 0.224f) * 9.2f;
    v->coef_age = 0;
    v->active = 1;
    v->quiet = 0;
}

static inline __attribute__((always_inline)) float tom_tick(d8_tom_t *v)
{
    float gate = 0.0f, strike, skin = 0.0f, x, y, out;
    if (v->gate > 0) {
        gate = v->accent_v;
        v->gate--;
    }
    strike = shaper(&v->shp, gate) * 4.0f;
    v->strike_lp_z += (strike - v->strike_lp_z) * D8_TOM_STRIKE_LPC;
    strike = v->strike_lp_z;
    if (v->skin_env > 1e-5f) {
        float n = rng_frand2(&v->rng) * v->skin_env * 0.095f * v->accent_v;
        v->skin_lp_z += (n - v->skin_lp_z) * v->skin_lp_c;
        skin = v->skin_lp_z;
        v->skin_env *= D8_TOM_SKIN_C;
    }
    v->pitch_env *= D8_TOM_PITCH_C;
    if (v->pitch_env < 1e-5f)
        v->pitch_env = 0.0f;
    if (--v->coef_age <= 0) {
        v->coef_age = 16;
        bp_set(&v->bt, v->f0 * (1.0f + 0.085f * v->pitch_env), TOM_Q);
    }
    x = strike + skin + v->loop_g * opamp_clip(v->fb, 12.0f, 1.0f / 12.0f);
    y = bp_run(&v->bt, x);
    v->fb = y;
    out = y * v->fwd;
    v->dc_z += (out - v->dc_z) * D8_BD_DC;
    out -= v->dc_z;
    if (!(out > -50.0f && out < 50.0f)) {
        tom_reset(v);
        return 0.0f;
    }
    QUIET(v, out, 400);
    return out;
}

/* ---- hand clap: sc808_cp_circuit.h ---- */
#define CP_F 8.743325448e+02f
#define CP_Q 1.290994449e+00f
#define CP_SPREAD 480                                 /* 10 ms between the first bursts */
#define CP_MAIN_OPEN 1344                             /* 2.8 spreads */

static void cp_trigger(d8_cp_t *v, float ratio, float decay)
{
    float dd = decay < 0.05f ? 0.05f : (decay > 4.0f ? 4.0f : decay);
    bp_set(&v->bp, CP_F * ratio, CP_Q);
    v->main_d = d8_exp(-1.0f / (0.0385f * dd * SR));
    v->floor_d = d8_exp(-1.0f / (0.330f * dd * SR));
    v->tooth_d = D8_CP_TOOTH_D;
    v->tooth = 0.80f;
    v->fired = 1;
    v->age = 0;
    v->main_level = 0.0f;
    v->main_target = 0.0f;
    v->floor_level = 0.0f;
    v->active = 1;
    v->quiet = 0;
}

static inline __attribute__((always_inline)) float cp_tick(d8_cp_t *v)
{
    float ctrl, n, y;
    if (v->fired < 3 && v->age >= v->fired * CP_SPREAD) {
        v->tooth = 0.80f;
        v->fired++;
    }
    if (v->age == CP_MAIN_OPEN) {
        v->main_target = 1.0f;
        v->floor_level = 0.030f;
    }
    v->age++;
    ctrl = v->tooth + v->main_level + v->floor_level;
    v->tooth *= v->tooth_d;
    if (v->main_target > 0.0f) {
        v->main_level += (v->main_target - v->main_level) * D8_CP_MAIN_ATK;
        if (v->main_level > 0.98f)
            v->main_target = 0.0f;
    } else
        v->main_level *= v->main_d;
    v->floor_level *= v->floor_d;
    n = rng_frand2(&v->rng);
    y = bp_run(&v->bp, (n > 0.0f ? n : 0.0f) * ctrl) * 0.62f;
    y = bq_run(&v->hp, y);
    QUIET(v, y, 400);
    return y;
}

/* ---- the metal bank: sc808_mt_circuit.h SchmittBank ---- */
static const float k_bank_hz[6] = {206.1f, 368.2f, 305.9f, 521.0f, 803.1f, 541.7f};
#define BANK_DUTY 2060725309u                     /* phase < 0.4798 */

/* a square's increment with its drift and jitter (8W8 applies them every sample; they change
 * only every 256 samples and at the square's wrap) */
static inline __attribute__((always_inline)) void bank_dt(d8_bank_t *b, int i)
{
    float fm1 = b->drift[i] + b->jm1[i] + b->drift[i] * b->jm1[i];
    float p = (float)b->inc[i] * fm1;
    b->dt[i] = b->inc[i] + (uint32_t)(int32_t)(p + (p >= 0.0f ? 0.5f : -0.5f));
}

static void bank_ratio(d8_bank_t *b, float r)
{
    int i;
    r = r < 0.25f ? 0.25f : (r > 4.0f ? 4.0f : r);
    for (i = 0; i < 6; i++) {
        b->inc[i] = (uint32_t)(k_bank_hz[i] * r * (4294967296.0f / SR));
        bank_dt(b, i);
    }
}

static inline __attribute__((always_inline)) float bank_tick(d8_bank_t *b)
{
    float sum = 0.0f;
    int i;
    if (--b->drift_cnt <= 0) {
        b->drift_cnt = 256;
#pragma GCC unroll 6
        for (i = 0; i < 6; i++) {
            b->drift[i] += 2.5e-4f * rng_frand2(&b->rng);
            b->drift[i] *= 0.98f;
            bank_dt(b, i);
        }
    }
#pragma GCC unroll 6
    for (i = 0; i < 6; i++) {
        uint32_t old = b->ph[i];
        b->ph[i] = old + b->dt[i];
        if (b->ph[i] < old || (b->pend_wrap & (1u << i))) {
            b->pend_wrap &= (uint8_t)~(1u << i);
            b->jm1[i] = 0.0052f * rng_frand2(&b->rng);
            bank_dt(b, i);
        }
        sum += b->ph[i] < BANK_DUTY ? 2.5f : -2.5f;
    }
    return sum * (1.0f / 21.0f);
}

/* ---- metal envelope ---- */
static inline __attribute__((always_inline)) float menv_tick(d8_menv_t *e)
{
    if (e->target > 0.0f) {
        e->v = e->target + (e->v - e->target) * e->up;
        if (e->v >= e->target * 0.995f)
            e->target = 0.0f;
    } else
        e->v *= e->down;
    return e->v;
}

#define DIODE_VON 0.5899f
static inline __attribute__((always_inline)) float knee_gate(float e)            /* the swing VCA's diode, knee 1.2 */
{
    float over = e - DIODE_VON;
    if (over <= 0.0f)
        return 0.0f;
    if (over < 1.2f)
        return over * over * (1.0f / 2.4f);
    return over - 0.6f;
}

/* ---- hi-hats: sc808_mt_circuit.h HatCircuit ---- */
static void hat_init(d8_hat_t *v, int mode)
{
    v->mode = (uint8_t)mode;
    if (mode == 0) {
        sk_hp_set(&v->ska, 5200.0f, 1.6f, 1.0f);
        sk_hp_set(&v->skb, 2700.0f, 0.75f, 1.0f);
    } else {
        sk_hp_set(&v->ska, 6200.0f, 0.9f, 1.0f);
        sk_hp_set(&v->skb, 5200.0f, 0.8f, 1.0f);
    }
    v->lp_c = mode == 1 ? D8_HAT_LPC_OPEN : D8_HAT_LPC_CLOSED;
    v->env.up = D8_MENV_UP;
    v->env.down = 1.0f;
    hp1_set_a(&v->dc, D8_HP1_A_200);
    v->out_scale = mode == 1 ? 5.0f : 8.0f;
}

static void hat_trigger(d8_hat_t *v, float decay_s, float accent_v)
{
    float a = accent_v / 8.0f;
    float t = decay_s > 0.02f ? decay_s : 0.02f;
    if (v->mode == 1) {
        v->lin_target = 8.0f * a;
        v->lin_level = 0.0f;
        v->lin_hold = (int32_t)(0.30f * t * SR);
        v->lin_step = v->lin_target / (0.70f * t * SR);
        v->ramp = 0;
        v->linear = 1;
    } else {
        v->env.target = 8.0f * a;
        v->env.down = d8_exp(-1.0f / ((t / 2.0f) * SR));
        v->linear = 0;
    }
    v->active = 1;
    v->quiet = 0;
}

static void hat_choke(d8_hat_t *v)
{
    v->env.v = 0.0f;
    v->env.target = 0.0f;
    v->lin_level = 0.0f;
    v->lin_base = 0.0f;
    v->lin_n = 0;
}

static inline __attribute__((always_inline)) float hat_tick(d8_hat_t *v, float bus)
{
    float t1 = bus - v->d1, hp, e, sv;
    v->d1 = bus;
    hp = (t1 - v->d2) * 30.0f;
    v->d2 = t1;
    if (v->linear) {
        e = v->lin_level;
        if (v->lin_target > 0.0f) {
            v->lin_level += (v->lin_target - v->lin_level) * D8_OH_ATK_C;
            if (v->lin_level > v->lin_target * 0.99f)
                v->lin_target = 0.0f;
        } else if (v->lin_hold > 0)
            v->lin_hold--;
        else {                                    /* C62's discharge, as start - n step */
            if (!v->ramp) {
                v->ramp = 1;
                v->lin_base = v->lin_level;
                v->lin_n = 0;
            }
            if (v->lin_level > 0.0f) {
                v->lin_level = v->lin_base - (float)(++v->lin_n) * v->lin_step;
                if (v->lin_level < 0.0f)
                    v->lin_level = 0.0f;
            }
        }
    } else
        e = menv_tick(&v->env);
    sv = bq_run(&v->skb, bq_run(&v->ska, hp * knee_gate(e)));
    v->lp[0] += (sv - v->lp[0]) * v->lp_c;
    sv = v->lp[0];
    if (v->mode == 1) {
        v->lp[1] += (sv - v->lp[1]) * v->lp_c;
        sv = v->lp[1];
        v->lp[2] += (sv - v->lp[2]) * v->lp_c;
        sv = v->lp[2];
        v->lp[3] += (sv - v->lp[3]) * v->lp_c;
        sv = v->lp[3];
        v->lp[4] += (sv - v->lp[4]) * v->lp_c;
        sv = v->lp[4];
    }
    sv = hp1_run(&v->dc, sv * v->out_scale);
    if (sv > D8_QUIET || sv < -D8_QUIET)
        v->quiet = 0;
    else if (++v->quiet > 400 &&
             (v->linear ? v->lin_level <= 0.0f : (v->env.target <= 0.0f && v->env.v < DIODE_VON * 0.5f)))
        v->active = 0;
    return sv;
}

/* ---- cowbell: sc808_mt_circuit.h CowbellCircuit ---- */
static void cb_init(d8_cb_t *v)
{
    bp_set(&v->bp, 812.0f, 5.5f);
    hp1_set_a(&v->hp, D8_HP1_A_200);
    hp1_set_a(&v->bla, D8_HP1_A_1400);
    hp1_set_a(&v->blb, D8_HP1_A_1400);
    v->body_d = 1.0f;
    v->ratio = 1.0f;
}

static void cb_ratio(d8_cb_t *v, float r)
{
    r = r < 0.25f ? 0.25f : (r > 4.0f ? 4.0f : r);
    if (r != v->ratio) {
        bp_set(&v->bp, 812.0f * r, 5.5f);
        v->ratio = r;
    }
}

static void cb_trigger(d8_cb_t *v, float decay_s, float accent_v)
{
    float a = accent_v / 8.0f;
    float t = decay_s > 0.05f ? decay_s : 0.05f;
    v->clank = 0.77f * a;
    v->body = 0.23f * a;
    v->body_d = d8_exp(-1.0f / (0.140f * (t / 0.43f) * SR));
    v->active = 1;
    v->quiet = 0;
}

static inline __attribute__((always_inline)) float cb_tick(d8_cb_t *v, float pair)  /* pair = (o5 + o6) / 2 */
{
    float x = bp_run(&v->bp, pair), env = v->clank + v->body, sv, y;
    v->clank *= D8_CB_CLANK_D;
    v->body *= v->body_d;
    sv = (x + 0.55f * hp1_run(&v->blb, hp1_run(&v->bla, pair))) * env;
    sv = sv + 2.0f * sv * sv;
    y = hp1_run(&v->hp, sv * 1.9f);
    if (y > D8_QUIET || y < -D8_QUIET)
        v->quiet = 0;
    else if (++v->quiet > 400 && env < 1e-4f)
        v->active = 0;
    return y;
}

/* ======================================================================= */
/* parameters                                                              */
/* ======================================================================= */

/* 8W8's pot table (sc808_params.h), per track and slot: min, max, exponential?, default */
typedef struct { float min, max; uint8_t exp, def; } d8_spec_t;
#define TU12 {-12.0f, 12.0f, 0, 64}
#define TUM {0.5f, 2.0f, 1, 64}
#define LVL {0.0f, 2.0f, 0, 64}
#define U01(d) {0.0f, 1.0f, 0, d}
#define NON {0.0f, 1.0f, 0, 0}
/*                          level tune  decay                    x1          x2 */
static const d8_spec_t k_spec[D8_NUM][D8P_NUM] = {
    [D8_BD] = {LVL, TU12, {0.0f, 1.0f, 0, 87},     U01(42),    U01(24)},   /* x1 tone, x2 attack */
    [D8_SD] = {LVL, TU12, {0.0f, 1.0f, 0, 108},    U01(64),    U01(89)},   /* x1 tone, x2 snappy */
    [D8_LT] = {LVL, TU12, {0.06f, 2.0f, 1, 68},    NON,        NON},       /* (8W8: tune +-2) */
    [D8_CP] = {LVL, TU12, {0.35f, 2.8f, 1, 64},    NON,        NON},
    [D8_CH] = {LVL, TUM,  {0.03f, 0.5f, 1, 47},    NON,        NON},
    [D8_OH] = {LVL, TUM,  {0.08f, 1.5f, 1, 74},    NON,        NON},
    [D8_CB] = {LVL, TUM,  {0.1f, 2.0f, 1, 62},     NON,        NON},
};

/* the lane trims 8W8 balanced the kit with */
static const float k_trim[D8_NUM] = {0.2964f, 0.1953f, 0.4110f, 3.9640f, 0.0021f, 0.0402f, 0.1219f};

/* base notes (sc808_engine.cpp kBaseNote) for the note-tuned sounds */
static const float k_base_note[D8_NUM] = {34.0f, 65.0f, 42.02f, 71.0f, 0.0f, 0.0f, 0.0f};

#define SC808_FULL_VELOCITY_GAIN 1.9921260f

#define PP(name, def) {name, 127, def, 0}
static const x0x_param_t k_p_bd[] = {PP("Level", 64), PP("Tune", 64), PP("Decay", 87), PP("Tone", 42), PP("Attack", 24)};
static const x0x_param_t k_p_sd[] = {PP("Level", 64), PP("Tune", 64), PP("Decay", 108), PP("Tone", 64), PP("Snappy", 89)};
static const x0x_param_t k_p_lt[] = {PP("Level", 64), PP("Tune", 64), PP("Decay", 68)};
static const x0x_param_t k_p_cp[] = {PP("Level", 64), PP("Tune", 64), PP("Decay", 64)};
static const x0x_param_t k_p_ch[] = {PP("Level", 64), PP("Tune", 64), PP("Decay", 47)};
static const x0x_param_t k_p_oh[] = {PP("Level", 64), PP("Tune", 64), PP("Decay", 74)};
static const x0x_param_t k_p_cb[] = {PP("Level", 64), PP("Tune", 64), PP("Decay", 62)};

typedef struct { const x0x_param_t *p; uint8_t n; } d8_track_t;
#define TRK(p) {p, (uint8_t)(sizeof(p) / sizeof(p[0]))}
static const d8_track_t k_tracks[D8_NUM] = {TRK(k_p_bd), TRK(k_p_sd), TRK(k_p_lt), TRK(k_p_cp),
                                            TRK(k_p_ch), TRK(k_p_oh), TRK(k_p_cb)};

int drum808_nparams(int track) { return track >= 0 && track < D8_NUM ? k_tracks[track].n : 0; }

const x0x_param_t *drum808_param(int track, int i)
{
    if (track < 0 || track >= D8_NUM || i < 0 || i >= k_tracks[track].n)
        return 0;
    return &k_tracks[track].p[i];
}

static float pot_value(int t, int slot, int pot)
{
    const d8_spec_t *p = &k_spec[t][slot];
    float x = (float)pot / 127.0f;
    if (p->exp && p->min > 0.0f)
        return p->min * fm_powf(p->max / p->min, x);
    return p->min + (p->max - p->min) * x;
}

void drum808_set(drum808_t *d, int track, int i, int value)
{
    const x0x_param_t *p = drum808_param(track, i);
    if (!p)
        return;
    if (value < 0) value = 0;
    if (value > p->max) value = p->max;
    d->pot[track][i] = (uint8_t)value;
    d->potv[track][i] = pot_value(track, i, value);
}

int drum808_get(const drum808_t *d, int track, int i)
{
    return drum808_param(track, i) ? d->pot[track][i] : 0;
}

void drum808_set_kit_level(drum808_t *d, int v)
{
    v = v < 0 ? 0 : v > 127 ? 127 : v;
    d->kit_level = (uint8_t)v;
    d->vol = (float)v / 127.0f;
}

/* ======================================================================= */
/* the engine                                                              */
/* ======================================================================= */

void drum808_init(drum808_t *d)
{
    int t, k;
    {
        uint8_t *z = (uint8_t *)d;
        unsigned i;
        for (i = 0; i < sizeof *d; i++)
            z[i] = 0;
    }
    for (t = 0; t < D8_NUM; t++)
        for (k = 0; k < D8P_NUM; k++) {
            d->pot[t][k] = k_spec[t][k].def;
            d->potv[t][k] = pot_value(t, k, k_spec[t][k].def);
        }
    drum808_set_kit_level(d, 100);
    drum808_set_guard(d, D8_NUM, 60);

    rng_seed(&d->sd.rng, 0x808D51Eu);
    sd_reset(&d->sd);
    d->bd.f0 = 50.0f;
    bd_reset(&d->bd);
    rng_seed(&d->lt.rng, 0x808704Du);
    tom_reset(&d->lt);
    rng_seed(&d->cp.rng, 0x808C1A7u);
    bp_set(&d->cp.bp, CP_F, CP_Q);
    sc_hpf_set(&d->cp.hp, 720.0f);

    for (k = 0; k < 6; k++)                       /* SchmittBank::init: phases 0.25 i */
        d->bank.ph[k] = (uint32_t)(k & 3) << 30;
    d->bank.pend_wrap = (1u << 4) | (1u << 5);    /* 1.0 and 1.25 wrap on the first tick */
    rng_seed(&d->bank.rng, 0x808D51F7u);
    bank_ratio(&d->bank, 1.0f);
    hat_init(&d->ch, 0);
    hat_init(&d->oh, 1);
    cb_init(&d->cb);

    for (k = 0; k < D8_NUM; k++) {
        d->lane[k].hit = 1.0f;
        d->lane[k].cg = 1.0f;
    }
}

/* fade a lane out over n samples (the hat choke: 2 ms) */
static void choke_lane(d8_lane_t *l, float n)
{
    if (l->cg > 0.0f && (l->cstep == 0.0f || l->cstep > -1.0f / n))
        l->cstep = -1.0f / n;
}

static int lane_on(const drum808_t *d, int t);

void drum808_set_guard(drum808_t *d, int max_voices, int tail_db)
{
    d->max_voices = max_voices < 1 ? 1 : max_voices > D8_NUM ? D8_NUM : max_voices;
    d->tail_k = fm_db2lin(-(float)(tail_db < 20 ? 20 : tail_db > 90 ? 90 : tail_db));
}

/* the voice cap: before track t starts, fade the oldest others out until it fits. The fade is
 * short (0.33 ms, under the new hit): a fading voice still costs its CPU until it ends, and the
 * cap is there because there isn't any to spare. Voices already fading don't count. */
#define CAP_FADE 16.0f
static void voice_cap(drum808_t *d, int t)
{
    for (;;) {
        int k, n = 0, oldest = -1;
        for (k = 0; k < D8_NUM; k++) {
            const d8_lane_t *l = &d->lane[k];
            if (k == t || !lane_on(d, k))
                continue;
            if (l->cstep != 0.0f)
                continue;
            n++;
            if (oldest < 0 || (int32_t)(l->serial - d->lane[oldest].serial) < 0)
                oldest = k;
        }
        if (n < d->max_voices || oldest < 0)
            return;
        choke_lane(&d->lane[oldest], CAP_FADE);
    }
}

static float lane_hz(int t, float tune) { return d8_midicps(k_base_note[t] + tune); }

void drum808_trigger(drum808_t *d, int track, float vel)
{
    float vgain, volts, soft, tune, decay;
    const float *pv;
    const uint8_t *pot;
    d8_lane_t *ln;
    if (track < 0 || track >= D8_NUM || !(vel > 0.0f))
        return;                                   /* drums are one-shots: no note-off */
    if (vel > 1.0f)
        vel = 1.0f;
    pv = d->potv[track];
    pot = d->pot[track];
    tune = pv[D8P_TUNE];
    decay = pv[D8P_DECAY];

    /* 8W8's velocity line (the kit's Accent pot at full): a trigger voltage on the circuit
     * lanes (4 V floor, 7.3 V at full), the rest as gain */
    vgain = SC808_FULL_VELOCITY_GAIN * vel;
    volts = 4.0f + 10.0f * ((vgain > 1.0f ? vgain : 1.0f) - 1.0f) / 3.0f;
    if (volts > 14.0f) volts = 14.0f;
    soft = vgain < 1.0f ? vgain : 1.0f;

    voice_cap(d, track);
    ln = &d->lane[track];
    ln->serial = ++d->serial;
    ln->hit = vgain;
    ln->pk = 0.0f;
    ln->qn = 0;
    ln->cg = 1.0f;
    ln->cstep = 0.0f;

    /* the hats share one metal source: CH cuts OH */
    if (track == D8_CH) {
        choke_lane(&d->lane[D8_OH], 0.002f * SR);
        hat_choke(&d->oh);
    }

    switch (track) {
    case D8_BD:
        bd_trigger(&d->bd, lane_hz(track, tune), (float)pot[D8P_DECAY] / 127.0f, (float)pot[D8P_X1] / 127.0f,
                   (float)pot[D8P_X2] / 127.0f, volts);
        ln->hit = soft;
        break;
    case D8_SD: {
        /* Tone: the 808 panel's (8W8 fixes it at the centre, 0.5 = pot 64) */
        int tp = pot[D8P_X1];
        float tone = tp <= 64 ? (float)tp * (1.0f / 128.0f) : 0.5f + (float)(tp - 64) * (0.5f / 63.0f);
        sd_trigger(&d->sd, fm_exp2f(tune / 12.0f), (float)pot[D8P_DECAY] / 127.0f, tone,
                   (float)pot[D8P_X2] / 127.0f, volts);
        ln->hit = soft;
        break;
    }
    case D8_LT:
        tom_trigger(&d->lt, lane_hz(track, tune), decay, volts);
        ln->hit = soft;
        break;
    case D8_CP:
        cp_trigger(&d->cp, fm_exp2f(tune / 12.0f), decay);
        break;
    case D8_CB:
        bank_ratio(&d->bank, tune);
        cb_ratio(&d->cb, tune);
        cb_trigger(&d->cb, decay, volts);
        ln->hit = soft;
        break;
    case D8_CH:                                   /* hats: velocity is a gain */
        bank_ratio(&d->bank, tune);
        hat_trigger(&d->ch, decay, 4.0f);
        break;
    default:                                      /* OH */
        bank_ratio(&d->bank, tune);
        hat_trigger(&d->oh, decay, 4.0f);
        break;
    }
}

static int voice_active(const drum808_t *d, int t)
{
    switch (t) {
    case D8_BD: return d->bd.active;
    case D8_SD: return d->sd.active;
    case D8_LT: return d->lt.active;
    case D8_CP: return d->cp.active;
    case D8_CH: return d->ch.active;
    case D8_OH: return d->oh.active;
    default: return d->cb.active;
    }
}

static void voice_stop(drum808_t *d, int t)
{
    switch (t) {
    case D8_BD: d->bd.active = 0; break;
    case D8_SD: d->sd.active = 0; break;
    case D8_LT: d->lt.active = 0; break;
    case D8_CP: d->cp.active = 0; break;
    case D8_CH: d->ch.active = 0; break;
    case D8_OH: d->oh.active = 0; break;
    default: d->cb.active = 0; break;
    }
}

/* X0X: a voice's tail ends once it has stayed 60 dB (tail_k) under the hit's own peak for 20 ms (8W8
 * runs each voice down to -90 dBFS-ish, a long, inaudible and costly tail in a mix) */
#define D8_TAIL_N 960                  /* 20 ms: the gaps between a clap's bursts are shorter */
#define D8_TAIL_FLOOR 1.0e-2f          /* the hit has sounded (voices peak at 0.2 .. 7) */
static void lane_tail(drum808_t *d, int t, const float *buf, int m)
{
    d8_lane_t *ln = &d->lane[t];
    float pk = ln->pk, k = d->tail_k;
    int32_t qn = ln->qn;
    int i;
    for (i = 0; i < m; i++) {
        float a = buf[i] < 0.0f ? -buf[i] : buf[i];
        if (a > pk)
            pk = a;
        if (a > pk * k)
            qn = 0;
        else
            qn++;
    }
    ln->pk = pk;
    ln->qn = qn;
    if (qn > D8_TAIL_N && pk > D8_TAIL_FLOOR)
        voice_stop(d, t);
}

static int lane_on(const drum808_t *d, int t)
{
    return d->lane[t].cg > 0.0f && voice_active(d, t);
}

int drum808_active(const drum808_t *d)
{
    int t;
    for (t = 0; t < D8_NUM; t++)
        if (lane_on(d, t))
            return 1;
    return 0;
}

/* Run one voice for up to m samples into buf; stops where the voice went inactive. Returns the
 * samples written. */
/* Run one voice for up to m samples into buf; stops where the voice went inactive. Returns the
 * samples written. Each voice runs on a local copy of its state (written back after), which the
 * compiler keeps in registers: on the device, reloading and storing the state every sample
 * through the struct cost about as much as the arithmetic. */
#define RUN(T, field, tick)                                \
    do {                                                   \
        T v = d->field;                                    \
        for (i = 0; i < m && v.active; i++)                \
            buf[i] = tick;                                 \
        d->field = v;                                      \
    } while (0)

static int voice_run(drum808_t *d, int t, float *restrict buf, int m, const float *restrict bus, const float *restrict pair)
{
    int i = 0;
    switch (t) {
    case D8_BD: RUN(d8_bd_t, bd, bd_tick(&v)); break;
    case D8_SD: RUN(d8_sd_t, sd, sd_tick(&v)); break;
    case D8_LT: RUN(d8_tom_t, lt, tom_tick(&v)); break;
    case D8_CP: RUN(d8_cp_t, cp, cp_tick(&v)); break;
    case D8_CH: RUN(d8_hat_t, ch, hat_tick(&v, bus[i])); break;
    case D8_OH: RUN(d8_hat_t, oh, hat_tick(&v, bus[i])); break;
    default: RUN(d8_cb_t, cb, cb_tick(&v, pair[i])); break;
    }
    return i;
}

/* trim x level x velocity x kit level x choke, into out */
static void lane_mix(drum808_t *d, int t, const float *buf, int m, float *out)
{
    d8_lane_t *l = &d->lane[t];
    float k = k_trim[t] * d->potv[t][D8P_LEVEL] * l->hit * d->vol;
    int i;
    if (l->cstep == 0.0f) {                       /* the common case */
        k *= l->cg;
        for (i = 0; i < m; i++)
            out[i] += buf[i] * k;
        return;
    }
    for (i = 0; i < m; i++) {
        l->cg += l->cstep;
        if (l->cg <= 0.0f) {
            l->cg = 0.0f;
            l->cstep = 0.0f;
            break;
        }
        out[i] += buf[i] * k * l->cg;
    }
}

void drum808_render(drum808_t *d, float *out, int n)
{
    float *buf = d->buf, *bus = d->bus, *pair = d->pair;
    int off, t, i;
    for (off = 0; off < n; off += CHUNK) {
        int m = n - off < CHUNK ? n - off : CHUNK;
        int cb_on = lane_on(d, D8_CB);
        /* the shared bank runs only while a metal voice sounds */
        if (cb_on || lane_on(d, D8_CH) || lane_on(d, D8_OH)) {
            d8_bank_t b = d->bank;
            for (i = 0; i < m; i++) {
                bus[i] = bank_tick(&b);
                if (cb_on)
                    pair[i] = ((b.ph[4] < BANK_DUTY ? 2.5f : -2.5f) + (b.ph[5] < BANK_DUTY ? 2.5f : -2.5f)) * 0.5f;
            }
            d->bank = b;
        }
        for (t = 0; t < D8_NUM; t++) {
            d8_lane_t *ln = &d->lane[t];
            int mm = m, got;
            if (!lane_on(d, t))
                continue;
            if (ln->cstep < 0.0f) {               /* 8W8 stops calling a voice whose choke ran out */
                float c = ln->cg;
                for (i = 0; i < m; i++) {
                    c += ln->cstep;
                    if (c <= 0.0f) {
                        mm = i + 1;
                        break;
                    }
                }
            }
            got = voice_run(d, t, buf, mm, bus, pair);
            lane_mix(d, t, buf, got, out + off);
            lane_tail(d, t, buf, got);
        }
    }
}
