/*
 * Drone: an evolving drone synth on the E16's own audio (the api's audio
 * stream: USB audio while a computer is connected, else TRS 2). The DSP
 * is core/drone.c (host-tested by test/drone/drone_test.c); this is the
 * controls, the screen, the rings, MIDI and the saved state.
 *
 *   row 1 pitch    1 root (push: octave up)   2 chord (push: open voicing)
 *                  3 detune                    4 wave (sine - tri - saw)
 *   row 2 filter   5 cutoff   6 resonance   7 mode (LP - BP - HP)   8 drive
 *   row 3 motion   9 drift rate   10 drift depth   11 evolve (push: hold)
 *                 12 level (push: mute)
 *   row 4 space   13 space (push: freeze)   14 decay   15 sub   16 air
 *   Shift held 1 s: back to the launcher (the launcher's own watch)
 *
 * MIDI in (USB and TRS): a note-on sets the root (folded into C1-C6) and
 * swells brightness and level with its velocity; CCs set the macros
 * (drone_params[].cc: docs/drone.md).
 *
 * The settings are saved to /apps/drone/state.bin 2 s after the last
 * change, at most every 20 s (the SPI flash's erase budget), and when
 * Shift has been held 0.4 s (on the way out); they're loaded at start.
 */
#include "e16_app.h"
#include "drone.h"
#include "drone_save.h"
#include "font.h"

#define W E16_DISPLAY_WIDTH
#define H E16_DISPLAY_HEIGHT

#define FRAME_MS 33u
#define LED_MS 40u
#define TOUCH_SHOW_MS 1500u
#define STATE_MAGIC 0x314E5244u /* "DRN1" */

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
static drone_t g_drone;

