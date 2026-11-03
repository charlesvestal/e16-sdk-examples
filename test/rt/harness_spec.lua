-- test/rt/harness_spec.lua: the host module's mock-bridge accessors, which
-- test/host/e16mock.lua builds the oxi-e16-lua mock interface on:
-- runtime introspection (clock state, clock errors, hold time, page), the
-- var slots as the mock's M.store, the sent log, and the cross-state
-- bridge (rt.get/rt.set) between the harness and the script's state.
local S = dofile("test/rt/spec.lua")
local rt = require("rt")

rt.reset()
assert(rt.load_string("h", [[
function page.onInit() end
function system.update() end
function clock.onPulse() end
function twice(x) return x * 2, "ok" end
function boom() error("script boom") end
function echo(t) return t end
function callsend() midi.sendCC(0, 0, 1, 2) end
]]))

-- --- clock_info: the mock's listening, res, bpm, tp, cerrs, hold ---
do
  local listening, res, bpm, tp, cerrs, hold = rt.clock_info()
  S.check(listening == false and res == 16 and bpm == 120 and math.type(bpm) == "integer"
    and tp == nil and cerrs == 0 and hold == 0, "clock_info at load: off, 16, 120, stopped, 0 errors, no hold")
  rt.eval("clock.listen(true, 96); clock.setInternalBpm(97.5); clock.startInternal(); controller.setHoldTime(10)")
  listening, res, bpm, tp, cerrs, hold = rt.clock_info()
  S.check(listening and res == 96 and bpm == 97.5 and tp == 2 and hold == 30,
    "clock_info follows listen/setInternalBpm/startInternal/setHoldTime")
  rt.eval("function clock.onPulse() error('pulse boom') end")
  rt.advance(100)
  cerrs = select(5, rt.clock_info())
  S.check(cerrs > 1, "clock_info counts clock callback errors (" .. cerrs .. ")")
  rt.eval("clock.stopInternal(); function clock.onPulse() end")
  rt.ext_start(100)
  S.check(select(4, rt.clock_info()) == 0, "tp 0 under external transport")
  rt.ext_stop()
  rt.advance(1)
  S.check(select(4, rt.clock_info()) == nil, "tp nil when stopped")
end

-- --- page ---
rt.set_page(3)
S.check(rt.page() == 3 and rt.eval("return controller.getPage()") == 3, "rt.page() is controller.getPage()")
rt.set_page(1)

-- --- vars as the mock's M.store ---
do
  rt.eval("var.register('a', 'int', 5)")
  S.check(rt.vars().a == 5, "rt.vars() lists registered vars")
  rt.var_put("a", 9)
  rt.var_put("fresh", 3)
  S.check(rt.eval("return var.get('a')") == 9 and rt.eval("return var.get('fresh')") == 3,
    "rt.var_put edits and creates vars the script sees")
  rt.var_put("fresh", nil)
  S.check(rt.vars().fresh == nil and rt.eval("return var.get('fresh')") == nil, "rt.var_put(name, nil) removes")
  rt.vars_clear()
  S.check(next(rt.vars()) == nil, "rt.vars_clear() empties every slot")
  for i = 1, 32 do rt.var_put("v" .. i, i) end
  S.check(not pcall(rt.var_put, "v33", 1), "a 33rd var is refused")
  rt.vars_clear()
  -- Seeding before a load of a different scene: var_bind keeps them.
  rt.var_put("seed", 42)
  rt.var_bind("other")
  assert(rt.load_string("other", "function page.onInit() SEEN = var.get('seed') end"))
  S.check(rt.eval("return SEEN") == 42, "var_bind: seeded vars survive the next scene's load")
  assert(rt.load_string("third", "function page.onInit() SEEN = var.get('seed') end"))
  S.check(rt.eval("return SEEN") == nil, "without var_bind, a different scene still starts empty")
end

-- --- sent log ---
do
  assert(rt.load_string("h", [[
function page.onInit() end
function burst(n) for i = 1, n do midi.sendCC(0, 0, 1, i % 128) end end
]]))
  rt.eval("burst(1000)")
  S.check(rt.sent_count() == 1000 and #rt.sent() == 1000, "sent log holds 1000 messages (no 256 cap)")
  local tail = rt.sent(998)
  S.check(#tail == 3 and tail[1].d2 == 998 % 128 and tail[3].d2 == 1000 % 128, "rt.sent(from) returns from index on")
  rt.clear_sent()
  S.check(rt.sent_count() == 0, "clear_sent empties it")
end

-- --- the bridge ---
assert(rt.load_string("h", [[
function page.onInit() end
function twice(x) return x * 2, "ok" end
function boom() error("script boom") end
function echo(t) return t end
function callsend() midi.sendCC(0, 0, 1, 2) end
function callpcall() return pcall(midi.sendCC, 0, 0, 1, 2) end
]]))
do
  local twice = rt.get("twice")
  local a, b = twice(21)
  S.check(a == 42 and b == "ok", "a script function called from the harness, results copied back")
  S.check(rt.get("missing.path") == nil and rt.get("page.nothing") == nil, "missing paths read as nil")
  local ok, err = pcall(rt.get("boom"))
  S.check(not ok and tostring(err):find("script boom") ~= nil, "script errors re-raise in the harness")
  local t = rt.get("echo")({id = 3, sub = {1, 2, x = "y"}})
  S.check(t.id == 3 and t.sub[2] == 2 and t.sub.x == "y", "tables copy deeply both ways")

  local calls, last = 0, nil
  local real = rt.get("midi.sendCC")
  rt.set("midi.sendCC", function(o, c, cc, v) calls = calls + 1; last = v end)
  rt.eval("callsend()")
  S.check(calls == 1 and last == 2 and rt.sent_count() == 0, "a harness stub replaces the script's API function")
  rt.set("midi.sendCC", function() error("stub boom") end)
  local okp = rt.eval("local ok, e = callpcall(); return tostring(ok) .. ' ' .. tostring(e)")
  S.check(okp:find("^false") and okp:find("stub boom") ~= nil, "a stub's error reaches the script as a Lua error")
  rt.set("midi.sendCC", real)
  rt.eval("callsend()")
  S.check(rt.sent_count() == 1, "storing the handle back restores the native function")

  -- a stub that calls through to the original, re-entering the script
  local st = rt.get("page.setTitle")
  local seen
  rt.set("page.setTitle", function(s) seen = s; st(s) end)
  rt.eval("page.setTitle('Hi')")
  S.check(seen == "Hi" and rt.title() == "Hi", "a stub can call the original (nested call into the script)")
  rt.set("page.setTitle", st)

  -- handles from a previous load are refused, not dangling
  assert(rt.load_string("h", "function page.onInit() end"))
  S.check(not pcall(twice, 1), "a handle from an unloaded script raises")
end

-- --- gc on the script's state ---
do
  rt.gc("collect")
  rt.gc("stop")
  local k0 = rt.gc("count")
  rt.eval("KEEP = {} for i = 1, 100 do KEEP[i] = {i} end")
  S.check(rt.gc("count") > k0 and rt.gc("isrunning") == false, "rt.gc counts the script's heap and stops its GC")
  rt.gc("restart")
  S.check(rt.gc("isrunning") == true, "rt.gc('restart')")
end

S.done()
