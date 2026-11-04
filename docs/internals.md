# How it works

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
`page.onInit()`. A load error shows the error screen. Leaving (Shift
held 5 s, then encoder 1) unloads the script (saving vars and store) and returns to the menu.

**Menu → native app**: selecting an `.e16app` entry checks the image,
then unloads Lua (saving state), writes a reset-surviving record, and
resets into app mode (see [app-runtime.md](app-runtime.md)).

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
