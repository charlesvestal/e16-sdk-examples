/*
 * The app-mode boot decision (src/apps/app_boot.c): a "run this app"
 * record is honoured only right after the launcher's own reset into app
 * mode, never after a reflash, a reset pin, a power-on or a watchdog.
 */
#include <stdio.h>

#include "app_boot.h"

static int failures;

#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            printf("ok   ");                                                                       \
        } else {                                                                                   \
            printf("FAIL ");                                                                       \
            failures++;                                                                            \
        }                                                                                          \
        printf(__VA_ARGS__);                                                                       \
        printf("\n");                                                                              \
    } while (0)

/* What NVIC_SystemReset() leaves on an STM32F4/GD32F4: software reset,
 * plus the reset pin it drives. */
#define SOFT (APP_BOOT_CSR_SFTRSTF | APP_BOOT_CSR_PINRSTF)

int main(void)
{
    const uint32_t check = 0x12345678u;
    const uint32_t tok = app_boot_token(check);

    CHECK(tok != 0, "the token is never 0");
    CHECK(app_boot_token(check) == tok, "the token is a function of the record");
    CHECK(app_boot_token(check ^ 1u) != tok, "another record has another token");
    CHECK(app_boot_token(0x7A11C0DEu) != 0, "even the record whose check equals the salt");

    /* the launch the menu asked for */
    CHECK(app_boot_decide(true, check, tok, SOFT) == APP_BOOT_APP, "launcher reset with its token: app mode");
    CHECK(app_boot_decide(true, check, tok, APP_BOOT_CSR_SFTRSTF) == APP_BOOT_APP,
          "SFTRSTF alone (no PINRSTF) is a software reset too");

    /* nothing asked */
    CHECK(app_boot_decide(false, check, tok, SOFT) == APP_BOOT_MENU_NO_REQUEST, "no run record: menu");
    CHECK(app_boot_decide(false, 0, 0, APP_BOOT_CSR_PORRSTF) == APP_BOOT_MENU_NO_REQUEST, "power-on: menu");

    /* the reported bug: an app ran (its boot consumed the token), the E16 was
     * reflashed, and the bootloader's own software reset started the new
     * launcher with a run record still in RAM */
    CHECK(app_boot_decide(true, check, 0, SOFT) == APP_BOOT_MENU_NO_TOKEN, "reflash (no token): menu");
    CHECK(app_boot_decide(true, check, 0, APP_BOOT_CSR_PINRSTF) == APP_BOOT_MENU_NO_TOKEN,
          "reset pin, no token: menu");
    CHECK(app_boot_decide(true, check, app_boot_token(check + 1u), SOFT) == APP_BOOT_MENU_NO_TOKEN,
          "a token for another record: menu");
    CHECK(app_boot_decide(true, check, 0xFFFFFFFFu, SOFT) == APP_BOOT_MENU_NO_TOKEN, "garbage token: menu");

    /* the token is there, but something else reset the E16 in between */
    CHECK(app_boot_decide(true, check, tok, APP_BOOT_CSR_PINRSTF) == APP_BOOT_MENU_NOT_SOFT,
          "token, but only the reset pin: menu");
    CHECK(app_boot_decide(true, check, tok, 0) == APP_BOOT_MENU_NOT_SOFT, "token, no reset flags at all: menu");
    CHECK(app_boot_decide(true, check, tok, SOFT | APP_BOOT_CSR_PORRSTF) == APP_BOOT_MENU_NOT_SOFT,
          "token, software reset plus power-on: menu");
    CHECK(app_boot_decide(true, check, tok, SOFT | APP_BOOT_CSR_BORRSTF) == APP_BOOT_MENU_NOT_SOFT,
          "token, brown-out: menu");
    CHECK(app_boot_decide(true, check, tok, SOFT | APP_BOOT_CSR_IWDGRSTF) == APP_BOOT_MENU_NOT_SOFT,
          "token, independent watchdog: menu");
    CHECK(app_boot_decide(true, check, tok, SOFT | APP_BOOT_CSR_WWDGRSTF) == APP_BOOT_MENU_NOT_SOFT,
          "token, window watchdog: menu");
    CHECK(app_boot_decide(true, check, tok, SOFT | APP_BOOT_CSR_LPWRRSTF) == APP_BOOT_MENU_NOT_SOFT,
          "token, low-power reset: menu");

    /* the NULL guard's size: the largest power of two at or below the text's alias */
    CHECK(app_boot_null_guard_log2(true, 0x81c8u) == 15u, "null guard below text at 0x81c8: 32 KB");
    CHECK(app_boot_null_guard_log2(true, 0x8000u) == 15u, "text at 0x8000 exactly: 32 KB (up to, not over, the text)");
    CHECK(app_boot_null_guard_log2(true, 0x7fffu) == 14u, "text at 0x7fff: 16 KB");
    CHECK(app_boot_null_guard_log2(true, 0x1b000u) == 16u, "text at 0x1b000: 64 KB");
    CHECK(app_boot_null_guard_log2(true, 32u) == 5u, "text at 32: the smallest region");
    CHECK(app_boot_null_guard_log2(true, 31u) == 0u, "text under 32: no region");
    CHECK(app_boot_null_guard_log2(false, 0x200081c8u) == APP_BOOT_NULL_GUARD_PLAIN, "no remap: 1 KB");
    for (uint32_t e = 32u; e < 0x1c000u; e += 0x1234u) {
        uint32_t n = app_boot_null_guard_log2(true, e);
        CHECK((1u << n) <= e && (1u << (n + 1u)) > e, "exec 0x%x: 2^%u fits under it, 2^%u wouldn't", (unsigned)e,
              (unsigned)n, (unsigned)(n + 1u));
    }

    if (failures) {
        printf("%d FAILED\n", failures);
        return 1;
    }
    printf("ALL PASSED\n");
    return 0;
}
