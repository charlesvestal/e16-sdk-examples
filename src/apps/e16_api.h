#ifndef E16_API_H
#define E16_API_H
#include <stddef.h>
#include <stdint.h>

/*
 * The table of functions a native app (.e16app) gets from the launcher:
 * its only way to reach the hardware. The app receives a pointer to it as
 * the argument of app_main() (apps/sdk/e16_app.h).
 *
 * Versioning. The table starts with abi_version and size. Functions are
 * only ever added at the end; existing entries never change signature or
 * meaning. An app built against a newer header must check
 * `api->size >= offsetof(e16_api_t, the_function) + sizeof(void *)` (see
 * E16_API_HAS() below) before calling anything added after the version it
 * requires. abi_version changes only if this rule has to be broken, and
 * the loader refuses images built for another ABI version
 * (E16APP_ABI_VERSION in e16_app_format.h).
 *
 * Calling convention. Plain AAPCS with the hard-float ABI
 * (-mfloat-abi=hard -mfpu=fpv4-sp-d16), the same as the firmware. These
 * functions are firmware code: they don't use r9 as a GOT pointer, but
 * r9 is callee-saved under AAPCS, so it's intact when they return to the
 * app. The one call back into the app is the audio render callback
 * (audio_start()), which the launcher makes with r9 set to the app's GOT.
 *
 * Context. Call these only from the app's main flow (the stack app_main()
 * runs on), not from an interrupt. The one exception is the audio render
 * callback (audio_start() below): it runs in an interrupt, and may call
 * millis() and nothing else.
 *
 * Pointers. A pointer an app passes in (a buffer, a string, a callback)
 * may be a CCM address (its data, stack-free .bss) or an address in its
 * text at 0x0000xxxx (app mode runs the text from SRAM1's alias at 0:
 * string literals and const tables live there). DMA can reach neither
 * (CCM isn't on the DMA bus, and DMA doesn't see the alias), so no
 * function here may hand a caller's pointer to a DMA transfer: copy into
 * the launcher's own buffer first.
 *
 * Trust. A native app is trusted, privileged code: it runs on the CPU
 * with the firmware's own privileges, not in a sandbox. This table offers
 * no way to write the E16's internal flash, and neither the launcher nor
 * the app runtime ever writes it, but an app could in principle reach the
 * hardware directly. As a guard against a stray write, app mode makes the
 * internal-flash controller's registers read-only (the MPU): touching
 * them faults, and the app ends back in the launcher. That is a seat
 * belt, not a sandbox -- the app could turn the MPU off again. File
 * writes through this table go only under the app's own folder,
 * /apps/<name>/.
 */

#define E16_API_VERSION 1u

/* Display: 128x64 monochrome, the SSD1306 page layout (display.h in the
 * SDK). Byte (page * 128 + x) holds column x of the 8-pixel strip `page`,
 * bit 0 at the top. */
#define E16_DISPLAY_WIDTH   128
#define E16_DISPLAY_HEIGHT  64
#define E16_FB_SIZE         (E16_DISPLAY_WIDTH * E16_DISPLAY_HEIGHT / 8)

#define E16_ENC_COUNT       16
#define E16_LED_RINGS       16
#define E16_LEDS_PER_RING   16

/* MIDI ports, as the SDK's midi_port_t. */
#define E16_MIDI_TRS_1      0  /* output 1 */
#define E16_MIDI_TRS_2      1  /* output 2 */
#define E16_MIDI_TRS_IN     2  /* input */
#define E16_MIDI_USB        3  /* both directions */

/* file_open() flags. */
#define E16_FILE_READ       0

/* Errors (negative returns). */
#define E16_EARGS           (-1) /* a bad argument */
#define E16_EIO             (-2) /* a FatFs or flash error */
#define E16_EFULL           (-3) /* no free file handle */
#define E16_EBUSY           (-4) /* file_write(): not now (see there); nothing was changed */

/* The most file_write() writes in one call. */
#define E16_FILE_WRITE_MAX  65536u

/* Audio streams (audio_start()). */
#define E16_AUDIO_RATE      48000u /* frames per second, always */
#define E16_AUDIO_BLOCK     32u    /* frames per render callback call */

/* The render callback: fill out[0 .. frames-1] with mono samples in
 * [-1, 1] (frames is E16_AUDIO_BLOCK). user is audio_start()'s. Runs in an
 * interrupt: see audio_start(). */
