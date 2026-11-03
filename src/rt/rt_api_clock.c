/*
 * system.setUpdateRate and clock.*: a C port of the mock's clock
 * (oxi-e16-lua/test/e16mock.lua -- clock, cb/safe/flush, pulse, M.run,
 * M.load's resets, M.extStart/extStop/extContinue).
 *
 * Times are rt_time_t ms (float on the host, like the mock under
 * LUA_32BITS; double on the device -- see rt_port.h), and the arithmetic
 * follows the mock's step by step (separate statements, so the host
 * compiler's default FMA contraction can't fuse them), so that pulse and
 * update times come out bit-identical to the mock's in the host tests.
 *
 * Ordering (rt_api_clock_tick): each pass flushes the queued transport
 * callbacks, then fires the earlier of pulse and update (pulse on a tie).
 * rt_tick() runs this before encoder-hold detection.
 */
#include "rt_api_clock.h"
#include "rt.h"
#include "rt_port.h"

#include "lauxlib.h"

#include <stdio.h>
#include <string.h>

#define NEVER RT_TIME_NEVER
#define NOW (-RT_TIME_NEVER)

/* --- system.update scheduling --- */

static int g_rate = 0;               /* ms; 0 = disabled */
static rt_time_t g_last_update = 0;

/* --- clock (the mock's M.tp, tick, nextPulse, at/atTick, ...) --- */

enum { TP_STOPPED = -1, TP_EXTERNAL = 0, TP_LUA = 2 };

/* A tempo that remembers whether Lua gave it as an integer, so getBpm()
 * returns 120 (not 120.0) where the mock does. */
typedef struct {
    float v;
    bool is_int;
} bpm_t;

static int g_tp = TP_STOPPED;        /* running source */
static int32_t g_tick = 0;           /* MIDI ticks (24 per quarter) */
static rt_time_t g_next_pulse = 0;   /* time of tick g_tick (timer mode) */
static rt_time_t g_at = 0;           /* grid anchor: time ... */
static int32_t g_at_tick = 0;        /* ... and tick */
static bpm_t g_bpm = {120.0f, true};
static bpm_t g_ext_bpm = {120.0f, true};
static bool g_listening = false;
static int g_res = 16;

/* External source: tick mode (0xF8 drives ticks) or timer mode. */
static bool g_ext_tick_mode = false;
static uint8_t g_ext_pending = 0;    /* 0xF8 ticks not yet turned into pulses */
static unsigned g_ext_dropped = 0;   /* 0xF8 ticks lost to a full g_ext_pending */

/* 0xF8 tempo estimate. */
#define EST_GAP_MS 500.0f            /* a longer gap restarts the estimate */
#define EST_DIV 16.0f              /* EMA weight 1/16 */
static bool g_est_have_last = false;
static rt_time_t g_est_last = 0;
static bool g_est_have_avg = false;
static float g_est_avg = 0.0f;

/* Queued transport callbacks (the mock's M.q), delivered at the start of
 * the next clock pass. */
enum { CB_START, CB_STOP, CB_CONTINUE };
static const char *const CB_PATH[] = {"clock.onStart", "clock.onStop", "clock.onContinue"};
#define QMAX 16
static struct {
    uint8_t kind, src;
} g_q[QMAX];
static int g_qn = 0;
static unsigned g_q_dropped = 0;     /* callbacks lost to a full queue */

/* Drops are logged once per load (like the first clock callback error),
 * with the count so far, from the next clock pass. */
static bool g_drops_logged = false;

static void cb(int kind, int src)
{
    if (g_qn < QMAX) { /* the mock's queue is unbounded */
        g_q[g_qn].kind = (uint8_t)kind;
        g_q[g_qn].src = (uint8_t)src;
        g_qn++;
    } else {
        g_q_dropped++;
    }
}

