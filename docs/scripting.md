# Writing scripts

Scripts are plain oxi-e16-lua scripts: anything that runs on the stock E16
also runs here unchanged, using the stock `midi`, `leds`, `slots`, `page`,
`controller`, `var`, `store`, `system` and `clock` API. `docs/compat-notes.md`
documents the few places this runtime's behavior was checked against the
stock mock. The built-in scripts' manuals, and how scripts get loaded onto a
stock E16, are in the [oxi-e16-lua README](https://github.com/charlesvestal/oxi-e16-lua).

## Disk script headers

A script dropped into `/scripts` can carry a header comment line:

```lua
--@scene title="My Script" pages="A,B" pad_pages=1 settings_page=2
```

Without it, the menu title is the file name (without `.lua`), there's one
pad page, no settings page, and the pages are derived from the control ids
the script assigns. `--@assign` lines (the same format `tools/embed_scripts.py`
uses for the built-in scripts) describe each control the same way they
always have. The scene name a script's saved vars/store are keyed under is
the file name without `.lua`, lowercased and compared case-insensitively
(FAT's own case rules). A script whose name matches a built-in's replaces
it in the menu, in place — unless it's byte-for-byte what the first boot
wrote for that built-in, in which case the built-in stays. A header that
doesn't parse still lists the script, marked broken; starting it shows the
file, line and message instead of running anything.

## Launcher APIs

Beyond the stock API, scripts on this firmware get:

- **`draw.*`** — a 1-bit canvas, 128×54 under the title bar or the full
  128×64 with `draw.fullscreen(true)`: `draw.clear()`, `draw.pixel(x, y
  [, on])`, `draw.line(x0, y0, x1, y1 [, on])`, `draw.rect(x, y, w, h
  [, fill] [, on])`, `draw.text(x, y, s [, on])`, `draw.show()` (marks it
  dirty; the screen refreshes at most every 20 ms), `draw.fullscreen(bool)`,
  `draw.release()` (back to the label grid), `draw.width()`, `draw.height()`.
  The canvas replaces the label grid once the script has drawn something
  since load; `draw.clear()` plus `draw.release()` brings the labels back.
- **`ring.*`** — per-LED color on any of the 16 rings: `ring.set(r, led,
  red, green, blue)`, `ring.fill(r, red, green, blue)`, `ring.clear(r)`,
  `ring.release(r)`, `ring.show()`. Channels are 0–255, scaled to the
  existing brightness cap. Writing a ring puts it in "custom" per-LED mode
  until `ring.release(r)` or `leds.reset(r)`; while custom,
  `leds.updateByIndex` on that ring does nothing.
- **`audio.*`** — the internal 303-style synth voice, played over the
  E16's USB audio input (MIDI out port 4 drives the same voice; TRS 1
  and TRS 2 are always plain MIDI): `audio.enable(bool)`,
  `audio.note(note, velocity[, slide])`, `audio.off()`,
  `audio.set(name, value)` for `cutoff`, `resonance`, `envmod`, `decay`,
  `waveform`, `volume`, `glide`, `slide` (values 0–127), and
  `audio.output([where])`, which returns where the synth plays (`"usb"`).
  The synth runs from the first thing the script does with it until the
  script is unloaded or calls `audio.enable(false)`, and the title bar
  shows a small speaker icon meanwhile. With no computer connected it
  plays nowhere. See [usb-audio.md](usb-audio.md), which also covers the optional analog
  output on TRS 2 (off by default).
- **MIDI out port 4** drives the same synth from `midi.sendMidi`/
  `midi.sendCC` without touching `audio.*` directly: note on/off (velocity
  ≥ 100 = accent), and CC 74 cutoff, CC 71 resonance, CC 75 env mod, CC 72
  decay, CC 70 waveform (< 64 saw, else square), CC 7 volume, CC 65 slide
  (≥ 64 on), CC 5 glide time, CC 120/123 all notes off. `out = 0` (all
  ports) does *not* include the synth, so an existing script makes no sound
  on port 4 unless it's told to use it. A note left on at port 4 with
  nothing else sent there for 2 s is released automatically, so a script
  that just changes its output port doesn't leave a drone stuck on.
- **MIDI in** — `midi.listen` plus callbacks the script defines in the
  `midi` table, in the shape of OXI's API 1.3.0 (whose MIDI input this
  runtime didn't have before), with one extension, `midi.onMessage`:

  ```lua
  midi.listen{notes = true, cc = true}   -- or midi.listen() for everything
  function midi.onNote(port, ch, note, vel) end   -- vel 0 = note off
  function midi.onCC(port, ch, cc, value) end
  function midi.onMessage(port, status, d1, d2) end   -- launcher extension
  ```

  - **Ports**: `1` = USB (the same number as USB out in `midi.sendMidi`),
    `2` = the TRS MIDI input. (Out port 2 is TRS 1, so echoing a message
    back on the port it came in on goes USB→USB, TRS in→TRS 1.)
  - **Channels** are 0–15, as `midi.sendCC` takes them.
  - **`midi.listen{notes=, cc=, other=, channel=, port=}`**: `notes`,
    `cc` and `other` (pitch bend, channel and poly pressure, program
    change → `midi.onMessage`) are booleans; with none of them given, all
    three are on, otherwise only those set true. `channel` (0–15) and
    `port` (1 or 2; 0 = both) narrow it down; nil means all. Each call
    replaces the last. `midi.listen(true)` / `midi.listen()` take
    everything, `midi.listen(false)` stops.
  - **`midi.onNote`** gets a Note Off, and a Note On with velocity 0, as
    velocity 0 (a Note Off's release velocity is dropped).
    **`midi.onMessage`**'s `status` includes the channel, as
    `midi.sendMidi` takes it; `d2` is 0 for two-byte messages.
  - Listening is off when a script loads and stops when it unloads. As on
    the stock firmware, an error in one of these callbacks stops
    listening (it's logged and counted like any callback error, plus
    `midi in: listening stopped after an error in midi.onNote`); call
    `midi.listen` again to resume.
  - **Delivery**: what the script listens for is queued as it arrives
    (128 messages, in the script's Lua heap from its first
    `midi.listen`), and the main loop calls the script for at most 16 of
    them per pass, in arrival order, so a flood can't stall the screen,
    the LEDs or the clock. What doesn't fit is dropped and logged:
    `midi in: N messages dropped (queue full), M since load`, at once the
    first time, then at most once a second.
  - **Not delivered**: SysEx (the dev console's `F0 7D 45 …` stays the
    launcher's; there's no `controller.onSysex` yet), system common, and
    clock/transport, which drive `clock.*` as before (`clock.listen(true,
    96)` gives a callback per MIDI clock tick while transport runs).
  - Not on the stock firmware's API (yet, as far as OXI's notes go):
    `other=` and `midi.onMessage`. The stock guide's port and channel
    numbering for MIDI in hasn't been checked against a 1.3.0 device;
    the numbering above is this runtime's.

  `scripts/demos/midimon.lua` is a MIDI monitor built on this; the Acid
  demo transposes its line to the last note received.
- **`launcher.*`** — feature detection, so one script runs on the stock E16
  and here: `launcher.version` (`"1.2"`: MIDI in; `"1.1"` added
  `audio.output()` and USB audio), `launcher.has(name)` for `"draw"`,
  `"ring"`, `"audio"`, `"usbaudio"`, `"midiin"` or `"disk"`. `launcher` itself doesn't exist on stock
  firmware, so check `launcher ~= nil` first:

  ```lua
  local HAS_DRAW = launcher ~= nil and launcher.has("draw")

  function page.onInit()
    if not HAS_DRAW then
      slots.update(1, "need"); slots.update(2, "draw")
      return
    end
    -- draw.* from here on
  end
  ```

  `scripts/demos/canvas.lua`, `rings.lua`, `acid.lua` and `midimon.lua`
  are built-in examples of exactly this pattern for `draw`, `ring`,
  `audio` and `midiin`.
