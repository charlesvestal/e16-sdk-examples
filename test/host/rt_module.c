/*
 * The `rt` Lua module: the host harness's window into the native runtime
 * (src/rt). Runs in the harness's own lua_State (opened with every stdlib),
 * never in the script's state, which rt_load() creates and owns separately.
 */
#include "lua.h"
#include "lauxlib.h"

#include "rt.h"
#include "rt_api_clock.h"
#include "rt_api_ctl.h"
#include "rt_api_midiin.h"
#include "rt_api_ring.h"
#include "rt_api_store.h"
#include "rt_api_var.h"
#include "rt_scene.h"
#include "port_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The wiring set by the most recent rt.scene{...} call, applied to every
 * load_string/load from then on (see l_scene()). */
static rt_scene_t g_pending_scene;

/* Bumped whenever the script's lua_State is replaced or closed, so a
 * cross-state function handle (see "Cross-state bridge" below) taken from
 * one load can't reach into the next. */
static unsigned g_gen = 1;
static bool g_have_pending_scene = false;

static void make_scene(rt_scene_t *scene, const char *name)
{
    if (g_have_pending_scene) {
        *scene = g_pending_scene;
    } else {
        memset(scene, 0, sizeof *scene);
    }
    scene->name = name;
    scene->title = name;
}

/* rt.scene{pages=, pad_pages=, settings_page=, assigned={ids...}}: builds
 * an rt_scene_t kept in a static, used by the next load_string/load. If a
 * script is already loaded, its wiring is updated immediately too
 * (ctl_spec sets the scene after loading the test script). */
static int l_scene(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    rt_scene_t scene;
    memset(&scene, 0, sizeof scene);

    lua_getfield(L, 1, "pages");
    scene.pages = (uint8_t)luaL_optinteger(L, -1, 1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "pad_pages");
    scene.pad_pages = (uint8_t)luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);

    lua_getfield(L, 1, "settings_page");
    scene.settings_page = (uint8_t)luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);

    lua_getfield(L, 1, "assigned");
    if (lua_istable(L, -1)) {
        lua_Integer n = (lua_Integer)luaL_len(L, -1);
        for (lua_Integer i = 1; i <= n; i++) {
            lua_geti(L, -1, i);
            lua_Integer id = luaL_checkinteger(L, -1);
            lua_pop(L, 1);
            if (id >= 1 && id <= 64) {
                scene.assigned[(id - 1) / 8] |= (uint8_t)(1 << ((id - 1) % 8));
            }
        }
    }
    lua_pop(L, 1);

    g_pending_scene = scene;
    g_have_pending_scene = true;

    if (rt_state()) {
        rt_api_ctl_set_wiring(&g_pending_scene);
    }
    return 0;
}

static int l_turn(lua_State *L)
{
    lua_Integer enc = luaL_checkinteger(L, 1);
    lua_Integer inc = luaL_checkinteger(L, 2);
    rt_turn((int)enc, (int)inc);
    return 0;
}

static int l_press(lua_State *L)
{
    lua_Integer enc = luaL_checkinteger(L, 1);
    rt_press((int)enc);
    return 0;
}

static int l_release(lua_State *L)
{
    lua_Integer enc = luaL_checkinteger(L, 1);
    rt_release((int)enc);
    return 0;
}

static int l_show(lua_State *L)
{
    lua_Integer page = luaL_checkinteger(L, 1);
    rt_show_page((int)page);
    return 0;
}

/* rt.set_page(p): the mock bridge's (e16mock.lua) silent page setter -- no
 * page.onPageChange call. */
static int l_set_page(lua_State *L)
{
    lua_Integer page = luaL_checkinteger(L, 1);
    rt_set_page_silent((int)page);
    return 0;
}

static int l_load_string(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    size_t len;
    const char *src = luaL_checklstring(L, 2, &len);

    /* Bigger than RT_SCENE_NAME_MAX (+1 for the NUL) so a name longer
     * than rt_load() accepts still reaches it untruncated, exercising
     * rt_load()'s own too-long check rather than this harness's. */
    static char name_buf[48];
    strncpy(name_buf, name, sizeof name_buf - 1);
    name_buf[sizeof name_buf - 1] = '\0';

    rt_scene_t scene;
    make_scene(&scene, name_buf);

    port_host_reset();
    g_gen++;
    bool ok = rt_load(&scene, src, len);
    lua_pushboolean(L, ok);
    return 1;
}

/* rt.load_chunked(name, src [, chunk]): rt_load_reader() through a reader
 * that hands the source out `chunk` bytes at a time (default 7, so
 * tokens, escapes and line ends straddle the pieces) -- the way a disk
 * script arrives from FatFs, one buffer-full at a time. Returns what
 * rt_load_reader() returned, plus how many pieces the reader handed out. */
typedef struct {
    const char *src;
    size_t len, pos, chunk;
    int pieces;
} chunk_reader_t;

static const char *read_chunk(lua_State *L, void *ud, size_t *size)
{
    (void)L;
    chunk_reader_t *r = ud;
    size_t n = r->len - r->pos;
    if (n > r->chunk) {
        n = r->chunk;
    }
    *size = n;
    if (n == 0) {
        return NULL;
    }
    const char *p = r->src + r->pos;
    r->pos += n;
    r->pieces++;
    return p;
}

