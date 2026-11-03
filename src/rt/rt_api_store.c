/*
 * store: store.data, a table up to 1 KiB (store.capacity()), saved on
 * unload and restored (a fresh table) on load, per the mock
 * (external/oxi-e16-lua/test/e16mock.lua's store/ssize/M.load) and the
 * notes (docs/e16-lua-notes.md, "Store" under "API 1.3.0: what changed").
 *
 * store.size() mirrors the mock's ssize() exactly, including its errors
 * ("store: only integers here (...)", "store can't hold a ...") and its
 * byte-cost rule (booleans 1; strings #s+2; integers 1/2/5 by magnitude;
 * tables 2 + sum(ssize(key)+ssize(value))) -- that's the number the task
 * says has to match the mock, and it's also what commit (on unload) uses
 * to enforce the 1024-byte cap.
 *
 * The actual saved encoding is a separate, denser tagged-byte format (not
 * ssize()'s accounting units): T table (u16 count, then count*(key,value)
 * pairs, depth capped at 8), S string (u8 length, bytes), I integer
 * (zigzag varint), B boolean (1 byte), F float32 (4 bytes). Keys are
 * restricted to strings and numbers (the task's decision, stricter than
 * the mock, which never writes a store key itself). Floats aren't
 * something the mock's scripts put in store.data (ssize() asserts on
 * them), but the encoding supports them for a future script that does;
 * only the mock-compatible ssize() path rejects them.
 */
#include "rt_api_store.h"
#include "rt.h"
#include "rt_port.h"
#include "ccm.h"

#include "lauxlib.h"

#include <stdbool.h>
#include <string.h>

/* Logical cap the task specifies (store.capacity(), and the limit
 * commit() enforces via ssize()). The actual byte buffers are sized
 * generously above this: the tagged encoding's per-value overhead (a tag
 * byte, a 2-byte table count) can run a bit over ssize()'s accounting for
 * content near the cap, and this keeps that from ever truncating a
 * commit that ssize() accepted. */
#define RT_STORE_MAX_SSIZE 1024
#define RT_STORE_BUF_CAP RT_STORE_SERIALIZED_MAX /* rt.h */

enum {
    TAG_TABLE = 'T',
    TAG_STRING = 'S',
    TAG_INT = 'I',
    TAG_BOOL = 'B',
    TAG_FLOAT = 'F',
};

static uint8_t g_scene_buf[RT_STORE_BUF_CAP] CCM_BSS;
static size_t g_scene_len = 0;
static uint8_t g_shared_buf[RT_STORE_BUF_CAP] CCM_BSS;
static size_t g_shared_len = 0;
static char g_scene_name[RT_SCENE_NAME_MAX + 1] = "";
static int g_target = 0; /* 0 = scene, 1 = shared */

/* Nesting cap shared with the saved encoding's store_write_table()/
 * store_read_table(): without it, a self-referential table
 * (t.self = t) recurses forever (crashing store.size() and, via
 * store_commit_protected(), rt_unload()). The mock has no such table to
 * reject, so there's no mock text to match; this reuses the encoder's
 * own "nested too deep" wording for both. */
#define RT_STORE_MAX_DEPTH 8

/* --- store.size(): the mock's ssize(), verbatim (plus the depth cap
 * above, which the mock doesn't need because it only ever runs on
 * `store.data` tables a script actually built, not an adversarial one). --- */

static lua_Integer store_ssize(lua_State *L, int idx, int depth)
{
    if (depth > RT_STORE_MAX_DEPTH) {
        return luaL_error(L, "store: table nested too deep");
    }
    idx = lua_absindex(L, idx);
    int t = lua_type(L, idx);
    if (t == LUA_TBOOLEAN) {
        return 1;
    }
    if (t == LUA_TSTRING) {
        size_t len;
        lua_tolstring(L, idx, &len);
        return (lua_Integer)len + 2;
    }
    if (t == LUA_TNUMBER) {
        if (!lua_isinteger(L, idx)) {
            return luaL_error(L, "store: only integers here (%s)", lua_tostring(L, idx));
        }
        lua_Integer v = lua_tointeger(L, idx);
        if (v >= 0 && v < 128) {
            return 1;
        }
        if (v < 16384 && v > -16384) {
            return 2;
        }
        return 5;
    }
    if (t == LUA_TTABLE) {
        lua_Integer n = 2;
        lua_pushvalue(L, idx);
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            n += store_ssize(L, -2, depth + 1);
            n += store_ssize(L, -1, depth + 1);
            lua_pop(L, 1); /* value; keep key for lua_next */
        }
        lua_pop(L, 1); /* the table copy */
        return n;
    }
    return luaL_error(L, "store can't hold a %s", lua_typename(L, t));
}

