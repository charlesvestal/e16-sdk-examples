#ifndef RT_API_RING_H
#define RT_API_RING_H
#include <stdbool.h>
#include "lua.h"

/* Registers the ring global. Resets every ring's custom-mode flag (a
 * freshly loaded script starts with leds.updateByIndex working on every
 * ring, like the stock firmware). */
void rt_api_ring_open(lua_State *L);

/* Called from rt_unload(): rings a script left in custom mode don't stay
 * that way for whatever loads next (rt_api_ring_open() would reset them
 * anyway, but this also lets the port release them right away rather
 * than waiting for the next load). */
void rt_api_ring_close(void);

/* ring 1-16. False (including out-of-range) for a ring leds.updateByIndex
 * is still allowed to drive. */
bool rt_api_ring_is_custom(int ring);

/* Takes `ring` out of custom mode, telling the port (rt_port_ring_release)
 * so it can restore whatever it was showing before. Used by ring.release()
 * and by leds.reset() (rt_api_out.c) on a custom ring. A no-op if the ring
 * wasn't in custom mode, or out of range. */
void rt_api_ring_release(int ring);

#endif
