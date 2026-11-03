-- test/rt/ctl_spec.lua: controller, encoder events and scene wiring.
--
-- Mirrors external/oxi-e16-lua/test/e16mock.lua's controller.onEncoder*
-- event shapes (M.turn/M.press/M.release) and controller.setHoldTime, but
-- drives the scene wiring (rt_api_ctl.c's header: pad_pages/settings_page/
-- assigned) through rt.scene{...} and the rt.turn/press/release/show
-- bindings onto src/rt/rt_api_ctl.c.
local S = dofile("test/rt/spec.lua")
local rt = require("rt")

local SCRIPT = [[
function page.onInit()
  log = {}
end
function page.onPageChange(prev, curr) end

local function logEvent(kind, e)
  log[#log + 1] = string.format("%s %d %d %d", kind, e.id, e.page,
    e.increment or e.held_ms or 0)
end
function controller.onEncoderTurn(e) logEvent("turn", e) end
function controller.onEncoderPress(e) logEvent("press", e) end
function controller.onEncoderRelease(e) logEvent("release", e) end
function controller.onEncoderHold(e) logEvent("hold", e) end
]]

local function log()
  return rt.eval("return table.concat(log, ';')")
end

rt.load_string("ctl", SCRIPT)

-- Sets the scene *after* the script is already loaded and running: the
-- wiring must apply immediately, not only on the next load.
rt.scene{pages = 5, pad_pages = 4, settings_page = 5, assigned = {2, 18, 34, 50, 51}}

-- controller.setByIndex/setControls are no-ops in the mock; a stock script
-- calling them must not raise "attempt to call a nil value".
S.check(rt.eval("return pcall(function() "
  .. "controller.setByIndex(1, {v = 8192}); "
  .. "controller.setControls({{id = 1, v = 8192}}); "
  .. "end)") == true,
  "controller.setByIndex/setControls are callable no-ops")

-- Pad page: encoder e turns as id e, pushes as id 16+e.
rt.show(3)
rt.eval("log = {}")
rt.turn(2, 1)
S.check(log() == "turn 2 3 1", "pad page turn: id = encoder, page carried")

-- Settings page: encoder e turns as id 32+e, pushes as id 48+e.
rt.show(5)
rt.eval("log = {}")
rt.turn(2, -1)
S.check(log() == "turn 34 5 -1", "settings page turn: id = 32+encoder")

rt.eval("log = {}")
rt.press(2)
S.check(log() == "press 50 5 0", "settings page press: id = 48+encoder")
rt.release(2) -- let go so later presses on encoder 2 start clean

-- An id that isn't in `assigned` produces no callback at all.
rt.eval("log = {}")
rt.turn(7, 1) -- turn id 32+7 = 39, not assigned
S.check(log() == "", "unassigned id: no callback")

-- Hold: fires once, exactly at press_time + hold_ms.
rt.eval("system.setUpdateRate(0)") -- keep system.update out of the way
rt.eval("log = {}")
rt.eval("controller.setHoldTime(300)")
rt.press(2)
rt.advance(300)
rt.release(2)
S.check(log() == "press 50 5 0;hold 50 5 0;release 50 5 300",
  "hold fires once at exactly press_time + hold_ms, then release follows")

-- Held for less than the armed time: no hold.
rt.eval("log = {}")
rt.press(2)
rt.advance(200)
rt.release(2)
S.check(log():find("hold", 1, true) == nil, "held less than hold_ms: no hold")

-- Release carries the press's id/page even if the page changed meanwhile.
rt.show(5)
rt.press(2) -- press id 50, page 5
rt.show(1) -- page changes before release
rt.eval("log = {}")
rt.release(2)
S.check(log():match("^release 50 5 %d+$") ~= nil,
  "release keeps the press's id/page across a page change")

-- setHoldTime(0) disarms *new* presses.
rt.show(5)
rt.eval("controller.setHoldTime(0)")
rt.eval("log = {}")
rt.press(2)
rt.advance(400)
rt.release(2)
S.check(log():find("hold", 1, true) == nil, "setHoldTime(0) disarms a new press")

-- setHoldTime is live, matching the mock (M.release checks M.hold at
-- release time, not at press time): calling setHoldTime(0) *after*
-- pressing disarms that in-flight press too, not just future ones.
rt.show(5)
rt.eval("controller.setHoldTime(300)")
rt.eval("log = {}")
rt.press(2)
rt.eval("controller.setHoldTime(0)")
rt.advance(400)
rt.release(2)
S.check(log():find("hold", 1, true) == nil,
  "setHoldTime(0) after pressing disarms the in-flight press (live semantics)")

-- Regression: within a single tick pass, g_hold_ms is read once and used
-- for every press checked in that pass -- the actual re-entrancy fix.
-- Two encoders pressed 5 ms apart, both armed at 300 ms: encoder A's hold
-- fires at its due time and its callback calls setHoldTime(0). At that
-- moment B has only been held 295 ms, so it doesn't fire in that same
-- pass regardless. Because the disarm is live, though, B never fires
-- afterwards either -- it's disarmed, not just future presses.
rt.show(5)
rt.eval("controller.setHoldTime(300)")
rt.eval([[
  log = {}
  controller.onEncoderPress = function() end -- keep the log to hold entries only
  controller.onEncoderHold = function(e)
    log[#log + 1] = string.format("hold %d %d", e.id, e.index)
    if e.index == 2 then controller.setHoldTime(0) end
  end
]])
rt.press(2)     -- encoder A, armed at 300
rt.advance(5)
rt.press(3)     -- encoder B, 5 ms behind A, armed at 300 too
rt.advance(295) -- lands exactly on A's due time; B has only been held 295 ms
S.check(log() == "hold 50 2", "A's hold fires at its due time and disarms setHoldTime")
rt.advance(10)  -- past B's due time too
S.check(log() == "hold 50 2",
  "B never fires afterwards: A's disarm is live, not scoped to future presses")
rt.release(2)
rt.release(3)

-- Regression: an out-of-range page (here 0) is simply unwired, like any
-- page nothing was assigned to.
rt.load_string("ctl", SCRIPT) -- fresh script: restores the logging onEncoderHold
rt.scene{pages = 5, pad_pages = 4, settings_page = 5, assigned = {2, 18, 34, 50, 51}}
rt.show(0)
rt.eval("log = {}")
rt.turn(2, 1)
S.check(log() == "", "page 0 is out of range: no callback")

-- rt.set_page (rt_set_page_silent): sets controller.getPage() without
-- calling page.onPageChange, for the mock bridge (test/host/e16mock.lua).
rt.eval("page_changes = 0")
rt.eval("function page.onPageChange(prev, curr) page_changes = page_changes + 1 end")
rt.set_page(2)
S.check(rt.eval("return controller.getPage()") == 2, "rt.set_page sets the current page")
S.check(rt.eval("return page_changes") == 0, "rt.set_page doesn't call page.onPageChange")

-- Loading a script disarms hold (and clears press state).
rt.eval("controller.setHoldTime(300)")
rt.load_string("ctl", SCRIPT) -- reload: wiring is sticky, press/hold state is fresh
rt.show(5)
rt.press(2)
rt.advance(400)
S.check(log():find("hold", 1, true) == nil, "loading a script disarms hold")
rt.eval("log = {}")
rt.release(2)
S.check(log():match("^release 50 5 %d+$") ~= nil,
  "release still works normally after reload")

S.done()
