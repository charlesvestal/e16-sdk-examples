#ifndef RT_PORT_H
#define RT_PORT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Absolute times (rt_port_now(), anchors, due times) are rt_time_t, in ms.
 *
 * The device uses double: a float's 24-bit mantissa can't hold a session's
 * millisecond count precisely for long (above 2^24 ms, about 4.7 hours, it
 * can't even count whole ms, and fractional pulse times drift long before),
 * and the M4F's software doubles are cheap enough for a few clock
 * operations per ms. The host build defines RT_TIME_FLOAT so times are
 * float, like the mock under LUA_32BITS, and the specs can compare against
 * the mock bit for bit. Durations (hold time, held_ms, tempo) stay float
 * or are converted to lua_Number where Lua sees them.
 */
#ifdef RT_TIME_FLOAT
typedef float rt_time_t;
#else
typedef double rt_time_t;
#endif
#define RT_TIME_NEVER ((rt_time_t)1e30)

/* Time in ms; may be fractional on the host, whole on the device. */
rt_time_t rt_port_now(void);
/* Send a MIDI message. out: 0 = all ports, 1 = USB, 2 = TRS 1, 3 = TRS 2
 * (provisional), 4 = the internal synth (audio.* below; not part of 0). */
void rt_port_midi(int out, const uint8_t *bytes, size_t len);
/* Ring index 1-16: value 0-16383, palette color 0-99; value < 0 = ring off (unset). */
void rt_port_ring(int index, int value, int color);
/* Label slot 1-16 (NULL = reset); title (NULL = reset to the scene title). */
void rt_port_label(int index, const char *text);
void rt_port_title(const char *text);
/* print() output and runtime errors. */
void rt_port_log(const char *text, size_t len);

/* lua_Alloc for the script's lua_State. The host port implements it with
 * realloc/free; the device port forwards to rt_heap_lua_alloc (rt_heap.h). */
void *rt_port_alloc(void *ud, void *ptr, size_t osize, size_t nsize);

/*
 * draw.* (rt_api_draw.c): a 128-wide script canvas (src/rt/canvas.h holds
 * the shared pixel-buffer ops and clipping both ports use). Coordinates
 * are 0-based and already floored to integers; out-of-range ones are
 * clipped silently, never an error. Drawing is buffered -- a port need
 * not show anything until rt_port_draw_show(). rt_port_draw_release()
 * clears the canvas and turns fullscreen off, handing the screen back to
 * whatever it shows without a canvas (the device's label grid).
 */
void rt_port_draw_clear(void);
void rt_port_draw_pixel(int x, int y, bool on);
void rt_port_draw_line(int x0, int y0, int x1, int y1, bool on);
void rt_port_draw_rect(int x, int y, int w, int h, bool fill, bool on);
void rt_port_draw_text(int x, int y, const char *text, bool on);
void rt_port_draw_show(void);
void rt_port_draw_fullscreen(bool on);
void rt_port_draw_release(void);
/* The canvas's current height (54, or 64 with fullscreen on): draw.height()
 * reads this back rather than rt_api_draw.c keeping its own copy of what
 * the port was told. Width is always RT_CANVAS_WIDTH (canvas.h). */
int rt_port_draw_height(void);

/*
 * ring.* (rt_api_ring.c): per-LED colour on one of the 16 rings. ring,
 * led: 1-based, 1-16 (led position within the ring; see leds.updateByIndex
 * for the ring index convention this matches). r, g, b: 0-255, scaled to
 * the existing brightness cap like the stock ring rendering. Writing a
 * ring this way is buffered like draw's canvas -- rt_port_ring_show()
 * flushes it. The runtime (rt_api_ring.c) tracks which rings are in this
 * "custom" per-LED mode; rt_port_ring_release() is the port's hook for
 * when the runtime takes a ring back out of it (ring.release() or
 * leds.reset() on a custom ring), to restore whatever it was showing
 * before (usually nothing, until leds.updateByIndex sets it again).
 */
void rt_port_led(int ring, int led, int r, int g, int b);
void rt_port_ring_release(int ring);
void rt_port_ring_show(void);

/*
 * audio.* (rt_api_audio.c): the internal mono synth voice, which plays
 * while it's enabled, over USB audio and/or on TRS 2 (TRS 1 stays MIDI),
 * as rt_port_audio_output() routes it. MIDI out port 4 drives the same
 * voice (rt_port_midi(4, ...)); the port owns that mapping.
 *
 * rt_port_audio_enable(): true starts the synth (TRS 2 switches to it if
 * it's routed there), false stops it (sounding notes are released,
 * parameters kept; TRS 2 back to MIDI). rt_port_audio_note() also
 * enables it. velocity 1-127 (>= 100 = accent), 0 = that note off.
 * rt_port_audio_set(): value 0-127, per rt_audio_param_t. All arguments
 * are already range-checked. rt_port_audio_output(): where the synth
 * plays (RT_AUDIO_OUT_AUTO: USB audio while a computer has the E16
 * connected over USB, else TRS 2); rt_port_audio_routing() is where it
 * plays now, AUTO resolved (never RT_AUDIO_OUT_AUTO).
 * rt_port_audio_release() is the runtime's unload hook: silence, TRS 2
 * back to MIDI, the routing back to AUTO and every parameter back to its
 * default, so the next script starts from the same sound.
 */
typedef enum {
    RT_AUDIO_CUTOFF,
    RT_AUDIO_RESONANCE,
    RT_AUDIO_ENVMOD,
    RT_AUDIO_DECAY,
    RT_AUDIO_WAVEFORM, /* < 64 saw, >= 64 square */
    RT_AUDIO_VOLUME,
    RT_AUDIO_GLIDE,    /* glide (slide) time */
    RT_AUDIO_SLIDE,    /* >= 64: overlapping notes glide */
    RT_AUDIO_PARAMS
} rt_audio_param_t;

void rt_port_audio_enable(bool on);
void rt_port_audio_note(int note, int velocity);
void rt_port_audio_off(void);
void rt_port_audio_set(rt_audio_param_t param, int value);
void rt_port_audio_release(void);

typedef enum {
    RT_AUDIO_OUT_AUTO, /* USB while USB is connected, else TRS 2 */
    RT_AUDIO_OUT_USB,
    RT_AUDIO_OUT_TRS2,
    RT_AUDIO_OUT_BOTH,
} rt_audio_out_t;

void rt_port_audio_output(rt_audio_out_t out);
rt_audio_out_t rt_port_audio_routing(void);

/* launcher.has("disk"): true while the FAT volume is mounted (the device
 * port asks volume.h; the host port has a switch for specs). */
bool rt_port_disk_available(void);

#endif
