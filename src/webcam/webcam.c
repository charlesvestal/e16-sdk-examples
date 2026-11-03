/*
 * Webcam mode: webcam.h.
 */
#include "webcam.h"

#include <stdio.h>
#include <string.h>

#include "board.h" /* DWT, SystemCoreClock */
#include "leds.h"
#include "system.h"
#include "usb_audio.h"
#include "usb_msc.h"
#include "usb_video.h"

#include "log.h"
#include "rt.h"
#include "rt_heap.h"
#include "rt_port.h"
#include "screen.h"
#include "wc_render.h"
#include "wc_ring.h"

#define FPS 20
#define LEDS_MS 40u     /* LED rings: the picture's colours, every this */
#define MIRROR_MS 80u   /* the OLED's mirror, every this */
#define FEEDBACK_MS 2000u /* a turned setting's name and value stay this long */
#define PALETTE_MS 15u  /* the palette is rebuilt at most this often */
#define STATS_MS 10000u
#define FPS_MS 2000u

/*
 * Rendering ahead. The USB interrupt needs a packet's worth of lines (2-3)
 * every millisecond and has to have them before the next one, so lines it
 * renders itself cost latency: a busy picture (two patterns, the
 * kaleidoscope, warp) takes ~170 us a line, and at that the stream drops
 * below 20 fps. So a lowest-priority interrupt (RENDER_IRQ, below every
 * driver: it only ever preempts the main loop) renders lines ahead into a
 * ring of AHEAD lines, and line_cb() just copies them out; it renders a
 * line itself only when the ring doesn't have it (the first line of a
 * stream, or when the renderer fell behind).
 *
 * Lines are numbered by frame ("generation", one per frame_cb()) and y;
 * the bookkeeping is wc_ring.h's (host-tested). The renderer works on the frame USB is sending or, near its end,
 * the next one; each frame has its own view (settings, palette, per-frame
 * tables), two in turn. A line it renders only counts (its slot's tag set)
 * if, after rendering, it is still the line due next and its view wasn't
 * replaced meanwhile -- USB may have moved on, or started a new frame,
 * while it rendered -- checked with USB's interrupts masked (BASEPRI), the
 * same as every other step of its bookkeeping. USB's interrupt can't be
 * preempted by the renderer, so it needs no masking. If USB overtakes the
 * renderer WC_RING_MAX_DISCARDS times in one frame, the renderer leaves
 * the rest of that frame to it rather than keep rendering lines that get
 * thrown away.
 */
#define AHEAD WC_RING_AHEAD /* lines (the bookkeeping is wc_ring.h's) */
_Static_assert(WC_H == WC_RING_H_LINES, "wc_ring.h's height");
#define RENDER_IRQ CAN2_SCE_IRQn /* unused here: a free vector for the renderer */
#define RENDER_IRQ_HANDLER CAN2_SCE_IRQHandler
#define USB_PRIORITY 6u    /* the SDK's USB interrupts (bsp/src/usb.c) */

/*
 * Everything bigger than a few words, from the Lua heap (webcam.h). The
 * main loop writes `pal` and `staged` (see "Handing over" below) and owns
 * `preview`; the video side (the USB interrupt and the renderer) owns the
 * rest, and copies the published palette and settings into each frame's
 * view (make_view()).
 */
typedef struct {
    wc_tables_t tab;
    wc_palette_t pal[2];        /* the palette, RGB for the LEDs too; pal[g_pal_pub] is current */
    wc_settings_t staged[2];    /* the settings; staged[g_set_pub] is current */
    wc_view_t preview;          /* main loop: for the OLED and the LEDs */
    char last_text[24];         /* main loop: the text overlay's text as last laid out */
    uint8_t view_pal[2][3][256]; /* each view's palette (Y, U, V) */
    wc_view_t view[2];          /* frame g's view is view[g & 1] */
    wc_line_t line_usb;         /* line_cb()'s working space */
    wc_line_t line_ahead;       /* the renderer's */
    uint8_t ring[AHEAD][WC_W * 2] __attribute__((aligned(4)));
} wc_mem_t;

static wc_mem_t *volatile g_mem;
static bool g_active;

