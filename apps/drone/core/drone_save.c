/*
 * When drone saves: drone_save.h.
 */
#include "drone_save.h"

void drone_save_changed(drone_save_t *s, uint32_t now)
{
    s->dirty = 1;
    s->change_ms = now;
}

drone_save_do_t drone_save_poll(drone_save_t *s, uint32_t now, uint32_t shift_held_ms)
{
    if (shift_held_ms == 0) {
        s->shift_done = 0;
    }
    if (!s->dirty) {
        return DRONE_SAVE_NONE;
    }
    if (shift_held_ms >= DRONE_SAVE_SHIFT_MS && !s->shift_done) {
        s->shift_done = 1;
        return DRONE_SAVE_LEAVING;
    }
    if (now - s->change_ms < DRONE_SAVE_QUIET_MS) {
        return DRONE_SAVE_NONE;
    }
    if (s->tried && now - s->try_ms < (s->last_ok ? DRONE_SAVE_EVERY_MS : DRONE_SAVE_RETRY_MS)) {
        return DRONE_SAVE_NONE;
    }
    return DRONE_SAVE_NOW;
}

void drone_save_result(drone_save_t *s, uint32_t now, int ok)
{
    s->tried = 1;
    s->try_ms = now;
    s->last_ok = ok;
    if (ok) {
        s->dirty = 0;
    }
}
