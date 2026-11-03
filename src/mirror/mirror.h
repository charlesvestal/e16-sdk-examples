#ifndef MIRROR_H
#define MIRROR_H

/*
 * The mirror: a live view of the E16 (OLED, LED rings, input events) for
 * a computer that subscribes over USB MIDI, plus remote control through
 * the console's virtual controls. tools/mirror/index.html is the browser
 * side, tools/mirror/mirror_client.py a command-line one. The protocol
 * and the pacing are in mirror_core.h; this is the device glue.
 *
 * Built in with the CMake option E16_MIRROR (default ON), independently
 * of E16_DEV_CONSOLE. Anything on the computer that can send MIDI to the
 * E16 can watch it and drive it while it's on (as with the dev console).
 *
 * Runs in every mode: the launcher's main loop calls mirror_poll() once a
 * pass, app mode's service (src/apps/app_mode.c) likewise. Every write is
 * a try (midi_write() queues all of a message or none): nothing here
 * waits for USB.
 */

#include <stdbool.h>
#include <stdint.h>

#include "midi.h"

#ifndef E16_MIRROR
#define E16_MIRROR 1
#endif

#if E16_MIRROR

/* A message from the computer: true if it was the mirror's (subscribe,
 * keepalive, unsubscribe). console_handle() asks this first. */
bool mirror_handle(const midi_msg_t *msg);

/* Send what's due (events, state, the rings, the OLED); also watches the
 * hardware buttons for events. Once per main-loop pass. */
void mirror_poll(void);

/* A hardware turn as it's read (launcher.c, the app api's enc_read); a
 * virtual one as the console receives it. */
void mirror_turn(int index, int steps, bool remote);

/* A virtual button changing (console.c). Hardware buttons are watched by
 * mirror_poll() itself. index 16 = Shift. */
void mirror_button(int index, bool down, bool remote);

/* App mode: mirror oled (what the app last presented) and call the mode
 * "app" with this name. */
void mirror_app_mode(const uint8_t *oled, const char *name);

#else

static inline bool mirror_handle(const midi_msg_t *msg)
{
    (void)msg;
    return false;
}
static inline void mirror_poll(void) {}
static inline void mirror_turn(int index, int steps, bool remote)
{
    (void)index;
    (void)steps;
    (void)remote;
}
static inline void mirror_button(int index, bool down, bool remote)
{
    (void)index;
    (void)down;
    (void)remote;
}
static inline void mirror_app_mode(const uint8_t *oled, const char *name)
{
    (void)oled;
    (void)name;
}

#endif

#endif
