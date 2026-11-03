/*
 * MIDI input: midi.listen and the queue behind midi.onNote/onCC/onMessage
 * (rt_api_midiin.h has the API and the delivery model).
 */
#include "rt_api_midiin.h"
#include "rt.h"

#include "lauxlib.h"

#include <stdio.h>
#include <string.h>

#if (RT_MIDI_IN_QUEUE & (RT_MIDI_IN_QUEUE - 1)) != 0
#error "RT_MIDI_IN_QUEUE must be a power of two"
#endif

#define DROP_LOG_MS 1000

enum { KIND_NONE, KIND_NOTE, KIND_CC, KIND_OTHER };

/* The filter midi.listen set. */
static bool g_listening = false;
static bool g_notes, g_cc, g_other;
static int g_channel = -1; /* 0-15, -1 = all */
static int g_port = 0;     /* RT_MIDI_IN_*, 0 = both */

typedef struct {
    uint8_t port, status, d1, d2;
} msg_t;

/* Single producer (push), single consumer (tick): the producer writes
 * only g_head and g_dropped, the consumer only g_tail. Free-running
 * counters, masked on access. The ring itself is a userdata in the
 * script's Lua heap, made by the first midi.listen and anchored in the
 * registry until the state closes: a script that never listens costs
 * nothing, and the launcher's static RAM (the stack's share) doesn't
 * grow. NULL while there's none; nothing is queued then. */
#define REG_KEY "rt.midiin.queue"
static msg_t *g_q;
static volatile uint32_t g_head, g_tail;
static volatile unsigned g_dropped;
static unsigned g_dropped_logged;
static rt_time_t g_drop_log_t;

/* What a message is, if the current filter wants it. */
static int classify(int port, uint8_t status)
{
    if (!g_listening || (g_port != 0 && port != g_port)) {
        return KIND_NONE;
    }
    if (g_channel >= 0 && (status & 0x0F) != g_channel) {
        return KIND_NONE;
    }
    switch (status & 0xF0) {
        case 0x80:
        case 0x90:
            return g_notes ? KIND_NOTE : KIND_NONE;
        case 0xB0:
            return g_cc ? KIND_CC : KIND_NONE;
        default:
            return g_other ? KIND_OTHER : KIND_NONE;
    }
}

void rt_api_midiin_push(int port, const uint8_t *bytes, size_t len)
{
    if (len < 2 || bytes[0] < 0x80 || bytes[0] >= 0xF0) {
        return; /* not a channel voice message */
    }
    uint8_t type = bytes[0] & 0xF0;
    size_t need = (type == 0xC0 || type == 0xD0) ? 2 : 3;
    if (len < need || classify(port, bytes[0]) == KIND_NONE) {
        return;
    }
    if (!g_q || !rt_state()) {
        return;
    }
    uint32_t head = g_head;
    if (head - g_tail >= RT_MIDI_IN_QUEUE) {
        g_dropped++;
        return;
    }
    g_q[head & (RT_MIDI_IN_QUEUE - 1)] =
        (msg_t){(uint8_t)port, bytes[0], (uint8_t)(bytes[1] & 0x7F), need == 3 ? (uint8_t)(bytes[2] & 0x7F) : 0};
    g_head = head + 1;
}

static void log_drops(void)
{
    unsigned d = g_dropped;
    if (d == g_dropped_logged) {
        return;
    }
    char buf[80];
    snprintf(buf, sizeof buf, "midi in: %u messages dropped (queue full), %u since load\n", d - g_dropped_logged,
             d);
    rt_port_log(buf, strlen(buf));
    g_dropped_logged = d;
}