/* The settings: kept between visits (until a reset). */
static int16_t g_params[WC_PARAMS];
static bool g_params_set;

/*
 * Handing over the main loop's palette and settings: each is double
 * buffered. The main loop writes the copy that isn't published, then
 * publishes it by switching the index. The video side only reads the
 * published copy, from interrupts, which the main loop can't interrupt:
 * so a view never takes half a change, whenever it's made.
 */
static volatile uint8_t g_pal_pub, g_set_pub;

/* The video side's bookkeeping (see "Rendering ahead"). */
static wc_ring_t g_ring;

/* Statistics: each counter has one writer; the main loop reads them. */
static volatile uint32_t g_usb_cycles, g_usb_lines; /* lines line_cb() rendered itself */
static volatile uint32_t g_ahead_cycles, g_ahead_lines;
static volatile uint32_t g_copied_lines;            /* lines line_cb() took from the ring */
static volatile uint32_t g_line_max;                /* slowest line, cycles */

/* Main loop state. */
static uint32_t g_last_ms, g_clock, g_cycle;
static bool g_palette_dirty;
static uint32_t g_palette_t;
static uint8_t g_flash;
static uint32_t g_flash_t;
static int g_note = -1;          /* the last MIDI note, for TEXT = NOTE */
static int g_feedback = -1;      /* the parameter last changed, shown on the OLED */
static uint32_t g_feedback_t;
static bool g_full_mirror;
static uint32_t g_leds_t, g_mirror_t;
static uint32_t g_fps_t, g_fps_frames, g_fps_x10;  /* the OLED's frame rate, over 2 s */
static uint32_t g_stats_t, g_stats_frames;          /* the log's, over 10 s */
static uint32_t g_stats_base[5];                    /* the counters above at g_stats_t */
static uint8_t g_usb_state;      /* what the OLED last showed */
static bool g_was_streaming;

/* ---- The video side (USB interrupt, renderer) ---- */

static void mask_usb(void)
{
    __set_BASEPRI(USB_PRIORITY << (8u - __NVIC_PRIO_BITS));
    __ISB();
}

static void unmask_usb(void)
{
    __set_BASEPRI(0);
}

/* Frame gen's view, from the published settings and palette. USB's
 * interrupt, or the renderer with it masked. */
static void make_view(wc_mem_t *m, uint32_t gen)
{
    int b = (int)(gen & 1u);
    const wc_palette_t *p = &m->pal[g_pal_pub];
    memcpy(m->view_pal[b][0], p->y, 256);
    memcpy(m->view_pal[b][1], p->u, 256);
    memcpy(m->view_pal[b][2], p->v, 256);
    wc_view_prepare(&m->view[b], &m->tab, &m->staged[g_set_pub], m->view_pal[b][0], m->view_pal[b][1],
                    m->view_pal[b][2]);
    wc_ring_view_made(&g_ring, gen, system_millis());
}

static void wake_renderer(void)
{
    NVIC_SetPendingIRQ(RENDER_IRQ);
}

static void frame_cb(uint32_t number)
{
    (void)number;
    wc_mem_t *m = g_mem;
    if (!m) {
        return;
    }
    bool need_view;
    uint32_t gen = wc_ring_frame(&g_ring, system_millis(), &need_view);
    if (need_view) {
        make_view(m, gen);
    }
    wake_renderer();
}

static void line_cb(uint16_t y, uint8_t *yuy2)
{
    wc_mem_t *m = g_mem;
    if (!m) {
        return;
    }
    int slot = wc_ring_take(&g_ring, y);
    if (slot >= 0) {
        memcpy(yuy2, m->ring[slot], WC_W * 2);
        g_copied_lines++;
    } else {
        uint32_t t0 = DWT->CYCCNT;
        wc_render_line(&m->view[g_ring.gen & 1u], &m->line_usb, y, yuy2);
        uint32_t took = DWT->CYCCNT - t0;
        g_usb_cycles += took;
        g_usb_lines++;
        if (took > g_line_max) {
            g_line_max = took;
        }
    }
    wc_ring_sent(&g_ring, y);
    wake_renderer();
}