static int l_load_chunked(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    size_t len;
    const char *src = luaL_checklstring(L, 2, &len);
    lua_Integer chunk = luaL_optinteger(L, 3, 7);
    luaL_argcheck(L, chunk >= 1, 3, "chunk must be >= 1");

    static char name_buf[48];
    strncpy(name_buf, name, sizeof name_buf - 1);
    name_buf[sizeof name_buf - 1] = '\0';

    rt_scene_t scene;
    make_scene(&scene, name_buf);

    port_host_reset();
    g_gen++;
    chunk_reader_t r = {src, len, 0, (size_t)chunk, 0};
    bool ok = rt_load_reader(&scene, read_chunk, &r);
    lua_pushboolean(L, ok);
    lua_pushinteger(L, r.pieces);
    return 2;
}

static int l_load(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return luaL_error(L, "cannot open %s", path);
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) {
        fclose(f);
        return luaL_error(L, "cannot stat %s", path);
    }
    char *buf = (char *)malloc((size_t)sz);
    if (!buf) {
        fclose(f);
        return luaL_error(L, "out of memory reading %s", path);
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);

    static char name_buf[48];
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    strncpy(name_buf, base, sizeof name_buf - 1);
    name_buf[sizeof name_buf - 1] = '\0';
    char *dot = strrchr(name_buf, '.');
    if (dot) {
        *dot = '\0';
    }

    rt_scene_t scene;
    make_scene(&scene, name_buf);

    port_host_reset();
    g_gen++;
    bool ok = rt_load(&scene, buf, rd);
    free(buf);
    lua_pushboolean(L, ok);
    return 1;
}

/* Runs src in the *script's* state and returns up to one number/boolean/
 * string/nil result. Errors propagate as a Lua error in the harness state
 * (unlike errors inside callbacks dispatched via rt_call, which rt.c
 * catches and logs -- that's the runtime's contract for real scripts). */
static int l_eval(lua_State *L)
{
    size_t len;
    const char *src = luaL_checklstring(L, 1, &len);
    lua_State *S = rt_state();
    if (!S) {
        return luaL_error(L, "no script loaded");
    }

    int top0 = lua_gettop(S);
    int status = luaL_loadbufferx(S, src, len, "eval", "t");
    if (status == LUA_OK) {
        status = lua_pcall(S, 0, LUA_MULTRET, 0);
    }
    if (status != LUA_OK) {
        const char *msg = lua_tostring(S, -1);
        char errbuf[256];
        snprintf(errbuf, sizeof errbuf, "%s", msg ? msg : "eval error");
        lua_settop(S, top0);
        return luaL_error(L, "%s", errbuf);
    }

    int nres = lua_gettop(S) - top0;
    if (nres <= 0) {
        lua_settop(S, top0);
        return 0;
    }

    int idx = top0 + 1;
    switch (lua_type(S, idx)) {
        case LUA_TNIL:
            lua_pushnil(L);
            break;
        case LUA_TBOOLEAN:
            lua_pushboolean(L, lua_toboolean(S, idx));
            break;
        case LUA_TNUMBER:
            if (lua_isinteger(S, idx)) {
                lua_pushinteger(L, lua_tointeger(S, idx));
            } else {
                lua_pushnumber(L, lua_tonumber(S, idx));
            }
            break;
        case LUA_TSTRING: {
            size_t slen;
            const char *s = lua_tolstring(S, idx, &slen);
            lua_pushlstring(L, s, slen);
            break;
        }
        default:
            lua_pushnil(L);
            break;
    }
    lua_settop(S, top0);
    return 1;
}

static int l_sent(lua_State *L)
{
    size_t n;
    const port_host_msg_t *m = port_host_sent(&n);
    /* rt.sent([from]): messages from index `from` (1-based) on, so a
     * caller mirroring the list can fetch just what's new. */
    size_t from = (size_t)luaL_optinteger(L, 1, 1);
    if (from < 1) {
        from = 1;
    }
    lua_createtable(L, n >= from ? (int)(n - from + 1) : 0, 0);
    for (size_t i = from - 1; i < n; i++) {
        lua_createtable(L, 0, 5);
        lua_pushnumber(L, m[i].t);
        lua_setfield(L, -2, "t");
        lua_pushinteger(L, m[i].out);
        lua_setfield(L, -2, "out");
        lua_pushinteger(L, m[i].st);
        lua_setfield(L, -2, "st");
        lua_pushinteger(L, m[i].d1);
        lua_setfield(L, -2, "d1");
        lua_pushinteger(L, m[i].d2);
        lua_setfield(L, -2, "d2");
        lua_rawseti(L, -2, (lua_Integer)(i - from + 2));
    }
    return 1;
}

