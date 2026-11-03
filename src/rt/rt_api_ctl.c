/*
 * Controller: physical encoder events (turn/press/release/hold), mapped
 * through the scene's wiring to the ids a script's controller.onEncoder*
 * callbacks see, plus the controller.* API (getPage/set/setByIndex/
 * setControls/get/setHoldTime) and page switches (page.onPageChange).
 *
 * Wiring (scripts/tools/make_scene.py's convention, restated in
 * external/oxi-e16-lua's docs): on a pad page (page <= pad_pages, or any
 * page when pad_pages == 0), encoder e turns as id e and pushes as id
 * 16+e. On the settings page, it turns as 32+e and pushes as 48+e. Only
 * ids a --@assign line declared (scene->assigned) are wired; others
 * produce no callback at all, same as an unwired page.
 *
 * Argument checking and the event tables' fields mirror
 * external/oxi-e16-lua/test/e16mock.lua exactly (the mock is the spec):
 * M.turn/M.press/M.release/M.show and controller.setHoldTime.
 */
#include "rt_api_ctl.h"
#include "rt.h"
#include "rt_port.h"

#include "lauxlib.h"

#include <stdbool.h>
#include <string.h>

/* Wiring, loaded from the scene. */
static uint8_t g_pages = 0;
static uint8_t g_pad_pages = 0;
static uint8_t g_settings_page = 0;
static uint8_t g_assigned[64 / 8];

/* Current page (1-based) and hold arming. g_hold_ms is live: like the
 * mock (M.release checks M.hold at release time, not at press time),
 * controller.setHoldTime(ms) takes effect for every press still down,
 * not just new ones. setHoldTime(0) while a press is held disarms it. */
static int g_page = 1;
static float g_hold_ms = 0.0f; /* 0 = disarmed */

/* Per-physical-encoder press state (index 0 = encoder 1). */
typedef struct {
    bool down;
    bool fire;       /* push id was assigned when the press happened */
    int id;           /* the press's push id (0 if the page wasn't wired) */
    int page;          /* the press's page */
    rt_time_t press_time;
    bool hold_fired;
} press_state_t;

static press_state_t g_press[16];

static bool is_assigned(int id)
{
    if (id < 1 || id > 64) {
        return false;
    }
    int bit = id - 1;
    return (g_assigned[bit / 8] >> (bit % 8)) & 1;
}

/* rt_api_ctl.h: maps physical encoder e (1-16) on page p to its turn/push
 * ids under the loaded scene's wiring, 0 if that page isn't wired for scene controls at all: page must be in range
 * (1..scene pages, or any page >= 1 when the scene doesn't say how many it
 * has) before pad_pages/settings_page are even consulted. An out-of-range
 * page (e.g. page 0, or beyond the scene's page count) is simply unwired,
 * the same fail-safe treatment as a page nothing was assigned to. */
void rt_ctl_ids(int page, int enc, int *turn_id, int *push_id)
{
    *turn_id = 0;
    *push_id = 0;
    if (page < 1 || (g_pages > 0 && page > g_pages)) {
        return;
    }

    bool is_pad = (g_pad_pages > 0 && page <= g_pad_pages) ||
                  (g_pad_pages == 0 && page == 1);
    if (is_pad) {
        *turn_id = enc;
        *push_id = 16 + enc;
    } else if (g_settings_page != 0 && page == g_settings_page) {
        *turn_id = 32 + enc;
        *push_id = 48 + enc;
    }
}

static void load_wiring(const rt_scene_t *scene)
{
    if (scene) {
        g_pages = scene->pages;
        g_pad_pages = scene->pad_pages;
        g_settings_page = scene->settings_page;
        memcpy(g_assigned, scene->assigned, sizeof g_assigned);
    } else {
        g_pages = 0;
        g_pad_pages = 0;
        g_settings_page = 0;
        memset(g_assigned, 0, sizeof g_assigned);
    }
}

void rt_api_ctl_set_wiring(const rt_scene_t *scene)
{
    load_wiring(scene);
}

/* --- controller.* --- */

static int l_controller_getPage(lua_State *L)
{
    lua_pushinteger(L, g_page);
    return 1;
}

static int l_controller_set(lua_State *L)
{
    /* A no-op that accepts the stock argument shapes: controller.set(id, t). */
    (void)L;
    return 0;
}