/* The renderer: lowest priority, so it only ever preempts the main loop. */
void RENDER_IRQ_HANDLER(void)
{
    for (int n = 0; n < AHEAD; n++) {
        mask_usb();
        wc_mem_t *m = g_mem;
        uint32_t gen, y;
        int slot;
        bool need_view;
        if (!m || !wc_ring_claim(&g_ring, &gen, &y, &slot, &need_view)) {
            unmask_usb();
            return;
        }
        if (need_view) {
            make_view(m, gen);
        }
        unmask_usb();

        uint32_t t0 = DWT->CYCCNT;
        wc_render_line(&m->view[gen & 1u], &m->line_ahead, (int)y, m->ring[slot]);
        uint32_t took = DWT->CYCCNT - t0;

        mask_usb();
        wc_ring_commit(&g_ring, gen, y);
        unmask_usb();
        g_ahead_cycles += took;
        g_ahead_lines++;
        if (took > g_line_max) {
            g_line_max = took; /* may lose to line_cb()'s: it's only for the log */
        }
    }
}

static const usb_video_ops_t g_video = {
    .width = WC_W,
    .height = WC_H,
    .fps = FPS,
    .frame = frame_cb,
    .line = line_cb,
};

/* ---- Settings ---- */

static void defaults(void)
{
    for (int i = 0; i < WC_PARAMS; i++) {
        g_params[i] = wc_param_info[i].def;
    }
}

static const char *overlay_text(char *buf, size_t size)
{
    char note[5];
    switch (g_params[WC_TEXT]) {
        case WC_TEXT_NAME: {
            char tmp[8];
            if (g_params[WC_MIX] > 0) {
                snprintf(buf, size, "%s+%s", wc_value_text(WC_PATTERN, g_params[WC_PATTERN], tmp),
                         wc_value_text(WC_PATTERN_B, g_params[WC_PATTERN_B], tmp));
            } else {
                snprintf(buf, size, "%s", wc_value_text(WC_PATTERN, g_params[WC_PATTERN], tmp));
            }
            return buf;
        }
        case WC_TEXT_NOTE:
            snprintf(buf, size, "%s", g_note >= 0 ? wc_note_name(g_note, note) : "");
            return buf;
        case WC_TEXT_E16:
            return "OXI E16";
        default:
            return "";
    }
}

/* The main loop's settings (and, if asked, a new palette) published (see "Handing over"). */
static void publish(wc_mem_t *m, bool palette)
{
    if (palette) {
        uint8_t w = g_pal_pub ^ 1u;
        wc_palette_build(&m->pal[w], &m->tab, g_params, g_flash);
        __DMB();
        g_pal_pub = w;
    }
    char text[24];
    const char *s = overlay_text(text, sizeof text);
    uint8_t w = g_set_pub ^ 1u;
    wc_settings_t *st = &m->staged[w];
    memcpy(st->params, g_params, sizeof g_params);
    st->clock = g_clock;
    st->cycle = g_cycle;
    if (strcmp(s, m->last_text) != 0) {
        snprintf(m->last_text, sizeof m->last_text, "%s", s);
        wc_text_set(&st->text, s);
    } else {
        st->text = m->staged[g_set_pub].text; /* unchanged: no need to lay it out again */
    }
    __DMB();
    g_set_pub = w;
}

static void changed(int param)
{
    g_feedback = param;
    g_feedback_t = system_millis();
    if (param == WC_HUE || param == WC_PALETTE || param == WC_POSTERIZE || param == WC_CONTRAST ||
        param == WC_BRIGHT) {
        g_palette_dirty = true;
    }
    screen_touch();
}

static void set_param(int param, int value)
{
    value = wc_param_limit(param, value);
    if (g_params[param] != value) {
        g_params[param] = (int16_t)value;
        changed(param);
    }
}

void webcam_turn(int enc, int steps)
{
    if (!g_active || enc < 0 || enc >= WC_PARAMS) {
        return;
    }
    set_param(enc, g_params[enc] + steps);
}

void webcam_press(int enc)
{
    if (!g_active || enc < 0 || enc >= WC_PARAMS) {
        return;
    }
    if (enc == 0) {
        g_full_mirror = !g_full_mirror;
        log_printf("webcam: OLED %s", g_full_mirror ? "full-screen mirror" : "status + mirror");
        screen_touch();
        return;
    }
    set_param(enc, wc_param_info[enc].def);
    changed(enc); /* show it even if it was at the default already */
}

