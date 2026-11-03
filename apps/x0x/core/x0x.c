/* SPDX-License-Identifier: GPL-3.0-only */
/* x0x-lite engine; see x0x.h. */
#include "x0x.h"
#include "fastmath.h"
#include "tb3po.h"

enum { CMD_SET, CMD_DRUM, CMD_BASS_ON, CMD_BASS_OFF };

#define X_BLOCK 32                     /* render in pieces of at most this (the E16's block) */

/* ---- parameters ---- */

static const x0x_param_t k_mix[X_MIX_N] = {
    {"Kit", 127, 90, 0},              /* level: 2 (v/127)^2, 90 = unity */
    {"303", 127, 72, 0},
    {"Drive", 127, 0, 0},             /* into the master's soft clip: 0..+12 dB */
    {"Volume", 127, 120, 0},          /* out: (v/127)^2 */
};

int x0x_nparams(int t)
{
    if (t >= X_T_DRUM0 && t < X_T_DRUM0 + X_NDRUM)
        return drum808_nparams(t - X_T_DRUM0);
    if (t == X_T_BASS)
        return bass303_nparams();
    return t == X_T_MIX ? X_MIX_N : 0;
}

const x0x_param_t *x0x_param(int t, int i)
{
    if (t >= X_T_DRUM0 && t < X_T_DRUM0 + X_NDRUM)
        return drum808_param(t - X_T_DRUM0, i);
    if (t == X_T_BASS)
        return bass303_param(i);
    if (t == X_T_MIX && i >= 0 && i < X_MIX_N)
        return &k_mix[i];
    return 0;
}

int x0x_get(const x0x_t *x, int t, int i)
{
    return x0x_param(t, i) ? x->pot[t][i] : 0;
}

void x0x_set(x0x_t *x, int t, int i, int v)
{
    const x0x_param_t *p = x0x_param(t, i);
    if (!p)
        return;
    v = v < 0 ? 0 : v > p->max ? p->max : v;
    if (x->pot[t][i] == v)
        return;
    x->pot[t][i] = (uint8_t)v;
    x->dirty[t] |= (uint16_t)(1u << i);
}

static float level_gain(int v)
{
    float g = (float)v * (1.0f / 127.0f);
    return 2.0f * g * g;
}

/* a parameter into the engines (the callback's, or before audio starts) */
static void apply(x0x_t *x, int t, int i, int v)
{
    if (t >= X_T_DRUM0 && t < X_T_DRUM0 + X_NDRUM) {
        drum808_set(&x->kit, t - X_T_DRUM0, i, v);
    } else if (t == X_T_BASS) {
        bass303_set(&x->bass, i, v);
    } else if (t == X_T_MIX) {
        switch (i) {
        case X_MIX_KIT: x->kit_gain = level_gain(v); break;
        case X_MIX_BASS: x->bass_gain = level_gain(v); break;
        case X_MIX_DRIVE: x->drive = fm_exp2f((float)v * (2.0f / 127.0f)); break;
        default: x->volume = 0.5f * level_gain(v); break;
        }
    }
}

static int cmd_put(x0x_t *x, int kind, int a, int b, int c)
{
    uint32_t h = x->cmd_head;
    x0x_cmd_t *m;
    if (h - x->cmd_tail >= X_CMDQ)
        return 0;
    m = &x->cmd[h & (X_CMDQ - 1u)];
    m->kind = (uint8_t)kind;
    m->a = (uint8_t)a;
    m->b = (uint8_t)b;
    m->c = (uint8_t)c;
    x->cmd_head = h + 1u;            /* published after the entry (single core: program order) */
    return 1;
}

void x0x_flush(x0x_t *x)
{
    int t, i;
    for (t = 0; t < X_NTARGETS; t++) {
        while (x->dirty[t]) {
            for (i = 0; i < 16 && !(x->dirty[t] >> i & 1u); i++)
                ;
            if (!cmd_put(x, CMD_SET, t, i, x->pot[t][i]))
                return;               /* full: the rest next time */
            x->dirty[t] &= (uint16_t)~(1u << i);
        }
    }
}

