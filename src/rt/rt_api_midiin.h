#ifndef RT_API_MIDIIN_H
#define RT_API_MIDIIN_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lua.h"
#include "rt_port.h"

/*
 * MIDI input for scripts, in the shape of OXI's stock API 1.3.0
 * (external/oxi-e16-lua/docs/e16-lua-notes.md, "MIDI in"): the script
 * turns listening on with midi.listen{...} and defines callbacks in the
 * midi table, like page.onInit:
 *
 *   midi.listen{notes=, cc=, other=, channel=, port=}
 *       notes, cc: booleans, as on the stock firmware; other (a launcher
 *       extension): pitch bend, channel and poly pressure, program change.
 *       If none of the three is given, all three are on; otherwise only
 *       the ones set true. channel: 0-15 (sendCC's numbering), nil = all.
 *       port: RT_MIDI_IN_USB or RT_MIDI_IN_TRS, nil or 0 = both. Each
 *       call replaces the previous filter.
 *   midi.listen(true) / midi.listen()   everything, from both ports
 *   midi.listen(false)                  stop
 *
 *   function midi.onNote(port, ch, note, vel)   vel 0 = note off (a real
 *       Note Off arrives this way too, its release velocity dropped)
 *   function midi.onCC(port, ch, cc, value)
 *   function midi.onMessage(port, status, d1, d2)   (launcher extension)
 *       status includes the channel, as midi.sendMidi takes it; d2 is 0
 *       for two-byte messages (program change, channel pressure).
 *
 * Listening starts off at every load and stops at unload. An error in
 * one of the callbacks is reported like any callback error (rt_call())
 * and, as on the stock firmware, stops listening: the script calls
 * midi.listen again to resume.
 *
 * SysEx, system common and realtime bytes never reach these callbacks:
 * the platform keeps the dev console's SysEx for itself, and clock and
 * transport go to clock.* (rt_midi_realtime()).
 *
 * Delivery: rt_api_midiin_push() puts a message that passes the filter
 * into a bounded single-producer/single-consumer ring (512 bytes of the
 * script's Lua heap, from its first midi.listen on) (no locks: the
 * producer only writes the head and the drop count, the consumer only
 * the tail), and rt_api_midiin_tick() (from rt_tick(), on the main loop)
 * calls the script for at most RT_MIDI_IN_PER_TICK of them, so a flood
 * can't hold up the rest of the loop. What doesn't fit in the ring is
 * dropped and counted; the count is logged at most once a second while
 * drops continue ("midi in: N messages dropped ...") and at unload.
 */
#define RT_MIDI_IN_USB 1 /* same number as USB out (midi.sendMidi) */
#define RT_MIDI_IN_TRS 2 /* the TRS MIDI input (out port 2 is TRS 1) */

#define RT_MIDI_IN_QUEUE 128 /* messages; a power of two */
#define RT_MIDI_IN_PER_TICK 16

/* Adds midi.listen to the midi global (rt_api_out_open() first). */
void rt_api_midiin_open(lua_State *L);

/* rt_unload(): listening off, the queue emptied, drops logged. */
void rt_api_midiin_close(void);

/* One received message (status byte first, running status already
 * resolved), port RT_MIDI_IN_*. Anything not a channel voice message,
 * or not asked for, is ignored. */
void rt_api_midiin_push(int port, const uint8_t *bytes, size_t len);

/* Calls the script for up to RT_MIDI_IN_PER_TICK queued messages. */
void rt_api_midiin_tick(rt_time_t now);

/* Introspection for the host harness. */
unsigned rt_api_midiin_dropped(void);  /* since the script loaded */
unsigned rt_api_midiin_pending(void);  /* queued, not yet delivered */
bool rt_api_midiin_listening(void);

#endif
