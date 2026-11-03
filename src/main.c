#include <stdbool.h>
#include <stdint.h>

#include "board.h" /* DWT, SystemCoreClock */
#include "display.h"
#include "midi.h"
#include "system.h"

#include "rt.h"
#include "rt_heap.h"
#include "rt_api_midiin.h" /* RT_MIDI_IN_USB, RT_MIDI_IN_TRS */
#include "port_e16.h"
#include "port_e16_audio.h"
#include "port_e16_usbaudio.h"
#include "log.h"
#include "console.h"
#include "stack_guard.h"
#include "screen.h"
#include "scenes.h"
#include "persist.h"
#include "disk_mode.h"
#include "volume.h"
#include "flash_budget.h"
#include "flash_e16.h"
#include "launcher.h"
#include "boot_records.h"
#include "mem_layout.h"
#include "app_mode.h"
#include "noise_lab.h"
#include "webcam.h"
#include "usb_functions.h"
#include "mirror.h"

/*
 * A crash in the launcher lands in the bootloader, where reflash.sh still
 * works; a crash in a native app (app mode, src/apps/app_mode.h) resets
 * back into the launcher instead, with the app's record saying so. On the
 * way, the fault handler notes the faulting PC and LR (from the exception
 * frame) in the crash record at the top of the CCM (boot_records.h),
 * which startup code doesn't clear and nothing else uses, so the next
 * boot can log it -- if the RAM kept power in between, i.e. a reset, or
 * the bootloader started the launcher again without a power cycle (a
 * reflash over USB, or leaving the bootloader). The check word guards
 * against reading power-on garbage as a record.
 */
#define g_crash (BOOT_RECORDS->crash)

/* Called from the handlers below with the stacked exception frame (r0-r3,
 * r12, lr, pc, xpsr). No flash writes or logging here: just the records. */
__attribute__((used, noreturn)) void fault_record(const uint32_t *frame)
{
    g_crash.pc = frame[6];
    g_crash.lr = frame[5];
    g_crash.check = CRASH_MAGIC ^ frame[6] ^ frame[5];
    g_crash.magic = CRASH_MAGIC;
    if (app_mode_active()) {
        app_mode_note_fault();
        NVIC_SystemReset();
    }
    system_enter_bootloader();
    for (;;) {
    }
}

/* Pass the frame from whichever stack was in use (EXC_RETURN bit 2). */
#define FAULT_HANDLER(name)                                                                        \
    __attribute__((naked)) void name(void)                                                         \
    {                                                                                              \
        __asm volatile("tst lr, #4\n"                                                              \
                       "ite eq\n"                                                                  \
                       "mrseq r0, msp\n"                                                           \
                       "mrsne r0, psp\n"                                                           \
                       "b fault_record\n");                                                        \
    }

FAULT_HANDLER(HardFault_Handler)
FAULT_HANDLER(MemManage_Handler)
FAULT_HANDLER(BusFault_Handler)
FAULT_HANDLER(UsageFault_Handler)

static void log_last_crash(void)
{
    if (g_crash.magic == CRASH_MAGIC && g_crash.check == (CRASH_MAGIC ^ g_crash.pc ^ g_crash.lr)) {
        log_printf("launcher: last reset was a fault at PC=0x%08lx LR=0x%08lx", (unsigned long)g_crash.pc,
                   (unsigned long)g_crash.lr);
    }
    g_crash.magic = 0;
}

/*
 * midi_read() has one consumer queue, so every message is read here once
 * and dispatched: realtime clock/transport (from USB or the TRS input) to
 * the runtime, console SysEx (reboot, log replay) to console.c, and
 * everything else to the runtime's MIDI-in queue (midi.onNote etc.,
 * src/rt/rt_api_midiin.h), which keeps only what a script listens for.
 * All of it is read every pass, however much a script is slow to take:
 * the SDK's TRS buffer overflows after about 80 ms, and the console's
 * SysEx shares this queue, so a flood is dropped (and counted) in the
 * runtime's queue rather than left here.
 */
static void poll_midi_in(void)
{
    midi_msg_t msg;
    while (midi_read(&msg)) {
        if (msg.len == 1 && msg.data[0] >= 0xF8 && msg.data[0] <= 0xFC) {
            if (msg.data[0] != 0xF8) {
                /* Turn clock ticks already queued into pulses first: a
                 * Start/Stop/Continue resets the runtime's pending ticks,
                 * so a burst read in one pass would otherwise lose them. */
                rt_tick();
            }
            rt_midi_realtime(msg.data[0]);
        } else if (!console_handle(&msg)) {
            if (webcam_active()) {
                webcam_midi(msg.data, msg.len); /* no script runs meanwhile */
            } else if (msg.port == MIDI_USB) {
                rt_midi_in(RT_MIDI_IN_USB, msg.data, msg.len);
            } else if (msg.port == MIDI_TRS_IN) {
                rt_midi_in(RT_MIDI_IN_TRS, msg.data, msg.len);
            }
        }
    }
}

