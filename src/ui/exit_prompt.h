#ifndef EXIT_PROMPT_H
#define EXIT_PROMPT_H
#include <stdbool.h>
#include <stdint.h>

/*
 * Leaving a script, an app, Disk mode, Webcam mode or the error screen:
 * hold Shift on its own (no encoder pushed or turned since it went down)
 * for EXIT_PROMPT_HOLD_MS. The launcher then draws a prompt over the
 * screen and stops passing input on (an app's enc_read / enc_pressed /
 * shift_pressed read nothing): push encoder 1 to leave; let go of Shift,
 * or wait EXIT_PROMPT_SHOW_MS, to stay.
 *
 * (Shift held 1 s used to leave at once, and then Shift + 13-16 for a
 * while; both collided with apps' own Shift gestures, because an app saw
 * each push before the launcher knew what it was part of. Shift held on
 * its own for 5 s isn't anyone's gesture, and the confirming push never
 * reaches the app.)
 *
 * The same state machine runs in the launcher's UI loop and in app mode's
 * SysTick watch (it touches no hardware: callers pass the input in).
 * Freestanding, host-tested by test/ui/exit_prompt_test.c.
 */

#define EXIT_PROMPT_HOLD_MS 5000u    /* Shift alone this long: the prompt */
#define EXIT_PROMPT_SHOW_MS 4000u    /* the prompt waits this long for encoder 1 */

enum { EXIT_PROMPT_IDLE, EXIT_PROMPT_ARMING, EXIT_PROMPT_SHOWN, EXIT_PROMPT_CONFIRMED };
/* exit_prompt_step()'s events */
enum { EXIT_PROMPT_NONE, EXIT_PROMPT_SHOW, EXIT_PROMPT_HIDE, EXIT_PROMPT_LEAVE };

typedef struct {
    uint8_t state;      /* EXIT_PROMPT_* state */
    bool touched;       /* an encoder was pushed or turned during this Shift hold */
    bool enc1_was;      /* encoder 1 was down at the last step */
    uint32_t t;         /* ms in this state */
} exit_prompt_t;

/* dt ms have passed. shift: Shift is down; pressed: bit i = encoder index
 * i is down; turned: an encoder was turned since the last step. Returns
 * EXIT_PROMPT_SHOW when the prompt should appear, _HIDE when it should go
 * (cancelled), _LEAVE once encoder 1 confirmed it (the state then stays
 * CONFIRMED until exit_prompt_reset()). */
int exit_prompt_step(exit_prompt_t *p, uint32_t dt, bool shift, uint32_t pressed, bool turned);

/* The prompt is up (or confirmed): input belongs to the launcher. */
bool exit_prompt_active(const exit_prompt_t *p);

void exit_prompt_reset(exit_prompt_t *p);

/* Draw the prompt over a 128x64 1-bit framebuffer in the SSD1306 page
 * layout (byte = 8 rows of one column, bit 0 on top; 8 pages of 128).
 * name: what's being left ("swell", "Euclid"; NULL for none);
 * confirmed: "Leaving..." instead of the question. */
void exit_prompt_draw(uint8_t *fb, const char *name, bool confirmed);

/* Draw "Release the buttons to leave" over the whole framebuffer: leaving
 * an app resets the E16, and OXI's bootloader starts its firmware update
 * mode when it sees encoders 15 and 16 (and Shift) held at power-up, so
 * the reset waits for every button to be up. */
void exit_prompt_draw_release(uint8_t *fb);

#endif
