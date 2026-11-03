/*
 * Host test for src/mirror/mirror_core.c, the mirror stream's portable
 * core. Plain C, run directly by test/run_all.sh; prints "ALL PASSED" and
 * exits 0 on success.
 *
 * A fake USB queue takes what fits in the room it has (all of a
 * message or none, like midi_write()), and a model client decodes every
 * message into its own framebuffer, rings, events and state, the way
 * tools/mirror/index.html does. Checks: the packing, the wire decoding,
 * what a subscribe sends, that only changes go out afterwards, the frame
 * pacing, that a starved queue still converges on the right picture,
 * events (order, coalescing, overflow), the state message, the full
 * refresh and the keepalive timeout.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mirror_core.h"

static int g_failures;
#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            g_failures++;                                                                          \
            if (g_failures < 30) {                                                                 \
                fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                               \
                fprintf(stderr, __VA_ARGS__);                                                      \
                fprintf(stderr, "\n");                                                             \
            }                                                                                      \
        }                                                                                          \
    } while (0)

/* ---- the device side: sources ---- */

static uint8_t g_oled[MC_OLED_BYTES];
static uint8_t g_rings[MC_RINGS][MC_RING_BYTES];

static void ring_src(void *ud, int ring, uint8_t out[MC_RING_BYTES])
{
    (void)ud;
    memcpy(out, g_rings[ring], MC_RING_BYTES);
}

/* ---- the fake USB queue and the model client ---- */

static size_t g_budget;       /* room left in the queue */
static size_t g_written;      /* total bytes taken */
static int g_msgs;            /* messages taken */
static int g_by_cmd[256];     /* messages taken, by command */

typedef struct {
    uint8_t oled[MC_OLED_BYTES];
    uint8_t rings[MC_RINGS][MC_RING_BYTES];
    int mode, flags, proto, states, syncs, last_seq;
    char version[16], name[40];
    mc_event_t ev[256];
    int n_ev;
} client_t;
static client_t g_cl;

static void client_take(const uint8_t *m, size_t len)
{
    CHECK(len >= 5 && m[0] == 0xF0 && m[1] == 0x7D && m[2] == 0x45 && m[len - 1] == 0xF7, "framing");
    CHECK(len <= MC_MSG_MAX, "message of %zu bytes over MC_MSG_MAX", len);
    for (size_t i = 1; i + 1 < len; i++) {
        CHECK(m[i] < 0x80, "byte %zu of a 0x%02x message is 0x%02x", i, m[3], m[i]);
    }
    const uint8_t *p = m + 4;
    size_t n = len - 5;
    switch (m[3]) {
        case MC_OUT_OLED: {
            CHECK(n >= 2, "short oled");
            int page = p[0], col = p[1];
            uint8_t buf[MC_SEG_COLS];
            size_t got = mc_unpack7(p + 2, n - 2, buf, sizeof buf);
            CHECK(got == MC_SEG_COLS && page < 8 && col % MC_SEG_COLS == 0 && col < 128, "oled %d %d %zu", page,
                  col, got);
            if (got == MC_SEG_COLS && page < 8 && col < 128) {
                memcpy(g_cl.oled + page * 128 + col, buf, MC_SEG_COLS);
            }
            break;
        }
        case MC_OUT_RING: {
            int r = p[0];
            uint8_t buf[MC_RING_BYTES];
            size_t got = mc_unpack7(p + 1, n - 1, buf, sizeof buf);
            CHECK(r < MC_RINGS && got == MC_RING_BYTES, "ring %d %zu", r, got);
            if (r < MC_RINGS) {
                memcpy(g_cl.rings[r], buf, MC_RING_BYTES);
            }
            break;
        }
        case MC_OUT_EVENT:
            CHECK(n == 4, "event length %zu", n);
            if (g_cl.n_ev < 256) {
                mc_event_t *e = &g_cl.ev[g_cl.n_ev++];
                e->kind = p[0];
                e->index = p[1];
                e->value = (int8_t)(p[0] == MC_EV_TURN ? p[2] - 64 : p[2]);
                e->source = p[3];
            }
            break;
        case MC_OUT_STATE: {
            g_cl.proto = p[0];
            g_cl.mode = p[1];
            g_cl.flags = p[2];
            size_t i = 3, k = 0;
            while (i < n && p[i] != 0 && k < sizeof g_cl.version - 1) {
                g_cl.version[k++] = (char)p[i++];
            }
            g_cl.version[k] = 0;
            CHECK(i < n && p[i] == 0, "state: no separator");
            i++;
            k = 0;
            while (i < n && k < sizeof g_cl.name - 1) {
                g_cl.name[k++] = (char)p[i++];
            }
            g_cl.name[k] = 0;
            g_cl.states++;
            break;
        }
        case MC_OUT_SYNC:
            CHECK(n == 1, "sync length");
            CHECK(g_cl.syncs == 0 || p[0] == ((g_cl.last_seq + 1) & 0x7F), "sync seq %d after %d", p[0],
                  g_cl.last_seq);
            g_cl.last_seq = p[0];
            g_cl.syncs++;
            break;
        default:
            CHECK(0, "unknown message 0x%02x", m[3]);
    }
}

