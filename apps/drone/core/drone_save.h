#ifndef DRONE_SAVE_H
#define DRONE_SAVE_H
#include <stdint.h>

/*
 * When apps/drone saves its settings (a pure decision, host-tested by
 * test/drone/drone_test.c): file_write() costs SPI-flash erases, so
 *   - not before DRONE_SAVE_QUIET_MS without a change,
 *   - then at most every DRONE_SAVE_EVERY_MS after an attempt that
 *     worked, every DRONE_SAVE_RETRY_MS after one that didn't (E16_EBUSY:
 *     the erase budget; or an error), counted from the attempt either way,
 *   - and once per Shift press, after it's been held
 *     DRONE_SAVE_SHIFT_MS (the app passes it when the user confirmed the launcher's leave prompt), whatever the
 *     interval: one attempt, not one per loop pass.
 * Only while there's a change not yet saved.
 */

#define DRONE_SAVE_QUIET_MS 2000u
#define DRONE_SAVE_EVERY_MS 20000u
#define DRONE_SAVE_RETRY_MS 5000u
#define DRONE_SAVE_SHIFT_MS 400u

typedef enum { DRONE_SAVE_NONE = 0, DRONE_SAVE_NOW, DRONE_SAVE_LEAVING } drone_save_do_t;

typedef struct {
    int dirty;          /* a change not saved yet */
    uint32_t change_ms; /* the last change */
    int tried;          /* an attempt has been made */
    uint32_t try_ms;    /* the last attempt */
    int last_ok;        /* how it went */
    int shift_done;     /* this Shift press has had its attempt */
} drone_save_t;

/* A setting changed at now. */
void drone_save_changed(drone_save_t *s, uint32_t now);

/* Should the app save now? shift_held_ms: how long Shift has been held
 * (0 = it isn't). Call drone_save_result() after every attempt. */
drone_save_do_t drone_save_poll(drone_save_t *s, uint32_t now, uint32_t shift_held_ms);

/* The attempt made at now worked (ok) or not. */
void drone_save_result(drone_save_t *s, uint32_t now, int ok);

#endif
