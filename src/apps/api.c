/*
 * The native-app api table (e16_api.h) over the SDK drivers: api.h.
 *
 * Every call that touches shared state (MIDI, the log, files, the synth,
 * the console's virtual controls) runs between ENTER() and LEAVE(), so
 * app_mode.c's PendSV service never runs in the middle of one; slow calls
 * (present, delay, midi_read) run that service themselves. An exit asked
 * for from the SysTick (the leave prompt) while the app is in here happens on
 * LEAVE().
 *
 * Errors are negative, e16_api.h's E16_E*.
 *
 * An app's pointers may be CCM or 0x0000xxxx (its text, run from SRAM1's
 * alias) addresses, neither of which DMA can read: every function here
 * copies what it's given before anything DMAs it (e16_api.h, "Pointers").
 *
 * No function here writes the internal flash: the files are on the SPI
 * flash's FAT volume (src/disk), and writes go only under the app's own
 * folder, /apps/<name>/, through app_files.c (which also decides when a
 * write has to wait: E16_EBUSY).
 */
#include "api.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "display.h"
#include "enc.h"
#include "leds.h"
#include "system.h"

#include "app_files.h"
#include "app_mode.h"
#include "exit_prompt.h"
#include "console.h"
#include "e16_app_format.h"
#include "ff.h"
#include "log.h"
#include "mirror.h"
#include "noise_lab.h"
#include "port_e16_audio.h"
#include "app_stream.h"
#include "rt_port.h"
#include "volume.h"

#define API_EARGS E16_EARGS
#define API_EIO E16_EIO
#define API_EFULL E16_EFULL

/* The brightest channel value an app can drive: the stock firmware's own
 * ceiling (leds.h: all 256 LEDs at full white is untested on the E16's
 * supply). */
#define LED_LIMIT 85u

#define MIDI_QUEUE 64u      /* power of two */
#define SYSEX_IN_QUEUE 0xFF /* a queue entry standing for the SysEx slot */
#define FILE_NAME_MAX 32u   /* file_write()'s name */
#define LOG_MAX 120         /* bytes of an app's log line kept */

typedef struct {
    uint8_t port;
    uint8_t len; /* 1-3, or SYSEX_IN_QUEUE */
    uint8_t data[3];
} short_msg_t;

typedef struct {
    uint8_t fb[E16_FB_SIZE];
    uint32_t present_ms;           /* when the app last presented a frame */
    uint8_t shown[E16_FB_SIZE];    /* fb as last presented: the screen dump's and the mirror's */
    FIL files[APP_FILES_MAX];
    bool open[APP_FILES_MAX];
    short_msg_t queue[MIDI_QUEUE];
    uint32_t qhead, qtail;
    uint8_t sysex[MIDI_SYSEX_MAX]; /* one SysEx waiting (len 0 = none) */
    uint32_t sysex_len;
    uint8_t out[MIDI_SYSEX_MAX];   /* what the last midi_read() returned */
    char name[E16APP_NAME_MAX];
    bool name_ok;                  /* usable as a folder name */
    app_stream_t stream;           /* audio_start()'s (port_e16_audio.c) */
} api_ram_t;

static api_ram_t *R;
static uint32_t g_r9; /* the app's GOT */
static volatile uint32_t g_depth;      /* api calls in progress (0 or 1: nothing nests) */
static volatile bool g_flash_busy;

#define ENTER() (g_depth++)
#define LEAVE()                                                                                    \
    do {                                                                                           \
        if (--g_depth == 0) {                                                                      \
            app_mode_check_exit();                                                                 \
        }                                                                                          \
    } while (0)

bool app_api_busy(void)
{
    return g_depth != 0;
}

bool app_api_flash_busy(void)
{
    return g_flash_busy;
}

size_t app_api_ram_size(void)
{
    return (sizeof(api_ram_t) + 7u) & ~(size_t)7u;
}

const uint8_t *app_api_framebuffer(void)
{
    return R ? R->shown : NULL;
}

uint32_t app_api_last_present(void)
{
    return R ? R->present_ms : 0;
}

