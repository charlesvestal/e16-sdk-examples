#ifndef PORT_HOST_H
#define PORT_HOST_H
#include <stdbool.h>
#include <stddef.h>

/*
 * Host port: implements rt_port.h by recording everything the runtime sends
 * out in plain C arrays, for test/host/rt_module.c to expose to Lua. Also
 * owns the host-controlled clock that rt_port_now() reports.
 */

typedef struct {
    double t;
    int out, st, d1, d2;
} port_host_msg_t;

typedef struct {
    int v, c;
    int set;
} port_host_ring_t;

/* Clears recorded output (sent/rings/labels/title/log) for a fresh load.
 * The host clock (port_host_now) is left running. */
void port_host_reset(void);

double port_host_now(void);
void port_host_set_now(double ms);

const port_host_msg_t *port_host_sent(size_t *count);
void port_host_clear_sent(void);

const port_host_ring_t *port_host_rings(void); /* array of 16: index 0 = ring 1 */
const char *port_host_label(int index1based);  /* NULL if unset */
const char *port_host_title(void);
const char *port_host_log(size_t *len);         /* last print()/error text */
const char *port_host_log_history(size_t *len); /* every line since the reset */

/* --- draw.* and ring.* --- */

/* The canvas pixel at (x, y) (0-based), or false if out of bounds or
 * beyond the canvas's current height (54, or 64 fullscreen) -- unlike
 * canvas.h's rt_canvas_get(), which reads the raw buffer regardless of
 * height, this is what the screen would actually show. */
bool port_host_canvas_pixel(int x, int y);
bool port_host_canvas_fullscreen(void);
int port_host_canvas_height(void);     /* 54 or 64 */
unsigned port_host_canvas_shows(void); /* draw.show() calls since the reset */

/* One LED on a ring.*-driven ring: r, g, b 0-255, as last written by
 * rt_port_led(). All zero (never written, or ring.clear()'d) if `set` is
 * false too, same shape as port_host_ring_t above. */
typedef struct {
    int r, g, b;
    bool set;
} port_host_led_t;

/* ring 1-16, led 1-16 (both 1-based, matching ring.set()'s own indices). */
const port_host_led_t *port_host_led(int ring, int led);
unsigned port_host_ring_shows(void); /* ring.show() calls since the reset */

/* --- audio.* --- */

/* One rt_port_audio_*() call, in order. op: "enable" (a = 0/1), "note"
 * (a = note, b = velocity), "off", "set" (a = rt_audio_param_t, b = value),
 * "output" (a = rt_audio_out_t) or "release". */
typedef struct {
    const char *op;
    int a, b;
} port_host_audio_t;

const port_host_audio_t *port_host_audio(size_t *count); /* since the reset */
/* What the device would do with TRS 2: true from audio.enable(true) or a
 * note until audio.enable(false) or a release. */
bool port_host_audio_enabled(void);
/* The last value set for each rt_audio_param_t, or -1 if never set. */
int port_host_audio_param(int param);
/* Whether a computer "has the E16 connected over USB", for the AUTO
 * routing (rt_port_audio_routing()); true until set. */
void port_host_set_usb(bool connected);

/* What rt_port_disk_available() (launcher.has("disk")) answers; true
 * until set. */
void port_host_set_disk(bool mounted);

#endif
