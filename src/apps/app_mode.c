/*
 * Clean-boot app mode: app_mode.h.
 */
#include "app_mode.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "display.h"
#include "enc.h"
#include "leds.h"
#include "midi.h"
#include "storage.h"
#include "system.h"

#include "api.h"
#include "app_boot.h"
#include "app_files.h"
#include "blockdev.h"
#include "console.h"
#include "disk_mode.h"
#include "ff.h"
#include "flash_budget.h"
#include "flash_e16.h"
#include "loader.h"
#include "mirror.h"
#include "log.h"
#include "mem_layout.h"
#include "noise_lab.h"
#include "port_e16_audio.h"
#include "port_e16_usbaudio.h"
#include "stack_guard.h"
#include "usb_functions.h"
#include "volume.h"
#include "exit_prompt.h"

/* An exit asked for from the SysTick is carried out by the api (on the
 * way out of the call the app is in) or by PendSV (if it's in none). If
 * neither has happened by then -- the app raised BASEPRI above PendSV, or
 * a call is stuck -- the SysTick resets on its own, unless a file write
 * is putting data on the SPI flash (bounded; cutting a sector erase short
 * would lose the sector). The SysTick itself runs at the highest
 * configurable priority, so only BASEPRI/PRIMASK/FAULTMASK set by the
 * app can stop this; an app that masks everything (PRIMASK) can't be
 * forced out at all -- native apps are trusted code (app_mode.h). */
#define EXIT_FORCE_MS 2000u
#define SERVICE_EVERY_MS 4u   /* PendSV service rate while the app computes */
/* Before a reset, the log gets this long to reach the computer: in app
 * mode USB has only just enumerated (an app like hello is done within
 * milliseconds of the boot), so this waits for the connection too. */
#define LOG_DRAIN_MS 1500u

static bool g_active;
static volatile bool g_watch;          /* the SysTick watches Shift, pends the service */
static volatile uint32_t g_exit_req;   /* app_end_t asked for from an interrupt */
static volatile int32_t g_exit_code;
static uint32_t g_exit_wait_ms;
#define RELEASE_WAIT_MS 10000u  /* the longest a reset waits for the buttons */
#define LEAVE_GRACE_MS 1500u    /* after the prompt's yes: the app's time to save and exit */
static exit_prompt_t g_prompt;        /* Shift alone 5 s, then encoder 1 (exit_prompt.h) */
static volatile bool g_turned;        /* the app read a turn since the SysTick last looked */
static volatile bool g_redraw;        /* the prompt appeared / went: draw it (or the app) now */
static volatile bool g_exit_pending;  /* the prompt was confirmed: api->exit_pending() */
static uint32_t g_leave_ms;           /* ms since then */
static uint32_t g_service_ms;
static uint32_t g_t_start;
static char g_name[E16APP_NAME_MAX];

/* Normal mode: what app_mode_boot_report() found. */
static char g_last_stem[APP_RECORD_PATH_MAX];
static char g_last_error[APP_RECORD_PATH_MAX + APP_RECORD_MSG_MAX + 16];
static bool g_have_last;

/* app_mode_requested(): what this boot found (reported by app_mode_boot_report()). */
static uint32_t g_reset_flags;
static app_boot_t g_boot;

/* app_launch_prepare() -> app_launch_reset() */
static char g_launch_path[APP_RECORD_PATH_MAX];
static char g_launch_error[APP_RECORD_MSG_MAX + 64];

/* ---- the record ---- */

static uint32_t record_check(const app_record_t *r)
{
    return app_crc32(APP_RECORD_MAGIC, r, offsetof(app_record_t, check));
}

static bool record_valid(const app_record_t *r)
{
    return r->magic == APP_RECORD_MAGIC && r->check == record_check(r);
}

static void record_seal(app_record_t *r)
{
    r->magic = APP_RECORD_MAGIC;
    r->check = record_check(r);
}

static void record_end(app_end_t reason, int code)
{
    app_record_t *r = &BOOT_RECORDS->app;
    r->state = APP_REC_ENDED;
    r->reason = reason;
    r->code = code;
    record_seal(r);
}

