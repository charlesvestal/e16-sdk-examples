/*
 * src/rt/rt_port.h on the E16 SDK.
 */
#include "port_e16.h"
#include "volume.h"
#include "port_e16_audio.h"
#include "noise_lab.h"
#include "rt_api_ring.h"
#include "rt_port.h"
#include "rt_heap.h"
#include "log.h"
#include "mem_layout.h"

#include "enc.h"
#include "leds.h"
#include "midi.h"
#include "system.h"

#include <stdbool.h>
#include <string.h>

/*
 * Lua heap: what's left of the CCM plus an SRAM pool (mem_layout.h).
 *
 * CCM (64 KB, NOLOAD, not DMA-able): the static buffers only the normal
 * mode uses and no DMA touches live there (src/ccm.h's CCM_BSS, ~9 KB:
 * store's two 2 KB buffers, the scene list (scripts and apps) and its
 * parsed header and read buffer, persistence's record shadows, the
 * screen's framebuffer, the draw canvas); the pool gets the rest, up to
 * the reset-surviving records at the top (boot_records.h).
 * CCM_STATIC_BYTES is "the rest of .ccmram" rounded up:
 * tools/check_mem_layout.py fails the build if .ccmram reaches the
 * records (check the map's .ccmram size when moving something there, and
 * grow CCM_STATIC_BYTES by as much). Buffers app mode also needs (the
 * log, the console line, the disk's block cache, the synth voice) are in
 * SRAM, since an app gets all of the CCM.
 *
 * SRAM: the pool starts right after .bss (_ebss) and is sized to leave
 * real stack headroom above it (see stack_guard.h). LUAI_MAXCCALLS is kept
 * at Lua's stock default (200); a 180-deep nested expression (the
 * parser's C recursion) measured ~21.5 KB of stack (~19.5 KB with this
 * layout, the audio interrupt running). The pool leaves ~30 KB of stack
 * above the canary at boot (the "launcher: stack free" log line), with
 * what must stay in SRAM: the LED driver's DMA buffers (~7 KB), the SDK's
 * MIDI rings and USB buffers, FatFs's ~0.7 KB (src/disk/volume.c), the
 * log, the console line and the block cache (~8 KB), the runtime's var
 * slots and controller state. Shrink this pool if more .bss is added (or
 * move the buffer to CCM); the build check insists on MIN_STACK_BYTES.
 */
static uint8_t ccm_pool[LUA_CCM_POOL_BYTES] __attribute__((section(".ccmram"), aligned(8)));

#define LED_MAX 40              /* brightest channel value we drive */
#define LED_FRAME_MS 10u        /* at most one leds_show() per this */

static bool g_leds_dirty;
static uint32_t g_leds_last;

/* What leds.updateByIndex last asked of each ring (value -1 = off), so a
 * ring leaving ring.*'s custom mode can go back to it. */
static int16_t g_ring_value[LEDS_RINGS];
static uint8_t g_ring_color[LEDS_RINGS];

static char g_labels[16][5];
static bool g_label_set[16];
static char g_title[16];
static bool g_title_set;
static uint32_t g_ui_version;

void port_e16_init(void)
{
    rt_heap_init(ccm_pool, sizeof ccm_pool);
    rt_heap_add_pool(mem_sram_free_base(), LUA_SRAM_POOL_BYTES);
    for (int i = 0; i < LEDS_RINGS; i++) {
        g_ring_value[i] = -1;
    }
    leds_init();
    enc_init();
    leds_show();
    g_leds_last = system_millis();
    port_e16_canvas_init();
    port_e16_audio_init();
}

/* --- time --- */

rt_time_t rt_port_now(void)
{
    /* system_millis() wraps after ~49 days; extend it to 64 bits. Only
     * ever called from the main loop. */
    static uint32_t last;
    static uint64_t high;
    uint32_t ms = system_millis();
    if (ms < last) {
        high += (uint64_t)1 << 32;
    }
    last = ms;
    return (rt_time_t)(high + ms);
}

