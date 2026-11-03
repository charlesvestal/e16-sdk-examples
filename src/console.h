#ifndef CONSOLE_H
#define CONSOLE_H
#include <stdbool.h>
#include "midi.h"

/*
 * USB SysEx console transport.
 *
 *   F0 7D 45 05 F7                reboot into the bootloader (in; always
 *                                 built in, dev console or not)
 *   F0 7D 45 61 F7                replay the log's retained history (in;
 *                                 always built in, dev console or not)
 *   F0 7D 45 60 <text> F7         one chunk of a log line, '\n' included,
 *                                 chunked to <=120 bytes of payload (out)
 *
 * Built in with E16_DEV_CONSOLE or E16_MIRROR (the mirror's remote
 * control; src/mirror/mirror.h, which also documents the mirror's own
 * messages, 0x20/0x21 in and 0x70-0x74 out, in mirror_core.h):
 *
 *   F0 7D 45 10 <ctl> <act> <n> F7   virtual control (in). ctl 0-15 =
 *                                 encoders 1-16, 16 = Shift. act 0 =
 *                                 release, 1 = press, 2 = turn by n - 64
 *                                 steps. A virtual press is OR-ed with the
 *                                 physical button, so the whole UI (menu,
 *                                 Shift gestures, scripts) sees it exactly
 *                                 like a real one. A "down" left without a
 *                                 matching "up" auto-releases after 3 s
 *                                 (console_virtual_down()), so a lost
 *                                 SysEx can't latch a press forever; a
 *                                 repeated "down" restarts the 3 s.
 *
 * Development aids (tools/console.py drives them). Gated behind the CMake
 * option E16_DEV_CONSOLE (default ON, for remote testing -- a release
 * build should turn it OFF); with it off, console_handle() ignores all of
 * these and they have no effect:
 *
 *   F0 7D 45 06 F7                reboot back into the launcher itself
 *                                 (in; a dev aid to emulate a power cycle
 *                                 without unplugging the device)
 *   F0 7D 45 11 F7                dump the OLED framebuffer (in); answered
 *                                 by 16 messages (out):
 *   F0 7D 45 62 <page> <half> <128 nibbles> F7
 *                                 page 0-7, half 0-1 = columns 0-63 /
 *                                 64-127, each byte as high then low
 *                                 nibble (bit 0 = top pixel of the page).
 *   F0 7D 45 12 F7                sample TRS 2's PWM duty (TIM8 CCR1) 50
 *                                 times over 100 ms, from the main loop
 *                                 (in); answered by a log line "console:
 *                                 pwm min A max B distinct N ..." -- the
 *                                 synth's audio-activity proxy.
 *   F0 7D 45 13 <op> <a> <b> F7   the synth without a script (in): op 0 =
 *                                 note a velocity b (0 = off), 1 =
 *                                 parameter a (rt_audio_param_t) = b, 2 =
 *                                 output a (rt_audio_out_t: 0 auto, 1 usb,
 *                                 2 trs2, 3 both), 3 = enable (b = 0/1).
 *                                 Logged as "console: synth op ...". A
 *                                 script load or unload releases it.
 *   F0 7D 45 14 <cmd> <hi> <lo> F7   noise lab (in; noise_lab.h): cmd a
 *                                 noise_cmd_t, value (hi << 7) | lo,
 *                                 0-255. Logs "noise: <all settings>".
 *   F0 7D 45 07 <index> F7        corrupt scenes[index]'s saved vars
 *                                 record's CRC on flash (in;
 *                                 persist.h), to exercise the
 *                                 corrupted-record path without a
 *                                 special build.
 *
 * midi_read() has a single consumer, the main loop: it hands every message
 * to console_handle(), which acts on the console SysEx messages above
 * (true) and ignores everything else (false). Call console_drain_log()
 * once per main-loop pass (it also sends a requested framebuffer dump).
 */
bool console_handle(const midi_msg_t *msg);
void console_drain_log(void);
/* True once every complete log line has gone out over USB. */
bool console_log_sent(void);
/* How long USB MIDI has been connected without a break, as seen by
 * console_drain_log() (called every main-loop pass, and by app mode's
 * service); 0 while it isn't. */
uint32_t console_usb_up_ms(void);

/* True if USB has been up at any point this boot (as far as the calls
 * here and console_drain_log() have seen). */
bool console_usb_seen(void);

/* Virtual controls (see above). index 0-15 = encoders, 16 = Shift. */
bool console_virtual_down(int index);
int console_virtual_take_steps(int index);   /* steps since the last call */
/* console_virtual_down() without its auto-release (or its log line): safe
 * from an interrupt (app mode's Shift watch). */
bool console_virtual_held(int index);

/* App mode (src/apps/app_mode.h): the screen dump sends fb (the app's
 * framebuffer) instead of the launcher's screen; the bootloader SysEx
 * goes straight there (no launcher state to save); the persistence dev
 * aid (07) is ignored. */
void console_app_mode(const uint8_t *fb);

#endif
