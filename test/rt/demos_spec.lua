-- test/rt/demos_spec.lua: the launcher's demo scripts (scripts/demos/) run
-- for a few simulated seconds without errors and actually use their APIs.
local S = dofile("test/rt/spec.lua")
local rt = require("rt")

local function run(name, assigned, ms)
  rt.reset()
  rt.scene{pages = 1, pad_pages = 1, assigned = assigned}
  local ok = rt.load("scripts/demos/" .. name .. ".lua")
  S.check(ok, name .. ": loads")
  rt.advance(ms or 2000)
  return ok
end

-- --- canvas ---
run("canvas", {1, 17})
S.check(rt.error_count() == 0, "canvas: no runtime errors")
S.check(rt.canvas_shows() >= 90, "canvas: about 50 frames a second (" .. rt.canvas_shows() .. " in 2 s)")
local lit = 0
for y = 0, 53 do for x = 0, 127 do if rt.canvas_pixel(x, y) then lit = lit + 1 end end end
S.check(lit > 100, "canvas: draws something (" .. lit .. " pixels lit)")
rt.press(1); rt.release(1)
S.check(rt.canvas_fullscreen() == true, "canvas: pressing encoder 1 goes fullscreen")
rt.advance(100)
S.check(rt.error_count() == 0, "canvas: still no errors fullscreen")

-- --- rings ---
run("rings", {1, 2})
S.check(rt.error_count() == 0, "rings: no runtime errors")
S.check(rt.ring_shows() >= 90, "rings: about 50 frames a second (" .. rt.ring_shows() .. " in 2 s)")
local custom, bright = 0, 0
for r = 1, 16 do
  if rt.ring_custom(r) then custom = custom + 1 end
  for led = 1, 16 do
    local cr, cg, cb = rt.led(r, led)
    if cr + cg + cb > 0 then bright = bright + 1 end
  end
end
S.check(custom == 16, "rings: every ring is in custom mode")
S.check(bright >= 16 and bright < 256, "rings: a comet per ring, not every LED (" .. bright .. " lit)")

-- --- acid ---
run("acid", {1, 2, 3, 4, 5, 6, 7, 24}, 4000)
S.check(rt.error_count() == 0, "acid: no runtime errors")
S.check(rt.audio_enabled() == true, "acid: the synth is enabled")
local ons, offs, slides = 0, 0, 0
for _, c in ipairs(rt.audio()) do
  if c.op == "note" and c.b > 0 then ons = ons + 1 end
  if c.op == "note" and c.b == 0 then offs = offs + 1 end
  if c.op == "set" and c.a == 7 and c.b == 127 then slides = slides + 1 end
end
S.check(ons >= 20, "acid: plays notes (" .. ons .. " note-ons in 4 s)")
S.check(offs >= 10, "acid: and releases them (" .. offs .. ")")
S.check(slides >= 1, "acid: with slides")
S.check(rt.ring_custom(16), "acid: ring 16 shows the steps")
rt.turn(1, 1)
S.check(rt.audio_param(0) == 42, "acid: encoder 1 sets the cutoff (40 + 2)")
rt.press(8); rt.release(8)
local last = rt.audio()[#rt.audio()]
S.check(last.op == "off", "acid: encoder 8 stops it and silences the voice")
local n = #rt.audio()
rt.advance(1000)
S.check(#rt.audio() == n, "acid: nothing plays while stopped")
rt.load_string("after", "function page.onInit() end")
S.check(rt.audio_enabled() == false, "acid: unloading turns the synth off")

-- acid follows incoming notes: the next steps play from the new root
run("acid", {1, 2, 3, 4, 5, 6, 7, 24}, 500)
rt.midi_in(1, 0x90, 81, 100)          -- A5, folded down to A3 (57)
rt.midi_in(1, 0x80, 81, 0)
local n0 = #rt.audio()
rt.advance(2000)
local lo, hi = 999, -1
local all = rt.audio()
for i = n0 + 1, #all do
  local c = all[i]
  if c.op == "note" and c.b > 0 then lo = math.min(lo, c.a); hi = math.max(hi, c.a) end
end
S.check(lo == 57 and hi == 57 + 15, "acid: a received note transposes the line (" .. lo .. "-" .. hi .. ")")
S.check(rt.error_count() == 0, "acid: no errors after transposing")

-- --- midimon ---
run("midimon", {1}, 100)
S.check(rt.error_count() == 0, "midimon: no runtime errors")
rt.midi_in(1, 0x90, 60, 100)
rt.midi_in(2, 0xB2, 7, 127)
rt.midi_in(1, 0xE0, 0, 0)
rt.midi_in(1, 0x80, 60, 0)
rt.advance(100)
S.check(rt.error_count() == 0, "midimon: still no errors after messages")
local hist = rt.log_history()
S.check(hist:find("midimon: U1 on C4 100", 1, true) and hist:find("midimon: T3 cc 7 = 127", 1, true)
  and hist:find("midimon: U1 bend -8192", 1, true) and hist:find("midimon: U1 off C4", 1, true),
  "midimon: logs each message")
local r, g = rt.led(8, 1)               -- CC 7 -> ring 8, a full meter
S.check(rt.ring_custom(8) and g > 0 and r == 0, "midimon: a CC fills its ring in green")
local _, _, _, set13 = rt.led(13, 1)    -- note 60 -> ring 13
S.check(rt.ring_custom(13) and set13, "midimon: a note lights its ring")
local lit = 0
for y = 0, 53 do for x = 0, 127 do if rt.canvas_pixel(x, y) then lit = lit + 1 end end end
S.check(lit > 200, "midimon: draws the message list (" .. lit .. " pixels lit)")
rt.turn(1, 1)
rt.midi_in(1, 0x90, 61, 100)            -- channel 1 (0) only now: arrives
rt.midi_in(1, 0x91, 62, 100)            -- channel 2: filtered out
rt.advance(100)
hist = rt.log_history()
S.check(hist:find("U1 on C#4", 1, true) and not hist:find("U2 on D4", 1, true),
  "midimon: encoder 1 listens to one channel")
for i = 1, 500 do rt.midi_in(1, 0xB0, i % 128, i % 128) end
rt.advance(2000)
S.check(rt.error_count() == 0 and rt.loaded(), "midimon: survives a 500-message burst")
S.check(rt.log_history():find("midi in: 372 messages dropped", 1, true) ~= nil, "midimon: the burst's drops are logged")

S.done()
