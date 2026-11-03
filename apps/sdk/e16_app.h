#ifndef E16_APP_H
#define E16_APP_H
/*
 * Native E16 apps. Include this, define app_main(), and build with
 * e16_add_app() (apps/sdk/cmake/e16app.cmake), which links with
 * apps/sdk/app.ld and the entry stub crt0.c and turns the ELF into an
 * .e16app with tools/mkapp.py.
 *
 * The rules (mkapp.py enforces the ones a build can check):
 *   - Freestanding C: no libc (no malloc, printf, stdio); libgcc's helpers
 *     (64-bit division, double arithmetic) are linked in. memcpy/memset
 *     must come from the app if the compiler emits calls to them.
 *   - No C++ global constructors, no .init_array.
 *   - Globals, statics and initialized pointers are fine: the loader
 *     rebases the GOT and the pointer fixups.
 *   - Everything goes through the api table. An app is trusted,
 *     privileged code (not sandboxed): the table has no flash function
 *     and the launcher never writes the internal flash, but nothing but
 *     convention stops an app touching hardware directly. Don't. App mode
 *     makes the internal-flash controller read-only with the MPU, so a
 *     stray write there faults back to the launcher -- a guard against
 *     accidents, not a sandbox (a privileged app could undo it).
 *   - The app runs on the launcher's main stack; keep big buffers in
 *     globals, not locals.
 */
#include "e16_api.h"

int app_main(const e16_api_t *api);

#endif