static int l_rings(lua_State *L)
{
    const port_host_ring_t *r = port_host_rings();
    lua_createtable(L, 16, 0);
    for (int i = 0; i < 16; i++) {
        if (!r[i].set) {
            continue;
        }
        lua_createtable(L, 0, 2);
        lua_pushinteger(L, r[i].v);
        lua_setfield(L, -2, "v");
        lua_pushinteger(L, r[i].c);
        lua_setfield(L, -2, "c");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int l_labels(lua_State *L)
{
    lua_createtable(L, 16, 0);
    for (int i = 1; i <= 16; i++) {
        const char *s = port_host_label(i);
        if (s) {
            lua_pushstring(L, s);
            lua_rawseti(L, -2, i);
        }
    }
    return 1;
}

static int l_title(lua_State *L)
{
    lua_pushstring(L, port_host_title());
    return 1;
}

static int l_rate(lua_State *L)
{
    lua_pushinteger(L, rt_api_clock_rate());
    return 1;
}

static int l_clear_sent(lua_State *L)
{
    (void)L;
    port_host_clear_sent();
    return 0;
}

static int l_now(lua_State *L)
{
    lua_pushnumber(L, port_host_now());
    return 1;
}

static int l_log(lua_State *L)
{
    size_t len;
    const char *s = port_host_log(&len);
    lua_pushlstring(L, s, len);
    return 1;
}

/* Steps host time forward like the mock's M.run(ms): one rt_tick() at the
 * current time first (it flushes callbacks queued since the last run and
 * fires anything already due), then straight to each next due event --
 * system.update, a clock pulse, or an encoder hold
 * (rt_api_ctl_next_due()) -- so each lands exactly on its due time.
 *
 * The steps after the first are continuations of the same M.run, so they
 * do rt_tick()'s work (clock, then holds) with flush_first = false: see
 * rt_api_clock_tick(). */
static int l_advance(lua_State *L)
{
    /* float, like the mock's M.now + ms under LUA_32BITS */
    float fms = (float)luaL_checknumber(L, 1);
    float fstop = (float)port_host_now() + fms;
    double now = port_host_now();
    double stop = (double)fstop;
    rt_tick();
    for (;;) {
        double due = (double)rt_api_clock_next_due();
        double due_pulse = (double)rt_api_clock_next_pulse();
        double due_hold = (double)rt_api_ctl_next_due();
        if (due_pulse < due) {
            due = due_pulse;
        }
        if (due_hold < due) {
            due = due_hold;
        }
        if (due > stop) {
            break;
        }
        if (due < now) {
            due = now;
        }
        port_host_set_now(due);
        now = due;
        rt_api_clock_tick((rt_time_t)now, false);
        rt_api_ctl_tick((rt_time_t)now);
    }
    port_host_set_now(stop);
    return 0;
}

/* rt.reset(): unloads the script, puts the clock back to its power-on
 * state and host time back to 0, so a spec can replay a mock scenario
 * from scratch. */
static int l_reset(lua_State *L)
{
    (void)L;
    g_gen++;
    rt_unload();
    rt_api_clock_reset();
    port_host_set_now(0.0);
    port_host_reset();
    return 0;
}

static int l_now_ms(lua_State *S)
{
    lua_pushnumber(S, (lua_Number)port_host_now());
    return 1;
}

/* rt.inject_now(): gives the loaded script a now_ms() global returning
 * host time, for specs that timestamp callbacks (a test hook; the
 * runtime itself has no such function). */
static int l_inject_now(lua_State *L)
{
    lua_State *S = rt_state();
    if (!S) {
        return luaL_error(L, "no script loaded");
    }
    lua_pushcfunction(S, l_now_ms);
    lua_setglobal(S, "now_ms");
    return 0;
}

/* rt.ext_start([bpm]) / rt.ext_stop() / rt.ext_continue(): external
 * transport in timer mode, exactly the mock's M.extStart etc. */
static int l_ext_start(lua_State *L)
{
    bool have = !lua_isnoneornil(L, 1);
    lua_Number bpm = have ? luaL_checknumber(L, 1) : 0;
    rt_api_clock_ext_start((float)bpm, have && lua_isinteger(L, 1), have);
    return 0;
}

static int l_ext_stop(lua_State *L)
{
    (void)L;
    rt_api_clock_ext_stop();
    return 0;
}

static int l_ext_continue(lua_State *L)
{
    (void)L;
    rt_api_clock_ext_continue();
    return 0;
}

/* rt.realtime(byte): a MIDI realtime byte, as from the device's MIDI in. */
static int l_realtime(lua_State *L)
{
    rt_midi_realtime((uint8_t)luaL_checkinteger(L, 1));
    return 0;
}

/* rt.midi_in(port, status, d1[, d2]): a received MIDI message, as the
 * device's main loop hands it over (rt_midi_in()). Delivered to the
 * script on the next rt_tick() (rt.advance(), rt.tick()). */
static int l_midi_in(lua_State *L)
{
    int port = (int)luaL_checkinteger(L, 1);
    int n = lua_gettop(L) - 1;
    uint8_t bytes[3];
    luaL_argcheck(L, n >= 1 && n <= 3, 2, "1-3 bytes");
    for (int i = 0; i < n; i++) {
        bytes[i] = (uint8_t)luaL_checkinteger(L, 2 + i);
    }
    rt_midi_in(port, bytes, (size_t)n);
    return 0;
}

/* rt.midi_in_info(): queued (not yet delivered), dropped since load,
 * listening. */
static int l_midi_in_info(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)rt_api_midiin_pending());
    lua_pushinteger(L, (lua_Integer)rt_api_midiin_dropped());
    lua_pushboolean(L, rt_api_midiin_listening());
    return 3;
}