static bool fake_write(void *ud, const uint8_t *msg, size_t len)
{
    (void)ud;
    if (len > g_budget) {
        return false;
    }
    g_budget -= len;
    g_written += len;
    g_msgs++;
    g_by_cmd[msg[3]]++;
    client_take(msg, len);
    return true;
}

static mc_t mc;
static uint32_t now;
static mc_src_t src;

static void reset(void)
{
    mc_init(&mc);
    memset(&g_cl, 0, sizeof g_cl);
    memset(g_oled, 0, sizeof g_oled);
    memset(g_rings, 0, sizeof g_rings);
    memset(g_by_cmd, 0, sizeof g_by_cmd);
    g_written = 0;
    g_msgs = 0;
    now = 1000;
    src = (mc_src_t){
        .oled = g_oled,
        .ring = ring_src,
        .mode = MC_MODE_MENU,
        .flags = MC_FLAG_REMOTE,
        .version = "1.2",
        .name = NULL,
        .write = fake_write,
    };
}

static mc_result_t sysex(const uint8_t *m, size_t len)
{
    return mc_handle(&mc, m, len, now);
}

static mc_result_t subscribe(void)
{
    static const uint8_t m[] = {0xF0, 0x7D, 0x45, MC_IN_SUBSCRIBE, 0xF7};
    return sysex(m, sizeof m);
}

/* One main-loop pass's worth of room, over enough polls (at the same
 * time) to finish any scan that's due: each does only part of one
 * (MC_SEGS_PER_POLL, MC_RINGS_PER_POLL). */
static mc_result_t poll(size_t budget)
{
    g_budget = budget;
    mc_result_t r = MC_HANDLED;
    for (int i = 0; i < 8 && r == MC_HANDLED; i++) {
        r = mc_poll(&mc, now, &src);
    }
    return r;
}

static void clear_counts(void)
{
    memset(g_by_cmd, 0, sizeof g_by_cmd);
    g_msgs = 0;
    g_written = 0;
}

static bool client_matches(void)
{
    return memcmp(g_cl.oled, g_oled, sizeof g_oled) == 0 && memcmp(g_cl.rings, g_rings, sizeof g_rings) == 0;
}

/* ---- tests ---- */

static void test_pack(void)
{
    srand(1);
    for (int t = 0; t < 2000; t++) {
        size_t n = (size_t)(rand() % 100);
        uint8_t in[100], packed[120], out[100];
        for (size_t i = 0; i < n; i++) {
            in[i] = (uint8_t)rand();
        }
        size_t pn = mc_pack7(in, n, packed);
        CHECK(pn == n + (n + 6) / 7, "packed length %zu for %zu", pn, n);
        for (size_t i = 0; i < pn; i++) {
            CHECK(packed[i] < 0x80, "packed byte 0x%02x", packed[i]);
        }
        size_t un = mc_unpack7(packed, pn, out, sizeof out);
        CHECK(un == n && memcmp(in, out, n) == 0, "round trip of %zu bytes", n);
    }
    /* The documented layout: group top bits first, bit i = byte i. */
    uint8_t in[] = {0x80, 0x01, 0xFF};
    uint8_t p[4];
    mc_pack7(in, 3, p);
    CHECK(p[0] == 0x05 && p[1] == 0x00 && p[2] == 0x01 && p[3] == 0x7F, "layout %02x %02x %02x %02x", p[0], p[1],
          p[2], p[3]);
}

/* leds.c's encoding, as the SDK does it. */
static void encode_byte(uint8_t value, uint8_t *out)
{
    for (int bit = 0; bit < 8; bit++) {
        out[bit] = (value & (0x80 >> bit)) ? MC_WS_ONE : MC_WS_ZERO;
    }
}

