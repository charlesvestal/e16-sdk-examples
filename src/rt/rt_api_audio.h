#ifndef RT_API_AUDIO_H
#define RT_API_AUDIO_H
#include <stddef.h>
#include <stdint.h>

#include "lua.h"
#include "rt_port.h"

/*
 * The audio global: the internal synth voice, over USB audio and/or on
 * TRS 2 (rt_port.h's rt_port_audio_* comment has the model).
 *
 *   audio.enable(on)              on: boolean. The synth plays while
 *                                 enabled; TRS 2 carries it then if it's
 *                                 routed there, MIDI otherwise.
 *   audio.note(note, vel[, slide]) note 0-127, vel 0-127 (0 = that note
 *                                 off, >= 100 = accent); enables the synth.
 *                                 slide (optional): sets the slide flag
 *                                 first, so this note glides from a note
 *                                 still held (legato), like a 303.
 *   audio.off()                   release whatever is sounding
 *   audio.set(name, value)        value 0-127; name: "cutoff",
 *                                 "resonance", "envmod", "decay",
 *                                 "waveform" (< 64 saw, else square),
 *                                 "volume", "glide" (glide time), "slide"
 *   audio.output([where])         where: "usb" (the E16's USB audio
 *                                 input; TRS 2 stays MIDI), "trs2", "both"
 *                                 or "auto" (the default: "usb" while a
 *                                 computer has the E16 connected over
 *                                 USB, else "trs2"). Returns where the
 *                                 synth plays now, "auto" resolved. Back
 *                                 to "auto" when the script unloads.
 *
 * Numbers are floored, like leds/ring. Bad arguments raise an error.
 */
void rt_api_audio_open(lua_State *L);

/* rt_unload(): silence and hand TRS 2 back to MIDI. */
void rt_api_audio_close(void);

/* A message the script sends to MIDI out port 4 (rt_api_out.c), before it
 * goes to the port: tracks the notes held there (see rt_api_audio.c). */
void rt_api_audio_port4(const uint8_t *bytes, size_t len);

/* From rt_tick(): releases a note left held through port 4 once port 4
 * has been quiet for 2 s. */
void rt_api_audio_tick(rt_time_t now);

#endif