void x0x_audition_drum(x0x_t *x, int track, float vel)
{
    int v = (int)(vel * 255.0f + 0.5f);
    cmd_put(x, CMD_DRUM, track, v < 0 ? 0 : v > 255 ? 255 : v, 0);
}

void x0x_audition_bass(x0x_t *x, int note, int accent)
{
    if (note < 0)
        cmd_put(x, CMD_BASS_OFF, 0, 0, 0);
    else
        cmd_put(x, CMD_BASS_ON, note > 127 ? 127 : note, accent != 0, 0);
}

int x0x_midi_out(x0x_t *x)
{
    uint32_t t = x->mo_tail;
    int b;
    if (t == x->mo_head)
        return -1;
    b = x->mo[t & (X_MIDIQ - 1u)];
    x->mo_tail = t + 1u;
    return b;
}

/* ---- the sequencer's sink (the callback's) ---- */

static int sink_drum(void *ctx, int v, float vel)
{
    x0x_t *x = ctx;
    drum808_trigger(&x->kit, v, vel);
    x->hits |= 1u << v;
    return 1;
}

static void sink_bass_on(void *ctx, int note, int accent, int slide)
{
    x0x_t *x = ctx;
    bass303_note_on(&x->bass, note, accent, slide);
    x->hits |= 1u << X_NDRUM;
}

static void sink_bass_off(void *ctx)
{
    x0x_t *x = ctx;
    bass303_note_off(&x->bass);
}

static void sink_midi(void *ctx, uint8_t st)
{
    x0x_t *x = ctx;
    uint32_t h = x->mo_head;
    if (h - x->mo_tail >= X_MIDIQ) {
        x->mo_dropped++;
        return;
    }
    x->mo[h & (X_MIDIQ - 1u)] = st;
    x->mo_head = h + 1u;
}

/* ---- render ---- */

static void drain(x0x_t *x)
{
    uint32_t t = x->cmd_tail;
    while (t != x->cmd_head) {
        const x0x_cmd_t *m = &x->cmd[t & (X_CMDQ - 1u)];
        switch (m->kind) {
        case CMD_SET:
            apply(x, m->a, m->b, m->c);
            break;
        case CMD_DRUM:
            drum808_trigger(&x->kit, m->a, (float)m->b * (1.0f / 255.0f));
            x->hits |= 1u << m->a;
            break;
        case CMD_BASS_ON:
            if (!x->seq.playing) {          /* the sequencer owns the 303 while it plays */
                bass303_note_on(&x->bass, m->a, m->b, 0);
                x->audition_note = 1;
            }
            break;
        default:                            /* CMD_BASS_OFF */
            if (x->audition_note) {
                x->audition_note = 0;
                if (!x->seq.playing)
                    bass303_note_off(&x->bass);
            }
            break;
        }
        t++;
    }
    x->cmd_tail = t;
}

/* the master's soft clip: linear to 0.5, then a tanh knee toward 1 */
static inline float soft_clip(float y)
{
    float a = fm_fabsf(y);
    if (a <= 0.5f)
        return y;
    a = 0.5f + 0.5f * fm_tanhf((a - 0.5f) * 2.0f);
    return y < 0.0f ? -a : a;
}

#define LIM_T 0.8f                          /* the limiter's ceiling, before the soft clip */
#define LIM_ATT 0.05f                       /* ~0.4 ms */
#define LIM_REL 0.99986112f                 /* exp(-1 / (150 ms * 48 kHz)) */