void app_api_redraw(void)
{
    if (!R) {
        return;
    }
    memcpy(R->shown, R->fb, sizeof R->shown);
    app_mode_draw_overlay(R->shown);
    for (int page = 0; page < DISPLAY_PAGES; page++) {
        display_write(page, 0, R->shown + page * DISPLAY_WIDTH, DISPLAY_WIDTH);
    }
}

/* ---- display ---- */

static uint8_t *api_display_fb(void)
{
    return R->fb;
}

static void api_display_present(void)
{
    ENTER();
    /* A copy first: the app may draw into fb again at any time, and the
     * screen dump and the mirror read what the OLED has. */
    memcpy(R->shown, R->fb, sizeof R->shown);
    app_mode_draw_overlay(R->shown);
    R->present_ms = system_millis();
    for (int page = 0; page < DISPLAY_PAGES; page++) {
        display_write(page, 0, R->shown + page * DISPLAY_WIDTH, DISPLAY_WIDTH);
    }
    app_mode_service();
    LEAVE();
}

/* ---- LEDs ---- */

static uint8_t led_level(uint8_t v)
{
    return noise_lab_led(v > LED_LIMIT ? (uint8_t)LED_LIMIT : v);
}

/* Guarded like the rest: app_mode_end() clears the LEDs on the way out,
 * and must not do that from PendSV in the middle of one of these. */
static void api_leds_set(int ring, int led, uint8_t r, uint8_t g, uint8_t b)
{
    if (ring < 0 || ring >= LEDS_RINGS || led < 0 || led >= LEDS_PER_RING) {
        return;
    }
    ENTER();
    leds_set(ring, led, led_level(r), led_level(g), led_level(b));
    LEAVE();
}

static int api_leds_show(void)
{
    ENTER();
    bool ok = noise_lab_leds_off() || leds_show(); /* the noise lab keeps them dark */
    LEAVE();
    return ok ? 1 : 0;
}

/* ---- encoders (physical and the console's virtual ones merged) ---- */

static int api_enc_read(int index)
{
    if (index < 0 || index >= ENC_COUNT) {
        return 0;
    }
    ENTER();
    int hw = enc_read(index);
    mirror_turn(index, hw, false); /* the console reports virtual turns itself */
    int steps = hw + console_virtual_take_steps(index);
    if (steps != 0) {
        app_mode_turned();
    }
    if (app_mode_input_blocked()) {
        steps = 0; /* the leave prompt is up: turns are dropped, not saved up */
    }
    LEAVE();
    return steps;
}

static int api_enc_pressed(int index)
{
    if (index < 0 || index >= ENC_COUNT) {
        return 0;
    }
    ENTER();
    bool down = (enc_pressed(index) || console_virtual_down(index)) && !app_mode_input_blocked();
    LEAVE();
    return down ? 1 : 0;
}

static int api_shift_pressed(void)
{
    ENTER();
    bool down = (enc_shift_pressed() || console_virtual_down(16)) && !app_mode_input_blocked();
    LEAVE();
    return down ? 1 : 0;
}

/* ---- MIDI ---- */

static int api_midi_write(int port, const uint8_t *bytes, uint32_t len)
{
    if (port < E16_MIDI_TRS_1 || port > E16_MIDI_USB || port == E16_MIDI_TRS_IN || !bytes || len == 0) {
        return 0;
    }
    /* TRS 2 carries the synth while an app plays it: no MIDI bytes on
     * the audio pin. */
    if (port == E16_MIDI_TRS_2 && !port_e16_audio_trs2_midi()) {
        return 0;
    }
    ENTER();
    bool ok = midi_write((midi_port_t)port, bytes, len);
    LEAVE();
    return ok ? 1 : 0;
}

/* From app_mode_service(), which never runs during an api call. */
void app_api_midi_in(const midi_msg_t *msg)
{
    if (!R || msg->len == 0) {
        return;
    }
    if (R->qhead - R->qtail >= MIDI_QUEUE) {
        return; /* full: dropped */
    }
    short_msg_t *q = &R->queue[R->qhead & (MIDI_QUEUE - 1u)];
    q->port = (uint8_t)msg->port;
    if (msg->len <= 3) {
        q->len = (uint8_t)msg->len;
        memcpy(q->data, msg->data, msg->len);
    } else {
        if (R->sysex_len != 0 || msg->len > sizeof R->sysex) {
            return; /* one SysEx at a time: dropped */
        }
        memcpy(R->sysex, msg->data, msg->len);
        R->sysex_len = msg->len;
        q->len = SYSEX_IN_QUEUE;
    }
    R->qhead++;
}

