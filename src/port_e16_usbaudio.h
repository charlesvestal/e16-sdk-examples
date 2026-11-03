#ifndef PORT_E16_USBAUDIO_H
#define PORT_E16_USBAUDIO_H
#include <stdbool.h>
#include <stdint.h>

/*
 * The synth as a USB audio input (port_e16_usbaudio.c), through
 * usb_audio.c from the SDK's examples, so the computer sees a
 * mono, 16-bit, 48 kHz input next to the MIDI port (and Disk mode's mass
 * storage), named after the USB product, "E16 Launcher" (or whatever
 * name macOS cached for the device, e.g. "E16 Custom").
 *
 * Mono: the synth is a single voice, so a second channel would only
 * carry a copy. (Stereo would fit the USB FIFO too: 49 words more.)
 *
 * Samples move through a lock-free single-producer/single-consumer ring:
 * the audio interrupt (TIM6, priority 2, port_e16_audio.c) pushes one
 * sample per tick at 48 kHz, and the SDK's fill() callback (USB
 * interrupt, priority 6, once per millisecond while the computer
 * records) takes 47, 48 or 49 of them, keeping the ring about 2 ms full:
 * the E16's crystal and the computer's USB clock drift apart by a few
 * samples a second, and the computer resamples from the packet sizes.
 * Until the ring holds 2 ms (at the start, and again after it ran dry)
 * fill() returns 0, which the SDK sends as a packet of silence, so a
 * note never starts with a string of short packets.
 */

/* Audio interrupt only: one sample into the ring (dropped while it's
 * full, i.e. while nobody records). */
void port_e16_usbaudio_push(int16_t s);

/* Adds the audio input to USB (usb_functions.c). Before midi_init(). */
bool port_e16_usbaudio_start(void);

/* Main loop: the producer starts (true) or stops (false). While stopped,
 * the ring running dry isn't counted as an underrun. */
void port_e16_usbaudio_producer(bool on);

/* False while the USB configuration has no audio input: in webcam mode
 * (src/webcam/webcam.h) it's MIDI + video, and while switching. The
 * synth's "auto" routing goes to TRS 2 then, and nothing is pushed into
 * the ring. */
bool port_e16_usbaudio_available(void);

/* True while the computer records from the E16's input. */
bool port_e16_usbaudio_streaming(void);

/* Every main-loop pass: logs when the computer starts and stops
 * recording, and the packet statistics every 10 s while it records. */
void port_e16_usbaudio_poll(uint32_t now_ms);

#endif
