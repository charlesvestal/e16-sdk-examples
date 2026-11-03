/* SPDX-License-Identifier: GPL-3.0-only */
/* x0x-lite's screen drawing (gfx.c). */
#pragma once
#include <stdint.h>

#define GFX_W 128
#define GFX_H 64

void gfx_bind(uint8_t *fb);
void gfx_clear(void);
void gfx_px(int x, int y, int on);            /* on: 0 off, 1 on, 2 invert */
void gfx_fill(int x, int y, int w, int h, int on);
void gfx_frame(int x, int y, int w, int h);
int gfx_text(int x, int y, const char *s, int inv);   /* returns the x after it */
int gfx_text_w(const char *s);