/* Also no-ops in the mock (controller.setByIndex/setControls = function()
 * end): accept any arguments so a stock script calling them doesn't raise
 * "attempt to call a nil value". */
static int l_controller_setByIndex(lua_State *L)
{
    (void)L;
    return 0;
}

static int l_controller_setControls(lua_State *L)
{
    (void)L;
    return 0;
}

static int l_controller_get(lua_State *L)
{
    (void)L;
    lua_pushnil(L);
    return 1;
}

static int l_controller_setHoldTime(lua_State *L)
{
    lua_Number ms = luaL_checknumber(L, 1);
    if (ms == 0) {
        g_hold_ms = 0.0f;
    } else {
        lua_Number clamped = ms < 30 ? 30 : (ms > 3000 ? 3000 : ms);
        g_hold_ms = (float)clamped;
    }
    return 0;
}

/* Resets current page, hold arming and every encoder's press state.
 * Shared by rt_api_ctl_open() (a new script) and rt_api_ctl_close()
 * (the script is gone; nothing should fire into it anymore). */
static void reset_state(void)
{
    g_page = 1;
    g_hold_ms = 0.0f;
    memset(g_press, 0, sizeof g_press);
}

void rt_api_ctl_open(lua_State *L, const rt_scene_t *scene)
{
    load_wiring(scene);
    reset_state();

    lua_newtable(L);
    lua_pushcfunction(L, l_controller_getPage);
    lua_setfield(L, -2, "getPage");
    lua_pushcfunction(L, l_controller_set);
    lua_setfield(L, -2, "set");
    lua_pushcfunction(L, l_controller_setByIndex);
    lua_setfield(L, -2, "setByIndex");
    lua_pushcfunction(L, l_controller_setControls);
    lua_setfield(L, -2, "setControls");
    lua_pushcfunction(L, l_controller_get);
    lua_setfield(L, -2, "get");
    lua_pushcfunction(L, l_controller_setHoldTime);
    lua_setfield(L, -2, "setHoldTime");
    lua_setglobal(L, "controller");
}

/* Called from rt_unload(): the script's lua_State is going away, so there
 * is nothing left to call into. Resets the same state rt_api_ctl_open()
 * resets for a fresh script, leaving the wiring alone (that's scene
 * state, not script state). */
void rt_api_ctl_close(void)
{
    reset_state();
}

/* --- event tables --- */

static void push_base_fields(lua_State *L, int id, int index, int page)
{
    lua_pushinteger(L, id);
    lua_setfield(L, -2, "id");
    lua_pushinteger(L, index);
    lua_setfield(L, -2, "index");
    lua_pushinteger(L, page);
    lua_setfield(L, -2, "page");
    lua_pushinteger(L, 8192);
    lua_setfield(L, -2, "value");
    lua_pushinteger(L, 64);
    lua_setfield(L, -2, "scaled");
}

void rt_turn(int enc, int increment)
{
    if (enc < 1 || enc > 16) {
        return;
    }
    int turn_id, push_id;
    rt_ctl_ids(g_page, enc, &turn_id, &push_id);
    (void)push_id;
    if (!turn_id || !is_assigned(turn_id)) {
        return;
    }

    lua_State *L = rt_state();
    if (!L) {
        return;
    }
    lua_newtable(L);
    push_base_fields(L, turn_id, enc, g_page);
    lua_pushinteger(L, increment);
    lua_setfield(L, -2, "increment");
    lua_pushboolean(L, false);
    lua_setfield(L, -2, "is_held");
    rt_call("controller.onEncoderTurn", 1);
}

void rt_press(int enc)
{
    if (enc < 1 || enc > 16) {
        return;
    }
    int turn_id, push_id;
    rt_ctl_ids(g_page, enc, &turn_id, &push_id);
    (void)turn_id;
    bool fire = push_id && is_assigned(push_id);

    press_state_t *ps = &g_press[enc - 1];
    ps->down = true;
    ps->fire = fire;
    ps->id = push_id;
    ps->page = g_page;
    ps->press_time = rt_port_now();
    ps->hold_fired = false;

    if (!fire) {
        return;
    }
    lua_State *L = rt_state();
    if (!L) {
        return;
    }
    lua_newtable(L);
    push_base_fields(L, push_id, enc, g_page);
    rt_call("controller.onEncoderPress", 1);
}

