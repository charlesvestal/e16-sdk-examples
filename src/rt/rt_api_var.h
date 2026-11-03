#ifndef RT_API_VAR_H
#define RT_API_VAR_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lua.h"
#include "rt_scene.h"

/* Registers the var global (register/get/set/delete/deleteAll) into L.
 *
 * Vars live in a C array outside the Lua heap (rt_api_var.c's g_vars), so
 * they survive rt_load() of the *same* script (identified by
 * scene->name): this only clears the array when the incoming scene name
 * differs from the one the array currently holds. Persistence across
 * power cycles is the platform's job: it binds the slots to the scene
 * (rt_api_var_bind_scene() below) and calls rt_vars_deserialize()
 * (rt.h) with a saved copy before rt_load(), per rt.h's restore order. */
void rt_api_var_open(lua_State *L, const rt_scene_t *scene);

/* Direct access to the slots, bypassing the Lua API. The first four are
 * for a host harness (test/host/rt_module.c, which presents the slots as
 * the mock's M.store); rt_api_var_bind_scene() is also the device's
 * restore step (src/ui/launcher.c, rt.h's restore order):
 *  - rt_api_var_at(i, ...): slot i (0-31); false if unused.
 *  - rt_api_var_put(): creates or overwrites a var (false: name too long
 *    or all 32 slots used), like a write into the mock's M.store table.
 *  - rt_api_var_remove(), rt_api_var_clear(): one var / all of them.
 *  - rt_api_var_bind_scene(): makes the slots belong to `name` without
 *    clearing them, so the next rt_load() of that scene keeps them (the
 *    device binds before deserializing a saved copy; the harness binds to
 *    emulate the mock's one slot shared by whatever script loads next). */
bool rt_api_var_at(int i, const char **name, size_t *len, int32_t *value);
bool rt_api_var_put(const char *name, size_t len, int32_t value);
void rt_api_var_remove(const char *name, size_t len);
void rt_api_var_clear(void);
void rt_api_var_bind_scene(const char *name);

#endif
