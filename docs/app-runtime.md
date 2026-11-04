# Native apps: how they run

How the launcher loads and runs `.e16app` images, and what it guards against. To write one, see [apps.md](apps.md).

A native app is a compiled C program, `/apps/<name>.e16app`, that the
launcher loads straight into RAM and runs — no Lua, no interpreter.
Selecting one in the menu unloads Lua (saving the current script's state),
resets into a clean boot that brings up only the drivers and the disk, and
jumps into the app. Every way out — returning from `app_main()`,
`api->exit()`, the leave prompt (Shift held alone 5 s, then encoder 1), a fault, or a smashed stack canary —
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
