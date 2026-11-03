#ifndef PERSIST_SLOTS_H
#define PERSIST_SLOTS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "flash_ops.h"

/*
 * Name-keyed persistence slots on SPI flash, in a reserve at the top of
 * the 16 MB chip -- strictly above the FAT volume this flash also holds
 * (the two never overlap, and nothing here ever touches an address below
 * PERSIST_RESERVE_BASE):
 *
 *   0xF80000-0xFFFFFF   the persistence reserve, 512 KB = 128 sectors of
 *                       4 KB, grouped into PERSIST_SLOT_COUNT slots of two
 *                       sectors each (vars, store):
 *
 *     slot 0             the shared store (persist_slots_save_shared()/
 *                        _load_shared(): a fixed address, no hashing;
 *                        slot 0's vars sector goes unused)
 *     slot 1..63         scenes: persist_slots_save()/_load() hash the
 *                        scene name onto one of these with linear
 *                        probing, so two names that hash the same don't
 *                        collide
 *
 * Each slot's two sectors (vars, store) hold their records append-only:
 * a save writes a new record after whatever's already in the sector,
 * only erasing (and starting over at offset 0) when the next record
 * wouldn't fit. A load reads the *last* valid record in the sector.
 * Probing a slot only ever looks at its sectors' *first* record's header
 * (vars, or store if vars was never written) -- the name a slot belongs
 * to is fixed the moment its first record is written, so that's enough
 * to tell apart "free" (never written), "already this name's" and
 * "someone else's" without needing a separate index. A name that probes
 * every scene slot without finding either of the first two is logged and
 * the save/load that asked for it does nothing (the table is full). A
 * scene that never saved anything doesn't take a slot
 * (persist_slots_save_unless_empty()), and slots of scenes that no
 * longer exist are freed at boot (persist_slots_reclaim()). See
 * persist_slots.c for the record format and the append/erase rule.
 *
 * Portable over flash_ops_t (flash_ops.h), so this is host-testable
 * (test/disk/persist_test.c's fake flash) the same way blockdev.c is;
 * src/persist.c binds it to the runtime's vars/store serialization
 * (rt.h) and the device's storage_init()/storage_size() checks.
 */
#define PERSIST_RESERVE_BASE 0xF80000u
#define PERSIST_RESERVE_SIZE 0x80000u /* 512 KB = 128 sectors of 4 KB */
#define PERSIST_SLOT_COUNT 64u        /* slot 0: shared store; 1..63: scenes */
#define PERSIST_SLOT_SIZE (2u * FLASH_SECTOR_SIZE) /* one sector each for vars, store */
#define PERSIST_SHARED_SLOT 0u
#define PERSIST_NAME_LEN 32u /* RT_SCENE_NAME_MAX + NUL, NUL-padded (persist.c asserts this) */

_Static_assert((uint64_t)PERSIST_SLOT_COUNT * PERSIST_SLOT_SIZE == PERSIST_RESERVE_SIZE,
               "the slot table must exactly fill the reserve");

/*
 * Records are append-only within a slot's sector: a save writes a new
 * record *after* whatever's already there (header "magic+len+kind+crc+
 * name+commit", then the payload, then padding up to a
 * PERSIST_RECORD_ALIGN-byte boundary -- persist_slots.c asserts the
 * struct matches PERSIST_HDR_SIZE and that `commit` sits at
 * PERSIST_COMMIT_OFFSET). The header's `commit` word is written in a
 * separate, final flash write, after the rest of the header and the
 * payload: a reset between those writes leaves `commit` at its erased
 * value, so the record reads back as uncommitted (a torn write) rather
 * than as a corrupt one, and the previous record (if any) is used
 * instead. A sector is only erased, and the next record written at
 * offset 0, when the next record wouldn't otherwise fit.
 *
 * Exposed so callers (and host tests) can size a payload buffer that's
 * guaranteed to fit a sector, and compute how many records of a given
 * payload size fit per sector / how many erases a run of saves costs.
 */
#define PERSIST_HDR_SIZE 48u
#define PERSIST_RECORD_ALIGN 16u
#define PERSIST_COMMIT_OFFSET (PERSIST_HDR_SIZE - 4u) /* `commit`: the header's last 4 bytes */
#define PERSIST_MAX_PAYLOAD (FLASH_SECTOR_SIZE - PERSIST_HDR_SIZE)

_Static_assert(PERSIST_HDR_SIZE % PERSIST_RECORD_ALIGN == 0,
               "the header size must itself be record-aligned");