static void render_block(x0x_t *x, float *out, int n)
{
    float *bb = x->bb, *kb = x->kb;
    seq_sink_t sink = {sink_drum, sink_bass_on, sink_bass_off, sink_midi, 0, x};
    int off = 0, i;
    float env = x->lim_env, peak = x->peak;
    const float kg = x->kit_gain, bg = x->bass_gain, dr = x->drive, vol = x->volume;
    for (i = 0; i < n; i++)
        kb[i] = 0.0f;
    while (off < n) {
        int k = (int)seq_until_event(&x->seq, (uint32_t)(n - off));
        bass303_render(&x->bass, bb + off, k);
        drum808_render(&x->kit, kb + off, k);
        seq_advance(&x->seq, (uint32_t)k, &sink);
        off += k;
    }
    for (i = 0; i < n; i++) {
        float y = (kb[i] * kg + bb[i] * bg) * dr, a = fm_fabsf(y);
        if (!(a < 1e6f)) {                  /* NaN / runaway: silence, and the limiter forgets */
            y = 0.0f;
            a = 0.0f;
            env = 0.0f;
        }
        if (a > env)
            env += (a - env) * LIM_ATT;
        else
            env *= LIM_REL;
        if (env > LIM_T)
            y *= LIM_T / env;
        y = soft_clip(y) * vol;
        out[i] = y;
        a = fm_fabsf(y);
        if (a > peak)
            peak = a;
    }
    x->lim_env = fm_flush(env);
    x->peak = peak;
}

static const uint8_t k_guard[X_GUARD_MAX + 1][2] = {{D8_NUM, 60}, {5, 50}, {4, 40}, {3, 40}};

int x0x_guard_step(x0x_t *x, uint32_t load, int late)
{
    int g = x->guard;
    if (load > X_GUARD_HI || late) {
        x->guard_calm = 0;
        g += load > X_GUARD_JUMP || late ? 2 : 1;
        if (g > X_GUARD_MAX)
            g = X_GUARD_MAX;
    } else if (load < X_GUARD_LO && g > 0) {
        if (++x->guard_calm >= 6) {       /* 3 s at 0.5 s a call */
            x->guard_calm = 0;
            g--;
        }
    } else {
        x->guard_calm = 0;
    }
    x->guard = (uint8_t)g;
    return g;
}

void x0x_render(x0x_t *x, float *out, int n)
{
    uint8_t g = x->guard;
    if (g != x->guard_on && g <= X_GUARD_MAX) {
        x->guard_on = g;
        drum808_set_guard(&x->kit, k_guard[g][0], k_guard[g][1]);
    }
    drain(x);
    while (n > 0) {
        int m = n < X_BLOCK ? n : X_BLOCK;
        render_block(x, out, m);
        out += m;
        n -= m;
    }
    x->blocks++;
}

/* ---- defaults ---- */

/* the factory sound: X0X's acid 303 (low cutoff, high resonance, a long sweep, RAT) */
static const uint8_t k_bass_factory[][2] = {
    {BASS303_CUTOFF, 48}, {BASS303_RESO, 100}, {BASS303_ENVMOD, 85}, {BASS303_ACCENT, 100},
    {BASS303_VOLUME, 127}, {BASS303_DRIVE, 45}, {BASS303_DRVTYPE, 2},
};

void x0x_init(x0x_t *x)
{
    int t, i;
    {
        uint8_t *z = (uint8_t *)x;
        uint32_t k;
        for (k = 0; k < sizeof *x; k++)
            z[k] = 0;
    }
    for (i = 0; i < X_NPAT; i++)
        pattern_init(&x->pat[i], 0x3B0u + (uint32_t)i * 977u);
    pattern_factory(&x->pat[0]);
    seq_init(&x->seq, x->pat);
    bass303_init(&x->bass);
    drum808_init(&x->kit);
    for (t = 0; t < X_NTARGETS; t++)
        for (i = 0; i < x0x_nparams(t); i++)
            x->pot[t][i] = x0x_param(t, i)->def;
    for (i = 0; i < (int)(sizeof k_bass_factory / sizeof k_bass_factory[0]); i++)
        x->pot[X_T_BASS][k_bass_factory[i][0]] = k_bass_factory[i][1];
    for (t = 0; t < X_NTARGETS; t++)
        for (i = 0; i < x0x_nparams(t); i++)
            apply(x, t, i, x->pot[t][i]);
    x->follow_clock = 1;
}