/* "/apps/plasma.e16app" -> "plasma" */
static void stem_of(const char *path, char *out, size_t cap)
{
    const char *s = strrchr(path, '/');
    s = s ? s + 1 : path;
    const char *dot = strrchr(s, '.');
    size_t n = dot ? (size_t)(dot - s) : strlen(s);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(out, s, n);
    out[n] = '\0';
}

static const char *end_text(uint32_t reason)
{
    switch (reason) {
        case APP_END_EXIT: return "exit";
        case APP_END_SHIFT: return "left from the prompt";
        case APP_END_FAULT: return "fault";
        case APP_END_STACK: return "stack overflow";
        case APP_END_CONSOLE: return "console";
        case APP_END_AUDIO: return "audio callback stuck";
        default: return "reset";
    }
}

/* ---- capacity ---- */

static uint8_t *text_base(void)
{
    uintptr_t p = (uintptr_t)mem_sram_free_base() + app_api_ram_size();
    return (uint8_t *)((p + 7u) & ~(uintptr_t)7u);
}

size_t app_mode_text_cap(void)
{
    return (size_t)(mem_app_heap_base() - text_base());
}

/*
 * Where the app's code runs from. The CPU fetches code over its I-code
 * bus and reads literals over D-code, which reach the flash, or SRAM1
 * when SYSCFG remaps SRAM1 at 0x00000000 (MEMRMP = 3); code at
 * 0x2000xxxx is fetched over the system bus instead, competing with the
 * stack and every peripheral access, at about half the speed (measured
 * on the E16 with apps/x0x/bench.c: 2 cycles an instruction from
 * 0x2000xxxx). So the text is copied to SRAM as always and, if it lies
 * wholly in SRAM1 (the first 112 KB), linked and run at its alias at 0.
 * The interrupt vectors don't move (VTOR is the launcher's flash,
 * bsp/src/system.c), and a reset puts the map back. Returns the address
 * the loader rebases the text to: the alias, or the plain address (text
 * that reaches SRAM2, or a header it can't read: the loader then refuses
 * the file as before).
 */
#define SRAM1_ADDR 0x20000000u
#define SRAM1_BYTES (112u * 1024u)
#define MEMRMP_SRAM1 3u
static uint32_t exec_addr(const uint8_t *text, FIL *f)
{
    uint32_t phys = (uint32_t)(uintptr_t)text;
    uint8_t h[16];
    UINT got = 0;
    if (f_read(f, h, sizeof h, &got) != FR_OK || got != sizeof h || f_lseek(f, 0) != FR_OK) {
        f_lseek(f, 0);
        return phys;
    }
    uint32_t text_size = (uint32_t)h[12] | (uint32_t)h[13] << 8 | (uint32_t)h[14] << 16 | (uint32_t)h[15] << 24;
    if (phys < SRAM1_ADDR || text_size > SRAM1_BYTES || phys - SRAM1_ADDR > SRAM1_BYTES - text_size) {
        log_printf("app mode: text at 0x%08lx (reaches SRAM2: the system bus)", (unsigned long)phys);
        return phys;
    }
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
    (void)RCC->APB2ENR;
    SYSCFG->MEMRMP = (SYSCFG->MEMRMP & ~SYSCFG_MEMRMP_MEM_MODE) | (MEMRMP_SRAM1 << SYSCFG_MEMRMP_MEM_MODE_Pos);
    __DSB();
    __ISB();
    log_printf("app mode: text at 0x%08lx, run at 0x%08lx (SRAM1 remapped at 0)", (unsigned long)phys,
               (unsigned long)(phys - SRAM1_ADDR));
    return phys - SRAM1_ADDR;
}

/* ---- normal mode: launching ---- */

static int volume_reader(void *ud, void *buf, size_t n)
{
    (void)ud;
    return volume_read(buf, n) == (int32_t)n ? 0 : -1;
}

