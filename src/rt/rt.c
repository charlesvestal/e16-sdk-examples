/*
 * Runtime core: lua_State lifecycle, error capture, callback dispatch.
 *
 * The script's state is separate from any host harness state: it opens only
 * base/math/string/table (like the stock firmware), uses rt_port_alloc() as
 * its allocator, and funnels print() through rt_port_log().
 */
#include "rt.h"
#include "rt_port.h"
#include "rt_api_out.h"
#include "rt_api_clock.h"
#include "rt_api_ctl.h"
#include "rt_api_var.h"
#include "rt_api_store.h"
#include "rt_api_draw.h"
#include "rt_api_ring.h"
#include "rt_api_launcher.h"
#include "rt_api_audio.h"
#include "rt_api_midiin.h"

#include "lauxlib.h"
#include "lualib.h"

#include <string.h>
#include <stdio.h>

static lua_State *g_L = NULL;
static char g_last_error[256] = "";

/*
 * luaL_tolstring() + luaL_addvalue() (NOT luaL_addlstring()): tolstring()
 * leaves its result on top of the buffer's box, and addlstring() assumes
 * nothing is there. addlstring() happens to work for short lines, but once a
 * line exceeds LUAL_BUFFERSIZE and the buffer has to grow, it misreads the
 * pushed value as its own box and corrupts the result (test/rt/out_spec.lua
 * checks a 300-character line).
 */
static int l_print(lua_State *L)
{
    int n = lua_gettop(L);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (int i = 1; i <= n; i++) {
        if (i > 1) {
            luaL_addchar(&b, '\t');
        }
        luaL_tolstring(L, i, NULL);
        luaL_addvalue(&b);
    }
    luaL_addchar(&b, '\n');
    luaL_pushresult(&b);
    size_t len;
    const char *s = lua_tolstring(L, -1, &len);
    rt_port_log(s, len);
    lua_pop(L, 1);
    return 0;
}

/* Same message handler as lua.c's stand-alone interpreter: appends a
 * traceback to the error object before it unwinds past the pcall. */
static int msghandler(lua_State *L)
{
    const char *msg = lua_tostring(L, 1);
    if (msg == NULL) {
        if (luaL_callmeta(L, 1, "__tostring") && lua_type(L, -1) == LUA_TSTRING) {
            return 1;
        }
        msg = lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
    }
    luaL_traceback(L, L, msg, 1);
    return 1;
}

/* Resolves a dotted global path ("a.b.c") and leaves the function on top of
 * the stack. Leaves nothing pushed and returns false if any segment is
 * missing or the final value isn't callable. */
static bool resolve_path(lua_State *L, const char *path)
{
    char buf[64];
    size_t n = strlen(path);
    if (n >= sizeof buf) {
        return false;
    }
    memcpy(buf, path, n + 1);

    char *tok = buf;
    char *next = strchr(tok, '.');
    if (next) {
        *next = '\0';
    }
    lua_getglobal(L, tok);
    tok = next ? next + 1 : NULL;

    while (tok) {
        next = strchr(tok, '.');
        if (next) {
            *next = '\0';
        }
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            return false;
        }
        lua_getfield(L, -1, tok);
        lua_remove(L, -2);
        tok = next ? next + 1 : NULL;
    }

    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    return true;
}

enum { CALL_OK, CALL_ABSENT, CALL_ERROR };

/* Calls the function at `path` with nargs already pushed, through the
 * traceback message handler. On error, copies the message into
 * g_last_error (prefixed) and logs it if `log` is set. */
static int call_path(const char *path, int nargs, const char *prefix, bool log)
{
    lua_State *L = g_L;
    if (!L) {
        return CALL_ABSENT;
    }

    int argbase = lua_gettop(L) - nargs; /* args occupy argbase+1 .. argbase+nargs */
    if (!resolve_path(L, path)) {
        lua_settop(L, argbase); /* drop the args; nothing to call */
        return CALL_ABSENT;
    }

    lua_insert(L, argbase + 1); /* function below the args */
    int fn_idx = argbase + 1;
    lua_pushcfunction(L, msghandler);
    lua_insert(L, fn_idx); /* message handler below the function */
    int msgh = fn_idx;

    int status = lua_pcall(L, nargs, 0, msgh);
    lua_remove(L, msgh);

    if (status != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        snprintf(g_last_error, sizeof g_last_error, "%s%s", prefix, msg ? msg : "error");
        if (log) {
            rt_port_log(g_last_error, strlen(g_last_error));
        }
        lua_pop(L, 1);
        return CALL_ERROR;
    }
    return CALL_OK;
}