static int l_store_size(lua_State *L)
{
    lua_getglobal(L, "store");
    lua_getfield(L, -1, "data");
    lua_Integer sz = store_ssize(L, -1, 1);
    lua_pushinteger(L, sz);
    return 1;
}

static int l_store_capacity(lua_State *L)
{
    lua_pushinteger(L, RT_STORE_MAX_SSIZE);
    return 1;
}

/* --- the saved tagged-byte encoding. --- */

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    bool overflow;
} wbuf_t;

static void wb_put(wbuf_t *w, const void *data, size_t n)
{
    if (w->overflow) {
        return;
    }
    if (w->len + n > w->cap) {
        w->overflow = true;
        return;
    }
    memcpy(w->buf + w->len, data, n);
    w->len += n;
}

static void wb_u8(wbuf_t *w, uint8_t v)
{
    wb_put(w, &v, 1);
}

static void wb_varint_u32(wbuf_t *w, uint32_t v)
{
    while (v >= 0x80) {
        wb_u8(w, (uint8_t)(v | 0x80));
        v >>= 7;
    }
    wb_u8(w, (uint8_t)v);
}

static void wb_zigzag_i32(wbuf_t *w, int32_t v)
{
    uint32_t zz = ((uint32_t)v << 1) ^ (uint32_t)(v >> 31);
    wb_varint_u32(w, zz);
}

/* Explicit little-endian bytes (not a raw memcpy of the float, which
 * would depend on the host's endianness matching the device's): the
 * same packing var's int32 encoding uses. */
static void wb_float32(wbuf_t *w, float f)
{
    uint32_t bits;
    memcpy(&bits, &f, sizeof bits); /* bit pattern via memcpy: no UB, no aliasing */
    wb_u8(w, (uint8_t)(bits & 0xFF));
    wb_u8(w, (uint8_t)((bits >> 8) & 0xFF));
    wb_u8(w, (uint8_t)((bits >> 16) & 0xFF));
    wb_u8(w, (uint8_t)((bits >> 24) & 0xFF));
}

static void store_write_value(lua_State *L, int idx, wbuf_t *w, int depth);

static void store_write_table(lua_State *L, int idx, wbuf_t *w, int depth)
{
    if (depth > RT_STORE_MAX_DEPTH) {
        luaL_error(L, "store: table nested too deep");
    }
    idx = lua_absindex(L, idx);

    lua_Integer count = 0;
    lua_pushvalue(L, idx);
    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
        count++;
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    if (count > 0xFFFF) {
        luaL_error(L, "store: table too large");
    }

    wb_u8(w, (uint8_t)TAG_TABLE);
    wb_u8(w, (uint8_t)(count & 0xFF));
    wb_u8(w, (uint8_t)((count >> 8) & 0xFF));

    lua_pushvalue(L, idx);
    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
        int keytype = lua_type(L, -2);
        if (keytype != LUA_TSTRING && keytype != LUA_TNUMBER) {
            luaL_error(L, "store can't hold a %s key", lua_typename(L, keytype));
        }
        store_write_value(L, -2, w, depth + 1);
        store_write_value(L, -1, w, depth + 1);
        lua_pop(L, 1); /* value; keep key for lua_next */
    }
    lua_pop(L, 1); /* the table copy */
}

static void store_write_value(lua_State *L, int idx, wbuf_t *w, int depth)
{
    idx = lua_absindex(L, idx);
    int t = lua_type(L, idx);
    switch (t) {
        case LUA_TBOOLEAN:
            wb_u8(w, (uint8_t)TAG_BOOL);
            wb_u8(w, lua_toboolean(L, idx) ? 1 : 0);
            break;
        case LUA_TSTRING: {
            size_t len;
            const char *s = lua_tolstring(L, idx, &len);
            if (len > 255) {
                luaL_error(L, "store: string too long");
            }
            wb_u8(w, (uint8_t)TAG_STRING);
            wb_u8(w, (uint8_t)len);
            wb_put(w, s, len);
            break;
        }
        case LUA_TNUMBER:
            if (lua_isinteger(L, idx)) {
                wb_u8(w, (uint8_t)TAG_INT);
                wb_zigzag_i32(w, (int32_t)lua_tointeger(L, idx));
            } else {
                wb_u8(w, (uint8_t)TAG_FLOAT);
                wb_float32(w, (float)lua_tonumber(L, idx));
            }
            break;
        case LUA_TTABLE:
            store_write_table(L, idx, w, depth);
            break;
        default:
            luaL_error(L, "store can't hold a %s", lua_typename(L, t));
    }
}

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
} rbuf_t;

