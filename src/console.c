#include <stdint.h>
#include <string.h>

#include "console.h"
#include "midi.h"
#include "system.h"
#include "log.h"
#include "screen.h"
#include "board.h" /* NVIC_SystemReset(), for the dev "reset" SysEx below */
#include "persist.h"
#include "launcher.h"
#include "scripts.h"
#include "port_e16_audio.h"
#include "app_mode.h"
#include "noise_lab.h"
#include "rt_port.h"
#include "mirror.h"

#ifndef E16_DEV_CONSOLE
#define E16_DEV_CONSOLE 1
#endif

/* The virtual controls (0x10) are the dev console's and the mirror's
 * remote control (mirror.h): built in with either. */
#define VIRTUAL_CONTROLS (E16_DEV_CONSOLE || E16_MIRROR)

#define CHUNK_MAX 120
/* log_take()'s longest possible payload (a maximally truncated line, see
 * log.c's LINE_MAX) plus the '\n' this transport adds back. */
#define LINE_BUF_SIZE 2048

static bool g_vdown[17];
static uint32_t g_vdown_t0[17];  /* system_millis() a still-down index went down */
static int g_vsteps[16];
static int g_dump_next = -1;   /* next dump message (0-15), -1 = none */
static const uint8_t *g_app_fb; /* app mode: dump this instead of the screen */

/* A virtual "down" with no matching "up" (a lost SysEx, a console.py
 * process that got killed mid-gesture, ...) auto-releases after this long,
 * so it can't latch forever -- in particular, it can't hold the Shift +
 * encoder-16 bootloader gesture open indefinitely. */
#define VIRTUAL_PRESS_TIMEOUT_MS 3000u

bool console_virtual_down(int index)
{
    if (index < 0 || index > 16) {
        return false;
    }
    if (g_vdown[index] && system_millis() - g_vdown_t0[index] >= VIRTUAL_PRESS_TIMEOUT_MS) {
        g_vdown[index] = false;
        mirror_button(index, false, true);
        log_printf("console: virtual %d auto-released after %u ms", index, VIRTUAL_PRESS_TIMEOUT_MS);
    }
    return g_vdown[index];
}

bool console_virtual_held(int index)
{
    return index >= 0 && index <= 16 && g_vdown[index];
}

void console_app_mode(const uint8_t *fb)
{
    g_app_fb = fb;
}

int console_virtual_take_steps(int index)
{
    if (index < 0 || index >= 16) {
        return 0;
    }
    int n = g_vsteps[index];
    g_vsteps[index] = 0;
    return n;
}

#if E16_DEV_CONSOLE
/* Audio-activity proxy: TIM8's compare register (the PWM duty on TRS 2)
 * sampled 50 times over 100 ms, then logged as min/max/distinct values. A
 * running synth changes it at audio rate; MIDI or silence doesn't. Sampled
 * from the main loop (console_drain_log()), one reading every 2 ms, so the
 * script -- a sequencer playing the synth -- keeps running meanwhile. */
#define PWM_SAMPLES 50
#define PWM_EVERY_MS 2u
static int g_pwm_left;          /* samples still to take, 0 = idle */
static uint32_t g_pwm_next;
static uint32_t g_pwm_seen[256 / 32];
static uint32_t g_pwm_lo, g_pwm_hi;
static int g_pwm_distinct;

static void sample_pwm_start(void)
{
    memset(g_pwm_seen, 0, sizeof g_pwm_seen);
    g_pwm_lo = UINT32_MAX;
    g_pwm_hi = 0;
    g_pwm_distinct = 0;
    g_pwm_left = PWM_SAMPLES;
    g_pwm_next = system_millis();
}

static void sample_pwm_poll(void)
{
    uint32_t now = system_millis();
    if (g_pwm_left == 0 || (int32_t)(now - g_pwm_next) < 0) {
        return;
    }
    g_pwm_next += PWM_EVERY_MS;
    uint32_t v = port_e16_audio_pwm_value() & 0xFF;
    g_pwm_lo = v < g_pwm_lo ? v : g_pwm_lo;
    g_pwm_hi = v > g_pwm_hi ? v : g_pwm_hi;
    if (!(g_pwm_seen[v / 32] & (1u << (v % 32)))) {
        g_pwm_seen[v / 32] |= 1u << (v % 32);
        g_pwm_distinct++;
    }
    if (--g_pwm_left == 0) {
        log_printf("console: pwm min %lu max %lu distinct %d (%d samples / %u ms), TRS 2 %s",
                   (unsigned long)g_pwm_lo, (unsigned long)g_pwm_hi, g_pwm_distinct, PWM_SAMPLES,
                   (unsigned)(PWM_SAMPLES * PWM_EVERY_MS), port_e16_audio_on_trs2() ? "synth" : "MIDI");
    }
}
#endif

