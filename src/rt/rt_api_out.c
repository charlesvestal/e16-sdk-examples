/*
 * Output APIs: midi, leds, slots, page.
 *
 * Argument checking mirrors external/oxi-e16-lua/test/e16mock.lua exactly
 * (the mock is the spec): same ranges, same error text, same quirks (e.g.
 * midi.sendMidi never validates 'out' or the channel; leds.reset never
 * validates its index). system.setUpdateRate lives in rt_api_clock.c.
 */
#include "rt_api_out.h"
#include "rt_api_ring.h"
#include "rt_api_audio.h"
#include "rt_port.h"

#include "lauxlib.h"

#include <math.h>
#include <string.h>

/* integers 0-127 ("data1 out of range: 1.5", etc; the mock requires the
 * *integer* subtype, so 1.5 is rejected even though it's in range). */
static void check_byte(lua_State *L, int idx, const char *what)
{
    luaL_checknumber(L, idx);
    if (!lua_isinteger(L, idx)) {
        luaL_error(L, "%s out of range: %s", what, lua_tostring(L, idx));
    }
    lua_Integer v = lua_tointeger(L, idx);
    if (v < 0 || v > 127) {
        luaL_error(L, "%s out of range: %s", what, lua_tostring(L, idx));
    }
}

/* floats floored (lua_Number -> (int)floorf), then range-checked. This
 * intentionally differs from the mock, which range-checks the raw number
 * (a float ring value like 100.9 passes its asserts unmodified): the device
 * needs an actual integer to drive the LEDs, so flooring is the runtime's
 * job rather than the script's. */
static lua_Integer check_floor_range(lua_State *L, int idx, lua_Integer lo, lua_Integer hi,
                                      const char *what)
{
    lua_Number n = luaL_checknumber(L, idx);
    lua_Integer v = (lua_Integer)floorf((float)n);
    if (v < lo || v > hi) {
        luaL_error(L, "%s %s", what, lua_tostring(L, idx));
    }
    return v;
}

static lua_Integer check_slot_index(lua_State *L, int idx)
{
    lua_Number n = luaL_checknumber(L, idx);
    lua_Integer v = (lua_Integer)floorf((float)n);
    if (v < 1 || v > 16) {
        luaL_error(L, "slot index");
    }
    return v;
}

/* --- midi --- */

static int l_midi_sendMidi(lua_State *L)
{
    lua_Integer out = luaL_checkinteger(L, 1);
    luaL_checknumber(L, 2); /* channel: present, but the mock never validates it here */
    luaL_checknumber(L, 3);
    if (!lua_isinteger(L, 3)) {
        luaL_error(L, "bad status %s", lua_tostring(L, 3));
    }
    lua_Integer st = lua_tointeger(L, 3);
    if (st < 0x80 || st > 0xFF) {
        luaL_error(L, "bad status %s", lua_tostring(L, 3));
    }
    check_byte(L, 4, "data1");
    check_byte(L, 5, "data2");
    uint8_t bytes[3] = {(uint8_t)st, (uint8_t)lua_tointeger(L, 4), (uint8_t)lua_tointeger(L, 5)};
    if (out == 4) {
        rt_api_audio_port4(bytes, 3);
    }
    rt_port_midi((int)out, bytes, 3);
    return 0;
}

static int l_midi_sendCC(lua_State *L)
{
    lua_Integer out = luaL_checkinteger(L, 1);
    lua_Number c = luaL_checknumber(L, 2);
    if (c < 0 || c > 15) {
        luaL_error(L, "channel out of range: %s", lua_tostring(L, 2));
    }
    check_byte(L, 3, "cc");
    check_byte(L, 4, "cc value");
    uint8_t status = (uint8_t)(0xB0 + (int)c);
    uint8_t bytes[3] = {status, (uint8_t)lua_tointeger(L, 3), (uint8_t)lua_tointeger(L, 4)};
    if (out == 4) {
        rt_api_audio_port4(bytes, 3);
    }
    rt_port_midi((int)out, bytes, 3);
    return 0;
}

/* --- leds --- */