static void log_drops(void)
{
    if (g_drops_logged || (g_q_dropped == 0 && g_ext_dropped == 0)) {
        return;
    }
    char buf[96];
    if (g_q_dropped > 0 && g_ext_dropped > 0) {
        snprintf(buf, sizeof buf, "clock: %u transport callbacks and %u MIDI clock ticks dropped\n",
                 g_q_dropped, g_ext_dropped);
    } else if (g_q_dropped > 0) {
        snprintf(buf, sizeof buf, "clock: %u transport callbacks dropped\n", g_q_dropped);
    } else {
        snprintf(buf, sizeof buf, "clock: %u MIDI clock ticks dropped\n", g_ext_dropped);
    }
    rt_port_log(buf, strlen(buf));
    g_drops_logged = true;
}

static void flush(void)
{
    if (g_qn == 0) {
        return;
    }
    /* Take the queue first: callbacks may queue more, for the next pass. */
    int n = g_qn;
    struct {
        uint8_t kind, src;
    } q[QMAX];
    for (int i = 0; i < n; i++) {
        q[i].kind = g_q[i].kind;
        q[i].src = g_q[i].src;
    }
    g_qn = 0;
    for (int i = 0; i < n; i++) {
        lua_State *L = rt_state();
        if (g_listening && L) {
            lua_pushinteger(L, q[i].src);
            rt_call_clock(CB_PATH[q[i].kind], 1);
        }
    }
}

static bool valid_res(lua_State *L, int idx, int *out)
{
    int isnum = 0;
    lua_Integer r = lua_tointegerx(L, idx, &isnum);
    if (isnum && (r == 4 || r == 8 || r == 16 || r == 32 || r == 96)) {
        *out = (int)r;
        return true;
    }
    return false;
}

static float timer_bpm(void)
{
    return g_tp == TP_EXTERNAL ? g_ext_bpm.v : g_bpm.v;
}

static void pulse(void)
{
    int every = 96 / g_res;
    if (g_listening && g_tick % every == 0) {
        int32_t t = g_tick;
        int b = t % 24 == 0 ? 4 : t % 12 == 0 ? 8 : t % 6 == 0 ? 16 : t % 3 == 0 ? 32 : 96;
        lua_State *L = rt_state();
        if (L) {
            lua_pushinteger(L, b);
            lua_pushinteger(L, t / every);
            lua_pushinteger(L, g_tp);
            rt_call_clock("clock.onPulse", 3);
        }
    }
    g_tick++;
    /* From the anchor, so float time doesn't drift. */
    rt_time_t n = (rt_time_t)(g_tick - g_at_tick);
    rt_time_t num = n * (rt_time_t)60000;
    rt_time_t den = (rt_time_t)timer_bpm() * (rt_time_t)24;
    rt_time_t d = num / den;
    g_next_pulse = g_at + d;
}

/* --- scheduling --- */

int rt_api_clock_rate(void)
{
    return g_rate;
}

rt_time_t rt_api_clock_next_due(void)
{
    if (g_rate <= 0) {
        return NEVER;
    }
    rt_time_t t = g_last_update + (rt_time_t)g_rate;
    return t + (rt_time_t)0.1;
}

rt_time_t rt_api_clock_next_pulse(void)
{
    if (g_tp == TP_STOPPED) {
        return NEVER;
    }
    if (g_tp == TP_EXTERNAL && g_ext_tick_mode) {
        return g_ext_pending > 0 ? NOW : NEVER;
    }
    return g_next_pulse;
}

void rt_api_clock_tick(rt_time_t now, bool flush_first)
{
    log_drops();
    for (;;) {
        if (flush_first) {
            flush();
        }
        flush_first = true;
        rt_time_t tu = rt_api_clock_next_due();
        rt_time_t tc = rt_api_clock_next_pulse();
        rt_time_t t = tu < tc ? tu : tc;
        if (t > now) {
            break;
        }
        if (tc <= tu) {
            if (g_tp == TP_EXTERNAL && g_ext_tick_mode) {
                g_ext_pending--;
            }
            pulse();
        } else {
            /* The mock's grid: the next update is due rate + 0.1 after
             * this one was due, not after `now`. If the device fell more
             * than a whole period behind, restart the grid at `now`
             * rather than firing a burst of catch-up updates. */
            rt_time_t next = t + (rt_time_t)g_rate;
            next = next + (rt_time_t)0.1;
            g_last_update = next <= now ? now : t;
            /* A failing (or missing) update disables further ones, like
             * the mock. rt_call() has already logged the error. */
            if (!rt_call("system.update", 0)) {
                g_rate = 0;
            }
        }
    }
}

