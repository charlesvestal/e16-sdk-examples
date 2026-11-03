#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "enc.h"
#include "system.h"

#include "rt.h"
#include "rt_api_var.h"
#include "rt_api_store.h"
#include "rt_heap.h"
#include "port_e16.h"
#include "log.h"
#include "console.h"
#include "screen.h"
#include "scenes.h"
#include "rt_port.h"
#include "persist.h"
#include "disk_mode.h"
#include "launcher.h"
#include "app_mode.h"
#include "webcam.h"
#include "mirror.h"
#include "mirror_core.h"

/*
 * The launcher UI: menu -> running script (or error screen) -> menu.
 *
 *   Menu      turn encoder 1 = move the selection, press it = start.
 *             A native app (an .e16app in /apps, marked "app") is checked and
 *             then run in app mode, a boot of its own (app_mode.h): the
 *             launcher comes back when it ends. The last entry, "Disk
 *             mode" (if the volume is available), enters Disk mode.
 *   Disk mode the computer has the volume as a USB drive (disk_mode.h).
 *             Ejecting it on the computer, pressing encoder 1, or
 *             holding Shift 1 s leaves, back to the menu.
 *   Webcam    the E16 is a USB camera with a video synth on the encoders
 *             (webcam.h), the menu's last entry. Holding encoder 1 for
 *             1 s, or Shift 1 s, leaves, back to the menu; a short press
 *             of encoder 1 is the webcam's (the OLED's view).
 *   Running   encoders go to the script, except:
 *               Shift + press encoder N (while Shift is held down) = show
 *                 page N directly (if the script has one), logged as
 *                 "ui: page N (<name>) via shift+press". Pressing any
 *                 encoder while Shift is down also cancels that Shift
 *                 hold's "1 s = back to menu" timer (see g_shift_used
 *                 below), so Shift+press never also exits to the menu,
 *                 and the Shift release that follows doesn't count as a
 *                 tap either (no picker opens from it).
 *               tap Shift (release within 1 s, no encoder pressed during
 *                 the hold), then press encoder N = show page N
 *                 (page.onPageChange); the grid shows the page names
 *                 until then. Another Shift tap, or 4 s without a
 *                 choice, closes it.
 *               hold Shift 1 s with no encoder pressed = back to the menu
 *                 (rt_unload: store saved).
 *   Error     press any encoder (or hold Shift 1 s) = back to the menu.
 *   Anywhere  Shift + hold encoder 16 for 2 s = bootloader (a quick
 *               Shift + press of 16 instead just tries to switch to page
 *               16 like any other encoder, which no script has a page for
 *               -- see on_press() -- so it's effectively ignored).
 *
 * Shift itself never reaches the script. Presses made while Shift is down
 * (or that open/choose in the menu, picker or error screen) are consumed
 * by the UI, release included, so the script never sees half a press.
 *
 * Turns are dropped while Shift is held: there's no Shift+turn gesture
 * yet, only Shift+press ones (see on_turn()).
 */
typedef enum { UI_MENU, UI_RUNNING, UI_ERROR, UI_DISK, UI_WEBCAM } ui_state_t;

#define SHIFT_HOLD_MS 1000u
#define WEBCAM_HOLD_MS 1000u   /* encoder 1 held this long leaves webcam mode */
#define BOOT_HOLD_MS 2000u
#define PICKER_TIMEOUT_MS 4000u
#define MENU_RING_COLOR 18      /* light blue */

static ui_state_t g_state = UI_MENU;
static int g_sel;                /* menu selection */
static const rt_scene_t *g_scene; /* the loaded script's scene, from scene_open() */

static bool g_down[ENC_COUNT];
static bool g_consumed[ENC_COUNT];   /* press handled by the UI, not the script */
static bool g_shift;
static bool g_shift_used;            /* an encoder was pressed during this Shift */
static bool g_shift_long;            /* this Shift hold already acted */
static uint32_t g_shift_t0;
static bool g_picker;
static uint32_t g_picker_t0;
static bool g_boot_armed;
static uint32_t g_boot_t0;
static bool g_enc1_armed;            /* webcam mode: encoder 1 is down, pressed in webcam mode */
static uint32_t g_enc1_t0;           /* ... since then */

