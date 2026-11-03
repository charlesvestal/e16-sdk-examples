-- test/rt/audio_spec.lua: audio.* (the internal synth on TRS 2): what
-- reaches the port, argument checks, and the release on unload.
local S = dofile("test/rt/spec.lua")
local rt = require("rt")

-- rt_port_audio_* indices (rt_port.h's rt_audio_param_t order)
local CUTOFF, RESONANCE, ENVMOD, DECAY, WAVEFORM, VOLUME, GLIDE, SLIDE = 0, 1, 2, 3, 4, 5, 6, 7

rt.load_string("t", [[
function page.onInit() end
function probe(f) local ok, err = pcall(f); return ok, err end
]])

-- calls recorded since index n0 (the load itself records a release from
-- rt_unload of whatever ran before)
local function since(n0)
  local all, out = rt.audio(), {}
  for i = n0 + 1, #all do out[#out + 1] = all[i] end
  return out
end

S.check(rt.eval("return type(audio)") == "table", "audio is a global table")
S.check(rt.audio_enabled() == false, "a freshly loaded script starts with the synth off")

-- --- enable ---
local n0 = #rt.audio()
rt.eval("audio.enable(true)")
local c = since(n0)
S.check(#c == 1 and c[1].op == "enable" and c[1].a == 1, "audio.enable(true) reaches the port")
S.check(rt.audio_enabled() == true, "the synth is on")
rt.eval("audio.enable(false)")
S.check(rt.audio_enabled() == false, "audio.enable(false) turns it off")

-- --- note ---
n0 = #rt.audio()
rt.eval("audio.note(48, 100)")
c = since(n0)
S.check(#c == 1 and c[1].op == "note" and c[1].a == 48 and c[1].b == 100, "audio.note(note, vel)")
S.check(rt.audio_enabled() == true, "audio.note enables the synth")
rt.eval("audio.note(48.7, 0)")
c = since(n0)
S.check(c[2].op == "note" and c[2].a == 48 and c[2].b == 0, "velocity 0 (note off), floored note")

-- slide: the flag is set before the note, so the note itself glides
n0 = #rt.audio()
rt.eval("audio.note(50, 90, true)")
c = since(n0)
S.check(#c == 2 and c[1].op == "set" and c[1].a == SLIDE and c[1].b == 127
  and c[2].op == "note" and c[2].a == 50, "audio.note(..., true) sets slide on, then plays")
n0 = #rt.audio()
rt.eval("audio.note(50, 90, false)")
c = since(n0)
S.check(c[1].op == "set" and c[1].a == SLIDE and c[1].b == 0, "audio.note(..., false) sets slide off")
n0 = #rt.audio()
rt.eval("audio.note(50, 90)")
c = since(n0)
S.check(#c == 1 and c[1].op == "note", "without the slide argument, the slide flag is left alone")

-- --- off ---
n0 = #rt.audio()
rt.eval("audio.off()")
c = since(n0)
S.check(#c == 1 and c[1].op == "off", "audio.off() reaches the port")

-- --- set: every name ---
local names = {cutoff = CUTOFF, resonance = RESONANCE, envmod = ENVMOD, decay = DECAY,
  waveform = WAVEFORM, volume = VOLUME, glide = GLIDE, slide = SLIDE}
local all_ok = true
for name, id in pairs(names) do
  rt.eval(("audio.set(%q, %d)"):format(name, 10 + id))
  if rt.audio_param(id) ~= 10 + id then all_ok = false; print("  bad: " .. name) end
end
S.check(all_ok, "audio.set reaches the port for every parameter name")
rt.eval("audio.set('cutoff', 99.9)")
S.check(rt.audio_param(CUTOFF) == 99, "audio.set floors its value")
rt.eval("audio.set('cutoff', 0); audio.set('resonance', 127)")
S.check(rt.audio_param(CUTOFF) == 0 and rt.audio_param(RESONANCE) == 127, "0 and 127 are in range")

-- --- argument errors ---
local function fails(code) return rt.eval("return probe(function() " .. code .. " end)") == false end
S.check(fails("audio.enable(1)"), "audio.enable rejects a non-boolean")
S.check(fails("audio.enable()"), "audio.enable requires its argument")
S.check(fails("audio.note(128, 100)"), "audio.note rejects note 128")
S.check(fails("audio.note(-1, 100)"), "audio.note rejects a negative note")
S.check(fails("audio.note(60, 128)"), "audio.note rejects velocity 128")
S.check(fails("audio.note(60)"), "audio.note requires a velocity")
S.check(fails("audio.note('C', 100)"), "audio.note rejects a non-number note")
S.check(fails("audio.set('cutof', 10)"), "audio.set rejects an unknown name")
S.check(fails("audio.set(1, 10)"), "audio.set rejects a non-string name")
S.check(fails("audio.set('cutoff', 128)"), "audio.set rejects 128")
S.check(fails("audio.set('cutoff', -1)"), "audio.set rejects -1")
S.check(fails("audio.set('cutoff')"), "audio.set requires a value")
local msg = rt.eval("return select(2, pcall(audio.set, 'accent', 10))")
S.check(type(msg) == "string" and msg:find("unknown audio parameter: accent", 1, true),
  "the unknown-name error names the parameter")

-- --- port 4 is MIDI out like any other on the host (the device routes it
-- to the synth); out = 0 doesn't include it ---
rt.clear_sent()
rt.eval("midi.sendMidi(4, 0, 0x90, 60, 100)")
local sent = rt.sent()
S.check(#sent == 1 and sent[1].out == 4 and sent[1].st == 0x90, "midi.sendMidi(4, ...) reaches the port as out 4")

-- --- port 4: a note left held is released after 2 s without MIDI there ---
-- (a script that moves its output away from port 4 while a note is down
-- never sends that note's off)
local function offs(from)
  local k = 0
  for _, e in ipairs(since(from)) do if e.op == "off" then k = k + 1 end end
  return k
end
rt.load_string("p4", "function page.onInit() end")
n0 = #rt.audio()
rt.eval("midi.sendMidi(4, 0, 0x90, 60, 100)")
rt.advance(1900); rt.advance(0)
S.check(offs(n0) == 0, "port 4: a held note is kept for 1.9 s without MIDI")
rt.advance(200); rt.advance(0)
S.check(offs(n0) == 1, "port 4: released (all notes off) after 2 s without MIDI")
S.check(rt.log():find("no MIDI to port 4", 1, true) ~= nil, "port 4: the release is logged")
rt.advance(5000); rt.advance(0)
S.check(offs(n0) == 1, "port 4: released once, nothing held afterwards")

-- the note's own off in time: nothing to release
n0 = #rt.audio()
rt.eval("midi.sendMidi(4, 0, 0x90, 62, 100)")
rt.advance(500)
rt.eval("midi.sendMidi(4, 0, 0x80, 62, 0)")
rt.advance(3000); rt.advance(0)
S.check(offs(n0) == 0, "port 4: a note released in time isn't released again")
-- velocity 0 is a note off too
rt.eval("midi.sendMidi(4, 0, 0x90, 63, 100)")
rt.eval("midi.sendMidi(4, 0, 0x90, 63, 0)")
rt.advance(3000); rt.advance(0)
S.check(offs(n0) == 0, "port 4: note on with velocity 0 counts as its off")

-- traffic keeps a held note: anything sent to port 4 within 2 s
rt.eval("midi.sendMidi(4, 0, 0x90, 64, 100)")
for _ = 1, 5 do
  rt.advance(1500)
  rt.eval("midi.sendCC(4, 0, 74, 64)")
end
rt.advance(0)
S.check(offs(n0) == 0, "port 4: a held note survives while port 4 keeps getting MIDI")
-- other outputs don't count: the script moved its output to port 1
rt.eval("midi.sendMidi(1, 0, 0x90, 65, 100)")
rt.advance(1500)
rt.eval("midi.sendMidi(1, 0, 0x80, 65, 0)")
rt.advance(1000); rt.advance(0)
S.check(offs(n0) == 1, "port 4: MIDI to other outputs doesn't keep it")

-- CC 123 (all notes off) to port 4 releases at once
n0 = #rt.audio()
rt.eval("midi.sendMidi(4, 0, 0x90, 66, 100)")
rt.eval("midi.sendCC(4, 0, 123, 0)")
S.check(offs(n0) == 1, "port 4: CC 123 releases at once")
rt.advance(3000); rt.advance(0)
S.check(offs(n0) == 1, "port 4: and nothing is held after it")

-- unloading forgets what was held (the release on unload covers it)
rt.eval("midi.sendMidi(4, 0, 0x90, 67, 100)")
rt.load_string("p4b", "function page.onInit() end")
n0 = #rt.audio()
rt.advance(3000); rt.advance(0)
S.check(offs(n0) == 0, "port 4: a new script doesn't inherit the old one's held notes")

-- --- output routing ---
rt.load_string("t", [[
function page.onInit() end
function probe(f) local ok, err = pcall(f); return ok, err end
]])
S.check(rt.eval("return audio.output()") == "usb", "audio.output() defaults to usb with USB connected")
rt.set_usb(false)
S.check(rt.eval("return audio.output()") == "trs2", "and to trs2 without USB")
rt.set_usb(true)
n0 = #rt.audio()
S.check(rt.eval("return audio.output('trs2')") == "trs2", "audio.output('trs2') returns the new routing")
c = since(n0)
S.check(#c == 1 and c[1].op == "output" and c[1].a == 2, "audio.output('trs2') reaches the port")
S.check(rt.eval("return audio.output('both')") == "both", "audio.output('both')")
S.check(rt.eval("return audio.output('usb')") == "usb", "audio.output('usb')")
rt.set_usb(false)
S.check(rt.eval("return audio.output()") == "usb", "an explicit usb stays usb without USB")
S.check(rt.eval("return audio.output(nil)") == "usb", "audio.output(nil) only reads")
S.check(rt.eval("return audio.output('auto')") == "trs2", "audio.output('auto') resolves again")
rt.set_usb(true)
S.check(rt.eval("return audio.output()") == "usb", "auto follows the USB connection")
S.check(fails("audio.output('usb2')"), "audio.output rejects an unknown name")
S.check(fails("audio.output(1)"), "audio.output rejects a non-string")
msg = rt.eval("return select(2, pcall(audio.output, 'TRS2'))")
S.check(type(msg) == "string" and msg:find("unknown audio output: TRS2", 1, true),
  "the unknown-output error names it")
rt.eval("audio.output('trs2')")
rt.load_string("t3", "function page.onInit() end")
S.check(rt.eval("return audio.output()") == "usb", "unloading puts the routing back to auto")
rt.load_string("t", [[
function page.onInit() end
function probe(f) local ok, err = pcall(f); return ok, err end
]])

-- --- feature detection ---
S.check(rt.eval("return launcher.has('audio')") == true, "launcher.has('audio')")
S.check(rt.eval("return launcher.has('usbaudio')") == true, "launcher.has('usbaudio')")

-- --- unload releases the synth ---
rt.eval("audio.note(40, 100)")
S.check(rt.audio_enabled() == true, "on before the reload")
rt.load_string("t2", "function page.onInit() end")
local a = rt.audio()
S.check(#a >= 1 and a[1].op == "release", "loading another script releases the synth first (rt_unload)")
S.check(rt.audio_enabled() == false, "and it's off for the new script")
S.check(rt.audio_param(CUTOFF) == -1, "with its parameters back to defaults")

S.done()