static bool rb_varint_u32(rbuf_t *r, uint32_t *out)
{
    uint32_t result = 0;
    int shift = 0;
    while (r->p < r->end) {
        uint8_t b = *r->p++;
        result |= ((uint32_t)(b & 0x7F)) << shift;
        if (!(b & 0x80)) {
            *out = result;
            return true;
        }
        shift += 7;
        if (shift >= 32) {
            return false;
        }
    }
    return false;
}

static int32_t zigzag_decode(uint32_t zz)
{
    return (int32_t)(zz >> 1) ^ -(int32_t)(zz & 1);
}

/* Inverse of wb_float32(): explicit little-endian bytes -> bit pattern
 * -> float via memcpy (no UB, no aliasing). */
static float rb_float32(const uint8_t *p)
{
    uint32_t bits = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

static bool store_read_value(lua_State *L, rbuf_t *r, int depth);

static bool store_read_table(lua_State *L, rbuf_t *r, int depth)
{
    if (depth > RT_STORE_MAX_DEPTH) {
        return false;
    }
    if (r->p + 2 > r->end) {
        return false;
    }
    uint16_t count = (uint16_t)(r->p[0] | (r->p[1] << 8));
    r->p += 2;
    lua_newtable(L);
    for (uint16_t i = 0; i < count; i++) {
        if (!store_read_value(L, r, depth + 1)) {
            lua_pop(L, 1);
            return false;
        }
        if (!store_read_value(L, r, depth + 1)) {
            lua_pop(L, 2);
            return false;
        }
        lua_settable(L, -3);
    }
    return true;
}

static bool store_read_value(lua_State *L, rbuf_t *r, int depth)
{
    if (r->p >= r->end) {
        return false;
    }
    uint8_t tag = *r->p++;
    switch (tag) {
        case TAG_BOOL:
            if (r->p >= r->end) {
                return false;
            }
            lua_pushboolean(L, *r->p != 0);
            r->p++;
            return true;
        case TAG_STRING: {
            if (r->p >= r->end) {
                return false;
            }
            uint8_t len = *r->p++;
            if (r->p + len > r->end) {
                return false;
            }
            lua_pushlstring(L, (const char *)r->p, len);
            r->p += len;
            return true;
        }
        case TAG_INT: {
            uint32_t zz;
            if (!rb_varint_u32(r, &zz)) {
                return false;
            }
            lua_pushinteger(L, zigzag_decode(zz));
            return true;
        }
        case TAG_FLOAT: {
            if (r->p + 4 > r->end) {
                return false;
            }
            float f = rb_float32(r->p);
            r->p += 4;
            lua_pushnumber(L, (lua_Number)f);
            return true;
        }
        case TAG_TABLE:
            return store_read_table(L, r, depth);
        default:
            return false;
    }
}

/* Pushes exactly one table: the one decoded from buf[0..len), or a fresh
 * empty one if len is 0 or the bytes don't decode to a table. */
static void build_table_from_buf(lua_State *L, const uint8_t *buf, size_t len)
{
    int top0 = lua_gettop(L);
    rbuf_t r = {.p = buf, .end = buf + len};
    bool ok = len > 0 && store_read_value(L, &r, 1) && lua_istable(L, -1);
    if (!ok) {
        lua_settop(L, top0);
        lua_newtable(L);
    }
}

static void set_store_data(lua_State *L)
{
    const uint8_t *buf = g_target == 0 ? g_scene_buf : g_shared_buf;
    size_t len = g_target == 0 ? g_scene_len : g_shared_len;
    lua_getglobal(L, "store");
    build_table_from_buf(L, buf, len);
    lua_setfield(L, -2, "data");
    lua_pop(L, 1); /* store table */
}

static int l_store_use(lua_State *L)
{
    const char *which = luaL_checkstring(L, 1);
    g_target = (strcmp(which, "shared") == 0) ? 1 : 0;
    set_store_data(L);
    return 0;
}

void rt_api_store_open(lua_State *L, const rt_scene_t *scene)
{
    const char *name = (scene && scene->name) ? scene->name : "";
    if (strcmp(name, g_scene_name) != 0) {
        g_scene_len = 0;
        strncpy(g_scene_name, name, sizeof g_scene_name - 1);
        g_scene_name[sizeof g_scene_name - 1] = '\0';
    }
    g_target = 0;

    lua_newtable(L);
    lua_pushcfunction(L, l_store_use);
    lua_setfield(L, -2, "use");
    lua_pushcfunction(L, l_store_size);
    lua_setfield(L, -2, "size");
    lua_pushcfunction(L, l_store_capacity);
    lua_setfield(L, -2, "capacity");
    lua_setglobal(L, "store");

    set_store_data(L);
}

/* Runs through lua_pcall so a luaL_error from store_ssize()/
 * store_write_value() (an oversized or ill-typed store.data) is caught
 * here rather than unwinding into rt_unload()'s caller. */
static int store_commit_protected(lua_State *L)
{
    lua_getglobal(L, "store");
    lua_getfield(L, -1, "data");
    if (!lua_istable(L, -1)) {
        return luaL_error(L, "store: data must be a table");
    }
    lua_Integer sz = store_ssize(L, -1, 1);
    if (sz > RT_STORE_MAX_SSIZE) {
        return luaL_error(L, "store over 1024 bytes");
    }

    uint8_t scratch[RT_STORE_BUF_CAP];
    wbuf_t w = {.buf = scratch, .cap = sizeof scratch, .len = 0, .overflow = false};
    store_write_value(L, -1, &w, 1);
    if (w.overflow) {
        return luaL_error(L, "store serialize overflow");
    }

    uint8_t *tbuf = g_target == 0 ? g_scene_buf : g_shared_buf;
    size_t *tlen = g_target == 0 ? &g_scene_len : &g_shared_len;
    memcpy(tbuf, w.buf, w.len);
    *tlen = w.len;
    return 0;
}

void rt_api_store_close(lua_State *L)
{
    if (!L) {
        return;
    }
    lua_pushcfunction(L, store_commit_protected);
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        if (msg) {
            rt_port_log(msg, strlen(msg));
        }
        lua_pop(L, 1);
    }
}

