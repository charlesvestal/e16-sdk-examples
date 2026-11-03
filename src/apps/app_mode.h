#ifndef APP_MODE_H
#define APP_MODE_H
#include <stdbool.h>
#include <stddef.h>

#include "boot_records.h"

/*
 * Clean-boot app mode: native apps (.e16app, e16_app_format.h) run from
 * RAM in a boot of their own. The app is copied from the SPI flash's FAT
 * volume into RAM and runs there.
 *
 * Trust: a native app is trusted, privileged code, not sandboxed. The
 * e16_api_t table offers no flash function, and neither the launcher nor
 * this runtime ever writes the internal flash
 * (tools/check_no_flash_writes.sh gates the build) -- but an app, running
 * with the CPU's full privileges, could in principle. As a seat belt
 * against stray writes, app mode makes the flash controller's registers
 * and the internal flash read-only with the MPU before jumping in (a
 * write faults, and the app ends back in the launcher); a deliberately
 * malicious app could switch the MPU off, so that is all it is.
 *
 *   1. The menu (launcher.c) picks /apps/<stem>.e16app.
 *      app_launch_prepare() checks the image (header, sizes against the
 *      app-mode capacity, length, CRC) and refuses a bad one right there;
 *      app_launch_reset() writes "run this app" into the reset-surviving
 *      record (boot_records.h), then a launch token, and resets.
 *   2. main() asks app_mode_requested() before anything else and, if the
 *      record asks for an app right after that reset (not after a reflash
 *      or any other reset: app_boot.h),
 *      runs app_mode_main() instead of the launcher: it marks the record
 *      taken (so an app that crashes can't boot-loop), brings up only the
 *      clocks, the display, MIDI/USB, the LEDs, the encoders, the synth
 *      (idle until used), the SPI flash and the FAT volume -- no Lua, no
 *      Lua heap -- loads the app (text into SRAM, data + bss into the CCM:
 *      mem_layout.h) and jumps to it with r9 = its GOT and r0 = the api
 *      table (src/apps/api.c), on a fresh stack at the top of SRAM.
 *   3. Every way out resets back into the launcher, noting why in the
 *      record: api->exit() or returning from app_main(); Shift held 1 s
 *      (watched from the SysTick, so it works whatever the app does); a
 *      fault (main.c's handlers); a smashed stack canary. The next normal
 *      boot logs it (app_mode_boot_report()); an image that didn't load
 *      comes back as an error screen.
 *
 * In app mode the dev console keeps working (log, screen dump of the
 * app's framebuffer, virtual controls, reboot): it's serviced inside the
 * api calls and, while the app is busy elsewhere, from PendSV.
 */

/* ---- normal mode ---- */

/* Checks /apps/<stem>.e16app without loading it. NULL if it can be run,
 * else a message for the error screen (also logged). */
const char *app_launch_prepare(const char *stem);

/* After app_launch_prepare() said yes: records the request, lets the log
 * drain, and resets into app mode. */
void app_launch_reset(void) __attribute__((noreturn));

/* At a normal boot, after the log is up: logs how the previous app run
 * ended (if there was one) and clears the record. */
void app_mode_boot_report(void);

/* From app_mode_boot_report(): the stem of the app that just ran (NULL if
 * none), and why it didn't load (NULL if it did). */
const char *app_mode_last_stem(void);
const char *app_mode_last_error(void);

/* The app-mode text capacity in bytes (SRAM); data + bss get
 * APP_DATA_SIZE (mem_layout.h). */
size_t app_mode_text_cap(void);

/* ---- boot ---- */

/* True if the record asks for an app and this boot is the launcher's own
 * reset into app mode (app_boot.h: the launch token and RCC_CSR's reset
 * flags); called once, first thing in main(). It reads and clears the
 * reset flags and consumes the token. */
bool app_mode_requested(void);
void app_mode_main(void) __attribute__((noreturn));

/* ---- app mode ---- */

/* True once app mode has started (main.c's fault handlers then reset into
 * the launcher instead of the bootloader). */
bool app_mode_active(void);

/* The fault handler's note in the record (no logging, no flash). */
void app_mode_note_fault(void);

/* The dev console's reset (SysEx 06) in app mode: notes it in the record,
 * so the launcher logs "(console)" rather than "(reset)". No flush: the
 * reset stands in for pulling the cable. Nothing outside app mode. */
void app_mode_note_console_reset(void);

/* Leave: log, close the app's files, flush the disk, note why in the
 * record, let the log drain, reset into the launcher. Thread or PendSV
 * context (never from a higher-priority interrupt). */
void app_mode_end(app_end_t reason, int code) __attribute__((noreturn));

/* What the api calls do while the app waits on them: console SysEx and
 * MIDI input, the log drain, the synth's TRS 2 switching, the stack
 * canary. Never runs concurrently with itself or with an api call. */
void app_mode_service(void);

/* An exit asked for from an interrupt (Shift held) while the app was
 * inside an api call: the api calls this on the way out. */
void app_mode_check_exit(void);

#endif
