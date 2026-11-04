# x0x-lite

`apps/x0x` is a small groovebox for the E16: an **808 kit** of seven
voices and a **303** with the **TB-3PO** line generator, on a 16-step
sequencer with 16 patterns, swing, accent and slide, MIDI clock in and
out. It's a port of the user's X0X groovebox (tbd-x0x, for the TBD-16;
it descends from X0X for the M-VAVE FM-1), cut down to fit a native app.
It plays through the app audio stream (`audio_start()`, `docs/apps.md`):
the E16's USB audio input while a computer is connected (TRS 2 audio is off in the default build; see [usb-audio.md](usb-audio.md)).
`docs/audio/x0x-demo.wav` is 10 s of it, rendered on the host by
`test/x0x/x0x_test.c --wav` (the factory loop, a cutoff sweep, then
closed hats and a new TB-3PO line).

**Licence**: unlike the rest of this repository, everything in
`apps/x0x/` is GPL-3.0-only (X0X's licence): `apps/x0x/LICENSE`, and
`apps/x0x/LICENSING.md` for where each part comes from.

Install: copy `build-apps/x0x.e16app` to `/apps/` in Disk mode, eject,
start "x0x" from the menu. It plays the factory loop at once (808 kick on
the beat, clap on 2 and 4, open hats on the off-beats, a TB-3PO acid
line). **Shift + push 16** stops and starts it. Hold Shift on its own for 5 s and
push encoder 1 at the prompt to go back to the launcher.

## Controls

The 16 encoders are the 16 steps of the selected track, or a page's 16
controls. Shift + push picks the track or page.

**Everywhere**

| Gesture | Effect |
|---|---|
| Shift + push 1-8 | select a track: 1 BD, 2 SD, 3 LT (low tom), 4 CP (clap), 5 CH, 6 OH (closed / open hat), 7 CB (cowbell), 8 303 — its step page. The same track again: its sound page (and back) |
| Shift + push 9 | the selected track's sound page |
| Shift + push 10 | the global page |
| Shift + push 11 | the pattern page |
| Shift + push 12 | the mute / level page |
| Shift + push 13 | a new TB-3PO line for the 303 (new seed) |
| Shift + push 14 | mutate the 303 line (re-rolls about a quarter of it) |
| Shift + push 16 | play / stop |
| Shift held alone 5 s, then encoder 1 | back to the launcher (the leave prompt; the state is saved on the way) |

**Step page, drum track** (encoder N = step N)

| Gesture | Effect |
|---|---|
| push | step on (normal level) / off; while stopped, you hear it |
| turn | the step's level, 1-15; 15 is the accent (red ring). Turning an off step turns it on |

**Step page, 303** (encoder N = step N)

| Gesture | Effect |
|---|---|
| push | gate on / off (the note is kept); while stopped, you hear it |
| turn | pitch, a semitone a detent, C1-C6 (an off step comes on); while stopped, you hear it |
| Shift + turn | accent / slide: clockwise steps through none → accent → slide → both, anticlockwise back |

The rings: a drum step's ring is an arc as long as its level, in the
track's colour (red when accented); the 303's ring has a dot at the
note's pitch class (C at the first LED), coloured by octave (blue, green,
lime, amber, pink from C1), with LED 14 red for an accent and 15-16 blue
for a slide. The step playing has a dim white background. The screen
shows all eight tracks' 16 steps (the selected one marked on the left, a
dash for a muted one), with the kit's and the 303's playheads under the
grid.

**Sound page** (the selected track; push any encoder: back to its default)

