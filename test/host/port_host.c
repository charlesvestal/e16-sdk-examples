#include "port_host.h"
#include "canvas.h"
#include "rt_port.h"

#include <stdlib.h>
#include <string.h>

#define MAX_LOG 4096

static double g_now = 0.0;

/* --- draw.* and ring.* --- */

static rt_canvas_t g_canvas;
static unsigned g_canvas_shows;

static port_host_led_t g_leds[16][16]; /* index 0 = ring 1, led 1 */
static unsigned g_ring_shows;

/* audio.* */
#define MAX_AUDIO 256
static port_host_audio_t g_audio[MAX_AUDIO];
static size_t g_audio_n;
static bool g_audio_on;
static int g_audio_param[RT_AUDIO_PARAMS];
static rt_audio_out_t g_audio_out;
static bool g_usb = true; /* "a computer has the E16 connected over USB" */

/* Grows as needed: the oxi-e16-lua suites record minutes of sequencer
 * output between clears. */
static port_host_msg_t *g_sent = NULL;
static size_t g_sent_count = 0;
static size_t g_sent_cap = 0;

static port_host_ring_t g_rings[16];

static char g_labels[16][5];
static int g_label_set[16];

static char g_title[16];

static char g_log[MAX_LOG];
static size_t g_log_len = 0;

/* Every line logged since the last reset, each ending in '\n' (grows as
 * needed), for checks on how often something was logged. */
static char *g_hist = NULL;
static size_t g_hist_len = 0;
static size_t g_hist_cap = 0;

void port_host_reset(void)
{
    g_sent_count = 0;
    memset(g_rings, 0, sizeof g_rings);
    memset(g_labels, 0, sizeof g_labels);
    memset(g_label_set, 0, sizeof g_label_set);
    g_title[0] = '\0';
    g_log[0] = '\0';
    g_log_len = 0;
    g_hist_len = 0;
    rt_canvas_clear(&g_canvas);
    rt_canvas_set_fullscreen(&g_canvas, false);
    g_canvas_shows = 0;
    memset(g_leds, 0, sizeof g_leds);
    g_ring_shows = 0;
    g_audio_n = 0;
    g_audio_on = false;
    g_audio_out = RT_AUDIO_OUT_AUTO;
    for (int i = 0; i < RT_AUDIO_PARAMS; i++) {
        g_audio_param[i] = -1;
    }
}

double port_host_now(void)
{
    return g_now;
}

void port_host_set_now(double ms)
{
    g_now = ms;
}

/* --- rt_port.h --- */

void *rt_port_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    (void)ud;
    (void)osize;
    if (nsize == 0) {
        free(ptr);
        return NULL;
    }
    return realloc(ptr, nsize);
}

rt_time_t rt_port_now(void)
{
    return (rt_time_t)g_now;
}

void rt_port_midi(int out, const uint8_t *bytes, size_t len)
{
    if (len < 3) {
        return;
    }
    if (g_sent_count == g_sent_cap) {
        size_t cap = g_sent_cap ? g_sent_cap * 2 : 1024;
        port_host_msg_t *p = realloc(g_sent, cap * sizeof *p);
        if (!p) {
            return;
        }
        g_sent = p;
        g_sent_cap = cap;
    }
    port_host_msg_t *m = &g_sent[g_sent_count++];
    m->t = g_now;
    m->out = out;
    m->st = bytes[0];
    m->d1 = bytes[1];
    m->d2 = bytes[2];
}

void rt_port_ring(int index, int value, int color)
{
    if (index < 1 || index > 16) {
        return;
    }
    port_host_ring_t *r = &g_rings[index - 1];
    if (value < 0) {
        r->set = 0;
        return;
    }
    r->v = value;
    r->c = color;
    r->set = 1;
}

/* --- draw.* --- */

void rt_port_draw_clear(void)
{
    rt_canvas_clear(&g_canvas);
}

void rt_port_draw_pixel(int x, int y, bool on)
{
    rt_canvas_pixel(&g_canvas, x, y, on);
}

void rt_port_draw_line(int x0, int y0, int x1, int y1, bool on)
{
    rt_canvas_line(&g_canvas, x0, y0, x1, y1, on);
}

void rt_port_draw_rect(int x, int y, int w, int h, bool fill, bool on)
{
    rt_canvas_rect(&g_canvas, x, y, w, h, fill, on);
}

void rt_port_draw_text(int x, int y, const char *text, bool on)
{
    rt_canvas_text(&g_canvas, x, y, text, on);
}

void rt_port_draw_show(void)
{
    g_canvas_shows++;
}

void rt_port_draw_fullscreen(bool on)
{
    rt_canvas_set_fullscreen(&g_canvas, on);
}

void rt_port_draw_release(void)
{
    rt_canvas_clear(&g_canvas);
    rt_canvas_set_fullscreen(&g_canvas, false);
}

int rt_port_draw_height(void)
{
    return rt_canvas_height(&g_canvas);
}

/* --- ring.* --- */

void rt_port_led(int ring, int led, int r, int g, int b)
{
    if (ring < 1 || ring > 16 || led < 1 || led > 16) {
        return;
    }
    port_host_led_t *p = &g_leds[ring - 1][led - 1];
    p->r = r;
    p->g = g;
    p->b = b;
    p->set = true;
}

void rt_port_ring_release(int ring)
{
    if (ring < 1 || ring > 16) {
        return;
    }
    memset(g_leds[ring - 1], 0, sizeof g_leds[0]);
}

void rt_port_ring_show(void)
{
    g_ring_shows++;
}

/* --- audio.* --- */

