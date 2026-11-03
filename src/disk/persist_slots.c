/*
 * Name-keyed persistence slots on SPI flash (persist_slots.h has the
 * layout and the public API). Portable over flash_ops_t so this file is
 * host-testable (test/disk/persist_test.c); src/persist.c is the only
 * device-coupled caller.
 *
 * Record format, append-only within a slot's sector (one sector each for
 * vars and store):
 *
 *   persist_hdr_t   magic, payload length, kind (vars/store), CRC-32 of
 *                   name + payload, the name this record belongs to, and
 *                   a commit word
 *   payload         the caller's opaque bytes
 *   padding         up to the next PERSIST_RECORD_ALIGN-byte boundary
 *
 * A save appends a new record right after whatever's already written in
 * the sector (scan_sector() finds where); only once the next record
 * wouldn't fit does it erase the sector and start over at offset 0. This
 * spreads a sector's ~100,000-erase lifetime over many records instead
 * of spending one erase per save.
 *
 * The header's `commit` word is written last, in a flash write of its
 * own, after the rest of the header and the payload are already on
 * flash: a reset between those writes leaves `commit` at its erased
 * value (PERSIST_COMMIT_PENDING), so scan_sector() treats the record as
 * torn and ignores it -- NOR only turns 1 bits into 0, so there's no way
 * a half-finished write could *look* committed by accident. The record
 * (if any) before it is what a load or a later save sees instead; the
 * space the torn record occupies is simply skipped over (never reused
 * without an erase).
 *
 * resolve_slot() hashes a name onto one of the PERSIST_SLOT_COUNT-1 scene
 * slots and probes forward (wrapping within 1..PERSIST_SLOT_COUNT-1),
 * reading each candidate slot's *first record's* header (vars sector, or
 * store if vars is blank) to tell apart "free" (blank, never written),
 * "already this name's" and "someone else's": the first slot that's
 * already this name's anywhere along the sequence, else the first free
 * one (freed slots can sit in the middle of a sequence). A slot's
 * name is fixed the moment its first record is written (even if that
 * first write turns out torn: magic+name are written before commit, so
 * they're already there), so this never needs to look past the first
 * record. Both persist_slots_save() and persist_slots_load() call it, so
 * a name's save and its later load (or a second save) land on the same
 * slot as long as the table's occupants haven't changed in between,
 * without needing a separate index anywhere.
 *
 * persist_slots_save() scans the sector for where to append (or whether
 * it must erase first), then writes header (minus commit) + payload +
 * commit, then reads the new record back and compares (storage_write()
 * semantics: a write only turns 1 bits into 0, so a write that doesn't
 * match either hit a flash fault or raced a reset). It skips the append
 * entirely if the record is byte-for-byte what was last written to that
 * sector this boot (an in-RAM shadow, not a flash read).
 *
 * persist_slots_load() finds the sector's last valid (committed,
 * CRC-good) record and validates its name, falling back to "no saved
 * state" or "bad record" as before.
 */
#include "persist_slots.h"

#include "disk_fatal.h"
#include "log.h"
#include "ccm.h"

#include <stddef.h>
#include <string.h>

/* Bytes 'E','1','6','3' little-endian: the append-only record format
 * with a name. The two older formats below can never be produced by
 * this file again, but a scan must still recognize and reject them
 * rather than mistake them (or their leftover bytes) for append-only
 * records:
 *
 *   PERSIST_MAGIC_V1       phase 1's nameless/index-only format. Its
 *                           records lived below where the FAT volume
 *                           now sits, so they can never actually appear
 *                           in the reserve -- kept only so a blank byte
 *                           pattern that happened to match it stays
 *                           unambiguous.
 *   PERSIST_MAGIC_OLD_SINGLE  phase 2's first named format: one record
 *                           per sector, no append/commit. A device that
 *                           flashed that build before this change has
 *                           sectors in this format; they're treated as
 *                           "no saved state" (new magic) rather than
 *                           misread.
 */
#define PERSIST_MAGIC 0x33363145u
#define PERSIST_MAGIC_V1 0x50363145u
#define PERSIST_MAGIC_OLD_SINGLE 0x32363145u
#define PERSIST_BLANK 0xFFFFFFFFu /* erased flash */

