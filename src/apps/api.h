#ifndef APPS_API_H
#define APPS_API_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "e16_api.h"
#include "midi.h"

/*
 * The e16_api_t table a native app gets (api.c), over the SDK drivers.
 * App mode only (app_mode.c).
 *
 * The api's own buffers (the framebuffer, file handles, the MIDI input
 * queue) aren't static: app mode places them in SRAM right after .bss,
 * ahead of the app's text (mem_layout.h), so they cost the normal-mode
 * Lua heap nothing.
 */

#define APP_FILES_MAX 4

/* Bytes app_api_init() needs at ram (8-byte aligned). */
size_t app_api_ram_size(void);

/* Sets the api up for the app called name (the image's header name):
 * clears its framebuffer, files and queue. r9: the app's GOT (its audio
 * callback runs with it). */
const e16_api_t *app_api_init(void *ram, const char *name, uint32_t r9);

/* The app's framebuffer as last presented (the dev console's screen
 * dump, the mirror): what the OLED shows. */
const uint8_t *app_api_framebuffer(void);

/* millis() when the app last presented a frame. */
uint32_t app_api_last_present(void);
/* Send the app's framebuffer again with the leave prompt over it (app
 * mode's service, when the app isn't presenting; not during an api call). */
void app_api_redraw(void);

/* An input message for the app's midi_read() (app_mode_service()). */
void app_api_midi_in(const midi_msg_t *msg);

/* True while the app is inside an api call: app_mode.c's PendSV service
 * then waits. */
bool app_api_busy(void);

/* True while a file write is putting data on the SPI flash. */
bool app_api_flash_busy(void);

/* Closes whatever the app left open (on the way out). */
void app_api_close_files(void);

#endif
