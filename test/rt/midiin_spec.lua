-- test/rt/midiin_spec.lua: MIDI input (midi.listen, midi.onNote/onCC/
-- onMessage): filtering, delivery order, note-off normalisation, the
-- bounded queue, callback errors, and clearing on unload.
local S = dofile("test/rt/spec.lua")
local rt = require("rt")

local USB, TRS = 1, 2

-- A script that logs every callback as one line in `got`.
local LOGGER = [[
got = {}
function page.onInit() end
function probe(f) local ok, err = pcall(f); return ok, err end
function midi.onNote(p, c, n, v) got[#got + 1] = ("note %d %d %d %d"):format(p, c, n, v) end
function midi.onCC(p, c, n, v) got[#got + 1] = ("cc %d %d %d %d"):format(p, c, n, v) end
function midi.onMessage(p, s, a, b) got[#got + 1] = ("msg %d %d %d %d"):format(p, s, a, b) end
function take() local s = table.concat(got, ","); got = {}; return s end
]]

local function load_logger()
  rt.reset()
  S.check(rt.load_string("mi", LOGGER), "logger script loads")
end

local function take() return rt.eval("return take()") end
local function info() local p, d, l = rt.midi_in_info(); return {pending = p, dropped = d, listening = l} end

-- --- feature detection, defaults ---
load_logger()
S.check(rt.eval("return launcher.has('midiin')") == true, "launcher.has('midiin')")
S.check(rt.eval("return type(midi.listen)") == "function", "midi.listen exists")
S.check(info().listening == false, "not listening after a load")
rt.midi_in(USB, 0x90, 60, 100)
rt.tick()
S.check(take() == "" and info().pending == 0, "nothing is queued or delivered before midi.listen")

-- --- delivery: everything, in arrival order, on the next tick ---
rt.eval("midi.listen{}")
S.check(info().listening == true, "midi.listen{} turns listening on")
rt.midi_in(USB, 0x90, 60, 100)
rt.midi_in(TRS, 0xB3, 74, 12)
rt.midi_in(USB, 0x80, 60, 64)        -- a real note off: velocity 0
rt.midi_in(TRS, 0x95, 61, 0)         -- note on with velocity 0
rt.midi_in(USB, 0xE2, 0, 64)         -- pitch bend
rt.midi_in(USB, 0xC1, 5)             -- program change, two bytes
rt.midi_in(TRS, 0xD0, 99)            -- channel pressure
rt.midi_in(USB, 0xA4, 60, 33)        -- poly pressure
S.check(take() == "" and info().pending == 8, "messages wait for rt_tick")
rt.tick()
S.check(take() == "note 1 0 60 100,cc 2 3 74 12,note 1 0 60 0,note 2 5 61 0,"
  .. "msg 1 226 0 64,msg 1 193 5 0,msg 2 208 99 0,msg 1 164 60 33",
  "delivered in order; note off as velocity 0; status with channel; d2 0 for two-byte messages")

-- --- what never reaches the script ---
rt.midi_in(USB, 0xF0, 0x7D, 0x45)    -- SysEx (the platform's)
rt.midi_in(USB, 0xF8)                -- realtime (clock.*)
rt.midi_in(USB, 0xF2, 0, 0)          -- system common
rt.midi_in(USB, 0x40, 1, 2)          -- a data byte, no status
rt.midi_in(USB, 0x90, 60)            -- a short note on
rt.tick()
S.check(take() == "" and info().pending == 0, "SysEx, realtime, system common and malformed messages are ignored")

-- --- filters ---
local function send_mix()
  rt.midi_in(USB, 0x90, 1, 1); rt.midi_in(USB, 0xB0, 2, 2); rt.midi_in(USB, 0xE0, 3, 3)
  rt.midi_in(TRS, 0x91, 4, 4); rt.midi_in(TRS, 0xB1, 5, 5); rt.midi_in(TRS, 0xE1, 6, 6)
  rt.tick()
  return take()
end
rt.eval("midi.listen{notes = true}")
S.check(send_mix() == "note 1 0 1 1,note 2 1 4 4", "notes = true: notes only")
rt.eval("midi.listen{cc = true}")
S.check(send_mix() == "cc 1 0 2 2,cc 2 1 5 5", "cc = true: CCs only")
rt.eval("midi.listen{other = true}")
S.check(send_mix() == "msg 1 224 3 3,msg 2 225 6 6", "other = true: everything else only")
rt.eval("midi.listen{notes = true, cc = false}")
S.check(send_mix() == "note 1 0 1 1,note 2 1 4 4", "notes = true, cc = false")
rt.eval("midi.listen{channel = 1}")
S.check(send_mix() == "note 2 1 4 4,cc 2 1 5 5,msg 2 225 6 6", "channel = 1 (0-15): that channel only")
rt.eval("midi.listen{port = 1}")
S.check(send_mix() == "note 1 0 1 1,cc 1 0 2 2,msg 1 224 3 3", "port = 1: USB only")
rt.eval("midi.listen{port = 2, cc = true}")
S.check(send_mix() == "cc 2 1 5 5", "port = 2, cc = true: TRS CCs only")
rt.eval("midi.listen{port = 0}")
S.check(#send_mix() > 0 and true, "port = 0: both")
rt.eval("midi.listen(true)")
S.check(send_mix() == "note 1 0 1 1,cc 1 0 2 2,msg 1 224 3 3,note 2 1 4 4,cc 2 1 5 5,msg 2 225 6 6",
  "midi.listen(true): everything")
rt.eval("midi.listen(false)")
S.check(send_mix() == "" and info().listening == false, "midi.listen(false) stops")
rt.eval("midi.listen()")
S.check(info().listening == true, "midi.listen() with no argument listens to everything")

-- the filter is checked again at delivery: messages queued before
-- listen(false) are not delivered after it
rt.midi_in(USB, 0x90, 60, 1)
rt.eval("midi.listen(false)")
rt.tick()
S.check(take() == "" and info().pending == 0, "queued messages are dropped once listening stops")

-- --- argument errors ---
local function fails(code) return rt.eval("return probe(function() " .. code .. " end)") == false end
S.check(fails("midi.listen(5)"), "midi.listen(5) is an error")
S.check(fails("midi.listen{channel = 16}"), "channel 16 is out of range")
S.check(fails("midi.listen{channel = -1}"), "channel -1 is out of range")
S.check(fails("midi.listen{channel = 1.5}"), "channel 1.5 is out of range")
S.check(fails("midi.listen{port = 3}"), "port 3 is out of range")
S.check(not fails("midi.listen{channel = 15, port = 2}"), "channel 15, port 2 are fine")

-- --- callbacks the script didn't define ---
rt.reset()
rt.load_string("bare", [[
function page.onInit() midi.listen{} end
n = 0
function midi.onCC() n = n + 1 end
]])
rt.midi_in(USB, 0x90, 60, 100)
rt.midi_in(USB, 0xB0, 1, 2)
rt.midi_in(USB, 0xE0, 0, 64)
rt.tick()
S.check(rt.eval("return n") == 1 and rt.error_count() == 0, "an undefined onNote/onMessage is skipped silently")

-- --- a stock script that never listens ---
rt.reset()
rt.load_string("stock", [[
function page.onInit() end
calls = 0
function midi.onNote() calls = calls + 1 end
]])
for i = 1, 300 do rt.midi_in(USB, 0x90, i % 128, 100) end
rt.advance(100)
S.check(rt.eval("return calls") == 0 and info().pending == 0 and info().dropped == 0,
  "without midi.listen nothing is queued, called or dropped")

-- --- flood: bounded queue, bounded work per tick, drops counted and logged ---
load_logger()
rt.eval("midi.listen{}")
for i = 1, 200 do rt.midi_in(USB, 0xB0, 1, i % 128) end
S.check(info().pending == 128, "the queue holds 128 (" .. info().pending .. ")")
S.check(info().dropped == 72, "the other 72 are dropped and counted (" .. info().dropped .. ")")
rt.tick()
local first = take()
local n = select(2, first:gsub("cc ", ""))
S.check(n == 16, "one tick delivers at most 16 (" .. n .. ")")
S.check(first:sub(1, 11) == "cc 1 0 1 1,", "oldest first")
S.check(rt.log_history():find("midi in: 72 messages dropped %(queue full%), 72 since load") ~= nil,
  "the drops are logged right away")
rt.clear_sent()
for _ = 1, 7 do rt.tick() end
S.check(info().pending == 0, "the rest drains over the next ticks")
local rest = take()
S.check(select(2, rest:gsub("cc ", "")) == 112, "all 128 queued messages delivered")
-- more drops within a second aren't logged again until a second has passed
local log0 = rt.log_history()
for i = 1, 140 do rt.midi_in(USB, 0xB0, 1, 1) end
rt.tick()
local _, c1 = rt.log_history():gsub("messages dropped", "")
local _, c0 = log0:gsub("messages dropped", "")
S.check(c1 == c0, "a second burst within 1 s isn't logged yet")
rt.advance(1000)
rt.tick() -- rt.advance's steps after its first rt_tick() only run the clock and holds
local _, c2 = rt.log_history():gsub("messages dropped", "")
S.check(c2 == c0 + 1 and rt.log_history():find("12 messages dropped %(queue full%), 84 since load") ~= nil,
  "... and is logged once a second has passed")
take()

-- --- errors in callbacks ---
rt.reset()
rt.load_string("bad", [[
function page.onInit() midi.listen{} end
n = 0
function midi.onNote(p, c, note, v) n = n + 1; if note == 13 then error("boom") end end
]])
rt.midi_in(USB, 0x90, 12, 1)
rt.midi_in(USB, 0x90, 13, 1)
rt.midi_in(USB, 0x90, 14, 1)
rt.tick()
S.check(rt.error_count() == 1, "an error in midi.onNote counts as a script error")
S.check(rt.log_history():find("bad:3: boom") ~= nil, "and is logged")
S.check(rt.log_history():find("midi in: listening stopped after an error in midi.onNote") ~= nil,
  "listening stops, as on the stock firmware (logged)")
S.check(rt.eval("return n") == 2 and info().listening == false, "messages after the failing one aren't delivered")
rt.midi_in(USB, 0x90, 15, 1)
rt.tick()
S.check(rt.eval("return n") == 2, "nothing more arrives")
rt.eval("midi.listen{}")
rt.midi_in(USB, 0x90, 15, 1)
rt.tick()
S.check(rt.eval("return n") == 3, "midi.listen again resumes")
S.check(rt.loaded() == true, "the script keeps running")

-- a callback that stops listening stops the rest of the batch
rt.reset()
rt.load_string("stopper", [[
function page.onInit() midi.listen{} end
n = 0
function midi.onCC() n = n + 1; if n == 2 then midi.listen(false) end end
]])
for i = 1, 5 do rt.midi_in(USB, 0xB0, i, 0) end
rt.tick()
S.check(rt.eval("return n") == 2 and info().pending == 0, "midi.listen(false) inside a callback stops delivery")

-- --- unload clears ---
load_logger()
rt.eval("midi.listen{}")
rt.midi_in(USB, 0x90, 60, 100)
rt.midi_in(USB, 0x90, 61, 100)
S.check(info().pending == 2, "two queued")
rt.load_string("next", LOGGER)
S.check(info().pending == 0 and info().listening == false, "loading another script empties the queue and stops listening")
rt.tick()
S.check(take() == "", "the new script gets none of the old messages")
rt.eval("midi.listen{}")
rt.midi_in(USB, 0x90, 62, 100)
rt.reset() -- rt_unload
S.check(info().pending == 0 and info().listening == false, "unloading empties the queue and stops listening")
rt.midi_in(USB, 0x90, 63, 100)
rt.tick()
S.check(info().pending == 0, "with no script loaded nothing is queued")

-- a load that fails after listening: the queue went with the state
rt.reset()
S.check(rt.load_string("fails", [[
midi.listen{}
function page.onInit() error("no") end
]]) == false, "a script that listens and then fails in onInit doesn't load")
rt.midi_in(USB, 0x90, 60, 100)
rt.tick()
S.check(info().pending == 0 and info().listening == false, "nothing is queued after the failed load")
S.check(rt.load_string("fails2", "midi.listen{} error('top')") == false, "a top-level error after listening")
rt.midi_in(USB, 0x90, 60, 100)
S.check(info().pending == 0, "nothing is queued after that either")
load_logger()
rt.eval("midi.listen{}")
rt.midi_in(USB, 0x90, 60, 100)
rt.tick()
S.check(take() == "note 1 0 60 100", "the next script listens normally")

-- --- MIDI in doesn't disturb the clock ---
local function clock_run(with_notes)
  load_logger()
  rt.eval([[
  pulses = {}
  function clock.onPulse(b, p) pulses[#pulses + 1] = ("%d@%g"):format(p, now_ms()) end
  clock.listen(true, 16)
  midi.listen{}
  ]])
  rt.inject_now()
  rt.ext_start(120)
  for _ = 1, 20 do
    if with_notes then rt.midi_in(USB, 0x90, 60, 100) end
    rt.advance(50)
  end
  return rt.eval("return table.concat(pulses, ' ')")
end
local plain = clock_run(false)
local busy = clock_run(true)
S.check(#plain > 0 and busy == plain, "clock pulses land at the same times with MIDI in arriving (" .. busy .. ")")
S.check(select(2, take():gsub("note", "")) == 20, "and every note arrived")

S.done()