#define PERSIST_COMMIT_DONE 0x00000000u
#define PERSIST_COMMIT_PENDING 0xFFFFFFFFu /* erased: not yet committed */

#define PERSIST_SHARED_NAME "shared"

typedef struct {
    uint32_t magic;
    uint16_t len;
    uint16_t kind;
    uint32_t crc;
    char name[PERSIST_NAME_LEN];
    uint32_t commit;
} persist_hdr_t;

_Static_assert(sizeof(persist_hdr_t) == PERSIST_HDR_SIZE,
               "persist_hdr_t's layout changed; update PERSIST_HDR_SIZE in persist_slots.h");
_Static_assert(offsetof(persist_hdr_t, commit) == PERSIST_COMMIT_OFFSET,
               "commit must stay the header's last field; update PERSIST_COMMIT_OFFSET");
_Static_assert(PERSIST_HDR_SIZE <= FLASH_SECTOR_SIZE, "a record header must fit a sector");

static const flash_ops_t *g_ops;
static uint32_t (*g_now_ms)(void);
static uint32_t g_last_deferred_log_ms;
static bool g_have_deferred_log;

/* Throttled to at most once a minute (see persist_slots_init()'s now_ms);
 * without a clock, every deferral just logs. */
static void log_deferred(void)
{
    if (g_now_ms == NULL) {
        log_printf("persist: deferred (flash budget)");
        return;
    }
    uint32_t now = g_now_ms();
    if (g_have_deferred_log && now - g_last_deferred_log_ms < 60000u) {
        return;
    }
    log_printf("persist: deferred (flash budget)");
    g_last_deferred_log_ms = now;
    g_have_deferred_log = true;
}

/* Bitwise CRC-32 (the common reflected/"CRC-32/ISO-HDLC" form, polynomial
 * 0xEDB88320), continued across calls from crc = 0. Payloads here are at
 * most ~2 KB, so a 1 KB lookup table isn't worth the static RAM. */
static uint32_t crc32_update(uint32_t crc, const uint8_t *buf, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
        }
    }
    return ~crc;
}

static uint32_t record_crc(const char name[PERSIST_NAME_LEN], const uint8_t *buf, size_t len)
{
    uint32_t crc = crc32_update(0, (const uint8_t *)name, PERSIST_NAME_LEN);
    return crc32_update(crc, buf, len);
}

/* record_crc() of a payload still on flash at addr, read through a small
 * stack buffer rather than a whole-payload (up to 4 KB) static one. */
static uint32_t flash_record_crc(uint32_t addr, const char name[PERSIST_NAME_LEN], uint32_t len)
{
    uint8_t chunk[128];
    uint32_t crc = crc32_update(0, (const uint8_t *)name, PERSIST_NAME_LEN);
    while (len > 0) {
        uint32_t n = len < sizeof chunk ? len : (uint32_t)sizeof chunk;
        g_ops->read(addr, chunk, n);
        crc = crc32_update(crc, chunk, n);
        addr += n;
        len -= n;
    }
    return crc;
}

/* FNV-1a, 32-bit: simple, fast, and good enough to spread scene names
 * across the scene slots -- linear probing (resolve_slot()) covers any
 * names that do land on the same bucket anyway, so the exact distribution
 * isn't load-bearing. */
static uint32_t name_hash(const char *name)
{
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)name; *p != '\0'; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

/*
 * In-RAM state of what's on flash this boot, per sector (vars or store):
 * where the next record should be appended, and the last valid record's
 * crc/len (so persist_slots_save() can skip writing a new record when
 * it would be byte-for-byte what's already there -- the caller's
 * debounce (launcher.c's PERSIST_DEBOUNCE_MS) fires on a timer, not on
 * an actual change, so this is what keeps a script that stopped changing
 * its vars from costing an append anyway).
 *
 * A cache miss (more distinct sectors touched in one boot than this
 * holds, or the first touch of a sector this boot) scans the sector from
 * flash once to fill this in; from then on every append/erase this file
 * does updates it in place, so later saves/loads of the same sector this
 * boot never need to re-scan. Sized with headroom above a typical boot's
 * working set (one running scene's vars+store, plus the shared store).
 */