/* rt.tick(): one rt_tick() at the current host time, as one main-loop
 * pass would. */
static int l_tick(lua_State *L)
{
    (void)L;
    rt_tick();
    return 0;
}

/* rt.persist_roundtrip(): exercises rt_vars_serialize/deserialize and
 * rt_store_serialize/deserialize plus rt_store_shared_serialize/
 * deserialize (rt.h's persistence API, the device's save path)
 * without a full reload. Commits the live store.data table
 * (rt_api_store_close(), same as rt_unload() does, into whichever of the
 * scene/shared buffers the script last selected with store.use()),
 * serializes vars and both store buffers to bytes, deserializes those
 * bytes back -- each deserialize fully replaces the array/buffer first,
 * so this only shows the original values again if the round trip is
 * actually faithful -- then rebuilds store.data from the restored buffer
 * so the script sees it. var.get() needs no such rebuild: it always
 * reads the live array directly. */
static int l_persist_roundtrip(lua_State *L)
{
    lua_State *S = rt_state();
    if (!S) {
        return luaL_error(L, "no script loaded");
    }

    rt_api_store_close(S);

    static uint8_t vbuf[4096];
    static uint8_t sbuf[4096];
    static uint8_t shbuf[4096];
    size_t vlen = rt_vars_serialize(vbuf, sizeof vbuf);
    size_t slen = rt_store_serialize(sbuf, sizeof sbuf);
    size_t shlen = rt_store_shared_serialize(shbuf, sizeof shbuf);

    rt_vars_deserialize(vbuf, vlen);
    rt_store_deserialize(sbuf, slen);
    rt_store_shared_deserialize(shbuf, shlen);

    rt_api_store_sync(S);
    return 0;
}

/* rt.persist_capture()/rt.persist_restore(name): the device's save/restore
 * path (src/ui/launcher.c), split into its two halves so a test can put a "power cycle"
 * (loading a different scene, which clears the in-RAM vars/store) in
 * between them -- unlike rt.persist_roundtrip() above, which round-trips
 * immediately with nothing in between.
 *
 * persist_capture() commits the live store.data and serializes vars plus
 * both store buffers into static snapshots (like persist_roundtrip's).
 * persist_restore(name) replays launcher.c's pre-load restore: bind the var
 * and scene-store buffers to `name` (so the rt_load() that follows -- of
 * that same name -- won't see a different name and clear them back out),
 * then deserialize the snapshots. Called before rt.load_string(name,...),
 * this proves a restored var/store is visible inside that load's
 * page.onInit(), which runs before rt_load() returns. */
static uint8_t g_pcap_vbuf[4096];
static uint8_t g_pcap_sbuf[4096];
static uint8_t g_pcap_shbuf[4096];
static size_t g_pcap_vlen, g_pcap_slen, g_pcap_shlen;

static int l_persist_capture(lua_State *L)
{
    (void)L;
    lua_State *S = rt_state();
    if (S) {
        rt_api_store_close(S);
    }
    g_pcap_vlen = rt_vars_serialize(g_pcap_vbuf, sizeof g_pcap_vbuf);
    g_pcap_slen = rt_store_serialize(g_pcap_sbuf, sizeof g_pcap_sbuf);
    g_pcap_shlen = rt_store_shared_serialize(g_pcap_shbuf, sizeof g_pcap_shbuf);
    return 0;
}

static int l_persist_restore(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    rt_api_var_bind_scene(name);
    rt_vars_deserialize(g_pcap_vbuf, g_pcap_vlen);
    rt_api_store_bind_scene(name);
    rt_store_deserialize(g_pcap_sbuf, g_pcap_slen);
    rt_store_shared_deserialize(g_pcap_shbuf, g_pcap_shlen);
    return 0;
}


/* ------------------------------------------------------------------ */
/* Cross-state bridge (test/host/e16mock.lua).
 *
 * The oxi-e16-lua suites were written against a mock where the test and
 * the script share one lua_State: they call the script's callbacks
 * directly (controller.onEncoderTurn{...}, page.onVarChange("bpm")), read
 * store.data, and swap API functions for stubs (midi.sendCC = function()
 * calls = calls + 1 end) that the script then calls. Here the script runs
 * in the runtime's own state, so the shim exposes the script's globals in
 * the harness state through rt.get/rt.set, which copy values across:
 *
 *  - plain values copy; tables copy deeply (they're snapshots);
 *  - a script function arrives in the harness as a callable handle
 *    (userdata with __call) that calls it in the script's state, with the
 *    arguments and results copied across, and errors re-raised;
 *  - a harness function stored into the script (rt.set) becomes a C
 *    closure there that calls back into the harness the same way, so a
 *    stub's error() reaches the script as an ordinary Lua error. Storing a
 *    handle back puts the original script function back.
 *
 * Test-only: nothing in src/rt knows about any of this. */

#define FNWRAP "rt.fnwrap"
#define BRIDGE_DEPTH 16

typedef struct {
    unsigned gen;
    int ref; /* in the script state's registry */
} fnwrap_t;

