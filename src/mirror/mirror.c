/*
 * The mirror's device glue: mirror.h (the protocol is in mirror_core.h).
 */
#include "mirror.h"

#if E16_MIRROR

#include <string.h>

#include "board.h" /* DWT, SystemCoreClock, the LED DMA streams */
#include "enc.h"
#include "system.h"

#include "launcher.h"
#include "log.h"
#include "mirror_core.h"
#include "rt_api_launcher.h" /* RT_LAUNCHER_VERSION */
#include "screen.h"

#ifndef E16_DEV_CONSOLE
#define E16_DEV_CONSOLE 1
#endif

#define STATS_MS 10000u

static mc_t g_mc;
static uint32_t g_hw_down;          /* hardware buttons as last seen, bit 16 = Shift */
static const uint8_t *g_app_oled;   /* app mode */
static const char *g_app_name;
static uint32_t g_stats_t0;
/* Cycles, the longest mirror_poll() since the last stats line: wall
 * time, so interrupts that ran meanwhile count too (with them masked
 * the longest was 118 us, docs/hw-acceptance.md). */
static uint32_t g_slowest;

/*
 * The LED levels as last sent to the LEDs. The SDK's leds.c keeps no
 * copy it hands out, but the frame it last sent stays in its DMA buffer
 * (the "wire" bytes, mirror_core.h's mc_decode_wire_led()) until the
 * next leds_show(), and each chain's DMA stream still points at it:
 * leds_show() starts the transfer with the buffer's address in M0AR,
 * which nothing else writes. Read only from the main loop (normal mode)
 * or app mode's service, which never runs inside an api call (so never
 * during the app's leds_show()); a transfer in progress only reads the
 * buffer. Before the first leds_show() M0AR is 0: all off.
 */
#define CHAIN_LEDS 128u
#define SRAM_START 0x20000000u
#define SRAM_BYTES (128u * 1024u)

static const uint8_t *chain_wire(const DMA_Stream_TypeDef *s)
{
    uint32_t a = s->M0AR;
    if (a < SRAM_START || a > SRAM_START + SRAM_BYTES - CHAIN_LEDS * MC_WIRE_BYTES_PER_LED) {
        return NULL;
    }
    return (const uint8_t *)(uintptr_t)a;
}

static void ring_levels(void *ud, int ring, uint8_t out[MC_RING_BYTES])
{
    (void)ud;
    const uint8_t *w = chain_wire(ring < 8 ? LEDS_A_DMA_STREAM : LEDS_B_DMA_STREAM);
    if (!w) {
        memset(out, 0, MC_RING_BYTES);
        return;
    }
    for (int led = 0; led < MC_LEDS; led++) {
        mc_decode_wire_led(w, (ring % 8) * MC_LEDS + led, out + led * 3);
    }
}

static bool usb_write(void *ud, const uint8_t *msg, size_t len)
{
    (void)ud;
    return midi_write(MIDI_USB, msg, len);
}

static uint32_t hw_buttons(void)
{
    uint32_t m = enc_shift_pressed() ? 1u << 16 : 0u;
    for (int i = 0; i < ENC_COUNT; i++) {
        if (enc_pressed(i)) {
            m |= 1u << i;
        }
    }
    return m;
}

bool mirror_handle(const midi_msg_t *msg)
{
    if (msg->port != MIDI_USB) {
        return false;
    }
    mc_result_t r = mc_handle(&g_mc, msg->data, msg->len, system_millis());
    if (r == MC_STARTED) {
        g_hw_down = hw_buttons();
        g_stats_t0 = system_millis();
        g_slowest = 0;
        log_printf("mirror: on");
    } else if (r == MC_STOPPED) {
        log_printf("mirror: off");
    }
    return r != MC_NOT_MINE;
}

void mirror_turn(int index, int steps, bool remote)
{
    if (steps != 0) {
        mc_event(&g_mc, MC_EV_TURN, index, steps, remote);
    }
}

void mirror_button(int index, bool down, bool remote)
{
    mc_event(&g_mc, MC_EV_BUTTON, index, down, remote);
}

void mirror_app_mode(const uint8_t *oled, const char *name)
{
    g_app_oled = oled;
    g_app_name = name;
}

static void log_stats(uint32_t now)
{
    uint32_t ms = now - g_stats_t0;
    mc_stats_t *s = &g_mc.stats;
    uint32_t mhz = SystemCoreClock / 1000000u;
    log_printf("mirror: %lu ms: %lu frames (%lu.%lu fps), oled %lu msgs, rings %lu msgs, events %lu (%lu dropped), "
               "%lu bytes/s, slowest poll %lu us (interrupts included)",
               (unsigned long)ms, (unsigned long)s->frames, (unsigned long)(s->frames * 1000u / ms),
               (unsigned long)(s->frames * 10000u / ms % 10u), (unsigned long)s->oled_msgs,
               (unsigned long)s->ring_msgs, (unsigned long)s->events, (unsigned long)s->events_dropped,
               (unsigned long)(s->bytes * 1000u / ms), (unsigned long)(g_slowest / mhz));
    memset(s, 0, sizeof *s);
    g_slowest = 0;
    g_stats_t0 = now;
}

void mirror_poll(void)
{
    if (!g_mc.on) {
        return;
    }
    uint32_t c0 = DWT->CYCCNT;
    uint32_t now = system_millis();

    uint32_t hw = hw_buttons();
    for (uint32_t changed = hw ^ g_hw_down; changed; changed &= changed - 1u) {
        int i = __builtin_ctz(changed);
        mc_event(&g_mc, MC_EV_BUTTON, i, (hw >> i) & 1u, false);
    }
    g_hw_down = hw;

    mc_src_t src = {
        .ring = ring_levels,
        .flags = MC_FLAG_REMOTE | (E16_DEV_CONSOLE ? MC_FLAG_DEV : 0),
        .version = RT_LAUNCHER_VERSION,
        .write = usb_write,
    };
    if (g_app_oled) {
        src.oled = g_app_oled;
        src.mode = MC_MODE_APP;
        src.name = g_app_name;
    } else {
        src.oled = screen_framebuffer();
        src.mode = (uint8_t)launcher_mirror_mode(&src.name);
    }
    if (midi_usb_connected() && mc_poll(&g_mc, now, &src) == MC_STOPPED) {
        log_printf("mirror: off (no keepalive for %u ms)", MC_KEEPALIVE_MS);
        return;
    }
    if (!midi_usb_connected() && now - g_mc.keepalive_ms >= MC_KEEPALIVE_MS) {
        g_mc.on = false; /* unplugged, or Webcam mode re-enumerating */
        log_printf("mirror: off (USB gone)");
        return;
    }
    uint32_t c = DWT->CYCCNT - c0;
    if (c > g_slowest) {
        g_slowest = c;
    }
    if (now - g_stats_t0 >= STATS_MS) {
        log_stats(now);
    }
}

#endif