static void audio_record(const char *op, int a, int b)
{
    if (g_audio_n < MAX_AUDIO) {
        g_audio[g_audio_n].op = op;
        g_audio[g_audio_n].a = a;
        g_audio[g_audio_n].b = b;
        g_audio_n++;
    }
}

void rt_port_audio_enable(bool on)
{
    audio_record("enable", on ? 1 : 0, 0);
    g_audio_on = on;
}

void rt_port_audio_note(int note, int velocity)
{
    audio_record("note", note, velocity);
    g_audio_on = true;
}

void rt_port_audio_off(void)
{
    audio_record("off", 0, 0);
}

void rt_port_audio_set(rt_audio_param_t param, int value)
{
    audio_record("set", (int)param, value);
    if ((int)param >= 0 && param < RT_AUDIO_PARAMS) {
        g_audio_param[param] = value;
    }
}

void rt_port_audio_output(rt_audio_out_t out)
{
    audio_record("output", (int)out, 0);
    g_audio_out = out;
}

/* Like the device: AUTO is USB while USB is connected, else TRS 2. */
rt_audio_out_t rt_port_audio_routing(void)
{
    if (g_audio_out == RT_AUDIO_OUT_AUTO) {
        return g_usb ? RT_AUDIO_OUT_USB : RT_AUDIO_OUT_TRS2;
    }
    return g_audio_out;
}

void port_host_set_usb(bool connected)
{
    g_usb = connected;
}

void rt_port_audio_release(void)
{
    audio_record("release", 0, 0);
    g_audio_on = false;
    g_audio_out = RT_AUDIO_OUT_AUTO;
    for (int i = 0; i < RT_AUDIO_PARAMS; i++) {
        g_audio_param[i] = -1;
    }
}

const port_host_audio_t *port_host_audio(size_t *count)
{
    *count = g_audio_n;
    return g_audio;
}

bool port_host_audio_enabled(void)
{
    return g_audio_on;
}

int port_host_audio_param(int param)
{
    return param >= 0 && param < RT_AUDIO_PARAMS ? g_audio_param[param] : -1;
}

void rt_port_label(int index, const char *text)
{
    if (index < 1 || index > 16) {
        return;
    }
    if (!text) {
        g_label_set[index - 1] = 0;
        g_labels[index - 1][0] = '\0';
        return;
    }
    size_t n = strlen(text);
    if (n > 4) {
        n = 4;
    }
    memcpy(g_labels[index - 1], text, n);
    g_labels[index - 1][n] = '\0';
    g_label_set[index - 1] = 1;
}

void rt_port_title(const char *text)
{
    if (!text) {
        g_title[0] = '\0';
        return;
    }
    size_t n = strlen(text);
    if (n > sizeof g_title - 1) {
        n = sizeof g_title - 1;
    }
    memcpy(g_title, text, n);
    g_title[n] = '\0';
}

void rt_port_log(const char *text, size_t len)
{
    if (len >= sizeof g_log) {
        len = sizeof g_log - 1;
    }
    memcpy(g_log, text, len);
    g_log[len] = '\0';
    g_log_len = len;

    size_t need = g_hist_len + len + 2;
    if (need > g_hist_cap) {
        size_t cap = g_hist_cap ? g_hist_cap * 2 : 4096;
        while (cap < need) {
            cap *= 2;
        }
        char *p = realloc(g_hist, cap);
        if (!p) {
            return;
        }
        g_hist = p;
        g_hist_cap = cap;
    }
    memcpy(g_hist + g_hist_len, text, len);
    g_hist_len += len;
    if (len == 0 || text[len - 1] != '\n') {
        g_hist[g_hist_len++] = '\n';
    }
}

/* --- accessors for rt_module.c --- */

const port_host_msg_t *port_host_sent(size_t *count)
{
    *count = g_sent_count;
    return g_sent;
}

void port_host_clear_sent(void)
{
    g_sent_count = 0;
}

const port_host_ring_t *port_host_rings(void)
{
    return g_rings;
}

const char *port_host_label(int index1based)
{
    if (index1based < 1 || index1based > 16 || !g_label_set[index1based - 1]) {
        return NULL;
    }
    return g_labels[index1based - 1];
}

const char *port_host_title(void)
{
    return g_title;
}

const char *port_host_log_history(size_t *len)
{
    *len = g_hist_len;
    return g_hist ? g_hist : "";
}

const char *port_host_log(size_t *len)
{
    *len = g_log_len;
    return g_log;
}

/* --- draw.* and ring.* accessors --- */

bool port_host_canvas_pixel(int x, int y)
{
    if (y < 0 || y >= rt_canvas_height(&g_canvas)) {
        return false;
    }
    return rt_canvas_get(&g_canvas, x, y);
}

bool port_host_canvas_fullscreen(void)
{
    return g_canvas.fullscreen;
}

int port_host_canvas_height(void)
{
    return rt_canvas_height(&g_canvas);
}

unsigned port_host_canvas_shows(void)
{
    return g_canvas_shows;
}

const port_host_led_t *port_host_led(int ring, int led)
{
    static const port_host_led_t zero;
    if (ring < 1 || ring > 16 || led < 1 || led > 16) {
        return &zero;
    }
    return &g_leds[ring - 1][led - 1];
}

unsigned port_host_ring_shows(void)
{
    return g_ring_shows;
}

/* --- launcher.has("disk") --- */

static bool g_disk = true;

bool rt_port_disk_available(void)
{
    return g_disk;
}

void port_host_set_disk(bool mounted)
{
    g_disk = mounted;
}