static lua_State *g_H = NULL; /* the harness's main thread */

static int bridge_call(lua_State *S);

/* Copies the value at S[idx] onto H's stack. */
static void s2h(lua_State *S, int idx, lua_State *H, int depth)
{
    idx = lua_absindex(S, idx);
    lua_checkstack(H, 4);
    switch (lua_type(S, idx)) {
        case LUA_TBOOLEAN:
            lua_pushboolean(H, lua_toboolean(S, idx));
            break;
        case LUA_TNUMBER:
            if (lua_isinteger(S, idx)) {
                lua_pushinteger(H, lua_tointeger(S, idx));
            } else {
                lua_pushnumber(H, lua_tonumber(S, idx));
            }
            break;
        case LUA_TSTRING: {
            size_t n;
            const char *str = lua_tolstring(S, idx, &n);
            lua_pushlstring(H, str, n);
            break;
        }
        case LUA_TTABLE:
            if (depth >= BRIDGE_DEPTH) {
                lua_pushnil(H);
                break;
            }
            lua_newtable(H);
            lua_checkstack(S, 3);
            lua_pushnil(S);
            while (lua_next(S, idx)) {
                s2h(S, -2, H, depth + 1);
                s2h(S, -1, H, depth + 1);
                if (lua_isnil(H, -2)) {
                    lua_pop(H, 2);
                } else {
                    lua_rawset(H, -3);
                }
                lua_pop(S, 1);
            }
            break;
        case LUA_TFUNCTION:
            if (lua_tocfunction(S, idx) == bridge_call) {
                /* one of ours: hand back the harness function itself */
                lua_getupvalue(S, idx, 1);
                int href = (int)lua_tointeger(S, -1);
                lua_pop(S, 1);
                lua_rawgeti(H, LUA_REGISTRYINDEX, href);
            } else {
                lua_pushvalue(S, idx);
                int ref = luaL_ref(S, LUA_REGISTRYINDEX);
                fnwrap_t *w = (fnwrap_t *)lua_newuserdatauv(H, sizeof *w, 0);
                w->gen = g_gen;
                w->ref = ref;
                luaL_setmetatable(H, FNWRAP);
            }
            break;
        default:
            lua_pushnil(H);
            break;
    }
}

/* Copies the value at H[idx] onto S's stack. */
static void h2s(lua_State *H, int idx, lua_State *S, int depth)
{
    idx = lua_absindex(H, idx);
    lua_checkstack(S, 4);
    switch (lua_type(H, idx)) {
        case LUA_TBOOLEAN:
            lua_pushboolean(S, lua_toboolean(H, idx));
            break;
        case LUA_TNUMBER:
            if (lua_isinteger(H, idx)) {
                lua_pushinteger(S, lua_tointeger(H, idx));
            } else {
                lua_pushnumber(S, lua_tonumber(H, idx));
            }
            break;
        case LUA_TSTRING: {
            size_t n;
            const char *str = lua_tolstring(H, idx, &n);
            lua_pushlstring(S, str, n);
            break;
        }
        case LUA_TTABLE:
            if (depth >= BRIDGE_DEPTH) {
                lua_pushnil(S);
                break;
            }
            lua_newtable(S);
            lua_checkstack(H, 3);
            lua_pushnil(H);
            while (lua_next(H, idx)) {
                h2s(H, -2, S, depth + 1);
                h2s(H, -1, S, depth + 1);
                if (lua_isnil(S, -2)) {
                    lua_pop(S, 2);
                } else {
                    lua_rawset(S, -3);
                }
                lua_pop(H, 1);
            }
            break;
        case LUA_TFUNCTION: {
            lua_pushvalue(H, idx);
            int href = luaL_ref(H, LUA_REGISTRYINDEX);
            lua_pushinteger(S, href);
            lua_pushcclosure(S, bridge_call, 1);
            break;
        }
        case LUA_TUSERDATA: {
            fnwrap_t *w = (fnwrap_t *)luaL_testudata(H, idx, FNWRAP);
            if (w && w->gen == g_gen) {
                lua_rawgeti(S, LUA_REGISTRYINDEX, w->ref);
            } else {
                lua_pushnil(S);
            }
            break;
        }
        default:
            lua_pushnil(S);
            break;
    }
}

/* A harness function, called from the script. */
static int bridge_call(lua_State *S)
{
    lua_State *H = g_H;
    int href = (int)lua_tointeger(S, lua_upvalueindex(1));
    int n = lua_gettop(S);
    int top = lua_gettop(H);
    if (!lua_checkstack(H, n + 4)) {
        return luaL_error(S, "rt bridge: harness stack overflow");
    }
    lua_rawgeti(H, LUA_REGISTRYINDEX, href);
    for (int i = 1; i <= n; i++) {
        s2h(S, i, H, 0);
    }
    if (lua_pcall(H, n, LUA_MULTRET, 0) != LUA_OK) {
        size_t len;
        const char *msg = lua_tolstring(H, -1, &len);
        if (msg) {
            lua_pushlstring(S, msg, len);
        } else {
            lua_pushliteral(S, "(error object is not a string)");
        }
        lua_settop(H, top);
        return lua_error(S);
    }
    int nr = lua_gettop(H) - top;
    if (!lua_checkstack(S, nr + 4)) {
        lua_settop(H, top);
        return luaL_error(S, "rt bridge: script stack overflow");
    }
    for (int i = 1; i <= nr; i++) {
        h2s(H, top + i, S, 0);
    }
    lua_settop(H, top);
    return nr;
}

