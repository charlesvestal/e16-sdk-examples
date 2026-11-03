#ifndef PORT_E16_AUDIO_H
#define PORT_E16_AUDIO_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_stream.h"

/*
 * The internal synth (port_e16_audio.c): the voice (src/synth) renders
 * one sample per TIM6 interrupt at 48 kHz, gained up and soft-clipped to
 * use the full range, into
 *   - the USB audio input (port_e16_usbaudio.h), and/or
 *   - TIM8 channel 1's 8-bit PWM (656 kHz) on PC6, the pin TRS 2's MIDI
 *     UART (USART6) uses otherwise. TRS 1 is always MIDI.
 * audio.output() picks (rt_port_audio_output()); the default, "auto", is
 * USB audio while a computer has the E16 connected over USB (TRS 2 then
 * stays MIDI: the analog path picks up noise on this hardware), TRS 2
 * otherwise (e.g. on a USB power supply).
 *
 * The synth starts when a script enables it (audio.enable(true),
 * audio.note(), or any message to MIDI out port 4) and stops after
 * audio.enable(false) or when the script unloads
 * (rt_port_audio_release()), once the last note's release has died away.
 * TRS 2 switches to the synth while it's routed there, once whatever MIDI
 * was still queued for TRS 2 has gone out, and back after a 20 ms fade.
 * While it isn't MIDI, rt_port_midi() sends nothing to TRS 2
 * (port_e16_audio_trs2_midi() is false), so out = 0 and out = 3 skip it.
 *
 * Threads: everything here runs in the main loop except the interrupt.
 * Notes reach the interrupt through a lock-free queue; parameters are
 * written straight into the voice (single-word writes, see voice.h).
 */

/* Once at boot, after system_init(): the voice, the timers (stopped), the
 * DWT cycle counter. */
void port_e16_audio_init(void);

/* Every main-loop pass: moves TRS 2 between MIDI and the synth when it's
 * due, and logs the interrupt's CPU load every 10 s while it runs. */
void port_e16_audio_poll(uint32_t now_ms);

/* MIDI out port 4 (rt_port_midi()): note on/off and the CC map below
 * drive the voice. Any message enables the synth.
 *   CC 74 cutoff, 71 resonance, 75 envelope mod, 72 decay,
 *   70 waveform (< 64 saw, else square), 7 volume, 65 slide (>= 64 on),
 *   5 glide time; 120 / 123 release whatever is sounding.
 * Note-on velocity >= 100 is an accent. The channel is ignored. */
void port_e16_audio_midi(const uint8_t *bytes, size_t len);

/* A voice parameter by voice.h's voice_param_id_t, value 0-127 (native
 * apps' synth_param(); scripts go through rt_port_audio_set()). */
void port_e16_audio_voice_param(int id, uint8_t value);

/* True while TRS 2 may carry MIDI (it isn't the synth's, nor about to be). */
bool port_e16_audio_trs2_midi(void);
/* True while TRS 2 carries the synth. */
bool port_e16_audio_on_trs2(void);
/* True while the synth runs, on any output (the title bar's speaker icon). */
bool port_e16_audio_running(void);

/*
 * A native app's own stream (e16_api.h's audio_start(), app mode only):
 * fn(out, frames, user) fills each APP_STREAM_BLOCK-frame block, called
 * from a low-priority interrupt with r9 = the app's GOT; the samples are
 * mixed with the voice's (src/synth/app_stream.h). stream: the state, in
 * the caller's memory (the api's app-mode buffers; always the same one).
 * start() replaces a running stream; stop() returns with the callback not
 * running and not to be called again. Main flow or PendSV only.
 */
typedef void (*port_e16_audio_render_t)(float *out, uint32_t frames, void *user);
void port_e16_audio_app_start(app_stream_t *stream, port_e16_audio_render_t fn, void *user, uint32_t r9);
void port_e16_audio_app_stop(void);

typedef struct {
    uint32_t rate, block;
    uint32_t state;          /* app_stream_state_t */
    uint32_t load_permille;  /* the last full second's average, of the CPU */
    uint32_t peak_permille;  /* its slowest call, of one block's time */
    uint32_t late, calls;    /* since start */
} port_e16_audio_app_info_t;
void port_e16_audio_app_info(port_e16_audio_app_info_t *info);

/* SysTick: the app's callback has been running for 500 ms or more
 * (app_mode.c then ends the app). */
bool port_e16_audio_app_stuck(uint32_t now_ms);

/* TIM8's compare register: the PWM duty the pin has now (0-255). For the
 * dev console's audio-activity check. */
uint32_t port_e16_audio_pwm_value(void);

#endif
