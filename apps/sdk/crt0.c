/*
 * The app's entry point, placed first in the text by app.ld. The loader has
 * already copied .data, zeroed .bss, rebased the GOT and set r9 to it, so
 * there's nothing to set up: hand the api table (r0) to app_main() and, if
 * it returns, leave through api->exit() (which doesn't return).
 */
#include "e16_app.h"

__attribute__((section(".text.e16_entry"), used, noreturn))
void _e16_start(const e16_api_t *api)
{
    int code = app_main(api);
    api->exit(code);
}