| Encoders | 303 | Drum |
|---|---|---|
| 1-4 | Cutoff, Reso, EnvMod, Decay | Level, Tune, Decay, and for BD Tone (4) / Attack (5), for SD Tone (4) / Snappy (5) |
| 5-8 | Accent, Wave (saw / square), Tune, Volume | |
| 9-12 | Drive, Drive type (off / soft / RAT), Slide time, Accent decay | |
| 13-16 | the mix: Kit level, 303 level, Drive (the master's, 0 to +12 dB into its soft clip), Volume | same |

The 303's ranges are X0X's (Open303 with the Devilfish slide and accent
decay ranges); the factory sound is X0X's acid: cutoff 48, resonance
100, env mod 85, accent 100, RAT drive 45.

**Global page** (the playing pattern's TB-3PO settings and the sequencer)

| Encoder | Turn | Push |
|---|---|---|
| 1 Tempo | 40-240 BPM | play / stop |
| 2 Swing | 0-100 (50 % to 75 % shuffle) | |
| 3 Kit steps | the kit part's length, 1-16 | |
| 4 303 steps | the 303 part's length, 1-16 | |
| 5 Density | TB-3PO note density, 0-100 % (the line follows) | a new line (new seed) |
| 6 Mutate bars | auto-mutate every 1, 2, 4, 8 or 16 bars, or off | mutate now |
| 7 Scale | minor, phrygian, harmonic minor, minor pentatonic, dorian, major (the line follows) | |
| 8 Root | C-B (the line follows) | |
| 9 Accent | TB-3PO accent probability (the line follows) | |
| 10 Slide | TB-3PO slide probability (the line follows) | |
| 11 Octaves | the line's range, 1-3 octaves (the line follows) | |
| 12 Transpose | the 303, ±24 semitones (MIDI notes set it too) | |
| 13 Clock out | on / off | toggle |
| 14 Clock in | on / off | toggle |
| 15 Copy to | the pattern to copy the playing one into | copy |
| 16 Clear | | twice within 2 s: clear the playing pattern |

"The line follows": the TB-3PO settings regenerate the 303 line from its
seed straight away, so turning them reshapes the line you hear (and
replaces hand edits of it); push 5 (or Shift + push 13) for a new seed.

**Pattern page**: push encoder N to play pattern N, at the end of the bar
while playing (its ring blinks until then), at once while stopped. The
playing pattern's ring is lit, with its playhead; a pattern with notes
in it has a dot.

**Mute page**: push 1-8 to mute / unmute a track (red ring); turn 1-7 for
a drum's level, 8 for the 303's.

## MIDI

- **In** (USB and TRS in, any channel): clock (24 ppqn), start, continue
  and stop drive the sequencer when Clock in is on (the default): the
  tempo follows the clock (the title shows `e`), and 0.5 s after the
  clock stops it falls back to its own tempo. A note-on transposes the
  303 by its distance from C4 (±24), live (not saved by itself).
- **Out** (USB and TRS 1): clock, start and stop while the sequencer runs
  on its own tempo and Clock out is on (the default).

## Saving

The patterns, the sound and the settings are saved to
`/apps/x0x/state.bin` (2.7 KB, with a CRC) 2 s after the last edit, at
most every 20 s (a write that gets `E16_EBUSY` from the SPI flash's erase
budget is retried after 5 s), and once when Shift has been held 0.6 s on
the way out; it's loaded at start. Auto-mutations count as edits (a line
mutating every bar is rewritten at most every 20 s, the same schedule);
MIDI transposition doesn't (it's saved with the next edit). Delete `state.bin` in Disk mode to get the factory
loop back.

## What it is, and what's left out