/* ---- TB-3PO on the playing pattern ---- */

void x0x_generate(x0x_t *x, uint32_t entropy)
{
    pattern_t *p = &x->pat[x->seq.ppat < X_NPAT ? x->seq.ppat : 0];
    p->gen.seed = tb3po_new_seed(entropy);
    tb3po_generate(p);
}

void x0x_mutate(x0x_t *x)
{
    pattern_t *p = &x->pat[x->seq.ppat < X_NPAT ? x->seq.ppat : 0];
    uint32_t r = p->gen.seed ^ (x->blocks * 2654435761u);
    tb3po_mutate(p, &r);
}

/* ---- the saved state ----
 *
 *    0 magic "X0X1"           4 length (bytes, all of it)    8 CRC-32 of bytes 12..length
 *   12 X_NPAT X_NSTEPS X_NDRUM X_MAXP
 *   16 bpm x10 (u16), swing, send clock, follow clock, transpose (s8), pattern, mutes
 *   24 the pots, X_NTARGETS x X_MAXP
 *   then X_NPAT patterns: X_NDRUM x X_NSTEPS drum levels, X_NSTEPS x (note, flags), drum
 *        length, 303 length, TB-3PO density accent slide octaves root scale base-octave
 *        mutate-bars, seed (u32)
 * Little-endian, byte by byte (no struct layout in the file). */

#define HDR 24
#define PAT_BYTES (X_NDRUM * X_NSTEPS + 2 * X_NSTEPS + 2 + 8 + 4)
#define STATE_LEN (HDR + X_NTARGETS * X_MAXP + X_NPAT * PAT_BYTES)

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t crc32_of(const uint8_t *p, uint32_t n)       /* zlib CRC-32, 4 bits per step */
{
    static const uint32_t T[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        c = (c >> 4) ^ T[c & 15u];
        c = (c >> 4) ^ T[c & 15u];
    }
    return ~c;
}

uint32_t x0x_state_pack(const x0x_t *x, uint8_t *buf, uint32_t cap)
{
    uint8_t *q;
    int t, i, s;
    uint32_t bpm10;
    if (cap < STATE_LEN)
        return 0;
    put32(buf, X_STATE_MAGIC);
    put32(buf + 4, STATE_LEN);
    buf[12] = X_NPAT;
    buf[13] = X_NSTEPS;
    buf[14] = X_NDRUM;
    buf[15] = X_MAXP;
    bpm10 = (uint32_t)(x->seq.bpm * 10.0f + 0.5f);
    buf[16] = (uint8_t)bpm10;
    buf[17] = (uint8_t)(bpm10 >> 8);
    buf[18] = x->seq.swing;
    buf[19] = x->seq.send_clock;
    buf[20] = x->follow_clock;
    buf[21] = (uint8_t)x->seq.xpose;
    buf[22] = x->seq.pcue < X_NPAT ? x->seq.pcue : x->seq.ppat;
    buf[23] = (uint8_t)x->seq.mute;
    q = buf + HDR;
    for (t = 0; t < X_NTARGETS; t++)
        for (i = 0; i < X_MAXP; i++)
            *q++ = x->pot[t][i];
    for (i = 0; i < X_NPAT; i++) {
        const pattern_t *p = &x->pat[i];
        for (t = 0; t < X_NDRUM; t++)
            for (s = 0; s < X_NSTEPS; s++)
                *q++ = p->drum[t][s];
        for (s = 0; s < X_NSTEPS; s++) {
            *q++ = p->bass[s].note;
            *q++ = p->bass[s].flags;
        }
        *q++ = p->drum_len;
        *q++ = p->bass_len;
        *q++ = p->gen.density;
        *q++ = p->gen.accent;
        *q++ = p->gen.slide;
        *q++ = p->gen.oct_range;
        *q++ = p->gen.root;
        *q++ = p->gen.scale;
        *q++ = p->gen.base_oct;
        *q++ = p->gen.mutate_bars;
        put32(q, p->gen.seed);
        q += 4;
    }
    put32(buf + 8, crc32_of(buf + 12, STATE_LEN - 12));
    return STATE_LEN;
}