static const char *describe(int err, const e16app_header_t *h)
{
    static char why[64];
    switch (err) {
        case APP_ERR_TEXT_SIZE:
            snprintf(why, sizeof why, "code too big (%lu > %u bytes)", (unsigned long)h->text_size,
                     (unsigned)app_mode_text_cap());
            return why;
        case APP_ERR_DATA_SIZE:
            snprintf(why, sizeof why, "data too big (%lu > %u bytes)",
                     (unsigned long)(h->data_size + h->bss_size), (unsigned)APP_DATA_SIZE);
            return why;
        case APP_ERR_CRC:
            return "checksum mismatch (corrupt file)";
        default:
            return app_strerror(err);
    }
}

const char *app_launch_prepare(const char *stem)
{
    int n = snprintf(g_launch_path, sizeof g_launch_path, "%s/%s.e16app", VOLUME_APPS_DIR, stem);
    if (n < 0 || (size_t)n >= sizeof g_launch_path) {
        snprintf(g_launch_error, sizeof g_launch_error, "%.32s...: name too long", stem);
        return g_launch_error;
    }
    if (!volume_open(g_launch_path)) {
        snprintf(g_launch_error, sizeof g_launch_error, "%s.e16app: can't open", stem);
        return g_launch_error;
    }
    e16app_header_t h;
    memset(&h, 0, sizeof h);
    int err = app_verify_stream(volume_reader, NULL, volume_open_size(), app_mode_text_cap(), APP_DATA_SIZE, &h);
    volume_close();
    if (err != APP_OK) {
        snprintf(g_launch_error, sizeof g_launch_error, "%s.e16app: %s", stem, describe(err, &h));
        log_printf("app: refused %s", g_launch_error);
        return g_launch_error;
    }
    log_printf("app: %s checks out (%s: text %lu data %lu bss %lu)", g_launch_path, h.name,
               (unsigned long)h.text_size, (unsigned long)h.data_size, (unsigned long)h.bss_size);
    return NULL;
}

/*
 * Before every reset between the launcher and app mode (either way):
 * USB must have been up without a break for USB_STABLE_MS, so the
 * computer has finished setting the device up (resetting while macOS is
 * still enumerating it, a few times in a row, leaves its MIDI service
 * without a port for the E16 until it's replugged). Gives up after
 * USB_STABLE_TIMEOUT_MS (no cable, no computer). Then the log is drained
 * (until it's all out, or ms have passed), and USB gets a moment to put
 * the last packets on the wire (midi_write() only queues them, and a
 * reset drops the queue).
 *
 * With no computer at all (a power-only USB supply) there's nothing to
 * drain to: once this boot has been up USB_STABLE_TIMEOUT_MS without USB
 * ever coming up, this returns straight away instead of waiting out both
 * timeouts (~4.6 s) on every exit.
 */
#define USB_STABLE_MS 1000u
#define USB_STABLE_TIMEOUT_MS 3000u
#define USB_TX_SETTLE_MS 50u
static void drain_log(uint32_t ms, bool before_reset)
{
    uint32_t t0 = system_millis();
    while (before_reset && system_millis() - t0 < USB_STABLE_TIMEOUT_MS && console_usb_up_ms() < USB_STABLE_MS) {
        if (!console_usb_seen() && system_millis() >= USB_STABLE_TIMEOUT_MS) {
            break; /* no USB this boot, and long enough since the boot to say so */
        }
        console_drain_log();
    }
    if (!console_usb_seen()) {
        return;
    }
    t0 = system_millis();
    while (system_millis() - t0 < ms) {
        console_drain_log();
        if (midi_usb_connected() && console_log_sent()) {
            break;
        }
    }
    t0 = system_millis();
    while (system_millis() - t0 < USB_TX_SETTLE_MS) {
        console_drain_log();
    }
}

void app_launch_reset(void)
{
    if (!app_files_flush()) {
        log_printf("app: disk flush failed before app mode");
    }
    app_record_t *r = &BOOT_RECORDS->app;
    memset(r, 0, sizeof *r);
    r->state = APP_REC_RUN;
    strncpy(r->path, g_launch_path, sizeof r->path - 1);
    record_seal(r);
    log_printf("app: resetting into app mode for %s", g_launch_path);
    drain_log(LOG_DRAIN_MS, true);
    /* Last, right before the reset: the next boot honours the record only
     * with this token (app_boot.h). */
    BOOT_RECORDS->launch_token = app_boot_token(r->check);
    NVIC_SystemReset();
}