/* Save debounce: persist_save_vars() runs at most once every
 * PERSIST_DEBOUNCE_MS while a var has changed (rt_vars_dirty()), rather
 * than on every var.set -- a script wired to an encoder (e.g. Euclid's
 * tempo) calls var.set on every turn, so without this a continuous turn
 * would erase a sector on every step. */
#define PERSIST_DEBOUNCE_MS 5000u
static uint32_t g_last_var_save;

/* A one-shot heap log 60 s into a script's run, so a console capture can
 * confirm free heap (docs/hw-acceptance.md) without a separate build.
 * Rearmed by start_script(); fires at most once per run. */
#define HEAP_LOG_MS 60000u
static uint32_t g_run_t0;
static bool g_heap_logged;

#define DISK_ENTRY "Disk mode"
#define WEBCAM_ENTRY "Webcam"

/* Menu entries: the scenes (scenes.h: built-ins and disk scripts), then
 * "Disk mode" if there's a volume, then "Webcam". */
static const char *const g_extras_disk[] = {DISK_ENTRY, WEBCAM_ENTRY};
static const char *const g_extras_no_disk[] = {WEBCAM_ENTRY};

static int menu_count(void)
{
    return scenes_count() + (disk_available() ? 2 : 1);
}

static bool disk_selected(void)
{
    return disk_available() && g_sel == scenes_count();
}

static bool webcam_selected(void)
{
    return g_sel == menu_count() - 1;
}

static void show_menu_screen(void)
{
    if (disk_available()) {
        screen_show_menu(scenes_count(), g_extras_disk, 2, g_sel);
    } else {
        screen_show_menu(scenes_count(), g_extras_no_disk, 1, g_sel);
    }
    if (disk_selected()) {
        log_printf("ui: menu (%s selected)", DISK_ENTRY);
    } else if (webcam_selected()) {
        log_printf("ui: menu (%s selected)", WEBCAM_ENTRY);
    } else {
        scene_info_t info = scene_info(g_sel);
        log_printf("ui: menu (%s selected%s%s)", info.title,
                   info.source == SCENE_DISK  ? ", disk"
                   : info.source == SCENE_APP ? ", app"
                                              : "",
                   info.broken ? ", broken" : "");
    }
}

static void menu_ring(void)
{
    int n = menu_count();
    int v = n > 1 ? g_sel * 16383 / (n - 1) : 16383;
    rt_port_ring(1, v, MENU_RING_COLOR);
}

/*
 * Reset the UI's own per-encoder press tracking. Called on every
 * transition between the menu and a running script (show_menu(),
 * start_script()), so a press straddling the transition -- held down in
 * one context, released in the other -- can't produce a release (or,
 * via g_down's edge detection, a turn) the new context never asked for.
 * rt_api_ctl resets its own press state the same way, from rt_load()/
 * rt_unload() (see rt_api_ctl.c's reset_state(), called from
 * rt_api_ctl_open()/rt_api_ctl_close()): each side only owns the
 * invariant for its own state, not the other's.
 */
static void reset_press_tracking(void)
{
    for (int i = 0; i < ENC_COUNT; i++) {
        g_down[i] = false;
        g_consumed[i] = false;
    }
    g_enc1_armed = false;
}

/*
 * Persist vars and store on scene exit. Vars are saved first
 * (they're always live in RAM, no commit step needed); rt_unload()
 * commits the live store.data table into its buffer (rt_api_store_close,
 * via whichever target the script last selected with store.use()), so
 * the store/shared saves have to follow rt_unload(), not precede it.
 */
