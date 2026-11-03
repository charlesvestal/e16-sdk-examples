/* SPDX-License-Identifier: GPL-3.0-only */
/* x0x-lite sequencer (X0X's seq/sequencer.c, cut down); see seq.h for the model. */
#include "seq.h"
#include "tb3po.h"

#define NEVER 0xFFFFFFFFu
#define TICKS_PER_STEP 6u                 /* 24 ppqn: a 16th */

void seq_init(seq_t *s, pattern_t *patterns)
{
    uint32_t i;
    uint8_t *p = (uint8_t *)s;
    for (i = 0; i < sizeof *s; i++)
        p[i] = 0;
    s->pat = patterns;
    s->pcue = 0xFF;
    s->bpm = 125.0f;
    s->ext_bpm = 125.0f;
    s->send_clock = 1;
    for (i = 0; i < SEQ_NPARTS; i++)
        s->t[i].to_next = NEVER;
    s->clk_to_next = NEVER;
    s->mut_rng = 0;
}

float seq_tempo(const seq_t *s) { return s->ext ? s->ext_bpm : s->bpm; }

uint32_t seq_step_q8(const seq_t *s)
{
    float bpm = seq_tempo(s), q;
    if (bpm < 20.0f)
        bpm = 20.0f;
    if (bpm > 300.0f)
        bpm = 300.0f;
    q = (float)SEQ_FS * 60.0f / bpm;               /* samples per quarter */
    return (uint32_t)(q * 256.0f * 0.25f);
}

static pattern_t *cur(const seq_t *s) { return &s->pat[s->ppat < X_NPAT ? s->ppat : 0]; }

int seq_part_len(const seq_t *s, int t)
{
    int n = t == SEQ_KIT ? cur(s)->drum_len : cur(s)->bass_len;
    return n < 1 ? 1 : n > X_NSTEPS ? X_NSTEPS : n;
}

/* swing offset of a step, Q8: odd 16ths come late */
static uint32_t swing_q8(const seq_t *s, uint32_t step_q8)
{
    uint32_t sw = s->swing;
    if (sw > 100u)
        sw = 100u;
    return step_q8 / 200u * sw;                     /* 100 -> half a step late = 75 % */
}

static int next_pos(const seq_t *s, int t, int p) { return (p + 1) % seq_part_len(s, t); }

static void midi(const seq_sink_t *o, uint8_t st)
{
    if (o->midi)
        o->midi(o->ctx, st);
}

static void bass_release(seq_t *s, const seq_sink_t *o)
{
    seq_track_t *tr = &s->t[SEQ_BASS];
    if (tr->sounding) {
        o->bass_off(o->ctx);
        tr->sounding = 0;
    }
    tr->to_off = 0;
}

static void fire_drum(seq_t *s, int p, const seq_sink_t *o)
{
    const pattern_t *pt = cur(s);
    int v;
    for (v = 0; v < X_NDRUM; v++) {
        int lvl = pt->drum[v][p];
        if (!lvl || (s->mute & (1u << v)))
            continue;
        o->drum(o->ctx, v, pattern_level_vel(lvl));
    }
}

static void fire_bass(seq_t *s, int p, uint32_t step_q8, uint32_t carry, const seq_sink_t *o)
{
    seq_track_t *tr = &s->t[SEQ_BASS];
    const pattern_t *pt = cur(s);
    const bstep_t *st = &pt->bass[p];
    int gate = bstep_gate(st), note, hold, nx;
    uint8_t prev_slide = tr->held_note & 0x80u;     /* bit 7: the step that sounded slides on */
    if (s->mute & (1u << SEQ_MUTE_BASS))
        gate = G_REST;
    if (gate == G_REST) {
        bass_release(s, o);
        tr->held_note = 0;
        return;
    }
    note = st->note + (int)s->xpose;
    note = note < 0 ? 0 : note > 127 ? 127 : note;
    if (gate == G_NOTE || !tr->sounding) {
        int slide = tr->sounding && prev_slide;
        o->bass_on(o->ctx, note, (st->flags & BS_ACCENT) != 0, slide);
        tr->sounding = 1;
    }
    nx = next_pos(s, SEQ_BASS, p);
    hold = (st->flags & BS_SLIDE) || bstep_gate(&pt->bass[nx]) == G_TIE;
    tr->held_note = (uint8_t)((note & 0x7F) | ((st->flags & BS_SLIDE) ? 0x80 : 0));
    tr->to_off = hold ? 0 : carry + step_q8 / 2u;
}