/* ---- normal mode: after an app ---- */

static const char *reset_text(uint32_t f)
{
    static char buf[48];
    buf[0] = '\0';
    static const struct {
        uint32_t bit;
        const char *name;
    } names[] = {
        {APP_BOOT_CSR_PORRSTF, " power-on"}, {APP_BOOT_CSR_BORRSTF, " brown-out"},
        {APP_BOOT_CSR_SFTRSTF, " software"}, {APP_BOOT_CSR_PINRSTF, " pin"},
        {APP_BOOT_CSR_IWDGRSTF, " iwdg"},    {APP_BOOT_CSR_WWDGRSTF, " wwdg"},
        {APP_BOOT_CSR_LPWRRSTF, " low-power"},
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (f & names[i].bit) {
            strncat(buf, names[i].name, sizeof buf - strlen(buf) - 1);
        }
    }
    return buf[0] ? buf + 1 : "none";
}

void app_mode_boot_report(void)
{
    log_printf("launcher: reset flags 0x%08lx (%s)", (unsigned long)g_reset_flags, reset_text(g_reset_flags));
    app_record_t *r = &BOOT_RECORDS->app;
    if (!record_valid(r)) {
        r->magic = 0;
        return;
    }
    stem_of(r->path, g_last_stem, sizeof g_last_stem);
    g_have_last = true;
    switch (r->state) {
        case APP_REC_FAILED:
            snprintf(g_last_error, sizeof g_last_error, "%s.e16app: %.*s", g_last_stem, APP_RECORD_MSG_MAX,
                     r->msg);
            log_printf("launcher: app %s didn't load: %.*s", g_last_stem, APP_RECORD_MSG_MAX, r->msg);
            break;
        case APP_REC_ENDED:
            if (r->reason == APP_END_EXIT) {
                log_printf("launcher: back from app %s (exit %ld)", g_last_stem, (long)r->code);
            } else {
                log_printf("launcher: back from app %s (%s)", g_last_stem, end_text(r->reason));
            }
            break;
        case APP_REC_RUNNING:
            log_printf("launcher: back from app %s (reset)", g_last_stem);
            break;
        case APP_REC_RUN:
            /* app_mode_requested() said no: not the launcher's own reset */
            g_have_last = false;
            log_printf("launcher: not starting app %s: %s", g_last_stem,
                       g_boot == APP_BOOT_MENU_NO_TOKEN ? "no launch token (a reflash or another reset)"
                                                        : "not the launcher's software reset");
            break;
        default:
            log_printf("launcher: app record for %s dropped (state %lu)", g_last_stem, (unsigned long)r->state);
            break;
    }
    memset(r, 0, sizeof *r);
}

const char *app_mode_last_stem(void)
{
    return g_have_last ? g_last_stem : NULL;
}

const char *app_mode_last_error(void)
{
    return g_last_error[0] ? g_last_error : NULL;
}

/* ---- boot ---- */

/*
 * Once, first thing at boot: reads and clears the reset flags, consumes
 * the launch token, and decides (app_boot.h). A "run" record that isn't
 * honoured stays for app_mode_boot_report() to log and clear.
 */
bool app_mode_requested(void)
{
    g_reset_flags = RCC->CSR;
    RCC->CSR |= RCC_CSR_RMVF;
    uint32_t token = BOOT_RECORDS->launch_token;
    BOOT_RECORDS->launch_token = 0;
    const app_record_t *r = &BOOT_RECORDS->app;
    bool run = record_valid(r) && r->state == APP_REC_RUN;
    g_boot = app_boot_decide(run, r->check, token, g_reset_flags);
    return g_boot == APP_BOOT_APP;
}

_Static_assert(APP_BOOT_CSR_SFTRSTF == RCC_CSR_SFTRSTF && APP_BOOT_CSR_PINRSTF == RCC_CSR_PINRSTF &&
                   APP_BOOT_CSR_PORRSTF == RCC_CSR_PORRSTF && APP_BOOT_CSR_BORRSTF == RCC_CSR_BORRSTF &&
                   APP_BOOT_CSR_IWDGRSTF == RCC_CSR_IWDGRSTF && APP_BOOT_CSR_WWDGRSTF == RCC_CSR_WWDGRSTF &&
                   APP_BOOT_CSR_LPWRRSTF == RCC_CSR_LPWRRSTF,
               "app_boot.h's reset flags are RCC_CSR's");

