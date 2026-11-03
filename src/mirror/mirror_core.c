/*
 * The mirror stream's portable core: mirror_core.h.
 */
#include "mirror_core.h"

#include <string.h>

#define HDR0 0xF0
#define HDR1 0x7D
#define HDR2 0x45

void mc_init(mc_t *mc)
{
    memset(mc, 0, sizeof *mc);
    mc->oled_next = MC_SEGS;
    mc->ring_next = MC_RINGS;
}

uint32_t mc_hash(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u; /* FNV-1a */
    for (size_t i = 0; i < n; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

size_t mc_pack7(const uint8_t *in, size_t n, uint8_t *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i += 7) {
        size_t k = n - i < 7 ? n - i : 7;
        uint8_t top = 0;
        for (size_t j = 0; j < k; j++) {
            top |= (uint8_t)((in[i + j] >> 7) << j);
        }
        out[o++] = top;
        for (size_t j = 0; j < k; j++) {
            out[o++] = in[i + j] & 0x7F;
        }
    }
    return o;
}

size_t mc_unpack7(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i += 8) {
        uint8_t top = in[i];
        for (size_t j = 1; j < 8 && i + j < n && o < cap; j++) {
            out[o++] = (uint8_t)(in[i + j] | (((top >> (j - 1)) & 1u) << 7));
        }
    }
    return o;
}

/* Four wire bytes (four bits, the first the most significant) at once:
 * a 1 (0xC0) has bit 2 clear, a 0 (0xFC) has it set. Gathers each
 * byte's flag into one nibble with a multiply instead of a loop of
 * compares (this runs for 256 LEDs every scan). */
static uint32_t wire_nibble(const uint8_t *w)
{
    uint32_t x;
    memcpy(&x, w, 4);
    /* Little-endian: the first byte's flag at bit 0, the last's at 24. */
    x = ((x & 0x04040404u) >> 2) ^ 0x01010101u; /* 1 for a one */
    /* bit 0 -> 27, 8 -> 26, 16 -> 25, 24 -> 24; no other product
     * reaches bit 24, and none overlap, so nothing carries. */
    return (x * 0x08040201u) >> 24 & 0xFu;
}

void mc_decode_wire_led(const uint8_t *chain_wire, int led, uint8_t rgb[3])
{
    const uint8_t *w = chain_wire + led * MC_WIRE_BYTES_PER_LED;
    uint8_t ch[3]; /* g, r, b on the wire */
    for (int c = 0; c < 3; c++) {
        ch[c] = (uint8_t)(wire_nibble(w + c * 8) << 4 | wire_nibble(w + c * 8 + 4));
    }
    rgb[0] = ch[1];
    rgb[1] = ch[0];
    rgb[2] = ch[2];
}

static bool is_ours(const uint8_t *msg, size_t len)
{
    return len >= 5 && msg[0] == HDR0 && msg[1] == HDR1 && msg[2] == HDR2 && msg[len - 1] == 0xF7;
}

static void full_refresh(mc_t *mc, uint32_t now)
{
    mc->oled_valid = 0;
    mc->ring_valid = 0;
    mc->state_valid = false;
    mc->full_t0 = now;
    mc->stats.full++;
}

mc_result_t mc_handle(mc_t *mc, const uint8_t *msg, size_t len, uint32_t now)
{
    if (!is_ours(msg, len)) {
        return MC_NOT_MINE;
    }
    if (msg[3] == MC_IN_SUBSCRIBE && (len == 5 || len == 6)) {
        mc->keepalive_ms = now;
        if (!mc->on) {
            mc_init(mc);
            mc->on = true;
            mc->keepalive_ms = now;
            /* Due at once: the first poll starts both scans. */
            mc->oled_t0 = mc->ring_t0 = now - MC_FRAME_MS;
            full_refresh(mc, now);
            mc->stats.full = 0;
            return MC_STARTED;
        }
        if (len == 6 && (msg[4] & 1u)) {
            full_refresh(mc, now);
        }
        return MC_HANDLED;
    }
    if (msg[3] == MC_IN_UNSUBSCRIBE && len == 5) {
        bool was = mc->on;
        mc->on = false;
        return was ? MC_STOPPED : MC_HANDLED;
    }
    return MC_NOT_MINE;
}