void webcam_midi(const uint8_t *data, size_t len)
{
    if (!g_active || len < 2) {
        return;
    }
    uint8_t status = data[0] & 0xF0;
    if (status == 0x90 && len >= 3 && data[2] > 0) {
        g_note = data[1];
        set_param(WC_HUE, (data[1] % 12) * 64 / 12);
        g_flash = (uint8_t)(data[2] * 2);
        g_flash_t = system_millis();
        g_palette_dirty = true;
    } else if (status == 0xB0 && len >= 3) {
        uint8_t cc = data[1];
        if (cc >= 20 && cc < 20 + WC_PARAMS) {
            set_param(cc - 20, wc_param_from_cc(cc - 20, data[2]));
        } else if (cc == 1) {
            set_param(WC_MIX, wc_param_from_cc(WC_MIX, data[2]));
        }
    } else if (status == 0xC0) {
        set_param(WC_PATTERN, data[1] % WC_PATTERNS);
    }
}

/* ---- OLED ---- */

/* Bayer-dithered luma of the picture, w x h pixels at (x0, y0) of the screen. */
static void draw_mirror(int x0, int y0, int w, int h)
{
    static const uint8_t bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    wc_mem_t *m = g_mem;
    uint8_t *fb = screen_draw_buffer();
    for (int sy = 0; sy < h; sy++) {
        int py = y0 + sy;
        int y = sy * WC_H / h;
        for (int sx = 0; sx < w; sx++) {
            int px = x0 + sx;
            uint8_t shade;
            uint8_t i = wc_sample(&m->preview, sx * WC_W / w, y, &shade);
            int luma = (16 + ((m->pal[g_pal_pub].y[i] - 16) * shade >> 8) - 16) * 255 / 219;
            if (luma > bayer[py & 3][px & 3] * 16 + 8) {
                fb[(py >> 3) * 128 + px] |= (uint8_t)(1u << (py & 7));
            }
        }
    }
}

static void draw_screen(void)
{
    wc_mem_t *m = g_mem;
    if (!m) {
        return;
    }
    if (g_full_mirror) {
        draw_mirror(0, 0, 128, 64);
        return;
    }
    char line[24], tmp[8];
    bool streaming = usb_video_streaming();
    screen_draw_title("WEBCAM");
    if (usb_video_switching()) {
        screen_draw_text(0, 12, "USB...", false);
    } else if (streaming) {
        snprintf(line, sizeof line, "LIVE %lu.%lu", (unsigned long)(g_fps_x10 / 10), (unsigned long)(g_fps_x10 % 10));
        screen_draw_text(0, 12, line, false);
    } else {
        screen_draw_text(0, 12, "NO APP", false);
    }
    screen_draw_text(0, 24, wc_value_text(WC_PATTERN, g_params[WC_PATTERN], tmp), false);
    if (g_params[WC_MIX] > 0) {
        snprintf(line, sizeof line, "+%s", wc_value_text(WC_PATTERN_B, g_params[WC_PATTERN_B], tmp));
        screen_draw_text(0, 34, line, false);
    }
    if (g_feedback >= 0) {
        snprintf(line, sizeof line, "%s", wc_param_info[g_feedback].name);
        screen_draw_text(0, 46, line, false);
        screen_draw_text(0, 55, wc_value_text(g_feedback, g_params[g_feedback], tmp), false);
    }
    draw_mirror(64, 12, 64, 48);
}

/* ---- LEDs ---- */

static void draw_leds(void)
{
    wc_mem_t *m = g_mem;
    for (int ring = 0; ring < LEDS_RINGS; ring++) {
        int x = (ring % 4) * (WC_W / 4) + WC_W / 8;
        int y = (ring / 4) * (WC_H / 4) + WC_H / 8;
        uint8_t shade;
        uint8_t i = wc_sample(&m->preview, x, y, &shade);
        const wc_palette_t *pal = &m->pal[g_pal_pub];
        int r = pal->r[i] * shade >> 9, g = pal->g[i] * shade >> 9, b = pal->b[i] * shade >> 9;
        const wc_param_info_t *p = &wc_param_info[ring];
        int dot = (g_params[ring] - p->min) * (LEDS_PER_RING - 1) / (p->max - p->min);
        for (int led = 0; led < LEDS_PER_RING; led++) {
            if (led == dot) {
                rt_port_led(ring + 1, led + 1, 255, 255, 255);
            } else {
                rt_port_led(ring + 1, led + 1, r, g, b);
            }
        }
    }
    rt_port_ring_show();
}

