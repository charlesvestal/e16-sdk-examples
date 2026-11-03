/*
 * The script canvas's shared rasterisation: see canvas.h. Plain pixel
 * buffer ops, no Lua, no port -- rt_api_draw.c only checks arguments and
 * calls rt_port_draw_*(); each port owns one of these and does the actual
 * drawing by calling into here, with the screen's own font (src/ui/font.c,
 * also built into the host), so a host script and a device script draw
 * identically.
 *
 * Every shape is clipped to the canvas before its loop, so the work is
 * bounded by what's visible (a line by its length, at most a few
 * thousand steps after rt_api_draw.c's clamping), never by how far off
 * the canvas a coordinate is.
 */
#include "canvas.h"
#include "font.h"
#include <string.h>

void rt_canvas_clear(rt_canvas_t *c)
{
    memset(c->bits, 0, sizeof c->bits);
}

void rt_canvas_set_fullscreen(rt_canvas_t *c, bool fullscreen)
{
    c->fullscreen = fullscreen;
}

int rt_canvas_height(const rt_canvas_t *c)
{
    return c->fullscreen ? RT_CANVAS_HEIGHT_FULL : RT_CANVAS_HEIGHT_NORMAL;
}

bool rt_canvas_get(const rt_canvas_t *c, int x, int y)
{
    if (x < 0 || x >= RT_CANVAS_WIDTH || y < 0 || y >= RT_CANVAS_HEIGHT_FULL) {
        return false;
    }
    uint8_t byte = c->bits[y * RT_CANVAS_STRIDE + x / 8];
    return (byte & (uint8_t)(1u << (x % 8))) != 0;
}

void rt_canvas_pixel(rt_canvas_t *c, int x, int y, bool on)
{
    if (x < 0 || x >= RT_CANVAS_WIDTH || y < 0 || y >= rt_canvas_height(c)) {
        return;
    }
    uint8_t *byte = &c->bits[y * RT_CANVAS_STRIDE + x / 8];
    uint8_t mask = (uint8_t)(1u << (x % 8));
    if (on) {
        *byte |= mask;
    } else {
        *byte &= (uint8_t)~mask;
    }
}

/* Bresenham, clipped per point by rt_canvas_pixel(). A line entirely off
 * one side of the canvas is skipped without stepping along it; otherwise
 * the steps are its length, which callers bound (rt_api_draw.c clamps
 * coordinates to +-RT_CANVAS_COORD_MAX), so this stays exact -- the same
 * pixels as the unclipped line -- and fast. */
void rt_canvas_line(rt_canvas_t *c, int x0, int y0, int x1, int y1, bool on)
{
    int h = rt_canvas_height(c);
    if ((x0 < 0 && x1 < 0) || (x0 >= RT_CANVAS_WIDTH && x1 >= RT_CANVAS_WIDTH) || (y0 < 0 && y1 < 0) ||
        (y0 >= h && y1 >= h)) {
        return;
    }
    long long ldx = (long long)x1 - x0, ldy = (long long)y1 - y0;
    long long ladx = ldx < 0 ? -ldx : ldx, lady = ldy < 0 ? -ldy : ldy;
    if (ladx > 4 * RT_CANVAS_COORD_MAX || lady > 4 * RT_CANVAS_COORD_MAX) {
        return; /* only reachable by a caller that doesn't clamp: refuse rather than spin */
    }
    int adx = (int)ladx, ady = (int)lady;
    int sx = ldx < 0 ? -1 : 1;
    int sy = ldy < 0 ? -1 : 1;
    int x = x0, y = y0;

    if (adx >= ady) {
        int err = adx / 2;
        for (int i = 0; i <= adx; i++) {
            rt_canvas_pixel(c, x, y, on);
            err -= ady;
            if (err < 0) {
                y += sy;
                err += adx;
            }
            x += sx;
        }
    } else {
        int err = ady / 2;
        for (int i = 0; i <= ady; i++) {
            rt_canvas_pixel(c, x, y, on);
            err -= adx;
            if (err < 0) {
                x += sx;
                err += ady;
            }
            y += sy;
        }
    }
}

static int clamp_int(long long v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : (int)v;
}

void rt_canvas_rect(rt_canvas_t *c, int x, int y, int w, int h, bool fill, bool on)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    int ch = rt_canvas_height(c);
    long long xe = (long long)x + w - 1, ye = (long long)y + h - 1; /* inclusive far edges */
    if (xe < 0 || ye < 0 || x >= RT_CANVAS_WIDTH || y >= ch) {
        return;
    }
    /* The visible part; an edge outside it isn't drawn. */
    int cx0 = clamp_int(x, 0, RT_CANVAS_WIDTH - 1), cx1 = clamp_int(xe, 0, RT_CANVAS_WIDTH - 1);
    int cy0 = clamp_int(y, 0, ch - 1), cy1 = clamp_int(ye, 0, ch - 1);
    if (fill) {
        for (int row = cy0; row <= cy1; row++) {
            for (int col = cx0; col <= cx1; col++) {
                rt_canvas_pixel(c, col, row, on);
            }
        }
        return;
    }
    for (int col = cx0; col <= cx1; col++) {
        if (y >= 0) {
            rt_canvas_pixel(c, col, y, on);
        }
        if (ye < ch) {
            rt_canvas_pixel(c, col, (int)ye, on);
        }
    }
    for (int row = cy0; row <= cy1; row++) {
        if (x >= 0) {
            rt_canvas_pixel(c, x, row, on);
        }
        if (xe < RT_CANVAS_WIDTH) {
            rt_canvas_pixel(c, (int)xe, row, on);
        }
    }
}

/* (x, y) is the glyphs' top-left; each advances by its width plus a 1 px
 * gap, like the screen's labels. Only inked pixels are written (on or
 * off): the background is left as it was. */
void rt_canvas_text(rt_canvas_t *c, int x, int y, const char *s, bool on)
{
    bool any = false;
    for (; *s && x < RT_CANVAS_WIDTH; s++) {
        const uint8_t *cols;
        int gw = font_glyph((uint8_t)*s, &cols);
        if (gw <= 0) {
            continue;
        }
        if (any) {
            x += FONT_GAP;
        }
        for (int col = 0; col < gw; col++) {
            for (int r = 0; r < FONT_HEIGHT; r++) {
                if (cols[col] & (1u << r)) {
                    rt_canvas_pixel(c, x + col, y + r, on);
                }
            }
        }
        x += gw;
        any = true;
    }
}
