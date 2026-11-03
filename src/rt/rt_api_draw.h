#ifndef RT_API_DRAW_H
#define RT_API_DRAW_H
#include "lua.h"

/* Registers the draw global and releases the canvas (rt_port_draw_release),
 * so a freshly loaded script starts with a blank, non-fullscreen canvas
 * rather than whatever the previous script left behind. */
void rt_api_draw_open(lua_State *L);

#endif