#if E16_DEV_CONSOLE
/* The synth from the console (SysEx 13): a test tone without a script.
 * op 0 = note a velocity b (0 = off), 1 = parameter a (rt_audio_param_t)
 * value b, 2 = output a (rt_audio_out_t), 3 = enable b (0/1). The next
 * script load (or unload) releases it like any script's. */
static void synth_command(int op, int a, int b)
{
    switch (op) {
        case 0:
            rt_port_audio_note(a, b);
            break;
        case 1:
            if (a < RT_AUDIO_PARAMS) {
                rt_port_audio_set((rt_audio_param_t)a, b);
            }
            break;
        case 2:
            if (a <= RT_AUDIO_OUT_BOTH) {
                rt_port_audio_output((rt_audio_out_t)a);
            }
            break;
        case 3:
            rt_port_audio_enable(b != 0);
            break;
        default:
            return;
    }
    log_printf("console: synth op %d %d %d", op, a, b);
}
#endif

static bool is_console(const midi_msg_t *msg, size_t len)
{
    return msg->port == MIDI_USB && msg->len == len && msg->data[0] == 0xF0 &&
           msg->data[1] == 0x7D && msg->data[2] == 0x45 && msg->data[len - 1] == 0xF7;
}

/*
 * SysEx F0 7D 45 05 F7 reboots into the bootloader (tools/reflash.sh). SysEx F0 7D 45 61 F7 asks for the log's retained history to be
 * resent (tools/console.py sends this right after opening the port, so a
 * console that opens after the boot chunk has already printed still sees
 * its output -- MIDI delivers nothing sent before a listener attaches).
 * These two are always built in, dev console or not: 0x05 is the only way
 * off the device without a debugger, and 0x61 is just a log resend.
 *
 * SysEx F0 7D 45 06 F7 (dev aid) reboots back into the launcher
 * itself -- a plain NVIC_SystemReset(), unlike 0x05's reboot into the
 * bootloader -- so a hardware check can emulate a power cycle without
 * physically unplugging the device (persisted vars/store load from flash
 * the same as a real power-on would). It and the other development
 * messages (virtual controls, framebuffer dump, corrupt-record), described
 * in console.h, are gated behind the E16_DEV_CONSOLE build option (CMake,
 * default ON -- release builds should turn it OFF): with it off, this
 * function ignores those commands entirely, so a stray or malicious SysEx
 * can't latch a virtual control or dump the screen on a shipped device.
 */
bool console_handle(const midi_msg_t *msg)
{
    if (mirror_handle(msg)) {
        return true;
    }
#if E16_DEV_CONSOLE
    if (is_console(msg, 6) && msg->data[3] == 0x07) {
        if (g_app_fb) {
            return true; /* app mode: persistence isn't up */
        }
        int idx = msg->data[4];
        if (idx < 0 || idx >= scene_count) {
            log_printf("persist: corrupt: no scene %d (0-%d)", idx, scene_count - 1);
        } else {
            persist_debug_corrupt_vars(scenes[idx].name);
        }
        return true;
    }
    if (is_console(msg, 8) && msg->data[3] == 0x13) {
        synth_command(msg->data[4], msg->data[5], msg->data[6]);
        return true;
    }
    if (is_console(msg, 8) && msg->data[3] == 0x14) {
        noise_lab_command((noise_cmd_t)msg->data[4], ((unsigned)msg->data[5] << 7) | msg->data[6]);
        return true;
    }
#endif
#if VIRTUAL_CONTROLS
    if (is_console(msg, 8) && msg->data[3] == 0x10) {
        int ctl = msg->data[4], act = msg->data[5], n = msg->data[6];
        if (ctl > 16) {
            return true;
        }
        if (act == 0 || act == 1) {
            if (g_vdown[ctl] != (act == 1)) {
                mirror_button(ctl, act == 1, true);
            }
            g_vdown[ctl] = (act == 1);
            g_vdown_t0[ctl] = system_millis(); /* a repeated down keeps it held */
        } else if (act == 2 && ctl < 16) {
            g_vsteps[ctl] += n - 64;
            mirror_turn(ctl, n - 64, true);
        }
        return true;
    }
#endif
    if (!is_console(msg, 5)) {
        return false;
    }
    if (msg->data[3] == 0x05) {
        /* Saves a running script's vars/store first: once the bootloader
         * runs there's no coming back to finish that short of a reflash.
         * App mode has nothing to save. */
        if (g_app_fb) {
            log_printf("app: bootloader (SysEx)");
            for (int i = 0; i < 50; i++) {
                console_drain_log();
                system_delay_ms(2);
            }
            system_enter_bootloader();
        }
        launcher_reboot_to_bootloader("SysEx");
#if E16_DEV_CONSOLE
    } else if (msg->data[3] == 0x06) {
        app_mode_note_console_reset(); /* only in app mode */
        system_delay_ms(20); /* let this message's SysEx F7 finish going out */
        NVIC_SystemReset();
#endif
    } else if (msg->data[3] == 0x61) {
        log_replay();
#if E16_DEV_CONSOLE
    } else if (msg->data[3] == 0x11) {
        g_dump_next = 0;
    } else if (msg->data[3] == 0x12) {
        sample_pwm_start();
#endif
    } else {
        return false;
    }
    return true;
}