/* the bar's end (the kit wrapping): a cued pattern takes over. Returns 1 if it changed. */
static int bar_end(seq_t *s)
{
    if (s->pcue < X_NPAT) {
        int changed = s->pcue != s->ppat;
        s->ppat = s->pcue;
        s->pcue = 0xFF;
        return changed;
    }
    return 0;
}

static void auto_mutate(seq_t *s)
{
    pattern_t *pt = cur(s);
    seq_track_t *tr = &s->t[SEQ_BASS];
    tr->bars++;
    if (pt->gen.mutate_bars && tr->bars % pt->gen.mutate_bars == 0) {
        tb3po_mutate(pt, &s->mut_rng);
        s->mutated++;
    }
}

/* the new pattern's 303 from step 1, now: the kit's wrap is on a step (and clock) boundary */
static void restart_bass(seq_t *s)
{
    seq_track_t *tr = &s->t[SEQ_BASS];
    tr->started = 0;
    tr->to_next = 0;
    tr->ext_ticks = 0;
    tr->bars = 0;
}

/* fire part t's next step now; returns 1 if the pattern changed (the kit wrapping) */
static int fire_step(seq_t *s, int t, const seq_sink_t *o)
{
    seq_track_t *tr = &s->t[t];
    int p, wrapped, changed = 0;
    uint32_t L = seq_step_q8(s);
    uint32_t carry = tr->to_next < 256u ? tr->to_next : 0;   /* the step was due this far into the sample: keep it */
    if (!tr->started) {
        p = 0;
        tr->started = 1;
        wrapped = 0;
    } else {
        p = next_pos(s, t, tr->pos);
        wrapped = p == 0;
    }
    if (wrapped && t == SEQ_KIT && (changed = bar_end(s)) != 0)
        p = 0;
    if (wrapped && t == SEQ_BASS)
        auto_mutate(s);
    tr->pos = (uint8_t)p;
    if (o->step)
        o->step(o->ctx, t, p);
    if (t == SEQ_KIT)
        fire_drum(s, p, o);
    else
        fire_bass(s, p, L, carry, o);
    if (s->ext) {
        tr->to_next = NEVER;                         /* the clock arms the next one */
    } else {
        /* the step that follows p: swung if it is an odd one */
        int np = next_pos(s, t, p);
        uint32_t sw = swing_q8(s, L);
        tr->to_next = carry + ((np & 1) ? L + sw : L - sw);
        if (!(np & 1) && (p & 1) == 0)               /* even -> even (odd lengths, wraps): no swing */
            tr->to_next = carry + L;
    }
    return changed;
}

static void do_start(seq_t *s, int from_top, const seq_sink_t *o)
{
    int t;
    if (from_top)
        bar_end(s);                                   /* a cue takes over at once */
    for (t = 0; t < SEQ_NPARTS; t++) {
        seq_track_t *tr = &s->t[t];
        if (from_top) {
            tr->started = 0;
            tr->bars = 0;
        }
        tr->to_off = 0;
        tr->to_next = s->ext ? NEVER : 0;
        tr->ext_ticks = TICKS_PER_STEP - 1u;          /* the first clock is the downbeat */
    }
    s->clk_to_next = 0;
    s->playing = 1;
    if (!s->ext && s->send_clock)
        midi(o, from_top ? 0xFA : 0xFB);
}

static void do_stop(seq_t *s, const seq_sink_t *o)
{
    int t;
    s->playing = 0;
    bass_release(s, o);
    s->t[SEQ_BASS].held_note = 0;
    for (t = 0; t < SEQ_NPARTS; t++)
        s->t[t].to_next = NEVER;
    if (!s->ext && s->send_clock)
        midi(o, 0xFC);
}

/* external clock ticks: arm the steps they complete */
static void ext_ticks(seq_t *s, uint32_t n)
{
    int t;
    for (; n; n--) {
        if (s->ext_since > 0 && s->ext_since < SEQ_FS) {  /* tempo: smoothed tick period */
            uint32_t q = s->ext_since << 8;
            s->ext_period = s->ext_period ? s->ext_period - (s->ext_period >> 3) + (q >> 3) : q;
            s->ext_bpm = (float)SEQ_FS * 60.0f * 256.0f / (24.0f * (float)s->ext_period);
        }
        s->ext_since = 0;
        if (!s->playing)
            continue;
        for (t = 0; t < SEQ_NPARTS; t++) {
            seq_track_t *tr = &s->t[t];
            if (++tr->ext_ticks < TICKS_PER_STEP)
                continue;
            tr->ext_ticks = 0;
            {
                int np = tr->started ? next_pos(s, t, tr->pos) : 0;
                uint32_t L = s->ext_period ? s->ext_period * TICKS_PER_STEP : seq_step_q8(s);
                tr->to_next = (np & 1) ? swing_q8(s, L) : 0;
            }
        }
    }
}

