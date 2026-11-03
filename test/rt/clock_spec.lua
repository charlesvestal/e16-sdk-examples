-- test/rt/clock_spec.lua: system.update scheduling and the clock.
--
-- Each scenario runs twice: on the real mock (external/oxi-e16-lua/test/
-- e16mock.lua, which is the spec) and on the native runtime, with the same
-- calls. Both record every callback as "time name args" and the two
-- sequences must be identical.
local S = dofile("test/rt/spec.lua")
local rt = require("rt")

local MOCK = "external/oxi-e16-lua/test/e16mock.lua"

-- The scratch script. now_ms() is supplied by each harness: the mock's
-- clock (E.now) or the host clock (rt.inject_now()).
local SCRIPT = [[
LOG = {}
local function log(name, ...)
  LOG[#LOG + 1] = string.format("%.3f %s %s", now_ms(), name, table.concat({...}, ","))
end
local pulses = 0
FAIL_ON = FAIL_ON or 0
STOP_AT = nil
function page.onInit() end
function system.update() log("update") end
function clock.onPulse(b, p, s)
  pulses = pulses + 1
  if pulses == FAIL_ON then error("pulse boom") end
  log("onPulse", b, p, s)
  if p == STOP_AT then clock.stopInternal() end
end
function clock.onStart(s)
  log("onStart", s)
  if STOP_ON_START then STOP_ON_START = nil; clock.stopInternal() end
end
function clock.onStop(s) log("onStop", s) end
function clock.onContinue(s) log("onContinue", s) end
]]

local path = os.tmpname()
do
  local f = assert(io.open(path, "w"))
  f:write(SCRIPT)
  f:close()
end

-- The two harnesses behind one interface.
local function mock_h()
  local E = dofile(MOCK)
  now_ms = function() return E.now end -- the script shares this global state
  LOG, FAIL_ON, STOP_AT, STOP_ON_START = nil, nil, nil, nil
  return {
    load = function() E.load(path) end,
    eval = function(code) return assert(load(code))() end,
    run = E.run,
    extStart = E.extStart, extStop = E.extStop, extContinue = E.extContinue,
    log = function() return table.concat(LOG, ";") end,
  }
end

local function rt_h()
  rt.reset() -- host time 0, clock at power-on state, nothing loaded
  return {
    load = function() assert(rt.load(path)); rt.inject_now() end,
    eval = rt.eval,
    run = rt.advance,
    extStart = rt.ext_start, extStop = rt.ext_stop, extContinue = rt.ext_continue,
    log = function() return rt.eval("return table.concat(LOG, ';')") end,
  }
end

-- float_only: the scenario only matches the mock bit for bit with float
-- runtime times (the default host build, rt.TIME_FLOAT); a double build
-- (E16_HOST_DOUBLE_TIME, the device's arithmetic) runs it but doesn't
-- compare. See the scenario's comment for why.
local function both(name, scenario, float_only)
  local a = mock_h(); scenario(a); local la = a.log()
  local b = rt_h(); scenario(b); local lb = b.log()
  if float_only and not rt.TIME_FLOAT then
    print("skip " .. name .. " (float-only; this build uses double times)")
    return la, lb, a, b
  end
  S.check(la == lb, name)
  if la ~= lb then
    print("  mock: " .. la)
    print("  rt:   " .. lb)
  end
  return la, lb, a, b
end

local function count(log, name)
  local n = 0
  for _ in (";" .. log):gmatch(";[%d.]+ " .. name) do n = n + 1 end
  return n
end

-- 1. Update count over 1000 ms at rate 20 (the firmware calls it once more
-- than 20 ms have passed: every 20.1 ms).
local l1 = both("update times match the mock (rate 20, 1000 ms)", function(h)
  h.load()
  h.eval("system.setUpdateRate(20)")
  h.run(1000)
end)
S.check(count(l1, "update") == 49, "49 updates in 1000 ms at rate 20")

-- 2. One bar at 120 BPM, res 16.
local pos500 = {}
local l2 = both("internal clock pulses match the mock (120 BPM, res 16)", function(h)
  h.load()
  h.eval("clock.listen(true, 16)")
  h.eval("clock.startInternal()")
  h.run(500)
  pos500[#pos500 + 1] = h.eval("return clock.getPosition(16)")
  pos500[#pos500 + 1] = h.eval("return clock.getBpm()")
  h.run(1500)
end)
do
  local bs, n = {}, 0
  for t, b in (";" .. l2):gmatch(";([%d.]+) onPulse (%d+),") do
    if tonumber(t) < 2000 then n = n + 1; bs[#bs + 1] = b end
  end
  S.check(n == 16, "16 pulses in one bar at res 16")
  S.check(table.concat(bs, ",") == "4,16,8,16,4,16,8,16,4,16,8,16,4,16,8,16",
    "boundaries 4,16,8,16,...")
  S.check(l2:find("^0%.000 onStart 2;") ~= nil, "onStart(2) is delivered first")
end
S.check(pos500[1] == pos500[3] and pos500[1] == 4, "getPosition(16) after 500 ms matches the mock (4)")
S.check(pos500[2] == pos500[4] and pos500[2] == 120, "getBpm() while running matches the mock (120)")

-- 3. External transport, then Lua takes over, then external Stop.
local l3 = both("external -> internal -> external stop matches the mock", function(h)
  h.load()
  h.eval("clock.listen(true, 16)")
  h.extStart(128)
  h.run(500)
  h.eval("clock.startInternal()")
  h.run(100)
  h.extStop()
  h.run(100)
end)
S.check(l3:find("500%.000 onStop 0;500%.000 onStart 2;") ~= nil,
  "startInternal while external runs: onStop(0) then onStart(2)")
do
  local last
  for t in (";" .. l3):gmatch(";([%d.]+) onPulse") do last = tonumber(t) end
  S.check(l3:find("600%.000 onStop 0$") ~= nil and last < 600, "extStop stops a Lua-started clock")
end

-- 4. An error in onPulse ends that call only.
local l4 = both("onPulse error: later pulses match the mock", function(h)
  h.load()
  h.eval("FAIL_ON = 3")
  h.eval("clock.listen(true, 16)")
  h.eval("clock.startInternal()")
  h.run(1000)
end)
S.check(count(l4, "onPulse") >= 6 and l4:find(" onPulse 16,3,2") ~= nil and l4:find(" onPulse 8,2,2") == nil,
  "pulse 3 failed, pulses 4+ still arrive")
S.check(rt.log():find("pulse boom", 1, true) ~= nil, "the clock callback error is logged")

-- 5. Reload during a Lua-started clock.
local l5, _, _, h5 = both("reload stops a Lua-started clock (matches the mock)", function(h)
  h.load()
  h.eval("clock.listen(true, 16)")
  h.eval("clock.startInternal()")
  h.run(300)
  h.load()
  h.eval("clock.listen(true, 16)")
  h.run(300)
end)
S.check(count(l5, "onPulse") == 0 and count(l5, "onStop") == 0, "after reload: no pulses, no onStop")
S.check(h5.eval("return clock.getBpm()") == 1 and h5.eval("return clock.getPosition(16)") == 0,
  "after reload: getBpm() 1, getPosition() 0")

-- 6. Reload turns listening off; external transport survives it.
local l6 = both("reload turns listening off, external transport survives", function(h)
  h.load()
  h.eval("clock.listen(true, 16)")
  h.extStart(120)
  h.run(200)
  h.load()
  h.run(200)
  h.eval("clock.listen(true, 4)")
  h.run(1000)
end)
S.check(count(l6, "onPulse") > 0 and l6:find("4,%d+,0") ~= nil, "pulses resume (source 0) once listening again")

-- 7. Continue, stop repeats, setInternalBpm re-anchoring, resolution 96.
both("continue, stop repeats, tempo change, res 96 match the mock", function(h)
  h.load()
  h.eval("clock.listen(true, 96)")
  h.extStart()
  h.run(110)
  h.extStop()
  h.run(50)
  h.extStop()
  h.extContinue()
  h.run(100)
  h.extStop()
  h.eval("clock.listen(true, 8)")
  h.eval("clock.startInternal(); clock.setInternalBpm(90)")
  h.run(700)
  h.eval("clock.setInternalBpm(173.5)")
  h.run(700)
  h.eval("clock.stopInternal()")
  h.run(100)
end)

-- 8. Pulse and update due at the same time (the pulse goes first); a
-- callback queued by a pulse is delivered at the pulse's time; listening
-- is checked when queued callbacks are delivered; a callback queued while
-- delivering another waits for the mock's next pass.
--
-- Float-only: the "tie" is one by float arithmetic. The update rate is set
-- at 19.9 ms (a float step, like the mock's), so the next update is due at
-- 19.9 + 20 + 0.1. In float that rounds to exactly 40, the same time as the
-- pulse, and the pulse goes first. In double (the device's rt_time_t) the
-- start time is the float 19.899999618..., so the update is due at
-- 39.9999996, a hair before the pulse, and goes first: not a tie at all.
-- The difference is rounding, not scheduling (on the device, times are
-- whole ms from system_millis() anyway), so only the float build compares
-- this scenario with the mock. The checks on l8 below read the mock's log.
local l8 = both("ties, callbacks queued by callbacks, late listen(false) match the mock", function(h)
  h.load()
  h.eval("clock.listen(true, 96); clock.setInternalBpm(125); clock.startInternal()")
  h.run(19.9)
  h.eval("system.setUpdateRate(20)")
  h.eval("STOP_AT = 5")
  h.run(200)
  h.eval("STOP_AT = nil; clock.startInternal()")
  h.run(30)
  h.eval("clock.stopInternal(); clock.listen(false)")
  h.run(30)
  h.eval("clock.listen(true, 96); STOP_ON_START = true; clock.startInternal()")
  h.run(30)
end, true)
S.check(l8:find("(%d+%.%d+) onPulse 96,2,2;%1 update") ~= nil, "a pulse and an update due together: pulse first")
S.check(l8:find("(%d+%.%d+) onPulse 96,5,2;%1 onStop 2;") ~= nil, "onStop queued by onPulse arrives at the pulse's time")
S.check(l8:find("241%.000 update ;261%.100 update ;279%.900 onStart 2;") ~= nil,
  "listen(false) before delivery drops the queued onStop")
S.check(l8:find("279%.900 onStart 2;281%.200 update ;281%.200 onStop 2;") ~= nil,
  "onStop queued by onStart arrives after the next event, like the mock's next pass")

-- External MIDI clock bytes: Start, then 0xF8 at 24 per quarter (120 BPM).
rt.reset()
assert(rt.load(path)); rt.inject_now()
rt.eval("clock.listen(true, 16)")
rt.realtime(0xF8) -- clock without Start: estimate only, no pulses
rt.advance(20.833333)
rt.realtime(0xFA)
rt.advance(5)
for _ = 1, 96 do
  rt.realtime(0xF8)
  rt.advance(60000 / (120 * 24))
end
local lx = rt.eval("return table.concat(LOG, ';')")
local srcs, n = {}, 0
for b, p, s in (";" .. lx):gmatch(";[%d.]+ onPulse (%d+),(%d+),(%d+)") do
  n = n + 1; srcs[s] = true
end
S.check(lx:find("onStart 0") ~= nil, "0xFA delivers onStart(0)")
S.check(n == 16 and srcs["0"] and not srcs["2"], "0xF8 ticks drive 16 pulses per bar, source 0")
local bpm = rt.eval("return clock.getBpm()")
S.check(math.abs(bpm - 120) <= 1, "getBpm() measures 120 within 1 BPM (" .. tostring(bpm) .. ")")
S.check(rt.eval("return clock.getPosition(16)") == 16, "getPosition(16) after 96 ticks is 16")
rt.realtime(0xFC)
rt.realtime(0xF8)
rt.advance(30)
S.check(rt.eval("return table.concat(LOG, ';')"):find("onStop 0$") ~= nil, "0xFC delivers onStop(0), no more pulses")

-- Millisecond-quantized timing, as on the device: the estimate holds.
rt.realtime(0xFB)
local t0 = rt.now()
for i = 1, 96 do
  rt.realtime(0xF8)
  local target = t0 + math.floor(i * 60000 / (120 * 24))
  rt.advance(target - rt.now())
end
bpm = rt.eval("return clock.getBpm()")
S.check(math.abs(bpm - 120) <= 1, "getBpm() within 1 BPM with 1 ms jitter (" .. tostring(bpm) .. ")")

-- A stalled main loop in tick mode: Start and 12 ticks arrive before the
-- next rt_tick(). All of them come out in order on that tick.
rt.reset()
assert(rt.load(path)); rt.inject_now()
rt.eval("clock.listen(true, 96)")
rt.realtime(0xFA)
for _ = 1, 12 do rt.realtime(0xF8) end
rt.advance(1)
do
  local want = {"0.000 onStart 0"}
  for i = 0, 11 do
    local t = i % 24 == 0 and 4 or i % 12 == 0 and 8 or i % 6 == 0 and 16 or i % 3 == 0 and 32 or 96
    want[#want + 1] = string.format("0.000 onPulse %d,%d,0", t, i)
  end
  local got = rt.eval("return table.concat(LOG, ';')")
  S.check(got == table.concat(want, ";"), "stalled loop: onStart(0), then 12 pulses at res 96, positions 0..11")
  if got ~= table.concat(want, ";") then print("  got: " .. got) end
end

-- Queue overflow: 20 transport callbacks queued with no rt_tick() between.
-- The queue keeps 16, the drop is logged, and the clock keeps working.
rt.reset()
assert(rt.load(path)); rt.inject_now()
rt.eval("clock.listen(true, 16)")
for _ = 1, 10 do rt.realtime(0xFA); rt.realtime(0xFC) end
rt.advance(1)
do
  local got = rt.eval("return table.concat(LOG, ';')")
  S.check(count(got, "onStart") == 8 and count(got, "onStop") == 8, "16 queued callbacks delivered")
  S.check(rt.log() == "clock: 4 transport callbacks dropped\n", "the drop is logged with its count")
  for _ = 1, 10 do rt.realtime(0xFA); rt.realtime(0xFC) end
  rt.advance(1)
  S.check(rt.log() == "clock: 4 transport callbacks dropped\n", "logged once per load")
  rt.eval("LOG = {}")
  rt.realtime(0xFA)
  for _ = 1, 24 do rt.realtime(0xF8); rt.advance(20) end
  got = rt.eval("return table.concat(LOG, ';')")
  S.check(got:find("^2%.000 onStart 0;") ~= nil and count(got, "onPulse") == 4,
    "after the overflow, Start and ticks deliver pulses")
end

os.remove(path)
S.done()