void mc_event(mc_t *mc, mc_event_kind_t kind, int index, int value, bool remote)
{
    if (!mc->on || index < 0 || index >= MC_CONTROLS) {
        return;
    }
    if (kind == MC_EV_TURN && mc->ev_count > 0) {
        /* Coalesce with the newest event if it's a turn of the same
         * encoder from the same source that hasn't gone out yet. */
        mc_event_t *e = &mc->ev[(mc->ev_head + mc->ev_count - 1) % MC_EVENTS];
        int sum = e->value + value;
        if (e->kind == MC_EV_TURN && e->index == index && e->source == (remote ? 1 : 0) && sum >= -63 &&
            sum <= 63) {
            e->value = (int8_t)sum;
            return;
        }
    }
    if (mc->ev_count == MC_EVENTS) {
        mc->stats.events_dropped++;
        return;
    }
    mc_event_t *e = &mc->ev[(mc->ev_head + mc->ev_count) % MC_EVENTS];
    if (value > 63) {
        value = 63;
    } else if (value < -63) {
        value = -63;
    }
    e->kind = (uint8_t)kind;
    e->index = (uint8_t)index;
    e->source = remote ? 1 : 0;
    e->value = (int8_t)value;
    mc->ev_count++;
}

static bool send(mc_t *mc, const mc_src_t *src, const uint8_t *msg, size_t len)
{
    if (!src->write(src->write_ud, msg, len)) {
        return false;
    }
    mc->stats.bytes += (uint32_t)len;
    return true;
}

static bool send_events(mc_t *mc, const mc_src_t *src)
{
    while (mc->ev_count > 0) {
        const mc_event_t *e = &mc->ev[mc->ev_head];
        /* mc_event() keeps a turn within +-63: 1-127 here. */
        uint8_t v = (uint8_t)(e->kind == MC_EV_TURN ? e->value + 64 : e->value != 0);
        uint8_t msg[] = {HDR0, HDR1, HDR2, MC_OUT_EVENT, e->kind, e->index, v, e->source, 0xF7};
        if (!send(mc, src, msg, sizeof msg)) {
            return false;
        }
        mc->ev_head = (uint8_t)((mc->ev_head + 1) % MC_EVENTS);
        mc->ev_count--;
        mc->stats.events++;
    }
    return true;
}

/* Printable ASCII only (a SysEx byte must stay below 0x80), at most max. */
static size_t put_text(uint8_t *out, const char *s, size_t max)
{
    size_t n = 0;
    for (; s && *s && n < max; s++) {
        char c = *s;
        out[n++] = (uint8_t)(c >= 0x20 && c < 0x7F ? c : '?');
    }
    return n;
}

static uint32_t state_hash(const mc_src_t *src)
{
    uint8_t buf[2 + MC_NAME_MAX + 8];
    buf[0] = src->mode;
    buf[1] = src->flags;
    size_t n = 2 + put_text(buf + 2, src->version, 8);
    n += put_text(buf + n, src->name, MC_NAME_MAX);
    return mc_hash(buf, n) ^ (uint32_t)n;
}

static bool send_state(mc_t *mc, const mc_src_t *src)
{
    uint32_t h = state_hash(src);
    if (mc->state_valid && h == mc->state_hash) {
        return true;
    }
    uint8_t msg[7 + 8 + 1 + MC_NAME_MAX + 1];
    size_t n = 0;
    msg[n++] = HDR0;
    msg[n++] = HDR1;
    msg[n++] = HDR2;
    msg[n++] = MC_OUT_STATE;
    msg[n++] = MC_PROTO;
    msg[n++] = src->mode & 0x7F;
    msg[n++] = src->flags & 0x7F;
    n += put_text(msg + n, src->version, 8);
    msg[n++] = 0;
    n += put_text(msg + n, src->name, MC_NAME_MAX);
    msg[n++] = 0xF7;
    if (!send(mc, src, msg, n)) {
        return false;
    }
    mc->state_hash = h;
    mc->state_valid = true;
    return true;
}