static void test_wire(void)
{
    static uint8_t wire[128 * MC_WIRE_BYTES_PER_LED];
    uint8_t rgb[128][3];
    srand(2);
    for (int led = 0; led < 128; led++) {
        for (int c = 0; c < 3; c++) {
            rgb[led][c] = (uint8_t)rand();
        }
        uint8_t *out = &wire[led * MC_WIRE_BYTES_PER_LED];
        encode_byte(rgb[led][1], out);
        encode_byte(rgb[led][0], out + 8);
        encode_byte(rgb[led][2], out + 16);
    }
    for (int led = 0; led < 128; led++) {
        uint8_t got[3];
        mc_decode_wire_led(wire, led, got);
        CHECK(memcmp(got, rgb[led], 3) == 0, "led %d: %d,%d,%d != %d,%d,%d", led, got[0], got[1], got[2],
              rgb[led][0], rgb[led][1], rgb[led][2]);
    }
}

static void test_subscribe_and_diff(void)
{
    reset();
    /* Off: nothing at all. */
    CHECK(poll(100000) == MC_HANDLED && g_msgs == 0, "sent %d while off", g_msgs);
    mc_event(&mc, MC_EV_TURN, 0, 1, false);
    CHECK(poll(100000) == MC_HANDLED && g_msgs == 0, "event queued while off");

    g_oled[5] = 0x81;
    g_rings[3][7] = 40;
    CHECK(subscribe() == MC_STARTED, "subscribe");
    CHECK(poll(100000) == MC_HANDLED, "poll");
    CHECK(g_by_cmd[MC_OUT_STATE] == 1, "state %d", g_by_cmd[MC_OUT_STATE]);
    CHECK(g_by_cmd[MC_OUT_RING] == MC_RINGS, "rings %d", g_by_cmd[MC_OUT_RING]);
    CHECK(g_by_cmd[MC_OUT_OLED] == MC_SEGS, "segments %d", g_by_cmd[MC_OUT_OLED]);
    CHECK(g_by_cmd[MC_OUT_SYNC] == 1, "sync %d", g_by_cmd[MC_OUT_SYNC]);
    CHECK(client_matches(), "client after the full refresh");
    CHECK(g_cl.proto == MC_PROTO && g_cl.mode == MC_MODE_MENU && g_cl.flags == MC_FLAG_REMOTE &&
              strcmp(g_cl.version, "1.2") == 0 && g_cl.name[0] == 0,
          "state %d %d %d '%s' '%s'", g_cl.proto, g_cl.mode, g_cl.flags, g_cl.version, g_cl.name);
    size_t full = g_written;
    printf("full refresh: %zu bytes (%d messages)\n", full, g_msgs);
    CHECK(full < 2500, "full refresh %zu bytes", full);

    /* Nothing changed: nothing sent, however often it polls. */
    clear_counts();
    for (int i = 0; i < 20; i++) {
        now += 10;
        poll(100000);
    }
    CHECK(g_msgs == 0, "%d messages with nothing changed", g_msgs);

    /* One pixel: one segment and a sync, on the next scan. */
    clear_counts();
    now += 40;
    poll(100000); /* starts and finishes a scan */
    g_oled[3 * 128 + 70] ^= 0x10;
    now += 1;
    poll(100000);
    CHECK(g_msgs == 0, "a scan within MC_FRAME_MS of the last (%d)", g_msgs);
    now += MC_FRAME_MS;
    poll(100000);
    CHECK(g_by_cmd[MC_OUT_OLED] == 1 && g_by_cmd[MC_OUT_SYNC] == 1 && g_msgs == 2, "one pixel: %d oled %d msgs",
          g_by_cmd[MC_OUT_OLED], g_msgs);
    CHECK(client_matches(), "client after one pixel");

    /* One LED: one ring. */
    clear_counts();
    g_rings[15][47] = 85;
    now += MC_FRAME_MS;
    poll(100000);
    CHECK(g_by_cmd[MC_OUT_RING] == 1 && g_msgs == 1, "one LED: %d rings %d msgs", g_by_cmd[MC_OUT_RING], g_msgs);
    CHECK(client_matches(), "client after one LED");

    /* A blank source (NULL) is a black screen. */
    src.oled = NULL;
    now += MC_FRAME_MS;
    poll(100000);
    static const uint8_t zero[MC_OLED_BYTES];
    CHECK(memcmp(g_cl.oled, zero, sizeof zero) == 0, "NULL oled");
}

/* Changes every few ms with the SDK's 256-byte queue, drained at 48 bytes
 * (a USB packet of SysEx) a millisecond: once the changes stop, the
 * client converges on the source exactly. */
/* Each poll does a bounded part of a scan. */
static void test_bounded(void)
{
    reset();
    subscribe();
    g_budget = 100000;
    mc_poll(&mc, now, &src);
    CHECK(g_by_cmd[MC_OUT_RING] == MC_RINGS_PER_POLL && g_by_cmd[MC_OUT_OLED] == MC_SEGS_PER_POLL,
          "first poll: %d rings %d segments", g_by_cmd[MC_OUT_RING], g_by_cmd[MC_OUT_OLED]);
    CHECK(g_by_cmd[MC_OUT_SYNC] == 0, "no sync before the scan ends");
}