#ifndef E16_DEV_CONSOLE
#define E16_DEV_CONSOLE 1
#endif

#if E16_DEV_CONSOLE
/*
 * Dev builds: while a script runs, log the main loop's rate and its
 * longest pass every 10 s ("loop: ..."), for checking that a script (or
 * the audio interrupt) doesn't starve it. Timed with the DWT cycle
 * counter port_e16_audio_init() starts.
 */
#define LOOP_LOG_MS 10000u
static void loop_stats(uint32_t now)
{
    static uint32_t t0, passes, longest, last_cyc;
    uint32_t cyc = DWT->CYCCNT;
    uint32_t pass = cyc - last_cyc;
    last_cyc = cyc;
    if (!rt_state()) {
        t0 = now;
        passes = 0;
        longest = 0;
        return;
    }
    passes++;
    if (passes > 1 && pass > longest) {
        longest = pass;
    }
    if (now - t0 >= LOOP_LOG_MS) {
        uint32_t mhz = SystemCoreClock / 1000000u;
        log_printf("loop: %lu passes/s, longest pass %lu us", (unsigned long)(passes * 1000u / (now - t0)),
                   (unsigned long)(longest / mhz));
        t0 = now;
        passes = 0;
        longest = 0;
    }
}
#endif

int main(void)
{
    system_init();
    if (app_mode_requested()) {
        app_mode_main(); /* a native app's boot instead (doesn't return) */
    }
    stack_guard_init(mem_normal_heap_base());
    display_init();
    noise_lab_apply_display(); /* the noise lab's display settings, if any */
    screen_init();          /* clear the bootloader's image */
    usb_functions_start();  /* drive, audio input, camera: before midi_init() starts USB */
    midi_init();
    port_e16_init();
    /* Before persist_init()/disk_init(): both bind to flash_budget_ops
     * rather than flash_e16_ops directly, so every sector erase either
     * of them ever does -- persistence saves, the FAT volume's block
     * cache -- goes through the erase-rate budget (flash_budget.h). */
    flash_budget_init(&flash_e16_ops, system_millis);
    persist_init();

    log_last_crash();
    app_mode_boot_report();
    disk_init();            /* after persist_init(): needs the flash up */
    scenes_rescan();        /* the menu: built-ins plus /scripts */
    /* Persistence slots of scenes that no longer exist: only with the
     * volume mounted and /scripts readable, never on a failed mount (every
     * disk script's state would look orphaned). */
    if (disk_available() && volume_mounted() && volume_count_scripts() >= 0) {
        persist_reclaim();
    }
    log_printf("launcher: heap %u bytes, %d scripts", (unsigned)rt_heap_total(), scenes_count());
    launcher_init();

    unsigned stack_free = stack_guard_free();
    log_printf("launcher: stack free %u bytes", stack_free);
    uint32_t stack_checked = system_millis();

    for (;;) {
        uint32_t now = system_millis();
        if (now - stack_checked >= 5000) {
            /* Report a new stack high-water mark (approximate). */
            stack_checked = now;
            unsigned f = stack_guard_free();
            if (f < stack_free) {
                stack_free = f;
                log_printf("launcher: stack free %u bytes", f);
            }
        }
        stack_guard_check();
        poll_midi_in();
        launcher_poll(now);
        rt_tick();
        launcher_persist_poll(now);
        /*
         * A debounce-triggered save blocks for a sector erase (~15 ms)
         * plus the write (storage.h), so drain anything that queued
         * during that stall right away rather than waiting for the next
         * pass -- MIDI clock bytes in particular are time-sensitive, and
         * poll_midi_in() already turns a burst into pulses before this
         * pass's rt_tick() would otherwise have seen them.
         */
        poll_midi_in();
        port_e16_audio_poll(now);
        port_e16_usbaudio_poll(now);
        port_e16_leds_poll();
        screen_poll(system_millis());
        console_drain_log();
        mirror_poll(); /* after the log, which gets the USB queue first */
        flash_budget_poll(now);
#if E16_DEV_CONSOLE
        loop_stats(now);
#endif
    }
}