/* A script function, called from the harness. */
static int fnwrap_call(lua_State *H)
{
    fnwrap_t *w = (fnwrap_t *)luaL_checkudata(H, 1, FNWRAP);
    lua_State *S = rt_state();
    if (!S || w->gen != g_gen) {
        return luaL_error(H, "rt: function from a script that is no longer loaded");
    }
    int n = lua_gettop(H) - 1;
    int top = lua_gettop(S);
    if (!lua_checkstack(S, n + 4)) {
        return luaL_error(H, "rt: script stack overflow");
    }
    lua_rawgeti(S, LUA_REGISTRYINDEX, w->ref);
    for (int i = 2; i <= n + 1; i++) {
        h2s(H, i, S, 0);
    }
    if (lua_pcall(S, n, LUA_MULTRET, 0) != LUA_OK) {
        size_t len;
        const char *msg = lua_tolstring(S, -1, &len);
        if (msg) {
            lua_pushlstring(H, msg, len);
        } else {
            lua_pushliteral(H, "(error object is not a string)");
        }
        lua_settop(S, top);
        return lua_error(H);
    }
    int nr = lua_gettop(S) - top;
    if (!lua_checkstack(H, nr + 4)) {
        lua_settop(S, top);
        return luaL_error(H, "rt: harness stack overflow");
    }
    for (int i = 1; i <= nr; i++) {
        s2h(S, top + i, H, 0);
    }
    lua_settop(S, top);
    return nr;
}

static int fnwrap_gc(lua_State *H)
{
    fnwrap_t *w = (fnwrap_t *)luaL_checkudata(H, 1, FNWRAP);
    lua_State *S = rt_state();
    if (S && w->gen == g_gen) {
        luaL_unref(S, LUA_REGISTRYINDEX, w->ref);
    }
    w->gen = 0;
    return 0;
}

/* Pushes onto S the table holding the last segment of a dotted path
 * ("a.b.c" -> a.b) and returns that segment, or NULL (nothing pushed) if
 * a parent is missing. buf receives the split path. */
static const char *resolve_parent(lua_State *S, const char *path, char *buf, size_t bufsz)
{
    size_t n = strlen(path);
    if (n == 0 || n >= bufsz) {
        return NULL;
    }
    memcpy(buf, path, n + 1);
    lua_pushglobaltable(S);
    char *seg = buf;
    for (;;) {
        char *dot = strchr(seg, '.');
        if (!dot) {
            return seg;
        }
        *dot = '\0';
        lua_getfield(S, -1, seg);
        lua_remove(S, -2);
        if (!lua_istable(S, -1)) {
            lua_pop(S, 1);
            return NULL;
        }
        seg = dot + 1;
    }
}

/* rt.get("a.b"): the script's value at a dotted global path, copied (see
 * above). nil if nothing is loaded or the path doesn't exist. */
static int l_get(lua_State *H)
{
    const char *path = luaL_checkstring(H, 1);
    lua_State *S = rt_state();
    char buf[128];
    const char *last;
    if (!S || !(last = resolve_parent(S, path, buf, sizeof buf))) {
        lua_pushnil(H);
        return 1;
    }
    lua_getfield(S, -1, last);
    s2h(S, -1, H, 0);
    lua_pop(S, 2);
    return 1;
}

/* rt.set("a.b", v): stores a copy of v at a dotted global path in the
 * script (a harness function becomes a bridge; see above). */
static int l_set(lua_State *H)
{
    const char *path = luaL_checkstring(H, 1);
    luaL_checkany(H, 2);
    lua_State *S = rt_state();
    if (!S) {
        return luaL_error(H, "no script loaded");
    }
    char buf[128];
    const char *last = resolve_parent(S, path, buf, sizeof buf);
    if (!last) {
        return luaL_error(H, "rt.set: no table for %s", path);
    }
    h2s(H, 2, S, 0);
    lua_setfield(S, -2, last);
    lua_pop(S, 1);
    return 0;
}

/* rt.gc(opt): collectgarbage(opt) on the script's state, for "collect",
 * "stop", "restart", "count" (KiB, like collectgarbage) and "isrunning". */
static int l_gc(lua_State *H)
{
    const char *opt = luaL_optstring(H, 1, "collect");
    lua_State *S = rt_state();
    if (!S) {
        lua_pushinteger(H, 0);
        return 1;
    }
    if (strcmp(opt, "count") == 0) {
        int k = lua_gc(S, LUA_GCCOUNT);
        int b = lua_gc(S, LUA_GCCOUNTB);
        lua_pushnumber(H, (lua_Number)k + ((lua_Number)b / 1024));
        return 1;
    }
    if (strcmp(opt, "stop") == 0) {
        lua_gc(S, LUA_GCSTOP);
    } else if (strcmp(opt, "restart") == 0) {
        lua_gc(S, LUA_GCRESTART);
    } else if (strcmp(opt, "isrunning") == 0) {
        lua_pushboolean(H, lua_gc(S, LUA_GCISRUNNING));
        return 1;
    } else {
        lua_gc(S, LUA_GCCOLLECT);
    }
    lua_pushinteger(H, 0);
    return 1;
}