static int api_midi_read(e16_midi_msg_t *msg)
{
    if (!msg) {
        return 0;
    }
    ENTER();
    app_mode_service();
    int got = 0;
    if (R->qtail != R->qhead) {
        const short_msg_t *q = &R->queue[R->qtail & (MIDI_QUEUE - 1u)];
        msg->port = q->port;
        if (q->len == SYSEX_IN_QUEUE) {
            memcpy(R->out, R->sysex, R->sysex_len);
            msg->len = R->sysex_len;
            R->sysex_len = 0;
        } else {
            memcpy(R->out, q->data, q->len);
            msg->len = q->len;
        }
        msg->data = R->out;
        R->qtail++;
        got = 1;
    }
    LEAVE();
    return got;
}

/* ---- time ---- */

static uint32_t api_millis(void)
{
    return system_millis();
}

static void api_delay_ms(uint32_t ms)
{
    ENTER();
    uint32_t t0 = system_millis();
    while (system_millis() - t0 < ms) {
        app_mode_service();
        app_mode_check_exit(); /* an exit asked for meanwhile: now, not after the wait */
    }
    LEAVE();
}

/* ---- files ---- */

static bool fd_ok(int fd)
{
    return fd >= 0 && fd < APP_FILES_MAX && R->open[fd];
}

static int api_file_open(const char *path, int flags)
{
    if (!path || path[0] != '/' || flags != E16_FILE_READ) {
        return API_EARGS;
    }
    ENTER();
    int fd = API_EFULL;
    for (int i = 0; i < APP_FILES_MAX; i++) {
        if (!R->open[i]) {
            fd = f_open(&R->files[i], path, FA_READ) == FR_OK ? i : API_EIO;
            if (fd >= 0) {
                R->open[i] = true;
            }
            break;
        }
    }
    LEAVE();
    return fd;
}

static int api_file_read(int fd, void *buf, uint32_t len)
{
    if (!buf || !fd_ok(fd)) {
        return API_EARGS;
    }
    ENTER();
    UINT n = 0;
    FRESULT r = f_read(&R->files[fd], buf, (UINT)len, &n);
    LEAVE();
    return r == FR_OK ? (int)n : API_EIO;
}

static int api_file_seek(int fd, uint32_t pos)
{
    if (!fd_ok(fd)) {
        return API_EARGS;
    }
    ENTER();
    FRESULT r = f_lseek(&R->files[fd], (FSIZE_t)pos);
    LEAVE();
    return r == FR_OK ? 0 : API_EIO;
}

static void api_file_close(int fd)
{
    if (!fd_ok(fd)) {
        return;
    }
    ENTER();
    f_close(&R->files[fd]);
    R->open[fd] = false;
    LEAVE();
}

void app_api_close_files(void)
{
    if (!R) {
        return;
    }
    for (int i = 0; i < APP_FILES_MAX; i++) {
        if (R->open[i]) {
            f_close(&R->files[i]);
            R->open[i] = false;
        }
    }
}

/* A plain file or folder name: 1-FILE_NAME_MAX of [A-Za-z0-9_.-], not
 * starting with '.' (so no "." or "..", and nothing hidden). */
static bool plain_name(const char *s)
{
    size_t n = 0;
    for (; s[n]; n++) {
        char c = s[n];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                  c == '-' || c == '.';
        if (!ok || n >= FILE_NAME_MAX) {
            return false;
        }
    }
    return n > 0 && s[0] != '.';
}

static int api_file_write(const char *name, const void *buf, uint32_t len)
{
    if (!name || (!buf && len) || !R->name_ok || !plain_name(name)) {
        return API_EARGS;
    }
    char dir[sizeof VOLUME_APPS_DIR + E16APP_NAME_MAX];
    char path[sizeof dir + FILE_NAME_MAX + 1];
    snprintf(dir, sizeof dir, "%s/%s", VOLUME_APPS_DIR, R->name);
    snprintf(path, sizeof path, "%s/%s", dir, name);
    ENTER();
    g_flash_busy = true;
    int r = app_files_write(dir, path, buf, len);
    g_flash_busy = false;
    LEAVE();
    return r;
}

