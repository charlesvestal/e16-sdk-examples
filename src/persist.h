#ifndef PERSIST_H
#define PERSIST_H
#include <stdbool.h>
#include <stdint.h>

/*
 * vars and store on the E16's external SPI flash (storage.h), so they
 * survive a power cycle. The record format and the name-keyed slot table
 * live in src/disk/persist_slots.h/.c (portable over flash_ops_t, so
 * that part is host-testable: test/disk/persist_test.c); this file binds
 * that engine to the runtime's vars/store buffers (rt.h) and to
 * storage_init()/storage_size()'s device-only availability checks.
 *
 * Every script's vars and store are saved and loaded by scene name (not
 * by menu position, which a manifest reorder would change). persist_
 * slots.c hashes the name onto one of 63 slots in a reserve at the top of
 * the chip, 0xF80000-0xFFFFFF (persist_slots.h), with linear probing so
 * two names that hash to the same slot don't collide. The shared store
 * has its own fixed slot. Below that reserve is the FAT volume, and below
 * that the stock firmware's data; nothing here ever touches either.
 *
 * Slots are only claimed by a scene that has something to keep: saving
 * empty vars or an empty store for a scene that has never saved does
 * nothing. persist_reclaim() frees, at boot, the slots of scenes that no
 * longer exist ("persist: freed N slots").
 *
 * persist_save_*() skips the flash write entirely if the record is
 * byte-for-byte what was last written this boot (persist_slots.c's
 * in-RAM shadow, not a flash read), so repeatedly saving an unchanged
 * value costs nothing. persist_load_*() deserializes a valid record into
 * the runtime's buffer, or clears that buffer and logs why:
 *
 *   "persist: no saved state for <name> (...)"  blank slot, the old
 *       pre-reserve record format, a collision that another name won,
 *       or the slot table is full
 *   "persist: bad record for <name> (...)"      bad magic/kind/length or
 *       CRC mismatch
 *
 * Either way the script starts with its defaults. Call them only after
 * binding the runtime's buffers to the scene name and before rt_load(),
 * per rt.h's restore order -- rt_load() runs page.onInit() itself, so
 * restoring after it returns is too late.
 *
 * If persist_init() finds no flash chip, or one smaller than the reserve
 * needs, it logs that and every call here becomes a no-op (loads clear
 * the buffers, saves do nothing).
 *
 * persist_save_*() go through an erase-rate budget (flash_budget.h), so
 * a save can be refused for now rather than failing outright: they
 * return false in that case (logged once a minute, "persist: deferred
 * (flash budget)"), with the runtime's buffer and the caller's dirty
 * flag untouched -- launcher_persist_poll() only clears rt_vars_dirty()
 * when persist_save_vars() returns true, so a deferred vars save is
 * retried the same way on the next debounce tick. Also false (logged,
 * not retried by anything here) for the older reasons: the slot table
 * is full, or the record doesn't fit a sector.
 */

void persist_init(void);

bool persist_save_vars(const char *name);
bool persist_save_store(const char *name);
bool persist_save_shared(void);

void persist_load_vars(const char *name);
void persist_load_store(const char *name);
void persist_load_shared(void);

/* Dev aid (console.c's SysEx F0 7D 45 07 <index> F7, only built with
 * E16_DEV_CONSOLE): zeroes the stored CRC of name's saved vars record in
 * place (a flash write only needs to turn 1 bits into 0, so this doesn't
 * need an erase first) so the next load of that scene finds a CRC
 * mismatch and logs "persist: bad record for <name>". A name with no
 * slot is logged and ignored. */
void persist_debug_corrupt_vars(const char *name);

/* At boot, once the scene list is built and only if the FAT volume
 * mounted (else every disk script's state would look orphaned): frees
 * the slots whose name is neither a built-in nor a file in /scripts
 * (scenes_name_exists()). Logs "persist: freed N slots" when it frees
 * any. */
void persist_reclaim(void);

#endif
