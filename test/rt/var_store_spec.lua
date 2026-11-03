-- test/rt/var_store_spec.lua: var (32 packed-variable slots) and
-- store.data (a persisted table, up to 1 KiB), per the mock
-- (external/oxi-e16-lua/test/e16mock.lua: var, store, ssize, M.load) and
-- the task's decisions (32-bit int32 values, a dropped 33rd registration
-- instead of the mock's assert).
local S = dofile("test/rt/spec.lua")
local rt = require("rt")

local MOCK = "external/oxi-e16-lua/test/e16mock.lua"

rt.load_string("t", [[
function page.onInit() end
function probe(f) local ok, err = pcall(f); return ok end
]])

-- --- var ---

-- register keeps an existing value (re-registering, as a script's
-- onInit typically does on every load, must not clobber a live edit).
rt.eval("var.register('x', 'int', 5)")
S.check(rt.eval("return var.get('x')") == 5, "register seeds the default")
rt.eval("var.set('x', 9)")
rt.eval("var.register('x', 'int', 5)")
S.check(rt.eval("return var.get('x')") == 9, "re-registering keeps the current value")

-- a 33rd registration (distinct name) is dropped silently: get() -> nil.
rt.eval("var.deleteAll()")
rt.eval([[
  for i = 1, 32 do var.register("v" .. i, "int", i) end
  var.register("v33", "int", 999)
]])
S.check(rt.eval("return var.get('v1')") == 1, "the first 32 registrations land")
S.check(rt.eval("return var.get('v32')") == 32, "the 32nd registration lands")
S.check(rt.eval("return var.get('v33')") == nil, "the 33rd registration is dropped")

-- set on an unregistered name does nothing.
rt.eval("var.deleteAll()")
rt.eval("var.set('nope', 42)")
S.check(rt.eval("return var.get('nope')") == nil, "set on an unregistered name is a no-op")

-- deleteAll empties everything.
rt.eval("var.register('a', 'int', 1); var.register('b', 'int', 2)")
rt.eval("var.deleteAll()")
S.check(rt.eval("return var.get('a')") == nil and rt.eval("return var.get('b')") == nil,
  "deleteAll empties the set")

-- delete removes one name.
rt.eval("var.register('a', 'int', 1); var.register('b', 'int', 2)")
rt.eval("var.delete('a')")
S.check(rt.eval("return var.get('a')") == nil, "delete removes the name")
S.check(rt.eval("return var.get('b')") == 2, "delete leaves other names alone")

-- a name over 16 characters errors (the mock's exact text). rt.eval()
-- (unlike a callback dispatched through rt_call) raises a normal Lua
-- error in the host state rather than logging it, so check the message
-- pcall hands back.
local ok, err = pcall(function() rt.eval("var.register(string.rep('n', 17), 'int', 0)") end)
S.check(not ok, "a 17-char name errors")
S.check(type(err) == "string" and err:find("var name too long", 1, true) ~= nil,
  "the error text matches the mock")

-- values survive rt.load of the *same* script (name "survive").
rt.load_string("survive", [[
function page.onInit() var.register("x", "int", 0) end
]])
rt.eval("var.set('x', 123)")
rt.load_string("survive", [[
function page.onInit() var.register("x", "int", 0) end
]])
S.check(rt.eval("return var.get('x')") == 123, "a var survives reloading the same scene")

-- ...but not a *different* script's vars (the runtime has no notion of
-- script identity beyond the scene name): a different name starts empty.
rt.load_string("other", [[
function page.onInit() end
]])
S.check(rt.eval("return var.get('x')") == nil, "a different scene name starts with no vars")

-- --- store ---

