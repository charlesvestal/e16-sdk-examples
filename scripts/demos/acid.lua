-- Acid demo: a 16-step 303-style line on the launcher's internal synth
-- (audio.*), which plays on TRS 2 while this runs (TRS 1 stays MIDI).
-- Connect TRS 2 to a line input (not headphones: the jack carries DC).
--
--   Encoder 1 cutoff     2 resonance   3 decay   4 accent (how many steps)
--   Encoder 5 env mod    6 tempo       7 waveform (saw / square)
--   Press encoder 8 to stop / play.
--
-- Ring 16 shows the steps: blue = a note, red = accented, green = slides
-- into the next, white = the step playing now.
--
-- A note arriving on USB or the TRS MIDI input (any channel) transposes
-- the line: that note, folded into C1-C4, becomes the root.
--
-- On firmware without audio.* the script only says so on the labels.
--@assign id=1  abbr="Cut"  name="Cutoff"    l=0 h=127 manual=true g=1
--@assign id=2  abbr="Res"  name="Resonance" l=0 h=127 manual=true g=2
--@assign id=3  abbr="Dec"  name="Decay"     l=0 h=127 manual=true g=3
--@assign id=4  abbr="Acc"  name="Accents"   l=0 h=127 manual=true g=4
--@assign id=5  abbr="Env"  name="Env mod"   l=0 h=127 manual=true g=5
--@assign id=6  abbr="BPM"  name="Tempo"     l=0 h=127 manual=true g=6
--@assign id=7  abbr="Wave" name="Waveform"  l=0 h=127 manual=true g=7
--@assign id=24 abbr="Play" name="Play/Stop" p=true g=8

local HAS_AUDIO = launcher ~= nil and launcher.has("audio")
local HAS_RING = launcher ~= nil and launcher.has("ring")
local HAS_MIDIIN = launcher ~= nil and launcher.has("midiin")

local ROOT = 36                                    -- C2
local NOTE = {0, 0, 12, 0, 3, 0, 15, 12, 7, 0, 0, 12, 5, 3, 10, 0}
local GATE = "1111101111011111"
local SLIDE = "0010001000100110"
-- accents are added in this order as encoder 4 goes up
local ACC_ORDER = {1, 9, 7, 13, 3, 11, 15, 5, 2, 10, 14, 6, 4, 12, 8, 16}

-- cutoff, resonance, decay, accents (0-16), envmod, bpm, waveform (0/1)
local V = {40, 96, 35, 4, 80, 126, 0}
local LO = {0, 0, 0, 0, 0, 60, 0}
local HI = {127, 127, 127, 16, 127, 200, 1}
local NAME = {"cutoff", "resonance", "decay", nil, "envmod", nil, "waveform"}

local acc = {}
local step, held, slide_from, run = 0, nil, false, true

local function on(s, str) return str:sub(s, s) == "1" end

local function set_accents()
  for i = 1, 16 do acc[i] = false end
  for i = 1, V[4] do acc[ACC_ORDER[i]] = true end
end

local function apply(k)
  if NAME[k] then audio.set(NAME[k], k == 7 and V[k] * 127 or V[k]) end
  if k == 4 then set_accents() end
  if k == 6 then clock.setInternalBpm(V[6]) end
  local v = V[k]
  slots.update(k, k == 7 and (v == 1 and "Sqr" or "Saw") or tostring(v))
  leds.updateByIndex(k, (v - LO[k]) * 16383 // (HI[k] - LO[k]), 30)
end

local function draw_steps()
  if not HAS_RING then return end
  for s = 1, 16 do
    if s == step and run then
      ring.set(16, s, 255, 255, 255)
    elseif not on(s, GATE) then
      ring.set(16, s, 0, 0, 0)
    elseif acc[s] then
      ring.set(16, s, 160, 0, 0)
    elseif on(s, SLIDE) then
      ring.set(16, s, 0, 140, 0)
    else
      ring.set(16, s, 0, 0, 120)
    end
  end
  ring.show()
end

local function release()
  if held then audio.note(held, 0) end
  held = nil
end

-- One 16th: a 303 slide holds the previous note into this one, so the
-- new note starts before the old one ends and the pitch glides.
local function tick()
  step = step % 16 + 1
  if on(step, GATE) then
    local n, v = ROOT + NOTE[step], acc[step] and 127 or 90
    if slide_from and held then
      audio.note(n, v, true)
      if held ~= n then audio.note(held, 0) end
    else
      release()
      audio.note(n, v, false)
    end
    held = n
    slide_from = on(step, SLIDE)
  else
    release()
    slide_from = false
  end
  draw_steps()
end

function clock.onPulse(b, p)
  if not run or not HAS_AUDIO then return end
  if p % 6 == 0 then
    tick()
  elseif p % 6 == 3 and not slide_from then
    release()                                       -- half-step gate
  end
end

function midi.onNote(port, ch, note, vel)
  if vel == 0 then return end
  while note < 24 do note = note + 12 end
  while note > 60 do note = note - 12 end
  ROOT = note                                       -- from the next step on
end

function controller.onEncoderTurn(e)
  local k, d = e.id, e.increment
  if d == 0 or k < 1 or k > 7 then return end
  local step_size = (k == 4 or k == 7) and 1 or (k == 6 and 1 or 2)
  V[k] = math.max(LO[k], math.min(HI[k], V[k] + (d > 0 and step_size or -step_size)))
  apply(k)
  draw_steps()
end

function controller.onEncoderPress(e)
  if e.id ~= 24 then return end
  run = not run
  if run then
    step = 0
    clock.startInternal()
  else
    clock.stopInternal()
    release()
    audio.off()
  end
  slots.update(8, run and "Stop" or "Play")
  draw_steps()
end

function page.onInit()
  if not HAS_AUDIO then
    slots.update(1, "need")
    slots.update(2, "audi")
    return
  end
  for k = 1, 7 do
    controller.set(k, {manual = true, v = 8192})
    apply(k)
  end
  audio.set("volume", 110)
  audio.set("glide", 40)
  audio.enable(true)
  slots.update(8, "Stop")
  clock.listen(true, 96)
  if HAS_MIDIIN then midi.listen{notes = true} end
  clock.setInternalBpm(V[6])
  clock.startInternal()
end