static void test_starved(void)
{
    reset();
    subscribe();
    srand(3);
    size_t fill = 0;
    for (int step = 0; step < 3000; step++) {
        now += 1;
        if (step < 2000 && rand() % 4 == 0) {
            g_oled[rand() % MC_OLED_BYTES] = (uint8_t)rand();
        }
        if (step < 2000 && rand() % 16 == 0) {
            g_rings[rand() % MC_RINGS][rand() % MC_RING_BYTES] = (uint8_t)(rand() % 86);
        }
        if (step % 500 == 0) {
            subscribe(); /* keepalive */
        }
        fill = fill > 48 ? fill - 48 : 0;
        size_t before = g_written;
        poll(256 - fill);
        fill += g_written - before;
    }
    CHECK(mc.on, "still on");
    CHECK(client_matches(), "client converged");
    printf("starved run: %d frames, %u oled msgs, %u ring msgs in 3 s\n", (int)mc.stats.frames,
           (unsigned)mc.stats.oled_msgs, (unsigned)mc.stats.ring_msgs);
    CHECK(mc.stats.frames > 10, "frames %u", (unsigned)mc.stats.frames);
}

static void test_events(void)
{
    reset();
    subscribe();
    poll(100000);
    g_cl.n_ev = 0;
    /* Turns of one encoder from one source coalesce until sent. */
    mc_event(&mc, MC_EV_TURN, 2, 1, false);
    mc_event(&mc, MC_EV_TURN, 2, 3, false);
    mc_event(&mc, MC_EV_TURN, 2, -1, true); /* remote: its own event */
    mc_event(&mc, MC_EV_BUTTON, 16, 1, false);
    mc_event(&mc, MC_EV_TURN, 2, 1, false); /* after the button: not merged */
    mc_event(&mc, MC_EV_BUTTON, 17, 1, false); /* out of range: ignored */
    mc_event(&mc, MC_EV_TURN, 5, 100, false);  /* clamped */
    clear_counts();
    poll(100000);
    CHECK(g_cl.n_ev == 5, "events %d", g_cl.n_ev);
    CHECK(g_cl.ev[0].kind == MC_EV_TURN && g_cl.ev[0].index == 2 && g_cl.ev[0].value == 4 && g_cl.ev[0].source == 0,
          "ev0 %d %d %d %d", g_cl.ev[0].kind, g_cl.ev[0].index, g_cl.ev[0].value, g_cl.ev[0].source);
    CHECK(g_cl.ev[1].value == -1 && g_cl.ev[1].source == 1, "ev1");
    CHECK(g_cl.ev[2].kind == MC_EV_BUTTON && g_cl.ev[2].index == 16 && g_cl.ev[2].value == 1, "ev2");
    CHECK(g_cl.ev[3].kind == MC_EV_TURN && g_cl.ev[3].value == 1, "ev3");
    CHECK(g_cl.ev[4].index == 5 && g_cl.ev[4].value == 63, "ev4 clamp %d", g_cl.ev[4].value);

    /* Events go first, before a frame that's due. */
    g_cl.n_ev = 0;
    g_oled[0] ^= 1;
    now += MC_FRAME_MS;
    mc_event(&mc, MC_EV_BUTTON, 0, 1, false);
    g_budget = 9; /* room for the event only */
    mc_poll(&mc, now, &src);
    CHECK(g_cl.n_ev == 1, "event first");

    /* A full queue drops (and counts) what doesn't fit. */
    for (int i = 0; i < MC_EVENTS + 4; i++) {
        mc_event(&mc, MC_EV_BUTTON, i % 16, i & 1, false);
    }
    CHECK(mc.stats.events_dropped == 4, "dropped %u", (unsigned)mc.stats.events_dropped);
    g_cl.n_ev = 0;
    poll(100000);
    CHECK(g_cl.n_ev == MC_EVENTS, "sent %d of the full queue", g_cl.n_ev);
}