/* A record's padded size on flash, header + payload rounded up to
 * PERSIST_RECORD_ALIGN bytes. */
static inline uint32_t persist_record_size(size_t payload_len)
{
    uint32_t n = PERSIST_HDR_SIZE + (uint32_t)payload_len;
    return (n + PERSIST_RECORD_ALIGN - 1u) / PERSIST_RECORD_ALIGN * PERSIST_RECORD_ALIGN;
}

typedef enum {
    PERSIST_KIND_VARS = 1,
    PERSIST_KIND_STORE = 2,
} persist_kind_t;

typedef enum {
    PERSIST_LOAD_OK,
    PERSIST_LOAD_NONE, /* blank slot, the old pre-reserve format, another name's record, or no slot for this name */
    PERSIST_LOAD_BAD,  /* bad magic/kind/length, or a CRC mismatch */
} persist_load_status_t;

/* Resets the in-RAM shadow cache (persist_slots.c) and remembers *ops for
 * every later call here. now_ms (nullable) is used only to throttle the
 * "persist: deferred (flash budget)" log to at most once a minute when a
 * save's erase is refused by *ops's erase-rate budget (flash_budget.h);
 * with now_ms == NULL (as the host tests pass, having no clock) every
 * deferral logs. Call once before any other call in this file. */
void persist_slots_init(const flash_ops_t *ops, uint32_t (*now_ms)(void));

/* Finds (or claims) name's slot and writes its vars or store record.
 * False, logged, if the table has no free or matching slot for this
 * name, if the record doesn't fit a sector (len > PERSIST_MAX_PAYLOAD),
 * or if the sector needs erasing to fit the new record and *ops's erase-
 * rate budget refuses that erase for now ("persist: deferred (flash
 * budget)") -- the caller's own data is untouched in all of these cases,
 * so a budget-deferred save is retried the same way as any other: the
 * caller (persist.c, bound to launcher.c's debounce) just tries again
 * later with the same dirty data.
 * Skips the write (true) if the record is byte-for-byte what was last
 * written to that slot's sector this boot. */
bool persist_slots_save(const char *name, persist_kind_t kind, const uint8_t *buf, size_t len);

/* persist_slots_save(), except that an empty record (empty: the
 * caller's encoding of "nothing to keep", e.g. no vars) for a name with
 * no slot yet is skipped -- true, nothing written, no slot claimed -- so
 * a scene that never kept anything doesn't use up one of the 63 slots.
 * A name that already has a slot gets the empty record written as usual
 * (so clearing state sticks). */
bool persist_slots_save_unless_empty(const char *name, persist_kind_t kind, const uint8_t *buf, size_t len,
                                     bool empty);

/* True if some scene slot already belongs to name. */
bool persist_slots_claimed(const char *name);

/* Frees (erases) every scene slot whose name keep() says no to, and any
 * holding a record this format can't name. For the boot, once the scene
 * list is known: keep() should say yes to every name that can still be
 * loaded (built-ins, /scripts files). Only call it when that list is
 * complete -- never on a disk that didn't mount, or every disk script's
 * state would go. Stops at the first erase the budget refuses (*deferred
 * true; the rest is freed on a later boot). Returns how many slots it
 * freed, logging each. The shared store's slot is never touched. */
int persist_slots_reclaim(bool (*keep)(void *ud, const char *name), void *ud, bool *deferred);

/* Finds name's slot and reads its vars or store record into buf (cap
 * bytes); *len gets the payload length on PERSIST_LOAD_OK, 0 otherwise.
 * Logs "no saved state"/"bad record" itself whenever it isn't
 * PERSIST_LOAD_OK. */
persist_load_status_t persist_slots_load(const char *name, persist_kind_t kind, uint8_t *buf, size_t cap,
                                          size_t *len);

/* The shared store's fixed slot: same record format, no name hashing. */
bool persist_slots_save_shared(const uint8_t *buf, size_t len);
persist_load_status_t persist_slots_load_shared(uint8_t *buf, size_t cap, size_t *len);

/* Dev aid (console.c's SysEx corrupt-record command): zeroes name's saved
 * vars record's CRC in place (a flash write only needs to turn 1 bits
 * into 0, so this doesn't need an erase first), so the next load of that
 * name finds a CRC mismatch and logs "persist: bad record for <name>".
 * Logs and does nothing if name has no slot. */
void persist_slots_corrupt_vars(const char *name);

#endif
