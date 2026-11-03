# Hardware acceptance: the five scripts

Run on the connected E16, launcher firmware built from this branch (`phase1`)
after adding the heap log (see "Heap log" below). This run used the build from
before the USB rename, so the device enumerated as "E16 Custom"; later builds
show up as "E16 Launcher". Driven with
`tools/console.py`'s dev controls (`press`, `down`/`up`, `turn`, `shift-tap`,
`shift-hold`, `screen`) and captured MIDI with `mido.open_input('E16 Custom')`,
filtering out the console's own SysEx. `euclid`, `modseq` and `tb3po`'s saved
vars were reset first with `console.py corrupt INDEX` + `console.py reset`, so
each ran from the script's own defaults rather than leftover dev-session state
(see "Needs a human" / notes below for why that mattered for Euclid).

Note on the heap column: in that build the heap log line fired right after
the script started, not 60 s in (the start time was taken after the main-loop
pass's timestamp, so the unsigned difference wrapped; fixed since). The figures
are the heap right after `page.onInit()`. A later 60 s reading for Euclid on
the fixed build was 35628 B used (35176 B at start).

Heap total (`rt_heap_total()`) is the same 136004 bytes for every script (one
shared Lua allocator pool); heap free = total − used.

| script | date | result | heap used / free after ≥60 s | MIDI evidence | screen evidence | notes |
|---|---|---|---|---|---|---|
| Euclid | 2026-10-31 | PASS | 35176 / 100828 B | Channel 9 (MIDI ch 10), notes 36/38/42/46 streaming continuously at 120 BPM (kick ~every 500 ms, hats on the 16ths) | log: `ui: running Euclid, page 1`, `ui: page 1 (Pat)`, `ui: page 2 (Set)` | Default (pre-reset) saved vars had channel set to MIDI ch 16 from earlier dev testing, not the documented default ch 10 — a persisted-state artifact, not a firmware bug (confirmed by clearing the record and re-testing, see below) |
| LFOx16 | 2026-10-31 | PASS | 40072 / 95932 B | Channel 0 (MIDI ch 1), continuous CC 74 stream, ramping both directions (LFO 1 running by default, no push needed) | log: `ui: running LFOx16, page 1` | none |
| Chords | 2026-10-31 | PASS | 41452 / 94552 B | Pad 1 (encoder 1) push: 5 simultaneous note-ons (39, 58, 61, 65, 66) on channel 0; release sent the matching 5 note-offs | log: `ui: running Chords, page 1` | none |
| Mod Seq | 2026-10-31 | PASS | 30128 / 105876 B | Channel 0, CC 74, 16-step pattern repeating every ~2 s at the default BPM; two glide steps (toggled on steps 1 and 2 before playing) produced smooth multi-message ramps (e.g. 0→16→19→21→24→27→29→32) between step values, non-glide steps jumped in one message | log: `ui: running Mod Seq, page 1`, `ui: page 2 (Set)` | none |
| TB-3PO | 2026-10-31 | PASS | 40416 / 95588 B | Channel 0, notes + CC 65 (slide). Baseline loop: 64,71,60,67,76,57,67,59,67,60,77,74,72,59,64,71,… repeating. After pressing Generate (encoder 1): 64,65,48,65,60,65,64,72,65,79,57,60,67,65,64,48,… — a different pattern, confirming the seed change | log: `ui: running TB-3PO, page 1`; no `ERR` header ever logged | none |

All five: no `persist: bad record` beyond the expected first-load message after
`corrupt`/`reset`, no crash/bootloader re-entry, no warnings during the
firmware build. The device enumerated as "E16 Custom" throughout and was left
at the menu afterward.

## Euclid: default channel was ch 16, not ch 10

The very first Euclid run (on the scene's saved vars, inherited from earlier
dev/testing) sent notes 42/46/48 on MIDI channel 16, not the
documented default of channel 10 with notes 36/38/42/46. This is **not** a
firmware bug: `console.py corrupt 0` (Euclid is `scenes[0]`) followed by
`console.py reset` forced the scene to reload from its own script defaults
(`persist: bad record for euclid` confirms the stale record was discarded),
and the re-test above shows exactly the documented defaults (ch 10, notes
36/38/42/46). The takeaway for future hardware checks: persisted scene vars
survive a reflash, so "default behavior" checks on a dev unit need a vars
reset first, or they'll report whatever was last saved instead of the script's
actual defaults.

## Heap log

The runtime didn't expose free heap in the log before this run. Added a
one-shot log line, 60 s into a script's run, to `src/ui/launcher.c`
(`launcher_persist_poll()`), using `rt_heap_used()`/`rt_heap_total()` from
`src/rt/rt_heap.h`:

    heap used <N> / total <M>

It fires at most once per run (rearmed in `start_script()`), piggybacking on
the existing once-per-pass poll that already runs only while a script is
`UI_RUNNING`, so it costs one `uint32_t` comparison per main-loop pass and no
extra polling infrastructure.

## Needs a human (phase 1)

- **Chord sound/feel** across the 11 Chords mood pages (Cinematic, Chill
  House, Gospel Soul, …) — voicing quality, register, how musical a mood
  feels — is a listening judgment, not something MIDI capture settles.
- **LED ring colors** (Euclid's playhead/density/rotation rings, LFOx16's
  blue/white/pink row states, Chords' sounding-pad ring, TB-3PO's
  accent/slide/playhead colors) — the console's `screen` dump is a 1-bit OLED
  framebuffer and doesn't carry ring color at all; this needs eyes on the
  actual hardware.
- **OLED label legibility at a glance** — the ASCII-art screen dump makes it
  possible to confirm *that* text is drawn in roughly the right place, but
  reading exact label text (e.g. `L16`, `i4`, `f#11`) from the dump reliably
  is error-prone; a person glancing at the device can confirm this in a
  second.
- **TB-3PO's `ERR` flash behavior** on an actual timer fault wasn't
  deliberately provoked (no fault occurred during this run) — worth a human
  check if someone wants to confirm the header flash and OXI App console
  message, since forcing that specific failure wasn't part of this task.

---

# Phase 2: disk mode

Evidence below is what the phase-2 commits themselves record as having
been run on the connected E16 (not a simulation); the device enumerated
as "E16 Custom" for the earlier commits in this run and "E16 Launcher"
later, same caveat as phase 1.

| Area | Evidence | Commit |
|---|---|---|
| Disk mode mount/eject, round trip | Entered disk mode, copied a 200 KB file, ejected, re-entered, confirmed an MD5 round-trip on the copied file, deleted it, left the device enumerating normally at the menu. | `80dc242` |
| Persistence in the reserve, erase budget | 60 s of continuous tempo turning on Euclid's settings page produced 9 debounced vars saves and **0** `persist: erase` lines (well under the 2-erase ceiling expected for that run); a reset and restart loaded the saved state with no `persist: no saved state` log. | `80dc242` |
| Persistence survives a reset after moving to the reserve | Euclid's tempo survives `console.py reset`: `persist: no saved state for euclid` logs only once, on the very first load after the reserve move, not on the reload that follows the reset. | `5269ff7` |
| Internal-flash build gate | `tools/check_no_flash_writes.sh` passes on the normal build and fails (as intended) when `HAL_FLASH_Unlock()` is temporarily called, confirmed against the linked `launcher.elf`. | `80dc242` |

These rows were also run on the device. They're recorded in the agents' run reports rather than in the commit messages:

| Area | Evidence | Commit |
|---|---|---|
| First-boot format | `disk: formatted, wrote 5 scripts (2699 ms)`; later boots `disk: mounted, 5 .lua files in /scripts`. On the Mac: `DOS_FAT_16_S E16 12.0 MB`, `/Volumes/E16/scripts` lists the five scripts. | `0744efa` |
| Disk mode mount, eject, leave | 1 MB random file round-trip, md5 `e3dd69f0…` on both sides. Leaving with encoder 1 or Shift unmounts on the Mac within ~1 s; `diskutil eject` → `disk: mode off (eject)`. Write ~103 KB/s, read ~490 KB/s, longest main-loop stall 53–55 ms (one 4 KB rewrite). | `0744efa` |
| New script from the host | `hello.lua` copied in Disk mode → after eject `disk: 6 .lua files`, the menu lists Hello with the disk marker, and starting it logs `hello from disk`. | `1ce8266` |
| Disk script overriding a built-in | Euclid edited to a default tempo of 99 on the disk → `ui: starting Euclid (disk, replaces the built-in)`, header "EUC ■ 99". The disk copy deleted → `ui: starting Euclid (built-in)`, "EUC ■ 120". | `1ce8266` |
| Broken disk script | `disk: broken: broken.lua:2: --@assign id= is not a number`; the menu shows the disk marker and "!", the error screen shows the message. | `1ce8266` |
| Streaming load | TB-3PO from disk: heap used after load 41304 B vs 41312 B built-in (out of 119620): streaming costs no extra heap. | `1ce8266` |

# Phase 3: canvas, rings, the synth

| Area | Evidence | Commit |
|---|---|---|
| Synth CPU load | Measured with the DWT cycle counter while an acid line played: ~390 cycles/sample average, 773 cycles/sample at most, 8.2% of the CPU (budget 35%); the main loop kept ~24 kHz throughout. | `90d12f9` |
| TRS 2 switching | TRS 2 switches to the synth once a script touches it (`audio.enable(true)`, `audio.note()`, or any message to MIDI out port 4), logged as `audio: TRS 2 -> synth`, and switches back (`audio: TRS 2 -> MIDI`) after `audio.enable(false)` or unload, each after a 20 ms DC fade. | `90d12f9` |
| PWM activity proxy | The dev SysEx PWM probe (`F0 7D 45 12 F7`, `tools/console.py pwm`) samples TIM8's compare register 50 times over 100 ms and reports it in the `console: pwm ...` log line, as a listening-free check that TRS 2 is actually carrying an audio-rate signal. | `90d12f9` |

| Canvas demo | The screen dump shows the "Canvas" title bar, "128x54", the ball and the sine at 50 fps. Encoder 1 switches to fullscreen ("128x64", no title bar). | `68254b2`, `52b6334` |
| Rings demo | 500 frames per 10 s (50 fps), no loop stalls. | `68254b2`, `52b6334` |
| TB-3PO into the synth | With its output port set to 4 ("O4"): `pwm min 73 max 156 distinct 19 … TRS 2 synth`, 7.8% average / 13.6% peak CPU. After unload: `pwm min 0 max 0 distinct 1 … TRS 2 MIDI`. USB MIDI from a script still reached the Mac with TRS 2 on the synth. | `90d12f9` |

# Phase 4: native apps

All run on the device, with the USB-settle guard in place; the E16 stayed on USB throughout.

| Area | Evidence |
|---|---|
| hello app | `app: hello text 400 data 80 bss 32 (cap 89800/65280), 580 bytes loaded in 3.7 ms`, its log line, `app: hello exit 0`, then `launcher: back from app hello (exit 0)`. |
| plasma app | `2196 bytes loaded in 12.1 ms`; `fps 166 (plasma, speed 4, palette smooth)`, the screen dump shows the dithered plasma; after encoder turns `fps 157 (tunnel, ...)`. Shift held 1 s → `app: plasma ended (Shift held) after 28 s`, `launcher: back from app plasma (Shift held)`. |
| Corrupt image | `app: refused corrupt.e16app: checksum mismatch (corrupt file)` and an error screen, with no reset. |
| Faulting app | `launcher: last reset was a fault at PC=0x2000705a LR=0x20007051` (inside the app's code in SRAM), then `launcher: back from app fault (fault)` and the menu. No boot loop. |
| Scripts afterwards | Euclid runs: `ui: running Euclid, page 1, heap used 36292 / total 118292`. |
| Missing built-ins | `disk: wrote 3 missing built-ins` once (canvas, rings, acid); not again after a reset. |

An early run without the USB-settle guard caused three resets within about 2 s. The device stayed enumerated on USB, but macOS's MIDI service never created its port until the cable was replugged. The guard (wait for USB to be up for 1 s, give up after 3 s, before any app-mode reset) fixed it. Faults still reset at once, with no wait.

# Final review fixes

Run on the connected E16 (enumerating as "E16 Custom") with the firmware
of `b5309cc` (the tree all five commits below add up to), dev console on,
flashed with `reflash.sh build/launcher.bin`. The E16 stayed on USB
throughout and was left at the menu.

| Area | Evidence | Commit |
|---|---|---|
| Boot | `launcher: heap 118292 bytes, 13 scripts`, `launcher: stack free 30712 bytes` (build check: normal-mode stack 33080 bytes; app mode: text region 91848, data 65280). | all |
| Slot reclaim | First boot: `persist: freed slot 12 (probe)`, `persist: freed slot 34 (unreadable record)`, `persist: freed 2 slots`; not again on later boots. | `31a737c` |
| Empty state claims no slot | Acid (no vars) unloads with no `persist: saved` line; Euclid still logs `persist: saved euclid (vars, 217 bytes)`. | `31a737c` |
| Disk-mode idle write-back | Copied a 4001-byte file, `sync`, waited 1 s **without ejecting**, `console.py reset` (as if unplugged). Re-entered Disk mode: md5 `e1b616645987e0ac115060babfc358a0` on both sides, `cmp` identical. | `fa0dd95` |
| App file writes | `filewrite` (two writes 2 s apart): `previous: none`, `write 1 -> 0`, `write 2 -> 0`, `exit 0`. Next run: `previous: run 1 second.... (32 bytes)`. A `console.py reset` right after `write 1` → `back from app filewrite (console)`; the next run reads `previous: run 3 first.... (32 bytes)` -- never an empty file. | `fa0dd95`, `b5309cc` |
| Internal-flash guard | `flashpoke` logs `writing the flash controller's key register`, then `launcher: last reset was a fault at PC=0x20007066 LR=0x20007063` (the app's SRAM text), `launcher: back from app flashpoke (fault)`, the menu. No boot loop, no "no MPU" line. | `7928673` |
| Regressions | plasma: `fps 166 (plasma, speed 4, palette smooth)`, Shift held → `back from app plasma (Shift held)`. Euclid: `ui: running Euclid, page 1, heap used 36292 / total 118292`. Acid: `console: pwm min 102 max 145 distinct 26 ..., TRS 2 synth`; after Shift held `pwm min 0 max 0 distinct 1 ..., TRS 2 MIDI`. | all |
| Gates | Both pass on the dev-console ON and OFF builds; the flash gate fails with `HAL_FLASH_Unlock()` temporarily called from `main()`. | `b5309cc` |

Not run on hardware: the power-only USB exit (the drain skip needs the
E16 on a supply with no computer), and huge `draw.*` coordinates on the
device (the clipping is the shared `canvas.c` the host specs cover).

## Needs a human (phases 2-4)

- **Listening to the synth through TRS 2.** The PWM-activity proxy
  (`tools/console.py pwm`) confirms TIM8's duty cycle is moving at audio
  rate, not that it sounds right. To listen:
  1. Connect TRS 2 (the second 1/4" jack) to a mixer or audio interface's
     **line input**, left/tip — **not headphones**: the jack carries a DC
     bias the synth's fade rides on, and it's not a safe headphone level.
  2. Load the built-in **Acid** demo (`scripts/demos/acid.lua`) from the
     menu. Since phase 5 the synth plays over USB audio while a computer
     is connected, so route it to the jack too: `tools/console.py synth
     output trs2` (or `both`) while Acid runs.
  3. Use encoders 1-7 (cutoff, resonance, decay, accents, env mod, tempo,
     waveform) to hear the filter and envelope respond.
  4. Encoder 8 toggles play/stop.
  5. Hold Shift for 1 s to leave back to the menu (this also releases the
     synth and returns TRS 2 to plain MIDI).
  6. To hear it from a script sending MIDI directly instead of using
     `audio.*`: load **TB-3PO**, go to its settings page, and turn
     encoder 5 until the output reads **"O4"** (port 4, the synth).
     Encoder 6 starts/stops the sequence. Turn encoder 5 back to **"All"**
     before leaving, so the script returns to its normal default output.
- **Looking at the canvas, rings and plasma visuals.** `console.py screen`
  dumps the OLED as 1-bit ASCII art (fine for confirming the bouncing
  ball and sine wave move, not for judging smoothness), and it carries no
  ring color at all, the same limitation as phase 1's LED colors. The
  rings demo's rainbow comet and plasma's dithered effect and 256-LED
  rainbow need eyes on the actual hardware; the fps each logs/shows on
  screen can be read off the log without a human, but whether the result
  *looks* good is a human judgment.
- **Disk-mode auto-exit on Windows and Linux.** The eject heuristic
  (`disk_mode.c`'s `watch_fat_write()`) watches FAT16's clean-shutdown bit
  because macOS's manual-eject path sends no SCSI `START STOP UNIT`; it's
  untested on Windows and Linux, where the eject/unmount path may differ
  enough that the heuristic doesn't fire (encoder 1 and the Shift-hold
  gesture always work as a fallback regardless of platform, so this is
  about convenience, not correctness).

# Phase 5: USB audio, noise lab, app-boot fix

Run on the connected E16 (MIDI port "E16 Custom", audio input "E16
Launcher") with the `usb-audio` branch's firmware (the tree of `8464e71`;
most rows ran on a build that differed only in feeding USB through the
last note's release tail and in the noise lab's "settings kept" boot
line, both re-checked on `8464e71`), dev console on, flashed with
`reflash.sh build/launcher.bin`. Recordings with
`sox -q --buffer 65536 -t coreaudio "E16 Launcher" -c 1 -b 16 out.wav trim 0 N`
(not ffmpeg: its avfoundation input drops buffers on this Mac), analysed in
pure Python (no numpy). The test tone came from the dev console
(`console.py synth ...`: envmod 0, resonance 0, cutoff 36, decay 127,
volume 127, note 57 = 220 Hz, velocity 90, re-struck every second so its
envelope doesn't decay away). The E16 stayed on USB throughout and was
left at the menu.

| Area | Evidence |
|---|---|
| Enumeration | `system_profiler SPAudioDataType`: "E16 Launcher", Input Channels 1, 48000 Hz, USB, OXI Instruments. MIDI kept working throughout (every check here is driven over it). Boot: `audio: synth ready, 48000 Hz (timer 168000000 Hz / 3500)`. |
| Silence | 5 s with the synth off: peak 1 LSB (the host's dither on the SDK's silent packets), i.e. -90 dBFS. Non-harmonic bins next to the test tone: -104 to -122 dB re the fundamental (3.17, 7.78, 15.0 kHz). |
| Test tone, 3 s | 220.001 Hz (+0.000%); peak -3.7 dBFS; 0 glitch events (sine-prediction residual median 0.9, max 73); no run of zeros over 1 sample. Harmonics (a 4-pole low-passed saw with the cutoff at the fundamental, so they're the waveform, not distortion): h2 -22, h3 -38, h4 -50, h5 -59 dB. |
| Test tone, 34 s (dropouts) | 220.001 Hz; 7524 periods, none off by more than 5%; 0 glitch events; longest run of zeros 1 sample. Device: `usbaudio: 10000 ms: packets 47:0 48:10001 49:0 short:0 silent:0, underruns 0, resyncs 0` for each 10 s. |
| Release tail | After `synth off`, 1 ms RMS falls 8208 → 2595 → 593 → 120 → 21 → 0 over ~10 ms (the voice's release), then `audio: synth off`. |
| Acid, 32 s | Peak -0.65 dBFS, RMS -16.5 dBFS, 0 samples at or above 32000 (no clipping), 168 pitch changes across 256 pitched 100 ms blocks; 36% of 10 ms blocks below -60 dBFS (the gaps between staccato steps); no run of zeros over 1 ms. Device: `packets 47:14 48:9979 49:2 ... resyncs 2` in the first 10 s (the ring had filled before recording started), then `47:1 48:9999` and `48:10000`, underruns 0. |
| TB-3PO into port 4, 32 s | Output set to "04" on its settings page: `audio: synth on (USB audio)`, TRS 2 stayed MIDI. Peak -1.8 dBFS, RMS -15.0 dBFS, no clipping, 157 pitch changes, underruns 0. Set back to "All" afterwards. |
| CPU, loop, memory | `audio: load 13.3% (avg 428, max 812 cycles of 3500 per sample)`, peak 24.3%, while Acid plays; 15.9% (avg 517) with a note held; 12.5% for TB-3PO. `loop: 23700-24500 passes/s, longest pass 1.3-1.5 ms` (Acid), 23000-23400 (TB-3PO). `launcher: stack free 26424 bytes` low-water with Acid (26.9-27.0 KB at boot; build check: normal-mode stack 29336 bytes, minimum 24576), heap 118292, Acid uses 22728, TB-3PO 43044. |
| Routing | `synth output trs2` + a held note: `audio: TRS 2 -> synth`, `pwm min 59 max 166 distinct 42`; `both`: `pwm min 68 max 159 distinct 34`; `usb`: `audio: TRS 2 -> MIDI`, `pwm min 0 max 0`. |
| Disk mode with audio | Disk mode mounted `/Volumes/E16` while the audio input stayed listed; a 3 s recording during Disk mode worked; `plasma.e16app` md5 `3b4c4f5b…` on the volume and in `build-apps`; a file written, read back, deleted; `diskutil eject` → `disk: mode off (eject): read 143 KB, wrote 271 KB, 29 erases`, the menu. FIFO: RX 56 + EP0 16 + MIDI 16 + MSC 16 + audio 25 = 129 of 320 words. |
| Reflash during an app | plasma started from the menu (`app mode: /apps/plasma.e16app`, `fps 166`), then `reflash.sh` while it ran: `launcher: reset flags 0x10000000 (software)`, `launcher: back from app plasma (reset)`, the menu. The bootloader's own reset leaves SFTRSTF set and doesn't clear the flags (the first boot after flashing from the old firmware showed `0x1e000000 (power-on brown-out software pin)`, accumulated since power-on), so it's the launch token that tells a reflash from the launcher's own reset. The app launch itself (token + software reset) was honoured. |
| Noise lab | Each switch applied live and logged, e.g. `noise: oled contrast 40 clock 0x80 precharge 0xF1 vcomh 0x20 on, leds on level 64, usb normal`; `leds-off`/`leds-on`, `oled-off`/`oled-on` likewise. `usb-quiet` held the log (`console.py --once` got nothing in 3 s) until `usb-loud`. After `console.py reset`: `noise: settings kept from before the reset ...` and `noise status` showed the same values (reapplied after display init); `noise reset` restored the SDK's defaults and the screen dump was normal. |

## Needs a human (phase 5)

**Noise lab listening, about 2 minutes.** TRS 2 to a mixer or interface
**line input** (not headphones). From the launcher menu, in a terminal in
the repo, with `c() { python3 tools/console.py "$@"; }` defined (bash or zsh):

1. `c synth output trs2; c synth set decay 127; c synth set volume 100;
   c synth set cutoff 60; c synth note 45 90` — a held 110 Hz note on
   TRS 2 (re-run the `note` command every few seconds; it decays slowly).
   Listen 10 s: note the whine's pitch and level against the tone.
2. `c noise oled-off` — 10 s — `c noise oled-on`. (Panel off: if the
   whine stops, it's the OLED.)
3. `c noise oled-contrast 0`, then `255`, then `143` (default), ~5 s each.
4. `c noise oled-clock 0x00` (slowest), then `0x80`, then `0xF0` (default).
   The SDK already chose 0xF0 because slower clocks whine audibly.
5. `c noise oled-precharge 0x11`, then `0xF1`, then `0x22` (default).
6. `c noise oled-vcomh 0x00`, then `0x20`, then `0x35` (default).
7. `c noise leds-off` — 10 s — `c noise leds-on`; then
   `c noise led-brightness 32` and `255`.
8. `c noise usb-quiet` — 10 s — `c noise usb-loud` (log traffic off).
9. `c synth off; c noise reset`.

Note for each step: whine louder / quieter / pitch changed / no change.
Whatever clearly helps can become the default in a later commit, after
you confirm it. The settings last until a power cycle or `noise reset`.

**Listening to USB audio.** Not needed for correctness (the recordings
above check level, pitch, dropouts and clipping), but worth a listen:
record or monitor "E16 Launcher" in any audio app while Acid plays.

# App audio stream and the Drone app

Run on the connected E16 (MIDI port "E16 Custom", audio input "E16
Launcher") with the `feat/drone` branch's firmware (dev console on),
flashed with `reflash.sh build/launcher.bin`; `drone.e16app` and
`audiostress.e16app` copied to `/apps` in Disk mode. Recordings with
`sox --buffer 131072 -q -t coreaudio "E16 Launcher" -c 1 -b 16 out.wav
trim 0 N`, analysed in pure Python (Goertzel scans, no numpy). Encoders
turned with `console.py turn`, MIDI sent from the Mac with mido to "E16
Custom". The E16 was left at the menu.

| Area | Evidence |
|---|---|
| Boot, memory | `launcher: stack free 27016 bytes` (the same as before the stream; build check: normal-mode stack 29296 bytes); `app: drone text 12600 data 544 bss 35328 (cap 85672/65280), 13420 bytes loaded in 25.0 ms`. |
| Stream start | `app drone: drone: default settings` (first run), `audio: app stream started (48000 Hz, 32-frame blocks, budget 80%)`, `audio: app stream on (TRS 2)`, `audio: TRS 2 -> synth`, then `audio: TRS 2 -> MIDI` once USB came up (the "auto" routing; USB enumerates after the app starts). |
| CPU | Drone: `audio: app load 41.2% (last second; slowest call 46.7% of a block), 14999 calls in 10 s, 0 late since start`; 39.0-42.1% over every 10 s window logged, slowest call 43-57% of a block, 0 late blocks in 150 s. The audio interrupt itself: `audio: load 5.8% (avg 163, max 233 cycles of 3500 per sample)` (the voice idle). The screen's title shows `cpu 41%`. |
| Recording, 20 s, encoders turned (cutoff, resonance, chord, space, wave, root) | Peak -2.7 dBFS, RMS -16.7 dBFS, 0 runs of 3+ samples at full scale, no run of zeros over 1 ms, 1 s RMS between -18.5 and -14.5 dBFS. Device: `usbaudio: 10000 ms: packets 47:14 48:9979 49:2 short:0 silent:0, underruns 0, resyncs 2` (the ring filled before recording), then `48:10000 ... underruns 0, resyncs 0`. |
| Pitch | D2 fifths (default): strongest components 146.0, 110.5, 220.5, 36.5, 73.0 Hz = D3, A2, A3, the sub D1, D2. After the root went to A1 and the chord to minor 7 (5 s window): 55.3, 110.3, 65.7, 98.4, 82.4 Hz = A1, A2, C2, G2, E2. |
| Demo recording, 20 s (`drone-demo.wav`, not in the repo) | Patch set over MIDI CCs (minor 9 on D2, space, evolve), then cutoff opened, the chord changed, a MIDI note to A1 with a swell, resonance and drive up: peak -1.3 dBFS, RMS -14.0 dBFS, 0 clip runs, 0 zero runs; D2 minor 9 = 109.5, 164.1, 87.8, 73.3, 36.7, 131.2 Hz at the start; A1 major at the end = 54.9, 69.7, 110.5, 165.2, 81.9, 138.2 Hz. |
| MIDI in | From the Mac: CC 20 = 0 then note 50 velocity 120: the title read `D3 unison`; recorded 146.6 Hz (voices at 146.0/147.7: the detune) and the sub at 73.4 Hz. CCs set the macros (the demo's patch). |
| Screen | `console.py screen`: title `D2 fifths` / `cpu 41%`, the triggered scope, the voice dots, the 16 macro bars; turning shows e.g. `Detune ... 13 ct`. |
| Saved state | `app drone: drone: saved (changed)` 2 s after the first change, then every ~20 s while changing; relaunch: `app drone: drone: settings loaded` with the root, chord and detune as left. A change followed at once by a Shift hold: `drone: saved (leaving)` before `app: drone ended (Shift held)`. |
| Exit | Re-checked on the final build (`audio: app load 39.1%`, `40.0%`, 0 late; a 5 s recording without clip or zero runs). Shift held: `audio: app stream stopped after 225146 calls, 0 late`, `app: drone ended (Shift held) after 150 s`, `launcher: back from app drone (Shift held)`, the menu. |
| Built-in synth (regression) | `console.py synth` test tone (note 57, cutoff 36, envmod 0, decay 127, re-struck): `audio: synth on (USB audio)`, recorded 220.0 Hz, no clip or zero runs, `audio: synth off` after `synth off`. Load with the note held `audio: load 16.1% (avg 526, max 746 cycles of 3500 per sample)` (phase 5 measured 15.9%, avg 517: outside app mode the audio interrupt skips the stream entirely). |
| Guards (`audiostress`, after the review fixes) | ~30%: `state=running load=30.1%`, 0 late. ~90% from 6 s: `load=90.1% ... late=1010` (blocks late: the budget's ceiling is real), then `audio: app callback over budget (90.1% of the CPU > 80%) for 3 s: stopped`, `state=OVERLOAD`; the app kept running and logging. `audio_start()` again at ~30%: `load=29.9%`, 0 late. ~150%: `audio: app callback took all the CPU for a second (own load 149.1%, 1502 late): stopped` within the second. Restarted at ~30%; then ~78% (under the budget) with the app also holding a synth note (the audio interrupt at 13.2%): `load=78.7% ... late=508` a second, `audio: app blocks late (505 in the last second, own load 78.7%) for 3 s: stopped`. Voice off, restarted at ~30% (`load=32.5%`, 0 late), then a callback that never returns: `launcher: back from app audiostress (audio callback stuck)`, the menu. (An earlier build, judging only the callback's own cycles, took 3 windows of ~1.3 s at 127% with the app's loop starved throughout.) Drone re-checked on the same build: `app load 39.1%`/`39.6%`, `saved (changed)`, `saved (leaving)` on a Shift hold, back at the menu; 4 s recording without clip or zero runs. |

## Needs a human (app audio, Drone)

- **Listening**: `drone-demo.wav` (20 s from the device, at the path in
  the branch's report) and `docs/audio/drone-demo.wav` (10 s, host): is
  it pleasant, are the evolve moves smooth, any zipper noise when
  turning cutoff/resonance fast, does freeze (push 13) ring on cleanly,
  does mute (push 12) fade without a click.
- **Rings**: colours, the live white dots on Cutoff/Wave/Space moving
  with evolve, the hold/mute/freeze/open markers, brightness.
- **TRS 2**: Drone on a USB power supply (no computer) should play on
  TRS 2; not checked (the analog path's whine is a known issue, see the
  noise lab).
- **TRS MIDI in**: notes and CCs from a hardware controller on TRS in
  (only USB MIDI was sent here).

# MIDI in for scripts (midi.listen, midimon)

Run on the connected E16 (MIDI port "E16 Custom") with the
`feat/lua-midi-in` firmware (`5971c03`, dev console on), flashed with
`reflash.sh build/launcher.bin`. MIDI was sent from the Mac with python
`mido` to the E16's USB port; the log was followed with `tools/console.py`
throughout, and every check below also drove the device through the dev
console (turns, presses, Shift holds, screen dumps), which kept working
the whole time. The earlier rows of the table were also run on `55ccce2`
(the queue in static RAM) with the same results.

| Check | Evidence |
|---|---|
| Boot, memory | `launcher: stack free 26936 bytes` at boot; build check: normal-mode stack 29288 bytes (main at `2ea8a37`: 29336, so 48 bytes of new statics; the 512-byte queue lives in the script's Lua heap from its first `midi.listen`). |
| midimon, every kind | Note on C4 100, CC 7 = 127 on channel 3, pitch bend +4096, program change 5 on channel 5, channel pressure 99, note off: logged as `midimon: U1 on C4 100`, `U3 cc 7 = 127`, `U1 bend 4096`, `U5 prog 5`, `U1 press 99`, `U1 off C4`, in that order; the screen dump shows `U 6  T 0 … all` and the last five as text. |
| Channel filter | Encoder 1 one step (midimon's `midi.listen{channel = 0}`), then a note on channel 1 and one on channel 2: only `U1 on E4 100` / `U1 off E4` arrived; the screen showed `ch1`. |
| Console SysEx not delivered | midimon's count stayed at the number of channel messages sent while the console sent dozens of SysEx commands (screen dumps, turns, presses). |
| Burst of 500 | 500 CCs sent in 5.7 ms: `midi in: 49 messages dropped (queue full), 49 since load`, then `midi in: 243 messages dropped (queue full), 292 since load` 1 s later; midimon counted 216 delivered (8 earlier + 208), so 208 + 292 = 500. No hang, the console answered right after. |
| Steady stream | ~200 CC/s for 10 s (2037 sent): all 2037 arrived (midimon 216 → 2253), no drops. |
| Loop, trivial callbacks | A disk test script counting notes/CCs in one-line callbacks: 500-message burst plus a 10 s stream, all delivered (`midicount: 2777` = 500 + 2277), `loop: 27546 passes/s, longest pass 1082 us` (idle: 1008 us). 2000 CCs paced at ~7000/s: all 2000 arrived. 2000 unpaced (21.8 ms): 576 arrived and the device logged no drops — the SDK's USB receive path is flow-controlled, so the loss is on the Mac's side (CoreMIDI), not the E16's. |
| Loop, midimon | Longest pass 6.2 ms during the burst and 15 ms during the stream (idle 1.1 ms). Not the MIDI path (see the row above): variants of midimon showed it's midimon's own redraw plus Lua's garbage collector working through the per-message strings — without `draw_screen()` 3.9 ms; with `collectgarbage("generational")` 5–6 ms; without `print` unchanged. The runtime uses Lua's default incremental GC (the stock firmware uses generational, e16-lua-notes.md); that's a runtime-wide choice left alone here. For comparison, Canvas: 1.5 ms, Rings: 4.7 ms. |
| Callback error | A disk test script whose `midi.onNote` errors on note 13, sent notes 12, 13, 14: `miderr:8: note 13` with its traceback, then `midi in: listening stopped after an error in midi.onNote`; note 13's note off and note 14 didn't arrive; the title bar showed `!`; the launcher kept running. Encoder press → `midi.listen{}` → note 15 arrived again. |
| Stock script | Euclid running while a 500 burst and a 10 s CC stream arrived: `loop: … longest pass 955 us`, nothing logged (it never calls `midi.listen`, so nothing is queued). |

The test scripts (`midicount`, the midimon variants, `miderr`) were
deleted from the disk afterwards.

## Needs a human (MIDI in)

- **TRS MIDI input.** Not checked: the E16 has no internal route from a
  TRS output back to the TRS input, so it needs a cable (TRS 1 out → TRS
  in, with the right TRS type A/B adapters, or another device's MIDI out
  into the TRS input). With midimon running, send a note: it should show
  as `T1 on …` (port 2) on the screen and in the log.
- **Acid follows notes.** Host-tested (`demos_spec`), not listened to.
  Note: this device's `/scripts/acid.lua` is a copy an earlier firmware
  wrote, which differs from the new built-in and so replaces it in the
  menu (the existing "a disk script replaces a built-in of the same name"
  rule); delete it in Disk mode to get the new Acid (the next boot writes
  a fresh copy). Then play a note into the E16 while Acid runs: the line
  moves to that root from the next step.
- **Looking at midimon's rings**: a note lights ring (note % 16) + 1 in
  amber, fading after the note off; a CC fills ring (cc % 16) + 1 in
  green like a meter.

# Webcam mode (the E16 as a USB video synth)

Run on the connected E16 (MIDI port "E16 Custom", camera and audio input
"E16 Launcher") with the `feat/webcam` firmware (`1bea139`: SDK dev
`98ef07f`, dev console on), flashed with `reflash.sh build/launcher.bin`.
Everything was driven through `tools/console.py` (turns, presses, Shift and
encoder-1 holds, screen dumps) and MIDI from the Mac with python `mido`;
the video was captured with `ffmpeg -f avfoundation -framerate 20
-video_size 160x120 -pixel_format yuyv422 -i "E16 Launcher"` into PNGs,
and the frames were counted and looked at.

| Check | Evidence |
|---|---|
| Boot, memory | `launcher: stack free 27024 bytes` at boot (build check: normal-mode stack 29384; main at `346d376`: 29264, measured 26.9 KB in earlier runs). The Lua heap's SRAM pool gave up 384 bytes for the new statics (`launcher: heap 117908 bytes`, was 118292). Entering logs `webcam: on, 160x120 at 20 fps, 23716 bytes at 0x10002df0 from the Lua heap (tables 9812 us)`: the tables, palettes and line ring come from the (otherwise idle) Lua heap, in CCM. No new stack low-water was logged during ~3 minutes of webcam mode at the busiest settings. |
| Enumeration | After selecting "Webcam" (`webcam: USB is MIDI + video now`, about a second later) `ffmpeg -f avfoundation -list_devices true` lists video device `[0] E16 Launcher`; `system_profiler SPAudioDataType` lists no E16 input. The MIDI port stays (the console's log and commands kept working throughout). |
| Frame rate, CPU (one pattern) | 10 s capture: 200 PNGs; device: `webcam: 10000 ms: 199 frames (19.9 fps), 20.9% CPU, 81 us/line (slowest 160), lines sent from the ring 23997, rendered in the USB interrupt 0`. |
| Frame rate, CPU (busiest) | Plasma + tunnel blended, kaleidoscope, warp 8, scanlines, fire palette, text overlay: 200 PNGs in 10 s; `200 frames (19.9 fps), 44.7% CPU, 173 us/line (slowest 254), lines sent from the ring 24022, rendered in the USB interrupt 0`. Before the render-ahead ring (rendering each line in the USB interrupt) the same settings gave 166 frames (16.6 fps), and before the pass-at-a-time renderer even plain plasma did (190 us/line). |
| Picture | Frames differ (consecutive PNGs all distinct; the plasma loops every 2 s at speed 4, so 10 s holds ~80 distinct frames). Looked at: plasma, tunnel, rings, bars, the mixed/kaleidoscope settings, fire and neon palettes, scanlines, the text overlay `PLASMA+TUNNEL` and `E4` (below). |
| Encoders change the picture | A 12 s capture while turning encoders 1, 5, 6, 8, 9, 10, 11, 14, 15, 16 via the console: the picture goes plasma → tunnel → rings → fire rings → quad/kaleidoscope → mixed → warped (`e16-webcam.gif`, 240 frames, 19.9 fps, 23.6% CPU). |
| OLED | Screen dump: title `WEBCAM`, `NO APP` / `LIVE 19.9`, the pattern names, the setting last changed (`HUE` `21`), and the dithered mirror on the right half. A short press of encoder 1 (`webcam: OLED full-screen mirror`): the dump shows the dithered plasma over all 128x64; another press brings the status back. |
| MIDI drives it | CC 27 = 127 set SYMMETRY to KALEID; note on E4 (64) vel 100 set HUE to 21 (pitch class 4 × 64 / 12) — OLED `HUE 21` — and, with TEXT on NOTE, the video showed `E4` in the corner. |
| Synth routing | (on `973047b`, the same routing code) In webcam mode, `synth note 57`: `audio: synth on (TRS 2)` ("auto" falls back to TRS 2 while USB has no audio input), `pwm min 113 max 137 distinct 20 … TRS 2 synth`; nothing is pushed into the USB audio ring meanwhile (`port_e16_usbaudio_available()`). |
| Leaving | Encoder 1 held 1.5 s → `webcam: off (encoder 1 hold), … USB -> MIDI + disk + audio`, menu on "Webcam"; Shift held → `webcam: off (shift hold)`. Afterwards the camera is gone from AVFoundation, the "E16 Launcher" audio input is back, and a held A3 recorded with `sox -t coreaudio "E16 Launcher"` measures RMS 3175, 220.0 Hz. Disk mode then mounts `/Volumes/E16` and ejects (`disk: mode off (eject)`), and Euclid runs (`ui: running Euclid`). |
| Robustness | Six enter/leave cycles over two firmware builds, 6-15 s apart, by both gestures: every time the camera appeared (3 s capture: 60 frames each) and the audio input came back on leaving. macOS's MIDI service followed every switch (the console's SysEx worked right after each); a console command sent during the half-second detach is lost (`console: virtual 16 auto-released after 3000 ms`), which is expected. |

Re-check after the review fixes (`bfd307a`: main merged in, the ring
bookkeeping in `wc_ring.c`, SDK `f04b644`): boot `launcher: stack free 27048
bytes`; one pattern `199 frames (19.9 fps), 23.2% CPU, … lines sent from the
ring 23997, rendered in the USB interrupt 0`; busiest settings `200 frames
(19.9 fps), 44.7% CPU, 173 us/line`; Shift hold → `webcam: off (shift hold),
409 frames sent in all; USB -> MIDI + disk + audio`, the audio input back,
a held A3 over USB audio RMS 3259 at 220.0 Hz.

Host tests: `test/webcam/wc_ring_test.c` (the render-ahead bookkeeping
at random interleavings of the USB interrupt and the renderer, see the
file) and `test/webcam/wc_render_test.c` (in `test/run_all.sh`): parameter
ranges and CC mapping, tables, every palette in video range, posterize
bands, 576 setting combinations in YUY2 range and identical to the
per-pixel `wc_sample()` path (the OLED's and LEDs'), mirror/four-fold/
diagonal symmetry, mix 0 = A only, wipe 16 = B, animation and colour
cycling move, the text overlay stays in its band, scanlines touch odd lines
only.

## Needs a human (webcam mode)

- **Looking at it in a real video app** (Photo Booth, QuickTime, OBS, a
  browser): only ffmpeg/AVFoundation was used here.
- **The LED rings** (the picture's colours on a 4x4 grid, a white dot
  per setting) and the OLED by eye: the OLED was only checked through
  screen dumps.
- **Physical gestures**: encoder 1 held 1 s and Shift held 1 s were sent by
  the console, not by hand.
- **TRS MIDI in** driving the picture (needs a cable), and the TRS 2 synth
  audio in webcam mode by ear.
- **Windows and Linux**: the run-time re-enumeration was only tried on
  macOS.

# x0x-lite, and app code from SRAM1's alias

Run on the connected E16 (MIDI port "E16 Custom", audio input "E16
Launcher") with the `feat/x0x` firmware (main merged in, `fe8dc8d`, SDK
`f04b644`, dev console on), flashed with `reflash.sh build/launcher.bin`;
`x0x.e16app` (and the bench, `x0xbench.e16app`) copied to `/apps` in Disk
mode. Driven with `tools/console.py` (turns, presses, Shift down/up around
a press for Shift + push, Shift holds, screen dumps); recordings with
`sox --buffer 131072 -q -t coreaudio "E16 Launcher" -c 1 -b 16`; MIDI from
the Mac with mido; analysis in Python with numpy (spectral-flux
autocorrelation for the tempo, YIN for the 303's pitch). The E16 was left
at the menu with `/apps/x0x/state.bin` deleted (the factory loop next
time) and the bench removed.

| Area | Evidence |
|---|---|
| Load | `app mode: text at 0x200081c8, run at 0x000081c8 (SRAM1 remapped at 0)`, `app: x0x text 52200 data 1416 bss 10720 (cap 85296/65280), 54132 bytes loaded in 69.2 ms`, `x0x: factory state` (first run) / `x0x: state loaded`. Boot: `launcher: stack free 27048 bytes` (26952 on another boot of the same build). |
| Plays at launch | `audio: app stream started`, `x0x: P1 125.0 BPM, playing, peak 664/1000 ...`; the factory loop's tempo measured from a recording: beat period 480 ms = 125.0 BPM. |
| CPU, factory loop | `audio: app load 41.1%`-`44.2%` (last second; slowest call 50-65% of a block), 0 late, over every 10 s window logged (about 6 minutes in all, with recording, edits, screen and rings). Stopped: 3.5%. |
| CPU, worst case | A state with every drum and the 303 on every step, accented, 180 BPM (`x0x_test --stress-state`): `x0x: CPU guard 2 (load 0%)` (late blocks in the first half second), `CPU guard 3 (load 58%)`, then `CPU guard 2 (load 39%)` and steady: `app load 45.9%`-`46.0%`, slowest call 68.8-69.3%, no more late blocks (13 in all, in the first second) over 25 s. Before the guard: 75% with 26 late blocks in the first second (an earlier build); the first build of all (2x 303, the struct-bound voices, no SRAM1 alias): 84% on the factory loop, stopped by the launcher (`app callback over budget (84.0% ...)`). |
| Recording 1, 20 s (factory loop; tempo +10, a kick added, cutoff and env mod up) | Peak -3.29 dBFS, RMS -16.66 dBFS, 0 runs of 3+ samples at full scale, no zero run over 2 samples, 1 s RMS -18.4 to -15.0 dBFS. Tempo before the change 125.0 BPM, after 135.1 BPM. `usbaudio: 10000 ms: packets 47:0 48:9945 49:51 short:0 silent:0, underruns 0, resyncs 1` (the ring filling as recording started), then `48:10000 ... underruns 0, resyncs 0`. |
| Recording 2, 20 s (`x0x-demo.wav`, at the path in the branch's report): cutoff swept down and up, a new TB-3PO line, swing 12, closed hats added | Peak -3.36 dBFS, RMS -16.62 dBFS, 0 clip runs, 0 zero runs over 1 ms, 1 s RMS -17.3 to -15.6 dBFS; 135.1 BPM throughout; `app load 44.2%`, slowest 63.5%, 0 late. |
| The 303's pitch | The drums muted (mute page), 8 s recorded: of 223 voiced YIN frames, 178 on the factory line's notes or an octave off (D3 C2 G3 E3 A1 F2 A2 B2 B1: `x0x_init`'s line), the rest mostly F#2 (B1's 3:2 partial, a YIN fifth error, and slide glides). Host: 109.92 / 54.96 / 219.84 Hz for held A2 / A1 / A3. |
| Screens | `console.py screen` of each page: steps (eight rows of 16 cells, accents hollow, the selected track's arrow, the two playheads), 303 sound (`Cutof Reso EnvMo Decay / Accen Wave Tune Volum / Drive DrvTy Slide AccDe / Kit 303 Drive Volum` with bars), global (`T135 Sw0 K16 B16 / D70 M0 MIN A / A40 S25 Oct2 Tr0 / CkO+ CkI+ Cp>2 Clr`), patterns (P1 inverted, a dot for patterns with notes), mute (`BD SD LT CP / CH OH CB 303` with level bars; muted ones `--`). A MIDI note D4 in: title `Transpose +2`. |
| Edits | Shift + push 10 → global, turn 1 +10: tempo 135; Shift + push 1 → BD, push 3: a kick on step 3 (seen in the dump); Shift + push 8 twice → 303 sound, turn 1: cutoff; Shift + push 13: a new line; Shift + push 5, pushes 2 4 6 8: closed hats (the dump shows them). |
| Saved state | `x0x: saved (changed)` 2 s after the edits; `saved (leaving)` on a Shift hold with unsaved edits (also on console Shift + push combinations, which hold Shift ~0.8 s: by hand they're shorter). Shift held 1.5 s: `audio: app stream stopped after 157026 calls, 0 late`, `app: x0x ended (Shift held)`, `launcher: back from app x0x (Shift held)`; relaunched: `x0x: state loaded`, title `P1 135`, global page `T135 Sw12` as left. |
| MIDI clock out | Counted on the Mac for 5 s while playing at 135 BPM: 270 clocks = 135.0 BPM. |
| MIDI clock in | Start + 480 clocks at 100 BPM + stop from the Mac (mido): `x0x: P1 100.1 BPM (ext), playing, ... clocks in 165`; recorded meanwhile: beat period 600 ms = 100.0 BPM in both halves; after the stop `x0x: P1 135.0 BPM, stopped, ... clocks in 480` (own tempo again, stopped). Shift + push 16 started it again. |
| SRAM1 alias, the bench | `x0xbench.e16app` (the same binary, before / after the launcher change): factory loop 1603 → 1190 cycles a sample, worst case 3464 → 2545 (with the launcher's audio interrupt, ~160). A float multiply-add loop: 24.8 → 21.6 cycles per 4-FMA iteration. |
| SRAM1 alias, other apps | Drone: `audio: app load 26.8%` (41% before), back from it on a Shift hold; `fault`: `launcher: last reset was a fault at PC=0x000081f2 LR=0x000081e9` (the alias address), `back from app fault (fault)`; `flashpoke`: `writing the flash controller's key register`, a fault, `back from app flashpoke (fault)` (the MPU guard still holds); `hello`: `hello from an app: zero total=708 c5=181`, `exit 0`, the menu. |

Host: `test/x0x/x0x_test.c` in `test/run_all.sh` (see `docs/x0x.md`):
`timing: 68.9 ns/sample (factory loop), 110.0 ns/sample (every drum and
the 303 on every step, 180 BPM)` on the Mac.

## Needs a human (x0x-lite)

- **Listening**: `docs/audio/x0x-demo.wav` (10 s, host) and the 20 s
  device recording (`x0x-demo.wav`, path in the report). Does the 303
  still sound like X0X's at 1x (no oversampling: listen for aliasing on
  bright, resonant notes and with the RAT), do the 808 hats sound right
  without the bank's polyBLEP, is the mix balanced (kit vs 303), any
  clicks when the CPU guard steals a voice (only with very dense
  patterns)?
- **The rings by eye**: brightness (kept at 24 of 255), the step arcs,
  the 303's pitch dots and accent / slide LEDs, the playhead background.
- **By hand**: Shift + push and Shift + turn timing (the app leaves at
  1 s of Shift, as every app), turn acceleration on the long ranges.
- **TRS MIDI** in (clock, notes) and out (clock on TRS 1): needs cables.
- **TRS 2 audio** (no computer connected) by ear.

# Overnight merge recheck (main `a134207` + `dd0120c`)

Run on the connected E16 (MIDI port "E16 Custom", audio input "E16
Launcher") after it was replugged. `build/launcher.bin` from main
`a134207` (SDK `f04b644`, dev console on; `dd0120c` only adds a test app)
flashed with `reflash.sh`; apps from `build-apps/` copied to `/apps` in
Disk mode (drone, x0x, hello, plasma, fault, flashpoke,
audiostress, and the new `nullpoke`; `cmp` matched each). Driven with
`tools/console.py`; Shift + push sent from one process (console.py's
separate `down`/`press`/`up` calls hold Shift over 1 s, which leaves the
app). Recordings with `sox --buffer 131072`, 10 s each, analysed in
Python (clip runs = 3+ samples at full scale).

| Area | Evidence |
|---|---|
| Boot | `launcher: heap 117908 bytes, 21 scripts`, `launcher: stack free 26968 bytes` (26880-27204 on the later boots; above `MIN_STACK_BYTES` 24576, below the brief's 27 KB target as on the x0x run). Menu dump: the title bar and the script list. |
| Disk | `/scripts/acid.lua` was the old Acid (the `--@scene` header plus the demo without its MIDI-in transpose, `2e62d3e`): deleted. The next boot: `disk: wrote 1 missing built-ins`, `10 scripts (0 replace built-ins, ...; 9 unchanged built-in copies)`. `corrupt.e16app` and `player`/`filewrite` left as they were. |
| App mode lines | Every app: `app mode: text at 0x200081c8, run at 0x000081c8 (SRAM1 remapped at 0)` and `app mode: MPU: flash read-only, no access below 0x00008000` (the 32 KB NULL region). |
| hello | `hello from an app: zero total=708 c5=181`, `exit 0`, `back from app hello (exit 0)`. |
| plasma | `fps 193 (plasma, speed 4, palette smooth)`, dump shows the dithered plasma; Shift held: `plasma ended (Shift held)`, the menu. |
| drone | `drone: settings loaded`, `audio: app load 27.8%` / `26.8%` (slowest call 30.6-32.0%), 0 late; `usbaudio: 10000 ms: packets 48:9941 49:53 ... underruns 0`. Recording: peak -6.95 dBFS, RMS -14.56 dBFS, 0 clip runs, longest zero run 1 sample, 1 s RMS -14.7..-14.3 dBFS. Shift held: `app stream stopped after 36534 calls, 0 late`, back at the menu. |
| x0x | `x0x: factory state`, `P1 125.0 BPM, playing`, `app load 41.5%` / `40.6%` (slowest 55% / 49%), guard 0, 0 late; `usbaudio: ... 48:9993 49:2 ... underruns 0`. Recording: peak -3.63 dBFS, RMS -16.91 dBFS, 0 clip runs, longest zero run 2 samples, 1 s RMS -17.9..-16.4 dBFS. |
| x0x transpose | Global page (Shift + push 10), encoder 12 +30: title `Transpose +24`, cell `Tr+24`; -60: `Transpose -24`, `Tr-24` (both clamp and fit the cell); +21: `Tr-3`. |
| x0x state | Tempo +5 (T130), `x0x: saved (changed)`; Shift held (`stopped after 56561 calls, 0 late`); relaunched: `x0x: state loaded`, global page `P1 130`, `T130 ... Tr-3`. `/apps/x0x/state.bin` deleted afterwards (factory loop next time). |
| fault | `last reset was a fault at PC=0x000081f2 LR=0x000081e9`, `back from app fault (fault)`. |
| flashpoke | `writing the flash controller's key register`, `fault at PC=0x000081ee`, `back from app flashpoke (fault)`. |
| NULL guard (`nullpoke`) | Address 0: `writing through a NULL pointer`, `last reset was a fault at PC=0x000081f2 LR=0x000081ef` (0x81c8 + 0x2a: the `str` in the disassembly), `back from app nullpoke (fault)`. A scratch build with `-DNULLPOKE_ADDR=0x100u`: `fault at PC=0x000081f4`, `back from app nullpk100 (fault)`. The fault log has no CFSR, so "MemManage" is inferred: without the region the store lands in SRAM1 and the app would log "no fault". Afterwards Acid ran (`running Acid`, `audio: load 13.8%`, 0 notes dropped) and a 5 s USB recording of it: peak -0.65 dBFS, RMS -16.45 dBFS, 0 clip runs. |
| audiostress (main's build) | ~30% step at 24.3%, ~90% step at **73.5%** and ~78% + voice at **61.7%** (1 late a second): neither guard fired, because the burn loop now runs from the alias (~14 cycles an iteration, was 17). ~150%: `took all the CPU for a second (own load 120.9%, 1502 late): stopped`; the hang: `back from app audiostress (audio callback stuck)`. |
| audiostress (`fix/recheck` `01f3bc4`, 81 iterations a percent) | ~30%: 31.8%. ~90%: `load=89.3%`, `app callback over budget (89.4% of the CPU > 80%) for 3 s: stopped`. ~150%: `own load 148.3%, 1502 late): stopped`. ~78% + voice: `load=78.0% ... late=506` a second, `app blocks late (504 in the last second, own load 78.0%) for 3 s: stopped`. Hang: `back from app audiostress (audio callback stuck)`. This build is the one left in `/apps`. |
| Shift hold | Ended plasma, drone and x0x (`... ended (Shift held)`, `back from app ... (Shift held)`). |
| Webcam | `webcam: on, 160x120 at 20 fps ...`, `USB is MIDI + video now`; ffmpeg lists `[0] E16 Launcher`; 5 s raw capture: 100 frames, 160x120. Shift held 1.5 s: back at the menu (the `webcam: off` line was lost from the log across the re-enumeration; the release reached the firmware as `virtual 16 auto-released`). USB audio input back: synth A3 recorded, 220.0 Hz, RMS 2496 in the first half second. |
| Lua MIDI (MIDI Mon) | mido: note on ch 3 D4 vel 99, CC 74 = 42 on ch 1. Log `midimon: U3 on D4 99`, `U1 cc 74 = 42`, `2 messages (usb 2, trs 0)`; dump: header `U 2 T 0  all`, rows `U1 cc 74 = 42`, `U3 on D4 99`. |

The E16 was left at the menu on main's `a134207` launcher.

# Mirror (`feat/mirror`)

Run on the connected E16 (MIDI port "E16 Custom") with `build/launcher.bin`
from `feat/mirror` at `33b08d2` (dev console and mirror on), flashed with
`reflash.sh`. Driven with `tools/mirror/mirror_client.py` (remote actions
over the mirror's own virtual controls), a small menu-navigation script
using the mirror's state messages, and headless Chrome for the page.
Numbers are the launcher's own `mirror: 10000 ms: ...` log lines and the
client's counts.

| Area | Evidence |
|---|---|
| Build, memory | No warnings. `check_mem_layout`: normal-mode stack 29336 bytes (as main), `-DE16_MIRROR=OFF` 29680, `-DE16_DEV_CONSOLE=OFF` 29400; flash 370892 of 496 KB (the embedded page is ~37 KB; 330220 without the mirror). Boot: `launcher: stack free 26984 bytes` (main measured 26.9-27.0 KB in earlier runs; above the 24576 minimum, just under the 27 KB target as before). |
| Page on the disk | Disk mode: `/Volumes/E16/mirror.html`, 37464 bytes, `cmp` identical to `tools/mirror/index.html`. Opened as `file:///Volumes/E16/mirror.html` in headless Chrome with MIDI granted: `isSecureContext` true, pill "Live · E16 Custom", mode "Disk mode", the OLED's Disk-mode screen drawn. |
| Menu, scrolling | 200 remote turns of encoder 1 at ~10/s: `112 frames (11.2 fps), oled 1774 msgs, rings 107 msgs, events 108 (0 dropped), 8822 bytes/s` (a frame per change: it follows the input). Idle menu: 2 frames per 10 s (the 5 s full refresh), ~500 bytes/s. |
| Acid (script) | `mode script, name Acid`; OLED static (2 full refreshes per 10 s), rings 11.5 updates/s (the step LED), ~1 KB/s. Ring levels match the screen: values 40, 96, 35, 80 show 5, 12, 4, 10 LEDs lit (value x 16 / 127), all `1c0000`. |
| Drone (app mode) | `mode app, name drone`: `303 frames (30.3 fps), oled 3998 msgs, rings 52 msgs, ... 18091 bytes/s` in each 10 s. |
| USB audio while mirroring Drone | 30 s recorded with sox during the stream (30.3 fps throughout): `usbaudio: 10000 ms: packets 47:0 48:9939 49:54 short:0 silent:0, underruns 0, resyncs 2` (the ring filling as recording started), then twice `48:10000 ... underruns 0, resyncs 0`. Recording: peak -0.83 dBFS, RMS -10.7 dBFS, longest run of zeros 2 samples, largest sample-to-sample jump 1578. |
| Webcam mode | `mode webcam`: `126 frames (12.6 fps), oled 1790 msgs, rings 3803 msgs, ... 31152 bytes/s`: all 16 rings change on every scan here, so the rings take most of the queue and the OLED gets the rest. The PNG shows the WEBCAM status screen and the dithered picture. |
| Disk mode | `mode disk`: 2 frames, 32 ring updates per 10 s (static screen), 480 bytes/s; the host's disk was mounted meanwhile. |
| Remote control, events | `--do "turn 1 3" "turn 1 -3" "press 7" "down shift" "up shift"` in Acid: events `turn enc 1 +3 (remote)`, `-3`, `down/up enc 7 (remote)`, `down/up Shift (remote)`; Shift opened the page picker (`ui: page picker open`). From the page (localhost, served by `serve.sh`): three wheel turns on knob 1 moved the menu from "Disk mode" to "Webcam", logged as three `Encoder 1 turned +1 · remote`. |
| Main-loop stall | `slowest poll` 485-1353 us across all modes (interrupts included; ~1.2 ms typical in app mode and with Acid playing). |
| Keepalive timeout | Not shown on the device: another subscriber (very likely a browser tab with the mirror page open, outside these tests) kept the stream on throughout (`mirror: on` right after every unsubscribe, stats every 10 s with no client of ours running). The 3 s timeout is covered by the host test (`test/mirror/mirror_test.c`). |
| Page states | No MIDI permission (headless Chrome, not granted): pill "MIDI access blocked" and the notice. `?demo&still` screenshots at 1000 px and 390 px (DPR 2) wide: no horizontal scroll, OLED at whole device pixels (192 CSS px at 390 px wide). |

## Needs a human (mirror)

- **Hardware inputs**: turn and push real encoders and Shift with the page
  open; they should show as "device" events (all inputs here were remote).
- **LED order**: the page draws LED 1 at 7 o'clock running clockwise to 5
  o'clock (the launcher fills value arcs from LED 1 clockwise). OXI hasn't
  confirmed the physical order; compare a ring on
  the E16 with the page, and flip it in the page's Settings if needed.
- **The keepalive timeout on the device**: close every mirror tab, run
  `tools/mirror/mirror_client.py --no-keepalive --log -t 8`, and look for
  `mirror: off (no keepalive for 3000 ms)`.
- **Safari / iOS**: the "no Web MIDI" notice by eye.