typedef void (*e16_audio_render_t)(float *out, uint32_t frames, void *user);

/* audio_info()'s state. */
#define E16_AUDIO_OFF       0u /* not started, or stopped by audio_stop() */
#define E16_AUDIO_RUNNING   1u
#define E16_AUDIO_OVERLOAD  2u /* stopped by the launcher: the callback was over budget (audio_start()) */

/* What audio_info() reports. Only ever grows at the end; audio_info()
 * fills as much as the app's copy holds. */
typedef struct {
    uint32_t rate;         /* E16_AUDIO_RATE */
    uint32_t block;        /* E16_AUDIO_BLOCK */
    uint32_t state;        /* E16_AUDIO_OFF / RUNNING / OVERLOAD */
    uint32_t load;         /* the callback's own average CPU use over the last full second, in 0.1 %
                              (1000 = all of it); the budget is E16_AUDIO_BUDGET */
    uint32_t load_peak;    /* the slowest call in that second, as 0.1 % of one block's time */
    uint32_t late;         /* blocks the callback hadn't finished in time, since audio_start() */
    uint32_t calls;        /* callback calls since audio_start() */
} e16_audio_info_t;

/* The sustained load (0.1 %) past which the launcher stops a stream. */
#define E16_AUDIO_BUDGET    800u

/* A received MIDI message. data is valid until the next midi_read(). */
typedef struct {
    int port;                /* E16_MIDI_* it arrived on */
    const uint8_t *data;     /* status byte first */
    uint32_t len;            /* 1-3, or a SysEx length (F0 ... F7) */
} e16_midi_msg_t;

