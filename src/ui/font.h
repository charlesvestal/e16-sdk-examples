#ifndef FONT_H
#define FONT_H
#include <stdint.h>

/*
 * The screen font: 8 pixels high (bit 0 = top row; descenders use bit 7),
 * up to 7 columns wide. ASCII 32-126, plus the stock firmware's icons at
 * codes 128-153 (external/oxi-e16-lua/docs/e16-lua-notes.md, "Labels and
 * title"): 128 -> 129 <- 130 up 131 down 132 no-entry 133 play 134 pause
 * 135 stop 136 fast-forward 137 rewind 138 skip-back 139 skip-forward
 * 140 small right 141 small left 142 lock 143 unlock 144 top-left corner
 * 145 top-right corner 146 blank (zero width) 147 small * 148 ' 149 "
 * 150 degree 151 tall double bar 152 lock (alt) 153 die. Everything else
 * (control codes, 127, 154-255) draws nothing and has zero width, like
 * the stock firmware.
 *
 * Proportional: a glyph's width is its inked columns (a space is 3), and
 * text advances by width + 1. '#' and 135 are both 5 wide.
 */

#define FONT_HEIGHT 8
#define FONT_GAP 1

/* Inked columns of glyph c (*cols points at the first); 0 = draws nothing.
 * A space returns 3 with *cols pointing at blank columns. */
int font_glyph(uint8_t c, const uint8_t **cols);

/* Pixel width of s drawn proportionally (gaps between glyphs, none after
 * the last), at most n bytes of it. */
int font_text_width(const char *s, int n);

#endif
