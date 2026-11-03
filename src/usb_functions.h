#ifndef USB_FUNCTIONS_H
#define USB_FUNCTIONS_H

/*
 * The launcher's USB functions next to MIDI (the SDK's usb.h): Disk
 * mode's drive, the synth's audio input and webcam mode's camera, from
 * the SDK's examples (usb_msc.c, usb_audio.c, usb_video.c, built
 * unchanged). Normally the computer sees MIDI + drive + audio; in webcam
 * mode, MIDI + camera (usb_video_select(), webcam.h).
 *
 * Call once, before midi_init(), in normal and in app mode.
 */
void usb_functions_start(void);

#endif