typedef struct e16_api {
    uint32_t abi_version;    /* E16_API_VERSION the firmware implements */
    uint32_t size;           /* sizeof(e16_api_t) in the firmware */

    /* ---- display ---- */
    /* The E16_FB_SIZE-byte framebuffer (the app's to draw in) and a call
     * that sends it to the OLED (about 0.5 ms; blocks). */
    uint8_t *(*display_fb)(void);
    void (*display_present)(void);

    /* ---- LEDs ---- */
    /* Buffer one LED (ring 0-15, led 0-15); show() sends the buffer in the
     * background (~1.5 ms) and returns 0 if the previous frame was still
     * going out (nothing is lost: call it again later). Keep levels low:
     * the stock firmware never drives a channel above ~85. */
    void (*leds_set)(int ring, int led, uint8_t r, uint8_t g, uint8_t b);
    int (*leds_show)(void);

    /* ---- encoders ---- */
    /* Steps since the last call for that encoder (clockwise positive);
     * push state; the Shift button. */
    int (*enc_read)(int index);
    int (*enc_pressed)(int index);
    int (*shift_pressed)(void);

    /* ---- MIDI ---- */
    /* Queue bytes on an output (all or nothing; 0 if there's no room).
     * read(): next complete message from any input, 1 if there was one. */
    int (*midi_write)(int port, const uint8_t *bytes, uint32_t len);
    int (*midi_read)(e16_midi_msg_t *msg);

    /* ---- time ---- */
    uint32_t (*millis)(void);
    void (*delay_ms)(uint32_t ms);

    /* ---- files (the USB disk, FatFs) ---- */
    /* Read-only access to any file: path is absolute ("/data/x.bin").
     * open() returns a handle >= 0 or a negative error; read() returns
     * bytes read (0 at the end) or a negative error; seek() sets the
     * absolute position and returns 0 or a negative error. A few handles
     * at most are open at a time; the launcher closes any left open when
     * the app exits. */
    int (*file_open)(const char *path, int flags);
    int (*file_read)(int fd, void *buf, uint32_t len);
    int (*file_seek)(int fd, uint32_t pos);
    void (*file_close)(int fd);
    /* Create or replace /apps/<app name>/<name> with len bytes (at most
     * E16_FILE_WRITE_MAX). name is a plain file name: 1-32 of
     * [A-Za-z0-9_.-], not starting with '.'. Returns 0 or a negative
     * error. Blocks while the data goes onto the SPI flash (tens of ms,
     * up to a second or two for a large file).
     *
     * E16_EBUSY: the SPI flash's erase budget can't cover a write right
     * now, so none was started -- the file is exactly as it was. Try
     * again later. The budget allows about one sector erase a second and
     * any one sector 4 erases a minute; a rewrite of a small file at the
     * same length costs one or two, a new file or a new length up to
     * about 8. In practice the same file can be rewritten about 4 times
     * a minute (every 15 s always works); more often than that gets
     * E16_EBUSY. Keep writes for state worth keeping, not every frame.
     *
     * The file is rewritten in place: if the E16 loses power in the
     * middle of a write, the file may hold part old, part new data. */
    int (*file_write)(const char *name, const void *buf, uint32_t len);

    /* ---- log ---- */
    /* One line to the launcher log (no newline needed). */
    void (*log)(const char *msg);

    /* ---- synth ---- */
    /* The launcher's mono synth voice. velocity 0 = note off. param ids
     * are the launcher's voice_param_id_t; values 0-127. */
    void (*synth_note)(uint8_t note, uint8_t velocity);
    void (*synth_param)(int id, uint8_t value);

    /* ---- leaving ---- */
    /* Back to the launcher (by a reset; does not return). Returning from
     * app_main() does the same. */
    void (*exit)(int code) __attribute__((noreturn));

    /* ---- audio stream (newer than the first ABI 1 table: check
     * E16_API_HAS(api, audio_start) first) ---- */
    /*
     * The app's own sound. audio_start() has the launcher call render
     * every E16_AUDIO_BLOCK frames (0.67 ms) at E16_AUDIO_RATE, mono, to
     * fill the next block; the samples go where the synth's do (USB audio
     * while a computer is connected, else TRS 2), mixed with the synth
     * voice if the app plays that too (the sum is clamped to [-1, 1] --
     * no soft clipping is applied to the app's samples: keep them within
     * +-1 yourself, ideally with a soft limiter; a NaN plays as 0). The
     * output runs a block behind the callback (plus the USB ring's 2 ms).
     * Returns 0, or E16_EARGS (render NULL, or called from the callback
     * itself). Calling it again replaces
     * the callback (and restarts the stream, with a block of silence).
     *
     * The callback runs in an interrupt, below every driver's and above
     * the app's own code (it preempts the app anywhere, between and inside
     * api calls). So:
     *   - It may call millis() and no other api function.
     *   - It must return well within a block's time: it gets about
     *     E16_AUDIO_BUDGET (80 %) of the CPU on average. The launcher stops
     *     the stream (logged; audio_info() says E16_AUDIO_OVERLOAD;
     *     audio_start() again to retry) when, 3 s in a row, the callback's
     *     own load is over that or more than 5 % of the blocks are late
     *     (it plus the interrupts above it -- the synth voice, USB --
     *     doesn't fit), or at once when its calls have taken a whole
     *     second's wall time (nothing else ran meanwhile). A block
     *     it doesn't finish in time plays as far as it got (counted in
     *     audio_info().late). A call that hasn't returned after 500 ms
     *     ends the app (back to the launcher, logged as "audio callback
     *     stuck").
     *   - It shares the app's stack: keep its locals small (well under
     *     1 KB).
     *   - Data it shares with the app's main flow: single aligned 32-bit
     *     words (ints, floats, pointers) are read and written whole, so a
     *     parameter the main flow sets is seen by the next block; anything
     *     bigger needs a handoff of your own (e.g. a sequence number), or
     *     audio_stop()/audio_start() around it.
     * The launcher logs the callback's load every 10 s ("audio: app
     * load ...").
     */
    int (*audio_start)(e16_audio_render_t render, void *user);
    /* Stops calling render (silence from the next sample); when this
     * returns, the callback isn't running and won't run again. Does
     * nothing when called from the callback itself. */
    void (*audio_stop)(void);
    /* Fills *info (as much as size bytes of it); returns 0 or E16_EARGS. */
    int (*audio_info)(e16_audio_info_t *info, uint32_t size);

    /* ---- leaving (newer: check E16_API_HAS(api, exit_pending)) ---- */
    /* 1 once the user has confirmed the launcher's leave prompt (Shift
     * held on its own for 5 s, then encoder 1). Save what's worth keeping
     * and call exit(): the launcher ends the app itself 1.5 s after the
     * confirmation either way. While the prompt is up the app reads no
     * input (enc_read 0, enc_pressed 0, shift_pressed 0); if the user
     * lets go of Shift instead, it carries on as before. */
    int (*exit_pending)(void);

    /* New entries go here, at the end, only. */
} e16_api_t;

/* True if the firmware's table has the given function (newer than ABI 1
 * entries must be checked with this before use). */
#define E16_API_HAS(api, field) \
    ((api)->size >= offsetof(e16_api_t, field) + sizeof((api)->field))

#endif
