#ifndef RT_API_CLOCK_H
#define RT_API_CLOCK_H
#include <stdbool.h>
#include <stdint.h>
#include "lua.h"
#include "rt_port.h"

/*
 * system.setUpdateRate and clock.*: a C port of the mock's clock
 * (oxi-e16-lua/test/e16mock.lua: clock, cb/safe/flush, pulse, run).
 *
 * Transport state (running source, tick, tempos) lives here across loads,
 * like the mock's: loading a script only turns listening off, drops queued
 * callbacks and stops an internal clock Lua started.
 */

/* Registers system and clock into L for a freshly loaded script: rate 0,
 * listening off, queue cleared, a Lua-started internal clock stopped (no
 * callback, like the mock's M.load). External transport carries on. */
void rt_api_clock_open(lua_State *L);

/* Called from rt_unload(): listening off, queue cleared, a Lua-started
 * internal clock stopped (same as a load, minus registering anything). */
void rt_api_clock_close(void);

/* Power-on state (stopped, tick 0, 120 BPM internal and external). For the
 * host harness's rt.reset(); the device gets it from static init. */
void rt_api_clock_reset(void);

/* Update scheduling: system.setUpdateRate's stored rate (0 = disabled), and
 * when system.update is next due (more than `rate` ms after the last one:
 * last + rate + 0.1, like the firmware's 10 kHz tick and the mock). */
int rt_api_clock_rate(void);
rt_time_t rt_api_clock_next_due(void);

/* When the next clock pulse is due: RT_TIME_NEVER if no clock runs; -RT_TIME_NEVER if
 * 0xF8 ticks are waiting (external MIDI clock). */
rt_time_t rt_api_clock_next_pulse(void);

/* Runs the clock up to `now`, in the mock's M.run order: each pass
 * flushes the queued transport callbacks, then fires whichever of pulse
 * and system.update is due first (the pulse on a tie), until nothing is
 * due at `now`. A failing update disables the rate. With no script loaded
 * the rate is 0 and listening is off, so this only moves the transport.
 *
 * One call is one M.run: rt_tick() passes flush_first = true. A host
 * harness that splits one M.run into several calls (to land on each event
 * at its own time) passes false for all but the first, so callbacks queued
 * by the previous call's last flush are delivered after the next event
 * fires, exactly where the mock's next pass would deliver them. */
void rt_api_clock_tick(rt_time_t now, bool flush_first);

/* External transport.
 *
 * Two ways to drive it, sharing everything except where pulse timing
 * comes from:
 *  - tick mode (the device): rt_api_clock_realtime() with the MIDI
 *    realtime bytes. Start/Continue arm the transport, and every 0xF8
 *    after that is one tick, so pulses follow the sender exactly. 0xF8 also
 *    feeds the tempo estimate getBpm() reports (an exponential moving
 *    average of the tick interval).
 *  - timer mode (the host harness's rt.ext_start etc., = the mock's
 *    M.extStart/extStop/extContinue): pulses come from a timer at the
 *    given BPM, exactly as in the mock.
 * Whichever starts the transport sets the mode. */
void rt_api_clock_realtime(uint8_t byte);
void rt_api_clock_ext_start(float bpm, bool bpm_is_int, bool have_bpm);
void rt_api_clock_ext_stop(void);
void rt_api_clock_ext_continue(void);

/* Read-only introspection for a host harness (the mock's M.listening,
 * M.res, M.tp and M.bpm). source: -1 stopped, 0 external, 2 internal
 * (started by Lua). internal_bpm: setInternalBpm's tempo, and whether Lua
 * gave it as an integer. */
bool rt_api_clock_listening(void);
int rt_api_clock_res(void);
int rt_api_clock_source(void);
float rt_api_clock_internal_bpm(bool *is_int);

#endif