static uint8_t clamp8(int v, int lo, int hi) { return (uint8_t)(v < lo ? lo : v > hi ? hi : v); }

int x0x_state_unpack(x0x_t *x, const uint8_t *buf, uint32_t len)
{
    const uint8_t *q;
    int t, i, s;
    uint32_t bpm10;
    if (len < STATE_LEN || get32(buf) != X_STATE_MAGIC || get32(buf + 4) != STATE_LEN ||
        buf[12] != X_NPAT || buf[13] != X_NSTEPS || buf[14] != X_NDRUM || buf[15] != X_MAXP ||
        get32(buf + 8) != crc32_of(buf + 12, STATE_LEN - 12))
        return -1;
    bpm10 = (uint32_t)buf[16] | (uint32_t)buf[17] << 8;
    x->seq.bpm = bpm10 < 300u ? 30.0f : bpm10 > 3000u ? 300.0f : (float)bpm10 * 0.1f;
    x->seq.swing = clamp8(buf[18], 0, 100);
    x->seq.send_clock = buf[19] ? 1 : 0;
    x->follow_clock = buf[20] ? 1 : 0;
    x->seq.xpose = (int8_t)((int)clamp8((int8_t)buf[21] + X_XPOSE_MAX, 0, 2 * X_XPOSE_MAX) - X_XPOSE_MAX);
    x->seq.ppat = buf[22] < X_NPAT ? buf[22] : 0;
    x->seq.pcue = 0xFF;
    x->seq.mute = buf[23];
    q = buf + HDR;
    for (t = 0; t < X_NTARGETS; t++)
        for (i = 0; i < X_MAXP; i++, q++) {
            const x0x_param_t *p = x0x_param(t, i);
            if (p) {
                x->pot[t][i] = clamp8(*q, 0, p->max);
                apply(x, t, i, x->pot[t][i]);
            }
        }
    for (i = 0; i < X_NPAT; i++) {
        pattern_t *p = &x->pat[i];
        for (t = 0; t < X_NDRUM; t++)
            for (s = 0; s < X_NSTEPS; s++)
                p->drum[t][s] = clamp8(*q++, 0, X_LVL_MAX);
        for (s = 0; s < X_NSTEPS; s++) {
            p->bass[s].note = clamp8(*q++, X_NOTE_LO, X_NOTE_HI);
            p->bass[s].flags = (uint8_t)(*q++ & (BS_GATE_MASK | BS_ACCENT | BS_SLIDE));
            if (bstep_gate(&p->bass[s]) > G_TIE)
                p->bass[s].flags &= (uint8_t)~BS_GATE_MASK;
        }
        p->drum_len = clamp8(*q++, 1, X_NSTEPS);
        p->bass_len = clamp8(*q++, 1, X_NSTEPS);
        p->gen.density = clamp8(*q++, 0, 100);
        p->gen.accent = clamp8(*q++, 0, 100);
        p->gen.slide = clamp8(*q++, 0, 100);
        p->gen.oct_range = clamp8(*q++, 1, 3);
        p->gen.root = clamp8(*q++, 0, 11);
        p->gen.scale = clamp8(*q++, 0, TB3PO_NSCALES - 1);
        p->gen.base_oct = clamp8(*q++, X_BASE_OCT_MIN, X_BASE_OCT_MAX);
        p->gen.mutate_bars = clamp8(*q++, 0, 16);
        p->gen.seed = get32(q);
        q += 4;
    }
    for (t = 0; t < X_NTARGETS; t++)
        x->dirty[t] = 0;
    return 0;
}
