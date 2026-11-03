/*
 * audio.*: argument checking plus rt_port_audio_*() calls (rt_api_audio.h
 * has the API). The synth itself, and what "enabled" does to TRS 2, are
 * the port's.
 */
#include "rt_api_audio.h"
#include "rt_port.h"

#include "lauxlib.h"

#include <math.h>
#include <string.h>

/* Floor, then range-check, like rt_api_ring.c's check_range. */
static lua_Integer check_range(lua_State *L, int idx, lua_Integer lo, lua_Integer hi, const char *what)
{
    lua_Number n = luaL_checknumber(L, idx);
    lua_Integer v = (lua_Integer)floorf((float)n);
    if (v < lo || v > hi) {
        luaL_error(L, "%s out of range: %s", what, lua_tostring(L, idx));
    }
    return v;
}

static const char *const PARAM_NAMES[RT_AUDIO_PARAMS] = {
    [RT_AUDIO_CUTOFF] = "cutoff",     [RT_AUDIO_RESONANCE] = "resonance",
    [RT_AUDIO_ENVMOD] = "envmod",     [RT_AUDIO_DECAY] = "decay",
    [RT_AUDIO_WAVEFORM] = "waveform", [RT_AUDIO_VOLUME] = "volume",
    [RT_AUDIO_GLIDE] = "glide",       [RT_AUDIO_SLIDE] = "slide",
};

static int l_audio_enable(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TBOOLEAN);
    rt_port_audio_enable(lua_toboolean(L, 1));
    return 0;
}

static int l_audio_note(lua_State *L)
{
    lua_Integer note = check_range(L, 1, 0, 127, "note");
    lua_Integer vel = check_range(L, 2, 0, 127, "velocity");
    if (!lua_isnoneornil(L, 3)) {
        rt_port_audio_set(RT_AUDIO_SLIDE, lua_toboolean(L, 3) ? 127 : 0);
    }
    rt_port_audio_note((int)note, (int)vel);
    return 0;
}

static int l_audio_off(lua_State *L)
{
    (void)L;
    rt_port_audio_off();
    return 0;
}

static int l_audio_set(lua_State *L)
{
    if (lua_type(L, 1) != LUA_TSTRING) {
        luaL_error(L, "audio parameter name must be a string");
    }
    const char *name = lua_tostring(L, 1);
    int param = -1;
    for (int i = 0; i < RT_AUDIO_PARAMS; i++) {
        if (strcmp(name, PARAM_NAMES[i]) == 0) {
            param = i;
            break;
        }
    }
    if (param < 0) {
        luaL_error(L, "unknown audio parameter: %s", name);
    }
    lua_Integer v = check_range(L, 2, 0, 127, "audio value");
    rt_port_audio_set((rt_audio_param_t)param, (int)v);
    return 0;
}

static const char *const OUT_NAMES[] = {
    [RT_AUDIO_OUT_AUTO] = "auto",
    [RT_AUDIO_OUT_USB] = "usb",
    [RT_AUDIO_OUT_TRS2] = "trs2",
    [RT_AUDIO_OUT_BOTH] = "both",
};
#define NUM_OUTS (sizeof OUT_NAMES / sizeof OUT_NAMES[0])

/* audio.output([where]): sets the routing if given; returns where the
 * synth plays now ("usb", "trs2" or "both"; "auto" resolved). */
static int l_audio_output(lua_State *L)
{
    if (!lua_isnoneornil(L, 1)) {
        if (lua_type(L, 1) != LUA_TSTRING) {
            luaL_error(L, "audio output must be a string");
        }
        const char *name = lua_tostring(L, 1);
        size_t i = 0;
        while (i < NUM_OUTS && strcmp(name, OUT_NAMES[i]) != 0) {
            i++;
        }
        if (i == NUM_OUTS) {
            luaL_error(L, "unknown audio output: %s (usb, trs2, both or auto)", name);
        }
        rt_port_audio_output((rt_audio_out_t)i);
    }
    rt_audio_out_t now = rt_port_audio_routing();
    lua_pushstring(L, (unsigned)now < NUM_OUTS ? OUT_NAMES[now] : "auto");
    return 1;
}

/* ---- port 4: notes left held ---- */

/*
 * MIDI out port 4 is the synth too (rt_port.h). A script that sends a note
 * on there and then moves its output elsewhere (a setting changed while a
 * note was down) never sends the note off, and the voice would hold it
 * forever. So the notes held through port 4 are tracked here, and released
 * (all notes off) once port 4 has had no MIDI for PORT4_IDLE_MS while one
 * is held, or at once on CC 123 / 120 sent there. A note that's meant to
 * ring longer than that with nothing else sent to port 4 is cut short:
 * audio.note() (which this doesn't watch) is the way to hold a drone.
 */
#define PORT4_IDLE_MS 2000

static uint32_t g_held[4]; /* notes 0-127 held through port 4 */
static bool g_any_held;
static rt_time_t g_port4_t; /* the last message sent to port 4 */

static void forget_held(void)
{
    memset(g_held, 0, sizeof g_held);
    g_any_held = false;
}

void rt_api_audio_port4(const uint8_t *bytes, size_t len)
{
    g_port4_t = rt_port_now();
    if (len < 3) {
        return;
    }
    uint8_t st = bytes[0] & 0xF0, d1 = bytes[1] & 0x7F, d2 = bytes[2] & 0x7F;
    uint32_t bit = 1u << (d1 & 31u);
    if (st == 0x90 && d2 > 0) {
        g_held[d1 >> 5] |= bit;
        g_any_held = true;
    } else if (st == 0x80 || st == 0x90) {
        g_held[d1 >> 5] &= ~bit;
        g_any_held = (g_held[0] | g_held[1] | g_held[2] | g_held[3]) != 0;
    } else if (st == 0xB0 && (d1 == 123 || d1 == 120)) {
        forget_held();
        rt_port_audio_off();
    }
}

void rt_api_audio_tick(rt_time_t now)
{
    if (g_any_held && now - g_port4_t >= (rt_time_t)PORT4_IDLE_MS) {
        forget_held();
        rt_port_audio_off();
        static const char msg[] = "audio: no MIDI to port 4 for 2 s with a note held: all notes off";
        rt_port_log(msg, sizeof msg - 1);
    }
}

void rt_api_audio_close(void)
{
    forget_held();
    rt_port_audio_release();
}

void rt_api_audio_open(lua_State *L)
{
    forget_held();
    lua_newtable(L);
    lua_pushcfunction(L, l_audio_enable);
    lua_setfield(L, -2, "enable");
    lua_pushcfunction(L, l_audio_note);
    lua_setfield(L, -2, "note");
    lua_pushcfunction(L, l_audio_off);
    lua_setfield(L, -2, "off");
    lua_pushcfunction(L, l_audio_set);
    lua_setfield(L, -2, "set");
    lua_pushcfunction(L, l_audio_output);
    lua_setfield(L, -2, "output");
    lua_setglobal(L, "audio");
}
