#ifndef LAUNCHER_H
#define LAUNCHER_H
#include <stdint.h>

/*
 * The launcher UI/state machine: menu -> running script (or error screen)
 * -> menu, Shift gestures, the page picker, encoder dispatch, the
 * bootloader gesture, and when vars/store are loaded and saved
 * (persist.h). main.c does bring-up and runs the main loop; each pass
 * calls, in order:
 *
 *   launcher_poll()          encoder/Shift input; may start or stop a
 *                            script and routes events to it (rt_turn() etc.)
 *   rt_tick()                the runtime: system.update, clock, holds
 *   launcher_persist_poll()  the debounced vars save
 *
 * launcher_persist_poll() goes last so a save in this pass covers every
 * var change made in it -- by a turn launcher_poll() routed to the script
 * as well as by the system.update or clock callbacks rt_tick() ran --
 * rather than leaving the newest ones for a later pass.
 */
void launcher_init(void);
void launcher_poll(uint32_t now);
void launcher_persist_poll(uint32_t now);

/*
 * Reboot into the bootloader (the Shift + hold-16 gesture, and console.c's
 * SysEx 05): saves a running script's vars/store first, the same save
 * the back-to-menu path does, so a var tweaked just before isn't lost;
 * shows "BOOTLOADER" with `why` under it; lets the log drain; then enters
 * the bootloader. Doesn't return. This does flash writes, so it must
 * never be called from a fault handler (main.c's fault handlers go
 * straight to system_enter_bootloader(): a flash write from a handler
 * that may have faulted in the middle of a flash op is asking for
 * trouble).
 */
void launcher_reboot_to_bootloader(const char *why);

/* What the launcher is doing, for the mirror (src/mirror/mirror_core.h's
 * mc_mode_t), and a name: the running script's title, or in the menu the
 * selected entry's (NULL otherwise). */
int launcher_mirror_mode(const char **name);

#endif