/*
 * Runtime error accounting. A script that fails in a callback usually
 * fails the same way on every call (every turn, every update), so logging
 * each one would flood the log and the USB console. Instead, per callback
 * path, the first error since the script loaded is logged in full (with
 * its traceback) and later ones are only counted; a summary line
 * ("rt: <path> failed N more times") follows every ERR_SUMMARY_MS while
 * new ones keep coming, and once more when the script unloads. Clock
 * callbacks keep their own single counter (rt_call_clock(), the mock's
 * M.cerrs) and are summarized the same way.
 */
#define ERR_PATHS 8
#define ERR_SUMMARY_MS 10000

typedef struct {
    const char *path;   /* a string literal from the caller */
    unsigned count;
    unsigned reported;  /* count as of the last log line about it */
} err_path_t;

static err_path_t g_err_paths[ERR_PATHS];
static unsigned g_runtime_errors = 0;
static unsigned g_clock_errors = 0;   /* the mock's M.cerrs */
static unsigned g_clock_reported = 0;
static rt_time_t g_err_summary_t = 0;

static void log_line(const char *s)
{
    rt_port_log(s, strlen(s));
}

static void reset_errors(void)
{
    memset(g_err_paths, 0, sizeof g_err_paths);
    g_runtime_errors = 0;
    g_clock_errors = 0;
    g_clock_reported = 0;
    g_err_summary_t = rt_port_now();
}

/* Logs one line per callback path with errors not yet reported. */
static void summarize_errors(void)
{
    char line[96];
    for (int i = 0; i < ERR_PATHS && g_err_paths[i].path; i++) {
        err_path_t *e = &g_err_paths[i];
        if (e->count > e->reported) {
            snprintf(line, sizeof line, "rt: %s failed %u more times (%u total)", e->path,
                     e->count - e->reported, e->count);
            log_line(line);
            e->reported = e->count;
        }
    }
    if (g_clock_errors > g_clock_reported) {
        snprintf(line, sizeof line, "rt: clock callbacks failed %u more times (%u total)",
                 g_clock_errors - g_clock_reported, g_clock_errors);
        log_line(line);
        g_clock_reported = g_clock_errors;
    }
}

bool rt_call(const char *path, int nargs)
{
    int status = call_path(path, nargs, "", false);
    if (status != CALL_ERROR) {
        return status == CALL_OK;
    }
    g_runtime_errors++;
    for (int i = 0; i < ERR_PATHS; i++) {
        err_path_t *e = &g_err_paths[i];
        if (!e->path) {
            e->path = path;
            e->count = e->reported = 1;
            log_line(g_last_error);
            return false;
        }
        if (strcmp(e->path, path) == 0) {
            e->count++;
            return false;
        }
    }
    /* More distinct failing paths than the table holds: just log it. */
    log_line(g_last_error);
    return false;
}

bool rt_call_clock(const char *path, int nargs)
{
    int status = call_path(path, nargs, "clock callback error: ", g_clock_errors == 0);
    if (status == CALL_ERROR) {
        if (g_clock_errors == 0) {
            g_clock_reported = 1;
        }
        g_clock_errors++;
        g_runtime_errors++;
    }
    return status == CALL_OK;
}

unsigned rt_clock_error_count(void)
{
    return g_clock_errors;
}

unsigned rt_error_count(void)
{
    return g_runtime_errors;
}

void rt_midi_realtime(uint8_t byte)
{
    rt_api_clock_realtime(byte);
}

void rt_midi_in(int port, const uint8_t *bytes, size_t len)
{
    rt_api_midiin_push(port, bytes, len);
}

/* Common failure path for rt_load: logs the error object on top of L's
 * stack (or 'fallback' if there isn't a string one), closes the state, and
 * leaves the runtime unloaded. */
static bool fail_load(lua_State *L, const char *fallback)
{
    const char *msg = lua_tostring(L, -1);
    snprintf(g_last_error, sizeof g_last_error, "%s", msg ? msg : fallback);
    rt_port_log(g_last_error, strlen(g_last_error));
    lua_pop(L, 1);
    lua_close(L);
    g_L = NULL;
    rt_api_midiin_close(); /* its queue lived in the closed state */
    return false;
}

/* rt_load()'s source: the whole script in one buffer, handed to Lua in a
 * single piece (what luaL_loadbufferx() does internally). */
typedef struct {
    const char *src;
    size_t len;
} buffer_reader_t;

static const char *read_buffer(lua_State *L, void *ud, size_t *size)
{
    (void)L;
    buffer_reader_t *b = ud;
    if (b->len == 0) {
        *size = 0;
        return NULL;
    }
    *size = b->len;
    b->len = 0;
    return b->src;
}

bool rt_load(const rt_scene_t *scene, const char *src, size_t len)
{
    buffer_reader_t b = {src, len};
    return rt_load_reader(scene, read_buffer, &b);
}

