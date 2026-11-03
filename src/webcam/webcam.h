#ifndef WEBCAM_H
#define WEBCAM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Webcam mode: the E16 as a USB video synth. Entered from the launcher's
 * menu ("Webcam", src/ui/launcher.c), only with no script running, like
 * Disk mode.
 *
 * Entering re-enumerates the E16 as USB MIDI + a UVC camera, 160x120 YUY2
 * at 20 fps (usb_video_select(), usb_video.h in the SDK's examples): there's no
 * mass storage and no USB audio input meanwhile (the USB core's packet
 * memory holds MIDI + video or MIDI + the rest, not both). Leaving
 * re-enumerates back to MIDI + mass storage + audio. Each switch detaches
 * the E16 from the computer for about half a second.
 *
 * The picture is made a line at a time (wc_render.h), no frame buffer:
 * a lowest-priority interrupt renders up to 12 lines ahead of the USB
 * service interrupt, which sends them (webcam.c, "Rendering ahead"); the
 * main loop only changes settings, builds the palette, and draws the OLED
 * (status and a dithered mirror) and the LED rings (the picture's colours
 * on a 4x4 grid, each ring's setting as a white dot). 20 fps with every
 * setting; 21% of the CPU (one pattern) to 45% (the busiest settings).
 *
 * Memory: the tables, palettes, views and the ring (~23 KB,
 * sizeof(wc_mem_t) in webcam.c, logged on entering) come from the Lua
 * heap, which nothing uses while no script runs, and go back to it on
 * leaving: no static RAM beyond a few words and the 16 settings, which
 * last until a reset.
 *
 * Settings (encoder N = parameter N, wc_render.h): turning steps it,
 * pressing resets it to its default, except encoder 1, whose short press
 * switches the OLED between status + small mirror and a full-screen
 * mirror (the launcher handles its long press: leave).
 *
 * MIDI in (USB or TRS, any channel): note on = jump the hue to the note's
 * pitch class and flash towards white by velocity, and show the note name
 * if TEXT is NOTE; CC 20-35 = parameters 1-16 across their range; CC 1 =
 * MIX; program change = pattern A.
 *
 * Main loop only, except the video callbacks in webcam.c.
 */

/* Allocates from the Lua heap and switches USB to MIDI + video. False
 * (nothing changed) if the heap can't spare the memory. */
bool webcam_enter(void);

/* Switches USB back (MIDI + mass storage + audio) and frees the memory.
 * why ends up in the log ("shift hold", "encoder 1", ...). */
void webcam_leave(const char *why);

bool webcam_active(void);

/* Every main-loop pass while active: animation, palette, OLED, LEDs, the log. */
void webcam_poll(uint32_t now_ms);

/* Encoder enc (0-15) turned or pressed (encoder 0's short press: the OLED view). */
void webcam_turn(int enc, int steps);
void webcam_press(int enc);

/* A MIDI message from USB or TRS (not realtime, not the console's SysEx). */
void webcam_midi(const uint8_t *data, size_t len);

#endif
