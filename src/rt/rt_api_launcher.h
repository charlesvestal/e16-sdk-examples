#ifndef RT_API_LAUNCHER_H
#define RT_API_LAUNCHER_H
#include "lua.h"

/* Bumped when the Lua API surface changes in a way scripts might care
 * about; launcher.version reports it as a string. */
#define RT_LAUNCHER_VERSION "1.2"

/* Registers the launcher global: launcher.version and
 * launcher.has(name) -> bool. The feature set is a plain table in
 * rt_api_launcher.c, so later tasks (audio, disk) flip their entry to
 * true once they land instead of adding a new mechanism. */
void rt_api_launcher_open(lua_State *L);

#endif