-- store.data round-trips tables, strings, booleans and integers through
-- a reload of the same scene (a scene exit saves it; the mock's M.load).
rt.load_string("storey", [[
function page.onInit() end
]])
rt.eval([[
  store.data = {n = 7, s = "hi", b = true, nested = {1, 2, x = -5}}
]])
rt.load_string("storey", [[
function page.onInit() end
]])
S.check(rt.eval("return store.data.n") == 7, "store.data integer survives a reload")
S.check(rt.eval("return store.data.s") == "hi", "store.data string survives a reload")
S.check(rt.eval("return store.data.b") == true, "store.data boolean survives a reload")
S.check(rt.eval("return store.data.nested.x") == -5, "store.data nested table survives a reload")
S.check(rt.eval("return store.data.nested[1]") == 1 and rt.eval("return store.data.nested[2]") == 2,
  "store.data nested array entries survive a reload")

-- a different scene name starts with an empty store.
rt.load_string("storey2", [[
function page.onInit() end
]])
S.check(type(rt.eval("return store.data")) == "nil" or rt.eval("return next(store.data)") == nil,
  "a different scene name starts with an empty store")

-- store.capacity() is 1024.
S.check(rt.eval("return store.capacity()") == 1024, "store.capacity() is 1024")

-- store.size() matches the mock's ssize() for the same table, across a
-- few shapes (flat, nested, negative, large magnitude). Loading the mock
-- sets `store`/`var`/etc as globals in *this* interpreter (separate from
-- the runtime's own Lua state), so store.size() here is the mock's.
dofile(MOCK)
local shapes = {
  "{a = 1, b = 'hello world', c = true}",
  "{n = 200, m = -300, big = 50000, neg_big = -50000}",
  "{nested = {1, 2, 3, deep = {x = true, y = 'z'}}}",
  "{}",
}
for _, shape in ipairs(shapes) do
  store.data = assert(load("return " .. shape))()
  local expected = store.size()
  rt.load_string("sizecheck", "function page.onInit() end")
  rt.eval("store.data = " .. shape)
  local actual = rt.eval("return store.size()")
  S.check(actual == expected, "store.size() matches the mock's ssize for " .. shape)
end

-- saving over 1024 bytes on unload logs an error and keeps the previous
-- saved data (the task's decision: the mock instead asserts/crashes).
rt.load_string("big", [[
function page.onInit() end
]])
rt.eval("store.data = {small = 1}")
rt.load_string("big", [[  -- reload: commits {small = 1} to the scene buffer
function page.onInit() end
]])
S.check(rt.eval("return store.data.small") == 1, "the small table committed fine")

rt.eval([[
  local big = {}
  for i = 1, 400 do big["k" .. i] = i end
  store.data = big
]])
S.check(rt.eval("return store.size()") > 1024, "the oversized table really is over 1024 by ssize")
rt.load_string("big", [[ -- reload: the oversized commit must be rejected
function page.onInit() end
]])
S.check(rt.log():find("store over 1024 bytes", 1, true) ~= nil,
  "an oversized commit logs 'store over 1024 bytes'")
S.check(rt.eval("return store.data.small") == 1,
  "an oversized commit keeps the previous saved data instead")
S.check(rt.eval("return store.data.k1") == nil,
  "the oversized table itself never got committed")

-- store.use("scene"|"shared") is accepted and selects a distinct buffer:
-- shared data set from one scene is visible (by name) to another.
rt.load_string("shareA", [[
function page.onInit() end
]])
rt.eval([[store.use("shared"); store.data = {fromA = 1}]])
rt.load_string("shareB", [[
function page.onInit() end
]])
rt.eval([[store.use("shared")]])
S.check(rt.eval("return store.data.fromA") == 1, "store.use('shared') shares data across scenes")
rt.eval([[store.use("scene")]])
S.check(rt.eval("return next(store.data)") == nil, "store.use('scene') is the scene's own, empty, buffer")

-- --- rt_vars_serialize/deserialize and rt_store_serialize/deserialize
-- round-trip (rt.persist_roundtrip(), per the task's decision). ---
rt.load_string("persist", [[
function page.onInit() end
]])
rt.eval([[
  var.register("a", "int", 0)
  var.register("b", "int", 0)
  var.set("a", 111)
  var.set("b", -222)
  store.data = {n = 3, s = "roundtrip", b = false, nested = {9, 8, 7}}
]])
rt.persist_roundtrip()
S.check(rt.eval("return var.get('a')") == 111, "persist_roundtrip preserves var a")
S.check(rt.eval("return var.get('b')") == -222, "persist_roundtrip preserves var b")
S.check(rt.eval("return store.data.n") == 3, "persist_roundtrip preserves store.data.n")
S.check(rt.eval("return store.data.s") == "roundtrip", "persist_roundtrip preserves store.data.s")
S.check(rt.eval("return store.data.b") == false, "persist_roundtrip preserves store.data.b")
S.check(rt.eval("return store.data.nested[2]") == 8, "persist_roundtrip preserves a nested table")

-- rt_store_shared_serialize/deserialize round-trip too (persist_roundtrip
-- now covers the shared buffer, not just the scene one).
rt.eval([[store.use("shared"); store.data = {sh = 55}]])
rt.persist_roundtrip()
S.check(rt.eval("return store.data.sh") == 55, "persist_roundtrip preserves the shared store buffer too")
rt.eval([[store.use("scene")]])

-- --- code-review fixes ---

-- 1. A self-referential store.data must not crash store.size() (or a
-- commit on reload): the depth cap errors instead of recursing forever.
rt.load_string("selfref", [[
function page.onInit() end
]])
rt.eval("store.data = {ok = 1}")
rt.load_string("selfref", [[ -- reload: commits {ok = 1} to the scene buffer
function page.onInit() end
]])
S.check(rt.eval("return store.data.ok") == 1, "the good table committed fine")
rt.eval([[
  local t = {}
  t.self = t
  store.data = t
]])
S.check(rt.eval("return pcall(store.size)") == false,
  "a self-referential store.data doesn't crash store.size()")
rt.load_string("selfref", [[ -- reload: the self-referential commit must be rejected
function page.onInit() end
]])
S.check(rt.log():find("nested too deep", 1, true) ~= nil,
  "a self-referential store.data logs 'nested too deep' on commit")
S.check(rt.eval("return store.data.ok") == 1,
  "a self-referential commit keeps the previous saved data instead")

-- 2. check_i32 (var.set/var.register's default) range-checks before the
-- float -> int32_t cast instead of casting an out-of-range or NaN float
-- (undefined behavior): 1e30, -1e30, 1/0 and 0/0 all raise a Lua error.
rt.load_string("ranges", [[
function page.onInit() var.register("x", "int", 0) end
]])
local function var_set_errors(expr)
  return not pcall(function() rt.eval("var.set('x', " .. expr .. ")") end)
end
S.check(var_set_errors("1e30"), "var.set rejects 1e30 instead of casting out of range")
S.check(var_set_errors("-1e30"), "var.set rejects -1e30 instead of casting out of range")
S.check(var_set_errors("1/0"), "var.set rejects +inf")
S.check(var_set_errors("0/0"), "var.set rejects NaN")
S.check(not pcall(function() rt.eval("var.register('y', 'int', 1e30)") end),
  "var.register rejects an out-of-range default too")

-- 3. var names compare by byte length (the mock's #n), not as
-- NUL-terminated C strings: two names differing only after an embedded
-- '\0' are distinct slots.
rt.load_string("nulnames", [[
function page.onInit() end
]])
rt.eval("var.register('ab' .. string.char(0) .. 'cd', 'int', 11)")
rt.eval("var.register('ab' .. string.char(0) .. 'ef', 'int', 22)")
S.check(rt.eval("return var.get('ab' .. string.char(0) .. 'cd')") == 11,
  "an embedded-NUL name keeps its own value")
S.check(rt.eval("return var.get('ab' .. string.char(0) .. 'ef')") == 22,
  "an embedded-NUL name differing only after the NUL is a distinct slot")

-- 4. store.data must be a table at commit: a scalar is rejected like any
-- other commit error (logged, previous saved data kept).
rt.load_string("scalar", [[
function page.onInit() end
]])
rt.eval("store.data = {ok = 1}")
rt.load_string("scalar", [[ -- reload: commits {ok = 1} to the scene buffer
function page.onInit() end
]])
S.check(rt.eval("return store.data.ok") == 1, "the good table committed fine")
rt.eval("store.data = 5")
rt.load_string("scalar", [[ -- reload: the scalar commit must be rejected
function page.onInit() end
]])
S.check(rt.log():find("store: data must be a table", 1, true) ~= nil,
  "a scalar store.data logs 'store: data must be a table' on commit")
S.check(rt.eval("return store.data.ok") == 1,
  "a scalar commit keeps the previous saved data instead")

-- 6. A scene name over RT_SCENE_NAME_MAX (31) fails the load outright
-- rather than being silently truncated (which could alias two different
-- scripts' saved vars/store onto the same name).
local long_name = string.rep("n", 32)
S.check(rt.load_string(long_name, "function page.onInit() end") == false,
  "a 32-char scene name fails the load")
S.check(rt.log():find("scene name too long", 1, true) ~= nil,
  "the failure is logged clearly")

-- --- Device persistence: restore before page.onInit runs ---
--
-- rt_load() calls page.onInit() itself before returning, so a platform
-- that restores a saved var/store only after rt_load() returns is too
-- late: the script has already registered its defaults. The fix is to
-- bind the var/store buffers to the scene name and deserialize *before*
-- calling rt_load() (rt_api_var_bind_scene() + rt_vars_deserialize(),
-- rt_api_store_bind_scene() + rt_store_deserialize()/
-- rt_store_shared_deserialize()). rt.persist_capture()/rt.persist_restore()
-- (test/host/rt_module.c) exercise exactly that sequence.
rt.load_string("preload", [[
function page.onInit() end
]])
rt.eval([[
  var.register("tempo", "int", 20)
  var.set("tempo", 133)
  store.data = {x = 42}
]])
-- store.data only commits to its buffer on scene exit (rt_api_store_close,
-- via rt_unload()/a reload), so commit the scene store with a reload
-- before switching store.use() to "shared" -- switching targets rebuilds
-- the live table from that target's buffer, discarding an uncommitted
-- edit (the "store.use('scene'|'shared')" tests above show the same
-- rule the other way, sharing committed data across scenes).
rt.load_string("preload", [[
function page.onInit() end
]])
S.check(rt.eval("return store.data.x") == 42, "sanity: the scene store committed on reload")

rt.eval([[store.use("shared"); store.data = {sh = 77}]])
rt.load_string("preload", [[  -- reload commits whatever's selected (shared) to its buffer
function page.onInit() end
]])
S.check(rt.eval("store.use('shared'); return store.data.sh") == 77,
  "sanity: the shared store committed on reload")

rt.persist_capture()

-- Simulate a power cycle with rt.reset(): nothing loaded, like a fresh
-- boot before any script starts. (Loading some other scene first would
-- call rt_unload() on it from *inside* the next rt_load(), committing
-- that unrelated script's own store over the same physical scene buffer
-- -- store has only one scene buffer, reused by name -- right after
-- persist_restore() below staged the restored bytes into it and before
-- rt_load() ever got to run. On the real device this can't happen: at
-- boot nothing is loaded yet, so rt_load()'s internal rt_unload() is a
-- no-op, exactly like rt.reset() here.)
rt.reset()

rt.persist_restore("preload")
rt.load_string("preload", [[
function page.onInit()
  preload_tempo_in_onInit = var.get("tempo")
  preload_store_in_onInit = store.data.x
  store.use("shared")
  preload_shared_in_onInit = store.data.sh
  store.use("scene")
end
]])
S.check(rt.eval("return preload_tempo_in_onInit") == 133,
  "a restored var is visible inside page.onInit")
S.check(rt.eval("return preload_store_in_onInit") == 42,
  "a restored store.data is visible inside page.onInit")
S.check(rt.eval("return preload_shared_in_onInit") == 77,
  "a restored shared store is visible inside page.onInit")

S.done()