#define SHADOW_N 48
typedef struct {
    uint32_t addr;           /* sector address */
    uint32_t append_offset;  /* where the next record would start */
    bool have_last;          /* a valid record exists in this sector */
    uint32_t last_off;       /* its offset, if have_last */
    uint32_t last_crc;
    uint16_t last_len;
    bool valid;
} shadow_t;
static shadow_t g_shadow[SHADOW_N] CCM_BSS; /* zeroed by persist_slots_init() */

static shadow_t *shadow_find(uint32_t addr)
{
    for (int i = 0; i < SHADOW_N; i++) {
        if (g_shadow[i].valid && g_shadow[i].addr == addr) {
            return &g_shadow[i];
        }
    }
    return NULL;
}

/* A free shadow slot for addr, reusing the first invalid one if the
 * table isn't full, or NULL if it is (the sector then just always gets
 * scanned fresh -- never wrong, only occasionally not free). */
static shadow_t *shadow_claim(uint32_t addr)
{
    for (int i = 0; i < SHADOW_N; i++) {
        if (!g_shadow[i].valid) {
            g_shadow[i].valid = true;
            g_shadow[i].addr = addr;
            g_shadow[i].have_last = false;
            return &g_shadow[i];
        }
    }
    return NULL;
}

static void shadow_invalidate(uint32_t addr)
{
    shadow_t *sh = shadow_find(addr);
    if (sh != NULL) {
        sh->valid = false;
    }
}

static uint32_t vars_addr(unsigned slot)
{
    return PERSIST_RESERVE_BASE + slot * PERSIST_SLOT_SIZE;
}

static uint32_t store_addr(unsigned slot)
{
    return vars_addr(slot) + FLASH_SECTOR_SIZE;
}

/*
 * Nothing in this file may ever touch an address outside the reserve --
 * below it is the FAT volume and, below that, the stock
 * firmware's own data. This can't actually happen with the address
 * arithmetic above (every address here is PERSIST_RESERVE_BASE plus an
 * offset bounded by PERSIST_SLOT_COUNT/FLASH_SECTOR_SIZE), but the check
 * stays active in a release (MinSizeRel) build too -- unlike a bare
 * assert(), which NDEBUG would strip -- because a silently-skipped check
 * here is a corrupted FAT volume or stock firmware, not a stack trace.
 */
static void require_in_bounds(uint32_t addr)
{
    bool ok = addr >= PERSIST_RESERVE_BASE && addr < PERSIST_RESERVE_BASE + PERSIST_RESERVE_SIZE;
    if (!ok) {
        log_printf("persist: BUG: refused flash access at 0x%08lx (outside the reserve)",
                   (unsigned long)addr);
        disk_fatal("persist: address outside the reserve");
    }
}

static const char *kind_name(persist_kind_t kind)
{
    return kind == PERSIST_KIND_VARS ? "vars" : "store";
}

typedef enum {
    SLOT_FREE,  /* never written (blank) */
    SLOT_MATCH, /* already holds this name's record (valid or not) */
    SLOT_TAKEN, /* holds some other name's record, or an unrecognized format */
} slot_state_t;

/* The name a slot belongs to: its first record's, in the vars sector or,
 * if that has never been written, the store sector (a scene that saved
 * only its store owns the slot all the same). Only ever looks at a
 * sector's first record: a slot's name is fixed the moment that
 * record's header is written (magic + name are written before commit, so
 * this doesn't need the record to be committed). SLOT_TAKEN with
 * owner[0] == '\0' is a record this format can't name (old formats,
 * garbage). */
static slot_state_t slot_owner(unsigned slot, char owner[PERSIST_NAME_LEN])
{
    owner[0] = '\0';
    uint32_t addrs[2] = {vars_addr(slot), store_addr(slot)};
    for (int i = 0; i < 2; i++) {
        require_in_bounds(addrs[i]);
        persist_hdr_t hdr;
        g_ops->read(addrs[i], &hdr, sizeof hdr);
        if (hdr.magic == PERSIST_BLANK) {
            continue;
        }
        if (hdr.magic == PERSIST_MAGIC && hdr.name[PERSIST_NAME_LEN - 1] == '\0') {
            memcpy(owner, hdr.name, PERSIST_NAME_LEN);
        }
        return SLOT_TAKEN;
    }
    return SLOT_FREE;
}