bool app_mode_active(void)
{
    return g_active;
}

void app_mode_note_fault(void)
{
    record_end(APP_END_FAULT, 0);
}

void app_mode_note_console_reset(void)
{
    if (g_active) {
        record_end(APP_END_CONSOLE, 0);
    }
}

/* Didn't load: note why for the launcher's error screen, and go back. */
__attribute__((noreturn, format(printf, 1, 2))) static void fail(const char *fmt, ...)
{
    app_record_t *r = &BOOT_RECORDS->app;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->msg, sizeof r->msg, fmt, ap);
    va_end(ap);
    log_printf("app: %s refused: %s", r->path, r->msg);
    if (blockdev_dirty() && !app_files_flush()) {
        log_printf("app: disk flush failed");
    }
    r->state = APP_REC_FAILED;
    record_seal(r);
    drain_log(LOG_DRAIN_MS, true);
    NVIC_SystemReset();
}

typedef struct {
    FIL *f;
    uint32_t read;
} load_ctx_t;

static int file_reader(void *ud, void *buf, size_t n)
{
    load_ctx_t *c = ud;
    UINT got = 0;
    if (f_read(c->f, buf, (UINT)n, &got) != FR_OK || got != n) {
        return -1;
    }
    c->read += got;
    return 0;
}

/*
 * A guard against stray writes to the internal flash, set up just before
 * the jump: the MPU, with PRIVDEFENA (the default memory map for
 * everything no region covers, so nothing else changes), makes the flash
 * controller's registers (FLASH_R_BASE, 1 KB: the keys, CR, OPTCR, ...)
 * and the internal flash itself read-only. Programming the flash needs
 * those registers, so a write there is the one that matters; the region
 * over the flash keeps a stray store from even reaching it. An app that
 * writes either gets a MemManage fault, and main.c's fault handler ends
 * it back in the launcher with the fault record (the PC and LR of the
 * write). The firmware's own code keeps running from flash: the region
 * is read-only, not no-execute.
 *
 * Region 2, NULL: no access at all (and XN) from address 0 up to the
 * largest power of two below the app's text (app_boot_null_guard_log2):
 * with SRAM1 remapped at 0 (exec_addr()) those addresses alias the
 * launcher's .data, which a NULL-pointer write would otherwise corrupt
 * silently; without the remap, 1 KB of the flash's boot alias. An app
 * that touches it gets the same MemManage fault.
 *
 * Not a sandbox: the app runs privileged (as everything on the E16
 * does), so it could turn the MPU off again; this catches accidents, not
 * malice. Native apps are trusted code (app_mode.h). The MPU is reset
 * with everything else by the reset that ends app mode.
 */
#define INTERNAL_FLASH_BASE 0x08000000u
#define MPU_SIZE_1KB 9u    /* region size 2^(SIZE+1) */
#define MPU_SIZE_512KB 18u
#define MPU_AP_RO 6u       /* read-only, privileged and unprivileged */
#define MPU_AP_NONE 0u     /* no access, privileged or not */
static bool mpu_guard_flash(uint32_t null_log2)
{
    if (((MPU->TYPE & MPU_TYPE_DREGION_Msk) >> MPU_TYPE_DREGION_Pos) < 3u) {
        return false; /* no MPU (or too few regions) on this part */
    }
    __DMB();
    MPU->CTRL = 0;
    /* Region 0: the flash controller's registers, device memory, XN. */
    MPU->RNR = 0;
    MPU->RBAR = FLASH_R_BASE;
    MPU->RASR = MPU_RASR_XN_Msk | (MPU_AP_RO << MPU_RASR_AP_Pos) | MPU_RASR_S_Msk | MPU_RASR_B_Msk |
                (MPU_SIZE_1KB << MPU_RASR_SIZE_Pos) | MPU_RASR_ENABLE_Msk;
    /* Region 1: the internal flash, normal memory as by default
     * (write-through), executable. */
    MPU->RNR = 1;
    MPU->RBAR = INTERNAL_FLASH_BASE;
    MPU->RASR = (MPU_AP_RO << MPU_RASR_AP_Pos) | MPU_RASR_C_Msk | (MPU_SIZE_512KB << MPU_RASR_SIZE_Pos) |
                MPU_RASR_ENABLE_Msk;
    /* Region 2: NULL, up to the text (above). */
    MPU->RNR = 2;
    MPU->RBAR = 0;
    MPU->RASR = null_log2 >= 5u ? MPU_RASR_XN_Msk | (MPU_AP_NONE << MPU_RASR_AP_Pos) |
                                      ((null_log2 - 1u) << MPU_RASR_SIZE_Pos) | MPU_RASR_ENABLE_Msk
                                : 0u;
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk;
    MPU->CTRL = MPU_CTRL_PRIVDEFENA_Msk | MPU_CTRL_ENABLE_Msk;
    __DSB();
    __ISB();
    return true;
}