bool rt_api_clock_listening(void)
{
    return g_listening;
}

int rt_api_clock_res(void)
{
    return g_res;
}

int rt_api_clock_source(void)
{
    return g_tp;
}

float rt_api_clock_internal_bpm(bool *is_int)
{
    if (is_int) {
        *is_int = g_bpm.is_int;
    }
    return g_bpm.v;
}

/* --- external transport --- */

static void ext_begin(rt_time_t now, bool tick_mode)
{
    g_tp = TP_EXTERNAL;
    g_next_pulse = now;
    g_at = now;
    g_ext_tick_mode = tick_mode;
    g_ext_pending = 0;
}

static void ext_start(bool tick_mode)
{
    if (g_tp > 0) {
        return; /* doesn't replace a running internal clock */
    }
    g_tick = 0;
    g_at_tick = 0;
    ext_begin(rt_port_now(), tick_mode);
    cb(CB_START, 0);
}

static void ext_continue(bool tick_mode)
{
    if (g_tp != TP_STOPPED) {
        return;
    }
    g_at_tick = g_tick;
    ext_begin(rt_port_now(), tick_mode);
    cb(CB_CONTINUE, 0);
}

void rt_api_clock_ext_stop(void)
{
    /* Stops any clock, Lua's too; repeats reach the script even when stopped. */
    g_tp = TP_STOPPED;
    g_ext_pending = 0;
    cb(CB_STOP, 0);
}

void rt_api_clock_ext_start(float bpm, bool bpm_is_int, bool have_bpm)
{
    if (have_bpm) {
        g_ext_bpm.v = bpm;
        g_ext_bpm.is_int = bpm_is_int;
    }
    ext_start(false);
}

void rt_api_clock_ext_continue(void)
{
    ext_continue(false);
}

static void estimate_tick(rt_time_t now)
{
    if (g_est_have_last) {
        float iv = (float)(now - g_est_last); /* a duration: float is plenty */
        if (iv > 0.0f && iv < EST_GAP_MS) {
            if (g_est_have_avg) {
                g_est_avg += (iv - g_est_avg) / EST_DIV;
            } else {
                g_est_avg = iv;
                g_est_have_avg = true;
            }
            g_ext_bpm.v = 60000.0f / (24.0f * g_est_avg);
            g_ext_bpm.is_int = false;
        } else if (iv >= EST_GAP_MS) {
            g_est_have_avg = false; /* the sender paused: start over */
        }
    }
    g_est_last = now;
    g_est_have_last = true;
}

void rt_api_clock_realtime(uint8_t byte)
{
    switch (byte) {
        case 0xF8:
            estimate_tick(rt_port_now());
            /* Ticks without Start (or under a Lua clock) make no pulses. */
            if (g_tp == TP_EXTERNAL && g_ext_tick_mode) {
                if (g_ext_pending < 255) {
                    g_ext_pending++;
                } else {
                    g_ext_dropped++; /* main loop stalled ~2.5 bars */
                }
            }
            break;
        case 0xFA:
            ext_start(true); /* tick 0 arrives with the next 0xF8 */
            break;
        case 0xFB:
            ext_continue(true);
            break;
        case 0xFC:
            rt_api_clock_ext_stop();
            break;
        default:
            break;
    }
}

/* --- Lua API --- */

static int l_system_setUpdateRate(lua_State *L)
{
    lua_Number ms = luaL_checknumber(L, 1);
    g_rate = (ms >= 5 && ms <= 1000) ? (int)ms : 0;
    g_last_update = rt_port_now();
    return 0;
}

