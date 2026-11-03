-- test/rt/ring_spec.lua: ring.* (per-LED rings), custom mode vs
-- leds.updateByIndex, leds.reset/ring.release, fill/clear, range errors.
local S = dofile("test/rt/spec.lua")
local rt = require("rt")

rt.load_string("t", [[
function page.onInit() end
function probe(f) local ok, err = pcall(f); return ok end
]])

-- --- set ---
rt.eval("ring.set(3, 1, 10, 20, 30)")
local r, g, b, set = rt.led(3, 1)
S.check(set and r == 10 and g == 20 and b == 30, "ring.set writes one LED")
S.check(rt.ring_custom(3) == true, "ring.set puts the ring in custom mode")
S.check((rt.led(3, 2))  == 0 and select(4, rt.led(3, 2)) == false, "other LEDs on the ring are untouched")
S.check(rt.ring_custom(4) == false, "a different ring is unaffected")

-- --- updateByIndex is ignored on a custom ring ---
rt.eval("leds.updateByIndex(3, 8000, 5)")
S.check(rt.rings()[3] == nil, "leds.updateByIndex does nothing on a custom ring")
rt.eval("leds.updateByIndex(4, 8000, 5)")
S.check(rt.rings()[4] ~= nil, "leds.updateByIndex still works on a non-custom ring")
-- a batch entry for a custom ring is skipped; others in the same batch aren't
rt.eval("leds.updateByIndex({{3, 100, 1}, {5, 200, 2}})")
S.check(rt.rings()[3] == nil and rt.rings()[5] ~= nil,
  "a batch skips the custom ring's entry but applies the rest")

-- --- fill ---
rt.eval("ring.release(3)") -- start clean
rt.eval("ring.fill(6, 1, 2, 3)")
local all_set = true
for led = 1, 16 do
  local lr, lg, lb, lset = rt.led(6, led)
  if not (lset and lr == 1 and lg == 2 and lb == 3) then all_set = false end
end
S.check(all_set, "ring.fill sets every LED on the ring")
S.check(rt.ring_custom(6) == true, "ring.fill puts the ring in custom mode")

-- --- clear ---
rt.eval("ring.clear(6)")
local all_zero = true
for led = 1, 16 do
  local lr, lg, lb, lset = rt.led(6, led)
  if not (lset and lr == 0 and lg == 0 and lb == 0) then all_zero = false end
end
S.check(all_zero, "ring.clear blanks every LED")
S.check(rt.ring_custom(6) == true, "ring.clear keeps the ring in custom mode")

-- --- release ---
rt.eval("ring.release(6)")
S.check(rt.ring_custom(6) == false, "ring.release exits custom mode")
S.check(select(4, rt.led(6, 1)) == false, "ring.release clears the recorded LEDs too")
rt.eval("leds.updateByIndex(6, 4096, 3)")
S.check(rt.rings()[6] ~= nil, "updateByIndex works again after release")

-- --- leds.reset also releases a custom ring ---
rt.eval("ring.set(7, 1, 9, 9, 9)")
S.check(rt.ring_custom(7) == true, "ring 7 is custom before reset")
rt.eval("leds.reset(7)")
S.check(rt.ring_custom(7) == false, "leds.reset releases a custom ring")
rt.eval("leds.updateByIndex(7, 1000, 0)")
S.check(rt.rings()[7] ~= nil, "updateByIndex works on ring 7 again after leds.reset")

-- --- show ---
local shows0 = rt.ring_shows()
rt.eval("ring.show()")
S.check(rt.ring_shows() == shows0 + 1, "ring.show() is recorded")

-- --- range errors ---
S.check(rt.eval("return probe(function() ring.set(0, 1, 0, 0, 0) end)") == false,
  "ring.set rejects ring index 0")
S.check(rt.eval("return probe(function() ring.set(17, 1, 0, 0, 0) end)") == false,
  "ring.set rejects ring index 17")
S.check(rt.eval("return probe(function() ring.set(1, 17, 0, 0, 0) end)") == false,
  "ring.set rejects led index 17")
S.check(rt.eval("return probe(function() ring.set(1, 1, 256, 0, 0) end)") == false,
  "ring.set rejects red 256")
S.check(rt.eval("return probe(function() ring.set(1, 1, -1, 0, 0) end)") == false,
  "ring.set rejects a negative colour")
S.check(rt.eval("return probe(function() ring.fill(0, 0, 0, 0) end)") == false,
  "ring.fill rejects ring index 0")
S.check(rt.eval("return probe(function() ring.clear(17) end)") == false,
  "ring.clear rejects ring index 17")
S.check(rt.eval("return probe(function() ring.release(0) end)") == false,
  "ring.release rejects ring index 0")
S.check(rt.eval("return probe(function() ring.set(1, 1, 'x', 0, 0) end)") == false,
  "ring.set rejects a non-number colour")

-- --- a fresh load exits every ring's custom mode ---
rt.eval("ring.set(8, 1, 1, 1, 1)")
S.check(rt.ring_custom(8) == true, "ring 8 is custom before the next load")
rt.load_string("t2", "function page.onInit() end")
S.check(rt.ring_custom(8) == false, "a new load starts with no custom rings")

S.done()