static void persist_save_on_exit(const char *name)
{
    persist_save_vars(name);
    rt_vars_clear_dirty();
    rt_unload();
    persist_save_store(name);
    persist_save_shared();
}

static void show_menu(void)
{
    if (g_state == UI_RUNNING) {
        persist_save_on_exit(g_scene->name);
        log_printf("ui: unloaded %s", g_scene->name);
    } else if (g_state == UI_DISK) {
        disk_mode_leave("shift hold");
        g_sel = scenes_count(); /* still on "Disk mode", wherever the rescan put it */
    } else if (g_state == UI_WEBCAM) {
        webcam_leave("shift hold");
        g_sel = menu_count() - 1;
    }
    g_scene = NULL;
    g_state = UI_MENU;
    g_picker = false;
    if (g_sel >= menu_count()) {
        g_sel = menu_count() - 1;
    }
    reset_press_tracking();
    port_e16_ui_clear();
    menu_ring();
    show_menu_screen();
}

/* After reset_press_tracking(), for a transition made by pressing enc:
 * that press is still down and belongs to the UI, so its release is
 * swallowed rather than seen as a new press on the next pass (which would
 * leave the screen the press just opened). */
static void keep_press_consumed(int enc)
{
    g_down[enc] = true;
    g_consumed[enc] = true;
}

/* Disk mode (disk_mode.h). Only from the menu, so no script is running:
 * its vars and store were already saved when it exited. */
static void enter_disk_mode(void)
{
    g_state = UI_DISK;
    reset_press_tracking();
    keep_press_consumed(0);
    port_e16_ui_clear();
    screen_show_notice("DISK MODE", "Eject on the computer,", "or press encoder 1", "to leave");
    log_printf("ui: disk mode");
    disk_mode_enter();
}

/* Back to the menu from Disk mode: on eject, or by_press (encoder 1). A
 * Shift hold goes through show_menu(), which leaves the same way. */
static void leave_disk_mode(const char *why, bool by_press)
{
    disk_mode_leave(why);
    g_state = UI_MENU; /* show_menu() has nothing more to leave */
    g_sel = scenes_count(); /* still on "Disk mode", wherever the rescan put it */
    show_menu();
    if (by_press) {
        keep_press_consumed(0);
    }
}

/* Webcam mode (webcam.h). Only from the menu, so no script is running and
 * the Lua heap is free for the video synth's tables. */
static void enter_webcam(void)
{
    port_e16_ui_clear();
    if (!webcam_enter()) {
        g_state = UI_ERROR;
        screen_show_error("Webcam: couldn't start (see the log)");
        log_printf("ui: error screen");
        return;
    }
    g_state = UI_WEBCAM;
    reset_press_tracking(); /* the caller keeps encoder 1's press the UI's */
    log_printf("ui: webcam mode");
}

/* Back to the menu from webcam mode by holding encoder 1 (a Shift hold
 * goes through show_menu(), which leaves the same way). */
static void leave_webcam(void)
{
    webcam_leave("encoder 1 hold");
    g_state = UI_MENU;
    g_sel = menu_count() - 1;
    show_menu();
    keep_press_consumed(0); /* its release is the UI's */
}

/* `now` is the main-loop pass's time (launcher_poll()'s), the same clock
 * launcher_persist_poll() compares against later in this pass: a fresher
 * system_millis() here would be ahead of it, and the unsigned difference
 * would wrap and fire the debounce and the heap log at once. */