bool rt_load_reader(const rt_scene_t *scene, lua_Reader reader, void *ud)
{
    rt_unload();
    g_last_error[0] = '\0';
    reset_errors();

    /* A scene name over RT_SCENE_NAME_MAX fails the load outright: var
     * and store key their per-scene RAM by this name in a fixed buffer
     * (rt_api_var.c, rt_api_store.c), and truncating it instead could
     * alias two different scripts' saved vars/store together. */
    const char *scene_name = (scene && scene->name) ? scene->name : "";
    if (strlen(scene_name) > RT_SCENE_NAME_MAX) {
        snprintf(g_last_error, sizeof g_last_error, "scene name too long: %s", scene_name);
        rt_port_log(g_last_error, strlen(g_last_error));
        return false;
    }

    lua_State *L = lua_newstate(rt_port_alloc, NULL);
    if (!L) {
        snprintf(g_last_error, sizeof g_last_error, "lua_newstate failed");
        rt_port_log(g_last_error, strlen(g_last_error));
        return false;
    }
    g_L = L;

    /* Only base/math/string/table, like the stock firmware. The harness's
     * own state (if any) opens whatever it likes; this one does not. */
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(L, 1);

    lua_pushcfunction(L, l_print);
    lua_setglobal(L, "print");

    rt_api_out_open(L);
    rt_api_midiin_open(L); /* adds midi.listen to out's midi table */
    rt_api_clock_open(L);
    rt_api_ctl_open(L, scene);
    rt_api_var_open(L, scene);
    rt_api_store_open(L, scene);
    rt_api_draw_open(L);
    rt_api_ring_open(L);
    rt_api_launcher_open(L);
    rt_api_audio_open(L);

    char title_buf[16];
    if (scene && scene->title) {
        strncpy(title_buf, scene->title, sizeof title_buf - 1);
        title_buf[sizeof title_buf - 1] = '\0';
    } else {
        title_buf[0] = '\0';
    }
    rt_port_title(title_buf);

    /* "=name" makes error messages read "euclid:12: ..." rather than
     * '[string "..."]:12: ...', which matters on a 21-column error screen. */
    char chunkname[RT_SCENE_NAME_MAX + 2];
    snprintf(chunkname, sizeof chunkname, "=%s", scene_name[0] ? scene_name : "script");
    /* Text only ("t"): precompiled bytecode is refused, whichever way the
     * source arrives. */
    if (lua_load(L, reader, ud, chunkname, "t") != LUA_OK) {
        return fail_load(L, "load error");
    }

    /* Run the chunk's top level through the same traceback message handler
     * as rt_call(), rather than a bare lua_pcall. */
    int base = lua_gettop(L); /* the loaded chunk function */
    lua_pushcfunction(L, msghandler);
    lua_insert(L, base);
    int status = lua_pcall(L, 0, 0, base);
    lua_remove(L, base);
    if (status != LUA_OK) {
        return fail_load(L, "load error");
    }

    /* The mock calls page.onInit() unprotected: an error there fails the
     * load, same as a syntax or top-level error. rt_call() already logs
     * the error (with traceback) before returning false. It's a load
     * failure (the error screen), not a runtime error of a running
     * script, so it doesn't count toward rt_error_count(). */
    if (!rt_call("page.onInit", 0)) {
        lua_close(L);
        g_L = NULL;
        rt_api_midiin_close(); /* its queue lived in the closed state */
        reset_errors();
        return false;
    }
    return true;
}

void rt_unload(void)
{
    if (g_L) {
        summarize_errors();
        /* Commit store.data to its buffer while L is still open: the
         * live table dies with lua_close(), but the buffer (keyed by
         * scene name, rt_api_store.c) survives to the next rt_load(). */
        rt_api_store_close(g_L);
        lua_close(g_L);
        g_L = NULL;
    }
    /* The script is gone; its controller state (current page, hold
     * arming, press state) shouldn't outlive it either. var's 32 slots
     * and store's saved buffer are untouched here: they live outside the
     * Lua heap and are only cleared by rt_api_var_open()/
     * rt_api_store_open() on the next rt_load(), when the scene name
     * differs from the one they currently hold. */
    rt_api_ctl_close();
    rt_api_clock_close();
    rt_api_ring_close();
    rt_port_draw_release();
    rt_api_audio_close();
    rt_api_midiin_close();
}

lua_State *rt_state(void)
{
    return g_L;
}

void rt_tick(void)
{
    rt_time_t now = rt_port_now();
    /* Clock and system.update first (in the mock's M.run order; see
     * rt_api_clock_tick()), then hold detection. The clock runs even with
     * no script loaded, so external transport keeps its position; there's
     * just nothing to call. */
    rt_api_clock_tick(now, true);
    rt_api_midiin_tick(now);
    rt_api_ctl_tick(now);
    rt_api_audio_tick(now);
    if (now - g_err_summary_t >= (rt_time_t)ERR_SUMMARY_MS) {
        g_err_summary_t = now;
        summarize_errors();
    }
}

const char *rt_last_error(void)
{
    return g_last_error;
}
