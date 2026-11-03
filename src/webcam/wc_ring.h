#ifndef WC_RING_H
#define WC_RING_H
#include <stdbool.h>
#include <stdint.h>

/*
 * Webcam mode's render-ahead bookkeeping (webcam.c, "Rendering ahead"),
 * portable so it's host-tested (test/webcam/wc_ring_test.c). It only
 * decides; the caller renders lines, makes views and owns the ring's
 * memory.
 *
 * Two contexts use it:
 *   the consumer  USB's interrupt: wc_ring_frame() at each frame's start,
 *                 then for each line wc_ring_take() (a slot to copy from,
 *                 or -1: render the line yourself) and wc_ring_sent().
 *   the producer  a lower-priority interrupt: wc_ring_claim(), render the
 *                 line into the slot, wc_ring_commit().
 * The consumer can preempt the producer, not the other way round. So the
 * producer must call wc_ring_claim() and wc_ring_commit() with the
 * consumer masked (each is short); rendering in between runs unmasked,
 * and wc_ring_commit() then refuses the line if the consumer moved past it
 * or replaced its view meanwhile. The consumer needs no masking.
 *
 * Lines are numbered by frame ("generation", one per wc_ring_frame()) and
 * y. Frame g's view is view[g & 1]: whoever needs it first makes it
 * (*make_view / the claim's need_view) and calls wc_ring_view_made().
 */

#define WC_RING_H_LINES 120   /* lines per frame (WC_H) */
#define WC_RING_AHEAD 12      /* slots; divides the height, so line y's slot is y % WC_RING_AHEAD */
#define WC_RING_NO_TAG 0xFFFFFFFFu
#define WC_RING_STALE_MS 100u /* a view made ahead this long before its frame starts is made again */
#define WC_RING_MAX_DISCARDS 4u /* refused lines per frame before the producer gives that frame up */

typedef struct {
    volatile uint32_t gen;                /* the frame being sent */
    volatile uint32_t cons_gen, cons_y;   /* the line the consumer wants next */
    volatile uint32_t prod_gen, prod_y;   /* the line the producer renders next */
    volatile uint32_t view_gen[2];        /* the frame each view was made for */
    volatile uint32_t view_tick[2];       /* when */
    volatile uint32_t tag[WC_RING_AHEAD]; /* gen << 8 | y of the line in each slot */
    volatile uint32_t discards;           /* lines refused in frame discard_gen */
    volatile uint32_t discard_gen;
} wc_ring_t;

/* Before the first frame (no frame yet; the first is generation 1). */
void wc_ring_reset(wc_ring_t *r);

/* Consumer: a frame starts at time now_ms. Returns its generation; if
 * *make_view, make view[gen & 1] now, then wc_ring_view_made(). A view
 * made ahead more than WC_RING_STALE_MS ago is skipped with its frame's
 * lines (a generation is skipped). */
uint32_t wc_ring_frame(wc_ring_t *r, uint32_t now_ms, bool *make_view);

/* Whoever made view[gen & 1] for frame gen, at now_ms. */
void wc_ring_view_made(wc_ring_t *r, uint32_t gen, uint32_t now_ms);

/* Consumer: the slot holding line y of the current frame, or -1 (render it yourself). */
int wc_ring_take(const wc_ring_t *r, uint32_t y);

/* Consumer: line y of the current frame went out. */
void wc_ring_sent(wc_ring_t *r, uint32_t y);

/* Producer, consumer masked: the next line to render. False: nothing to
 * do (the ring is full, or the producer is past the current and the next
 * frame, or gave this frame up). Else *gen, *y, *slot; if *make_view,
 * make view[*gen & 1] (still masked) and call wc_ring_view_made(). The
 * slot's old tag is cleared. */
bool wc_ring_claim(wc_ring_t *r, uint32_t *gen, uint32_t *y, int *slot, bool *make_view);

/* Producer, consumer masked: the line claimed is rendered. True if it
 * counts (its slot is tagged and the producer moves on). */
bool wc_ring_commit(wc_ring_t *r, uint32_t gen, uint32_t y);

#endif
