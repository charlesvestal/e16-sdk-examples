/*
 * The app-mode boot decision: app_boot.h.
 */
#include "app_boot.h"

#define TOKEN_SALT 0x7A11C0DEu

uint32_t app_boot_token(uint32_t record_check)
{
    uint32_t t = record_check ^ TOKEN_SALT;
    return t ? t : TOKEN_SALT;
}

app_boot_t app_boot_decide(bool run_request, uint32_t check, uint32_t token, uint32_t csr)
{
    if (!run_request) {
        return APP_BOOT_MENU_NO_REQUEST;
    }
    if (token == 0 || token != app_boot_token(check)) {
        return APP_BOOT_MENU_NO_TOKEN;
    }
    const uint32_t power_or_watchdog = APP_BOOT_CSR_BORRSTF | APP_BOOT_CSR_PORRSTF | APP_BOOT_CSR_IWDGRSTF |
                                       APP_BOOT_CSR_WWDGRSTF | APP_BOOT_CSR_LPWRRSTF;
    if (!(csr & APP_BOOT_CSR_SFTRSTF) || (csr & power_or_watchdog)) {
        return APP_BOOT_MENU_NOT_SOFT;
    }
    return APP_BOOT_APP;
}

uint32_t app_boot_null_guard_log2(bool remapped, uint32_t exec)
{
    if (!remapped) {
        return APP_BOOT_NULL_GUARD_PLAIN;
    }
    if (exec < 32u) {
        return 0;
    }
    uint32_t n = 5;
    while (n < 31u && (1u << (n + 1u)) <= exec) {
        n++;
    }
    return n;
}
