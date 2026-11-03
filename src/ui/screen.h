#ifndef SCREEN_H
#define SCREEN_H
#include <stdbool.h>
#include <stdint.h>
#include "rt_scene.h"

/*
 * The OLED: a 1 KB framebuffer (8 pages x 128 columns, the SSD1306's own
 * layout: bit 0 of fb[page][x] is the top pixel of that 8-row strip) and
 * three screens.
 *
 *   Running  stock-style: the title centered in an inverted bar at the
 *            top, and the 16 labels as a 4x4 grid (rows at y = 16, 28,
 *            40, 52; each slot 32 px wide, text centered as a block, like
 *            the stock firmware). A slot the script never set (or reset)
 *            shows the abbr of the control wired there on the current
 *            page, the stock default. With the page picker open (Shift
 *            tap), the grid shows the page names instead, the current one
 *            inverted. A "!" at the right end of the bar means the script
 *            has had runtime errors since it started (rt_error_count();
 *            the log has the first one per callback in full), and a
 *            speaker at its left end that TRS 2 carries the synth
 *            (port_e16_audio.h). Once the script draws (draw.*), its
 *            128x54 canvas replaces the grid, under the bar; with
 *            draw.fullscreen(true) the canvas is all 128x64 and there's
 *            no bar. It's redrawn on draw.show(), not on every drawing
 *            call.
 *   Menu     the script list (scenes.h) with an inverted selection bar,
 *            scrolling to keep it on screen, markers for disk and broken
 *            scripts, and optionally more entries after the scripts
 *            ("Disk mode", "Webcam").
 *   Error    an inverted "ERROR" bar and the first 3 lines of the message,
 *            wrapped to 21 columns.
 *   Custom   drawn by a callback (webcam mode, src/webcam/webcam.h) with
 *            the screen_draw_*() helpers, whenever screen_poll() redraws:
 *            after screen_touch().
 *
 * Nothing is drawn by the show functions themselves: screen_poll() redraws
 * when something changed, at most every 20 ms, and only sends the 8-row
 * pages whose contents differ from what the display has.
 */
void screen_init(void);           /* clears the display (the bootloader leaves its image) */
void screen_poll(uint32_t now_ms);

/* The menu: scenes.h's first `count` entries (scene_info()), then the
 * n_extra entries of extra (NULL: none). selected runs 0..count +
 * n_extra - 1; count and up are the extra entries. */
void screen_show_menu(int count, const char *const *extra, int n_extra, int selected);
void screen_show_running(const rt_scene_t *scene);
void screen_show_error(const char *message);
void screen_show_page_picker(bool open);
void screen_show_text(const char *title, const char *line); /* e.g. "BOOTLOADER" */
/* A title bar and up to three centered lines (NULL ends them early). */
void screen_show_notice(const char *title, const char *l1, const char *l2, const char *l3);

/* Redraws and sends now, ignoring the 20 ms limit (before a reboot). */
void screen_flush(void);

/* The framebuffer as last sent: 8 pages of 128 bytes. */
const uint8_t *screen_framebuffer(void);

/* The custom screen: draw() runs inside screen_poll()'s redraw, on a
 * cleared framebuffer, and draws with the helpers below; screen_touch()
 * asks for a redraw (at most every 20 ms, like the others). */
typedef void (*screen_draw_fn)(void);
void screen_show_custom(screen_draw_fn draw);
void screen_touch(void);
/* For draw() only: the framebuffer (8 pages of 128 bytes, SSD1306 layout,
 * see the top of this file), text, the title bar, a filled rectangle. */
uint8_t *screen_draw_buffer(void);
void screen_draw_text(int x, int y, const char *s, bool invert);
void screen_draw_title(const char *s);
void screen_draw_rect(int x, int y, int w, int h);

#endif
