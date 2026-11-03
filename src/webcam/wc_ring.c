/*
 * Webcam mode's render-ahead bookkeeping: wc_ring.h.
 */
#include "wc_ring.h"

#define H WC_RING_H_LINES
#define AHEAD WC_RING_AHEAD

_Static_assert(H % AHEAD == 0, "WC_RING_AHEAD must divide the height");

/* Lines from (gen, y) up to the line the consumer wants next (negative: behind it). */
static int32_t lines_ahead(const wc_ring_t *r, uint32_t gen, uint32_t y)
{
    return (int32_t)(gen - r->cons_gen) * H + (int32_t)y - (int32_t)r->cons_y;
}

void wc_ring_reset(wc_ring_t *r)
{
    r->gen = 0;
    r->cons_gen = r->prod_gen = 1;
    r->cons_y = r->prod_y = 0;
    r->view_gen[0] = r->view_gen[1] = WC_RING_NO_TAG;
    r->view_tick[0] = r->view_tick[1] = 0;
    for (int i = 0; i < AHEAD; i++) {
        r->tag[i] = WC_RING_NO_TAG;
    }
    r->discards = 0;
    r->discard_gen = 0;
}

uint32_t wc_ring_frame(wc_ring_t *r, uint32_t now_ms, bool *make_view)
{
    uint32_t gen = r->gen + 1u;
    int b = (int)(gen & 1u);
    if (r->view_gen[b] == gen && now_ms - r->view_tick[b] > WC_RING_STALE_MS) {
        /* Made ahead before the computer paused: skip that frame, lines
         * and all (their tags name a frame that never comes). */
        gen++;
        b ^= 1;
    }
    r->gen = gen;
    *make_view = r->view_gen[b] != gen;
    r->cons_gen = gen;
    r->cons_y = 0;
    int32_t ahead = lines_ahead(r, r->prod_gen, r->prod_y);
    if (ahead < 0 || ahead >= AHEAD) {
        r->prod_gen = gen;
        r->prod_y = 0;
    }
    return gen;
}

void wc_ring_view_made(wc_ring_t *r, uint32_t gen, uint32_t now_ms)
{
    r->view_gen[gen & 1u] = gen;
    r->view_tick[gen & 1u] = now_ms;
}

int wc_ring_take(const wc_ring_t *r, uint32_t y)
{
    int slot = (int)(y % AHEAD);
    return r->tag[slot] == (r->gen << 8 | y) ? slot : -1;
}

void wc_ring_sent(wc_ring_t *r, uint32_t y)
{
    if (y + 1u < H) {
        r->cons_gen = r->gen;
        r->cons_y = y + 1u;
    } else {
        r->cons_gen = r->gen + 1u;
        r->cons_y = 0;
    }
    if (lines_ahead(r, r->prod_gen, r->prod_y) < 0) {
        r->prod_gen = r->cons_gen;
        r->prod_y = r->cons_y;
    }
}

bool wc_ring_claim(wc_ring_t *r, uint32_t *gen, uint32_t *y, int *slot, bool *make_view)
{
    uint32_t g = r->prod_gen, yy = r->prod_y;
    int32_t ahead = lines_ahead(r, g, yy);
    if (ahead < 0 || ahead >= AHEAD || (g != r->gen && g != r->gen + 1u)) {
        return false;
    }
    if (r->discard_gen == g && r->discards >= WC_RING_MAX_DISCARDS) {
        return false; /* the consumer keeps overtaking: leave this frame to it */
    }
    *gen = g;
    *y = yy;
    *slot = (int)(yy % AHEAD);
    *make_view = r->view_gen[g & 1u] != g;
    r->tag[*slot] = WC_RING_NO_TAG;
    return true;
}

bool wc_ring_commit(wc_ring_t *r, uint32_t gen, uint32_t y)
{
    /* The view check is belt and braces: the view of frame gen is only
     * replaced for frame gen + 2 (or after a skip), by which time the
     * consumer has moved the producer past gen anyway. */
    if (r->prod_gen != gen || r->prod_y != y || r->view_gen[gen & 1u] != gen) {
        if (r->discard_gen != gen) {
            r->discard_gen = gen;
            r->discards = 0;
        }
        r->discards++;
        return false;
    }
    r->tag[y % AHEAD] = gen << 8 | y;
    if (y + 1u < H) {
        r->prod_y = y + 1u;
    } else {
        r->prod_gen = gen + 1u;
        r->prod_y = 0;
    }
    return true;
}
