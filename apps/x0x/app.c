/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * x0x-lite: a groovebox for the E16 -- an 808 kit (seven voices) and a 303 with the TB-3PO
 * line generator, on a 16-step sequencer -- ported from X0X (see LICENSING.md). The engine is
 * core/ (portable C, host-tested by test/x0x/x0x_test.c); this is the controls, the screen,
 * the rings, MIDI and the saved state. docs/x0x.md has the full control table.
 *
 * The 16 encoders are the 16 steps of the selected track (or a page's 16 controls):
 *   drum track   push: step on / off       turn: its level (the top is the accent)
 *   303          push: gate on / off       turn: pitch      Shift + turn: accent / slide
 *   Shift + push 1-8: select a track (BD SD LT CP CH OH CB 303); again: its sound page
 *   Shift + push 9 sound, 10 global, 11 patterns, 12 mute / levels,
 *               13 new 303 line, 14 mutate it, 16 play / stop
 *   Shift held alone 5 s, then push 1: back to the launcher (the launcher's
 *     leave prompt; the state is saved on the way)
 *
 * MIDI in (USB and TRS): clock, start, continue and stop drive the sequencer (if "clock in"
 * is on); a note transposes the 303 (C4 = none). MIDI out (USB and TRS 1): clock and
 * transport while it runs on its own tempo (if "clock out" is on).
 *
 * The state (patterns, sound, settings) is saved to /apps/x0x/state.bin 2 s after the last
 * change, at most every 20 s (the SPI flash's erase budget), and when Shift has been held
 * 0.6 s (on the way out); it is loaded at start.
 */
#include "e16_app.h"
#include "gfx.h"
#include "tb3po.h"
#include "x0x.h"
#include "x0x_save.h"

#define FRAME_MS 40u
#define LED_MS 30u
#define MSG_MS 1500u
#define AUDITION_MS 180u
#define NTRACK 8                       /* the 7 drums and the 303 */
#define T303 7
#define NOTE_LO X_NOTE_LO              /* the 303 steps' range: C1 .. C6 (pattern.h) */
#define NOTE_HI X_NOTE_HI
#define OVERLOAD_RESTARTS 3            /* the launcher stopped the stream: restart this often, then give up */

/* ---- libc bits the compiler may call ---- */

void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *a = d;
    const unsigned char *b = s;
    while (n--)
        *a++ = *b++;
    return d;
}

void *memset(void *d, int c, size_t n)
{
    unsigned char *a = d;
    while (n--)
        *a++ = (unsigned char)c;
    return d;
}

static const e16_api_t *api;
static x0x_t g_x;
static uint32_t now_ms;

/* The audio callback: runs in an interrupt (e16_api.h's audio_start()). */
static void render(float *out, uint32_t frames, void *user)
{
    x0x_render((x0x_t *)user, out, (int)frames);
}

/* ---- text ---- */

static char *put_s(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    *p = 0;
    return p;
}

static char *put_u(char *p, uint32_t v)
{
    char tmp[10];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        *p++ = tmp[--n];
    *p = 0;
    return p;
}

static char *put_i(char *p, int v)
{
    if (v < 0) {
        *p++ = '-';
        v = -v;
    } else if (v > 0) {
        *p++ = '+';
    }
    return put_u(p, (uint32_t)v);
}

static const char *const note_names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

static char *put_note(char *p, int note)
{
    note = note < 12 ? 12 : note > 127 ? 127 : note;   /* (octave 0 up: no minus sign) */
    p = put_s(p, note_names[note % 12]);
    return put_u(p, (uint32_t)(note / 12 - 1));
}

/* ---- the UI's state ---- */

enum { P_STEPS, P_SOUND, P_GLOBAL, P_PATTERN, P_MUTE };
static const char *const page_names[] = {"STEPS", "SOUND", "GLOBAL", "PATTERN", "MUTE"};
static const char *const track_names[NTRACK] = {"BD", "SD", "LT", "CP", "CH", "OH", "CB", "303"};
static const char *const track_long[NTRACK] = {"Bass drum", "Snare", "Low tom", "Clap",
                                               "Closed hat", "Open hat", "Cowbell", "303"};

static int page = P_STEPS, track = 0;
static char msg[40];
static uint32_t msg_ms;
static int msg_on;
static x0x_save_t g_save;
static uint32_t last_turn_ms[16];
static int copy_dest = 1;              /* GLOBAL: the pattern "copy to" writes */
static uint32_t clear_armed_ms;        /* GLOBAL: the first push of "clear" */
static int audition_on;
static uint32_t audition_off_ms;

static void say(const char *a, const char *b)
{
    char *p = put_s(msg, a);
    if (b)
        put_s(p, b);
    msg_ms = now_ms;
    msg_on = 1;
}

static void changed(void) { x0x_save_changed(&g_save, now_ms); }

static pattern_t *cur_pat(void) { return &g_x.pat[g_x.seq.ppat < X_NPAT ? g_x.seq.ppat : 0]; }

/* turn speed: fast turns move the long ranges further */
static int accel(int e, int d, int span)
{
    int fast = now_ms - last_turn_ms[e] < 35u;
    last_turn_ms[e] = now_ms;
    if (span > 40 && fast)
        return d * 4;
    return d;
}

/* ---- the screen ---- */

static uint32_t cpu_permille, cpu_peak;
static int cpu_state;

static void title(void)
{
    char s[40], *p;
    if (msg_on && now_ms - msg_ms < MSG_MS) {
        gfx_text(0, 0, msg, 0);
    } else {
        msg_on = 0;
        p = put_s(s, page == P_STEPS || page == P_SOUND ? track_names[track] : page_names[page]);
        if (page == P_SOUND)
            put_s(p, " snd");
        gfx_text(0, 0, s, 0);
        p = put_s(s, "P");
        p = put_u(p, (uint32_t)g_x.seq.ppat + 1u);
        if (g_x.seq.pcue < X_NPAT) {
            p = put_s(p, ">");
            p = put_u(p, (uint32_t)g_x.seq.pcue + 1u);
        }
        p = put_s(p, " ");
        p = put_u(p, (uint32_t)(seq_tempo(&g_x.seq) + 0.5f));
        p = put_s(p, g_x.seq.ext ? "e " : " ");
        if (g_x.guard)
            p = put_s(put_u(put_s(p, "G"), g_x.guard), " ");
        if (cpu_state == E16_AUDIO_OVERLOAD)
            put_s(p, "OVL");
        else
            put_s(p, g_x.seq.playing ? ">" : "#");
        gfx_text(GFX_W - gfx_text_w(s), 0, s, 0);
    }
    gfx_fill(0, 9, GFX_W, 1, 1);
}

/* the steps page: every track's 16 steps, the selected one's row marked */
static void draw_steps(void)
{
    const pattern_t *pt = cur_pat();
    const int x0 = 9, cw = 7, y0 = 11, rh = 6;
    int pos_k = g_x.seq.t[SEQ_KIT].pos, pos_b = g_x.seq.t[SEQ_BASS].pos;
    for (int t = 0; t < NTRACK; t++) {
        int y = y0 + t * rh;
        int len = t == T303 ? pt->bass_len : pt->drum_len;
        int muted = (g_x.seq.mute >> t) & 1u;
        /* left of the row: an arrow on the selected track, a dash on a muted one (the title
         * names the selected track) */
        if (t == track) {
            gfx_fill(0, y, 1, 4, 1);
            gfx_fill(1, y, 1, 4, 1);
            gfx_fill(2, y + 1, 1, 2, 1);
            gfx_fill(3, y + 1, 1, 2, 1);
        }
        if (muted)
            gfx_fill(4, y + 2, 4, 1, 1);
        for (int s = 0; s < X_NSTEPS; s++) {
            int x = x0 + s * cw;
            if (s >= len) {
                gfx_px(x + 2, y + 2, 1);
                continue;
            }
            if (t == T303) {
                const bstep_t *b = &pt->bass[s];
                if (bstep_gate(b) != G_REST) {
                    gfx_fill(x, y, 5, 4, 1);
                    if (b->flags & BS_ACCENT)
                        gfx_fill(x + 1, y + 1, 3, 2, 0);
                    if (b->flags & BS_SLIDE)
                        gfx_fill(x + 5, y + 1, 2, 2, 1);
                } else {
                    gfx_px(x + 2, y + 2, 1);
                }
            } else {
                int lv = pt->drum[t][s];
                if (lv >= X_LVL_MAX) {
                    gfx_fill(x, y, 5, 4, 1);
                    gfx_fill(x + 1, y + 1, 3, 2, 0);
                    gfx_px(x + 2, y + 1, 1);
                } else if (lv > 0) {
                    int h = lv >= X_LVL_NORMAL ? 4 : lv >= 4 ? 3 : 2;
                    gfx_fill(x, y + 4 - h, 5, h, 1);
                } else {
                    gfx_px(x + 2, y + 2, 1);
                }
            }
        }
    }
    /* the playheads */
    if (g_x.seq.playing) {
        gfx_fill(x0 + pos_k * cw, 60, 5, 1, 1);
        gfx_fill(x0 + pos_b * cw, 62, 5, 2, 1);
    }
}

/* a 4 x 4 grid of controls: a label and a bar (or the label alone) per cell */
static void grid16(const char *const *labels, const int *frac /* 0..100, < 0 = none */, int hi)
{
    for (int i = 0; i < 16; i++) {
        int x = (i & 3) * 32, y = 11 + (i >> 2) * 13;
        if (!labels[i] || !labels[i][0])
            continue;
        gfx_text(x + 1, y, labels[i], i == hi);
        if (frac[i] >= 0) {
            gfx_fill(x + 1, y + 9, 29, 1, 1);
            gfx_fill(x + 1, y + 8, (29 * frac[i] + 50) / 100, 3, 1);
        }
    }
}

static int touched_ctl = -1;
static uint32_t touched_ms;

static void touch(int e)
{
    touched_ctl = e;
    touched_ms = now_ms;
}

static int sound_target(void) { return track == T303 ? X_T_BASS : X_T_DRUM0 + track; }

/* the sound page's encoder e: (target, param), or -1 */
static int sound_ctl(int e, int *t)
{
    int tg = sound_target(), n = x0x_nparams(tg);
    if (e >= 12) {
        *t = X_T_MIX;
        return e - 12;
    }
    *t = tg;
    return e < n ? e : -1;
}

static void param_text(char *s, int t, int i)
{
    const x0x_param_t *p = x0x_param(t, i);
    int v = x0x_get(&g_x, t, i);
    char *q = put_s(s, p->name);
    q = put_s(q, " ");
    if (p->names)
        put_s(q, p->names[v]);
    else
        put_u(q, (uint32_t)v);
}

static void draw_sound(void)
{
    const char *lab[16];
    int fr[16];
    char names[16][6];
    for (int e = 0; e < 16; e++) {
        int t, i = sound_ctl(e, &t);
        lab[e] = 0;
        fr[e] = -1;
        if (i < 0)
            continue;
        const x0x_param_t *p = x0x_param(t, i);
        int k = 0;
        for (; k < 5 && p->name[k]; k++)
            names[e][k] = p->name[k];
        names[e][k] = 0;
        lab[e] = names[e];
        fr[e] = x0x_get(&g_x, t, i) * 100 / p->max;
    }
    grid16(lab, fr, now_ms - touched_ms < MSG_MS ? touched_ctl : -1);
}

static const uint8_t mutate_steps[6] = {0, 1, 2, 4, 8, 16};

static void global_label(int e, char *s)
{
    const pattern_t *pt = cur_pat();
    char *p = s;
    switch (e) {
    case 0: p = put_s(p, "T"); put_u(p, (uint32_t)(g_x.seq.bpm + 0.5f)); break;
    case 1: p = put_s(p, "Sw"); put_u(p, g_x.seq.swing); break;
    case 2: p = put_s(p, "K"); put_u(p, pt->drum_len); break;
    case 3: p = put_s(p, "B"); put_u(p, pt->bass_len); break;
    case 4: p = put_s(p, "D"); put_u(p, pt->gen.density); break;
    case 5: p = put_s(p, "M"); put_u(p, pt->gen.mutate_bars); break;
    case 6: put_s(p, TB3PO_SCALE_NAMES[pt->gen.scale]); break;
    case 7: put_s(p, note_names[pt->gen.root]); break;
    case 8: p = put_s(p, "A"); put_u(p, pt->gen.accent); break;
    case 9: p = put_s(p, "S"); put_u(p, pt->gen.slide); break;
    case 10: p = put_s(p, "Oct"); put_u(p, pt->gen.oct_range); break;
    case 11: p = put_s(p, "Tr"); put_i(p, g_x.seq.xpose); break;
    case 12: put_s(p, g_x.seq.send_clock ? "CkO+" : "CkO-"); break;
    case 13: put_s(p, g_x.follow_clock ? "CkI+" : "CkI-"); break;
    case 14: p = put_s(p, "Cp>"); put_u(p, (uint32_t)copy_dest + 1u); break;
    default: put_s(p, clear_armed_ms && now_ms - clear_armed_ms < 2000u ? "CLR?" : "Clr"); break;
    }
}

static void draw_global(void)
{
    const char *lab[16];
    int fr[16];
    char names[16][8];
    for (int e = 0; e < 16; e++) {
        global_label(e, names[e]);
        names[e][5] = 0;
        lab[e] = names[e];
        fr[e] = -1;
    }
    grid16(lab, fr, now_ms - touched_ms < MSG_MS ? touched_ctl : -1);
}

static int pattern_empty(const pattern_t *p)
{
    for (int t = 0; t < X_NDRUM; t++)
        for (int s = 0; s < X_NSTEPS; s++)
            if (p->drum[t][s])
                return 0;
    for (int s = 0; s < X_NSTEPS; s++)
        if (bstep_gate(&p->bass[s]) != G_REST)
            return 0;
    return 1;
}

static void draw_patterns(void)
{
    for (int i = 0; i < X_NPAT; i++) {
        int x = (i & 3) * 32, y = 11 + (i >> 2) * 13;
        char s[4];
        put_u(s, (uint32_t)i + 1u);
        if (i == g_x.seq.ppat) {
            gfx_fill(x + 1, y - 1, 30, 11, 1);
            gfx_text(x + 4, y + 1, s, 1);
        } else {
            if (i == g_x.seq.pcue && (now_ms / 200u) & 1u)
                gfx_frame(x + 1, y - 1, 30, 11);
            gfx_text(x + 4, y + 1, s, 0);
        }
        if (!pattern_empty(&g_x.pat[i]))
            gfx_fill(x + 24, y + 3, 3, 3, i == g_x.seq.ppat ? 0 : 1);
    }
}

static void draw_mute(void)
{
    const char *lab[16];
    int fr[16];
    for (int e = 0; e < 16; e++) {
        lab[e] = 0;
        fr[e] = -1;
        if (e < NTRACK) {
            lab[e] = (g_x.seq.mute >> e & 1u) ? "--" : track_names[e];
            fr[e] = (e == T303 ? x0x_get(&g_x, X_T_MIX, X_MIX_BASS) : x0x_get(&g_x, X_T_DRUM0 + e, D8P_LEVEL)) * 100 / 127;
        }
    }
    grid16(lab, fr, now_ms - touched_ms < MSG_MS ? touched_ctl : -1);
}

static void draw(void)
{
    gfx_clear();
    title();
    switch (page) {
    case P_STEPS: draw_steps(); break;
    case P_SOUND: draw_sound(); break;
    case P_GLOBAL: draw_global(); break;
    case P_PATTERN: draw_patterns(); break;
    default: draw_mute(); break;
    }
    api->display_present();
}

/* ---- LED rings ---- */

#define LED_MAX 24
static uint8_t led_now[16][16][3], led_sent[16][16][3];
static int leds_pending;
static uint32_t hit_seen[NTRACK];      /* when each track last fired (for a flash) */

static const uint8_t track_rgb[NTRACK][3] = {
    {LED_MAX, 6, 0}, {LED_MAX, 16, 0}, {0, LED_MAX, 4}, {LED_MAX, 0, 14},
    {0, 14, LED_MAX}, {6, 6, LED_MAX}, {16, 16, 8}, {8, LED_MAX, 0},
};

static void ring_clear(int e)
{
    for (int i = 0; i < 16; i++)
        led_now[e][i][0] = led_now[e][i][1] = led_now[e][i][2] = 0;
}

static void ring_set(int e, int i, int r, int g, int b)
{
    if (i < 0 || i > 15)
        return;
    led_now[e][i][0] = (uint8_t)r;
    led_now[e][i][1] = (uint8_t)g;
    led_now[e][i][2] = (uint8_t)b;
}

static void ring_arc(int e, int n, const uint8_t *c, int dim_div)
{
    for (int i = 0; i <= n && i < 16; i++) {
        int div = i == n ? 1 : dim_div;
        ring_set(e, i, c[0] / div, c[1] / div, c[2] / div);
    }
}

static void ring_bg(int e, int v)
{
    for (int i = 0; i < 16; i++)
        if (!led_now[e][i][0] && !led_now[e][i][1] && !led_now[e][i][2])
            ring_set(e, i, v, v, v);
}

static void leds_steps(void)
{
    const pattern_t *pt = cur_pat();
    int bass = track == T303;
    int len = bass ? pt->bass_len : pt->drum_len;
    int pos = g_x.seq.t[bass ? SEQ_BASS : SEQ_KIT].pos;
    const uint8_t *c = track_rgb[track];
    for (int s = 0; s < 16; s++) {
        ring_clear(s);
        if (s >= len)
            continue;
        if (bass) {
            const bstep_t *b = &pt->bass[s];
            int on = bstep_gate(b) != G_REST;
            int oct = (b->note - NOTE_LO) / 12, pc = b->note % 12;
            static const uint8_t oct_rgb[5][3] = {{0, 8, LED_MAX}, {0, LED_MAX, 6}, {8, LED_MAX, 0},
                                                  {LED_MAX, 14, 0}, {LED_MAX, 0, 8}};
            const uint8_t *oc = oct_rgb[oct < 0 ? 0 : oct > 4 ? 4 : oct];
            int div = on ? 1 : 6;
            ring_set(s, pc, oc[0] / div, oc[1] / div, oc[2] / div);
            if (on && (b->flags & BS_ACCENT))
                ring_set(s, 13, LED_MAX, 0, 0);
            if (on && (b->flags & BS_SLIDE)) {
                ring_set(s, 14, 0, 0, LED_MAX);
                ring_set(s, 15, 0, 0, LED_MAX);
            }
        } else {
            int lv = pt->drum[track][s];
            if (lv >= X_LVL_MAX) {
                ring_arc(s, 15, (const uint8_t[3]){LED_MAX, 0, 0}, 3);
            } else if (lv > 0) {
                ring_arc(s, lv, c, 4);
            }
        }
        if (g_x.seq.playing && s == pos)
            ring_bg(s, 5);
    }
}

static void leds_sound(void)
{
    for (int e = 0; e < 16; e++) {
        int t, i = sound_ctl(e, &t);
        ring_clear(e);
        if (i < 0)
            continue;
        const x0x_param_t *p = x0x_param(t, i);
        const uint8_t *c = t == X_T_MIX ? (const uint8_t[3]){10, 10, 10} : track_rgb[track];
        ring_arc(e, x0x_get(&g_x, t, i) * 15 / p->max, c, 4);
    }
}

static void leds_global(void)
{
    const pattern_t *pt = cur_pat();
    static const uint8_t col[3] = {LED_MAX, 12, 0}, gen[3] = {8, LED_MAX, 0}, sys[3] = {0, 10, LED_MAX};
    int v[16] = {
        (int)(g_x.seq.bpm - 60.0f) * 15 / 140, g_x.seq.swing * 15 / 100, pt->drum_len - 1, pt->bass_len - 1,
        pt->gen.density * 15 / 100, pt->gen.mutate_bars, pt->gen.scale * 15 / (TB3PO_NSCALES - 1), pt->gen.root * 15 / 11,
        pt->gen.accent * 15 / 100, pt->gen.slide * 15 / 100, (pt->gen.oct_range - 1) * 7, (g_x.seq.xpose + X_XPOSE_MAX) * 15 / (2 * X_XPOSE_MAX),
        g_x.seq.send_clock ? 15 : -1, g_x.follow_clock ? 15 : -1, copy_dest, 0,
    };
    for (int e = 0; e < 16; e++) {
        ring_clear(e);
        const uint8_t *c = e < 4 ? col : e < 12 ? gen : sys;
        int n = v[e] < -1 ? 0 : v[e] > 15 ? 15 : v[e];
        if (e == 5) {
            for (int k = 0; k < 6; k++)
                if (mutate_steps[k] == pt->gen.mutate_bars)
                    n = k * 3;
        }
        if (n >= 0)
            ring_arc(e, n, c, 4);
    }
    if (clear_armed_ms && now_ms - clear_armed_ms < 2000u)
        for (int i = 0; i < 16; i++)
            ring_set(15, i, LED_MAX, 0, 0);
}

static void leds_patterns(void)
{
    for (int i = 0; i < 16; i++) {
        ring_clear(i);
        if (i == g_x.seq.ppat) {
            int pos = g_x.seq.t[SEQ_KIT].pos;
            ring_arc(i, 15, (const uint8_t[3]){0, LED_MAX, 8}, 5);
            if (g_x.seq.playing)
                ring_set(i, pos, LED_MAX, LED_MAX, LED_MAX);
        } else if (i == g_x.seq.pcue) {
            if ((now_ms / 200u) & 1u)
                ring_arc(i, 15, (const uint8_t[3]){LED_MAX, 12, 0}, 3);
        } else if (!pattern_empty(&g_x.pat[i])) {
            ring_set(i, 0, 6, 6, 6);
        }
    }
}

static void leds_mute(void)
{
    for (int e = 0; e < 16; e++) {
        ring_clear(e);
        if (e >= NTRACK)
            continue;
        int lvl = e == T303 ? x0x_get(&g_x, X_T_MIX, X_MIX_BASS) : x0x_get(&g_x, X_T_DRUM0 + e, D8P_LEVEL);
        if (g_x.seq.mute >> e & 1u)
            ring_arc(e, 15, (const uint8_t[3]){LED_MAX, 0, 0}, 6);
        else
            ring_arc(e, lvl * 15 / 127, track_rgb[e], 4);
        if (now_ms - hit_seen[e] < 60u)
            ring_bg(e, 10);
    }
}

static void leds_update(void)
{
    uint32_t hits = __atomic_exchange_n(&g_x.hits, 0u, __ATOMIC_RELAXED);   /* (x0x.h) */
    if (hits) {
        for (int t = 0; t < NTRACK; t++)
            if (hits >> t & 1u)
                hit_seen[t] = now_ms;
    }
    switch (page) {
    case P_STEPS: leds_steps(); break;
    case P_SOUND: leds_sound(); break;
    case P_GLOBAL: leds_global(); break;
    case P_PATTERN: leds_patterns(); break;
    default: leds_mute(); break;
    }
    for (int e = 0; e < 16; e++)
        for (int i = 0; i < 16; i++)
            if (led_now[e][i][0] != led_sent[e][i][0] || led_now[e][i][1] != led_sent[e][i][1] ||
                led_now[e][i][2] != led_sent[e][i][2]) {
                api->leds_set(e, i, led_now[e][i][0], led_now[e][i][1], led_now[e][i][2]);
                led_sent[e][i][0] = led_now[e][i][0];
                led_sent[e][i][1] = led_now[e][i][1];
                led_sent[e][i][2] = led_now[e][i][2];
                leds_pending = 1;
            }
    if (leds_pending && api->leds_show())
        leds_pending = 0;
}

/* ---- state ---- */

static uint8_t state_buf[X_STATE_MAX];

static int state_load(void)
{
    int fd = api->file_open("/apps/x0x/state.bin", E16_FILE_READ);
    if (fd < 0)
        return 0;
    int got = api->file_read(fd, state_buf, sizeof state_buf);
    api->file_close(fd);
    return got > 0 && x0x_state_unpack(&g_x, state_buf, (uint32_t)got) == 0;
}

static void try_save(const char *why)
{
    uint32_t n = x0x_state_pack(&g_x, state_buf, sizeof state_buf);
    int r = n ? api->file_write("state.bin", state_buf, n) : E16_EARGS;
    char s[48], *p;
    x0x_save_result(&g_save, now_ms, r == 0);
    if (r == 0) {
        p = put_s(s, "x0x: saved (");
        p = put_s(p, why);
        put_s(p, ")");
    } else {
        /* E16_EBUSY (the erase budget) or an error: later */
        p = put_s(s, "x0x: save deferred (");
        if (r < 0) {
            *p++ = '-';
            r = -r;
        }
        p = put_u(p, (uint32_t)r);
        put_s(p, ")");
    }
    api->log(s);
}

/* The user confirmed the launcher's leave prompt: save, then exit (the
 * launcher ends the app 1.5 s later either way). */
static int leaving(void)
{
    return E16_API_HAS(api, exit_pending) && api->exit_pending();
}

static void save_poll(uint32_t shift_held_ms)
{
    switch (x0x_save_poll(&g_save, now_ms, shift_held_ms)) {
    case X0X_SAVE_LEAVING:
        try_save("leaving");
        break;
    case X0X_SAVE_NOW:
        try_save("changed");
        break;
    default:
        break;
    }
}

/* ---- transport ---- */

static int overload_restarts;          /* restarts after the launcher stopped the stream (cpu_guard()) */

static void play_stop(void)
{
    if (overload_restarts > OVERLOAD_RESTARTS && cpu_state == E16_AUDIO_OVERLOAD) {
        overload_restarts = 0;          /* gave up on the stream (cpu_guard()): try again */
        g_x.guard = X_GUARD_MAX;
        if (api->audio_start(render, &g_x) == 0) {
            cpu_state = E16_AUDIO_RUNNING;
            say("Audio on again", 0);
        }
        return;
    }
    if (g_x.seq.playing) {
        seq_stop(&g_x.seq);
        say("Stop", 0);
    } else {
        seq_start(&g_x.seq);
        say("Play", 0);
    }
}

/* ---- input ---- */

static void audition_note(int note, int accent)
{
    if (g_x.seq.playing)
        return;
    x0x_audition_bass(&g_x, note, accent);
    audition_on = 1;
    audition_off_ms = now_ms + AUDITION_MS;
}

static void show_step(int s)
{
    char t[32], *p;
    const pattern_t *pt = cur_pat();
    p = put_s(t, "Step ");
    p = put_u(p, (uint32_t)s + 1u);
    p = put_s(p, " ");
    if (track == T303) {
        const bstep_t *b = &pt->bass[s];
        if (bstep_gate(b) == G_REST) {
            put_s(p, "rest");
        } else {
            p = put_note(p, b->note);
            if (b->flags & BS_ACCENT)
                p = put_s(p, " acc");
            if (b->flags & BS_SLIDE)
                put_s(p, " sld");
        }
    } else {
        int lv = pt->drum[track][s];
        if (!lv)
            put_s(p, "off");
        else if (lv >= X_LVL_MAX)
            put_s(p, "ACCENT");
        else
            put_u(put_s(p, "lvl "), (uint32_t)lv);
    }
    say(t, 0);
}

static void steps_push(int s)
{
    pattern_t *pt = cur_pat();
    if (track == T303) {
        pattern_bass_toggle(pt, s);
        if (bstep_gate(&pt->bass[s]) != G_REST)
            audition_note(pt->bass[s].note + g_x.seq.xpose, pt->bass[s].flags & BS_ACCENT);
    } else {
        int lv = pattern_drum_toggle(pt, track, s);
        if (lv && !g_x.seq.playing)
            x0x_audition_drum(&g_x, track, pattern_level_vel(lv));
    }
    changed();
    show_step(s);
}

static void steps_turn(int s, int d, int shift)
{
    pattern_t *pt = cur_pat();
    if (track == T303) {
        if (shift)
            pattern_bass_flags(pt, s, d);
        else {
            pattern_bass_nudge(pt, s, d, NOTE_LO, NOTE_HI);
            audition_note(pt->bass[s].note + g_x.seq.xpose, pt->bass[s].flags & BS_ACCENT);
        }
    } else {
        if (shift)
            return;
        int lv = pattern_drum_nudge(pt, track, s, d);
        if (!g_x.seq.playing)
            x0x_audition_drum(&g_x, track, pattern_level_vel(lv));
    }
    changed();
    show_step(s);
}

static void sound_turn(int e, int d)
{
    int t, i = sound_ctl(e, &t);
    char s[32];
    if (i < 0)
        return;
    const x0x_param_t *p = x0x_param(t, i);
    if (p->max < 127)
        d = d > 0 ? 1 : -1;
    else
        d = accel(e, d, p->max);
    x0x_set(&g_x, t, i, x0x_get(&g_x, t, i) + d);
    param_text(s, t, i);
    say(s, 0);
    touch(e);
    changed();
}

static void sound_push(int e)                  /* a push: back to the default */
{
    int t, i = sound_ctl(e, &t);
    char s[32];
    if (i < 0)
        return;
    x0x_set(&g_x, t, i, x0x_param(t, i)->def);
    param_text(s, t, i);
    say(s, " (dflt)");
    touch(e);
    changed();
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void global_turn(int e, int d)
{
    pattern_t *pt = cur_pat();
    tb3po_cfg_t *g = &pt->gen;
    char s[16], *p;
    int regen_line = 0;
    switch (e) {
    case 0: g_x.seq.bpm = (float)clampi((int)(g_x.seq.bpm + 0.5f) + accel(e, d, 100), 40, 240); break;
    case 1: g_x.seq.swing = (uint8_t)clampi(g_x.seq.swing + accel(e, d, 100), 0, 100); break;
    case 2: pt->drum_len = (uint8_t)clampi(pt->drum_len + d, 1, X_NSTEPS); break;
    case 3: pt->bass_len = (uint8_t)clampi(pt->bass_len + d, 1, X_NSTEPS); break;
    case 4: g->density = (uint8_t)clampi(g->density + accel(e, d, 100), 0, 100); regen_line = 1; break;
    case 5: {
        int k = 0;
        for (int i = 0; i < 6; i++)
            if (mutate_steps[i] == g->mutate_bars)
                k = i;
        g->mutate_bars = mutate_steps[clampi(k + (d > 0 ? 1 : -1), 0, 5)];
        break;
    }
    case 6: g->scale = (uint8_t)clampi(g->scale + (d > 0 ? 1 : -1), 0, TB3PO_NSCALES - 1); regen_line = 1; break;
    case 7: g->root = (uint8_t)((g->root + (d > 0 ? 1 : 11)) % 12); regen_line = 1; break;
    case 8: g->accent = (uint8_t)clampi(g->accent + accel(e, d, 100), 0, 100); regen_line = 1; break;
    case 9: g->slide = (uint8_t)clampi(g->slide + accel(e, d, 100), 0, 100); regen_line = 1; break;
    case 10: g->oct_range = (uint8_t)clampi(g->oct_range + (d > 0 ? 1 : -1), 1, 3); regen_line = 1; break;
    case 11: g_x.seq.xpose = (int8_t)clampi(g_x.seq.xpose + d, -X_XPOSE_MAX, X_XPOSE_MAX); break;
    case 12: g_x.seq.send_clock = d > 0; break;
    case 13: g_x.follow_clock = d > 0; break;
    case 14: copy_dest = clampi(copy_dest + (d > 0 ? 1 : -1), 0, X_NPAT - 1); break;
    default: return;
    }
    if (regen_line)
        tb3po_generate(pt);             /* the same seed: the line follows the settings */
    static const char *const names[16] = {"Tempo ", "Swing ", "Kit steps ", "303 steps ", "Density ",
                                          "Mutate bars ", "Scale ", "Root ", "Accent ", "Slide ",
                                          "Octaves ", "Transpose ", "Clock out ", "Clock in ", "Copy to P", ""};
    p = s;
    switch (e) {                        /* the value, in full */
    case 0: put_u(p, (uint32_t)(g_x.seq.bpm + 0.5f)); break;
    case 1: put_s(put_u(p, g_x.seq.swing), "%"); break;
    case 2: put_u(p, pt->drum_len); break;
    case 3: put_u(p, pt->bass_len); break;
    case 4: put_s(put_u(p, g->density), "%"); break;
    case 5: put_s(p, g->mutate_bars ? "" : "off"); if (g->mutate_bars) put_u(p, g->mutate_bars); break;
    case 6: put_s(p, TB3PO_SCALE_NAMES[g->scale]); break;
    case 7: put_s(p, note_names[g->root]); break;
    case 8: put_s(put_u(p, g->accent), "%"); break;
    case 9: put_s(put_u(p, g->slide), "%"); break;
    case 10: put_u(p, g->oct_range); break;
    case 11: put_i(p, g_x.seq.xpose); break;
    case 12: put_s(p, g_x.seq.send_clock ? "on" : "off"); break;
    case 13: put_s(p, g_x.follow_clock ? "on" : "off"); break;
    default: put_u(p, (uint32_t)copy_dest + 1u); break;
    }
    say(names[e], s);
    touch(e);
    changed();
}

static void global_push(int e)
{
    pattern_t *pt = cur_pat();
    char s[24];
    switch (e) {
    case 0: play_stop(); return;
    case 4:
        x0x_generate(&g_x, now_ms * 2654435761u ^ g_x.blocks);
        say("New 303 line", 0);
        break;
    case 5:
        x0x_mutate(&g_x);
        say("Mutated", 0);
        break;
    case 12:
        g_x.seq.send_clock = !g_x.seq.send_clock;
        say("Clock out ", g_x.seq.send_clock ? "on" : "off");
        break;
    case 13:
        g_x.follow_clock = !g_x.follow_clock;
        say("Clock in ", g_x.follow_clock ? "on" : "off");
        break;
    case 14:
        if (copy_dest == g_x.seq.ppat) {
            say("Copy: pick another", 0);
            return;
        }
        g_x.pat[copy_dest] = *pt;
        put_u(put_s(s, "Copied to P"), (uint32_t)copy_dest + 1u);
        say(s, 0);
        break;
    case 15:
        if (clear_armed_ms && now_ms - clear_armed_ms < 2000u) {
            pattern_clear(pt);
            clear_armed_ms = 0;
            say("Pattern cleared", 0);
        } else {
            clear_armed_ms = now_ms;
            say("Clear: push again", 0);
            touch(e);
            return;
        }
        break;
    default:
        return;
    }
    touch(e);
    changed();
}

static void pattern_push(int i)
{
    char s[24];
    if (i == g_x.seq.ppat && g_x.seq.pcue >= X_NPAT)
        return;
    seq_cue(&g_x.seq, i);
    put_u(put_s(s, g_x.seq.playing ? "Next: P" : "Pattern P"), (uint32_t)i + 1u);
    say(s, 0);
    changed();
}

static void mute_push(int e)
{
    if (e >= NTRACK)
        return;
    g_x.seq.mute ^= 1u << e;
    say(track_long[e], (g_x.seq.mute >> e & 1u) ? " muted" : " on");
    touch(e);
    changed();
}

static void mute_turn(int e, int d)
{
    char s[32];
    if (e >= NTRACK)
        return;
    int t = e == T303 ? X_T_MIX : X_T_DRUM0 + e, i = e == T303 ? X_MIX_BASS : D8P_LEVEL;
    x0x_set(&g_x, t, i, x0x_get(&g_x, t, i) + accel(e, d, 127));
    put_u(put_s(put_s(s, track_names[e]), " level "), (uint32_t)x0x_get(&g_x, t, i));
    say(s, 0);
    touch(e);
    changed();
}

static void nav(int e)
{
    if (e < NTRACK) {
        if (track == e && page == P_STEPS)
            page = P_SOUND;
        else if (track == e && page == P_SOUND)
            page = P_STEPS;
        else {
            track = e;
            page = P_STEPS;
        }
        say(track_long[track], page == P_SOUND ? " sound" : 0);
        return;
    }
    switch (e) {
    case 8: page = P_SOUND; say(track_long[track], " sound"); break;
    case 9: page = P_GLOBAL; say("Global", 0); break;
    case 10: page = P_PATTERN; say("Patterns", 0); break;
    case 11: page = P_MUTE; say("Mute / levels", 0); break;
    case 12:
        x0x_generate(&g_x, now_ms * 2654435761u ^ g_x.blocks);
        say("New 303 line", 0);
        changed();
        break;
    case 13:
        x0x_mutate(&g_x);
        say("Mutated 303 line", 0);
        changed();
        break;
    case 15: play_stop(); break;
    default: break;
    }
}

static int was_down[16];

static void controls(int shift)
{
    for (int e = 0; e < 16; e++) {
        int d = api->enc_read(e);
        int down = api->enc_pressed(e);
        int pushed = down && !was_down[e];
        was_down[e] = down;
        if (shift) {
            if (pushed)
                nav(e);
            if (d && page == P_STEPS)
                steps_turn(e, d, 1);    /* (Shift + turn elsewhere: nothing) */
            continue;
        }
        switch (page) {
        case P_STEPS:
            if (pushed)
                steps_push(e);
            if (d)
                steps_turn(e, d, 0);
            break;
        case P_SOUND:
            if (pushed)
                sound_push(e);
            if (d)
                sound_turn(e, d);
            break;
        case P_GLOBAL:
            if (pushed)
                global_push(e);
            if (d)
                global_turn(e, d);
            break;
        case P_PATTERN:
            if (pushed)
                pattern_push(e);
            break;
        default:
            if (pushed)
                mute_push(e);
            if (d)
                mute_turn(e, d);
            break;
        }
    }
}

/* ---- MIDI ---- */

static uint32_t midi_clocks_in;

static void midi_in(void)
{
    e16_midi_msg_t m;
    while (api->midi_read(&m)) {
        uint8_t st = m.data[0];
        if (m.len == 1) {
            if (!g_x.follow_clock)
                continue;
            switch (st) {
            case 0xF8: seq_ext_clock(&g_x.seq); midi_clocks_in++; break;
            case 0xFA: seq_start(&g_x.seq); break;
            case 0xFB: seq_continue(&g_x.seq); break;
            case 0xFC: seq_stop(&g_x.seq); break;
            default: break;
            }
        } else if (m.len == 3 && (st & 0xF0) == 0x90 && (m.data[2] & 0x7F)) {
            /* a note transposes the 303: C4 (60) = none (live: not saved by itself) */
            int x = clampi((int)(m.data[1] & 0x7F) - 60, -X_XPOSE_MAX, X_XPOSE_MAX);
            char s[24];
            g_x.seq.xpose = (int8_t)x;
            put_i(put_s(s, "Transpose "), x);
            say(s, 0);
        }
    }
}

static void midi_out(void)
{
    int b;
    while ((b = x0x_midi_out(&g_x)) >= 0) {
        uint8_t c = (uint8_t)b;
        api->midi_write(E16_MIDI_USB, &c, 1);
        api->midi_write(E16_MIDI_TRS_1, &c, 1);
    }
}

/* ---- the CPU guard (x0x.h): fewer drum voices at once while the load is high ---- */

static int guard_shown;
static uint32_t late_seen;

static void cpu_guard(const e16_audio_info_t *info)
{
    char s[64], *p;
    if (info->state == E16_AUDIO_OVERLOAD) {
        /* the launcher stopped the stream (over budget for 3 s): the strictest guard, and again,
         * a few times; then leave it off (the title says OVL) until play is pushed */
        if (overload_restarts >= OVERLOAD_RESTARTS) {
            if (overload_restarts++ == OVERLOAD_RESTARTS)
                api->log("x0x: still over budget after 3 restarts; audio off (Shift + push 16 tries again)");
            say("CPU overload: audio off", 0);
            return;
        }
        overload_restarts++;
        g_x.guard = X_GUARD_MAX;
        api->log("x0x: audio stopped by the launcher (overload); restarting with the CPU guard at 3");
        if (api->audio_start(render, &g_x) == 0)
            say("CPU overload: restarted", 0);
        late_seen = 0;
        return;
    }
    if (info->state != E16_AUDIO_RUNNING)
        return;
    /* (a late block with next to no load is the stream starting, not the engine) */
    int late = info->late != late_seen && info->load > 300u;
    late_seen = info->late;
    int g = x0x_guard_step(&g_x, info->load, late);
    if (g != guard_shown) {
        p = put_s(s, "x0x: CPU guard ");
        p = put_u(p, (uint32_t)g);
        p = put_s(p, " (load ");
        p = put_u(p, info->load / 10u);
        p = put_s(p, "%): at most ");
        p = put_u(p, g == 0 ? 7u : g == 1 ? 5u : g == 2 ? 4u : 3u);
        put_s(p, " drum voices");
        api->log(s);
        guard_shown = g;
    }
}

/* ---- main ---- */

static void fail_screen(const char *a, const char *b)
{
    gfx_clear();
    gfx_text(0, 16, a, 0);
    gfx_text(0, 28, b, 0);
    gfx_text(0, 48, "Hold Shift 5 s to leave", 0);
    api->display_present();
    for (;;)
        api->delay_ms(100);
}

static void log_status(void)
{
    char s[160], *p = put_s(s, "x0x: P");
    float pk = g_x.peak;
    g_x.peak = 0.0f;               /* (not atomic: x0x.h says why that's fine) */
    p = put_u(p, (uint32_t)g_x.seq.ppat + 1u);
    p = put_s(p, " ");
    p = put_u(p, (uint32_t)(seq_tempo(&g_x.seq) * 10.0f + 0.5f) / 10u);
    p = put_s(p, ".");
    p = put_u(p, (uint32_t)(seq_tempo(&g_x.seq) * 10.0f + 0.5f) % 10u);
    p = put_s(p, g_x.seq.ext ? " BPM (ext), " : " BPM, ");
    p = put_s(p, g_x.seq.playing ? "playing" : "stopped");
    p = put_s(p, ", peak ");
    p = put_u(p, (uint32_t)(pk * 1000.0f + 0.5f));
    p = put_s(p, "/1000, clocks in ");
    p = put_u(p, midi_clocks_in);
    p = put_s(p, ", cpu ");
    p = put_u(p, cpu_permille / 10u);
    p = put_s(p, ".");
    p = put_u(p, cpu_permille % 10u);
    p = put_s(p, "% (slowest ");
    p = put_u(p, cpu_peak / 10u);
    p = put_s(p, "%), guard ");
    put_u(p, g_x.guard);
    api->log(s);
}

int app_main(const e16_api_t *a)
{
    api = a;
    gfx_bind(api->display_fb());
    now_ms = api->millis();

    if (!E16_API_HAS(api, audio_start)) {
        api->log("x0x: this launcher has no app audio (audio_start); update it");
        fail_screen("x0x needs a newer", "launcher (app audio)");
    }

    x0x_init(&g_x);
    int loaded = state_load();
    api->log(loaded ? "x0x: state loaded" : "x0x: factory state");
    seq_start(&g_x.seq);              /* it plays on launch */
    if (api->audio_start(render, &g_x) != 0) {
        api->log("x0x: audio_start failed");
        fail_screen("x0x: no audio", "(audio_start failed)");
    }
    say(loaded ? "x0x - restored" : "x0x - factory loop", 0);

    uint32_t frame_ms = 0, led_ms = 0, info_ms = 0, log_ms = now_ms, mutated_seen = 0;
    for (;;) {
        now_ms = api->millis();
        int shift = api->shift_pressed();

        midi_in();
        controls(shift);
        if (g_x.seq.mutated != mutated_seen) {   /* an auto-mutation changed the line: save it */
            mutated_seen = g_x.seq.mutated;
            changed();
        }
        x0x_flush(&g_x);
        midi_out();
        if (audition_on && (int32_t)(now_ms - audition_off_ms) >= 0) {
            x0x_audition_bass(&g_x, -1, 0);
            audition_on = 0;
        }
        /* (+1: a Shift down this very millisecond still counts as held) */
        /* Leaving: the save core's "on the way out" attempt, then exit. */
        int bye = leaving();
        save_poll(bye ? X0X_SAVE_SHIFT_MS : 0u);
        if (bye)
            api->exit(0);

        if (now_ms - info_ms >= 500u) {
            info_ms = now_ms;
            e16_audio_info_t info;
            if (api->audio_info(&info, sizeof info) == 0) {
                cpu_permille = info.load;
                cpu_peak = info.load_peak;
                cpu_state = (int)info.state;
                cpu_guard(&info);
            }
        }
        if (now_ms - log_ms >= 10000u) {
            log_ms = now_ms;
            log_status();
        }
        if (now_ms - frame_ms >= FRAME_MS) {
            frame_ms = now_ms;
            draw();
        }
        if (now_ms - led_ms >= LED_MS) {
            led_ms = now_ms;
            leds_update();
        }
        api->delay_ms(1);
    }
}