/* --- MIDI out --- */

/* out: 0 = all, 1 = USB, 2 = TRS 1, 3 = TRS 2 (provisional numbering, see
 * rt_port.h), 4 = the internal synth (port_e16_audio.h); other values send
 * nothing. 0 is the three MIDI outputs, not the synth. While TRS 2 belongs
 * to the synth (or is about to), 0 and 3 send nothing there: MIDI bytes
 * must never reach the audio pin. A full output buffer (or no computer on
 * USB) drops the message for that output only. */
void rt_port_midi(int out, const uint8_t *bytes, size_t len)
{
    switch (out) {
        case 0:
            midi_write(MIDI_USB, bytes, len);
            midi_write(MIDI_TRS_1, bytes, len);
            if (port_e16_audio_trs2_midi()) {
                midi_write(MIDI_TRS_2, bytes, len);
            }
            break;
        case 1:
            midi_write(MIDI_USB, bytes, len);
            break;
        case 2:
            midi_write(MIDI_TRS_1, bytes, len);
            break;
        case 3:
            if (port_e16_audio_trs2_midi()) {
                midi_write(MIDI_TRS_2, bytes, len);
            }
            break;
        case 4:
            port_e16_audio_midi(bytes, len);
            break;
        default:
            break;
    }
}

/* --- rings --- */

static uint8_t scale(uint8_t c, uint8_t max)
{
    /* Keep the brightest channel at or below LED_MAX, preserving the hue. */
    if (max <= LED_MAX) {
        return c;
    }
    return (uint8_t)((unsigned)c * LED_MAX / max);
}

/* Provisional drawing (stock rendering not measured): value 0-16383 lights
 * (value * 16 + 8191) / 16383 LEDs from LED 0 clockwise in palette[color];
 * the rest are off. value < 0 (reset) turns the ring off. */
static void ring_draw(int ring, int value, int color)
{
    int lit = 0;
    uint8_t r = 0, g = 0, b = 0;
    if (value >= 0) {
        if (value > 16383) {
            value = 16383;
        }
        lit = (value * LEDS_PER_RING + 8191) / 16383;
        if (color < 0) {
            color = 0;
        } else if (color >= PORT_E16_COLORS) {
            color = PORT_E16_COLORS - 1; /* leds.updateByIndex accepts 100 */
        }
        const uint8_t *p = port_e16_palette[color];
        uint8_t max = p[0] > p[1] ? p[0] : p[1];
        max = max > p[2] ? max : p[2];
        r = scale(p[0], max);
        g = scale(p[1], max);
        b = scale(p[2], max);
    }
    for (int led = 0; led < LEDS_PER_RING; led++) {
        if (led < lit) {
            leds_set(ring, led, noise_lab_led(r), noise_lab_led(g), noise_lab_led(b));
        } else {
            leds_set(ring, led, 0, 0, 0);
        }
    }
    g_leds_dirty = true;
}

void rt_port_ring(int index, int value, int color)
{
    if (index < 1 || index > LEDS_RINGS) {
        return;
    }
    g_ring_value[index - 1] = (int16_t)(value < 0 ? -1 : value > 16383 ? 16383 : value);
    g_ring_color[index - 1] = (uint8_t)(color < 0 ? 0 : color > 255 ? 255 : color);
    ring_draw(index - 1, value, color);
}

/* --- ring.* (per-LED colour; rt_api_ring.c tracks which rings are custom) --- */

/* 0-255 -> 0-LED_MAX: the same cap the value rings stay under. */
static uint8_t scale255(int c)
{
    return (uint8_t)((c * LED_MAX + 127) / 255);
}

/* Written straight into the LED driver's buffer, like ring_draw(): it goes
 * out with the next frame after ring.show() (or after any other ring
 * change, which in practice means the same frame -- a script sets its
 * LEDs and shows them in one callback). */