static bool scan_rings(mc_t *mc, uint32_t now, const mc_src_t *src)
{
    if (mc->ring_next >= MC_RINGS) {
        if (now - mc->ring_t0 < MC_FRAME_MS) {
            return true;
        }
        mc->ring_t0 = now;
        mc->ring_next = 0;
    }
    for (int budget = MC_RINGS_PER_POLL; mc->ring_next < MC_RINGS && budget > 0; budget--) {
        int r = mc->ring_next;
        uint8_t rgb[MC_RING_BYTES];
        if (src->ring) {
            src->ring(src->ring_ud, r, rgb);
        } else {
            memset(rgb, 0, sizeof rgb);
        }
        uint32_t h = mc_hash(rgb, sizeof rgb);
        if (!(mc->ring_valid & (1u << r)) || h != mc->ring_hash[r]) {
            uint8_t msg[MC_MSG_MAX];
            size_t n = 0;
            msg[n++] = HDR0;
            msg[n++] = HDR1;
            msg[n++] = HDR2;
            msg[n++] = MC_OUT_RING;
            msg[n++] = (uint8_t)r;
            n += mc_pack7(rgb, sizeof rgb, msg + n);
            msg[n++] = 0xF7;
            if (!send(mc, src, msg, n)) {
                return false;
            }
            mc->ring_hash[r] = h;
            mc->ring_valid |= 1u << r;
            mc->stats.ring_msgs++;
        }
        mc->ring_next++;
    }
    return true;
}

static bool scan_oled(mc_t *mc, uint32_t now, const mc_src_t *src)
{
    static const uint8_t blank[MC_SEG_COLS];
    if (mc->sync_pending) {
        uint8_t msg[] = {HDR0, HDR1, HDR2, MC_OUT_SYNC, mc->seq, 0xF7};
        if (!send(mc, src, msg, sizeof msg)) {
            return false;
        }
        mc->seq = (uint8_t)((mc->seq + 1) & 0x7F);
        mc->sync_pending = false;
    }
    if (mc->oled_next >= MC_SEGS) {
        if (now - mc->oled_t0 < MC_FRAME_MS) {
            return true;
        }
        mc->oled_t0 = now;
        mc->oled_next = 0;
        mc->oled_any = false;
    }
    for (int budget = MC_SEGS_PER_POLL; mc->oled_next < MC_SEGS && budget > 0; budget--) {
        int s = mc->oled_next;
        int page = s / MC_SEGS_PER_PAGE, col = (s % MC_SEGS_PER_PAGE) * MC_SEG_COLS;
        const uint8_t *p = src->oled ? src->oled + page * MC_OLED_W + col : blank;
        uint32_t h = mc_hash(p, MC_SEG_COLS);
        if (!(mc->oled_valid & (1u << s)) || h != mc->oled_hash[s]) {
            uint8_t msg[MC_MSG_MAX];
            size_t n = 0;
            msg[n++] = HDR0;
            msg[n++] = HDR1;
            msg[n++] = HDR2;
            msg[n++] = MC_OUT_OLED;
            msg[n++] = (uint8_t)page;
            msg[n++] = (uint8_t)col;
            n += mc_pack7(p, MC_SEG_COLS, msg + n);
            msg[n++] = 0xF7;
            if (!send(mc, src, msg, n)) {
                return false;
            }
            mc->oled_hash[s] = h;
            mc->oled_valid |= 1u << s;
            mc->oled_any = true;
            mc->stats.oled_msgs++;
        }
        mc->oled_next++;
    }
    if (mc->oled_next < MC_SEGS) {
        return true; /* the rest of the scan at the next poll */
    }
    if (mc->oled_any) {
        mc->oled_any = false;
        mc->stats.frames++;
        mc->sync_pending = true;
        uint8_t msg[] = {HDR0, HDR1, HDR2, MC_OUT_SYNC, mc->seq, 0xF7};
        if (send(mc, src, msg, sizeof msg)) {
            mc->seq = (uint8_t)((mc->seq + 1) & 0x7F);
            mc->sync_pending = false;
        }
    }
    return true;
}

mc_result_t mc_poll(mc_t *mc, uint32_t now, const mc_src_t *src)
{
    if (!mc->on) {
        return MC_HANDLED;
    }
    if (now - mc->keepalive_ms >= MC_KEEPALIVE_MS) {
        mc->on = false;
        return MC_STOPPED;
    }
    if (now - mc->full_t0 >= MC_FULL_MS) {
        full_refresh(mc, now);
    }
    /* In priority order; each stops at the first write that doesn't fit. */
    if (!send_events(mc, src) || !send_state(mc, src) || !scan_rings(mc, now, src)) {
        return MC_HANDLED;
    }
    scan_oled(mc, now, src);
    return MC_HANDLED;
}