| Part | From X0X | Not here (why) |
|---|---|---|
| 303 | `dsp/bass303`: Open303's signal path and constants (TeeBeeFilter, envelopes, accent, slide, de-clicker), Devilfish ranges, soft and RAT drive | X0X's 2x oversampling of the filter and schwung-303's 2x in the drive (CPU, below: bright resonant notes and hard drive alias a little more) |
| 808 | `dsp/drum808` (8W8): BD, SD, LT, CP, CH, OH, CB circuit models, pot ranges, kit trims, velocity law, CH chokes OH | mid / high toms, congas, rim / claves, maracas, cymbal; per-sound drive, sends and pan (CPU and code). The metal bank's squares have no polyBLEP and their drift and jitter change the increment at each wrap / 256 samples instead of every sample (CPU) |
| Sequencer | `seq/sequencer`: sample-accurate steps, 303 gate / tie / slide semantics, swing, cue at the bar, external clock, clock out, auto-mutate | the 909 part, a second 303, the break, song mode, chains, step rates and directions, motion |
| TB-3PO | `seq/tb3po` as is (same draws: a seed gives schwung-tb3po's line) | |
| Master | | X0X's compressor, filter, reverb and tape delay. Here: mix, a peak limiter (0.4 ms attack, 150 ms release) and a soft clip (linear to 0.5, a tanh knee to 1) |

Everything runs at 48 kHz (X0X: 44.1); the 808's precomputed
coefficients were converted (`apps/x0x/core/drum808.h`).

## CPU

The audio callback measured on the E16 (the launcher's `audio: app load`,
its own cycles, the launcher's audio interrupt excluded):

| | load | slowest call |
|---|---|---|
| factory loop, 125 BPM | 41-44 % | 50-61 % of a block |
| every drum and the 303 on every step, accented, 180 BPM (the CPU guard settles at 2) | 46 % | 69 % |
| the same with the guard off (seen in its first second) | 75 % | over 100 % (late blocks) |

`apps/x0x/bench.c` (`x0xbench.e16app`, a dev aid) times each part on the
device in cycles per 48 kHz sample (of 3500), with the launcher's audio
interrupt (~160) included: 303 300 (no drive) to 441 (RAT), BD 300, SD
406, LT 343, CP 219, CH 414, OH 447, CB 387; the engine on the factory
loop 1188, on the worst case 2549. Running in the audio callback costs
about 1.35 times that.

**The CPU guard**: seven 808 voices ringing at once plus the 303 don't
fit, so the app watches the launcher's load figure (every 0.5 s) and
lowers the number of drum voices that may sound at once: level 1 above
55 % (5 voices, tails cut 50 dB down), 2 (4 voices, 40 dB), 3 (3
voices) — two levels at once above 70 % or after a late block — and
relaxes a level after 3 s under 40 %. A voice over the limit (the oldest)
fades out in 0.33 ms under the new hit. The title shows `G1`-`G3` while
it acts, and `x0x: CPU guard N` is logged. If the launcher still stops
the stream (over budget for 3 s), the app restarts it at level 3, up to
three times; then the audio stays off (`OVL` in the title) until Shift +
push 16. With
the factory loop and ordinary patterns the guard stays at 0.

Two things made the room: the 303 at 1x (above), and a launcher change:
app mode now runs app code from SRAM1's alias at 0 ([app-runtime.md](app-runtime.md)), which made x0x's engine about 25 % faster.

## Files

- `apps/x0x/core/`: the engine, portable C (no api calls, no libm):
  `bass303`, `drum808`, `seq` (the sequencer), `pattern`, `tb3po`,
  `x0x` (the engine: parameters, the main-flow → callback queue, the
  master, the CPU guard, the saved state), `x0x_save` (when to save),
  `fastmath.h`.
- `apps/x0x/app.c`: the controls, screen, rings, MIDI and files;
  `gfx.c`: the screen font and drawing.
- `apps/x0x/bench.c`: the on-device timing app.
- `test/x0x/x0x_test.c`: host tests (`test/run_all.sh`): the 303's
  pitch on held notes, the kick's spectrum and decay, every voice
  sounding and ending, sequencer timing on the sample grid (120 BPM:
  6000 samples a 16th), swing, slide, cue, transpose, mutes, stop,
  external clock (tempo and step spacing, and the fall-back), the edits,
  TB-3PO determinism, the saved state's round trip and damage checks,
  the save schedule, the CPU guard and voice cap, 120 s of random edits
  with no NaN and nothing outside ±1, and the timing.
  `--wav out.wav` writes the demo; `--stress-state out.bin` writes the
  worst-case state used for the CPU measurements above.