/*
 * Into the app: MSP to the top of SRAM (a fresh stack; nothing of this
 * boot's is needed any more), r9 = the GOT, r0 = the api table, branch to
 * the Thumb entry. Never returns: the app leaves through a reset.
 */
__attribute__((naked)) static void app_jump(const e16_api_t *api, uint32_t r9, uint32_t entry, uint32_t sp)
{
    (void)api;
    (void)r9;
    (void)entry;
    (void)sp;
    __asm volatile("msr msp, r3\n"
                   "mov r9, r1\n"
                   "bx r2\n");
}

void app_mode_main(void)
{
    /* First: the record is taken, so whatever happens from here on
     * (a fault while loading, a crash in the app) the next boot is the
     * launcher's. */
    app_record_t *r = &BOOT_RECORDS->app;
    r->state = APP_REC_RUNNING;
    r->reason = APP_END_NONE;
    record_seal(r);
    g_active = true;

    stack_guard_init(mem_app_heap_base());
    display_init();
    noise_lab_apply_display();
    static const uint8_t blank[DISPLAY_WIDTH];
    for (int page = 0; page < DISPLAY_PAGES; page++) {
        display_write(page, 0, blank, sizeof blank);
    }
    usb_functions_start();
    midi_init();
    leds_init();
    leds_clear();
    leds_show();
    enc_init();
    port_e16_audio_init(); /* the synth (idle until the app plays it), the DWT cycle counter */
    flash_budget_init(&flash_e16_ops, system_millis);
    log_printf("app mode: %s", r->path);

    if (!storage_init() || storage_size() < VOLUME_FLASH_END) {
        fail("no SPI flash");
    }
    blockdev_init(&flash_budget_ops, VOLUME_FLASH_BASE, VOLUME_FLASH_SIZE, disk_block_cache);
    if (!volume_mount()) {
        fail("no disk volume");
    }

    void *api_ram = mem_sram_free_base();
    uint8_t *text = text_base();

    static FIL f;
    if (f_open(&f, r->path, FA_READ) != FR_OK) {
        fail("can't open");
    }
    uint32_t exec = exec_addr(text, &f);
    app_mem_t mem = {
        .text = text,
        .text_cap = app_mode_text_cap(),
        .text_addr = exec,
        .data = (uint8_t *)APP_DATA_BASE,
        .data_cap = APP_DATA_SIZE,
        .data_addr = APP_DATA_BASE,
    };
    uint32_t size = (uint32_t)f_size(&f);
    load_ctx_t ctx = {&f, 0};
    app_loaded_t app;
    uint32_t c0 = DWT->CYCCNT;
    int err = app_load_stream(file_reader, &ctx, &mem, &app);
    uint32_t cycles = DWT->CYCCNT - c0;
    f_close(&f);
    if (err != APP_OK) {
        fail("%s", app_strerror(err));
    }
    if (ctx.read != size) {
        fail("%s", app_strerror(APP_ERR_LENGTH));
    }
    uint32_t us = cycles / (SystemCoreClock / 1000000u);
    memcpy(g_name, app.name, sizeof g_name);
    log_printf("app: %s text %lu data %lu bss %lu (cap %u/%u), %lu bytes loaded in %lu.%lu ms", g_name,
               (unsigned long)app.text_size, (unsigned long)app.data_size, (unsigned long)app.bss_size,
               (unsigned)mem.text_cap, (unsigned)mem.data_cap, (unsigned long)size, (unsigned long)(us / 1000u),
               (unsigned long)(us % 1000u / 100u));

    const e16_api_t *api = app_api_init(api_ram, g_name, app.r9);
    console_app_mode(app_api_framebuffer());
    mirror_app_mode(app_api_framebuffer(), g_name);
    uint32_t null_log2 = app_boot_null_guard_log2(exec < SRAM1_ADDR, exec);
    if (!mpu_guard_flash(null_log2)) {
        log_printf("app mode: no MPU; the internal flash isn't guarded");
    } else {
        log_printf("app mode: MPU: flash read-only, no access below 0x%08lx", null_log2 ? (unsigned long)(1u << null_log2) : 0ul);
    }
    drain_log(20, false);

    /* PendSV: the service while the app computes, below every driver
     * interrupt so it only ever preempts the app itself. */
    NVIC_SetPriority(PendSV_IRQn, (1u << __NVIC_PRIO_BITS) - 1u);
    g_t_start = system_millis();
    g_watch = true;
    app_jump(api, app.r9, app.entry, (uint32_t)(uintptr_t)mem_sram_top());
    for (;;) {
    }
}