static void start_script(int i, uint32_t now)
{
    reset_press_tracking();
    port_e16_ui_clear();
    scene_info_t info = scene_info(i);
    log_printf("ui: starting %s (%s%s)", info.title, info.source == SCENE_DISK ? "disk" : "built-in",
               info.replaces ? ", replaces the built-in" : "");
    /*
     * rt.h's restore order: bind var/store's per-scene buffers to this
     * scene's name (without clearing them) and deserialize the saved
     * record into them *before* the load, which runs page.onInit()
     * itself before it returns -- restoring after it returns would be
     * too late, the script's onInit would already have registered its
     * defaults. rt_api_var_open()/rt_api_store_open() (called from
     * inside the load, ahead of onInit) only clear a buffer when its
     * bound name doesn't match the scene being loaded, so binding first
     * keeps what's deserialized here intact. A broken disk script never
     * loads (scene_open() stops at its header), so it skips this.
     */
    if (!info.broken) {
        rt_api_var_bind_scene(info.name);
        persist_load_vars(info.name);
        rt_api_store_bind_scene(info.name);
        persist_load_store(info.name);
        persist_load_shared();
    }
    const rt_scene_t *sc = scene_open(i);
    if (sc) {
        /* After the load, not before: onInit's var.register calls (the
         * defaults of vars that had no saved value) mark vars dirty, and
         * that isn't a change worth a flash write. */
        rt_vars_clear_dirty();
        g_scene = sc;
        g_state = UI_RUNNING;
        g_last_var_save = now;
        g_run_t0 = now;
        g_heap_logged = false;
        screen_show_running(sc);
        log_printf("ui: running %s, page %d, heap used %u / total %u", sc->title, rt_page(),
                   (unsigned)rt_heap_used(), (unsigned)rt_heap_total());
    } else {
        /* The runtime or the provider already logged the message; the
         * screen shows it too. */
        g_state = UI_ERROR;
        screen_show_error(scenes_last_error());
        log_printf("ui: error screen");
    }
}

/* A native app: refused on the error screen if its image doesn't check
 * out, else run in app mode (the reset doesn't return). Only from the
 * menu, so no script is running: its vars and store were saved when it
 * exited. */
static void start_app(int i)
{
    scene_info_t info = scene_info(i);
    log_printf("ui: starting app %s", info.name);
    const char *err = app_launch_prepare(info.name);
    if (err) {
        g_state = UI_ERROR;
        screen_show_error(err);
        log_printf("ui: error screen");
        return;
    }
    port_e16_ui_clear();
    screen_show_notice("APP", info.title, "starting", NULL);
    screen_flush();
    app_launch_reset();
}

static void set_picker(bool open, uint32_t now)
{
    g_picker = open;
    g_picker_t0 = now;
    screen_show_page_picker(open);
}

/* Shows page enc+1 of the running script if it has one (rt_show_page():
 * page.onPageChange), logged as "ui: page N (<name>)<how>". */
static void show_page(int enc, const char *how)
{
    const rt_scene_t *sc = g_scene;
    if (enc + 1 <= sc->pages) {
        rt_show_page(enc + 1);
        log_printf("ui: page %d (%s)%s", enc + 1, sc->page_names[enc] ? sc->page_names[enc] : "", how);
    }
}

static void on_turn(int enc, int steps)
{
    /* Turns are dropped while Shift is held -- there's no Shift+turn
     * gesture yet (every Shift gesture so far is press-based: a tap or
     * hold of Shift itself, or Shift + press of an encoder). */
    if (g_shift) {
        return;
    }
    if (g_state == UI_RUNNING) {
        rt_turn(enc + 1, steps);
    } else if (g_state == UI_WEBCAM) {
        webcam_turn(enc, steps);
    } else if (g_state == UI_MENU && enc == 0) {
        int n = menu_count();
        int sel = g_sel + steps;
        sel = sel < 0 ? 0 : (sel >= n ? n - 1 : sel);
        if (sel != g_sel) {
            g_sel = sel;
            menu_ring();
            show_menu_screen();
        }
    }
}