/* ---- Enter, leave, poll ---- */

/* The video side's counters, read once each (a counter may move between reads). */
static void counters(uint32_t c[5])
{
    c[0] = g_usb_cycles;
    c[1] = g_usb_lines;
    c[2] = g_ahead_cycles;
    c[3] = g_ahead_lines;
    c[4] = g_copied_lines;
}

static void stats_reset(uint32_t now)
{
    g_stats_t = now;
    g_stats_frames = usb_video_frames();
    counters(g_stats_base);
    g_line_max = 0;
}

/* Frames per second, the CPU the picture took (rendering in the USB
 * interrupt and ahead), and how many lines the ring had ready. */
static void log_stats(uint32_t now)
{
    uint32_t ms = now - g_stats_t;
    uint32_t frames = usb_video_frames() - g_stats_frames;
    if (frames == 0 || ms == 0) {
        return;
    }
    uint32_t c[5];
    counters(c);
    for (int i = 0; i < 5; i++) {
        c[i] -= g_stats_base[i];
    }
    uint32_t mhz = SystemCoreClock / 1000000u;
    uint64_t cycles = (uint64_t)c[0] + c[2];
    /* tenths of a percent of the CPU */
    uint32_t load = (uint32_t)(cycles * 1000u / ((uint64_t)SystemCoreClock * ms / 1000u));
    uint32_t fps_x10 = frames * 10000u / ms;
    uint32_t lines = c[1] + c[3];
    log_printf("webcam: %lu ms: %lu frames (%lu.%lu fps), %lu.%lu%% CPU, %lu us/line (slowest %lu), "
               "lines sent from the ring %lu, rendered in the USB interrupt %lu",
               (unsigned long)ms, (unsigned long)frames, (unsigned long)(fps_x10 / 10u),
               (unsigned long)(fps_x10 % 10u), (unsigned long)(load / 10u), (unsigned long)(load % 10u),
               (unsigned long)(lines ? cycles / lines / mhz : 0), (unsigned long)(g_line_max / mhz),
               (unsigned long)c[4], (unsigned long)c[1]);
}

bool webcam_active(void)
{
    return g_active;
}

bool webcam_enter(void)
{
    if (g_active) {
        return true;
    }
    /* The Lua heap is only free with no script loaded (the launcher only
     * enters from the menu); never borrow it from under a Lua state. */
    if (rt_state()) {
        log_printf("webcam: a script is loaded; not entering");
        return false;
    }
    wc_mem_t *m = rt_heap_lua_alloc(NULL, NULL, 0, sizeof(wc_mem_t));
    if (!m) {
        log_printf("webcam: no memory (%u bytes wanted, heap used %u / total %u)", (unsigned)sizeof(wc_mem_t),
                   (unsigned)rt_heap_used(), (unsigned)rt_heap_total());
        return false;
    }
    memset(m, 0, sizeof *m);
    uint32_t t0 = DWT->CYCCNT;
    m->last_text[0] = '\x01'; /* matches no text: the first publish lays it out */
    wc_tables_init(&m->tab);
    if (!g_params_set) {
        defaults();
        g_params_set = true;
    }
    g_last_ms = system_millis();
    g_flash = 0;
    g_feedback = -1;
    g_full_mirror = false;
    g_usb_state = 0xFF;
    g_was_streaming = false;
    g_fps_frames = usb_video_frames();
    g_stats_t = g_fps_t = g_last_ms;
    g_fps_x10 = 0;
    stats_reset(g_last_ms);
    publish(m, true);
    wc_ring_reset(&g_ring);
    NVIC_SetPriority(RENDER_IRQ, (1u << __NVIC_PRIO_BITS) - 1u);
    NVIC_ClearPendingIRQ(RENDER_IRQ);
    g_mem = m;
    NVIC_EnableIRQ(RENDER_IRQ);
    g_active = true;
    uint32_t init_us = (DWT->CYCCNT - t0) / (SystemCoreClock / 1000000u);
    usb_video_select(&g_video);
    screen_show_custom(draw_screen);
    log_printf("webcam: on, %dx%d at %d fps, %u bytes at %p from the Lua heap (tables %lu us); USB -> MIDI + video",
               WC_W, WC_H, FPS, (unsigned)sizeof(wc_mem_t), (void *)m, (unsigned long)init_us);
    return true;
}

