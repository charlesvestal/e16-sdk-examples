/*
 * launcher.*: feature detection, so a script can run on stock firmware
 * (where `launcher` doesn't exist at all) and on this one. See
 * rt_api_launcher.h for RT_LAUNCHER_VERSION.
 */
#include "rt_api_launcher.h"
#include "rt_port.h"

#include "lauxlib.h"

#include <stdbool.h>
#include <string.h>

/* The features this firmware has. An unlisted name is simply not found
 * below, so launcher.has() returns false for it. "disk" depends on the
 * moment: true only while the FAT volume is mounted. */
static const char *const FEATURES[] = {"draw", "ring", "audio", "usbaudio", "midiin"};
#define NUM_FEATURES (sizeof FEATURES / sizeof FEATURES[0])

static int l_launcher_has(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    if (strcmp(name, "disk") == 0) {
        lua_pushboolean(L, rt_port_disk_available());
        return 1;
    }
    for (size_t i = 0; i < NUM_FEATURES; i++) {
        if (strcmp(FEATURES[i], name) == 0) {
            lua_pushboolean(L, 1);
            return 1;
        }
    }
    lua_pushboolean(L, 0);
    return 1;
}

void rt_api_launcher_open(lua_State *L)
{
    lua_newtable(L);
    lua_pushstring(L, RT_LAUNCHER_VERSION);
    lua_setfield(L, -2, "version");
    lua_pushcfunction(L, l_launcher_has);
    lua_setfield(L, -2, "has");
    lua_setglobal(L, "launcher");
}