static slot_state_t slot_state(unsigned slot, const char *name)
{
    char owner[PERSIST_NAME_LEN];
    if (slot_owner(slot, owner) == SLOT_FREE) {
        return SLOT_FREE;
    }
    return owner[0] != '\0' && strncmp(owner, name, PERSIST_NAME_LEN) == 0 ? SLOT_MATCH : SLOT_TAKEN;
}

/*
 * name's slot: the one that already names it anywhere along its probe
 * sequence, else the first free one on it; -1 if every scene slot names
 * someone else (the table is full for this name). The whole sequence is
 * searched for a match, not just up to the first free slot: a slot
 * freed by persist_slots_reclaim() may sit in the middle of another
 * name's probe sequence. *is_new says it's a free slot (nothing of this
 * name's saved yet).
 */
static int resolve_slot_ex(const char *name, bool *is_new)
{
    unsigned buckets = PERSIST_SLOT_COUNT - 1u; /* slots 1..PERSIST_SLOT_COUNT-1 */
    unsigned start = 1u + (name_hash(name) % buckets);
    int first_free = -1;
    for (unsigned i = 0; i < buckets; i++) {
        unsigned slot = 1u + (start - 1u + i) % buckets;
        slot_state_t st = slot_state(slot, name);
        if (st == SLOT_MATCH) {
            *is_new = false;
            return (int)slot;
        }
        if (st == SLOT_FREE && first_free < 0) {
            first_free = (int)slot;
        }
    }
    *is_new = first_free >= 0;
    return first_free;
}

static int resolve_slot(const char *name)
{
    bool is_new;
    return resolve_slot_ex(name, &is_new);
}

/*
 * Scans a sector from offset 0, record by record. Returns the offset to
 * append the next record at (FLASH_SECTOR_SIZE if the sector is full, or
 * holds something this format doesn't recognize, so the caller must
 * erase). If a valid (committed, CRC-good) record is found, *out_valid is
 * set true and out_hdr/out_off describe the *last* one seen (an
 * append-only sector can hold several records; the newest wins).
 *
 * A record whose header is present (magic matches) but whose commit word
 * is still PERSIST_COMMIT_PENDING is a torn write: its space counts
 * toward the append offset (it can't be reused without an erase), but it
 * never updates out_hdr/out_off. A record whose length doesn't fit
 * what's left in the sector, or a magic this format doesn't produce
 * (blank aside), means the rest of the sector can't be trusted as a
 * record stream; the caller erases rather than guess.
 */
static uint32_t scan_sector(uint32_t addr, persist_hdr_t *out_hdr, uint32_t *out_off, bool *out_valid)
{
    *out_valid = false;
    uint32_t offset = 0;
    while (offset + PERSIST_HDR_SIZE <= FLASH_SECTOR_SIZE) {
        persist_hdr_t hdr;
        g_ops->read(addr + offset, &hdr, sizeof hdr);
        if (hdr.magic == PERSIST_BLANK) {
            return offset; /* free space starts here */
        }
        if (hdr.magic != PERSIST_MAGIC) {
            return FLASH_SECTOR_SIZE; /* old format or garbage: force an erase */
        }
        if (hdr.len > PERSIST_MAX_PAYLOAD || (uint64_t)PERSIST_HDR_SIZE + hdr.len > FLASH_SECTOR_SIZE - offset) {
            return FLASH_SECTOR_SIZE; /* corrupt length: force an erase */
        }
        uint32_t record_size = persist_record_size(hdr.len);
        if (hdr.commit == PERSIST_COMMIT_DONE) {
            if (hdr.name[PERSIST_NAME_LEN - 1] == '\0' &&
                flash_record_crc(addr + offset + PERSIST_HDR_SIZE, hdr.name, hdr.len) == hdr.crc) {
                *out_hdr = hdr;
                *out_off = offset;
                *out_valid = true;
            }
            /* Committed but CRC-bad: genuinely corrupt, not torn (the
             * commit write did happen). Its space is still consumed;
             * keep scanning in case a later record is fine. */
        }
        offset += record_size;
    }
    return FLASH_SECTOR_SIZE; /* no room even for one more header */
}