/* rt.loaded(): whether a script is loaded. */
static int l_loaded(lua_State *H)
{
    lua_pushboolean(H, rt_state() != NULL);
    return 1;
}

/* rt.page(): the current page (controller.getPage()). */
static int l_page(lua_State *H)
{
    lua_pushinteger(H, rt_page());
    return 1;
}

/* rt.clock_info(): the mock's M.listening, M.res, M.bpm, M.tp (nil when
 * stopped), M.cerrs and M.hold. */
static int l_clock_info(lua_State *H)
{
    lua_pushboolean(H, rt_api_clock_listening());
    lua_pushinteger(H, rt_api_clock_res());
    bool is_int = false;
    float bpm = rt_api_clock_internal_bpm(&is_int);
    if (is_int) {
        lua_pushinteger(H, (lua_Integer)bpm);
    } else {
        lua_pushnumber(H, bpm);
    }
    int src = rt_api_clock_source();
    if (src < 0) {
        lua_pushnil(H);
    } else {
        lua_pushinteger(H, src);
    }
    lua_pushinteger(H, (lua_Integer)rt_clock_error_count());
    float hold = rt_api_ctl_hold_ms();
    if (hold == (float)(lua_Integer)hold) {
        lua_pushinteger(H, (lua_Integer)hold);
    } else {
        lua_pushnumber(H, hold);
    }
    return 6;
}

/* rt.vars(): {name = value} for every var slot in use. */
static int l_vars(lua_State *H)
{
    lua_newtable(H);
    for (int i = 0; i < 32; i++) {
        const char *name;
        size_t len;
        int32_t v;
        if (rt_api_var_at(i, &name, &len, &v)) {
            lua_pushlstring(H, name, len);
            lua_pushinteger(H, v);
            lua_rawset(H, -3);
        }
    }
    return 1;
}

/* rt.var_put(name, v): writes a var slot directly (creating it), or
 * removes it when v is nil -- a write into the mock's M.store table. */
static int l_var_put(lua_State *H)
{
    size_t len;
    const char *name = luaL_checklstring(H, 1, &len);
    if (lua_isnoneornil(H, 2)) {
        rt_api_var_remove(name, len);
        return 0;
    }
    lua_Integer v = luaL_checkinteger(H, 2);
    if (!rt_api_var_put(name, len, (int32_t)v)) {
        return luaL_error(H, "rt.var_put: can't store %s (name too long or 32 vars in use)", name);
    }
    return 0;
}

static int l_vars_clear(lua_State *H)
{
    (void)H;
    rt_api_var_clear();
    return 0;
}

/* rt.var_bind(name): the var slots belong to scene `name` from now on,
 * without being cleared (the mock's single shared slot). */
static int l_var_bind(lua_State *H)
{
    rt_api_var_bind_scene(luaL_checkstring(H, 1));
    return 0;
}

/* rt.log_history(): every line logged since the last load or reset, each
 * ending in '\n' (rt.log() is only the last one). */
static int l_log_history(lua_State *H)
{
    size_t len;
    const char *s = port_host_log_history(&len);
    lua_pushlstring(H, s, len);
    return 1;
}

/* rt.error_count(): rt_error_count(), callback errors since the script
 * loaded (the screen's "!" indicator). */
static int l_error_count(lua_State *H)
{
    lua_pushinteger(H, (lua_Integer)rt_error_count());
    return 1;
}

/* rt.sent_count(): number of messages recorded so far. */
static int l_sent_count(lua_State *H)
{
    size_t n;
    port_host_sent(&n);
    lua_pushinteger(H, (lua_Integer)n);
    return 1;
}

/* --- draw.* and ring.* window into the host port --- */

/* rt.canvas_pixel(x, y): the canvas pixel at (x, y), as the screen would
 * show it (false past the current height, same as port_host_canvas_pixel). */
static int l_canvas_pixel(lua_State *L)
{
    lua_Integer x = luaL_checkinteger(L, 1);
    lua_Integer y = luaL_checkinteger(L, 2);
    lua_pushboolean(L, port_host_canvas_pixel((int)x, (int)y));
    return 1;
}

static int l_canvas_fullscreen(lua_State *L)
{
    lua_pushboolean(L, port_host_canvas_fullscreen());
    return 1;
}

/* rt.set_disk(bool): whether the host "has a mounted volume" for
 * launcher.has("disk"). */
static int l_set_disk(lua_State *L)
{
    port_host_set_disk(lua_toboolean(L, 1));
    return 0;
}

/* rt.set_usb(bool): whether the host "has USB connected", for
 * audio.output()'s "auto". */
static int l_set_usb(lua_State *L)
{
    port_host_set_usb(lua_toboolean(L, 1));
    return 0;
}

static int l_canvas_height(lua_State *L)
{
    lua_pushinteger(L, port_host_canvas_height());
    return 1;
}

/* rt.canvas_shows(): draw.show() calls since the last load/reset. */
static int l_canvas_shows(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)port_host_canvas_shows());
    return 1;
}

