# e16-launcher

Custom firmware for the [OXI E16](https://oxiinstruments.com) MIDI controller
that embeds Lua 5.4 and runs scripts written for the E16's stock Lua API,
plus a disk mode, a few new APIs (screen canvas, per-LED rings, an internal
synth that plays over USB audio), and native C apps that run from
RAM.

![The mirror page showing the E16 live, running the Drone app](docs/images/mirror.png)

What comes with it:

- **Built-in scripts** from [oxi-e16-lua](https://github.com/charlesvestal/oxi-e16-lua) (each one's manual is in
  that README): [Euclid](https://github.com/charlesvestal/oxi-e16-lua#euclid) (Euclidean drum sequencer),
  [LFOx16](https://github.com/charlesvestal/oxi-e16-lua#lfox16) (16 LFOs), [Chords](https://github.com/charlesvestal/oxi-e16-lua#chords) (chord pads),
  [Mod Seq](https://github.com/charlesvestal/oxi-e16-lua#mod-seq) (CC modulation sequencer),
  [Note Seq](https://github.com/charlesvestal/oxi-e16-lua#note-seq) (16-step note sequencer) and
  [TB-3PO](https://github.com/charlesvestal/oxi-e16-lua#tb-3po) (generative acid sequencer), plus the launcher's
  own demos for its new APIs: Canvas, Rings, Acid and MIDI Mon
  (`scripts/demos/`).
- **Native apps** (`apps/`): [x0x-lite](docs/x0x.md) (an 808 kit and a 303
  with TB-3PO), [Drone](docs/drone.md), a GIF player and small
  demos; [docs/apps.md](docs/apps.md) shows how to write your own.
- **Modes**: [Disk mode](#disk-mode) (the E16 as a USB drive),
  [Webcam mode](#webcam-mode) (a USB video synth) and the
  [Mirror](#mirror) (the E16 live in a browser, and remote control).

## Quick start

### Build

Firmware (default), cross-compiled for the E16's GD32F405 with the
[E16 SDK](https://github.com/OXI-Instruments/E16-SDK):

```sh
cmake -S . -B build
cmake --build build
```

Output: `build/launcher.bin` (flash this), plus `.elf`, `.hex` and `.syx`.

`E16_DEV_CONSOLE` (default ON) builds in the developer SysEx commands
(virtual controls, screen dump, reset, corrupt-record, PWM-activity probe,
the synth without a script, the noise lab; see "Development" below). They let anything with a MIDI connection poke the
UI, so **release builds should turn it off**:
`cmake -S . -B build-release -DE16_DEV_CONSOLE=OFF`. The log, log replay and
reboot-to-bootloader messages stay in either way.

Host build, a native Lua interpreter (`e16rt-host`) used to run tests, built
with `LUA_32BITS` so Lua's numbers match the firmware's 32-bit integers and
floats:

```sh
cmake -S . -B build-host -DE16_HOST=ON
cmake --build build-host
build-host/e16rt-host -e 'print(math.type(1), 2^24+1 == 2^24)'
```

### Flashing

**First time** (stock firmware, or any firmware that doesn't answer the
SDK's reboot SysEx): with the E16 off, hold encoders 15 and 16 and press
Shift to power on. It shows up as "OXI E16 FW Update", then:

```sh
tools/reflash.sh build/launcher.bin
```

**After that**, with the launcher (or any firmware that answers the reboot
SysEx) already running and connected over USB, the same command reboots it
into the bootloader, uploads, and waits for the launcher's port to come
back. Shift + hold encoder 16 for 2 s also reboots the launcher into the
bootloader without the SysEx round trip.

### Submodules

```sh
git submodule update --init --recursive
```

`external/E16-SDK` is OXI's [E16 SDK](https://github.com/OXI-Instruments/E16-SDK),
which brings its own submodules (ST's CMSIS and HAL, TinyUSB 0.21.0).

## Using the launcher

**Menu**: turn encoder 1 to move the selection, press it to start. Scripts
from `/scripts` on the disk are listed too (marked), and a disk script
replaces a built-in of the same name. A native app (`/apps/*.e16app`,
marked "app") runs in its own clean boot instead of loading into Lua.
After them come "Disk mode" (when the volume is available), which enters
disk mode, and "Webcam", which turns the E16 into a USB camera (see
"Webcam mode").

**Running a script**, encoders go to the script except for these gestures
(Shift itself never reaches the script, and a press made while Shift is
down is consumed by the UI, release included, so the script never sees
half a press):

| Gesture | Effect |
|---|---|
| Shift tap (release within 1 s, no encoder pressed meanwhile) | opens the page picker: press encoder N to show page N (`page.onPageChange`), or tap Shift again or wait 4 s to close it |
| Shift + press encoder N (while Shift is held down) | shows page N directly, if the script has one, logged as `ui: page N (<name>) via shift+press` |
| Shift hold 1 s (no encoder pressed during the hold) | back to the menu (unloads the script, saving vars and store) |
| Shift + hold encoder 16 for 2 s | reboot into the bootloader, from anywhere |

Turns are dropped while Shift is held — there's no Shift+turn gesture yet.
On the error screen (a script that failed to load), pressing any encoder or
holding Shift 1 s returns to the menu.

## Disk mode

Selecting "Disk mode" from the menu unloads any running script (saving its
state first) and presents the E16's SPI flash as a USB drive. On macOS it
mounts as `/Volumes/E16`, holding `/scripts` (the six built-in scripts
plus the canvas/rings/acid/midimon demos, each with a `--@scene` header) and
`/apps` (native apps), plus a `README.TXT` at the root that documents the
header format on the device itself.

Leave disk mode by ejecting the volume on the computer, pressing encoder 1,
or holding Shift 1 s. Leaving flushes the block cache, remounts the
firmware's own view of the volume, and rescans `/scripts` and `/apps` for
the menu. Outside disk mode the volume isn't presented at all — the Mac
shows nothing to mount.

On Windows and Linux, the auto-exit-on-eject heuristic (watching FAT16's
clean-shutdown bit, since macOS's manual-eject path sends no SCSI eject
command) hasn't been tried — see `docs/hw-acceptance.md`'s "Needs a human"
section. Encoder 1 or a Shift hold always works regardless of platform.

**Wear**: it's FAT on raw NOR with no wear leveling, fine for copying
scripts now and then but not for heavy or constant rewriting. Persistence
(vars/store) never writes into the FAT volume's region — see "Safety
model" below.

## Webcam mode

Selecting "Webcam" from the menu turns the E16 into a USB video synth: it
re-enumerates (it disappears from the computer for about half a second) as
USB MIDI plus a UVC camera, "E16 Launcher" (or the name macOS cached),
160x120 at 20 fps, uncompressed YUY2. Any video app sees it: Photo Booth,
QuickTime, OBS, a browser, or

```sh
ffmpeg -f avfoundation -framerate 20 -video_size 160x120 -pixel_format yuyv422 \
    -i "E16 Launcher" -t 10 out.mp4
```

Leave by holding encoder 1 for 1 s, or Shift for 1 s: the E16 re-enumerates
back to MIDI + Disk mode's mass storage + USB audio, on the menu. While the
camera is on there's no mass storage and no USB audio input (the GD32's USB
core has the packet memory for MIDI + video or for the rest, not both); the
synth is silent meanwhile, and USB MIDI, the dev
console and the log keep working.

**Encoders** (turn = change, press = back to the default, except encoder
1's short press, which switches the OLED between status + a small dithered
mirror of the picture and a full-screen mirror):

| | 1 | 2 | 3 | 4 |
|---|---|---|---|---|
| **row 1** | pattern: plasma, tunnel, rings, bars, spiral, checks | hue | speed | zoom |
| **row 2** | pattern B | mix: how much of B (0-16) | mix mode: blend, add, xor, wipe | symmetry: off, mirror, quad, kaleidoscope |
| **row 3** | palette: rainbow, fire, ice, neon, mono | colour cycling (-16..16) | posterize (off, 2-16 bands) | contrast |
| **row 4** | brightness | warp (sideways wobble per line) | text overlay: off, the pattern names, the last MIDI note, "OXI E16" | scanlines (0-3) |

The OLED's status shows the stream state ("LIVE" with the frame rate, "NO
APP" while nothing captures), the patterns and the setting last changed;
the LED rings show the picture's colours on a 4x4 grid, each with a white
dot at its encoder's setting. The settings last until a reset.

**MIDI in** (USB or TRS, any channel): a note-on jumps the hue to the
note's pitch class and flashes the picture towards white by its velocity
(and shows the note name with the text overlay on "note"); CC 20-35 set
settings 1-16 across their range; CC 1 sets the mix; a program change
picks pattern A.

**How**: `usb_video_select()` (usb_video.h, from the SDK's `examples/usb_cam`) switches the USB
configuration at run time. There's no frame buffer: the picture is made a
line at a time from tables (`src/webcam/wc_render.c`), by a lowest-priority
interrupt that stays up to 12 lines ahead of the USB interrupt sending
them. 20 fps with any settings, at 21% of the CPU (one pattern) to 45%
(two patterns, kaleidoscope, warp); the log has `webcam: 10000 ms: ...`
every 10 s while the computer captures. Its ~23 KB of tables, palettes and
line buffers come from the Lua heap (free while no script runs) and go
back on leaving. `src/webcam/webcam.h` has the details.

## Mirror

A live view of the E16 in a browser, over the same USB MIDI cable: the
OLED pixel for pixel, the 16 LED rings in their colours, and every turn,
push and Shift press, on the E16 or from the page. The page can drive the
E16 too: scroll or drag on a knob to turn it, click to push, hold to keep
it pushed, hold the round "O" at the top left for Shift. It works in every
mode (the menu, scripts, native apps, Disk mode, Webcam mode).

Open it in Chrome or Edge (Web MIDI with SysEx; Safari and iOS browsers
have no Web MIDI, Firefox asks to install a site permission), and allow
MIDI when asked. Any of:

- **From the E16's disk**: the launcher keeps a copy at the root of its
  disk, `mirror.html` (written at boot when it's missing or differs from
  the firmware's copy, so it always matches the firmware). Enter Disk
  mode, copy it to the computer or open it from there (`file://` pages
  count as secure, so Web MIDI works).
- **From the repo**: open `tools/mirror/index.html` as a file, or run
  `tools/mirror/serve.sh` and open `http://localhost:8016/`.
- `index.html?demo` shows animated sample data without an E16.

**The page**: the drawn E16 (screenshot at the top of this README) with
the OLED, the 16 rings in their colours and the Shift button. Above it, a
status pill: "Live · <port name>", "Demo", or what's wrong, with a note
on how to fix it (no Web MIDI in this browser, MIDI access blocked, the
E16 not connected or not streaming, a page not opened from localhost or a
file). Below it, the current mode (menu,
script, app, Disk mode, Webcam) with what's selected or running, the
launcher's version and the mirror's frame rate, and an **Events** list
of the latest turns, pushes and Shift presses, the ones made from the
page marked as remote. Controls:

| | on the page |
|---|---|
| turn | scroll on a knob, or drag up/down |
| push | click a knob; hold to keep it pushed |
| Shift | hold the round "O" at the top left |
| keyboard | Tab to a knob, arrow keys to turn, Space to push |

**Settings** (the fold-out under the events, kept in the browser):
"LED 1 of each ring" picks where a ring's first LED sits, at 7 o'clock
running clockwise (the default) or at 5 o'clock running
counter-clockwise. The launcher fills rings from LED 1 clockwise, like a
knob's travel; OXI hasn't confirmed the physical order, so if the rings
on the page run the other way round than on your E16, flip this.

The page finds the ports by name ("E16 Launcher" or "E16 Custom"). The
stream runs only while something subscribes and keeps it alive (the page
sends a keepalive every second while it's visible; the E16 stops by
itself 3 s after the last one), so a closed tab costs nothing.

**What it costs**: the OLED goes in 32-column segments only when they
change, at most ~30 times a second; the rings likewise; events at once.
Every write is a try, so the stream never waits for USB: with the queue
full it carries on at the next pass. Measured on the device: 30.3 fps
with the Drone app animating (~17 KB/s) and no USB audio underruns over
30 s; 12.6 fps in Webcam mode, where all 16 rings change constantly
(~31 KB/s in all); the longest mirror poll ~1.0-1.3 ms including
interrupts. `tools/mirror/mirror_client.py` is a command-line client
(events, the OLED as PNG or text art, ring levels, remote actions); the
launcher logs `mirror: 10000 ms: ...` every 10 s while it streams.

**Protocol**: SysEx in the dev console's framing (`F0 7D 45 <cmd> ...
F7`): `20` subscribe/keepalive, `21` unsubscribe, `10` a virtual control
(as in `src/console.h`); the E16 sends `70` state, `71` an OLED segment,
`72` a ring, `73` an input event, `74` end of frame.
`src/mirror/mirror_core.h` has the details.

**Security**: while the mirror is built in, anything on the computer that
can send MIDI to the E16 can watch it and drive it (turn, push, Shift),
like the dev console's virtual controls. It's the CMake option
`E16_MIRROR` (default ON), independent of `E16_DEV_CONSOLE`; build with
`-DE16_MIRROR=OFF` to leave it (and the page on the disk) out.

## Writing scripts

Scripts are plain oxi-e16-lua scripts: anything that runs on the stock E16
also runs here unchanged, using the stock `midi`, `leds`, `slots`, `page`,
`controller`, `var`, `store`, `system` and `clock` API. `docs/compat-notes.md`
documents the few places this runtime's behavior was checked against the
stock mock. The built-in scripts' manuals, and how scripts get loaded onto a
stock E16, are in the [oxi-e16-lua README](https://github.com/charlesvestal/oxi-e16-lua).

### Disk script headers

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

### Launcher APIs

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
  plays nowhere. See "USB audio" below; for the optional analog output
  on TRS 2 (off by default), see "TRS 2 audio (optional)".
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

## USB audio

The E16 shows up on the computer as a USB audio input next to its MIDI
port: "E16 Launcher" (or the name macOS cached, e.g. "E16 Custom"; see
"USB name" below), one channel, 16-bit, 48 kHz, USB Audio Class 2.0 with
the operating system's own driver. Record it in any audio app, or e.g.
`sox -t coreaudio "E16 Launcher" -c 1 -b 16 out.wav trim 0 10`. The synth
plays there by default whenever a computer is connected, as clean
digital audio. It's the only audio output in the default build (see
"TRS 2 audio (optional)" below).

- **Mono**: the synth is one voice, so a second channel would only carry
  a copy (stereo would fit the USB FIFO too).
- **Level**: the voice's output is doubled and soft-clipped (a cubic
  curve, -0.3 dB at half scale, reaching full scale at 1.5) so it uses the
  whole 16-bit range: a held note peaks around -4 dBFS, an acid line
  around -1 dBFS, and nothing clips hard.
- **Rate**: the synth renders at 48 kHz always (TIM6, 84 MHz / 1750), for
  USB and (when built in) TRS 2 alike: one rate keeps the voice from being rebuilt when
  the computer starts or stops recording. That costs about half again the
  CPU of the earlier 32 kHz: 13% average while an acid line plays, 16%
  with a note held, 24% at most (`audio: load ...`, logged every 10 s).
- **Clock**: the E16 runs on its own crystal. The audio interrupt pushes
  one sample per tick into a 512-frame lock-free ring
  (`src/port_e16_usbaudio.c`); `fill()`, called by `usb_audio.c` from the USB
  interrupt once per millisecond while the computer records, takes 48
  frames, 49 when the ring runs above 2 ms + 16 frames, 47 below
  2 ms - 16, and the computer resamples from the packet sizes. Until the
  ring holds 2 ms (when recording starts, or after it ran dry) `fill()`
  returns 0 and `usb_audio.c` sends a packet of silence. While the computer
  records, `usbaudio: 10000 ms: packets 47:… 48:… 49:… short:… silent:…,
  underruns …, resyncs …, ring …` is logged every 10 s.
- **USB functions**: MIDI (EP1), Disk mode's mass storage (EP2) and audio
  (EP3 IN) are all in the one configuration; the GD32's 320-word FIFO RAM
  holds RX 56 + EP0 16 + MIDI 16 + MSC 16 + audio 25 words (mono) = 129.
  The camera needs the rest of the FIFO RAM, so it can't share the
  configuration with them: Webcam mode re-enumerates without them (see
  "Webcam mode"), so the synth is silent while it's on.
- **Where they come from**: the drive, the audio input and the camera are
  the SDK's examples (`usb_msc.c`, `usb_audio.c`, `usb_video.c` in
  `external/E16-SDK/examples/usb_disk`, `usb_tone`, `usb_cam`), compiled
  unchanged. The SDK takes two USB functions next to MIDI, so
  `src/usb_functions.c` gives it two: the drive, which leaves the
  descriptors while the camera is on, and one slot that is the camera in
  Webcam mode and the audio input otherwise. TinyUSB's classes for them
  are switched on in `src/tusb_config_app.h`.
- **Native apps** play the same synth (`synth_note`/`synth_param`), and
  can stream their own sound (`audio_start()`, see "Native apps"), with
  the same "auto" routing; the two are summed.


### TRS 2 audio (optional)

Off by default. Build with `-DE16_TRS_AUDIO=ON` to let the synth and app
audio play on TRS 2 as PWM (TIM8_CH1 on PC6). It isn't usable on this
hardware: the jack idles on the E16's internal supply, which carries a
loud whine from the OLED's power that changes with what the screen
shows. A listening test with the noise lab (below, "Development") found
it on the ring with stereo headphones and on the tip with a mono plug;
screen off removes the hum but leaves a crackle, and no OLED setting
makes it usable. In the default build `audio.output()`
still accepts `"trs2"`, `"both"` and `"auto"` so scripts keep working,
but they all mean USB, and asking for TRS 2 logs `audio: TRS 2 audio is
off in this build (E16_TRS_AUDIO), using USB`.

With the option on, `audio.output([where])` takes `"usb"`, `"trs2"`,
`"both"` or `"auto"` (the default: `"usb"` while a computer has the E16
connected over USB, `"trs2"` otherwise, and `"trs2"` in Webcam mode); it
returns where the synth plays now and goes back to `"auto"` when the
script unloads. While the synth is routed to TRS 2, TRS 2 switches from
MIDI to audio (after queued MIDI drains, and fading back afterwards), and
`out = 0` and `out = 3` skip TRS 2 so stray MIDI bytes never land on the
audio pin. The PWM is 8-bit at 656 kHz, and the jack's output stage only
pulls toward ground, so the level is low.

## Native apps

A native app is a compiled C program, `/apps/<name>.e16app`, that the
launcher loads straight into RAM and runs — no Lua, no interpreter.
Selecting one in the menu unloads Lua (saving the current script's state),
resets into a clean boot that brings up only the drivers and the disk, and
jumps into the app. Every way out — returning from `app_main()`,
`api->exit()`, holding Shift 1 s, a fault, or a smashed stack canary —
resets back into the normal launcher, which logs why.

**Format and ABI**: an `.e16app` (`src/apps/e16_app_format.h`) is a 76-byte
header (magic `E16A`, ABI version, text/data/bss sizes, GOT and fixup
tables, entry point, CRC-32, a 24-byte name) followed by the app's code
and read-only data, then its GOT and `.data`, then a table of data-pointer
fixups. The app is built position-independent
(`-fpic -msingle-pic-base -mpic-register=r9 -mno-pic-data-is-text-relative`),
so the loader can place its code and data independently and patch the GOT
and fixups by address range rather than needing a real linker at load
time. `src/apps/loader.c` validates the header and CRC, copies the text
into SRAM and the data into CCM, zeroes `.bss`, rebases the GOT and
fixups, and jumps to `app_main(const e16_api_t *api)` with r9 pointing at
the GOT. App mode remaps SRAM1 at 0x00000000 (SYSCFG) and runs the text
from that alias whenever it lies wholly in SRAM1 (`app mode: text at
0x200080b8, run at 0x000080b8`): the CPU then fetches it over its I-code
and D-code buses rather than the system bus, which it shares with the
stack and the peripherals. Measured with `apps/x0x/bench.c`, that makes
app code about 25% faster (one x0x build's factory loop: 1603 → 1190 cycles a
sample, with the launcher's own audio interrupt; the system bus costs
about 2 cycles an instruction). Drone went from 41% to 27%.
The vectors stay in flash (VTOR), and a reset puts the map back; a
fault in an app now reports its PC at the alias (`PC=0x000081f2` is
SRAM's 0x200081f2). Since the alias puts the launcher's own `.data` at
0x00000000, app mode also gives the MPU a no-access region from 0 up to
the largest power of two below the app's text (32 KB for a text at
0x81c8; 1 KB of the flash's boot alias without the remap), so an app's
NULL-pointer access faults instead of corrupting it (`app mode: MPU:
flash read-only, no access below 0x00008000`). API functions copy what
an app passes them before any DMA (`e16_api.h`, "Pointers"). `tools/mkapp.py` turns a linked ELF into an `.e16app`, and does
the build-time half of the safety check: it inspects every relocation
`--emit-relocs` kept and refuses anything the loader couldn't redo (an
absolute address baked into code, a PC-relative reference between text
and data, an undefined weak symbol, global constructors, an unexpected
section).

**Hardware access** is only through the versioned `e16_api_t` function
table (`src/apps/e16_api.h`), passed in as `app_main`'s argument: the
128×64 1-bit display framebuffer and `present()`, LEDs, encoders and
Shift, MIDI read/write, `millis()`/`delay_ms()`, read-only file access to
any path on the disk plus write access under `/apps/<name>/`, one log
line, the synth voice (`note`/`param`), `exit()`, and an audio stream of
the app's own. The table starts
with `abi_version` and `size`; new functions are only ever appended, and
`E16_API_HAS(api, field)` checks whether a given entry exists before an
app that was built against a newer header calls it.

**App audio**: `audio_start(render, user)` has the launcher call the
app's `render(out, 32, user)` every 32 frames (0.67 ms) to fill the next
block of mono float samples at 48 kHz, which go where the synth's go
(USB audio), summed with the
synth voice and clamped to ±1. The callback runs in a software-pended
interrupt (CAN2_TX's vector, which nothing else uses) at priority 8:
below every driver's interrupt, so it never delays a MIDI byte or a USB
packet, and above PendSV and the app's own code; the launcher calls it
with r9 = the app's GOT. The audio interrupt double-buffers the blocks
(`src/synth/app_stream.c`, host-tested by `test/synth/app_stream_test.c`).
In the callback an app may call `millis()` and nothing else. Its DWT
cycle count (less the audio interrupts that preempt it) is kept per
second and logged every 10 s (`audio: app load 41.2% (last second;
slowest call 46.7% of a block), 15000 calls in 10 s, 0 late since
start`; the load is the callback's own, the audio interrupts that
preempt it taken out). Guards: the callback's own load over 80%, or more
than 5% of the blocks late, 3 s in a row, or calls that took a whole
second's wall time (nothing else ran), stop the stream (logged; `audio_info()` says
`E16_AUDIO_OVERLOAD`; `audio_start()` restarts it), and a call that
hasn't returned in 500 ms ends the app from the SysTick (`launcher: back
from app <name> (audio callback stuck)`). `docs/apps.md` has the rules;
`apps/tests/audiostress` checks the guards on the device. The stream's
state lives in the api's app-mode buffers, so it costs the normal mode
nothing.

**Building an app**:

```sh
cmake -S apps -B build-apps
cmake --build build-apps
```

produces `build-apps/<name>.e16app` for each app in `apps/CMakeLists.txt`
(`e16_add_app(name sources...)`, from `apps/sdk/cmake/e16app.cmake`). Copy
the result to `/apps` on the E16's disk (via disk mode) to run it from the
menu. `apps/demos/hello` is the smallest example (exercises every kind of
address the loader has to rebase); `apps/demos/plasma` is a fuller one (a
dithered plasma/tunnel effect with a 256-LED rainbow, encoder controls,
and an on-screen/logged fps counter); `apps/tests/fault` deliberately
faults, to exercise the fault-recovery path.

**Capacities**: roughly 88 KB of code (SRAM, after the resident drivers,
the app's buffers and the app stack) and roughly 64 KB of data (all of
CCM, minus the fixed-size boot records at its top) — `tools/check_mem_layout.py`
checks these after every build and the device logs the exact figures at
load time (`app: <name> text N data M bss K`). Anything that doesn't fit
belongs in its own firmware, installed through OXI's bootloader, not as
an app (Doom, for one, doesn't fit the app budget).

**Starting only when asked**: the "run this app" record survives any
reset that keeps the RAM powered, the bootloader's after a reflash
included. So the launcher also writes a launch token, bound to that
record, right before its own reset into app mode, and every boot consumes
the token: the record is honoured only with its token *and* RCC_CSR
saying the reset was a software reset with no power-on, brown-out or
watchdog reset next to it (`src/apps/app_boot.h`; host-tested by
`test/apps/app_boot_test.c`). Anything else (a reflash while an app runs,
the reset pin) boots the menu, logging `launcher: not starting app ...`
if a request was pending. Every normal boot logs the reset flags
(`launcher: reset flags 0x10000000 (software)`) and clears them.

**Exits and fault handling**: a crash inside an app is caught by the same
fault handlers as the launcher itself, but while app mode is active they
reset back into the launcher (noting the fault in a RAM record that
survives the reset) instead of dropping into the bootloader. The next
normal boot logs what happened (`app: ...` / `launcher: last reset was a
fault at PC=... LR=...`) and clears the record. A refused image (bad
header, wrong ABI, bad CRC, oversized) is reported on the error screen
without ever being loaded.

**Trust model**: a native app is trusted, privileged code — it runs on
the CPU with the same privileges as the firmware, not in a sandbox. The
API table offers no function that writes the E16's internal flash, and
neither the launcher nor the app runtime ever writes it, but an app could
in principle reach the hardware directly. As a seat belt against stray
writes, app mode turns on the MPU before jumping into the app, making the
internal-flash controller's registers and the internal flash itself
read-only: an app that writes there takes a MemManage fault and ends back
in the launcher with the fault logged (`apps/tests/flashpoke` checks
this; `apps/tests/nullpoke` checks the NULL guard below). That catches accidents, not malice — a privileged app can switch
the MPU off again — so only install apps you trust, as you would a
firmware. Disk writes through the API are limited to the app's own
folder, `/apps/<name>/`, and to the SPI flash's erase budget
(`file_write()` returns `E16_EBUSY`, having changed nothing, when the
budget can't cover a write right now). The app runs on the launcher's
own main stack (it has no stack of its own); nothing in the api table
runs apps concurrently with anything else.

### GIF player

`apps/demos/player` plays animations on the OLED, with matching colours on
the LED rings. `tools/gif2e16.py` (needs Pillow) converts an animated GIF,
or any image or image sequence Pillow reads, into the player's `.e16v`
format. Each frame is stored ready to send to the screen at 128×64 1-bit,
along with 16×16 LED colours and its own delay:

```sh
tools/gif2e16.py cat.gif cat.e16v                    # letterbox, Floyd–Steinberg
tools/gif2e16.py cat.gif cat.e16v --fill --invert    # crop to fill; swap lit/dark
tools/gif2e16.py cat.gif cat.e16v --threshold 100    # hard threshold, no dither
tools/gif2e16.py cat.gif cat.e16v --preview cat.png  # the OLED ×4 + rings (.gif: animated)
```

Copy `player.e16app` to `/apps/` and the `.e16v` files to `/apps/player/`
in Disk mode. The app API can't list a folder, so the player plays the
files named in `/apps/player/playlist.txt`, one name per line.
`tools/gif2e16.py --playlist /Volumes/E16/apps/player` writes that list
for every `.e16v` already there. Then eject and start "player" from the
menu. It loops the first file. If it finds nothing playable, the screen
says where the files go.

| Control | Effect |
|---|---|
| encoder 1 (turn) | next / previous file |
| encoder 2 (turn) | speed, 25%–400% |
| encoder 3 (turn) | LED brightness, off–8 (default 3) |
| encoder 4 (press) | pause / resume |
| Shift held 1 s | back to the launcher |

The player logs `player: <file> N frames, X fps` every 5 s. The rings are
laid out as a 4×4 grid over the picture, and each ring shows the colours
around its own cell (the docstring in `tools/gif2e16.py` has the mapping
and the file format). `test/player/make_test_gifs.py` builds two test
animations into `build-player/`, and `test/player/match_dump.py` checks a
`tools/console.py screen` dump against an `.e16v`'s frames.

### Drone

`apps/drone` is an evolving drone synth on the app audio stream: six
detuned, drifting oscillators (sine → triangle → saw) on a chord over
the root, a sine sub and noise, drive into a resonant LP/BP/HP filter
with a slow LFO, a small freezable reverb, and a limiter; "evolve"
wanders the macros by itself. The 16 encoders are its 16 macros (root,
chord, detune, wave, cutoff, resonance, mode, drive, drift rate, drift,
evolve, level, space, decay, sub, air; pushes: octave, open voicing,
hold, mute, freeze), the rings show them, and the screen shows a scope
of the output. MIDI notes set the root, CCs the macros. Settings are
saved to `/apps/drone/`. It uses about 27% of the CPU (41% before app code ran from SRAM1's alias: "Native apps"). Copy
`drone.e16app` to `/apps/`; `docs/drone.md` has the controls, the CC map
and the details, and `docs/audio/drone-demo.wav` a 10 s sample.

### x0x-lite

`apps/x0x` is a groovebox ported from X0X (the user's TBD-16 groovebox):
seven 808 voices (BD, SD, low tom, clap, closed and open hat, cowbell;
8W8's circuit models) and a 303 (Open303 with Devilfish ranges and RAT
drive) with the TB-3PO line generator, on a 16-step sequencer with 16
patterns, swing, accent and slide, MIDI clock in and out, and MIDI notes
transposing the 303. The encoders are the 16 steps of the selected track
(push: on/off; turn: level, or the 303's pitch; Shift + turn: accent and
slide); Shift + push 1-8 picks the track, 9-12 the sound, global, pattern
and mute pages, 16 plays and stops. It plays its factory loop at launch,
saves to `/apps/x0x/`, uses 41-44% of the CPU on the factory loop, and
has a CPU guard that limits the drum voices sounding at once when a
pattern is dense. **It is GPL-3.0-only** (`apps/x0x/LICENSE`,
`apps/x0x/LICENSING.md`), unlike the rest of this repository. Copy
`x0x.e16app` to `/apps/`; `docs/x0x.md` has the controls, MIDI, the CPU
measurements and what was left out, and `docs/audio/x0x-demo.wav` a 10 s
sample.

## Safety model

**Internal flash**: OXI's bootloader is the only thing that ever writes
the E16's *internal* (MCU) flash at runtime — that's where the bootloader
itself and manufacturing calibration live, and a bad write there has no
recovery path. This firmware never does, and a CMake post-build step
enforces it: `tools/check_no_flash_writes.sh` runs `arm-none-eabi-nm` on
the linked `launcher.elf` and fails the build if any of
`HAL_FLASH_Program`, `HAL_FLASH_Unlock`, `HAL_FLASHEx_Erase[_IT]`,
`FLASH_Program_*`, `FLASH_Erase_Sector`, `FLASH_MassErase`, or anything
matching `HAL_FLASH(Ex)?_*Program*`/`*Erase*` is actually linked in (not
merely compiled — the SDK's HAL_FLASH module can still live unused inside
`libe16_bsp.a`; `--gc-sections` is what keeps it from ever being pulled
into the firmware the check inspects) or any option-byte entry point
(`HAL_FLASH_OB_Unlock`/`_Launch`, `HAL_FLASHEx_OBProgram`, ...), plus a
source grep over `src/` for direct register access that wouldn't go
through the HAL at all (`FLASH->CR`/`KEYR`/`SR`/`OPTCR`/`OPTKEYR`,
`FLASH_KEY1/2`, `FLASH_OPTKEY1/2`, any `0x40023Cxx` address literal). A
failing `nm` fails the check too. Native apps are the one exception to
"nothing else runs on the CPU": see "Native apps" above for how app mode
guards against a stray write from one. Separately, the SPI-flash side
(`src/disk/flash_e16.c`) refuses any access below `0x400000`, where the
stock firmware keeps its own data.

**External (SPI) flash** is a different, deliberately softer story:
writes there are fine, just rate-limited. Every sector erase on the SPI
flash — persistence saves and the FAT volume's write-back cache alike —
goes through a shared token-bucket limiter, `src/disk/flash_budget.c`:
about 1 erase/s sustained with a burst of up to 16, and any single sector
capped at 4 erases per rolling minute, so no bug or runaway script can
wear out the chip even on top of the append-only record format below.
Disk mode raises the budget for as long as it's active (~40 erases/s,
burst 256, no per-sector cap) because the host computer legitimately
rewrites FAT tables and directory sectors often while copying files; it
drops back to normal on leaving. An erase refused for being over budget
is deferred, not performed: persistence keeps the save in RAM and retries
on the next debounce tick, and the block device keeps its cache dirty and
retries the same way (disk mode's write path retries a refused erase
for a few seconds before failing the host's request outright; a verify
failure isn't retried). Three bounded jobs run under the Disk-mode budget
because the normal one can't cover them: the first boot's format (it
needs all 16 of the normal burst on non-blank flash), the flush before
every reset into or out of app mode, and Disk mode itself, which also
writes its cache back after 300 ms without I/O, so a copy that ends
without an eject (a pulled cable) doesn't lose its last sector. An app's
`file_write()` starts only if the normal budget can cover its worst case,
then runs to the end without the per-sector cap cutting it short
(`src/apps/app_files.h` has the measured numbers).

**Append-only persistence records**: var's 32 slots and store's buffers
live in RAM outside the Lua heap, bound to the running script's name
(`src/rt/rt.h`). `src/persist.c` saves them into a 512 KB reserve at the
top of the SPI flash, `0xF80000–0xFFFFFF` — never anywhere else, in
particular never into the FAT volume that occupies the rest of the chip.
The reserve holds 64 slots of two 4 KB sectors each (vars, store): slot 0
is the shared store, and a script's slot is found by hashing its name
with linear probing, so up to 63 scripts can each keep saved state
regardless of manifest order. A script only takes a slot once it has
something to keep (empty vars or an empty store claim nothing), and at
boot, if the disk mounted, the slots of scripts that no longer exist
(neither built in nor in `/scripts`) are freed (`persist: freed N
slots`). Within a slot's sector, saves are
append-only — a save writes a new record right after whatever's already
there, rather than erasing every time, and the sector is only erased (and
the new record written at offset 0) once the next record wouldn't fit. A
record's commit marker is written last, in a flash write of its own, so a
power loss mid-save leaves a torn, uncommitted record that's ignored in
favor of whatever was there before. A load reads the *last* valid record
in the sector. Vars save 5 s after a change and on exit; store only saves
on exit, so store changes are lost if the E16 loses power while a script
is running. At a typical vars record size this gives roughly one erase
every 75 s of continuous encoder turning, which works out to on the order
of 2,200 hours of nonstop turning before a slot's vars sector nears a
typical 100,000-erase NOR rating.

**Flash map** (16 MB SPI NOR):

| Range | Contents |
|---|---|
| `0x000000-0x3FFFFF` | Stock firmware data. Never touched. |
| `0x400000-0xF7FFFF` | FAT volume (disk mode): scripts, apps, and anything else copied there. |
| `0xF80000-0xFFFFFF` | Persistence reserve: the name-keyed slot table above. |

## Development

`tools/console.py` (needs `pip install mido python-rtmidi`) talks to the
launcher over USB MIDI SysEx. It finds any port with "E16" in its name
except the bootloader's "FW Update".

```sh
tools/console.py                 # follow the log (replays the boot lines first)
tools/console.py --once          # print one line and exit
# dev console only (E16_DEV_CONSOLE=ON):
tools/console.py screen          # the OLED as text art
tools/console.py turn N STEPS    # turn encoder N (1-16)
tools/console.py press N         # press and release encoder N
tools/console.py down N | up N   # N may be "shift"
tools/console.py shift-tap       # open/close the page picker
tools/console.py shift-hold [S]  # hold Shift (default 1.3 s: back to the menu)
tools/console.py pwm             # sample TRS 2's PWM duty 50x over 100 ms (synth activity proxy)
tools/console.py reset           # reboot the launcher (like a power cycle)
tools/console.py corrupt INDEX   # break scenes[INDEX]'s saved vars record
tools/console.py synth note N [V] | synth off | synth set NAME V | synth output WHERE
                                 # the synth without a script (a test tone)
tools/console.py noise ...       # the noise lab, below
```

**Noise lab**: TRS 2's analog audio carries a whine that changes with
the OLED's contents. `tools/console.py noise <switch>` changes one
suspect at a time, live, and logs every setting (`noise: ...`):
`oled-contrast N` (0-255, default 143), `oled-off` / `oled-on`,
`oled-clock N` (SSD1306 0xD5, default 0xF0), `oled-precharge N` (0xD9,
default 0x22), `oled-vcomh N` (0xDB, default 0x35), `leds-off` /
`leds-on`, `led-brightness N` (0-255, a scale on every LED level),
`usb-quiet` / `usb-loud` (hold the log back), `status`, `reset`. The
settings live in a reset-surviving RAM record (`src/noise_lab.h`) and are
sent again after every display init, so they last through app launches
and resets until a power cycle or `noise reset`. To listen, build with
`-DE16_TRS_AUDIO=ON`, route the synth to TRS 2 (`tools/console.py synth output trs2`, or
`audio.output("trs2")` in a script) and hold a tone
(`tools/console.py synth set decay 127`, `synth note 45 90`).

The SysEx protocol is documented in `src/console.h`. `E16_DEV_CONSOLE`
gates all of the above (and nothing else): a release build
(`-DE16_DEV_CONSOLE=OFF`) drops them, but keeps the plain log, log replay
and reboot-to-bootloader messages.

### Tests

```sh
cmake -S . -B build-host -DE16_HOST=ON && cmake --build build-host
cmake -S . -B build-host-double -DE16_HOST=ON -DE16_HOST_DOUBLE_TIME=ON \
    && cmake --build build-host-double       # optional, see below
test/run_all.sh
```

`test/run_all.sh` builds nothing except, if `arm-none-eabi-gcc` is on
`PATH`, the native apps in `apps/` (so `loader_test` can check real
cross-built images; without the toolchain that part is skipped with a
note). It runs the runtime specs (`out`, `ctl`, `clock`, `var_store`,
`harness`, `draw`, `ring`, `audio`, `midiin`, `demos`, `reader` — `test/rt/*_spec.lua`
through `e16rt-host`, each ending in `ALL PASSED`), the plain-C host
tests under `test/disk`, `test/synth`, `test/webcam`, `test/drone`, `test/x0x` and `test/apps` (`blockdev_test`,
`persist_test`, `flash_budget_test`, `fatfs_test`, `scene_parse_test`,
`scenes_test`, `voice_test`, `app_stream_test`, `drone_test`, `x0x_test`, `wc_render_test`, `wc_ring_test`, `loader_test`, `app_boot_test`), and `test/run_suites.sh`
(oxi-e16-lua's six script suites, unmodified, against the native
runtime) — all on `build-host`, and again on `build-host-double` if it
exists. It exits nonzero on any failure. The default host build uses
float runtime times so the specs can match the float mock bit for bit;
the double build uses the device's double times (`docs/compat-notes.md`).
`HOST=path/to/e16rt-host test/run_suites.sh` runs the suites on any build.

**Host harness** (`test/host`): `e16rt-host` is Lua with an `rt` module
(`rt_module.c`) that drives the runtime against a recording port
(`port_host.c`). `test/host/e16mock.lua` is a shim that presents the
runtime through the oxi-e16-lua mock's interface, so the suites run
unmodified; `docs/compat-notes.md` lists what the shim has to bridge.

## How it works

**Layers**:

- `src/rt` — the portable runtime: the Lua state, the stock API (midi,
  leds, slots, page, controller, var, store, system, clock), the new
  launcher APIs (draw, ring, audio, launcher), and callback dispatch. It
  only talks to the outside through `rt_port.h`.
- `src/port_e16*.c` — `rt_port.h` on the E16 SDK: Lua heap (TLSF over CCM
  + SRAM), LED rings (value, per-LED, or custom), MIDI out, labels and
  title, the `draw.*` canvas (`port_e16_canvas.c`), the synth on USB
  audio and TRS 2 (`port_e16_audio.c`, over the voice in `src/synth`; the
  USB ring in `port_e16_usbaudio.c`).
- `src/disk` — the block device over SPI flash (`blockdev.c`), FatFs
  glue (`diskio.c`, `volume.c`), the flash erase-rate budget
  (`flash_budget.c`), the name-keyed persistence slots (`persist_slots.c`),
  and the disk-script header parser (`scene_parse.c`).
- `src/apps` — the native-app loader and ABI (`loader.c`, `api.c`,
  clean-boot `app_mode.c`).
- `src/ui` — the OLED (`screen.c`, `font.c`) and the launcher state
  machine (`launcher.c`: menu, Shift gestures, page picker, disk mode,
  webcam mode, load/save).
- `src/webcam` — Webcam mode (`webcam.c`) and its video synth
  (`wc_render.c`, portable, host-tested).
- `src/persist.c`, `src/disk_mode.c`, `src/console.c`, `src/log.c` —
  persistence binding, the FAT volume as a USB drive, the USB SysEx
  console, the log ring buffer.

**Boot and main loop** (`src/main.c`): if a native app was requested (a
record left by the menu, with its launch token, right after the
launcher's own reset), boot straight into app mode instead of the normal
launcher. Otherwise: bring up the display, MIDI, the stack guard,
the port (Lua heap, LEDs, encoders), the flash erase budget, persistence,
report the last crash (if any) and the last app run (if any), mount or
format the disk, scan the menu (built-ins plus `/scripts` plus `/apps`),
and show the menu. Each pass of the main loop reads MIDI in (clock bytes
to the runtime, console SysEx to `console.c`, anything else to the
runtime's MIDI-in queue), then calls
`launcher_poll()` (input; starts and stops scripts, routes encoder events
to the running one, and — while in disk mode — services the one pending
host read/write request), `rt_tick()` (`system.update`, clock pulses,
up to 16 queued MIDI-in callbacks, holds), `launcher_persist_poll()` (the debounced vars save), reads MIDI
again, services the synth's TRS 2 switching, the LEDs, the screen and the
log, and polls the flash budget for a once-a-minute deferral summary. The synth voice itself (`audio.*`,
MIDI out port 4) renders in the TIM6 interrupt at 48 kHz into the USB
audio ring and/or PWM on TRS 2 (`src/port_e16_audio.h`); Lua never runs
there.

**Menu → script**: pressing encoder 1 on a menu entry restores that
script's saved vars and store, then `rt_load()` (built-in) or
`rt_load_reader()` (a disk script, streamed through FatFs so the source
is never held whole in RAM) runs the script's top level and
`page.onInit()`. A load error shows the error screen. Holding Shift 1 s
unloads the script (saving vars and store) and returns to the menu.

**Menu → native app**: selecting an `.e16app` entry checks the image,
then unloads Lua (saving state), writes a reset-surviving record, and
resets into app mode (see "Native apps" above).

**Scripts** are compiled in. `scripts/manifest.txt` lists them in menu
order, from `external/oxi-e16-lua` (plus the launcher's own demos in
`scripts/demos`: canvas, rings, acid, midimon), with title, page names, pad pages
and settings page; `tools/embed_scripts.py` turns that and each script's
`--@assign`/`--@scene` lines into `scripts_gen.c` (`scenes[]`, `sources[]`)
at build time. `src/scenes.c` is the menu's only view of what's runnable:
these built-ins, plus whatever `/scripts` and `/apps` hold on the disk,
rescanned at boot and after disk mode.

**Memory**: SRAM (128 KB) and CCM (64 KB, CPU-only — no DMA, no code) are
split as `src/mem_layout.h` defines, checked after every build by
`tools/check_mem_layout.py`. In normal mode, CCM holds the launcher's
non-DMA static buffers (log ring, screen framebuffer, disk block cache,
persistence shadows, the scene parser's arena, ...) plus the Lua heap's
CCM pool; SRAM holds the resident drivers' statics, the Lua heap's SRAM
pool, the newlib heap, a stack canary, and the stack. In app mode the
same two regions become, respectively, the app's `.data`/`.bss` (all of
CCM below a small fixed region of reset-surviving boot records at its
top) and the app's code plus a smaller newlib heap/canary/stack (SRAM).
`tools/check_mem_layout.py` fails the build if any CCM symbol reaches the
boot records, or if the normal-mode stack above the Lua pool and the
newlib heap would be smaller than its floor.

**Errors**: a callback that fails logs its first error in full, then only
a count every 10 s (`rt: <callback> failed N more times`); the title bar
shows a `!` while the running script has had errors. A failing
`system.update` stops further updates, like the stock firmware.

**Where to look**:

| for | see |
|---|---|
| a Lua API function | `src/rt/rt_api_*.c` |
| callback dispatch, load/unload, error handling | `src/rt/rt.c`, `rt.h` |
| MIDI in for scripts (midi.listen, the queue) | `src/rt/rt_api_midiin.h`, `.c`; `poll_midi_in()` in `src/main.c` |
| what the device does with LEDs, MIDI, labels, the canvas, the synth | `src/port_e16*.c` |
| USB audio (the ring, `fill()`, packet statistics) | `src/port_e16_usbaudio.h`, `.c` |
| when a "run this app" record is honoured | `src/apps/app_boot.h` |
| the noise lab | `src/noise_lab.h` |
| menu, gestures, disk mode entry/exit, when state is saved | `src/ui/launcher.c` |
| screens | `src/ui/screen.c` |
| the scene provider (built-ins, disk scripts, apps) | `src/scenes.h`, `scenes.c` |
| persistence binding to the runtime | `src/persist.h`, `persist.c` |
| persistence flash layout, slot hashing, append-only record format | `src/disk/persist_slots.h`, `persist_slots.c` |
| the flash erase-rate budget (both persistence and the block device) | `src/disk/flash_budget.h`, `flash_budget.c` |
| the FAT volume and disk mode | `src/disk/volume.h`, `src/disk_mode.h` |
| webcam mode (USB switch, rendering ahead, MIDI) and its video synth | `src/webcam/webcam.h`, `src/webcam/wc_render.h` |
| the disk-script header parser | `src/disk/scene_parse.h`, `scene_parse.c` |
| native app format, loader, ABI | `src/apps/e16_app_format.h`, `src/apps/loader.c`, `src/apps/e16_api.h` |
| app audio (the render interrupt, the guards; the double buffering) | `src/port_e16_audio.c`, `src/synth/app_stream.h` |
| clean-boot app mode | `src/apps/app_mode.h`, `app_mode.c` |
| the native-app SDK (headers, linker script, CMake helper) | `apps/sdk/` |
| the internal-flash build check | `tools/check_no_flash_writes.sh` |
| the RAM layout build check | `src/mem_layout.h`, `tools/check_mem_layout.py` |
| SysEx console protocol | `src/console.h` |
| script list | `scripts/manifest.txt` |

## USB name

The launcher shows up as "E16 Launcher". macOS caches MIDI port names per
device, so a Mac that has seen the E16 under another name ("E16 Custom",
"OXI E16") keeps showing that name until the device is removed in Audio MIDI
Setup (Window > Show MIDI Studio, select the E16, delete) and plugged in again.
The tools find the port either way.

## License

MIT (`LICENSE`), except `apps/x0x/` (x0x-lite), which is GPL-3.0-only
because it's ported from GPL code (`apps/x0x/LICENSING.md`).
Third-party components keep their own licenses, listed in `NOTICE`; among
them `tb3po.lua` in `external/oxi-e16-lua` is GPL-3.0, and it's built into
the firmware image. The E16 SDK (`external/E16-SDK`) is OXI Instruments'
own, under its own MIT license.

Written with Claude Code under the author's direction, and tested on E16
hardware.
