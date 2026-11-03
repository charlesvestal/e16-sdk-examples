-- MIDI monitor demo: shows the last few messages arriving on USB or the TRS
-- MIDI input (midi.listen, midi.onNote/onCC/onMessage) and lights a ring
-- for each: a note lights ring (note % 16) + 1 as bright as its velocity,
-- fading out after the note off; a CC fills ring (cc % 16) + 1 like a
-- meter, in green.
--
-- The top line counts messages per port (U = USB, T = TRS in) and shows
-- the channel filter. Turn encoder 1 to listen to one channel (1-16) or
-- all. The log gets a line per message (up to 8 a second) and a total
-- every 2 s.
--
-- On firmware without MIDI input for scripts, the labels say so.
--@assign id=1 abbr="Ch" name="Channel" l=0 h=127 manual=true g=1

local HAS = launcher ~= nil and launcher.has("midiin") and launcher.has("draw")
local HAS_RING = launcher ~= nil and launcher.has("ring")

local NAMES = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}
local LINES = 5

local lines = {}                       -- newest first
local count = {0, 0}                   -- per port: 1 USB, 2 TRS
local reported = -1                    -- total at the last summary line
local budget, ticks = 8, 0             -- log lines left this second
local channel = 0                      -- 0 = all, else 1-16
local level = {}                       -- per ring: note brightness 0-255
local held = {}                        -- per ring: a note is still down
local meter = {}                       -- per ring: CC value, or nil
local changed = {}                     -- per ring: needs drawing
local dirty = true                     -- the screen needs drawing

for r = 1, 16 do level[r] = 0; held[r] = false end

local function note_name(n)
  return NAMES[n % 12 + 1] .. (n // 12 - 1)
end

local function add(port, ch, text)
  count[port] = count[port] + 1
  local line = (port == 1 and "U" or "T") .. (ch + 1) .. " " .. text
  table.insert(lines, 1, line)
  lines[LINES + 1] = nil
  if budget > 0 then
    budget = budget - 1
    print("midimon: " .. line)
  end
  dirty = true
end

function midi.onNote(port, ch, note, vel)
  local r = note % 16 + 1
  if vel > 0 then
    add(port, ch, "on " .. note_name(note) .. " " .. vel)
    level[r], held[r], meter[r] = vel * 2 + 1, true, nil
    changed[r] = true
  else
    add(port, ch, "off " .. note_name(note))
    held[r] = false
  end
end

function midi.onCC(port, ch, cc, value)
  add(port, ch, "cc " .. cc .. " = " .. value)
  local r = cc % 16 + 1
  meter[r], level[r], held[r] = value, 0, false
  changed[r] = true
end

function midi.onMessage(port, status, d1, d2)
  local kind, ch = status & 0xF0, status & 0x0F
  if kind == 0xE0 then
    add(port, ch, "bend " .. ((d2 << 7 | d1) - 8192))
  elseif kind == 0xC0 then
    add(port, ch, "prog " .. d1)
  elseif kind == 0xD0 then
    add(port, ch, "press " .. d1)
  elseif kind == 0xA0 then
    add(port, ch, "poly " .. note_name(d1) .. " " .. d2)
  end
end

local function listen()
  midi.listen{channel = channel > 0 and channel - 1 or nil}
  slots.update(1, channel > 0 and ("Ch" .. channel) or "All")
  dirty = true
end

local function draw_screen()
  draw.clear()
  draw.text(0, 0, ("U %d  T %d"):format(count[1], count[2]))
  draw.text(96, 0, channel > 0 and ("ch" .. channel) or "all")
  draw.line(0, 9, 127, 9)
  for i, line in ipairs(lines) do
    draw.text(0, 1 + i * 9, line)
  end
  draw.show()
end

-- Only the rings that changed: a flood of CCs would otherwise cost 256
-- ring.set calls every frame.
local function draw_rings()
  local any = false
  for r = 1, 16 do
    if changed[r] then
      changed[r], any = false, true
      if meter[r] then
        local n = (meter[r] * 16 + 64) // 128
        ring.fill(r, 0, 0, 0)
        for led = 1, n do ring.set(r, led, 0, 160, 0) end
      else
        local v = level[r]
        ring.fill(r, v, v // 3, 0)
      end
    end
  end
  if any then ring.show() end
end

function system.update()
  if not HAS then return end
  ticks = ticks + 1
  if ticks % 50 == 0 then budget = 8 end          -- 50 updates = 1 s
  if ticks % 100 == 0 and count[1] + count[2] ~= reported then
    reported = count[1] + count[2]
    print(("midimon: %d messages (usb %d, trs %d)"):format(reported, count[1], count[2]))
  end
  for r = 1, 16 do
    if not held[r] and level[r] > 0 then
      level[r] = math.max(0, level[r] - 12)
      changed[r] = true
    end
  end
  if dirty then draw_screen() end
  if HAS_RING then draw_rings() end
  dirty = false
end

function controller.onEncoderTurn(e)
  if e.id ~= 1 or e.increment == 0 then return end
  channel = (channel + (e.increment > 0 and 1 or -1)) % 17
  listen()
end

function page.onInit()
  if not HAS then
    slots.update(1, "need")
    slots.update(2, "midi")
    return
  end
  controller.set(1, {manual = true, v = 8192})
  system.setUpdateRate(20)
  listen()
end
