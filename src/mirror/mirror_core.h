#ifndef MIRROR_CORE_H
#define MIRROR_CORE_H

/*
 * The mirror stream's portable core: a computer that subscribes over USB
 * MIDI gets the OLED, the LED rings and every input event as they happen
 * (tools/mirror/index.html shows them in a browser). Plain C with no
 * device dependencies, host-tested by test/mirror/mirror_test.c; the
 * device glue (src/mirror/mirror.h) feeds it the framebuffer, the LED
 * levels and the inputs, and hands its messages to midi_write().
 *
 * Protocol (SysEx, the dev console's framing F0 7D 45 <cmd> ... F7, see
 * src/console.h). In (computer -> E16):
 *
 *   F0 7D 45 20 F7          subscribe, and keepalive: starts the stream
 *                           (with a full refresh) if it isn't running.
 *                           Without one for MC_KEEPALIVE_MS (3 s) the
 *                           stream stops by itself, so a closed browser
 *                           tab costs nothing for long.
 *   F0 7D 45 20 01 F7       the same, plus a full refresh now
 *   F0 7D 45 21 F7          unsubscribe
 *   F0 7D 45 10 <ctl> <act> <n> F7
 *                           remote control: console.h's virtual control
 *                           (encoders 0-15, 16 = Shift; act 0 = up,
 *                           1 = down, 2 = turn by n - 64)
 *
 * Out (E16 -> computer), while subscribed:
 *
 *   F0 7D 45 70 <proto> <mode> <flags> <version> 00 <name> F7
 *                           state: on subscribe, on every change, and
 *                           with each full refresh. proto = MC_PROTO,
 *                           mode = mc_mode_t, flags bit 0 = remote
 *                           control on, bit 1 = dev console built in;
 *                           version and name are ASCII ("1.2"; the
 *                           script or app running, or in the menu the
 *                           entry selected).
 *   F0 7D 45 71 <page> <col> <packed 32 bytes> F7
 *                           OLED: 32 columns of an 8-pixel page (the
 *                           SSD1306 layout: bit 0 = top pixel), sent
 *                           when they change.
 *   F0 7D 45 72 <ring> <packed 48 bytes> F7
 *                           one LED ring, 16 x (r, g, b) from LED 0, at
 *                           the levels sent to the LEDs (0-255; the
 *                           launcher keeps them at or under ~85).
 *   F0 7D 45 73 <kind> <index> <value> <source> F7
 *                           input event, sent at once: kind 0 = turn
 *                           (value = steps + 64, clamped to 1-127), 1 =
 *                           button (value 1 = down, 0 = up); index 0-15
 *                           = encoders 1-16, 16 = Shift; source 0 = the
 *                           hardware, 1 = remote (a virtual control).
 *   F0 7D 45 74 <seq> F7    end of an OLED frame that changed something
 *                           (seq counts 0-127): a client can draw then.
 *
 * "packed" is 7-bit packing in groups of up to 7 bytes: a byte holding
 * the group's top bits (bit i = top bit of the group's byte i), then the
 * bytes with their top bit cleared; n bytes take n + ceil(n / 7).
 *
 * Pacing: an OLED scan (32 segments compared by hash against what was
 * last sent) starts at most every MC_FRAME_MS, the rings likewise, and
 * each poll does a bounded part of a scan (MC_SEGS_PER_POLL); a
 * full refresh every MC_FULL_MS heals anything a lost message left
 * behind. Every write is a try: when the USB queue is full the scan
 * stops where it is and carries on at the next poll (so a frame that
 * changes meanwhile is sent as it is by then); events wait in a small
 * queue (turns of the same encoder coalesce) and go first.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MC_PROTO 1

#define MC_OLED_W 128
#define MC_OLED_PAGES 8
#define MC_OLED_BYTES (MC_OLED_W * MC_OLED_PAGES)
#define MC_SEG_COLS 32
#define MC_SEGS_PER_PAGE (MC_OLED_W / MC_SEG_COLS)
#define MC_SEGS (MC_OLED_PAGES * MC_SEGS_PER_PAGE) /* 32: one bit each in a uint32_t */
#define MC_RINGS 16
#define MC_LEDS 16
#define MC_RING_BYTES (MC_LEDS * 3)
#define MC_EVENTS 16
#define MC_CONTROLS 17 /* encoders 0-15, Shift 16 */
#define MC_NAME_MAX 31

#define MC_KEEPALIVE_MS 3000u
#define MC_FRAME_MS 33u /* ~30 fps at most, OLED and rings each */
#define MC_FULL_MS 5000u

/* Work per mc_poll(), so no one call holds up the caller's loop for
 * long (the device: ~6 us a segment, ~10 us a ring, interrupts aside):
 * a scan goes on at the next call. */
