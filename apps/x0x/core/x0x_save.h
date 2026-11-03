/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef X0X_SAVE_H
#define X0X_SAVE_H
#include <stdint.h>

/*
 * When apps/x0x saves its state (a pure decision, host-tested by
 * test/x0x/x0x_test.c; apps/drone/core/drone_save.h's scheduler, renamed,
 * with a later Shift point: x0x uses Shift for quick combinations, which
 * shouldn't each cost a write). file_write() costs SPI-flash erases, so
 *   - not before X0X_SAVE_QUIET_MS without a change,
 *   - then at most every X0X_SAVE_EVERY_MS after an attempt that
 *     worked, every X0X_SAVE_RETRY_MS after one that didn't (E16_EBUSY:
 *     the erase budget; or an error), counted from the attempt either way,
 *   - and once per Shift press, after it's been held
 *     X0X_SAVE_SHIFT_MS (the launcher leaves at 1 s), whatever the
 *     interval: one attempt, not one per loop pass.
 * Only while there's a change not yet saved.
 */

#define X0X_SAVE_QUIET_MS 2000u
#define X0X_SAVE_EVERY_MS 20000u
#define X0X_SAVE_RETRY_MS 5000u
#define X0X_SAVE_SHIFT_MS 600u

typedef enum { X0X_SAVE_NONE = 0, X0X_SAVE_NOW, X0X_SAVE_LEAVING } x0x_save_do_t;

typedef struct {
    int dirty;          /* a change not saved yet */
    uint32_t change_ms; /* the last change */
    int tried;          /* an attempt has been made */
    uint32_t try_ms;    /* the last attempt */
    int last_ok;        /* how it went */
    int shift_done;     /* this Shift press has had its attempt */
} x0x_save_t;

/* A setting changed at now. */
void x0x_save_changed(x0x_save_t *s, uint32_t now);

/* Should the app save now? shift_held_ms: how long Shift has been held
 * (0 = it isn't). Call x0x_save_result() after every attempt. */
x0x_save_do_t x0x_save_poll(x0x_save_t *s, uint32_t now, uint32_t shift_held_ms);

/* The attempt made at now worked (ok) or not. */
void x0x_save_result(x0x_save_t *s, uint32_t now, int ok);

#endif
