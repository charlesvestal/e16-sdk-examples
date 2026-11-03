#ifndef RT_API_OUT_H
#define RT_API_OUT_H
#include "lua.h"

/* Registers the midi/leds/slots/page globals (output APIs) into L.
 * system.setUpdateRate lives in rt_api_clock.c instead. */
void rt_api_out_open(lua_State *L);

#endif