static void on_press(int enc, uint32_t now)
{
    g_consumed[enc] = true;
    if (g_shift) {
        /* Setting g_shift_used here is what cancels this Shift hold's
         * "1 s = back to menu" timer (poll_shift() only fires that while
         * !g_shift_used) and makes the Shift release that follows not
         * count as a tap (poll_shift() only opens the picker on release
         * while !g_shift_used), for every encoder, not just this
         * direct-page-switch one. */
        g_shift_used = true;
        if (enc == 15) {
            g_boot_armed = true;
            g_boot_t0 = now;
        }
        if (g_state == UI_RUNNING) {
            if (g_picker) {
                /* Direct switch supersedes an already-open tap-opened
                 * picker rather than leaving it stuck on screen. */
                set_picker(false, now);
            }
            show_page(enc, " via shift+press");
        }
        return;
    }
    switch (g_state) {
        case UI_RUNNING:
            if (g_picker) {
                set_picker(false, now);
                show_page(enc, "");
                return;
            }
            g_consumed[enc] = false;
            rt_press(enc + 1);
            break;
        case UI_MENU:
            if (enc == 0) {
                if (disk_selected()) {
                    enter_disk_mode();
                } else if (webcam_selected()) {
                    enter_webcam();
                    keep_press_consumed(0);
                } else if (scene_info(g_sel).source == SCENE_APP) {
                    start_app(g_sel);
                    keep_press_consumed(0);
                } else {
                    /* The press that started the script stays the UI's:
                     * without this, start_script()'s reset leaves it to
                     * be seen again on the next pass as a fresh press of
                     * encoder 1 (and its release), reaching the script --
                     * or closing the error screen at once. */
                    start_script(g_sel, now);
                    keep_press_consumed(0);
                }
            }
            break;
        case UI_ERROR:
            /* Kept as the UI's for the same reason as starting a script:
             * otherwise encoder 1's press, seen again by the menu, starts
             * the failed script straight back up. */
            show_menu();
            keep_press_consumed(enc);
            break;
        case UI_DISK:
            if (enc == 0) {
                leave_disk_mode("encoder 1", true);
            }
            break;
        case UI_WEBCAM:
            if (enc == 0) {
                /* Short press: the webcam's (on release); 1 s: leave (launcher_poll()). */
                g_enc1_armed = true;
                g_enc1_t0 = now;
            } else {
                webcam_press(enc);
            }
            break;
    }
}

static void on_release(int enc)
{
    if (enc == 15) {
        g_boot_armed = false;
    }
    if (enc == 0 && g_enc1_armed) {
        g_enc1_armed = false;
        webcam_press(0); /* released before the hold: a short press */
    }
    if (g_consumed[enc]) {
        g_consumed[enc] = false;
    } else if (g_state == UI_RUNNING) {
        rt_release(enc + 1);
    }
}

void launcher_reboot_to_bootloader(const char *why)
{
    log_printf("ui: bootloader (%s)", why);
    if (g_state == UI_RUNNING) {
        persist_save_on_exit(g_scene->name);
        log_printf("ui: flushed %s before bootloader", g_scene->name);
        g_scene = NULL;
    } else if (g_state == UI_DISK) {
        disk_mode_leave("bootloader");
    } else if (g_state == UI_WEBCAM) {
        webcam_leave("bootloader");
    }
    screen_show_text("BOOTLOADER", why);
    screen_flush();
    /* The flash writes above are synchronous, so the save has happened
     * either way; this just gives the log lines documenting it a chance
     * to get out over USB first. */
    for (int i = 0; i < 50; i++) {
        console_drain_log();
        system_delay_ms(2);
    }
    system_delay_ms(20);
    system_enter_bootloader();
}

static void poll_shift(uint32_t now)
{
    bool shift = enc_shift_pressed() || console_virtual_down(16);
    if (shift && !g_shift) {
        g_shift_t0 = now;
        g_shift_used = false;
        g_shift_long = false;
    } else if (shift && !g_shift_used && !g_shift_long && now - g_shift_t0 >= SHIFT_HOLD_MS) {
        g_shift_long = true;
        if (g_state != UI_MENU) {
            show_menu();
        }
    } else if (!shift && g_shift) {
        g_boot_armed = false;
        if (!g_shift_used && !g_shift_long && g_state == UI_RUNNING) {
            set_picker(!g_picker, now);
            log_printf("ui: page picker %s", g_picker ? "open" : "closed");
        }
    }
    g_shift = shift;

    if (g_boot_armed && now - g_boot_t0 >= BOOT_HOLD_MS) {
        launcher_reboot_to_bootloader("Shift + 16");
    }
    if (g_picker && now - g_picker_t0 >= PICKER_TIMEOUT_MS) {
        set_picker(false, now);
        log_printf("ui: page picker closed");
    }
}