static int l_leds_updateByIndex(lua_State *L)
{
    if (lua_istable(L, 1)) {
        lua_Integer n = (lua_Integer)luaL_len(L, 1);
        for (lua_Integer k = 1; k <= n; k++) {
            lua_geti(L, 1, k);
            luaL_argcheck(L, lua_istable(L, -1), 1, "batch entries must be tables");
            lua_geti(L, -1, 1);
            lua_Integer idx = check_floor_range(L, -1, 1, 16, "led index");
            lua_pop(L, 1);
            lua_geti(L, -1, 2);
            lua_Integer val = check_floor_range(L, -1, 0, 16383, "led value");
            lua_pop(L, 1);
            lua_Integer col = 0;
            lua_geti(L, -1, 3);
            if (!lua_isnil(L, -1)) {
                col = check_floor_range(L, -1, 0, 100, "led color");
            }
            lua_pop(L, 1);
            lua_pop(L, 1); /* the entry table */
            /* A ring in ring.*'s custom per-LED mode ignores the old
             * value-based API entirely (rt_port.h's rt_port_led comment). */
            if (!rt_api_ring_is_custom((int)idx)) {
                rt_port_ring((int)idx, (int)val, (int)col);
            }
        }
        return 0;
    }

    lua_Integer idx = check_floor_range(L, 1, 1, 16, "led index");
    lua_Integer val = check_floor_range(L, 2, 0, 16383, "led value");
    lua_Integer col = 0;
    if (!lua_isnoneornil(L, 3)) {
        col = check_floor_range(L, 3, 0, 100, "led color");
    }
    if (!rt_api_ring_is_custom((int)idx)) {
        rt_port_ring((int)idx, (int)val, (int)col);
    }
    return 0;
}

static int l_leds_reset(lua_State *L)
{
    /* The mock never validates the index here (M.rings[i] = nil on
     * anything); mirror that by silently ignoring an out-of-range one.
     * Also releases a custom ring (rt_port.h's rt_port_led comment): the
     * next leds.updateByIndex on it should work again. */
    lua_Integer idx = luaL_checkinteger(L, 1);
    if (idx >= 1 && idx <= 16) {
        rt_api_ring_release((int)idx);
        rt_port_ring((int)idx, -1, 0);
    }
    return 0;
}

/* --- slots --- */

static int l_slots_update(lua_State *L)
{
    lua_Integer idx = check_slot_index(L, 1);
    if (lua_type(L, 2) != LUA_TSTRING) {
        luaL_error(L, "label must be a string");
    }
    size_t len;
    const char *s = lua_tolstring(L, 2, &len);
    if (len > 4) {
        luaL_error(L, "label too long: '%s'", s);
    }
    rt_port_label((int)idx, s);
    return 0;
}

static int l_slots_reset(lua_State *L)
{
    lua_Integer idx = check_slot_index(L, 1);
    rt_port_label((int)idx, NULL);
    return 0;
}

/* --- page --- */

static int l_page_setTitle(lua_State *L)
{
    size_t len;
    const char *s = luaL_checklstring(L, 1, &len);
    if (len > 15) {
        luaL_error(L, "title too long: '%s'", s);
    }
    rt_port_title(s);
    return 0;
}

static int l_page_resetTitle(lua_State *L)
{
    /* The mock's resetTitle is a no-op (it never resets M.title either);
     * match that exactly rather than inventing new behavior. */
    (void)L;
    return 0;
}

void rt_api_out_open(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_midi_sendMidi);
    lua_setfield(L, -2, "sendMidi");
    lua_pushcfunction(L, l_midi_sendCC);
    lua_setfield(L, -2, "sendCC");
    lua_setglobal(L, "midi");

    lua_newtable(L);
    lua_pushcfunction(L, l_leds_updateByIndex);
    lua_setfield(L, -2, "updateByIndex");
    lua_pushcfunction(L, l_leds_reset);
    lua_setfield(L, -2, "reset");
    lua_setglobal(L, "leds");

    lua_newtable(L);
    lua_pushcfunction(L, l_slots_update);
    lua_setfield(L, -2, "update");
    lua_pushcfunction(L, l_slots_reset);
    lua_setfield(L, -2, "reset");
    lua_setglobal(L, "slots");

    lua_newtable(L);
    lua_pushcfunction(L, l_page_setTitle);
    lua_setfield(L, -2, "setTitle");
    lua_pushcfunction(L, l_page_resetTitle);
    lua_setfield(L, -2, "resetTitle");
    lua_setglobal(L, "page");
}
