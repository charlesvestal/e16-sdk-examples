/* SPDX-License-Identifier: GPL-3.0-only */
/* x0x-lite patterns: defaults, the factory loop and the UI's edits; see pattern.h. */
#include "pattern.h"
#include "drum808.h"
#include "tb3po.h"

float pattern_level_vel(int level)
{
    if (level <= 0)
        return 0.0f;
    if (level >= X_LVL_MAX)
        return 1.0f;                              /* the accent */
    return (float)level * (D8_VEL_NORMAL / (float)X_LVL_NORMAL);
}

static uint8_t root_note(const tb3po_cfg_t *g) { return (uint8_t)(12 * (g->base_oct + 1) + g->root); }

void pattern_init(pattern_t *p, uint32_t seed)
{
    uint32_t i;
    uint8_t *z = (uint8_t *)p;
    for (i = 0; i < sizeof *p; i++)
        z[i] = 0;
    p->drum_len = X_NSTEPS;
    p->bass_len = X_NSTEPS;
    tb3po_defaults(&p->gen, seed);
    for (i = 0; i < X_NSTEPS; i++) {
        p->bass[i].note = root_note(&p->gen);
        p->bass[i].flags = G_REST;
    }
}

void pattern_factory(pattern_t *p)
{
    int s;
    for (s = 0; s < X_NSTEPS; s++) {
        if (s % 4 == 0)
            p->drum[D8_BD][s] = (s % 8 == 0) ? X_LVL_MAX : X_LVL_NORMAL;   /* accents on 1 and 3 */
        if (s % 8 == 4)
            p->drum[D8_CP][s] = X_LVL_NORMAL;
        if (s % 4 == 2)
            p->drum[D8_OH][s] = X_LVL_NORMAL;
    }
    tb3po_generate(p);
}

int pattern_drum_toggle(pattern_t *p, int track, int step)
{
    uint8_t *v;
    if (track < 0 || track >= X_NDRUM || step < 0 || step >= X_NSTEPS)
        return 0;
    v = &p->drum[track][step];
    *v = *v ? 0 : X_LVL_NORMAL;
    return *v;
}

int pattern_drum_nudge(pattern_t *p, int track, int step, int d)
{
    uint8_t *v;
    int n;
    if (track < 0 || track >= X_NDRUM || step < 0 || step >= X_NSTEPS)
        return 0;
    v = &p->drum[track][step];
    n = *v ? *v + d : (d > 0 ? X_LVL_NORMAL : X_LVL_NORMAL - 1);   /* an off step comes on near normal */
    n = n < 1 ? 1 : n > X_LVL_MAX ? X_LVL_MAX : n;
    *v = (uint8_t)n;
    return n;
}

void pattern_bass_toggle(pattern_t *p, int step)
{
    bstep_t *b;
    if (step < 0 || step >= X_NSTEPS)
        return;
    b = &p->bass[step];
    b->flags = (uint8_t)((b->flags & ~BS_GATE_MASK) | (bstep_gate(b) == G_REST ? G_NOTE : G_REST));
}

void pattern_bass_nudge(pattern_t *p, int step, int d, int lo, int hi)
{
    bstep_t *b;
    int n;
    if (step < 0 || step >= X_NSTEPS)
        return;
    b = &p->bass[step];
    if (bstep_gate(b) == G_REST)
        b->flags = (uint8_t)((b->flags & ~BS_GATE_MASK) | G_NOTE);
    n = b->note + d;
    b->note = (uint8_t)(n < lo ? lo : n > hi ? hi : n);
}

void pattern_bass_flags(pattern_t *p, int step, int d)
{
    /* none -> accent -> slide -> accent + slide */
    static const uint8_t order[4] = {0, BS_ACCENT, BS_SLIDE, BS_ACCENT | BS_SLIDE};
    bstep_t *b;
    int k, cur = 0;
    if (step < 0 || step >= X_NSTEPS || d == 0)
        return;
    b = &p->bass[step];
    for (k = 0; k < 4; k++)
        if ((b->flags & (BS_ACCENT | BS_SLIDE)) == order[k])
            cur = k;
    cur += d > 0 ? 1 : -1;
    cur = cur < 0 ? 0 : cur > 3 ? 3 : cur;
    b->flags = (uint8_t)((b->flags & BS_GATE_MASK) | order[cur]);
}

void pattern_clear(pattern_t *p)
{
    int t, s;
    for (t = 0; t < X_NDRUM; t++)
        for (s = 0; s < X_NSTEPS; s++)
            p->drum[t][s] = 0;
    for (s = 0; s < X_NSTEPS; s++)
        p->bass[s].flags = G_REST;
}