/* The vars save debounce (see PERSIST_DEBOUNCE_MS above). Call once per
 * main-loop pass, after rt_tick() (see launcher.h). */
void launcher_persist_poll(uint32_t now)
{
    if (g_state != UI_RUNNING) {
        return;
    }
    if (rt_vars_dirty() && now - g_last_var_save >= PERSIST_DEBOUNCE_MS) {
        /* Only clear dirty (and so stop retrying) once the save actually
         * happened: persist_save_vars() returns false both for a flash-
         * budget deferral and for the older "table full"/"doesn't fit"
         * reasons, logged either way -- the data stays safe in RAM and
         * this just tries again at the next debounce tick. */
        if (persist_save_vars(g_scene->name)) {
            rt_vars_clear_dirty();
        }
        g_last_var_save = now;
    }
    if (!g_heap_logged && now - g_run_t0 >= HEAP_LOG_MS) {
        g_heap_logged = true;
        log_printf("heap used %u / total %u", (unsigned)rt_heap_used(), (unsigned)rt_heap_total());
    }
}

/* Encoders 0-15 are rt's 1-16; physical and console-injected (console.h)
 * input are merged. Call once per main-loop pass, before rt_tick() (see
 * launcher.h). */
void launcher_poll(uint32_t now)
{
    if (g_state == UI_DISK && disk_mode_poll()) {
        leave_disk_mode("eject", false);
    }
    if (g_state == UI_WEBCAM && g_enc1_armed && now - g_enc1_t0 >= WEBCAM_HOLD_MS) {
        leave_webcam();
    }
    if (g_state == UI_WEBCAM) {
        webcam_poll(now);
    }
    poll_shift(now);
    for (int i = 0; i < ENC_COUNT; i++) {
        int hw = enc_read(i);
        mirror_turn(i, hw, false); /* the console reports virtual turns itself */
        int steps = hw + console_virtual_take_steps(i);
        if (steps != 0) {
            on_turn(i, steps);
        }
        bool down = enc_pressed(i) || console_virtual_down(i);
        if (down != g_down[i]) {
            g_down[i] = down;
            if (down) {
                on_press(i, now);
            } else {
                on_release(i);
            }
        }
    }
}

int launcher_mirror_mode(const char **name)
{
    *name = NULL;
    switch (g_state) {
        case UI_RUNNING:
            *name = g_scene ? g_scene->title : NULL;
            return MC_MODE_SCRIPT;
        case UI_ERROR:
            return MC_MODE_ERROR;
        case UI_DISK:
            return MC_MODE_DISK;
        case UI_WEBCAM:
            return MC_MODE_WEBCAM;
        default:
            *name = disk_selected()     ? DISK_ENTRY
                    : webcam_selected() ? WEBCAM_ENTRY
                                        : scene_info(g_sel).title;
            return MC_MODE_MENU;
    }
}

/* After an app ran (app_mode.h): the menu comes back on that app, or
 * with the error screen if it didn't load. */
void launcher_init(void)
{
    const char *stem = app_mode_last_stem();
    for (int i = 0; stem && i < scenes_count(); i++) {
        scene_info_t info = scene_info(i);
        if (info.source == SCENE_APP && strcmp(info.name, stem) == 0) {
            g_sel = i;
            break;
        }
    }
    show_menu();
    const char *err = app_mode_last_error();
    if (err) {
        g_state = UI_ERROR;
        screen_show_error(err);
        log_printf("ui: error screen");
    }
}