/* The audio callback: runs in an interrupt (e16_api.h's audio_start()). */
static void render(float *out, uint32_t frames, void *user)
{
    drone_render((drone_t *)user, out, (int)frames);
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

/* v / 10^decimals, e.g. put_fixed(p, 125, 2) = "1.25" */
static char *put_fixed(char *p, uint32_t v, int decimals)
{
    uint32_t div = 1;
    for (int i = 0; i < decimals; i++)
        div *= 10;
    p = put_u(p, v / div);
    if (decimals) {
        *p++ = '.';
        uint32_t f = v % div;
        for (uint32_t d = div / 10; d; d /= 10) {
            *p++ = (char)('0' + f / d % 10);
        }
        *p = 0;
    }
    return p;
}

static const char *const note_names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

static char *put_note(char *p, int note)
{
    p = put_s(p, note_names[note % 12]);
    int oct = note / 12 - 1;
    if (oct < 0) {
        *p++ = '-';
        oct = -oct;
    }
    return put_u(p, (uint32_t)oct);
}

/* ---- drawing ---- */

static uint8_t *fb;

static void clear(void)
{
    for (int i = 0; i < E16_FB_SIZE; i++)
        fb[i] = 0;
}

static void pixel(int x, int y, int on)
{
    if (x < 0 || x >= W || y < 0 || y >= H)
        return;
    uint8_t m = (uint8_t)(1u << (y & 7));
    if (on)
        fb[(y >> 3) * W + x] |= m;
    else
        fb[(y >> 3) * W + x] &= (uint8_t)~m;
}

static void fill(int x0, int y0, int w, int h, int on)
{
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            pixel(x, y, on);
}

static void vline(int x, int y0, int y1)
{
    if (y0 > y1) {
        int t = y0;
        y0 = y1;
        y1 = t;
    }
    for (int y = y0; y <= y1; y++)
        pixel(x, y, 1);
}

static int text(int x, int y, const char *s, int on)
{
    for (; *s; s++) {
        const uint8_t *cols;
        int gw = font_glyph((uint8_t)*s, &cols);
        for (int c = 0; c < gw; c++)
            for (int r = 0; r < FONT_HEIGHT; r++)
                if (cols[c] >> r & 1)
                    pixel(x + c, y + r, on);
        if (gw > 0)
            x += gw + FONT_GAP;
    }
    return x;
}

static int text_w(const char *s)
{
    return font_text_width(s, 64);
}

/* ---- the macros ---- */

static int touched = -1;   /* param shown in the title, or -1 */
static uint32_t touch_ms;
static drone_save_t g_save; /* when to save: drone_save.h */
static uint32_t now_ms;

static void changed(int param)
{
    touched = param;
    touch_ms = now_ms;
    drone_save_changed(&g_save, now_ms);
}

/* The macro's value as text, into s. */
static void value_text(char *s, int p)
{
    int v = drone_get(&g_drone, p);
    char *q = s;
    switch (p) {
    case DP_ROOT:
        put_note(s, v);
        break;
    case DP_CHORD:
        q = put_s(s, drone_chord_names[v]);
        if (drone_get_flag(&g_drone, DF_OPEN))
            put_s(q, " open");
        break;
    case DP_DETUNE:
        q = put_u(s, (uint32_t)(drone_detune_cents(v) + 0.5f));
        put_s(q, " ct");
        break;
    case DP_MORPH: {
        static const char *const w[5] = {"sine", "sine-tri", "tri", "tri-saw", "saw"};
        put_s(s, w[(v * 4 + 63) / 127]);
        break;
    }
    case DP_CUTOFF: {
        float hz = drone_cutoff_hz(v);
        if (hz >= 1000.0f) {
            q = put_fixed(s, (uint32_t)(hz / 100.0f + 0.5f), 1);
            put_s(q, " kHz");
        } else {
            q = put_u(s, (uint32_t)(hz + 0.5f));
            put_s(q, " Hz");
        }
        break;
    }
    case DP_RESO:
        q = put_s(s, "Q ");
        put_fixed(q, (uint32_t)(5.0f * drone_exp2(5.06f * (float)v / 127.0f) + 0.5f), 1); /* Q x10 */
        break;
    case DP_MODE: {
        static const char *const m[5] = {"low-pass", "LP-BP", "band-pass", "BP-HP", "high-pass"};
        put_s(s, m[(v * 4 + 63) / 127]);
        break;
    }
    case DP_DRIFT_RATE:
        q = put_fixed(s, (uint32_t)(drone_drift_hz(v) * 100.0f + 0.5f), 2);
        put_s(q, " Hz");
        break;
    default:
        q = put_u(s, (uint32_t)((v * 100 + 63) / 127));
        put_s(q, "%");
        break;
    }
}

/* ---- the screen ---- */

#define SCOPE_W 100
static int scope_y[DRONE_SCOPE_N];
static int have_scope;
static uint32_t cpu_permille;
static int cpu_state;

static void scope_take(void)
{
    if (drone_scope_ready(&g_drone)) {
        const int8_t *s = drone_scope_data(&g_drone);
        for (int i = 0; i < DRONE_SCOPE_N; i++) {
            scope_y[i] = 31 - s[i] * 20 / 127;
        }
        have_scope = 1;
    }
    if (drone_scope_ready(&g_drone) || g_drone.scope_state == 0) {
        /* about three periods of the root across the screen */
        float hz = drone_note_hz(g_drone.live_root);
        int dec = (int)(3.0f * 48000.0f / hz / (float)DRONE_SCOPE_N + 0.5f);
        drone_scope_arm(&g_drone, dec);
    }
}

static void draw(void)
{
    clear();
    char s[40], v[24];

    /* title row: the touched macro, or the root and chord */
    if (touched >= 0 && now_ms - touch_ms < TOUCH_SHOW_MS) {
        text(0, 0, drone_params[touched].name, 1);
        value_text(v, touched);
        text(W - text_w(v), 0, v, 1);
    } else {
        touched = -1;
        char *p = put_note(s, drone_get(&g_drone, DP_ROOT));
        p = put_s(p, " ");
        put_s(p, drone_chord_names[drone_get(&g_drone, DP_CHORD)]);
        text(0, 0, s, 1);
        /* toggles, else the audio load */
        char *f = v;
        *f = 0;
        if (drone_get_flag(&g_drone, DF_MUTE))
            f = put_s(f, "MUTE ");
        if (drone_get_flag(&g_drone, DF_FREEZE))
            f = put_s(f, "FRZ ");
        if (drone_get_flag(&g_drone, DF_HOLD))
            f = put_s(f, "HOLD ");
        if (f == v) {
            if (cpu_state == E16_AUDIO_RUNNING) {
                f = put_s(v, "cpu ");
                f = put_u(f, (cpu_permille + 5) / 10);
                put_s(f, "%");
            } else {
                put_s(v, cpu_state == E16_AUDIO_OVERLOAD ? "OVERLOAD" : "no audio");
            }
        } else {
            f[-1] = 0; /* the last space */
        }
        text(W - text_w(v), 0, v, 1);
    }
    fill(0, 9, W, 1, 1);

    /* the scope (about three periods of the root, squeezed into
     * SCOPE_W columns), centre line dotted */
    for (int x = 0; x < SCOPE_W; x += 4)
        pixel(x, 31, 1);
    if (have_scope) {
        int prev = scope_y[0];
        for (int x = 1; x < SCOPE_W; x++) {
            int y = scope_y[x * DRONE_SCOPE_N / SCOPE_W];
            vline(x, prev, y);
            prev = y;
        }
    }
    /* the six voices, right of a divider: a dot each, its height the
     * voice's detune + drift (cents, 2 per pixel), bigger while it's
     * loud (the breathing) */
    for (int y = 12; y < 52; y += 2)
        pixel(SCOPE_W + 2, y, 1);
    for (int i = 0; i < DRONE_VOICES; i++) {
        float c = g_drone.live_voice_cents[i];
        int y = 31 - (int)(c * 0.5f);
        y = y < 12 ? 12 : y > 51 ? 51 : y;
        int x = SCOPE_W + 6 + i * 4;
        int r = g_drone.live_voice_level[i] > 0.7f ? 1 : 0;
        fill(x - r, y - r, 1 + 2 * r, 1 + 2 * r, 1);
    }

    /* bottom: the 16 macros as bars (the touched one boxed) */
    fill(0, 54, W, 1, 1);
    for (int p = 0; p < DP_COUNT; p++) {
        const drone_param_info_t *pi = &drone_params[p];
        int val = drone_get(&g_drone, p) - pi->lo, span = pi->hi - pi->lo;
        int h = span > 0 ? (val * 8 + span / 2) / span : 0;
        int x = p * 8;
        fill(x + 2, 64 - h, 4, h, 1);
        if (p == touched) {
            fill(x, 56, 1, 8, 1);
            fill(x + 7, 56, 1, 8, 1);
        }
    }
    api->display_present();
}

/* ---- LED rings ---- */

#define LED_MAX 26
static uint8_t led_now[16][16][3], led_sent[16][16][3];
static int leds_pending;

static const uint8_t row_rgb[4][3] = {{LED_MAX, 10, 0}, {0, LED_MAX, 12}, {16, 0, LED_MAX}, {0, 12, LED_MAX}};

static void ring_clear(int e)
{
    for (int i = 0; i < 16; i++)
        led_now[e][i][0] = led_now[e][i][1] = led_now[e][i][2] = 0;
}

static void ring_set(int e, int i, uint8_t r, uint8_t g, uint8_t b)
{
    if (i < 0 || i > 15)
        return;
    led_now[e][i][0] = r;
    led_now[e][i][1] = g;
    led_now[e][i][2] = b;
}

/* An arc from LED 0 to the value's, the row's colour, dimmed except the
 * tip. */
static void ring_arc(int e, int val, int lo, int hi)
{
    const uint8_t *c = row_rgb[e / 4];
    ring_clear(e);
    int n = hi > lo ? (val - lo) * 15 / (hi - lo) : 0;
    for (int i = 0; i <= n; i++) {
        int div = i == n ? 1 : 4;
        ring_set(e, i, (uint8_t)(c[0] / div), (uint8_t)(c[1] / div), (uint8_t)(c[2] / div));
    }
}

/* log2(x) for x > 0, to a few hundredths: plenty for a ring. */
static float log2_approx(float x)
{
    union {
        float f;
        uint32_t u;
    } v = {x};
    float e = (float)((int)(v.u >> 23 & 0xFFu) - 127);
    v.u = (v.u & 0x7FFFFFu) | 0x3F800000u; /* the mantissa, 1..2 */
    float m = v.f - 1.0f;
    return e + m * (1.3466f - 0.3466f * m);
}

/* A white dot where a wandering macro is right now. */
static void ring_live(int e, float frac)
{
    int i = (int)(frac * 15.0f + 0.5f);
    ring_set(e, i < 0 ? 0 : i > 15 ? 15 : i, 18, 18, 18);
}

static void leds_update(void)
{
    for (int e = 0; e < 16; e++) {
        const drone_param_info_t *pi = &drone_params[e];
        ring_arc(e, drone_get(&g_drone, e), pi->lo, pi->hi);
    }
    /* live positions */
    ring_live(DP_CUTOFF, (log2_approx(g_drone.live_cutoff_hz) - 4.907f) / 8.644f);
    ring_live(DP_MORPH, g_drone.live_morph * 0.5f);
    ring_live(DP_SPACE, g_drone.live_space);
    /* toggles */
    if (drone_get_flag(&g_drone, DF_HOLD))
        ring_set(DP_EVOLVE, 15, LED_MAX, 0, 0);
    if (drone_get_flag(&g_drone, DF_MUTE))
        for (int i = 0; i < 16; i += 2)
            ring_set(DP_LEVEL, i, LED_MAX, 0, 0);
    if (drone_get_flag(&g_drone, DF_FREEZE))
        for (int i = 0; i < 16; i++)
            ring_set(DP_SPACE, i, 0, LED_MAX, LED_MAX);
    if (drone_get_flag(&g_drone, DF_OPEN))
        ring_set(DP_CHORD, 15, LED_MAX, LED_MAX, 0);
    /* the output level on the level ring's far side, as it breathes */
    if (!drone_get_flag(&g_drone, DF_MUTE)) {
        int pk = (int)(g_drone.live_peak * 8.0f);
        for (int i = 0; i < pk && i < 8; i++)
            if (!led_now[DP_LEVEL][15 - i][0] && !led_now[DP_LEVEL][15 - i][2])
                ring_set(DP_LEVEL, 15 - i, 0, 6, 0);
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

typedef struct {
    uint32_t magic;
    uint8_t count;
    uint8_t flags; /* bit 0 hold, bit 1 open */
    uint8_t pad[2];
    int8_t p[DP_COUNT];
} state_t;

static int state_save(void)
{
    state_t st;
    memset(&st, 0, sizeof st);
    st.magic = STATE_MAGIC;
    st.count = DP_COUNT;
    st.flags = (uint8_t)(drone_get_flag(&g_drone, DF_HOLD) | drone_get_flag(&g_drone, DF_OPEN) << 1);
    for (int p = 0; p < DP_COUNT; p++)
        st.p[p] = (int8_t)drone_get(&g_drone, p);
    return api->file_write("state.bin", &st, sizeof st);
}

static int state_load(void)
{
    state_t st;
    int fd = api->file_open("/apps/drone/state.bin", E16_FILE_READ);
    if (fd < 0)
        return 0;
    int got = api->file_read(fd, &st, sizeof st);
    api->file_close(fd);
    if (got != (int)sizeof st || st.magic != STATE_MAGIC || st.count != DP_COUNT)
        return 0;
    for (int p = 0; p < DP_COUNT; p++)
        drone_set(&g_drone, p, st.p[p]);
    drone_set_flag(&g_drone, DF_HOLD, st.flags & 1);
    drone_set_flag(&g_drone, DF_OPEN, st.flags >> 1 & 1);
    return 1;
}

static void try_save(const char *why)
{
    int r = state_save();
    drone_save_result(&g_save, now_ms, r == 0);
    if (r == 0) {
        char s[48];
        put_s(put_s(put_s(s, "drone: saved ("), why), ")");
        api->log(s);
    } else {
        /* E16_EBUSY (the erase budget) or an error: later */
        char s[48], *p = put_s(s, "drone: save deferred (");
        if (r < 0) {
            *p++ = '-';
            r = -r;
        }
        p = put_u(p, (uint32_t)r);
        put_s(p, ")");
        api->log(s);
    }
}

static void save_poll(uint32_t shift_held_ms)
{
    switch (drone_save_poll(&g_save, now_ms, shift_held_ms)) {
    case DRONE_SAVE_LEAVING:
        try_save("leaving");
        break;
    case DRONE_SAVE_NOW:
        try_save("changed");
        break;
    default:
        break;
    }
}

/* ---- input ---- */

static int was_down[16];

static void turn(int e, int d)
{
    int p = e, v = drone_get(&g_drone, p);
    const drone_param_info_t *pi = &drone_params[p];
    if (pi->hi - pi->lo > 64 && (d > 1 || d < -1))
        d *= 3; /* fast turns go further on the long ranges */
    drone_set(&g_drone, p, v + d);
    changed(p);
}

static void push(int e)
{
    switch (e) {
    case DP_ROOT: {
        int r = drone_get(&g_drone, DP_ROOT) + 12;
        if (r > DRONE_ROOT_MAX)
            r = DRONE_ROOT_MIN + (r - DRONE_ROOT_MIN) % 12;
        drone_set(&g_drone, DP_ROOT, r);
        changed(DP_ROOT);
        break;
    }
    case DP_CHORD:
        drone_set_flag(&g_drone, DF_OPEN, !drone_get_flag(&g_drone, DF_OPEN));
        changed(DP_CHORD);
        break;
    case DP_EVOLVE:
        drone_set_flag(&g_drone, DF_HOLD, !drone_get_flag(&g_drone, DF_HOLD));
        api->log(drone_get_flag(&g_drone, DF_HOLD) ? "drone: evolve held" : "drone: evolve free");
        changed(DP_EVOLVE);
        break;
    case DP_LEVEL:
        drone_set_flag(&g_drone, DF_MUTE, !drone_get_flag(&g_drone, DF_MUTE));
        touched = DP_LEVEL;
        touch_ms = now_ms;
        break;
    case DP_SPACE:
        drone_set_flag(&g_drone, DF_FREEZE, !drone_get_flag(&g_drone, DF_FREEZE));
        api->log(drone_get_flag(&g_drone, DF_FREEZE) ? "drone: space frozen" : "drone: space free");
        touched = DP_SPACE;
        touch_ms = now_ms;
        break;
    default:
        break;
    }
}

static void controls(int shift)
{
    for (int e = 0; e < 16; e++) {
        int d = api->enc_read(e);
        int down = api->enc_pressed(e);
        if (shift) {
            was_down[e] = down; /* Shift (leaving) eats presses */
            continue;
        }
        if (down && !was_down[e])
            push(e);
        was_down[e] = down;
        if (d)
            turn(e, d);
    }
}

static void midi_in(void)
{
    e16_midi_msg_t m;
    while (api->midi_read(&m)) {
        if (m.len != 3)
            continue;
        uint8_t st = m.data[0] & 0xF0, d1 = m.data[1] & 0x7F, d2 = m.data[2] & 0x7F;
        if (st == 0x90 && d2 > 0) {
            drone_note(&g_drone, d1, d2);
            changed(DP_ROOT);
        } else if (st == 0xB0) {
            for (int p = 0; p < DP_COUNT; p++) {
                if (drone_params[p].cc && drone_params[p].cc == d1) {
                    const drone_param_info_t *pi = &drone_params[p];
                    drone_set(&g_drone, p, pi->lo + (d2 * (pi->hi - pi->lo) + 63) / 127);
                    changed(p);
                }
            }
        }
    }
}

/* ---- main ---- */

static void say(const char *a, const char *b)
{
    clear();
    text(0, 16, a, 1);
    text(0, 28, b, 1);
    text(0, 48, "Hold Shift to leave", 1);
    api->display_present();
}

int app_main(const e16_api_t *a)
{
    api = a;
    fb = api->display_fb();
    now_ms = api->millis();

    if (!E16_API_HAS(api, audio_start)) {
        api->log("drone: this launcher has no app audio (audio_start); update it");
        say("Drone needs a newer", "launcher (app audio).");
        for (;;)
            api->delay_ms(100);
    }

    drone_init(&g_drone, (float)E16_AUDIO_RATE, api->millis() * 2654435761u + 1u);
    int loaded = state_load();
    api->log(loaded ? "drone: settings loaded" : "drone: default settings");
    if (api->audio_start(render, &g_drone) != 0) {
        api->log("drone: audio_start failed");
        say("Drone: no audio", "(audio_start failed)");
        for (;;)
            api->delay_ms(100);
    }

    uint32_t frame_ms = 0, led_ms = 0, info_ms = 0, shift_t0 = 0;
    int shift_was = 0;
    for (;;) {
        now_ms = api->millis();
        int shift = api->shift_pressed();
        if (shift && !shift_was)
            shift_t0 = now_ms;
        shift_was = shift;

        controls(shift);
        midi_in();
        /* (+1: a Shift down this very millisecond still counts as held) */
        save_poll(shift ? now_ms - shift_t0 + 1u : 0u);

        if (now_ms - info_ms >= 500u) {
            info_ms = now_ms;
            e16_audio_info_t info;
            if (api->audio_info(&info, sizeof info) == 0) {
                cpu_permille = info.load;
                cpu_state = (int)info.state;
            }
        }
        if (now_ms - frame_ms >= FRAME_MS) {
            frame_ms = now_ms;
            scope_take();
            draw();
        }
        if (now_ms - led_ms >= LED_MS) {
            led_ms = now_ms;
            leds_update();
        }
        api->delay_ms(1);
    }
}
