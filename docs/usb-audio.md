# USB audio

The E16 shows up on the computer as a USB audio input next to its MIDI
port: "E16 Launcher" (or the name macOS cached, e.g. "E16 Custom"; see
the README's ["USB name"](../README.md#usb-name)), one channel, 16-bit, 48 kHz, USB Audio Class 2.0 with
the operating system's own driver. Record it in any audio app, or e.g.
`sox -t coreaudio "E16 Launcher" -c 1 -b 16 out.wav trim 0 10`. The synth
plays there by default whenever a computer is connected, as clean
digital audio. It's the only audio output in the default build (see
["TRS 2 audio (optional)"](#trs-2-audio-optional) below).

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
  [Webcam mode](modes.md#webcam-mode)), so the synth is silent while it's on.
- **Where they come from**: the drive, the audio input and the camera are
  the SDK's examples (`usb_msc.c`, `usb_audio.c`, `usb_video.c` in
  `external/E16-SDK/examples/usb_disk`, `usb_tone`, `usb_cam`), compiled
  unchanged. The SDK takes two USB functions next to MIDI, so
  `src/usb_functions.c` gives it two: the drive, which leaves the
  descriptors while the camera is on, and one slot that is the camera in
  Webcam mode and the audio input otherwise. TinyUSB's classes for them
  are switched on in `src/tusb_config_app.h`.
- **Native apps** play the same synth (`synth_note`/`synth_param`), and
  can stream their own sound (`audio_start()`, see [app-runtime.md](app-runtime.md)), with
  the same "auto" routing; the two are summed.


## TRS 2 audio (optional)

Off by default. Build with `-DE16_TRS_AUDIO=ON` to let the synth and app
audio play on TRS 2 as PWM (TIM8_CH1 on PC6). It isn't usable on this
hardware: the jack idles on the E16's internal supply, which carries a
loud whine from the OLED's power that changes with what the screen
shows. A listening test with the noise lab ([development.md](development.md)) found
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
