# Writing a native app

A native app is a small, freestanding C program that the launcher loads
straight into RAM and runs, with no Lua involved. This is a walk-through
plus a reference; the authoritative details are the headers it points at.

## Hello, app

The smallest real app, `apps/demos/hello/hello.c`, deliberately exercises
every kind of address the loader has to get right (a global array, an
initialized global, a const table, const string pointers, initialized
function pointers, and a function pointer picked at run time) before
logging a line and exiting:

```c
#include "e16_app.h"

static int counts[8];
int seed = 7;
static const unsigned char squares[8] = { 0, 1, 4, 9, 16, 25, 36, 49 };
static const char *const words[] = { "zero", "one", "two", "three" };

int app_main(const e16_api_t *api)
{
    char line[64];
    /* ... build a message using counts/seed/squares/words ... */
    api->log(line);
    api->exit(0);
}
```

Every app `#include "e16_app.h"` and defines exactly one entry point,
`int app_main(const e16_api_t *api)`. There's no `main()`, no startup code
to write — `apps/sdk/crt0.c` is the entry trampoline the loader jumps to,
and it's already shared by every app.

Build and copy it to the device:

```sh
cmake -S apps -B build-apps
cmake --build build-apps
```

This produces `build-apps/hello.e16app` (plus `.elf` and `.map`). Enter
disk mode on the device, copy `hello.e16app` to `/apps/`, leave disk mode,
and it shows up in the menu marked "app". Selecting it unloads whatever
Lua script was running, resets into a clean boot, runs `hello`, and
`api->exit(0)` takes you straight back to the launcher's menu.

## The API table