/* The shadow entry for addr, scanning the sector to fill it in on the
 * first touch this boot. NULL only if the shadow table itself is full
 * (SHADOW_N sectors already tracked, none of them addr) -- callers then
 * fall back to a non-cached scan, see save_record()/load_record(). */
static shadow_t *touch_sector(uint32_t addr)
{
    shadow_t *sh = shadow_find(addr);
    if (sh != NULL) {
        return sh;
    }
    sh = shadow_claim(addr);
    if (sh == NULL) {
        return NULL;
    }
    persist_hdr_t hdr;
    uint32_t off;
    bool valid;
    sh->append_offset = scan_sector(addr, &hdr, &off, &valid);
    sh->have_last = valid;
    if (valid) {
        sh->last_off = off;
        sh->last_crc = hdr.crc;
        sh->last_len = hdr.len;
    }
    return sh;
}

static bool verify_record(uint32_t addr, const persist_hdr_t *hdr, const uint8_t *buf, size_t len)
{
    persist_hdr_t h2;
    g_ops->read(addr, &h2, sizeof h2);
    if (memcmp(&h2, hdr, sizeof h2) != 0) {
        return false;
    }
    uint8_t chunk[256];
    size_t off = 0;
    while (off < len) {
        size_t n = len - off;
        if (n > sizeof chunk) {
            n = sizeof chunk;
        }
        g_ops->read(addr + sizeof *hdr + off, chunk, n);
        if (memcmp(chunk, buf + off, n) != 0) {
            return false;
        }
        off += n;
    }
    return true;
}

/* Appends one record at offset within sector_addr: header (minus commit)
 * + payload, then the commit word last, in its own write. */
static void write_record(uint32_t sector_addr, uint32_t offset, const persist_hdr_t *hdr, const uint8_t *buf,
                          size_t len)
{
    uint32_t addr = sector_addr + offset;
    g_ops->write(addr, hdr, PERSIST_COMMIT_OFFSET);
    if (len > 0) {
        g_ops->write(addr + PERSIST_HDR_SIZE, buf, len);
    }
    uint32_t commit = PERSIST_COMMIT_DONE;
    g_ops->write(addr + PERSIST_COMMIT_OFFSET, &commit, sizeof commit);
}

static bool save_record(uint32_t sector_addr, persist_kind_t kind, const char *name, const uint8_t *buf,
                         size_t len)
{
    require_in_bounds(sector_addr);
    require_in_bounds(sector_addr + FLASH_SECTOR_SIZE - 1u);
    if (len > 0xFFFFu || PERSIST_HDR_SIZE + len > FLASH_SECTOR_SIZE) {
        /* Keep whatever record is there rather than overwrite it with an
         * empty or truncated one. */
        log_printf("persist: not saving %s (%s): doesn't fit a record", name, kind_name(kind));
        return false;
    }

    persist_hdr_t hdr;
    memset(&hdr, 0, sizeof hdr);
    hdr.magic = PERSIST_MAGIC;
    hdr.len = (uint16_t)len;
    hdr.kind = (uint16_t)kind;
    strncpy(hdr.name, name, PERSIST_NAME_LEN - 1u);
    hdr.crc = record_crc(hdr.name, buf, len);
    hdr.commit = PERSIST_COMMIT_PENDING; /* overwritten by write_record()'s last write */

    shadow_t *sh = touch_sector(sector_addr);
    if (sh != NULL && sh->have_last && sh->last_crc == hdr.crc && sh->last_len == hdr.len) {
        return true; /* this sector's last record already holds exactly this */
    }

    uint32_t record_size = persist_record_size(len);
    uint32_t append_offset;
    if (sh != NULL) {
        append_offset = sh->append_offset;
    } else {
        /* Shadow table full: fall back to a direct scan, uncached. */
        persist_hdr_t discard_hdr;
        uint32_t discard_off;
        bool discard_valid;
        append_offset = scan_sector(sector_addr, &discard_hdr, &discard_off, &discard_valid);
    }

    if ((uint64_t)append_offset + record_size > FLASH_SECTOR_SIZE) {
        if (!g_ops->erase_sector(sector_addr)) {
            log_deferred();
            return false; /* buf/len untouched: the caller's data is still only in RAM */
        }
        log_printf("persist: erase 0x%06lx", (unsigned long)sector_addr);
        append_offset = 0;
    }

    write_record(sector_addr, append_offset, &hdr, buf, len);
    hdr.commit = PERSIST_COMMIT_DONE; /* what's actually on flash now; verify_record reads it back */

    if (!verify_record(sector_addr + append_offset, &hdr, buf, len)) {
        if (sh != NULL) {
            sh->valid = false; /* unknown state: re-scan next time */
        }
        log_printf("persist: write failed for %s (%s)", name, kind_name(kind));
        return false;
    }

    if (sh != NULL) {
        sh->append_offset = append_offset + record_size;
        sh->have_last = true;
        sh->last_off = append_offset;
        sh->last_crc = hdr.crc;
        sh->last_len = hdr.len;
    }
    log_printf("persist: saved %s (%s, %u bytes)", name, kind_name(kind), (unsigned)len);
    return true;
}

