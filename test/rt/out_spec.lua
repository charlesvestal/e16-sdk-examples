-- test/rt/out_spec.lua: output APIs of the native runtime.
local S = dofile("test/rt/spec.lua")
local rt = require("rt")

rt.load_string("t", [[
function page.onInit() end
function probe(f) local ok, err = pcall(f); return ok end
]])

rt.eval("midi.sendMidi(0, 0, 0x90, 60, 100)")
local m = rt.sent()[1]
S.check(m and m.out == 0 and m.st == 0x90 and m.d1 == 60 and m.d2 == 100, "sendMidi reaches the port")
rt.eval("midi.sendCC(2, 3, 74, 10)")
m = rt.sent()[2]
S.check(m and m.out == 2 and m.st == 0xB3 and m.d1 == 74 and m.d2 == 10, "sendCC builds the status byte")
S.check(rt.eval("return probe(function() midi.sendCC(0, 0, 128, 0) end)") == false, "sendCC rejects cc 128")
S.check(rt.eval("return probe(function() midi.sendMidi(0, 0, 0x90, 60, 1.5) end)") == false, "sendMidi rejects non-integers")

rt.eval("leds.updateByIndex(3, 8192, 18)")
S.check(rt.rings()[3].v == 8192 and rt.rings()[3].c == 18, "updateByIndex single")
rt.eval("leds.updateByIndex({{1, 100, 6}, {2, 200}})")
S.check(rt.rings()[1].c == 6 and rt.rings()[2].v == 200 and rt.rings()[2].c == 0, "updateByIndex batch")
rt.eval("leds.reset(3)")
S.check(rt.rings()[3] == nil, "leds.reset")

rt.eval("slots.update(5, 'Abcd')")
S.check(rt.labels()[5] == "Abcd", "slots.update")
S.check(rt.eval("return probe(function() slots.update(5, 'Abcde') end)") == false, "label > 4 chars rejected")
S.check(rt.eval("return probe(function() page.setTitle(string.rep('x', 16)) end)") == false, "title > 15 chars rejected")
rt.eval("page.setTitle('EUC')")
S.check(rt.title() == "EUC", "setTitle")

S.check(rt.eval("return launcher.version") == "1.2", "launcher.version")
S.check(rt.eval("return launcher.has('draw')") == true, "launcher.has('draw')")
S.check(rt.eval("return launcher.has('ring')") == true, "launcher.has('ring')")
S.check(rt.eval("return launcher.has('audio')") == true, "launcher.has('audio') is true")
S.check(rt.eval("return launcher.has('usbaudio')") == true, "launcher.has('usbaudio') is true")
S.check(rt.eval("return launcher.has('disk')") == true, "launcher.has('disk') is true with a volume mounted")
rt.set_disk(false)
S.check(rt.eval("return launcher.has('disk')") == false, "launcher.has('disk') is false without one")
rt.set_disk(true)
S.check(rt.eval("return launcher.has('nope')") == false, "launcher.has is false for an unknown name")

rt.eval("system.setUpdateRate(3)")
S.check(rt.rate() == 0, "rate 3 disables updates")
rt.eval("system.setUpdateRate(20)")
S.check(rt.rate() == 20, "rate 20 accepted")

-- Regression: building print output with luaL_tolstring +
-- luaL_addlstring corrupts lines over LUAL_BUFFERSIZE (256 B with
-- LUA_32BITS). Check that a 300-char line reaches rt_port_log intact.
rt.eval("print(string.rep('x', 300))")
local logged = rt.log()
S.check(logged == string.rep('x', 300) .. "\n", "long print reaches rt_port_log intact")

-- Code-review fixes: the mock calls page.onInit() unprotected, so an error
-- there fails the load (rather than leaving a half-loaded script running).
local ok = rt.load_string("bad_init", [[
function page.onInit() error("boom") end
]])
S.check(ok == false, "onInit error fails the load")
S.check(rt.log():find("boom", 1, true) ~= nil, "onInit error reaches rt_port_log")
S.check(rt.error_count() == 0, "a failed load isn't a runtime error (no !)")

-- A failing system.update disables further updates, like the mock.
rt.load_string("bad_update", [[
local calls = 0
function page.onInit() end
function system.update() calls = calls + 1; error("update boom") end
function get_calls() return calls end
]])
rt.eval("system.setUpdateRate(20)")
rt.advance(200)
S.check(rt.eval("return get_calls()") == 1, "a failing update is called exactly once")
S.check(rt.rate() == 0, "a failing update disables further updates")
S.check(rt.error_count() == 1, "a failing update counts as a runtime error (the screen's !)")

-- Runtime errors are rate-limited per callback path: the first is logged
-- in full, later ones only counted, then summarized every 10 s and on
-- unload. rt.error_count() (the screen's "!") counts every one.
do
  local function count(pat)
    local _, n = rt.log_history():gsub(pat, "")
    return n
  end
  rt.scene{pages = 1, pad_pages = 1, assigned = {1, 2}}
  assert(rt.load_string("bad_turn", [[
function page.onInit() end
function controller.onEncoderTurn(e)
  if e.id == 1 then error("turn boom") end
end
function clock.onPulse() error("pulse boom") end
]]))
  S.check(rt.error_count() == 0, "no runtime errors right after a load")
  for _ = 1, 5 do rt.turn(1, 1) end
  S.check(count("turn boom") == 1, "5 failing turns log the error once (" .. count("turn boom") .. ")")
  S.check(rt.log():find("stack traceback", 1, true) ~= nil, "the first one in full, with its traceback")
  S.check(rt.error_count() == 5, "rt.error_count() counts all 5")
  rt.turn(2, 1)
  S.check(rt.error_count() == 5, "a callback that succeeds doesn't count")
  S.check(count("failed") == 0, "no summary before 10 s")
  rt.advance(10000)
  rt.advance(1)
  S.check(count("rt: controller%.onEncoderTurn failed 4 more times %(5 total%)\n") == 1,
    "a summary line after 10 s")
  rt.advance(10001)
  S.check(count("failed") == 1, "no new summary when nothing new failed")
  rt.turn(1, 1)
  rt.eval("clock.listen(true, 16); clock.startInternal()")
  rt.advance(500)
  local errs = rt.error_count()
  local cerrs = select(5, rt.clock_info())
  S.check(cerrs > 1 and errs == 6 + cerrs, "clock callback errors count too (" .. errs .. ")")
  S.check(count("pulse boom") == 1, "the clock callback error is logged once")
  S.check(count("rt: controller%.onEncoderTurn failed 1 more times %(6 total%)\n") == 1,
    "the next summary covers only the new errors")
  -- Loading another script unloads this one: a final summary for what's
  -- left (rt.load_string clears the log first, so it holds only that).
  assert(rt.load_string("ok", "function page.onInit() end"))
  S.check(count("rt: clock callbacks failed %d+ more times %(" .. cerrs .. " total%)\n") == 1
    and count("onEncoderTurn") == 0, "unload summarizes what wasn't reported yet")
  S.check(rt.error_count() == 0, "a new load clears the error count")
  rt.scene{}
end

S.done()