void rt_port_led(int ring, int led, int r, int g, int b)
{
    if (ring < 1 || ring > LEDS_RINGS || led < 1 || led > LEDS_PER_RING) {
        return;
    }
    leds_set(ring - 1, led - 1, noise_lab_led(scale255(r)), noise_lab_led(scale255(g)),
             noise_lab_led(scale255(b)));
}

/* Back to what leds.updateByIndex last set (usually nothing). */
void rt_port_ring_release(int ring)
{
    if (ring < 1 || ring > LEDS_RINGS) {
        return;
    }
    ring_draw(ring - 1, g_ring_value[ring - 1], g_ring_color[ring - 1]);
}

void rt_port_ring_show(void)
{
    g_leds_dirty = true;
}

/* The noise lab switched the LEDs back on or changed their brightness:
 * the value rings again; custom (ring.*) rings come back the next time
 * the script writes them. */
void port_e16_leds_redraw(void)
{
    for (int ring = 0; ring < LEDS_RINGS; ring++) {
        if (!rt_api_ring_is_custom(ring + 1)) {
            ring_draw(ring, g_ring_value[ring], g_ring_color[ring]);
        }
    }
}

void port_e16_leds_poll(void)
{
    if (!g_leds_dirty || noise_lab_leds_off()) {
        return;
    }
    uint32_t now = system_millis();
    if (now - g_leds_last < LED_FRAME_MS) {
        return;
    }
    /* leds_set() only touches the SDK's RGB buffer, so rings can change
     * while a frame is still going out; a busy show is retried next pass. */
    if (leds_show()) {
        g_leds_dirty = false;
        g_leds_last = now;
    }
}

/* --- labels and title (drawn by src/ui/screen.c) --- */

/* A slot or the title is "unset" (NULL to the screen) until the script sets
 * it, and again after slots.reset(): the screen then shows the stock
 * default (the control's abbr, or the scene title). g_ui_version moves
 * only when what the screen would draw changes, so a script rewriting the
 * same text every 20 ms costs no redraw. */
static void set_text(char *dst, size_t size, bool *is_set, const char *text)
{
    if (!text) {
        if (*is_set) {
            *is_set = false;
            dst[0] = '\0';
            g_ui_version++;
        }
        return;
    }
    if (*is_set && strncmp(dst, text, size - 1) == 0) {
        return;
    }
    strncpy(dst, text, size - 1);
    dst[size - 1] = '\0';
    *is_set = true;
    g_ui_version++;
}

void rt_port_label(int index, const char *text)
{
    if (index < 1 || index > 16) {
        return;
    }
    set_text(g_labels[index - 1], sizeof g_labels[0], &g_label_set[index - 1], text);
}

void rt_port_title(const char *text)
{
    set_text(g_title, sizeof g_title, &g_title_set, text);
}

const char *port_e16_label(int index)
{
    return (index >= 1 && index <= 16 && g_label_set[index - 1]) ? g_labels[index - 1] : NULL;
}

const char *port_e16_title(void)
{
    return g_title_set ? g_title : NULL;
}

uint32_t port_e16_ui_version(void)
{
    return g_ui_version;
}

void port_e16_ui_clear(void)
{
    for (int i = 1; i <= 16; i++) {
        rt_port_label(i, NULL);
        rt_port_ring(i, -1, 0);
    }
    rt_port_title(NULL);
}

/* --- log and heap --- */

/* print() output ends in '\n'; runtime error text doesn't, so end the line
 * here rather than let it merge into the next one. */
void rt_port_log(const char *text, size_t len)
{
    log_write(text, len);
    if (len == 0 || text[len - 1] != '\n') {
        log_write("\n", 1);
    }
}

void *rt_port_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    return rt_heap_lua_alloc(ud, ptr, osize, nsize);
}

bool rt_port_disk_available(void)
{
    return volume_mounted();
}