static persist_load_status_t load_record(uint32_t sector_addr, persist_kind_t kind, const char *name,
                                          uint8_t *buf, size_t cap, size_t *len)
{
    const char *kname = kind_name(kind);
    *len = 0;

    require_in_bounds(sector_addr);
    persist_hdr_t first_hdr;
    g_ops->read(sector_addr, &first_hdr, sizeof first_hdr);
    if (first_hdr.magic == PERSIST_BLANK || first_hdr.magic == PERSIST_MAGIC_V1 ||
        first_hdr.magic == PERSIST_MAGIC_OLD_SINGLE) {
        log_printf("persist: no saved state for %s (%s%s)", name, kname,
                   first_hdr.magic == PERSIST_BLANK ? "" : ", old record format");
        return PERSIST_LOAD_NONE;
    }

    shadow_t *sh = touch_sector(sector_addr);
    bool have_last;
    uint32_t last_off = 0;
    persist_hdr_t hdr;
    if (sh != NULL) {
        have_last = sh->have_last;
        if (have_last) {
            last_off = sh->last_off;
            g_ops->read(sector_addr + last_off, &hdr, sizeof hdr);
        }
    } else {
        bool valid;
        uint32_t off;
        scan_sector(sector_addr, &hdr, &off, &valid);
        have_last = valid;
        last_off = off;
    }

    if (!have_last) {
        log_printf("persist: bad record for %s (%s)", name, kname);
        return PERSIST_LOAD_BAD;
    }
    if (hdr.kind != (uint16_t)kind || hdr.len > cap) {
        log_printf("persist: bad record for %s (%s)", name, kname);
        return PERSIST_LOAD_BAD;
    }
    if (hdr.len > 0) {
        g_ops->read(sector_addr + last_off + PERSIST_HDR_SIZE, buf, hdr.len);
    }
    if (record_crc(hdr.name, buf, hdr.len) != hdr.crc) {
        log_printf("persist: bad record for %s (%s)", name, kname);
        return PERSIST_LOAD_BAD;
    }
    if (strncmp(hdr.name, name, PERSIST_NAME_LEN) != 0) {
        /* Another name won this slot since it was last saved (shouldn't
         * happen -- resolve_slot() would have found that name's own slot
         * first -- but a record this file didn't just write is never
         * trusted blindly). */
        log_printf("persist: no saved state for %s (%s; sector held %s)", name, kname, hdr.name);
        return PERSIST_LOAD_NONE;
    }
    *len = hdr.len;
    return PERSIST_LOAD_OK;
}

void persist_slots_init(const flash_ops_t *ops, uint32_t (*now_ms)(void))
{
    g_ops = ops;
    g_now_ms = now_ms;
    g_have_deferred_log = false;
    memset(g_shadow, 0, sizeof g_shadow);
}

bool persist_slots_save(const char *name, persist_kind_t kind, const uint8_t *buf, size_t len)
{
    int slot = resolve_slot(name);
    if (slot < 0) {
        log_printf("persist: no free slot for %s", name);
        return false;
    }
    uint32_t addr = (kind == PERSIST_KIND_VARS) ? vars_addr((unsigned)slot) : store_addr((unsigned)slot);
    return save_record(addr, kind, name, buf, len);
}