#define MC_SEGS_PER_POLL 8
#define MC_RINGS_PER_POLL 4

/* Longest message the core writes (a ring: 4 + 1 + 55 + 1). */
#define MC_MSG_MAX 64

#define MC_IN_SUBSCRIBE 0x20
#define MC_IN_UNSUBSCRIBE 0x21
#define MC_OUT_STATE 0x70
#define MC_OUT_OLED 0x71
#define MC_OUT_RING 0x72
#define MC_OUT_EVENT 0x73
#define MC_OUT_SYNC 0x74

#define MC_FLAG_REMOTE 0x01
#define MC_FLAG_DEV 0x02

typedef enum {
    MC_MODE_MENU,
    MC_MODE_SCRIPT,
    MC_MODE_ERROR,
    MC_MODE_DISK,
    MC_MODE_WEBCAM,
    MC_MODE_APP,
} mc_mode_t;

typedef enum { MC_EV_TURN, MC_EV_BUTTON } mc_event_kind_t;

typedef struct {
    uint8_t kind, index, source;
    int8_t value; /* steps (turn) or 0/1 (button) */
} mc_event_t;

/* All-or-nothing write of one message; false when there's no room (the
 * core retries later). */
typedef bool (*mc_write_fn)(void *ud, const uint8_t *msg, size_t len);

/* What there is to mirror, read at each mc_poll(). */
typedef struct {
    const uint8_t *oled; /* MC_OLED_BYTES, page-major; NULL = blank */
    /* Ring r's 16 LEDs as r, g, b (MC_RING_BYTES); NULL = all off. */
    void (*ring)(void *ud, int ring, uint8_t out[MC_RING_BYTES]);
    void *ring_ud;
    uint8_t mode;     /* mc_mode_t */
    uint8_t flags;    /* MC_FLAG_* */
    const char *version;
    const char *name; /* NULL = "" */
    mc_write_fn write;
    void *write_ud;
} mc_src_t;

typedef struct {
    uint32_t frames;       /* OLED scans that sent something */
    uint32_t oled_msgs, ring_msgs, events, events_dropped, bytes;
    uint32_t full;         /* full refreshes */
} mc_stats_t;

typedef struct {
    bool on;
    uint32_t keepalive_ms;   /* when the last subscribe/keepalive came */
    uint32_t oled_hash[MC_SEGS];
    uint32_t ring_hash[MC_RINGS];
    uint32_t oled_valid;     /* bit s: oled_hash[s] is what was sent */
    uint32_t ring_valid;
    uint8_t oled_next;       /* next segment of the scan; MC_SEGS = none running */
    uint8_t ring_next;
    bool oled_any;           /* this scan sent something */
    bool sync_pending;
    uint8_t seq;
    uint32_t oled_t0, ring_t0, full_t0;
    uint32_t state_hash;
    bool state_valid;
    mc_event_t ev[MC_EVENTS];
    uint8_t ev_head, ev_count;
    mc_stats_t stats;
} mc_t;

/* mc_handle()'s result. */
typedef enum { MC_NOT_MINE, MC_HANDLED, MC_STARTED, MC_STOPPED } mc_result_t;

void mc_init(mc_t *mc);

/* A whole SysEx message from the computer (F0 ... F7). Handles the
 * subscribe/unsubscribe messages; anything else is MC_NOT_MINE. */
mc_result_t mc_handle(mc_t *mc, const uint8_t *msg, size_t len, uint32_t now);

/* Queue an input event (nothing while unsubscribed). index 0-16. */
void mc_event(mc_t *mc, mc_event_kind_t kind, int index, int value, bool remote);

/* Send what's due. MC_STOPPED when the keepalive just ran out (the
 * stream is off from here), else MC_HANDLED. */
mc_result_t mc_poll(mc_t *mc, uint32_t now, const mc_src_t *src);

/* Helpers, exposed for the tests and the device glue. */
size_t mc_pack7(const uint8_t *in, size_t n, uint8_t *out);
size_t mc_unpack7(const uint8_t *in, size_t n, uint8_t *out, size_t cap);
uint32_t mc_hash(const uint8_t *p, size_t n);

/*
 * The LED driver's wire format (the SDK's leds.c): each LED is 24 UART
 * bytes, one per bit, green then red then blue, most significant bit
 * first; a 1 is MC_WS_ONE, a 0 MC_WS_ZERO. Decodes LED `led` of a chain
 * (128 LEDs: rings 0-7 or 8-15) into r, g, b.
 */
#define MC_WS_ONE 0xC0
#define MC_WS_ZERO 0xFC
#define MC_WIRE_BYTES_PER_LED 24
void mc_decode_wire_led(const uint8_t *chain_wire, int led, uint8_t rgb[3]);

#endif