static int l_listen(lua_State *L)
{
    if (!lua_isboolean(L, 1)) {
        return luaL_error(L, "clock.listen needs a boolean");
    }
    g_listening = lua_toboolean(L, 1);
    if (!valid_res(L, 2, &g_res)) {
        g_res = 16;
    }
    return 0;
}

static int l_startInternal(lua_State *L)
{
    (void)L;
    if (g_tp > 0) {
        return 0; /* already running: unchanged */
    }
    if (g_tp == TP_EXTERNAL) {
        cb(CB_STOP, 0); /* takes over from external transport */
    }
    rt_time_t now = rt_port_now();
    g_tp = TP_LUA;
    g_tick = 0;
    g_next_pulse = now;
    g_at = now;
    g_at_tick = 0;
    g_ext_pending = 0;
    cb(CB_START, TP_LUA);
    return 0;
}

static int l_stopInternal(lua_State *L)
{
    (void)L;
    if (g_tp > 0) {
        g_tp = TP_STOPPED;
        cb(CB_STOP, TP_LUA);
    }
    return 0;
}

static int l_setInternalBpm(lua_State *L)
{
    if (lua_type(L, 1) != LUA_TNUMBER) {
        return luaL_error(L, "setInternalBpm needs a number");
    }
    lua_Number b = lua_tonumber(L, 1);
    if (b >= 20 && b <= 300) {
        if (g_tp > 0) { /* re-anchor the grid */
            g_at = g_next_pulse;
            g_at_tick = g_tick;
        }
        g_bpm.v = (float)b;
        g_bpm.is_int = lua_isinteger(L, 1);
    }
    return 0;
}

static void push_bpm(lua_State *L, bpm_t b)
{
    if (b.is_int) {
        lua_pushinteger(L, (lua_Integer)b.v);
    } else {
        lua_pushnumber(L, b.v);
    }
}

static int l_getBpm(lua_State *L)
{
    if (g_tp == TP_EXTERNAL) {
        push_bpm(L, g_ext_bpm);
    } else if (g_tp == TP_LUA) {
        push_bpm(L, g_bpm);
    } else {
        lua_pushinteger(L, 1);
    }
    return 1;
}

static int l_getPosition(lua_State *L)
{
    int res = 16;
    valid_res(L, 1, &res);
    lua_pushinteger(L, g_tp != TP_STOPPED ? g_tick / (96 / res) : 0);
    return 1;
}

static const luaL_Reg clock_funcs[] = {
    {"listen", l_listen},
    {"startInternal", l_startInternal},
    {"stopInternal", l_stopInternal},
    {"setInternalBpm", l_setInternalBpm},
    {"getBpm", l_getBpm},
    {"getPosition", l_getPosition},
    {NULL, NULL},
};

/* What a load and an unload both reset (the mock's M.load). */
static void script_reset(void)
{
    g_rate = 0;
    g_last_update = rt_port_now();
    g_listening = false;
    g_res = 16;
    g_qn = 0;
    g_q_dropped = 0;
    g_ext_dropped = 0;
    g_drops_logged = false;
    if (g_tp == TP_LUA) {
        g_tp = TP_STOPPED; /* no callback, like the mock */
    }
}

void rt_api_clock_open(lua_State *L)
{
    script_reset();

    lua_newtable(L);
    lua_pushcfunction(L, l_system_setUpdateRate);
    lua_setfield(L, -2, "setUpdateRate");
    lua_setglobal(L, "system");

    luaL_newlib(L, clock_funcs);
    lua_setglobal(L, "clock");
}

void rt_api_clock_close(void)
{
    script_reset();
}

void rt_api_clock_reset(void)
{
    script_reset();
    g_last_update = 0;
    g_tp = TP_STOPPED;
    g_tick = 0;
    g_next_pulse = 0;
    g_at = 0;
    g_at_tick = 0;
    g_bpm = (bpm_t){120.0f, true};
    g_ext_bpm = (bpm_t){120.0f, true};
    g_ext_tick_mode = false;
    g_ext_pending = 0;
    g_est_have_last = false;
    g_est_have_avg = false;
}
