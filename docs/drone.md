# Drone

`apps/drone` turns the E16 into an evolving drone synth. It's a native
app that renders its own audio through the app audio stream
(`audio_start()`, `docs/apps.md`): the sound plays on the E16's USB
audio input while a computer is connected (TRS 2 audio is off in the default build), like the
built-in synth. `docs/audio/drone-demo.wav` is 10 s of it, rendered on
the host by `test/drone/drone_test.c --wav`.

Install: copy `build-apps/drone.e16app` to `/apps/` in Disk mode, eject,
start "drone" from the menu. Hold Shift on its own for 5 s and push encoder 1 at the prompt to go
back to the launcher (settings are saved on the way).

## The sound

Six oscillators play a chord over the root. Each morphs from sine to
triangle to saw (`Wave`), sits at a fixed offset of the detune spread
(`Detune`, up to 40 cents across the voices), and drifts on its own,
slowly and at random (`Drift`, `Drift rate`). Their levels breathe. A
sine sub an octave below the root and high-passed noise (`Air`) join
them. Then:

- **Drive**: soft saturation (a rational tanh) into
- **the filter**: a resonant state-variable filter whose `Mode` morphs
  low-pass → band-pass → high-pass, with a slow LFO and a random drift
  on the cutoff (the same `Drift` depth and rate), then
- **Space**: two diffusing allpasses into a 4-line feedback delay network
  (Householder feedback, damped: a small, dark reverb, 23-58 ms lines).
  `Decay` sets the feedback; pushing encoder 13 freezes it (feedback 1,
  no damping, no new input: whatever's in it rings on until released).
- **Level**, then a peak limiter and a soft clip: the output never leaves
  ±1 and never clips hard.

**Evolve** sets how far six slow random walks move the macros on their
own: cutoff (±1.6 octaves), wave, resonance, detune, space, the voices'
levels, and, past halfway, now and then one voice's octave (gliding over
about two seconds). Push encoder 11 to hold the walks where they are.

A MIDI note (USB or TRS in) moves the root there (gliding, ~0.35 s),
folded by octaves into C1-C6, and its velocity swells the cutoff (up to
two octaves) and the level for a couple of seconds. Note-offs do nothing:
it's a drone.

## Controls

| Encoder | Turn | Push | MIDI CC |
|---|---|---|---|
| 1 Root | C1-C6 | octave up (wraps to the lowest) | (notes) |
| 2 Chord | unison, octaves, fifths, quartal, sus2, sus4, minor, major, minor 7, major 7, minor 9, cluster | open voicing (every other voice an octave up) | 20 |
| 3 Detune | 0-40 cents of spread | | 94 |
| 4 Wave | sine → triangle → saw | | 70 |
| 5 Cutoff | 30 Hz - 12 kHz | | 74 |
| 6 Reso | Q 0.5-16 | | 71 |
| 7 Mode | LP → BP → HP | | 21 |
| 8 Drive | clean → heavy | | 22 |
| 9 Drift rate | 0.01-2 Hz | | 76 |
| 10 Drift | pitch drift (to ±25 cents) and cutoff LFO depth | | 77 |
| 11 Evolve | how far the macros wander | hold | 1 |
| 12 Level | | mute (fades out/in) | 7 |
| 13 Space | reverb mix | freeze | 91 |
| 14 Decay | reverb feedback | | 23 |
| 15 Sub | sine an octave down | | 24 |
| 16 Air | noise | | 25 |

A CC's 0-127 maps onto the macro's whole range (Chord: 12 steps). CCs are
taken on any channel. Fast turns move the 0-127 macros three steps a
detent.

## Screen and rings

The title row shows the root and chord, plus `HOLD`, `FRZ`, `MUTE` while
those are on, or else the audio callback's CPU load (`cpu 27%`). Turning
an encoder (or a CC) shows that macro's name and value there for 1.5 s.
Below it: an oscilloscope of the output, triggered on a rising zero
crossing and spanning about three periods of the root; to its right the
six voices as dots, their height the current detune plus drift (2 cents
a pixel), bigger while the voice is loud. The bottom row has the 16
macros as bars (the last touched one boxed).

Each ring shows its macro as an arc in its row's colour (pitch amber,
filter green, motion violet, space blue), brightest at the tip. Cutoff,
Wave and Space also show a white dot where the macro is right now, with
the LFO, drift and evolve applied. Hold puts a red LED at the top of the
Evolve ring, mute turns every other Level LED red (otherwise the far end
of the Level ring shows the output peak in green), freeze lights the
whole Space ring cyan, open voicing adds a yellow LED to Chord.
Brightness stays under the stock firmware's ceiling (26 of 255 here).

## Saved settings

The 16 macros, hold and open voicing are saved to
`/apps/drone/state.bin` (24 bytes) 2 s after the last change, then at
most every 20 s while things keep changing, and once more when Shift has
been held 0.4 s (on the way out, before the launcher's 1 s exit). A
write the SPI flash's erase budget refuses (`E16_EBUSY`) is retried 5 s
later. Each save logs `app drone: drone: saved (changed)` or
`(leaving)`. Mute and freeze start off.

## Cost

On the device the callback averages 39-42% of the CPU (`audio: app load
41.2% (last second; slowest call 46.7% of a block)`), plus about 6% for
the audio interrupt itself. The DSP works out everything that changes
slowly once per 32-sample block and interpolates the filter's
coefficients across it; there's no libm (2^x is a polynomial, tan a Padé
approximant, sin a folded odd polynomial). On the host it's about 40 ns
a sample. Memory: 12.6 KB code, 36 KB data (most of it the reverb's
delay lines).

## Tests

`test/drone/drone_test.c` (run by `test/run_all.sh`): the default patch
is audible and not hot (peak -5 dBFS, RMS -16 dBFS); 10 minutes of
random macros, toggles and notes never produce a non-finite sample or
one outside ±1; every macro at its top (and frozen) stays bounded; level
0 and mute go silent and come back; a plain patch at A2 puts its
strongest component at 110 Hz, fifths add 164.8 Hz, the sub 55 Hz, and a
MIDI note moves it; the scope captures; 2^x is within 3e-5; after 60 s
of silence it's no slower (no denormals).
