/*
 * var: 32 packed-variable slots, outside the Lua heap, per the mock
 * (external/oxi-e16-lua/test/e16mock.lua's `var` table) and the firmware
 * notes (docs/e16-lua-notes.md, "Variables (var)").
 *
 * Deviations from the mock, both per the task text:
 *  - register() on a full set of 32 (all distinct names) silently drops
 *    the registration (get() on that name returns nil) instead of the
 *    mock's assert. The notes say the firmware drops; the mock's assert
 *    only exists so a test runner notices an unintended 33rd var.
 *  - values are 32-bit integers (int32_t), not arbitrary Lua values: a
 *    non-integer default/set value is floored like rt_api_out.c's ring
 *    values, since the device needs an actual int32 to pack. Unlike
 *    rt_api_out.c's ranges (which clamp values the device can still
 *    represent, e.g. a ring index), a var value that doesn't fit in
 *    int32 after flooring has no valid representation at all, so it's a
 *    Lua error instead of a clamp.
 *
 * Names are compared and stored by byte length (the mock's #n), not as
 * NUL-terminated C strings: a Lua string can contain an embedded '\0',
 * and two names differing only after one would otherwise collide.
 */
#include "rt_api_var.h"
#include "rt.h"
#include "rt_port.h"

#include "lauxlib.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

/* RT_VAR_MAX and RT_VAR_NAME_MAX are in rt.h (they size
 * RT_VARS_SERIALIZED_MAX). */

typedef struct {
    char name[RT_VAR_NAME_MAX];
    uint8_t namelen;
    int32_t value;
    bool used;
} rt_var_slot_t;

static rt_var_slot_t g_vars[RT_VAR_MAX];
static char g_scene_name[RT_SCENE_NAME_MAX + 1] = "";

/* rt.h's rt_vars_dirty()/rt_vars_clear_dirty(): the platform's
 * save-debounce signal, set by every mutation below. */
static bool g_dirty = false;

static rt_var_slot_t *find_var(const char *name, size_t len)
{
    for (int i = 0; i < RT_VAR_MAX; i++) {
        if (g_vars[i].used && g_vars[i].namelen == len && memcmp(g_vars[i].name, name, len) == 0) {
            return &g_vars[i];
        }
    }
    return NULL;
}

static rt_var_slot_t *find_free_slot(void)
{
    for (int i = 0; i < RT_VAR_MAX; i++) {
        if (!g_vars[i].used) {
            return &g_vars[i];
        }
    }
    return NULL;
}

/* lua_Number (float under LUA_32BITS) -> int32_t: floored like
 * rt_api_out.c's check_floor_range (an explicit (float) cast first keeps
 * -Wdouble-promotion quiet when lua_Number is actually double, as on the
 * device build without LUA_32BITS applied to this target), but range-
 * checked *before* the float -> int32_t cast rather than after: casting a
 * float outside int32_t's range is undefined behavior in C, not a
 * saturating truncation, so the check has to happen on the float value.
 * 2147483648.0f (2^31) is exactly representable in a float and is the
 * tight exclusive upper bound; -2147483648.0f (-2^31, int32_t's min) is
 * the inclusive lower bound. NaN fails both comparisons and falls
 * through to the error, same as +-inf. */
static int32_t check_i32(lua_State *L, int idx)
{
    lua_Number n = luaL_checknumber(L, idx);
    float f = floorf((float)n);
    if (!(f >= -2147483648.0f && f < 2147483648.0f)) {
        luaL_error(L, "var value out of range: %s", lua_tostring(L, idx));
    }
    return (int32_t)f;
}

static int l_var_register(lua_State *L)
{
    size_t len;
    const char *name = luaL_checklstring(L, 1, &len);
    luaL_checkstring(L, 2); /* type string; stored nowhere, like the mock */
    if (len > RT_VAR_NAME_MAX) {
        return luaL_error(L, "var name too long: %s", name);
    }
    if (find_var(name, len) != NULL) {
        return 0; /* already registered: keep its current value */
    }
    int32_t def = check_i32(L, 3);
    rt_var_slot_t *slot = find_free_slot();
    if (!slot) {
        return 0; /* 32 slots full: drop silently (get() stays nil) */
    }
    memcpy(slot->name, name, len);
    slot->namelen = (uint8_t)len;
    slot->value = def;
    slot->used = true;
    g_dirty = true;
    return 0;
}

static int l_var_get(lua_State *L)
{
    size_t len;
    const char *name = luaL_checklstring(L, 1, &len);
    rt_var_slot_t *slot = find_var(name, len);
    if (!slot) {
        lua_pushnil(L);
    } else {
        lua_pushinteger(L, slot->value);
    }
    return 1;
}

static int l_var_set(lua_State *L)
{
    size_t len;
    const char *name = luaL_checklstring(L, 1, &len);
    rt_var_slot_t *slot = find_var(name, len);
    if (slot) {
        slot->value = check_i32(L, 2);
        g_dirty = true;
    }
    return 0;
}

static int l_var_delete(lua_State *L)
{
    size_t len;
    const char *name = luaL_checklstring(L, 1, &len);
    rt_var_slot_t *slot = find_var(name, len);
    if (slot) {
        memset(slot, 0, sizeof *slot);
        g_dirty = true;
    }
    return 0;
}

