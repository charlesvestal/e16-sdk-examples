# Development

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

## Tests

The host build is a native Lua interpreter (`e16rt-host`), built with
`LUA_32BITS` so Lua's numbers match the firmware's 32-bit integers and
floats:

```sh
cmake -S . -B build-host -DE16_HOST=ON
cmake --build build-host
build-host/e16rt-host -e 'print(math.type(1), 2^24+1 == 2^24)'
```

To run everything:

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
