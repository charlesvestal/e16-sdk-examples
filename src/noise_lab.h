#ifndef NOISE_LAB_H
#define NOISE_LAB_H
#include <stdbool.h>
#include <stdint.h>

/*
 * The noise lab: dev-console switches for finding what puts the whine on
 * TRS 2's analog audio (it changes with what the OLED shows). Each one
 * changes a single suspect live, so a listener can hear which one
 * matters (tools/console.py noise ...; the SysEx is in console.h):
 *
 *   OLED   contrast (0x81), display off/on (0xAE/0xAF), clock divide and
 *          oscillator (0xD5), pre-charge period (0xD9), VCOMH level (0xDB)
 *   LEDs   off (cleared, and no more frames sent) / on, brightness 0-255
 *          (a scale on every level the launcher or an app drives)
 *   USB    quiet: the log stops going out (held until "loud")
 *
 * The settings live in a reset-surviving record (boot_records.h), so they
 * last through a reset into or out of app mode (and a reflash), until a
 * power cycle or "noise reset"; the display ones are sent again after
 * every display_init(). Without a valid record everything is the SDK's
 * and the launcher's defaults.
 */

typedef enum {
    NOISE_STATUS = 0,     /* log the current settings */
    NOISE_OLED_CONTRAST,  /* value 0-255 */
    NOISE_OLED_OFF,
    NOISE_OLED_ON,
    NOISE_OLED_CLOCK,     /* value: 0xD5's byte (high nibble oscillator, low divide - 1) */
    NOISE_OLED_PRECHARGE, /* value: 0xD9's byte (high nibble phase 2, low phase 1) */
    NOISE_OLED_VCOMH,     /* value: 0xDB's byte (0x00, 0x20, 0x30 typical) */
    NOISE_LEDS_OFF,
    NOISE_LEDS_ON,
    NOISE_LED_BRIGHTNESS, /* value 0-255, 255 = as drawn */
    NOISE_USB_QUIET,
    NOISE_USB_LOUD,
    NOISE_RESET,          /* everything back to the defaults */
    NOISE_COMMANDS
} noise_cmd_t;

/* A console command (value 0-255 where it takes one). Logs what it did. */
void noise_lab_command(noise_cmd_t cmd, unsigned value);

/* Right after every display_init(): sends the display settings again. */
void noise_lab_apply_display(void);

/* An LED level (0-255) as the noise lab's brightness scales it. */
uint8_t noise_lab_led(uint8_t level);
/* True while the LEDs are switched off (no frames are sent). */
bool noise_lab_leds_off(void);
/* True while the log is held back from USB. */
bool noise_lab_usb_quiet(void);

#endif