/* ---- app mode: service and leaving ---- */

void app_mode_service(void)
{
    midi_msg_t msg;
    while (midi_read(&msg)) {
        if (!console_handle(&msg)) {
            app_api_midi_in(&msg);
        }
    }
    console_drain_log();
    mirror_poll();
    port_e16_audio_poll(system_millis());
    port_e16_usbaudio_poll(system_millis());
    if (!stack_guard_intact()) {
        app_mode_end(APP_END_STACK, 0);
    }
    /* the leave prompt over the app: when it appears or goes, and every
     * 250 ms while it's up if the app isn't presenting frames itself */
    static uint32_t last_draw;
    uint32_t now = system_millis();
    if (g_redraw || (exit_prompt_active(&g_prompt) && now - app_api_last_present() >= 250u &&
                     now - last_draw >= 250u)) {
        g_redraw = false;
        last_draw = now;
        app_api_redraw();
    }
}

bool app_mode_input_blocked(void)
{
    return exit_prompt_active(&g_prompt);
}

void app_mode_draw_overlay(uint8_t *fb)
{
    if (exit_prompt_active(&g_prompt)) {
        exit_prompt_draw(fb, g_name, g_prompt.state == EXIT_PROMPT_CONFIRMED);
    }
}

void app_mode_turned(void)
{
    g_turned = true;
}

bool app_mode_exit_pending(void)
{
    return g_exit_pending;
}

void app_mode_check_exit(void)
{
    if (g_exit_req != APP_END_NONE) {
        app_mode_end((app_end_t)g_exit_req, g_exit_code);
    }
}

static void reset_when_released(void) __attribute__((noreturn));

void app_mode_end(app_end_t reason, int code)
{
    g_watch = false;
    port_e16_audio_app_stop();
    if (reason == APP_END_EXIT) {
        log_printf("app: %s exit %d after %lu s", g_name, code,
                   (unsigned long)((system_millis() - g_t_start) / 1000u));
    } else {
        log_printf("app: %s ended (%s) after %lu s", g_name, end_text(reason),
                   (unsigned long)((system_millis() - g_t_start) / 1000u));
    }
    app_api_close_files();
    if (!app_files_flush()) {
        log_printf("app: disk flush failed");
    }
    record_end(reason, code);
    leds_clear();
    leds_show();
    drain_log(LOG_DRAIN_MS, true);
    reset_when_released();
}

/* Shift or an encoder physically down (the bootloader can't see the
 * console's virtual presses). */
static bool buttons_down(void)
{
    for (int i = 0; i < ENC_COUNT; i++) {
        if (enc_pressed(i)) {
            return true;
        }
    }
    return enc_shift_pressed();
}

