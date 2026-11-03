/*
 * draw.*: argument checking plus rt_port_draw_*() calls; the actual
 * rasterisation and clipping are shared code (src/rt/canvas.c) each port
 * runs on its own canvas, so this file stays thin. Coordinates are
 * integers, floats floored like leds (rt_api_out.c's check_floor_range),
 * but -- unlike leds -- not range errors: an off-canvas coordinate is the
 * canvas's job to clip silently. They are clamped to
 * +-RT_CANVAS_COORD_MAX first (so draw.rect(0, 0, 1e9, 1e9) is a full
 * screen, not a billion-step loop, and the int conversion can't
 * overflow); NaN and infinities are an error.
 */
#include "rt_api_draw.h"
#include "canvas.h"
#include "rt_port.h"

#include "lauxlib.h"

#include <math.h>

static lua_Number check_finite(lua_State *L, int idx)
{
    lua_Number n = luaL_checknumber(L, idx);
    if (isnan(n) || isinf(n)) {
        luaL_argerror(L, idx, "coordinate must be a finite number");
    }
    return n;
}

static lua_Number clamp_coord(lua_Number n)
{
    if (n > (lua_Number)RT_CANVAS_COORD_MAX) {
        return (lua_Number)RT_CANVAS_COORD_MAX;
    }
    if (n < -(lua_Number)RT_CANVAS_COORD_MAX) {
        return -(lua_Number)RT_CANVAS_COORD_MAX;
    }
    return n;
}

static lua_Integer check_coord(lua_State *L, int idx)
{
    return (lua_Integer)floorf((float)clamp_coord(check_finite(L, idx)));
}

/* A rect's size: floored like a coordinate, then cut so the far edge
 * (start + size) lies within the clamp too -- draw.rect(-1e12, 0, 2e12, 1)
 * still crosses the canvas. */
static lua_Integer check_size(lua_State *L, int idx, lua_Integer start)
{
    lua_Number n = check_finite(L, idx);
    if (n > 4 * (lua_Number)RT_CANVAS_COORD_MAX) {
        n = 4 * (lua_Number)RT_CANVAS_COORD_MAX; /* anything past the far clamp */
    } else if (n < 0) {
        n = 0; /* nothing drawn either way */
    }
    lua_Integer size = (lua_Integer)floorf((float)n);
    lua_Integer end = start + size;
    if (end > RT_CANVAS_COORD_MAX) {
        size = RT_CANVAS_COORD_MAX - start;
    }
    return size;
}

/* on/fill flags: optional, default `def`, any value's truthiness (nil and
 * false are "off", everything else including a present-but-odd value is
 * "on") -- same leniency as an ordinary Lua boolean parameter. */
static bool opt_flag(lua_State *L, int idx, bool def)
{
    if (lua_isnoneornil(L, idx)) {
        return def;
    }
    return lua_toboolean(L, idx);
}

static int l_draw_clear(lua_State *L)
{
    (void)L;
    rt_port_draw_clear();
    return 0;
}

static int l_draw_pixel(lua_State *L)
{
    lua_Integer x = check_coord(L, 1);
    lua_Integer y = check_coord(L, 2);
    bool on = opt_flag(L, 3, true);
    rt_port_draw_pixel((int)x, (int)y, on);
    return 0;
}

static int l_draw_line(lua_State *L)
{
    lua_Integer x0 = check_coord(L, 1);
    lua_Integer y0 = check_coord(L, 2);
    lua_Integer x1 = check_coord(L, 3);
    lua_Integer y1 = check_coord(L, 4);
    bool on = opt_flag(L, 5, true);
    rt_port_draw_line((int)x0, (int)y0, (int)x1, (int)y1, on);
    return 0;
}

static int l_draw_rect(lua_State *L)
{
    lua_Integer x = check_coord(L, 1);
    lua_Integer y = check_coord(L, 2);
    lua_Integer w = check_size(L, 3, x);
    lua_Integer h = check_size(L, 4, y);
    bool fill = opt_flag(L, 5, false);
    bool on = opt_flag(L, 6, true);
    rt_port_draw_rect((int)x, (int)y, (int)w, (int)h, fill, on);
    return 0;
}

static int l_draw_text(lua_State *L)
{
    lua_Integer x = check_coord(L, 1);
    lua_Integer y = check_coord(L, 2);
    if (lua_type(L, 3) != LUA_TSTRING) {
        luaL_error(L, "text must be a string");
    }
    const char *s = lua_tostring(L, 3);
    bool on = opt_flag(L, 4, true);
    rt_port_draw_text((int)x, (int)y, s, on);
    return 0;
}

static int l_draw_show(lua_State *L)
{
    (void)L;
    rt_port_draw_show();
    return 0;
}

static int l_draw_fullscreen(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TBOOLEAN);
    rt_port_draw_fullscreen(lua_toboolean(L, 1));
    return 0;
}

static int l_draw_release(lua_State *L)
{
    (void)L;
    rt_port_draw_release();
    return 0;
}

static int l_draw_width(lua_State *L)
{
    lua_pushinteger(L, 128);
    return 1;
}

static int l_draw_height(lua_State *L)
{
    lua_pushinteger(L, rt_port_draw_height());
    return 1;
}

void rt_api_draw_open(lua_State *L)
{
    rt_port_draw_release();

    lua_newtable(L);
    lua_pushcfunction(L, l_draw_clear);
    lua_setfield(L, -2, "clear");
    lua_pushcfunction(L, l_draw_pixel);
    lua_setfield(L, -2, "pixel");
    lua_pushcfunction(L, l_draw_line);
    lua_setfield(L, -2, "line");
    lua_pushcfunction(L, l_draw_rect);
    lua_setfield(L, -2, "rect");
    lua_pushcfunction(L, l_draw_text);
    lua_setfield(L, -2, "text");
    lua_pushcfunction(L, l_draw_show);
    lua_setfield(L, -2, "show");
    lua_pushcfunction(L, l_draw_fullscreen);
    lua_setfield(L, -2, "fullscreen");
    lua_pushcfunction(L, l_draw_release);
    lua_setfield(L, -2, "release");
    lua_pushcfunction(L, l_draw_width);
    lua_setfield(L, -2, "width");
    lua_pushcfunction(L, l_draw_height);
    lua_setfield(L, -2, "height");
    lua_setglobal(L, "draw");
}
