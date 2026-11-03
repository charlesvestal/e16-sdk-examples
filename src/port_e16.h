#ifndef PORT_E16_H
#define PORT_E16_H
#include <stdbool.h>
#include <stdint.h>

/*
 * The device side of src/rt/rt_port.h: rings, MIDI out, time, log, the
 * Lua heap. Labels and the title are only stored here; the screen
 * (src/ui/screen.c) reads them back.
 */

#define PORT_E16_COLORS 100

/* port_e16_palette.c. Provisional: see the comment there. */
extern const uint8_t port_e16_palette[PORT_E16_COLORS][3];

/* Lua heap pools (CCM + SRAM), LEDs and encoders. Call once after
 * system_init(), before rt_load(). */
void port_e16_init(void);

/* Push dirty rings to the LEDs, at most once every 10 ms. Call every
 * main-loop pass. */
void port_e16_leds_poll(void);
/* Draw the value rings again (the noise lab's LED switches). */
void port_e16_leds_redraw(void);

/* What the script last set, or NULL while unset/reset (the screen then
 * draws the stock default). Index 1-16. */
const char *port_e16_label(int index);
const char *port_e16_title(void);

/* Changes whenever a label or the title changes (to skip redraws). */
uint32_t port_e16_ui_version(void);

/* Unset every label and the title, and turn all rings off: before a
 * script loads, and when returning to the menu. */
void port_e16_ui_clear(void);

/*
 * The draw.* canvas (port_e16_canvas.c; layout in src/rt/canvas.h). The
 * screen shows it instead of the label grid while it's active: from the
 * script's first drawing call until draw.release() (which the runtime
 * also calls on every load and unload). The version moves on draw.show(),
 * a fullscreen toggle and a release -- the screen redraws then, not on
 * every drawing call.
 */
void port_e16_canvas_init(void);
bool port_e16_canvas_active(void);
bool port_e16_canvas_fullscreen(void);
uint32_t port_e16_canvas_version(void);
const uint8_t *port_e16_canvas_bits(void); /* RT_CANVAS_BYTES, row-major */

#endif
