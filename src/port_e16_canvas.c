/*
 * draw.* on the device: rt_port.h's rt_port_draw_*() over one shared
 * canvas (src/rt/canvas.h), which the screen (src/ui/screen.c) composites
 * under the title bar -- or alone, fullscreen -- in place of the label
 * grid. Text is canvas.c's, in the screen's own proportional font
 * (font.h), the same on the host.
 */
#include "port_e16.h"
#include "rt_port.h"
#include "canvas.h"
#include "ccm.h"

#include <stdbool.h>
#include <stdint.h>

static rt_canvas_t g_canvas CCM_BSS; /* cleared by port_e16_canvas_init() */
static bool g_active;     /* drawn since the load (or the last release) */
static uint32_t g_version;

void port_e16_canvas_init(void)
{
    rt_canvas_clear(&g_canvas);
    rt_canvas_set_fullscreen(&g_canvas, false);
    g_active = false;
    g_version++;
}

void rt_port_draw_clear(void)
{
    rt_canvas_clear(&g_canvas);
    g_active = true;
}

void rt_port_draw_pixel(int x, int y, bool on)
{
    rt_canvas_pixel(&g_canvas, x, y, on);
    g_active = true;
}

void rt_port_draw_line(int x0, int y0, int x1, int y1, bool on)
{
    rt_canvas_line(&g_canvas, x0, y0, x1, y1, on);
    g_active = true;
}

void rt_port_draw_rect(int x, int y, int w, int h, bool fill, bool on)
{
    rt_canvas_rect(&g_canvas, x, y, w, h, fill, on);
    g_active = true;
}

void rt_port_draw_text(int x, int y, const char *text, bool on)
{
    rt_canvas_text(&g_canvas, x, y, text, on);
    g_active = true;
}

void rt_port_draw_show(void)
{
    g_version++;
}

void rt_port_draw_fullscreen(bool on)
{
    if (g_canvas.fullscreen != on) {
        rt_canvas_set_fullscreen(&g_canvas, on);
        g_version++;
    }
}

void rt_port_draw_release(void)
{
    port_e16_canvas_init();
}

int rt_port_draw_height(void)
{
    return rt_canvas_height(&g_canvas);
}

bool port_e16_canvas_active(void)
{
    return g_active;
}

bool port_e16_canvas_fullscreen(void)
{
    return g_canvas.fullscreen;
}

uint32_t port_e16_canvas_version(void)
{
    return g_version;
}

const uint8_t *port_e16_canvas_bits(void)
{
    return g_canvas.bits;
}
