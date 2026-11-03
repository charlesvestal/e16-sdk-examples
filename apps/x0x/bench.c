/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * x0xbench: x0x-lite's engines timed on the E16 itself (DWT cycle counter), from RAM as the app
 * runs them, with no audio stream: each part alone, then the whole engine on the factory loop
 * and on the worst case (every drum and the 303 on every step). Logs cycles per sample (of the
 * 3500 a 48 kHz sample has at 168 MHz) and exits. A development aid (docs/x0x.md, "CPU").
 */
#include "e16_app.h"
#include "x0x.h"

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

#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004u)
#define N_BLOCKS 1500 /* 1 s */

static const e16_api_t *api;
static x0x_t g_x;
static float buf[32];

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

static void report(const char *what, uint32_t cycles, uint32_t worst_block)
{
    char s[96], *p = put_s(s, "x0xbench: ");
    p = put_s(p, what);
    p = put_s(p, " ");
    p = put_u(p, cycles / (N_BLOCKS * 32u));
    p = put_s(p, " cycles/sample avg, slowest block ");
    p = put_u(p, worst_block / 32u);
    put_s(p, "/sample");
    api->log(s);
}

static uint32_t worst;

static uint32_t time_block(void (*fn)(int), int k)
{
    uint32_t t0 = DWT_CYCCNT;
    fn(k);
    uint32_t dt = DWT_CYCCNT - t0;
    if (dt > worst)
        worst = dt;
    return dt;
}

static int g_drv, g_part;

static void b_bass(int k)
{
    if (k % 90 == 0)
        bass303_note_on(&g_x.bass, 36 + k % 24, (k / 90) & 1, 0);
    bass303_render(&g_x.bass, buf, 32);
}

static void b_drum(int k)
{
    if (k % 90 == 0)
        drum808_trigger(&g_x.kit, g_part, 1.0f);
    for (int i = 0; i < 32; i++)
        buf[i] = 0.0f;
    drum808_render(&g_x.kit, buf, 32);
}

static void b_engine(int k)
{
    (void)k;
    x0x_flush(&g_x);
    x0x_render(&g_x, buf, 32);
}

static void run(const char *what, void (*fn)(int))
{
    uint32_t total = 0;
    worst = 0;
    for (int k = 0; k < N_BLOCKS; k++)
        total += time_block(fn, k);
    report(what, total, worst);
}

/* sanity: an independent float multiply-add chain, 4 per iteration, and an integer loop */
static volatile float g_sink;
static void micro(void)
{
    float a0 = 1.0f, a1 = 2.0f, a2 = 3.0f, a3 = 4.0f;
    uint32_t t0 = DWT_CYCCNT;
    for (int i = 0; i < 100000; i++) {
        a0 = a0 * 0.999f + 0.001f;
        a1 = a1 * 0.999f + 0.001f;
        a2 = a2 * 0.999f + 0.001f;
        a3 = a3 * 0.999f + 0.001f;
    }
    uint32_t dt = DWT_CYCCNT - t0;
    g_sink = a0 + a1 + a2 + a3;
    char s[80], *p = put_s(s, "x0xbench: 4 fma x 100000: ");
    p = put_u(p, dt / 1000u);
    put_s(p, " cycles per 100 iterations");
    api->log(s);
    volatile uint32_t v = 0;
    t0 = DWT_CYCCNT;
    for (uint32_t i = 0; i < 100000u; i++)
        v = v + i;
    dt = DWT_CYCCNT - t0;
    p = put_s(s, "x0xbench: volatile int loop: ");
    p = put_u(p, dt / 1000u);
    put_s(p, " cycles per 100 iterations");
    api->log(s);
}

/* the attack: the trigger itself, then the first blocks after it, in cycles per sample */
static void attack(const char *what, int mask)
{
    char s[120], *p;
    x0x_init(&g_x);
    uint32_t t0 = DWT_CYCCNT;
    for (int t = 0; t < D8_NUM; t++)
        if (mask >> t & 1)
            drum808_trigger(&g_x.kit, t, 1.0f);
    uint32_t trig = DWT_CYCCNT - t0;
    p = put_s(s, "x0xbench: attack ");
    p = put_s(p, what);
    p = put_s(p, ": trigger ");
    p = put_u(p, trig);
    p = put_s(p, " cycles; blocks");
    for (int k = 0; k < 10; k++) {
        for (int i = 0; i < 32; i++)
            buf[i] = 0.0f;
        t0 = DWT_CYCCNT;
        drum808_render(&g_x.kit, buf, 32);
        uint32_t dt = DWT_CYCCNT - t0;
        p = put_s(p, " ");
        p = put_u(p, dt / 32u);
    }
    api->log(s);
}

