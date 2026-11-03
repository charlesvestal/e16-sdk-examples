/*
 * The noise lab: noise_lab.h.
 */
#include "noise_lab.h"

#include <string.h>

#include "display.h"
#include "leds.h"
#include "system.h"

#include "app_mode.h"
#include "boot_records.h"
#include "log.h"
#include "port_e16.h"

/* noise_record_t.set */
#define NOISE_SET_CONTRAST 0x01u
#define NOISE_SET_CLOCK 0x02u
#define NOISE_SET_PRECHARGE 0x04u
#define NOISE_SET_VCOMH 0x08u
/* noise_record_t.flags */
#define NOISE_F_OLED_OFF 0x01u
#define NOISE_F_LEDS_OFF 0x02u
#define NOISE_F_USB_QUIET 0x04u

#define g_rec (BOOT_RECORDS->noise)

static uint32_t record_check(const noise_record_t *r)
{
    uint32_t w[2];
    memcpy(w, &r->set, sizeof w);
    return NOISE_RECORD_MAGIC ^ w[0] ^ w[1] ^ 0x5A5A5A5Au;
}

static bool record_valid(void)
{
    return g_rec.magic == NOISE_RECORD_MAGIC && g_rec.check == record_check(&g_rec);
}

/* The record, made valid (all defaults) if it wasn't. */
static noise_record_t *record(void)
{
    if (!record_valid()) {
        memset(&g_rec, 0, sizeof g_rec);
        g_rec.led_level = 255;
        g_rec.magic = NOISE_RECORD_MAGIC;
    }
    return &g_rec;
}

static void record_seal(void)
{
    g_rec.check = record_check(&g_rec);
}

static void cmd2(uint8_t c, uint8_t v)
{
    const uint8_t b[2] = {c, v};
    display_command(b, sizeof b);
}

static void cmd1(uint8_t c)
{
    display_command(&c, 1);
}

void noise_lab_apply_display(void)
{
    if (!record_valid()) {
        return;
    }
    if (g_rec.set & NOISE_SET_CONTRAST) {
        display_contrast(g_rec.contrast);
    }
    if (g_rec.set & NOISE_SET_CLOCK) {
        cmd2(0xD5, g_rec.clock);
    }
    if (g_rec.set & NOISE_SET_PRECHARGE) {
        cmd2(0xD9, g_rec.precharge);
    }
    if (g_rec.set & NOISE_SET_VCOMH) {
        cmd2(0xDB, g_rec.vcomh);
    }
    if (g_rec.flags & NOISE_F_OLED_OFF) {
        cmd1(0xAE);
    }
    if (g_rec.set || g_rec.flags || g_rec.led_level != 255) {
        log_printf("noise: settings kept from before the reset (console.py noise status; noise reset)");
    }
}

uint8_t noise_lab_led(uint8_t level)
{
    if (!record_valid() || g_rec.led_level == 255) {
        return level;
    }
    return (uint8_t)(((unsigned)level * g_rec.led_level + 127u) / 255u);
}

bool noise_lab_leds_off(void)
{
    return record_valid() && (g_rec.flags & NOISE_F_LEDS_OFF);
}

bool noise_lab_usb_quiet(void)
{
    return record_valid() && (g_rec.flags & NOISE_F_USB_QUIET);
}

/* All LEDs dark now: the frame goes out once the driver is free. */
static void leds_dark(void)
{
    leds_clear();
    uint32_t t0 = system_millis();
    while (!leds_show() && system_millis() - t0 < 10u) {
    }
}

/* The LEDs as the launcher (not an app: it redraws them itself) last drew them. */
static void leds_redraw(void)
{
    if (!app_mode_active()) {
        port_e16_leds_redraw();
    }
}

void noise_lab_command(noise_cmd_t cmd, unsigned value)
{
    noise_record_t *r = record();
    uint8_t v = (uint8_t)(value > 255u ? 255u : value);
    switch (cmd) {
        case NOISE_STATUS:
            break;
        case NOISE_OLED_CONTRAST:
            r->contrast = v;
            r->set |= NOISE_SET_CONTRAST;
            display_contrast(v);
            break;
        case NOISE_OLED_OFF:
            r->flags |= NOISE_F_OLED_OFF;
            cmd1(0xAE);
            break;
        case NOISE_OLED_ON:
            r->flags &= (uint8_t)~NOISE_F_OLED_OFF;
            cmd1(0xAF);
            break;
        case NOISE_OLED_CLOCK:
            r->clock = v;
            r->set |= NOISE_SET_CLOCK;
            cmd2(0xD5, v);
            break;
        case NOISE_OLED_PRECHARGE:
            r->precharge = v;
            r->set |= NOISE_SET_PRECHARGE;
            cmd2(0xD9, v);
            break;
        case NOISE_OLED_VCOMH:
            r->vcomh = v;
            r->set |= NOISE_SET_VCOMH;
            cmd2(0xDB, v);
            break;
        case NOISE_LEDS_OFF:
            r->flags |= NOISE_F_LEDS_OFF;
            leds_dark();
            break;
        case NOISE_LEDS_ON:
            r->flags &= (uint8_t)~NOISE_F_LEDS_OFF;
            record_seal();
            leds_redraw();
            break;
        case NOISE_LED_BRIGHTNESS:
            r->led_level = v;
            record_seal();
            leds_redraw();
            break;
        case NOISE_USB_QUIET:
            r->flags |= NOISE_F_USB_QUIET;
            break;
        case NOISE_USB_LOUD:
            r->flags &= (uint8_t)~NOISE_F_USB_QUIET;
            break;
        case NOISE_RESET:
            memset(r, 0, sizeof *r);
            r->led_level = 255;
            r->magic = NOISE_RECORD_MAGIC;
            record_seal();
            /* The SDK's display_init() values (bsp/src/display.c). */
            display_contrast(143);
            cmd2(0xD5, 0xF0);
            cmd2(0xD9, 0x22);
            cmd2(0xDB, 0x35);
            cmd1(0xAF);
            leds_redraw();
            break;
        default:
            log_printf("noise: unknown command %d", (int)cmd);
            return;
    }
    record_seal();
    log_printf("noise: oled contrast %s%u clock %s0x%02X precharge %s0x%02X vcomh %s0x%02X %s, leds %s level %u, "
               "usb %s",
               (r->set & NOISE_SET_CONTRAST) ? "" : "default ", (r->set & NOISE_SET_CONTRAST) ? r->contrast : 143u,
               (r->set & NOISE_SET_CLOCK) ? "" : "default ", (r->set & NOISE_SET_CLOCK) ? r->clock : 0xF0u,
               (r->set & NOISE_SET_PRECHARGE) ? "" : "default ",
               (r->set & NOISE_SET_PRECHARGE) ? r->precharge : 0x22u, (r->set & NOISE_SET_VCOMH) ? "" : "default ",
               (r->set & NOISE_SET_VCOMH) ? r->vcomh : 0x35u, (r->flags & NOISE_F_OLED_OFF) ? "off" : "on",
               (r->flags & NOISE_F_LEDS_OFF) ? "off" : "on", (unsigned)r->led_level,
               (r->flags & NOISE_F_USB_QUIET) ? "quiet (log held)" : "normal");
}
