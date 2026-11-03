/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * When x0x saves: x0x_save.h. (apps/drone/core/drone_save.c, renamed.)
 */
#include "x0x_save.h"

void x0x_save_changed(x0x_save_t *s, uint32_t now)
{
    s->dirty = 1;
    s->change_ms = now;
}

x0x_save_do_t x0x_save_poll(x0x_save_t *s, uint32_t now, uint32_t shift_held_ms)
{
    if (shift_held_ms == 0) {
        s->shift_done = 0;
    }
    if (!s->dirty) {
        return X0X_SAVE_NONE;
    }
    if (shift_held_ms >= X0X_SAVE_SHIFT_MS && !s->shift_done) {
        s->shift_done = 1;
        return X0X_SAVE_LEAVING;
    }
    if (now - s->change_ms < X0X_SAVE_QUIET_MS) {
        return X0X_SAVE_NONE;
    }
    if (s->tried && now - s->try_ms < (s->last_ok ? X0X_SAVE_EVERY_MS : X0X_SAVE_RETRY_MS)) {
        return X0X_SAVE_NONE;
    }
    return X0X_SAVE_NOW;
}

void x0x_save_result(x0x_save_t *s, uint32_t now, int ok)
{
    s->tried = 1;
    s->try_ms = now;
    s->last_ok = ok;
    if (ok) {
        s->dirty = 0;
    }
}