uint32_t seq_until_event(seq_t *s, uint32_t n)
{
    uint32_t m = n, e;
    int t;
    if (!s->playing)
        return n;
    for (t = 0; t < SEQ_NPARTS; t++) {
        if (s->t[t].to_next != NEVER && (e = s->t[t].to_next >> 8) < m)
            m = e;
        if (s->t[t].to_off && (e = s->t[t].to_off >> 8) < m)
            m = e;
    }
    if (!s->ext && s->send_clock && s->clk_to_next != NEVER && (e = s->clk_to_next >> 8) < m)
        m = e;
    return m ? m : 1;
}

void seq_advance(seq_t *s, uint32_t k, const seq_sink_t *o)
{
    int t, guard;
    uint32_t dq = k << 8, ext_new = s->ext_req - s->ext_done;
    s->ext_done += ext_new;
    if (s->ext_since < 0x7FFFFFFFu)
        s->ext_since += k;
    if (ext_new) {
        s->ext = 1;
    } else if (s->ext && s->ext_since > SEQ_FS / 2u) {   /* clock gone for 0.5 s: back to internal */
        s->ext = 0;
        s->ext_period = 0;
        for (t = 0; t < SEQ_NPARTS; t++)
            if (s->playing && s->t[t].to_next == NEVER)
                s->t[t].to_next = 0;
    }
    if (s->stop_req != s->stop_done) {
        s->stop_done = s->stop_req;
        s->start_done = s->start_req;               /* a stop cancels a start asked for with it */
        s->cont_done = s->cont_req;
        if (s->playing)
            do_stop(s, o);
    }
    if (s->start_req != s->start_done) {
        s->start_done = s->start_req;
        do_start(s, 1, o);
    }
    if (s->cont_req != s->cont_done) {
        s->cont_done = s->cont_req;
        if (!s->playing)
            do_start(s, 0, o);
    }
    if (ext_new)
        ext_ticks(s, ext_new);
    if (!s->playing) {
        if (s->pcue < X_NPAT) {                      /* stopped: a cue takes effect at once */
            s->ppat = s->pcue;
            s->pcue = 0xFF;
        }
        return;
    }
    for (t = 0; t < SEQ_NPARTS; t++) {              /* move time */
        seq_track_t *tr = &s->t[t];
        if (tr->to_next != NEVER)
            tr->to_next = tr->to_next > dq ? tr->to_next - dq : 0;
        if (tr->to_off)
            tr->to_off = tr->to_off > dq ? tr->to_off - dq : 1;   /* 1: due, still "set" */
    }
    if (!s->ext && s->send_clock)
        s->clk_to_next = s->clk_to_next > dq ? s->clk_to_next - dq : 0;
    /* fire what is due; the kit first, since its wrap may switch the pattern */
    for (guard = 0; guard < 8; guard++) {
        int fired = 0;
        for (t = 0; t < SEQ_NPARTS; t++) {
            seq_track_t *tr = &s->t[t];
            if (tr->to_off && tr->to_off < 256u) {    /* gate falls (before a new step at the same time) */
                bass_release(s, o);
                fired = 1;
            }
            if (tr->to_next < 256u) {
                if (fire_step(s, t, o))
                    restart_bass(s);
                fired = 1;
            }
        }
        if (!fired)
            break;
    }
    if (!s->ext && s->send_clock && s->clk_to_next < 256u) {
        midi(o, 0xF8);
        s->clk_to_next += seq_step_q8(s) / TICKS_PER_STEP;   /* 24 ppqn */
    }
}

void seq_start(seq_t *s) { s->start_req++; }
void seq_stop(seq_t *s) { s->stop_req++; }
void seq_continue(seq_t *s) { s->cont_req++; }
void seq_ext_clock(seq_t *s) { s->ext_req++; }
void seq_cue(seq_t *s, int p)
{
    if (p >= 0 && p < X_NPAT)
        s->pcue = (uint8_t)p;
}