/* rt.led(ring, led): r, g, b, set -- the last colour rt_port_led()
 * recorded for that LED (0, 0, 0, false if never written or released). */
static int l_led(lua_State *L)
{
    lua_Integer ring = luaL_checkinteger(L, 1);
    lua_Integer led = luaL_checkinteger(L, 2);
    const port_host_led_t *p = port_host_led((int)ring, (int)led);
    lua_pushinteger(L, p->r);
    lua_pushinteger(L, p->g);
    lua_pushinteger(L, p->b);
    lua_pushboolean(L, p->set);
    return 4;
}

/* rt.ring_custom(ring): whether ring.* has put this ring in per-LED mode
 * (rt_api_ring.c's bookkeeping, not the port's). */
static int l_ring_custom(lua_State *L)
{
    lua_Integer ring = luaL_checkinteger(L, 1);
    lua_pushboolean(L, rt_api_ring_is_custom((int)ring));
    return 1;
}

/* rt.ring_shows(): ring.show() calls since the last load/reset. */
static int l_ring_shows(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)port_host_ring_shows());
    return 1;
}

/* rt.audio(): every rt_port_audio_*() call since the last load/reset, as
 * {op=, a=, b=} tables (port_host.h). */
static int l_audio(lua_State *L)
{
    size_t n;
    const port_host_audio_t *a = port_host_audio(&n);
    lua_createtable(L, (int)n, 0);
    for (size_t i = 0; i < n; i++) {
        lua_createtable(L, 0, 3);
        lua_pushstring(L, a[i].op);
        lua_setfield(L, -2, "op");
        lua_pushinteger(L, a[i].a);
        lua_setfield(L, -2, "a");
        lua_pushinteger(L, a[i].b);
        lua_setfield(L, -2, "b");
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    return 1;
}

static int l_audio_enabled(lua_State *L)
{
    lua_pushboolean(L, port_host_audio_enabled());
    return 1;
}

/* rt.audio_param(i): the last value set for rt_audio_param_t i (0-based,
 * rt_port.h's order), or -1. */
static int l_audio_param(lua_State *L)
{
    lua_pushinteger(L, port_host_audio_param((int)luaL_checkinteger(L, 1)));
    return 1;
}

static const luaL_Reg rt_funcs[] = {
    {"load_string", l_load_string},
    {"load", l_load},
    {"load_chunked", l_load_chunked},
    {"eval", l_eval},
    {"sent", l_sent},
    {"rings", l_rings},
    {"labels", l_labels},
    {"title", l_title},
    {"rate", l_rate},
    {"clear_sent", l_clear_sent},
    {"now", l_now},
    {"advance", l_advance},
    {"log", l_log},
    {"scene", l_scene},
    {"turn", l_turn},
    {"press", l_press},
    {"release", l_release},
    {"show", l_show},
    {"set_page", l_set_page},
    {"reset", l_reset},
    {"inject_now", l_inject_now},
    {"ext_start", l_ext_start},
    {"ext_stop", l_ext_stop},
    {"ext_continue", l_ext_continue},
    {"realtime", l_realtime},
    {"midi_in", l_midi_in},
    {"midi_in_info", l_midi_in_info},
    {"tick", l_tick},
    {"persist_roundtrip", l_persist_roundtrip},
    {"persist_capture", l_persist_capture},
    {"persist_restore", l_persist_restore},
    {"get", l_get},
    {"set", l_set},
    {"gc", l_gc},
    {"loaded", l_loaded},
    {"page", l_page},
    {"clock_info", l_clock_info},
    {"vars", l_vars},
    {"var_put", l_var_put},
    {"vars_clear", l_vars_clear},
    {"var_bind", l_var_bind},
    {"sent_count", l_sent_count},
    {"error_count", l_error_count},
    {"log_history", l_log_history},
    {"canvas_pixel", l_canvas_pixel},
    {"canvas_fullscreen", l_canvas_fullscreen},
    {"canvas_height", l_canvas_height},
    {"set_disk", l_set_disk},
    {"set_usb", l_set_usb},
    {"canvas_shows", l_canvas_shows},
    {"led", l_led},
    {"ring_custom", l_ring_custom},
    {"ring_shows", l_ring_shows},
    {"audio", l_audio},
    {"audio_enabled", l_audio_enabled},
    {"audio_param", l_audio_param},
    {NULL, NULL},
};

int luaopen_rt(lua_State *L)
{
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
    g_H = lua_tothread(L, -1);
    lua_pop(L, 1);
    if (luaL_newmetatable(L, FNWRAP)) {
        lua_pushcfunction(L, fnwrap_call);
        lua_setfield(L, -2, "__call");
        lua_pushcfunction(L, fnwrap_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1);
    luaL_newlib(L, rt_funcs);
    /* rt.TIME_FLOAT: runtime times are float (RT_TIME_FLOAT, like the
     * mock) rather than the device's double (E16_HOST_DOUBLE_TIME). */
#ifdef RT_TIME_FLOAT
    lua_pushboolean(L, 1);
#else
    lua_pushboolean(L, 0);
#endif
    lua_setfield(L, -2, "TIME_FLOAT");
    return 1;
}
