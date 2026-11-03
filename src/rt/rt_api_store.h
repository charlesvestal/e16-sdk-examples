#ifndef RT_API_STORE_H
#define RT_API_STORE_H
#include "lua.h"
#include "rt_scene.h"

/* Registers the store global (use/size/capacity/data) into L and sets
 * store.data from the scene buffer (rt_api_store.c's g_scene_buf): a
 * fresh table deserialized from whatever was last committed for this
 * scene name, or empty if the scene name just changed or nothing was
 * saved yet. */
void rt_api_store_open(lua_State *L, const rt_scene_t *scene);

/* Called from rt_unload(), with L still open (before lua_close): commits
 * the live store.data table into the currently selected buffer (scene or
 * shared, per the last store.use()) as the tagged-byte encoding. If the
 * table's mock-style ssize() exceeds 1024 bytes, logs "store over 1024
 * bytes" and leaves the buffer from the previous successful commit
 * untouched (rt_port_log, same as other runtime errors). */
void rt_api_store_close(lua_State *L);

/* Rebuilds store.data in place from the currently selected buffer,
 * without a full reload. Used after rt_store_deserialize() replaces the
 * scene buffer's bytes (rt.persist_roundtrip() in the host harness), so
 * the restored content becomes visible to the script again. */
void rt_api_store_sync(lua_State *L);

/* Like rt_api_var_bind_scene(): makes the scene buffer belong to `name`
 * without clearing it, so a platform can call rt_store_deserialize()
 * with a previously saved buffer and then rt_load() that same scene
 * without rt_api_store_open() wiping it back out (it only clears the
 * buffer when the incoming scene name differs from the one it already
 * holds). Step 1 of rt.h's restore order (bind, deserialize, rt_load). */
void rt_api_store_bind_scene(const char *name);

#endif