static void test_state(void)
{
    reset();
    subscribe();
    poll(100000);
    CHECK(g_cl.states == 1, "one state");
    now += 5;
    poll(100000);
    CHECK(g_cl.states == 1, "no state without a change");
    src.mode = MC_MODE_SCRIPT;
    src.name = "Acid\x01\xC3\xA9 line";
    poll(100000);
    CHECK(g_cl.states == 2 && g_cl.mode == MC_MODE_SCRIPT && strcmp(g_cl.name, "Acid??? line") == 0,
          "state after a change: %d '%s'", g_cl.states, g_cl.name);
    static char longname[100];
    memset(longname, 'x', sizeof longname - 1);
    src.name = longname;
    poll(100000);
    CHECK(strlen(g_cl.name) == MC_NAME_MAX, "name cut to %zu", strlen(g_cl.name));
    /* A state that doesn't fit waits, and everything after it too. */
    src.mode = MC_MODE_APP;
    g_oled[100] ^= 0xFF;
    now += MC_FRAME_MS;
    clear_counts();
    poll(10);
    CHECK(g_msgs == 0, "nothing past a blocked state");
    poll(100000);
    CHECK(g_cl.mode == MC_MODE_APP && client_matches(), "after the blocked state");
}

static void test_full_refresh_and_timeout(void)
{
    reset();
    CHECK(subscribe() == MC_STARTED, "start");
    poll(100000);
    CHECK(subscribe() == MC_HANDLED, "keepalive");
    /* 0x20 01: a full refresh on request. */
    static const uint8_t full[] = {0xF0, 0x7D, 0x45, MC_IN_SUBSCRIBE, 0x01, 0xF7};
    CHECK(sysex(full, sizeof full) == MC_HANDLED, "refresh request");
    clear_counts();
    now += MC_FRAME_MS;
    poll(100000);
    CHECK(g_by_cmd[MC_OUT_OLED] == MC_SEGS && g_by_cmd[MC_OUT_RING] == MC_RINGS && g_by_cmd[MC_OUT_STATE] == 1,
          "requested refresh %d %d %d", g_by_cmd[MC_OUT_OLED], g_by_cmd[MC_OUT_RING], g_by_cmd[MC_OUT_STATE]);

    /* The periodic one, MC_FULL_MS after the last. */
    clear_counts();
    for (uint32_t t = 0; t < MC_FULL_MS; t += 10) {
        now += 10;
        if (t % 1000 == 0) {
            subscribe();
        }
        poll(100000);
    }
    CHECK(g_by_cmd[MC_OUT_OLED] == MC_SEGS && g_by_cmd[MC_OUT_RING] == MC_RINGS, "periodic refresh %d %d",
          g_by_cmd[MC_OUT_OLED], g_by_cmd[MC_OUT_RING]);

    /* No keepalive for MC_KEEPALIVE_MS: off, and quiet. */
    subscribe();
    now += MC_KEEPALIVE_MS - 1;
    CHECK(poll(100000) == MC_HANDLED && mc.on, "on just before the timeout");
    now += 1;
    CHECK(poll(100000) == MC_STOPPED && !mc.on, "off at the timeout");
    clear_counts();
    g_oled[9] ^= 1;
    now += 100;
    mc_event(&mc, MC_EV_TURN, 1, 1, false);
    CHECK(poll(100000) == MC_HANDLED && g_msgs == 0, "quiet after the timeout");
    /* A keepalive after that starts it again, with a full refresh. */
    CHECK(subscribe() == MC_STARTED, "restart");
    g_cl.syncs = 0; /* the frame counter starts over */
    clear_counts();
    poll(100000);
    CHECK(g_by_cmd[MC_OUT_OLED] == MC_SEGS && client_matches(), "restart refresh");

    /* Unsubscribe. */
    static const uint8_t unsub[] = {0xF0, 0x7D, 0x45, MC_IN_UNSUBSCRIBE, 0xF7};
    CHECK(sysex(unsub, sizeof unsub) == MC_STOPPED && !mc.on, "unsubscribe");
    CHECK(sysex(unsub, sizeof unsub) == MC_HANDLED, "unsubscribe again");
    /* Not the mirror's. */
    static const uint8_t other[] = {0xF0, 0x7D, 0x45, 0x10, 0x00, 0x02, 0x41, 0xF7};
    CHECK(sysex(other, sizeof other) == MC_NOT_MINE, "0x10 isn't the core's");
    static const uint8_t bad[] = {0xF0, 0x7D, 0x45, MC_IN_SUBSCRIBE, 0x01, 0x02, 0xF7};
    CHECK(sysex(bad, sizeof bad) == MC_NOT_MINE, "wrong length");
}

int main(void)
{
    test_pack();
    test_wire();
    test_subscribe_and_diff();
    test_bounded();
    test_starved();
    test_events();
    test_state();
    test_full_refresh_and_timeout();
    if (g_failures) {
        printf("%d FAILED\n", g_failures);
        return 1;
    }
    printf("ALL PASSED\n");
    return 0;
}
