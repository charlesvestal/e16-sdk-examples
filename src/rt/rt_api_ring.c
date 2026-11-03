/*
 * ring.*: per-LED colour on one of the 16 rings (see rt_port.h's
 * rt_port_led/rt_port_ring_release/rt_port_ring_show comment for the
 * design). This module is the only thing that knows which rings are in
 * "custom" per-LED mode -- rt_api_out.c's leds.updateByIndex and
 * leds.reset ask rt_api_ring_is_custom()/call rt_api_ring_release() so a
 * custom ring ignores the old value-based API until it's released, and
 * ports only ever render what they're told, never decide a mode.
 */
#include "rt_api_ring.h"
#include "rt_port.h"

#include "lauxlib.h"

#include <math.h>
#include <string.h>

static bool g_custom[16];

/* Same floor-then-range-check style as rt_api_out.c's check_floor_range
 * (not shared: that one is static to its own file). */
static lua_Integer check_range(lua_State *L, int idx, lua_Integer lo, lua_Integer hi,
                                const char *what)
{
    lua_Number n = luaL_checknumber(L, idx);
    lua_Integer v = (lua_Integer)floorf((float)n);
    if (v < lo || v > hi) {
        luaL_error(L, "%s out of range: %s", what, lua_tostring(L, idx));
    }
    return v;
}

static lua_Integer check_ring(lua_State *L, int idx)
{
    return check_range(L, idx, 1, 16, "ring index");
}

static lua_Integer check_led(lua_State *L, int idx)
{
    return check_range(L, idx, 1, 16, "led index");
}

static lua_Integer check_channel(lua_State *L, int idx, const char *what)
{
    return check_range(L, idx, 0, 255, what);
}

static int l_ring_set(lua_State *L)
{
    lua_Integer ring = check_ring(L, 1);
    lua_Integer led = check_led(L, 2);
    lua_Integer r = check_channel(L, 3, "red");
    lua_Integer g = check_channel(L, 4, "green");
    lua_Integer b = check_channel(L, 5, "blue");
    g_custom[ring - 1] = true;
    rt_port_led((int)ring, (int)led, (int)r, (int)g, (int)b);
    return 0;
}

static int l_ring_fill(lua_State *L)
{
    lua_Integer ring = check_ring(L, 1);
    lua_Integer r = check_channel(L, 2, "red");
    lua_Integer g = check_channel(L, 3, "green");
    lua_Integer b = check_channel(L, 4, "blue");
    g_custom[ring - 1] = true;
    for (int led = 1; led <= 16; led++) {
        rt_port_led((int)ring, led, (int)r, (int)g, (int)b);
    }
    return 0;
}

static int l_ring_clear(lua_State *L)
{
    lua_Integer ring = check_ring(L, 1);
    g_custom[ring - 1] = true;
    for (int led = 1; led <= 16; led++) {
        rt_port_led((int)ring, led, 0, 0, 0);
    }
    return 0;
}

static int l_ring_release(lua_State *L)
{
    lua_Integer ring = check_ring(L, 1);
    rt_api_ring_release((int)ring);
    return 0;
}

static int l_ring_show(lua_State *L)
{
    (void)L;
    rt_port_ring_show();
    return 0;
}

bool rt_api_ring_is_custom(int ring)
{
    if (ring < 1 || ring > 16) {
        return false;
    }
    return g_custom[ring - 1];
}

void rt_api_ring_release(int ring)
{
    if (ring < 1 || ring > 16) {
        return;
    }
    if (!g_custom[ring - 1]) {
        return;
    }
    g_custom[ring - 1] = false;
    rt_port_ring_release(ring);
}

void rt_api_ring_close(void)
{
    for (int ring = 1; ring <= 16; ring++) {
        rt_api_ring_release(ring);
    }
}

void rt_api_ring_open(lua_State *L)
{
    memset(g_custom, 0, sizeof g_custom);

    lua_newtable(L);
    lua_pushcfunction(L, l_ring_set);
    lua_setfield(L, -2, "set");
    lua_pushcfunction(L, l_ring_fill);
    lua_setfield(L, -2, "fill");
    lua_pushcfunction(L, l_ring_clear);
    lua_setfield(L, -2, "clear");
    lua_pushcfunction(L, l_ring_release);
    lua_setfield(L, -2, "release");
    lua_pushcfunction(L, l_ring_show);
    lua_setfield(L, -2, "show");
    lua_setglobal(L, "ring");
}