static void stream_render(float *out, uint32_t frames, void *user)
{
    x0x_render((x0x_t *)user, out, (int)frames);
}

static void live_phase(const char *what, int busy)
{
    uint8_t *fb = api->display_fb();
    uint32_t t0 = api->millis(), led = t0, scr = t0, k = 0;
    while (api->millis() - t0 < 4000u) {
        uint32_t now = api->millis();
        if ((busy & 1) && now - led >= 30u) {
            led = now;
            k++;
            for (int r = 0; r < 16; r++)
                for (int i = 0; i < 16; i++)
                    api->leds_set(r, i, (uint8_t)((i + k) & 15), 4, (uint8_t)(r & 7));
            api->leds_show();
        }
        if ((busy & 2) && now - scr >= 40u) {
            scr = now;
            for (int i = 0; i < 1024; i++)
                fb[i] = (uint8_t)(i + k);
            api->display_present();
        }
        api->delay_ms(1);
    }
    e16_audio_info_t info;
    api->audio_info(&info, sizeof info);
    char s[96], *p = put_s(s, "x0xbench: ");
    p = put_s(p, what);
    p = put_s(p, ": load ");
    p = put_u(p, info.load);
    p = put_s(p, "/1000, slowest ");
    p = put_u(p, info.load_peak);
    p = put_s(p, "/1000, late ");
    put_u(p, info.late);
    api->log(s);
}

int app_main(const e16_api_t *a)
{
    static const char *const drv_names[3] = {"303 drive off", "303 drive soft", "303 drive RAT"};
    static const char *const drum_names[D8_NUM] = {"BD", "SD", "LT", "CP", "CH", "OH", "CB"};
    api = a;
    api->log("x0xbench: start (cycles of 3500 per sample)");
    micro();
    {
        static const char *const nm[D8_NUM] = {"BD", "SD", "LT", "CP", "CH", "OH", "CB"};
        for (int t = 0; t < D8_NUM; t++)
            attack(nm[t], 1 << t);
        attack("all", 0x7F);
    }
    for (g_drv = 0; g_drv < 3; g_drv++) {
        x0x_init(&g_x);
        bass303_set(&g_x.bass, BASS303_DRIVE, g_drv ? 45 : 0);
        bass303_set(&g_x.bass, BASS303_DRVTYPE, g_drv);
        bass303_set(&g_x.bass, BASS303_DECAY, 127);
        run(drv_names[g_drv], b_bass);
    }
    for (g_part = 0; g_part < D8_NUM; g_part++) {
        x0x_init(&g_x);
        run(drum_names[g_part], b_drum);
    }
    x0x_init(&g_x);
    seq_start(&g_x.seq);
    run("engine, factory loop", b_engine);
    x0x_init(&g_x);
    for (int t = 0; t < X_NDRUM; t++)
        for (int s = 0; s < X_NSTEPS; s++)
            g_x.pat[0].drum[t][s] = X_LVL_MAX;
    for (int s = 0; s < X_NSTEPS; s++)
        g_x.pat[0].bass[s].flags = G_NOTE | BS_ACCENT;
    x0x_set(&g_x, X_T_BASS, BASS303_DECAY, 127);
    g_x.seq.bpm = 180.0f;
    seq_start(&g_x.seq);
    run("engine, worst case", b_engine);
    for (int g = 1; g <= X_GUARD_MAX; g++) {
        static const char *const gn[4] = {"", "worst case, guard 1", "worst case, guard 2", "worst case, guard 3"};
        g_x.guard = (uint8_t)g;
        run(gn[g], b_engine);
    }
    /* the factory loop live, on the audio stream: the main flow idle, then busy with the LEDs and
     * the screen the way the app keeps them (how much of the live load is the main flow's) */
    x0x_init(&g_x);
    seq_start(&g_x.seq);
    api->audio_start(stream_render, &g_x);
    live_phase("live, main flow idle", 0);
    live_phase("live, LEDs every 30 ms", 1);
    live_phase("live, LEDs + screen", 3);
    api->audio_stop();
    api->log("x0xbench: done");
    api->exit(0);
}