/* ---- log ---- */

static void api_log(const char *msg)
{
    if (!msg) {
        return;
    }
    ENTER();
    log_printf("app %s: %.*s", R->name, LOG_MAX, msg);
    LEAVE();
}

/* ---- synth ---- */

static void api_synth_note(uint8_t note, uint8_t velocity)
{
    ENTER();
    rt_port_audio_note(note & 0x7F, velocity & 0x7F);
    LEAVE();
}

static void api_synth_param(int id, uint8_t value)
{
    ENTER();
    port_e16_audio_voice_param(id, value > 127 ? 127 : value);
    LEAVE();
}

/* ---- audio stream ---- */

/* True when called from an interrupt (the render callback itself): the
 * stream can't be started or stopped from inside its own callback. */
static bool in_interrupt(void)
{
    return __get_IPSR() != 0u;
}

static int api_audio_start(e16_audio_render_t render, void *user)
{
    if (!render || in_interrupt()) {
        return API_EARGS;
    }
    ENTER();
    port_e16_audio_app_start(&R->stream, render, user, g_r9);
    LEAVE();
    return 0;
}

static void api_audio_stop(void)
{
    if (in_interrupt()) {
        return;
    }
    ENTER();
    port_e16_audio_app_stop();
    port_e16_audio_poll(system_millis());
    LEAVE();
}

static int api_audio_info(e16_audio_info_t *info, uint32_t size)
{
    if (!info) {
        return API_EARGS;
    }
    port_e16_audio_app_info_t a;
    port_e16_audio_app_info(&a);
    e16_audio_info_t i = {
        .rate = a.rate,
        .block = a.block,
        .state = a.state == APP_STREAM_RUNNING        ? E16_AUDIO_RUNNING
                 : a.state == APP_STREAM_STOP_OVERLOAD ? E16_AUDIO_OVERLOAD
                                                       : E16_AUDIO_OFF,
        .load = a.load_permille,
        .load_peak = a.peak_permille,
        .late = a.late,
        .calls = a.calls,
    };
    memcpy(info, &i, size < sizeof i ? size : sizeof i);
    return 0;
}

_Static_assert(E16_AUDIO_BLOCK == APP_STREAM_BLOCK && E16_AUDIO_RATE == 48000u, "e16_api.h's audio constants");

/* ---- leaving ---- */

__attribute__((noreturn)) static void api_exit(int code)
{
    app_mode_end(APP_END_EXIT, code);
}

static int api_exit_pending(void)
{
    return app_mode_exit_pending() ? 1 : 0;
}

static const e16_api_t g_api = {
    .abi_version = E16_API_VERSION,
    .size = sizeof(e16_api_t),
    .display_fb = api_display_fb,
    .display_present = api_display_present,
    .leds_set = api_leds_set,
    .leds_show = api_leds_show,
    .enc_read = api_enc_read,
    .enc_pressed = api_enc_pressed,
    .shift_pressed = api_shift_pressed,
    .midi_write = api_midi_write,
    .midi_read = api_midi_read,
    .millis = api_millis,
    .delay_ms = api_delay_ms,
    .file_open = api_file_open,
    .file_read = api_file_read,
    .file_seek = api_file_seek,
    .file_close = api_file_close,
    .file_write = api_file_write,
    .log = api_log,
    .synth_note = api_synth_note,
    .synth_param = api_synth_param,
    .exit = api_exit,
    .audio_start = api_audio_start,
    .audio_stop = api_audio_stop,
    .audio_info = api_audio_info,
    .exit_pending = api_exit_pending,
};

const e16_api_t *app_api_init(void *ram, const char *name, uint32_t r9)
{
    g_r9 = r9;
    R = ram;
    memset(R, 0, sizeof *R);
    strncpy(R->name, name, sizeof R->name - 1);
    R->name_ok = plain_name(R->name);
    return &g_api;
}
