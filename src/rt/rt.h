#ifndef RT_H
#define RT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lua.h"
#include "rt_scene.h"

/* Load (or reload, like a scene re-entry) a script and run page.onInit().
 * Returns false and logs the error if it doesn't load. */
bool rt_load(const rt_scene_t *scene, const char *src, size_t len);

/* rt_load() with the source coming from a lua_Reader instead of one
 * buffer: everything else (the state, the APIs, the chunk name, text
 * only, page.onInit(), the error handling) is the same. The reader is
 * called repeatedly from inside the Lua parser -- possibly deep in its
 * recursion, so it should use little stack -- until it returns NULL or a
 * size of 0. A disk script is read this way a sector at a time, so its
 * whole source is never in RAM (src/scenes.c). */
bool rt_load_reader(const rt_scene_t *scene, lua_Reader reader, void *ud);
void rt_unload(void);
lua_State *rt_state(void);

/* Call a callback stored at global path "a.b" (e.g. "page.onInit") with nargs
 * already pushed; `path` must be a string literal (it's kept for the error
 * summary). Returns false on error or if absent. Errors are rate-limited
 * per path: the first one since the script loaded is logged in full, later
 * ones are counted and summarized ("rt: <path> failed N more times (T
 * total)") every 10 s while they keep coming and when the script unloads. */
bool rt_call(const char *path, int nargs);

/* Like rt_call, for clock callbacks: an error ends only that call. The
 * first one since the script loaded is logged ("clock callback error: ...");
 * later ones are only counted (and summarized like rt_call's). Returns
 * false on error or if absent. */
bool rt_call_clock(const char *path, int nargs);

/* Clock callback errors since the script loaded (the mock's M.cerrs). */
unsigned rt_clock_error_count(void);

/* Errors in any callback (rt_call or rt_call_clock) since the script
 * loaded; 0 after a load or a failed load. A failing system.update, which
 * also stops further updates, counts too. The screen shows a "!" in the
 * title bar while this is nonzero. */
unsigned rt_error_count(void);

/* External MIDI realtime bytes (0xF8 clock, 0xFA start, 0xFB continue,
 * 0xFC stop); others are ignored. Call from the main loop, not an ISR:
 * the bytes only update state, and pulses and callbacks follow on the
 * next rt_tick(). Works with or without a script loaded. */
void rt_midi_realtime(uint8_t byte);

/* Any other received MIDI message (status byte first, running status
 * resolved), for midi.onNote/onCC/onMessage: port RT_MIDI_IN_USB (1) or
 * RT_MIDI_IN_TRS (2) (rt_api_midiin.h). Call from the main loop with
 * everything the platform doesn't keep for itself (the dev console's
 * SysEx); only channel voice messages the script asked for with
 * midi.listen are queued, and the script sees them on a later rt_tick(),
 * at most RT_MIDI_IN_PER_TICK per call. */
void rt_midi_in(int port, const uint8_t *bytes, size_t len);

/* Advance runtime time to rt_port_now(): system.update ticks, clock pulses
 * and encoder-hold detection. */
void rt_tick(void);

/* Last error text ("" if none), for the error screen. */
const char *rt_last_error(void);

/* Physical events. enc 1-16. The runtime maps them through the scene
 * wiring (rt_api_ctl.c) to the ids controller.onEncoder* sees. */
void rt_turn(int enc, int increment);
void rt_press(int enc);
void rt_release(int enc);            /* held time is measured from rt_press */
void rt_show_page(int page);         /* 1-based; calls page.onPageChange(prev, page) */
int rt_page(void);

/* Sets the current page controller.getPage() reports without calling
 * page.onPageChange. For the host harness's mock bridge
 * (test/host/e16mock.lua: restoring a scene's page across a reload
 * without replaying a transition the script never asked for). */
void rt_set_page_silent(int page);

/*
 * var and store persistence.
 *
 * var's 32 slots live in a C array outside the Lua heap (rt_api_var.c),
 * and store's data is committed to a tagged-byte buffer on unload
 * (rt_api_store.c). Both are kept in RAM only for the scene they're bound
 * to, identified by rt_scene_t.name: rt_load() of the bound name keeps
 * them, rt_load() of a different name clears both and binds them to it,
 * because the runtime has no notion of script identity beyond the scene
 * name.
 *
 * Restoring saved state (src/ui/launcher.c's start_script() does this):
 *
 *   1. rt_api_var_bind_scene(name) and rt_api_store_bind_scene(name)
 *      (rt_api_var.h, rt_api_store.h): bind the buffers to the scene about
 *      to load, without clearing them.
 *   2. rt_vars_deserialize(), rt_store_deserialize() and
 *      rt_store_shared_deserialize() with the saved bytes (or NULL, 0 when
 *      there are none, which clears them).
 *   3. rt_load() of that scene: it sees the bound name, keeps the buffers,
 *      and page.onInit() (run inside rt_load()) already sees the restored
 *      values. Deserializing after rt_load() returns is too late: onInit
 *      has registered its defaults by then.
 *
 * Saving: rt_vars_serialize() any time (vars are always live); the store
 * buffers after rt_unload(), which commits the live store.data table into
 * them.
 *
 * store.use("shared") (rt_api_store.c) selects a second buffer that
 * isn't keyed by scene name at all -- it's the same buffer regardless of
 * which scene is loaded -- so it has its own serialize/deserialize pair
 * below rather than sharing rt_store_serialize/deserialize's.
 *
 * The buffers are opaque to the caller: an internal, versionless encoding
 * private to rt_api_var.c/rt_api_store.c, at most RT_VARS_SERIALIZED_MAX /
 * RT_STORE_SERIALIZED_MAX bytes. `*_serialize` returns the number of bytes
 * written (0 is a valid, empty store), or RT_SERIALIZE_FAILED if they
 * don't fit in `max` (nothing is written in that case). `*_deserialize`
 * always replaces the whole array/buffer (clears it first), even if
 * `in`/`len` describes less than everything that was there.
 */
#define RT_VAR_MAX 32
#define RT_VAR_NAME_MAX 16
#define RT_VARS_SERIALIZED_MAX (1 + RT_VAR_MAX * (1 + RT_VAR_NAME_MAX + 4))
#define RT_STORE_SERIALIZED_MAX 2048
#define RT_SERIALIZE_FAILED ((size_t)-1)

size_t rt_vars_serialize(uint8_t *out, size_t max);
void rt_vars_deserialize(const uint8_t *in, size_t len);
size_t rt_store_serialize(uint8_t *out, size_t max);
/* True if serialized bytes (as the two above wrote them) hold nothing: no
 * vars, an empty store. A platform needn't keep those for a scene that
 * never saved anything (persist.c). */
bool rt_vars_serialized_empty(const uint8_t *buf, size_t len);
bool rt_store_serialized_empty(const uint8_t *buf, size_t len);
void rt_store_deserialize(const uint8_t *in, size_t len);
size_t rt_store_shared_serialize(uint8_t *out, size_t max);
void rt_store_shared_deserialize(const uint8_t *in, size_t len);

/*
 * Save-debounce signal for a platform persisting vars: true once any var
 * has changed (a new registration, var.set, delete, deleteAll, or the host
 * harness's rt_api_var_put/remove/clear) since the flag was last cleared. A
 * platform that saves vars some time after the last change (rather than
 * on every change) polls this and calls rt_vars_clear_dirty() once it
 * has started (or finished) a save, so only changes after that save set
 * it again.
 */
bool rt_vars_dirty(void);
void rt_vars_clear_dirty(void);

#endif