bool persist_slots_save_unless_empty(const char *name, persist_kind_t kind, const uint8_t *buf, size_t len,
                                     bool empty)
{
    if (empty) {
        bool is_new;
        int slot = resolve_slot_ex(name, &is_new);
        if (slot < 0 || is_new) {
            return true; /* nothing saved under this name, nothing to save: no slot claimed */
        }
    }
    return persist_slots_save(name, kind, buf, len);
}

bool persist_slots_claimed(const char *name)
{
    bool is_new;
    return resolve_slot_ex(name, &is_new) >= 0 && !is_new;
}

int persist_slots_reclaim(bool (*keep)(void *ud, const char *name), void *ud, bool *deferred)
{
    int freed = 0;
    *deferred = false;
    for (unsigned slot = 1; slot < PERSIST_SLOT_COUNT; slot++) {
        char owner[PERSIST_NAME_LEN];
        if (slot_owner(slot, owner) == SLOT_FREE) {
            continue;
        }
        if (owner[0] != '\0' && keep(ud, owner)) {
            continue;
        }
        /* The store sector first, then vars: if the budget stops this in
         * between, the slot is still named by its vars record and is
         * finished next time -- never a store record in a slot that
         * looks free. */
        uint32_t order[2] = {store_addr(slot), vars_addr(slot)};
        bool ok = true;
        for (int i = 0; i < 2; i++) {
            uint32_t magic;
            g_ops->read(order[i], &magic, sizeof magic);
            if (magic == PERSIST_BLANK) {
                continue;
            }
            if (!g_ops->erase_sector(order[i])) {
                ok = false;
                break;
            }
            shadow_invalidate(order[i]);
        }
        if (!ok) {
            *deferred = true;
            break;
        }
        log_printf("persist: freed slot %u (%s)", slot, owner[0] ? owner : "unreadable record");
        freed++;
    }
    return freed;
}

persist_load_status_t persist_slots_load(const char *name, persist_kind_t kind, uint8_t *buf, size_t cap,
                                          size_t *len)
{
    int slot = resolve_slot(name);
    if (slot < 0) {
        *len = 0;
        log_printf("persist: no saved state for %s (%s; the slot table is full)", name, kind_name(kind));
        return PERSIST_LOAD_NONE;
    }
    uint32_t addr = (kind == PERSIST_KIND_VARS) ? vars_addr((unsigned)slot) : store_addr((unsigned)slot);
    return load_record(addr, kind, name, buf, cap, len);
}

bool persist_slots_save_shared(const uint8_t *buf, size_t len)
{
    return save_record(store_addr(PERSIST_SHARED_SLOT), PERSIST_KIND_STORE, PERSIST_SHARED_NAME, buf, len);
}

persist_load_status_t persist_slots_load_shared(uint8_t *buf, size_t cap, size_t *len)
{
    return load_record(store_addr(PERSIST_SHARED_SLOT), PERSIST_KIND_STORE, PERSIST_SHARED_NAME, buf, cap,
                        len);
}

void persist_slots_corrupt_vars(const char *name)
{
    int slot = resolve_slot(name);
    if (slot < 0) {
        log_printf("persist: corrupt: no slot for %s", name);
        return;
    }
    uint32_t sector_addr = vars_addr((unsigned)slot);
    require_in_bounds(sector_addr);
    shadow_t *sh = touch_sector(sector_addr);
    if (sh == NULL || !sh->have_last) {
        log_printf("persist: corrupt: no record for %s", name);
        return;
    }
    uint32_t crc_addr = sector_addr + sh->last_off + offsetof(persist_hdr_t, crc);
    uint32_t zero = 0;
    g_ops->write(crc_addr, &zero, sizeof zero);
    /* Not in the shadow cache's sense of "we last wrote this": forget
     * whatever this sector's shadow entry said, so a later save of the
     * same content doesn't think it's redundant and skip the append that
     * fixes the corruption. */
    shadow_invalidate(sector_addr);
    log_printf("persist: dev-corrupted vars record at 0x%06lx+0x%lx", (unsigned long)sector_addr,
               (unsigned long)sh->last_off);
}
