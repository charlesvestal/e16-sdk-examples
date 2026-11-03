/* SPDX-License-Identifier: GPL-3.0-only */
/* x0x-lite parameter descriptors (from X0X's x0x_param.h), shared by the sound engines.
 *
 * Every user-facing sound parameter is a "pot": an integer 0..max (max is 127 for a
 * continuous control, n-1 for an n-way switch). The UI draws and edits them from these
 * descriptors alone, and the saved state stores the integers. */
#pragma once
#include <stdint.h>

typedef struct {
    const char *name;                 /* <= 6 characters */
    uint8_t max;                      /* 127 = continuous; else a switch with max+1 positions */
    uint8_t def;                      /* power-on value */
    const char *const *names;         /* switch: max+1 labels (<= 5 chars), else 0 */
} x0x_param_t;
