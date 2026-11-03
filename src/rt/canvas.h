#ifndef RT_CANVAS_H
#define RT_CANVAS_H
#include <stdbool.h>
#include <stdint.h>

/*
 * The script canvas: a 1-bit, 128-wide pixel buffer that draw.* paints
 * into. Shared rasterisation (clear/pixel/line/rect/text and the clipping
 * that goes with them) lives here so the host port (test/host/port_host.c)
 * and the device port (src/port_e16_canvas.c) render identically -- each
 * port just owns one rt_canvas_t and forwards rt_port_draw_*() calls into
 * it.
 *
 * Height is 54 normally (the screen's title bar stays visible above the
 * canvas) or 64 with draw.fullscreen(true) (no title bar); width is
 * always 128. Every drawing call clips silently to the canvas's current
 * height -- a coordinate off either edge, or an edge the shape crosses,
 * is simply not drawn, never an error. rt_canvas_set_fullscreen() doesn't
 * clear pixels already set above row 54: they just aren't visible (or
 * reachable by new drawing) until fullscreen is on again, same as the
 * real buffer never being touched by the toggle.
 *
 * Storage: 16 bytes/row (128 bits) x 64 rows = 1024 bytes, row-major, bit
 * 0 of byte (y*16 + x/8) is the leftmost of that byte's 8 columns.
 */
#define RT_CANVAS_WIDTH 128
#define RT_CANVAS_HEIGHT_FULL 64
#define RT_CANVAS_HEIGHT_NORMAL 54
#define RT_CANVAS_STRIDE (RT_CANVAS_WIDTH / 8)
#define RT_CANVAS_BYTES (RT_CANVAS_STRIDE * RT_CANVAS_HEIGHT_FULL)

/* draw.* clamps every coordinate and size to +-this before it gets here
 * (rt_api_draw.c), so no shape's loop can run long. */
#define RT_CANVAS_COORD_MAX 4096

typedef struct {
    uint8_t bits[RT_CANVAS_BYTES];
    bool fullscreen;
} rt_canvas_t;

void rt_canvas_clear(rt_canvas_t *c);
void rt_canvas_set_fullscreen(rt_canvas_t *c, bool fullscreen);
/* Current clip height: RT_CANVAS_HEIGHT_FULL or _NORMAL. */
int rt_canvas_height(const rt_canvas_t *c);

void rt_canvas_pixel(rt_canvas_t *c, int x, int y, bool on);
void rt_canvas_line(rt_canvas_t *c, int x0, int y0, int x1, int y1, bool on);
void rt_canvas_rect(rt_canvas_t *c, int x, int y, int w, int h, bool fill, bool on);
/* Text in the screen's proportional font (src/ui/font.h): (x, y) is the
 * top-left, glyphs advance by their width plus 1 px; only inked pixels
 * are written. */
void rt_canvas_text(rt_canvas_t *c, int x, int y, const char *s, bool on);

/* The pixel at (x, y), or false if out of the buffer's bounds (not just
 * the current height -- a direct read of what's stored). */
bool rt_canvas_get(const rt_canvas_t *c, int x, int y);

#endif