/* Reset once every button is up, at most
 * RELEASE_WAIT_MS later, saying so on the OLED meanwhile: reset with
 * 15 + 16 + Shift held and OXI's bootloader starts its update mode. */
static void reset_when_released(void)
{
    if (buttons_down()) {
        /* drawn into the api's "as presented" buffer: the app is over, and
         * the screen dump then shows it too */
        uint8_t *fb = (uint8_t *)app_api_framebuffer();
        if (fb) {
            exit_prompt_draw_release(fb);
            for (int p = 0; p < DISPLAY_PAGES; p++) {
                display_write(p, 0, fb + p * DISPLAY_WIDTH, DISPLAY_WIDTH);
            }
        }
        uint32_t t0 = system_millis();
        uint32_t up_since = 0;
        bool up = false;
        while (system_millis() - t0 < RELEASE_WAIT_MS) {
            if (buttons_down()) {
                up = false;
            } else if (!up) {
                up = true;
                up_since = system_millis();
            } else if (system_millis() - up_since >= 50u) {
                break; /* released for 50 ms: no bounce */
            }
        }
    }
    NVIC_SystemReset();
}

/* From the SysTick: carried out by the api or PendSV (see EXIT_FORCE_MS). */
static void request_exit(app_end_t reason, int code)
{
    if (g_exit_req == APP_END_NONE) {
        g_exit_code = code;
        g_exit_req = reason;
        g_exit_wait_ms = 0;
        SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
    }
}

/*
 * The HAL's 1 ms tick, from the SDK's SysTick handler (HAL_IncTick() is
 * weak in the HAL; this is the same plus app mode's watch). SysTick has
 * the highest priority, so the watch runs whatever the app is doing.
 */
void HAL_IncTick(void)
{
    uwTick += uwTickFreq;
    if (!g_watch) {
        return;
    }
    bool shift = enc_shift_pressed() || console_virtual_held(16);
    uint32_t pressed = 0;
    for (int i = 0; i < ENC_COUNT; i++) {
        if (enc_pressed(i) || console_virtual_held(i)) {
            pressed |= 1u << i;
        }
    }
    bool turned = g_turned;
    g_turned = false;
    int r = exit_prompt_step(&g_prompt, 1, shift, pressed, turned);
    if (r == EXIT_PROMPT_SHOW || r == EXIT_PROMPT_HIDE) {
        g_redraw = true;
    } else if (r == EXIT_PROMPT_LEAVE) {
        g_redraw = true;
        g_exit_pending = true;
        g_leave_ms = 0;
    }
    /* the app had its chance to save and api->exit(): leave anyway */
    if (g_exit_pending && ++g_leave_ms >= LEAVE_GRACE_MS) {
        request_exit(APP_END_SHIFT, 0);
    }
    /* An audio callback that doesn't return holds up everything below
     * it (the app, PendSV, the api): reset right away, whatever else is
     * going on (a file write it interrupted is cut short like a pulled
     * cable would). */
    if (port_e16_audio_app_stuck(uwTick)) {
        record_end(APP_END_AUDIO, 0);
        NVIC_SystemReset();
    }
    if (g_exit_req != APP_END_NONE) {
        /* (forced: every button up first, see reset_when_released(),
         * but not waiting forever on them) */
        if (++g_exit_wait_ms >= EXIT_FORCE_MS && !app_api_flash_busy() &&
            (!buttons_down() || g_exit_wait_ms >= EXIT_FORCE_MS + RELEASE_WAIT_MS)) {
            record_end((app_end_t)g_exit_req, g_exit_code);
            NVIC_SystemReset();
        }
    }
    if (++g_service_ms >= SERVICE_EVERY_MS) {
        g_service_ms = 0;
        SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
    }
}

/* Lowest priority: runs only between the app's own instructions, never
 * inside an api call (those service themselves). */
void PendSV_Handler(void)
{
    if (!g_watch || app_api_busy()) {
        return;
    }
    if (g_exit_req != APP_END_NONE) {
        app_mode_end((app_end_t)g_exit_req, g_exit_code);
    }
    app_mode_service();
}
