# Compatibility notes: oxi-e16-lua suites on the native runtime

`test/run_suites.sh` runs oxi-e16-lua's five suites (euclid, lfo, chords,
modseq, tb3po), unmodified, against the native runtime (src/rt) through
`test/host/e16mock.lua`. This file lists every difference found while
getting them to pass, and where it was fixed.

## Runtime behavior changes (src/rt)

None. The suites passed without changing how the Lua API behaves. The only
src/rt additions are read-only introspection and harness accessors, each
covered by `test/rt/harness_spec.lua`:

- `rt_api_clock_listening/res/source/internal_bpm` and `rt_clock_error_count`
  (the mock's `M.listening`, `M.res`, `M.tp`, `M.bpm`, `M.cerrs`), and
  `rt_api_ctl_hold_ms` (`M.hold`).
- `rt_api_var_at/put/remove/clear/bind_scene`: direct access to the 32 var
  slots, so the shim can present them as the mock's `M.store`.

## Harness differences (test/host, not the runtime)

- all: the suites record minutes of output between clears, more than the
  host port's 256-message log held → the host port's log grows as needed.
- all: the suites call the script's callbacks and read `store.data`
  directly, and swap API functions for stubs the script then calls
  (e.g. `midi.sendCC = function() calls = calls + 1 end`). In the mock, the
  test and the script share one lua_State, but here they don't → the shim's
  `midi`, `leds`, `slots`, `page`, `controller`, `var`, `system`, `clock`
  and `store` globals are proxies for the script's, with values copied
  across and functions bridged both ways (`rt.get`/`rt.set`).
- all: the mock has one var slot, whatever script loads next, and the
  suites seed it before loading (tb3po's seed, the "foreign variables"
  checks). The runtime keeps vars per scene name → the shim binds the
  slots to the scene it's about to load (`rt.var_bind`).
- euclid, lfo, modseq, tb3po ("no per-tick garbage"): about 0.8 KB showed
  up once after `collectgarbage()`, not 10 s × per tick. It was the same
  over 100 ms, 1 s and 10 s, so it doesn't grow with time. Lua's GC shrinks
  an idle thread's stack and CallInfo list, and the script's state is idle
  when the test collects. The next callback grows them back. In the mock
  the test's own frames keep the shared stack in use, so it's never
  shrunk → after collecting, the shim's `collectgarbage` runs one call
  chain deeper than any callback before the suite takes its baseline. The
  check still catches real garbage: one table per update measures about
  55 KB over 10 s.
- euclid ("no drawing on another page"): the suite pushes a page-1 button
  while page 3 is shown. No device can produce that event, and the
  runtime's wiring rightly drops it. Switching the runtime to page 1 for
  the push makes the script draw, so the check fails → events the current
  page can't produce go straight to the script's callback with the
  mock's fields, as the mock delivers them. This path runs twice in all
  five suites. Every other event goes through `rt.turn`/`rt.press`/
  `rt.release`.
- chords: `E.release(id, held)` gives the push length without time
  passing, but the runtime measures it from the press → the shim advances
  time if the stated length is longer than what has elapsed. In the
  suites, the two lengths are always equal.
- chords: the suite reads the voiced chord sets from
  `build/chordgen/sets` → `run_suites.sh` builds them with oxi-e16-lua's
  `tools/chordgen/build.py --no-reference`, which runs offline.

## MIDI input (API 1.3.0)

OXI's API 1.3.0 adds MIDI note/CC input: `midi.listen{notes=, cc=,
channel=, port=}`, then script-defined `midi.onNote(port, ch, note, vel)`
(Note Off as velocity 0) and `midi.onCC`; an error in them stops
listening (`external/oxi-e16-lua/docs/e16-lua-notes.md`, from OXI's guide,
not checked on a 1.3.0 device). The mock's `midi.listen` is a no-op and it
never delivers input, so the suites don't exercise any of it, and before
this the runtime had no `midi.listen` at all (a 1.3.0 script calling it
failed). The runtime now implements it as described in README.md
("Launcher APIs", MIDI in), covered by `test/rt/midiin_spec.lua`. What the
notes leave open, and this runtime chose:

- port numbers: 1 = USB, 2 = TRS in (USB matches `midi.sendMidi`'s out 1);
- channels 0–15 (as `midi.sendCC`), for `channel=` and the callbacks;
- `midi.listen{channel = 1}` with none of `notes`/`cc` given listens to
  both; `midi.listen(true)`, `midi.listen()` and `midi.listen(false)`
  are accepted;
- a Note Off's release velocity is dropped (it arrives as velocity 0).

If OXI's guide or a 1.3.0 device says otherwise, these are the places to
change (`src/rt/rt_api_midiin.c`). `other=` and `midi.onMessage` are this
launcher's own extension (`launcher.has("midiin")`).

## Float and double runtime times

The runtime keeps absolute times in `rt_time_t` (`src/rt/rt_port.h`): double
on the device, float in the default host build (`RT_TIME_FLOAT`) so
`test/rt/clock_spec.lua` can compare update and pulse times with the float
mock bit for bit. A second host build, `-DE16_HOST_DOUBLE_TIME=ON` (in
`build-host-double`), uses the device's double; `test/run_all.sh` runs the
five specs and the five suites on both.

Everything passes on both, except one clock_spec scenario that is marked
float-only ("ties, callbacks queued by callbacks, late listen(false)"): it
sets the update rate at host time 19.9 ms, so the next update is due at
19.9 + 20 + 0.1. In float that rounds to exactly 40 ms, a tie with a pulse,
and the pulse goes first, as in the mock. In double the 19.9 ms start is the
float value 19.899999618…, so the update is due 0.4 µs before the pulse and
goes first. That's rounding, not a scheduling difference, so the double
build runs the scenario but doesn't compare it (it prints `skip … float-only`).

## Known limitations

- **Non-ASCII script names.** Disk script names are lowercased ASCII-only
  (`src/scenes.c`'s `lower()`), while FAT matches names case-insensitively
  with its own code-page folding (CP437, `src/disk/ffconf.h`). A script
  whose name has non-ASCII letters in different cases (say `Ärger.lua`
  and `ärger.lua`) can therefore be one file to FAT but two names to the
  scene list and to persistence -- one of them may then not find its
  saved vars/store, and the boot's slot reclaim (`persist_reclaim()`)
  looks the name up as FAT does. Plain ASCII names behave as documented.
  Not fixed: it needs FatFs's code-page tables in the scene code for an
  edge case the stock firmware's script names never hit.
- **draw.\* coordinates** (the launcher's own API, not in the stock
  firmware) are clamped to +-4096 before drawing, and a NaN or infinite
  coordinate is an error rather than silently drawing nothing; a shape
  with an end beyond +-4096 is drawn as if it ended there.