/* One framebuffer dump message per call while a dump is pending; false
 * when the USB buffer is full (retried next pass). */
static bool send_dump(void)
{
    uint8_t msg[6 + 128 + 1];
    int page = g_dump_next / 2, half = g_dump_next % 2;
    const uint8_t *src = (g_app_fb ? g_app_fb : screen_framebuffer()) + page * 128 + half * 64;
    msg[0] = 0xF0;
    msg[1] = 0x7D;
    msg[2] = 0x45;
    msg[3] = 0x62;
    msg[4] = (uint8_t)page;
    msg[5] = (uint8_t)half;
    for (int i = 0; i < 64; i++) {
        msg[6 + 2 * i] = src[i] >> 4;
        msg[7 + 2 * i] = src[i] & 0x0F;
    }
    msg[sizeof msg - 1] = 0xF7;
    if (!midi_write(MIDI_USB, msg, sizeof msg)) {
        return false;
    }
    if (++g_dump_next == 16) {
        g_dump_next = -1;
    }
    return true;
}

static int pending; /* console_drain_log(): a line is partly sent */
static uint32_t g_usb_since; /* system_millis() | 1 when USB came up, 0 while down */
static bool g_usb_seen;      /* USB has been up at some point this boot */

static void watch_usb(void)
{
    if (!midi_usb_connected()) {
        g_usb_since = 0;
    } else if (g_usb_since == 0) {
        g_usb_since = system_millis() | 1u;
        g_usb_seen = true;
    }
}

bool console_usb_seen(void)
{
    watch_usb();
    return g_usb_seen;
}

uint32_t console_usb_up_ms(void)
{
    watch_usb();
    return g_usb_since ? system_millis() - g_usb_since : 0;
}

bool console_log_sent(void)
{
    return !pending && log_unsent() == 0;
}

/*
 * Drain the console log to USB as SysEx F0 7D 45 60 <text> F7. log_take()
 * strips each line's trailing '\n'; this transport adds it back before
 * chunking, so tools/console.py can just concatenate the payloads of
 * consecutive 0x60 messages and split lines on '\n' itself -- a single
 * line sent as several SysEx messages still arrives as one line.
 *
 * A line is only removed from the log once it's fully handed to
 * midi_write(): while no computer is connected (or its 256-byte buffer is
 * full), it stays queued here instead of being silently dropped.
 */
void console_drain_log(void)
{
    /* SRAM, not CCM: app mode drains the log too (mem_layout.h). */
    static char line[LINE_BUF_SIZE];
    static size_t len, off;

    watch_usb();
#if E16_DEV_CONSOLE
    sample_pwm_poll();
#endif
    if (g_dump_next >= 0 && midi_usb_connected()) {
        send_dump();
    }
    if (!pending) {
        if (noise_lab_usb_quiet()) {
            return; /* the noise lab holds the log back (it keeps in the log ring) */
        }
        size_t n = log_take(line, sizeof(line) - 1);
        if (n == 0) {
            return;
        }
        line[n] = '\n';
        len = n + 1;
        off = 0;
        pending = 1;
    }
    if (!midi_usb_connected()) {
        return;
    }
    uint8_t msg[4 + CHUNK_MAX + 1];
    while (off < len) {
        size_t chunk = len - off;
        if (chunk > CHUNK_MAX) {
            chunk = CHUNK_MAX;
        }
        msg[0] = 0xF0;
        msg[1] = 0x7D;
        msg[2] = 0x45;
        msg[3] = 0x60;
        for (size_t i = 0; i < chunk; i++) {
            msg[4 + i] = (uint8_t)line[off + i];
        }
        msg[4 + chunk] = 0xF7;
        if (!midi_write(MIDI_USB, msg, 4 + chunk + 1)) {
            return; /* buffer full; retry this chunk next time */
        }
        off += chunk;
    }
    pending = 0;
}