static int l_var_deleteAll(lua_State *L)
{
    (void)L;
    memset(g_vars, 0, sizeof g_vars);
    g_dirty = true;
    return 0;
}

void rt_api_var_open(lua_State *L, const rt_scene_t *scene)
{
    /* rt_load() has already rejected a scene name over RT_SCENE_NAME_MAX,
     * so this copy always fits without truncating. */
    const char *name = (scene && scene->name) ? scene->name : "";
    if (strcmp(name, g_scene_name) != 0) {
        memset(g_vars, 0, sizeof g_vars);
        strncpy(g_scene_name, name, sizeof g_scene_name - 1);
        g_scene_name[sizeof g_scene_name - 1] = '\0';
    }

    lua_newtable(L);
    lua_pushcfunction(L, l_var_register);
    lua_setfield(L, -2, "register");
    lua_pushcfunction(L, l_var_get);
    lua_setfield(L, -2, "get");
    lua_pushcfunction(L, l_var_set);
    lua_setfield(L, -2, "set");
    lua_pushcfunction(L, l_var_delete);
    lua_setfield(L, -2, "delete");
    lua_pushcfunction(L, l_var_deleteAll);
    lua_setfield(L, -2, "deleteAll");
    lua_setglobal(L, "var");
}

/* --- host harness access (rt_api_var.h) --- */

bool rt_api_var_at(int i, const char **name, size_t *len, int32_t *value)
{
    if (i < 0 || i >= RT_VAR_MAX || !g_vars[i].used) {
        return false;
    }
    *name = g_vars[i].name;
    *len = g_vars[i].namelen;
    *value = g_vars[i].value;
    return true;
}

bool rt_api_var_put(const char *name, size_t len, int32_t value)
{
    if (len > RT_VAR_NAME_MAX) {
        return false;
    }
    rt_var_slot_t *slot = find_var(name, len);
    if (!slot) {
        slot = find_free_slot();
        if (!slot) {
            return false;
        }
        memcpy(slot->name, name, len);
        slot->namelen = (uint8_t)len;
        slot->used = true;
    }
    slot->value = value;
    g_dirty = true;
    return true;
}

void rt_api_var_remove(const char *name, size_t len)
{
    rt_var_slot_t *slot = find_var(name, len);
    if (slot) {
        memset(slot, 0, sizeof *slot);
        g_dirty = true;
    }
}

void rt_api_var_clear(void)
{
    memset(g_vars, 0, sizeof g_vars);
    g_dirty = true;
}

void rt_api_var_bind_scene(const char *name)
{
    strncpy(g_scene_name, name ? name : "", sizeof g_scene_name - 1);
    g_scene_name[sizeof g_scene_name - 1] = '\0';
}

/* --- rt.h's persistence API: a plain blob of the live array. --- */

size_t rt_vars_serialize(uint8_t *out, size_t max)
{
    uint8_t count = 0;
    size_t need = 1;
    for (int i = 0; i < RT_VAR_MAX; i++) {
        if (g_vars[i].used) {
            count++;
            need += 1 + g_vars[i].namelen + 4;
        }
    }
    if (need > max) {
        return RT_SERIALIZE_FAILED;
    }

    uint8_t *p = out;
    *p++ = count;
    for (int i = 0; i < RT_VAR_MAX; i++) {
        if (!g_vars[i].used) {
            continue;
        }
        *p++ = g_vars[i].namelen;
        memcpy(p, g_vars[i].name, g_vars[i].namelen);
        p += g_vars[i].namelen;
        uint32_t v = (uint32_t)g_vars[i].value;
        *p++ = (uint8_t)(v & 0xFF);
        *p++ = (uint8_t)((v >> 8) & 0xFF);
        *p++ = (uint8_t)((v >> 16) & 0xFF);
        *p++ = (uint8_t)((v >> 24) & 0xFF);
    }
    return (size_t)(p - out);
}

bool rt_vars_serialized_empty(const uint8_t *buf, size_t len)
{
    return len == 0 || (len == 1 && buf[0] == 0); /* a count of 0 */
}

void rt_vars_deserialize(const uint8_t *in, size_t len)
{
    memset(g_vars, 0, sizeof g_vars);
    if (len < 1) {
        return;
    }
    size_t pos = 0;
    uint8_t count = in[pos++];
    int slot = 0;
    for (uint8_t i = 0; i < count && slot < RT_VAR_MAX; i++) {
        if (pos >= len) {
            break;
        }
        uint8_t namelen = in[pos++];
        if (namelen > RT_VAR_NAME_MAX || pos + (size_t)namelen + 4 > len) {
            break;
        }
        memcpy(g_vars[slot].name, in + pos, namelen);
        g_vars[slot].namelen = namelen;
        pos += namelen;
        uint32_t v = (uint32_t)in[pos] | ((uint32_t)in[pos + 1] << 8) | ((uint32_t)in[pos + 2] << 16) |
                     ((uint32_t)in[pos + 3] << 24);
        pos += 4;
        g_vars[slot].value = (int32_t)v;
        g_vars[slot].used = true;
        slot++;
    }
}

bool rt_vars_dirty(void)
{
    return g_dirty;
}

void rt_vars_clear_dirty(void)
{
    g_dirty = false;
}