void webcam_leave(const char *why)
{
    if (!g_active) {
        return;
    }
    /* From here on the SDK never calls frame_cb()/line_cb() again
     * (usb_video.h), so the memory can go straight back. */
    usb_video_select(NULL);
    /* The renderer only preempts the main loop, so it isn't half-way
     * through a line now; once it's off it can't start one. */
    NVIC_DisableIRQ(RENDER_IRQ);
    NVIC_ClearPendingIRQ(RENDER_IRQ);
    wc_mem_t *m = g_mem;
    g_mem = NULL;
    g_active = false;
    rt_heap_lua_alloc(NULL, m, sizeof(wc_mem_t), 0);
    log_printf("webcam: off (%s), %lu frames sent in all; USB -> MIDI + disk + audio", why,
               (unsigned long)usb_video_frames());
}

void webcam_poll(uint32_t now)
{
    wc_mem_t *m = g_mem;
    if (!g_active || !m) {
        return;
    }
    uint32_t dt = now - g_last_ms;
    g_last_ms = now;
    g_clock += (uint32_t)g_params[WC_SPEED] * dt;
    g_cycle += (uint32_t)((int32_t)g_params[WC_CYCLE] * (int32_t)dt);

    /* A note's flash fades over ~300 ms. */
    if (g_flash && now - g_flash_t >= 10u) {
        g_flash_t = now;
        g_flash = (uint8_t)(g_flash > 12 ? g_flash - g_flash / 6 - 1 : 0);
        g_palette_dirty = true;
    }
    bool palette = g_palette_dirty && now - g_palette_t >= PALETTE_MS;
    if (palette) {
        g_palette_dirty = false;
        g_palette_t = now;
    }
    publish(m, palette);

    /* Frames per second (the OLED, every 2 s) and the statistics (the log, every 10 s). */
    uint32_t frames = usb_video_frames();
    if (now - g_fps_t >= FPS_MS) {
        g_fps_x10 = (frames - g_fps_frames) * 10000u / (now - g_fps_t);
        g_fps_frames = frames;
        g_fps_t = now;
        screen_touch();
    }
    bool streaming = usb_video_streaming();
    if (streaming != g_was_streaming) {
        g_was_streaming = streaming;
        log_printf("webcam: the computer %s capturing", streaming ? "started" : "stopped");
        stats_reset(now);
    }
    if (now - g_stats_t >= STATS_MS) {
        if (streaming) {
            log_stats(now);
        }
        stats_reset(now);
    }

    uint8_t usb = (uint8_t)(usb_video_switching() | streaming << 1 | usb_video_active() << 2);
    if (usb != g_usb_state) {
        if ((usb & 4) && !(g_usb_state & 4) && g_usb_state != 0xFF) {
            log_printf("webcam: USB is MIDI + video now");
        }
        g_usb_state = usb;
        screen_touch();
    }
    if (g_feedback >= 0 && now - g_feedback_t >= FEEDBACK_MS) {
        g_feedback = -1;
        screen_touch();
    }

    if (now - g_leds_t >= LEDS_MS || now - g_mirror_t >= MIRROR_MS) {
        const wc_palette_t *p = &m->pal[g_pal_pub];
        wc_view_prepare(&m->preview, &m->tab, &m->staged[g_set_pub], p->y, p->u, p->v);
    }
    if (now - g_leds_t >= LEDS_MS) {
        g_leds_t = now;
        draw_leds();
    }
    if (now - g_mirror_t >= MIRROR_MS) {
        g_mirror_t = now;
        screen_touch();
    }
}