void rt_release(int enc)
{
    if (enc < 1 || enc > 16) {
        return;
    }
    press_state_t *ps = &g_press[enc - 1];
    if (!ps->down) {
        return; /* no matching press: nothing to release */
    }
    ps->down = false;

    if (!ps->fire) {
        return;
    }
    lua_State *L = rt_state();
    if (!L) {
        return;
    }
    int held_ms = (int)(rt_port_now() - ps->press_time);
    lua_newtable(L);
    push_base_fields(L, ps->id, enc, ps->page);
    lua_pushinteger(L, held_ms);
    lua_setfield(L, -2, "held_ms");
    rt_call("controller.onEncoderRelease", 1);
}

/* page is whatever the caller passes, including out of range (0, or
 * beyond the scene's page count): rt_show_page doesn't validate it, since
 * rt_ctl_ids() already treats an out-of-range current page as unwired
 * (fail-safe) rather than rejecting the show itself. */
void rt_show_page(int page)
{
    int prev = g_page;
    g_page = page;
    lua_State *L = rt_state();
    if (!L) {
        return;
    }
    lua_pushinteger(L, prev);
    lua_pushinteger(L, page);
    rt_call("page.onPageChange", 2);
}

int rt_page(void)
{
    return g_page;
}

/* Sets the current page without calling page.onPageChange: for the host
 * harness's mock bridge (test/host/e16mock.lua), which needs to seed the page controller.getPage() reports (e.g.
 * restoring where a scene re-entry left off) without replaying a page
 * transition the script never asked for. */
void rt_set_page_silent(int page)
{
    g_page = page;
}

/* --- hold ---
 *
 * Live semantics (matching the mock: M.release reads M.hold at release
 * time, not at press time): controller.setHoldTime() applies to every
 * press still down, including one already in flight. setHoldTime(0)
 * disarms a held-down press just as it does a future one.
 *
 * Within a single rt_api_ctl_tick() pass, though, g_hold_ms is read once
 * into a local and that local is used for every press checked in the
 * pass -- not re-read per press. This is the actual re-entrancy fix: an
 * onEncoderHold callback that calls setHoldTime() (commonly to disarm
 * itself) must not retroactively change the threshold other presses in
 * *this same pass* are evaluated against, since they were already due
 * under the threshold the pass started with. The new value takes effect
 * from the next pass (and next_due() call) on.
 *
 * On the device, this is driven from the main loop's ~1 ms tick, so a hold
 * can fire up to ~1 ms later than its exact due time, never earlier. The
 * host harness's rt.advance() (test/host/rt_module.c) lands on due times
 * exactly instead, since it steps straight to them. */

void rt_api_ctl_tick(rt_time_t now)
{
    /* No script loaded: nothing to call into, and nothing should be
     * marked as fired without actually firing (a later rt_load() starts
     * every press fresh via rt_api_ctl_open() anyway, but don't rely on
     * that from here). */
    lua_State *L = rt_state();
    if (!L) {
        return;
    }
    float hold_ms = g_hold_ms; /* one snapshot for the whole pass */
    for (int i = 0; i < 16; i++) {
        press_state_t *ps = &g_press[i];
        if (!ps->down || ps->hold_fired || !ps->fire || hold_ms <= 0.0f) {
            continue;
        }
        if (now - ps->press_time < (rt_time_t)hold_ms) {
            continue;
        }
        ps->hold_fired = true;
        lua_newtable(L);
        push_base_fields(L, ps->id, i + 1, ps->page);
        rt_call("controller.onEncoderHold", 1);
    }
}

rt_time_t rt_api_ctl_next_due(void)
{
    if (g_hold_ms <= 0.0f) {
        return RT_TIME_NEVER;
    }
    rt_time_t due = RT_TIME_NEVER;
    for (int i = 0; i < 16; i++) {
        press_state_t *ps = &g_press[i];
        if (!ps->down || ps->hold_fired || !ps->fire) {
            continue;
        }
        rt_time_t t = ps->press_time + (rt_time_t)g_hold_ms;
        if (t < due) {
            due = t;
        }
    }
    return due;
}

float rt_api_ctl_hold_ms(void)
{
    return g_hold_ms;
}