void rt_api_midiin_tick(rt_time_t now)
{
    /* A callback can't unload the script, so L and the queue stay valid
     * through the loop. */
    lua_State *L = rt_state();
    for (int n = 0; L && g_q && n < RT_MIDI_IN_PER_TICK && g_tail != g_head; n++) {
        msg_t m = g_q[g_tail & (RT_MIDI_IN_QUEUE - 1)];
        g_tail = g_tail + 1;
        /* Checked again here: a callback may have changed the filter (or
         * stopped listening) since this was queued. */
        int kind = classify(m.port, m.status);
        if (kind == KIND_NONE) {
            continue;
        }
        const char *path;
        lua_pushinteger(L, m.port);
        if (kind == KIND_OTHER) {
            path = "midi.onMessage";
            lua_pushinteger(L, m.status);
        } else {
            path = kind == KIND_NOTE ? "midi.onNote" : "midi.onCC";
            lua_pushinteger(L, m.status & 0x0F);
        }
        lua_pushinteger(L, m.d1);
        lua_pushinteger(L, (m.status & 0xF0) == 0x80 ? 0 : m.d2);
        unsigned errors = rt_error_count();
        rt_call(path, 4);
        if (rt_error_count() != errors) {
            /* As on the stock firmware: an error stops listening. */
            char buf[80];
            snprintf(buf, sizeof buf, "midi in: listening stopped after an error in %s\n", path);
            rt_port_log(buf, strlen(buf));
            g_listening = false;
        }
    }
    /* The first drops since the load are logged right away, later ones
     * at most once a second. */
    if (g_dropped != g_dropped_logged && (g_dropped_logged == 0 || now - g_drop_log_t >= (rt_time_t)DROP_LOG_MS)) {
        g_drop_log_t = now;
        log_drops();
    }
}

/* Empties the queue from the consumer side. */
static void flush(void)
{
    g_tail = g_head;
}

static bool opt_flag(lua_State *L, const char *key, bool *out)
{
    lua_getfield(L, 1, key);
    bool present = !lua_isnil(L, -1);
    *out = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return present;
}

/* channel / port: nil, or an integer in [lo, hi]. */
static int opt_int(lua_State *L, const char *key, int lo, int hi, int dflt)
{
    lua_getfield(L, 1, key);
    int v = dflt;
    if (!lua_isnil(L, -1)) {
        int isint = 0;
        lua_Integer n = lua_tointegerx(L, -1, &isint);
        if (!isint || n < lo || n > hi) {
            luaL_error(L, "%s out of range: %s", key, luaL_tolstring(L, -1, NULL));
        }
        v = (int)n;
    }
    lua_pop(L, 1);
    return v;
}

static int l_listen(lua_State *L)
{
    bool on = true, notes = true, cc = true, other = true;
    int channel = -1, port = 0;
    switch (lua_type(L, 1)) {
        case LUA_TNONE:
        case LUA_TNIL:
            break;
        case LUA_TBOOLEAN:
            on = lua_toboolean(L, 1);
            break;
        case LUA_TTABLE: {
            bool n, c, o;
            bool any = opt_flag(L, "notes", &n);
            any = opt_flag(L, "cc", &c) || any;
            any = opt_flag(L, "other", &o) || any;
            if (any) {
                notes = n;
                cc = c;
                other = o;
            }
            channel = opt_int(L, "channel", 0, 15, -1);
            port = opt_int(L, "port", 0, RT_MIDI_IN_TRS, 0);
            break;
        }
        default:
            return luaL_error(L, "midi.listen needs a table or a boolean");
    }
    if (on && !g_q) {
        msg_t *q = lua_newuserdatauv(L, sizeof(msg_t) * RT_MIDI_IN_QUEUE, 0);
        lua_setfield(L, LUA_REGISTRYINDEX, REG_KEY);
        g_head = g_tail = 0;
        g_q = q;
    }
    g_listening = on;
    g_notes = notes;
    g_cc = cc;
    g_other = other;
    g_channel = channel;
    g_port = port;
    return 0;
}

static void script_reset(void)
{
    g_listening = false;
    g_channel = -1;
    g_port = 0;
    flush();
}

void rt_api_midiin_open(lua_State *L)
{
    script_reset();
    g_q = NULL; /* the previous state's, if any, is gone */
    g_dropped = 0;
    g_dropped_logged = 0;
    g_drop_log_t = rt_port_now();
    lua_getglobal(L, "midi");
    lua_pushcfunction(L, l_listen);
    lua_setfield(L, -2, "listen");
    lua_pop(L, 1);
}

void rt_api_midiin_close(void)
{
    script_reset();
    g_q = NULL; /* freed with the state (rt_unload() closes it first) */
    log_drops();
}

unsigned rt_api_midiin_dropped(void)
{
    return g_dropped;
}

unsigned rt_api_midiin_pending(void)
{
    return (unsigned)(g_head - g_tail);
}

bool rt_api_midiin_listening(void)
{
    return g_listening;
}