`app_main`'s only argument is the whole hardware surface an app can touch.
There's no other way in — no direct register access, no libc syscalls,
nothing. The table (`src/apps/e16_api.h`, mirrored for apps as
`apps/sdk/e16_app.h`'s include of the same header):

| Group | Functions | Notes |
|---|---|---|
| Display | `display_fb()`, `display_present()` | `display_fb()` returns the 128×64, 1-bit SSD1306-layout framebuffer (`E16_FB_SIZE` = 1024 bytes) to draw into directly; `display_present()` sends it (~0.5 ms, blocks). |
| LEDs | `leds_set(ring, led, r, g, b)`, `leds_show()` | 16 rings × 16 LEDs (`E16_LED_RINGS`, `E16_LEDS_PER_RING`). `leds_show()` sends in the background and returns 0 if the previous frame was still going out (call again later; nothing is lost). Keep levels modest — the stock firmware never drives a channel above ~85. |
| Encoders | `enc_read(index)`, `enc_pressed(index)`, `shift_pressed()` | Steps since the last call (clockwise positive), push state, Shift. |
| MIDI | `midi_write(port, bytes, len)`, `midi_read(msg)` | Ports are `E16_MIDI_TRS_1/TRS_2/TRS_IN/USB`. `midi_write` is all-or-nothing (0 if there's no room); `midi_read` returns the next complete message from any input. |
| Time | `millis()`, `delay_ms(ms)` | |
| Files | `file_open/read/seek/close`, `file_write(name, buf, len)` | Read-only access to any path on the disk; writes only to `/apps/<app name>/<name>` (a plain file name, no `/` or `..`), at most `E16_FILE_WRITE_MAX` (64 KB), in place. `file_write` returns `E16_EBUSY` (-4), having changed nothing, when the SPI flash's erase budget can't cover a write right now: about 4 rewrites of the same file a minute go through (every 15 s always works). Errors are `E16_EARGS/EIO/EFULL/EBUSY` in `e16_api.h`. |
| Log | `log(msg)` | One line to the launcher's log, no newline needed. |
| Synth | `synth_note(note, velocity)`, `synth_param(id, value)` | The launcher's own mono synth voice (the same one `audio.*` and MIDI port 4 drive from Lua). |
| Leaving | `exit(code)` | Resets back into the launcher; doesn't return. Returning from `app_main()` does the same thing. |
| Audio stream | `audio_start(render, user)`, `audio_stop()`, `audio_info(info, size)` | The app's own sound: a callback fills 32-frame mono float blocks at 48 kHz, in an interrupt. Added after the first ABI 1 table: check `E16_API_HAS(api, audio_start)`. See "Audio" below. |

The table starts with `abi_version` and `size`. New functions are only
ever appended at the end, so an app built against an older header still
works against a newer firmware, and an app that wants to use something
added after the version it was built against should check first:

```c
if (E16_API_HAS(api, synth_param)) {
    api->synth_param(VOICE_P_CUTOFF, 90);
}
```

## Audio

An app makes its own sound by handing the launcher a render callback:

```c
static float phase;

static void render(float *out, uint32_t frames, void *user)
{
    for (uint32_t i = 0; i < frames; i++) {   /* frames == E16_AUDIO_BLOCK (32) */
        phase += 220.0f / E16_AUDIO_RATE;
        if (phase >= 1.0f) phase -= 1.0f;
        out[i] = (phase - 0.5f) * 0.2f;       /* a quiet saw, within [-1, 1] */
    }
}

/* in app_main(): */
if (E16_API_HAS(api, audio_start))
    api->audio_start(render, NULL);
```

- **Where it goes**: wherever the built-in synth would play: the USB
  audio input while a computer is connected. TRS 2 audio is off in the
  default build (README "USB audio"), so with no computer it plays
  nowhere. Mono, 48 kHz. If the app also
  plays the synth voice (`synth_note`), the two are summed.
- **Level**: the samples are clamped to [-1, 1] and otherwise passed
  through untouched (no soft clip, unlike the synth voice's): keep the
  output inside ±1 yourself, ideally with a soft limiter. A NaN plays as
  silence.
- **Timing**: the callback runs every 32 frames (0.67 ms), a block ahead
  of what's playing: it has one block's time to fill the next. The
  stream starts with a block of silence; the output lags the callback by
  about a block plus the USB ring's 2 ms.
- **Interrupt context**: the callback runs in an interrupt below every
  driver's (USB, MIDI, LEDs, encoders) and above the app's own code: it
  can preempt the app anywhere, inside api calls too. In it, call
  `millis()` and nothing else. It runs on the app's stack (keep locals
  small). The launcher calls it with r9 set to the app's GOT, so globals
  work as usual.
- **Sharing data with the main flow**: aligned 32-bit words (ints,
  floats, pointers) are read and written whole, so a parameter the main
  flow sets is seen by the next block. Anything bigger needs a handoff
  (a sequence number, a flag the callback acts on), or `audio_stop()` /
  `audio_start()` around the change: `audio_stop()` returns with the
  callback not running and never called again.
- **Budget**: the callback may use about 80% of the CPU on average
  (`E16_AUDIO_BUDGET`); the audio interrupt, USB and the app's own code
  need the rest. The launcher stops the stream when, three seconds in a
  row, the callback's own load is over 80% or more than 5% of the blocks
  are late (the callback plus the interrupts above it, e.g. the synth
  voice, don't fit in real time even though its own share is under
  budget), or at once when its calls have taken a whole second of wall
  time (nothing else ran at all meanwhile): `audio: app callback over
  budget ...`, `audio: app blocks late ...` or `audio: app callback took
  all the CPU for a second ...` in the log,
  `audio_info()` says `E16_AUDIO_OVERLOAD`, and `audio_start()` again
  restarts it. A block the callback hasn't finished in time plays as far
  as it got (a glitch) and counts in `audio_info().late`. A call that
  hasn't returned after 500 ms ends the app (the launcher logs `back from
  app <name> (audio callback stuck)`).
- **Load**: `audio_info()` reports the callback's average load over the
  last full second and its slowest call (0.1% units), late blocks and
  calls; the launcher logs `audio: app load ...` every 10 s.
  `apps/drone` (an evolving drone synth, `docs/drone.md`) is a full
  example, `apps/x0x` (a groovebox, `docs/x0x.md`) a heavier one with
  its own CPU guard on top of the launcher's; `apps/tests/audiostress`
  exercises the guards.

## Building and running

- `apps/CMakeLists.txt` is a standalone project (its own
  `cmake -S apps -B build-apps`), cross-compiled with the E16 SDK's ARM
  toolchain file. It lists each app with `e16_add_app(name sources...)`.
- `e16_add_app` (`apps/sdk/cmake/e16app.cmake`) compiles the sources
  position-independent (`-fpic -msingle-pic-base -mpic-register=r9
  -mno-pic-data-is-text-relative`), links against `apps/sdk/app.ld` and
  `crt0.c`, and runs `tools/mkapp.py` on the resulting ELF to produce
  `build-apps/<name>.e16app`.
- Copy the `.e16app` file to `/apps/` on the device's disk (disk mode).
  It appears in the menu, marked as an app, the next time the menu is
  built (at boot, or after leaving disk mode).
- Selecting it unloads any running script (saving its state first),
  resets into a clean boot that brings up only the drivers and the disk,
  loads the app's code into SRAM and its data into CCM, and jumps to
  `app_main`.
- Leaving — `api->exit()`, returning from `app_main`, holding Shift for
  1 s, or a fault — always resets back into the normal launcher menu.

## Limits

- **Freestanding C only**: no libc (no `malloc`, no `printf`, no stdio).
  `libgcc`'s helpers (64-bit division, double arithmetic) are linked in
  automatically; if the compiler emits a call to `memcpy`/`memset` for a
  loop, the app has to supply its own (the build passes
  `-fno-tree-loop-distribute-patterns` specifically so the compiler
  doesn't do this behind your back).
- **No C++ global constructors, no `.init_array`.** `mkapp.py` refuses an
  image that has one.
- **Capacity**: roughly 88 KB of code (everything that isn't the
  launcher's own resident drivers, the app's buffers, or its stack, all
  carved from the device's 128 KB of SRAM) and roughly 64 KB of data
  (all of the 64 KB CCM region, minus a small fixed block of
  reset-surviving records at its top). The device logs the exact figures
  for a given app at load time (`app: <name> text N data M bss K`), and
  `tools/check_mem_layout.py` checks the launcher's own build against
  the same layout. Anything bigger belongs in its own firmware installed
  through OXI's bootloader, not as an app.
- **Code speed**: the text runs from SRAM1's alias at 0x00000000 (app
  mode remaps it), so the CPU fetches it over its I-code/D-code buses;
  an app whose text would reach past the first 112 KB of SRAM runs from
  its plain address, over the system bus, about a quarter slower. The
  stack is still in SRAM: keep hot loops' state in registers or in
  `.bss` (CCM), not in big locals.
- **The app runs on the launcher's own main stack** — there is no stack
  of its own, so keep large buffers in globals, not locals.
- **No internal-flash writes, ever.** The API table has no function that
  could do it, and `tools/check_no_flash_writes.sh` still gates the
  overall firmware build (it checks the launcher binary, which an app
  can't add code to anyway, since it never gets compiled in). Disk
  writes are limited to the app's own folder.
- **Relocations the loader can't redo are refused at build time**, by
  `tools/mkapp.py`: an absolute address baked into code (build everything
  `-fpic`), a PC-relative reference from code to data or vice versa (text
  and data load at independent, unrelated addresses), an undefined weak
  symbol (its GOT entry would read as a real, if wrong, address after
  rebasing — there's no way to tell "absent" from "present but zero"),
  and any section the linker script doesn't expect.

## Why these rules exist

An app's code and data move to different, freshly-chosen addresses every
time it loads (SRAM/CCM placement isn't fixed ahead of time the way a
normal ELF's is), so the loader has to be able to find and fix up every
address the linker baked in. Position-independent code with a GOT reached
through a fixed register (r9) makes that tractable: the loader only has
to know two things — the GOT's entries, and a short fixup table for
initialized data pointers — and it can rebase both by checking which of
two address ranges (text, data) a given value falls in. `mkapp.py` proves,
at build time, that nothing in the image needs more than that; the
device-side loader then doesn't have to handle anything it wasn't already
checked for. The own-folder-only rule and the API's lack of any flash
function follow the same firmware-wide promise the launcher itself keeps
(README's "Safety model"): nothing but OXI's bootloader ever writes the
E16's internal flash, and nothing an app does through the API should be
able to interfere with another app's or the launcher's own files on disk.

## Trust

A native app is trusted, privileged code. It runs directly on the CPU
with the firmware's own privileges — there is no sandbox. The API gives it
no way to write the internal flash, and the launcher and the app runtime
never do, but an app could in principle poke the hardware directly. App
mode adds a cheap guard against accidents: before jumping into the app it
turns on the MPU (keeping the default memory map for everything else)
with the internal-flash controller's registers and the internal flash
itself read-only, so a stray write there takes a MemManage fault, and the
app ends back in the launcher with the fault logged (`launcher: last
reset was a fault at PC=...`, `back from app <name> (fault)`).
`apps/tests/flashpoke` is the test for it; `apps/tests/nullpoke` writes
through a NULL pointer, which the MPU's no-access region at 0 stops the
same way. The limit: the app is
privileged, so a deliberately malicious one could turn the MPU off first.
Treat installing an app like installing a firmware.