void rt_api_store_sync(lua_State *L)
{
    if (!L) {
        return;
    }
    set_store_data(L);
}

void rt_api_store_bind_scene(const char *name)
{
    strncpy(g_scene_name, name ? name : "", sizeof g_scene_name - 1);
    g_scene_name[sizeof g_scene_name - 1] = '\0';
}

/* --- rt.h's persistence API: a plain copy of the scene buffer's bytes
 * (already the tagged encoding above; opaque to the caller). --- */

size_t rt_store_serialize(uint8_t *out, size_t max)
{
    if (g_scene_len > max) {
        return RT_SERIALIZE_FAILED;
    }
    memcpy(out, g_scene_buf, g_scene_len);
    return g_scene_len;
}

bool rt_store_serialized_empty(const uint8_t *buf, size_t len)
{
    /* Nothing yet, or store_write_table()'s empty table: the tag and a
     * count of 0. */
    return len == 0 || (len == 3 && buf[0] == (uint8_t)TAG_TABLE && buf[1] == 0 && buf[2] == 0);
}

void rt_store_deserialize(const uint8_t *in, size_t len)
{
    if (len > sizeof g_scene_buf) {
        len = sizeof g_scene_buf;
    }
    memset(g_scene_buf, 0, sizeof g_scene_buf);
    if (len > 0) {
        memcpy(g_scene_buf, in, len);
    }
    g_scene_len = len;
}

/* Same contract as rt_store_serialize/deserialize, for the "shared"
 * buffer (store.use("shared")): it isn't keyed by scene name at all, so
 * a platform persisting it just needs these two, not a scene name to go
 * with them. */
size_t rt_store_shared_serialize(uint8_t *out, size_t max)
{
    if (g_shared_len > max) {
        return RT_SERIALIZE_FAILED;
    }
    memcpy(out, g_shared_buf, g_shared_len);
    return g_shared_len;
}

void rt_store_shared_deserialize(const uint8_t *in, size_t len)
{
    if (len > sizeof g_shared_buf) {
        len = sizeof g_shared_buf;
    }
    memset(g_shared_buf, 0, sizeof g_shared_buf);
    if (len > 0) {
        memcpy(g_shared_buf, in, len);
    }
    g_shared_len = len;
}
