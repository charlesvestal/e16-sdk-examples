#ifndef APP_BOOT_H
#define APP_BOOT_H
#include <stdbool.h>
#include <stdint.h>

/*
 * Whether a boot takes the "run this app" record (boot_records.h) or
 * boots the launcher's menu. Pure: no hardware, so the host tests it
 * (test/apps/app_boot_test.c); app_mode.c feeds it the device's state.
 *
 * The record survives any reset that keeps RAM powered, including the
 * bootloader's after a reflash, so on its own it can't tell "the menu
 * asked for this app a moment ago" from "an app ran, and something else
 * reset the E16 since" (a reflash, the reset pin, a stale record a newer
 * firmware misreads). So the launcher writes a launch token right before
 * its own reset into app mode, bound to that exact record, and every boot
 * consumes the token whatever it decides. The record is honoured only if
 *   - it is valid and asks to run an app,
 *   - the token is there and matches that record, and
 *   - the reset was a software reset (RCC_CSR's SFTRSTF; NVIC_SystemReset()
 *     also drives the reset pin, so PINRSTF may be set too), with no
 *     power-on, brown-out or watchdog reset flag next to it.
 * Anything else boots the menu (and the caller clears the record).
 *
 * The reset flags are RCC_CSR's (STM32F4 numbering, same on the GD32F405);
 * the boot clears them (RMVF) after reading, so each boot sees only the
 * reset that started it.
 */
#define APP_BOOT_CSR_BORRSTF (1u << 25)
#define APP_BOOT_CSR_PINRSTF (1u << 26)
#define APP_BOOT_CSR_PORRSTF (1u << 27)
#define APP_BOOT_CSR_SFTRSTF (1u << 28)
#define APP_BOOT_CSR_IWDGRSTF (1u << 29)
#define APP_BOOT_CSR_WWDGRSTF (1u << 30)
#define APP_BOOT_CSR_LPWRRSTF (1u << 31)

/* Token value for a record whose check word is record_check: what the
 * launcher writes, and what a boot expects. Never 0. */
uint32_t app_boot_token(uint32_t record_check);

typedef enum {
    APP_BOOT_MENU_NO_REQUEST,  /* no valid "run" record: nothing to do */
    APP_BOOT_APP,              /* honour it: app mode */
    APP_BOOT_MENU_NO_TOKEN,    /* a "run" record without its token (e.g. a reflash) */
    APP_BOOT_MENU_NOT_SOFT,    /* the token is there, but the reset wasn't the launcher's */
} app_boot_t;

/* run_request: the app record is valid and in the "run" state; check: its
 * check word; token: what the token record held (0 if none); csr: RCC_CSR
 * as the boot found it. */
app_boot_t app_boot_decide(bool run_request, uint32_t check, uint32_t token, uint32_t csr);

/*
 * The no-access MPU region app mode puts at address 0 (app_mode.c), as
 * log2 of its size in bytes (an MPU region is a power of two, at least
 * 32 bytes, aligned to its size). With SRAM1 remapped at 0 (the app's
 * text runs from its alias there, at exec), 0 aliases the launcher's
 * .data: an app's NULL-pointer write would silently land in it. So the
 * region covers the largest power of two at or below exec (never the
 * text itself; 0 = no region, if exec is under 32). Without the remap,
 * 0 aliases the flash (boot mapping): a 1 KB region (10) catches NULL
 * accesses there too.
 */
#define APP_BOOT_NULL_GUARD_PLAIN 10u
uint32_t app_boot_null_guard_log2(bool remapped, uint32_t exec);

#endif
