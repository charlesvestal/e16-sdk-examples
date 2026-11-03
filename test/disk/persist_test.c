/*
 * Host test for src/disk/persist_slots.c: a fake flash_ops_t over a RAM
 * array sized and offset-mapped exactly like the real persistence reserve
 * (PERSIST_RESERVE_BASE/_SIZE), same NOR-write-semantics fake as test/
 * disk/blockdev_test.c. Tests the record layer directly with raw byte
 * payloads -- it doesn't need the runtime (rt.h) src/persist.c binds it
 * to on the device. Plain C, run directly (not through e16rt-host).
 * Prints "ALL PASSED" and exits 0 on success; exits nonzero otherwise.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "persist_slots.h"

#define FAKE_BASE PERSIST_RESERVE_BASE
#define FAKE_SIZE PERSIST_RESERVE_SIZE

static int g_failures;
#define CHECK(cond, ...)                                                                                     \
    do {                                                                                                      \
        if (!(cond)) {                                                                                        \
            g_failures++;                                                                                     \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                               \
            fprintf(stderr, __VA_ARGS__);                                                                      \
            fprintf(stderr, "\n");                                                                             \
        }                                                                                                      \
    } while (0)

/* ---- fake flash: a RAM array that enforces NOR write semantics (same
 * pattern as blockdev_test.c's fake) ---- */

static uint8_t fake_flash[FAKE_SIZE];
static uint32_t fake_erase_count;
static int fake_write_violations;

static void fake_offset_ok(uint32_t addr, size_t len)
{
    assert(addr >= FAKE_BASE);
    assert((uint64_t)addr + len <= (uint64_t)FAKE_BASE + FAKE_SIZE);
}

static void fake_read(uint32_t addr, void *buf, size_t len)
{
    fake_offset_ok(addr, len);
    memcpy(buf, fake_flash + (addr - FAKE_BASE), len);
}

static void fake_write(uint32_t addr, const void *data, size_t len)
{
    fake_offset_ok(addr, len);
    const uint8_t *src = data;
    uint8_t *dst = fake_flash + (addr - FAKE_BASE);
    for (size_t i = 0; i < len; i++) {
        if ((uint8_t)(src[i] & (uint8_t)~dst[i]) != 0) {
            fake_write_violations++;
        }
        dst[i] = (uint8_t)(dst[i] & src[i]);
    }
}

static bool fake_erase_sector(uint32_t addr)
{
    fake_offset_ok(addr, FLASH_SECTOR_SIZE);
    assert((addr - FAKE_BASE) % FLASH_SECTOR_SIZE == 0);
    memset(fake_flash + (addr - FAKE_BASE), 0xFF, FLASH_SECTOR_SIZE);
    fake_erase_count++;
    return true;
}

static const flash_ops_t fake_ops = {
    .read = fake_read,
    .write = fake_write,
    .erase_sector = fake_erase_sector,
};

static void fake_flash_reset(void)
{
    memset(fake_flash, 0xFF, FAKE_SIZE);
    fake_erase_count = 0;
    fake_write_violations = 0;
    persist_slots_init(&fake_ops, NULL);
}

/* A local copy of persist_slots.c's name hash, for the collision test
 * below: it needs two names that land in the same bucket to prove linear
 * probing actually separates them, which means computing buckets the same
 * way the module under test does. Kept in sync by inspection (it's a
 * three-line FNV-1a); if persist_slots.c's hash ever changes, this test
 * still passes (it searches for *some* colliding pair at test time), it
 * just stops being a one-line diff to see why. */
static uint32_t name_hash(const char *name)
{
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)name; *p != '\0'; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

#define SCENE_BUCKETS (PERSIST_SLOT_COUNT - 1u)

static unsigned bucket_of(const char *name)
{
    return name_hash(name) % SCENE_BUCKETS;
}

/* ---- tests ---- */

static void test_save_load_roundtrip(void)
{
    fake_flash_reset();
    const char *name = "euclid";
    uint8_t vars[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t store[4] = {0xAA, 0xBB, 0xCC, 0xDD};

    CHECK(persist_slots_save(name, PERSIST_KIND_VARS, vars, sizeof vars), "save vars for %s", name);
    CHECK(persist_slots_save(name, PERSIST_KIND_STORE, store, sizeof store), "save store for %s", name);

    uint8_t rbuf[64];
    size_t rlen = 0;
    CHECK(persist_slots_load(name, PERSIST_KIND_VARS, rbuf, sizeof rbuf, &rlen) == PERSIST_LOAD_OK,
          "load vars for %s", name);
    CHECK(rlen == sizeof vars && memcmp(rbuf, vars, sizeof vars) == 0, "vars round-trip for %s", name);

    CHECK(persist_slots_load(name, PERSIST_KIND_STORE, rbuf, sizeof rbuf, &rlen) == PERSIST_LOAD_OK,
          "load store for %s", name);
    CHECK(rlen == sizeof store && memcmp(rbuf, store, sizeof store) == 0, "store round-trip for %s", name);
}

static void test_blank_flash_is_no_saved_state(void)
{
    fake_flash_reset();
    uint8_t rbuf[64];
    size_t rlen = 0;
    CHECK(persist_slots_load("never-saved", PERSIST_KIND_VARS, rbuf, sizeof rbuf, &rlen) ==
              PERSIST_LOAD_NONE,
          "blank flash: no saved state");
    CHECK(rlen == 0, "blank flash: *len left at 0");
}

static void test_corrupt_record(void)
{
    fake_flash_reset();
    const char *name = "tb3po";
    uint8_t vars[4] = {9, 9, 9, 9};
    CHECK(persist_slots_save(name, PERSIST_KIND_VARS, vars, sizeof vars), "save vars for %s", name);

    persist_slots_corrupt_vars(name);

    uint8_t rbuf[64];
    size_t rlen = 0;
    CHECK(persist_slots_load(name, PERSIST_KIND_VARS, rbuf, sizeof rbuf, &rlen) == PERSIST_LOAD_BAD,
          "corrupted record loads as bad");

    /* A fresh save of the same content clears the corruption (the shadow
     * cache must have forgotten this sector, or the save would wrongly
     * skip the erase+write that fixes it). */
    CHECK(persist_slots_save(name, PERSIST_KIND_VARS, vars, sizeof vars), "re-save after corruption");
    CHECK(persist_slots_load(name, PERSIST_KIND_VARS, rbuf, sizeof rbuf, &rlen) == PERSIST_LOAD_OK,
          "loads cleanly again after the re-save");
}

static void test_shared_slot(void)
{
    fake_flash_reset();
    uint8_t store[6] = {1, 2, 3, 4, 5, 6};
    CHECK(persist_slots_save_shared(store, sizeof store), "save shared store");

    /* Lands at slot 0's store sector, not anywhere a scene hash could
     * pick (PERSIST_SHARED_SLOT == 0, never a linear-probe target: scene
     * buckets start at slot 1). */
    uint32_t want_addr = PERSIST_RESERVE_BASE + PERSIST_SHARED_SLOT * PERSIST_SLOT_SIZE + FLASH_SECTOR_SIZE;
    CHECK(memcmp(fake_flash + (want_addr - FAKE_BASE) + PERSIST_HDR_SIZE, store, sizeof store) == 0,
          "shared store landed at the fixed shared slot address");

    uint8_t rbuf[64];
    size_t rlen = 0;
    CHECK(persist_slots_load_shared(rbuf, sizeof rbuf, &rlen) == PERSIST_LOAD_OK, "load shared store");
    CHECK(rlen == sizeof store && memcmp(rbuf, store, sizeof store) == 0, "shared store round-trip");

    /* A scene named "shared" doesn't collide with the real shared slot:
     * it gets its own scene slot (1..63), hashed like any other name. */
    uint8_t scene_vars[2] = {0x11, 0x22};
    CHECK(persist_slots_save("shared", PERSIST_KIND_VARS, scene_vars, sizeof scene_vars),
          "save vars for a scene literally named 'shared'");
    CHECK(persist_slots_load_shared(rbuf, sizeof rbuf, &rlen) == PERSIST_LOAD_OK &&
              rlen == sizeof store && memcmp(rbuf, store, sizeof store) == 0,
          "the real shared store is untouched by a same-named scene");
}

/* Two scene names that hash to the same bucket must still both save and
 * load correctly: the second one's save finds the first's slot occupied
 * (a different name) and probes forward to a free one; both then load
 * back their own, distinct data. */
static void test_hash_collision(void)
{
    fake_flash_reset();

    char a[32] = {0}, b[32] = {0};
    bool found = false;
    for (int i = 0; i < 100000 && !found; i++) {
        char cand[32];
        snprintf(cand, sizeof cand, "scene%d", i);
        unsigned bucket = bucket_of(cand);
        for (int j = 0; j < i; j++) {
            char other[32];
            snprintf(other, sizeof other, "scene%d", j);
            if (bucket_of(other) == bucket) {
                strcpy(a, other);
                strcpy(b, cand);
                found = true;
                break;
            }
        }
    }
    CHECK(found, "found two names that collide on the hash bucket");
    if (!found) {
        return;
    }
    CHECK(bucket_of(a) == bucket_of(b), "sanity: %s and %s really do share a bucket", a, b);
    CHECK(strcmp(a, b) != 0, "sanity: the colliding names are distinct");

    uint8_t va[4] = {1, 1, 1, 1};
    uint8_t vb[4] = {2, 2, 2, 2};
    CHECK(persist_slots_save(a, PERSIST_KIND_VARS, va, sizeof va), "save vars for %s", a);
    CHECK(persist_slots_save(b, PERSIST_KIND_VARS, vb, sizeof vb), "save vars for %s (collides with %s)", b,
          a);

    uint8_t rbuf[64];
    size_t rlen = 0;
    CHECK(persist_slots_load(a, PERSIST_KIND_VARS, rbuf, sizeof rbuf, &rlen) == PERSIST_LOAD_OK &&
              rlen == sizeof va && memcmp(rbuf, va, sizeof va) == 0,
          "%s still loads its own data", a);
    CHECK(persist_slots_load(b, PERSIST_KIND_VARS, rbuf, sizeof rbuf, &rlen) == PERSIST_LOAD_OK &&
              rlen == sizeof vb && memcmp(rbuf, vb, sizeof vb) == 0,
          "%s loads its own data, not %s's", b, a);
}

/* Filling every scene slot (63 distinct names), then asking for a 64th,
 * must fail the save and report "no saved state" on load -- without
 * disturbing the 63 that are already there. */
static void test_full_table(void)
{
    fake_flash_reset();
    char names[SCENE_BUCKETS][16];
    for (unsigned i = 0; i < SCENE_BUCKETS; i++) {
        snprintf(names[i], sizeof names[i], "slot%u", i);
        uint8_t v = (uint8_t)(i + 1);
        uint8_t buf[1] = {v};
        CHECK(persist_slots_save(names[i], PERSIST_KIND_VARS, buf, sizeof buf), "fill slot for %s (%u/%u)",
              names[i], i + 1, SCENE_BUCKETS);
    }

    uint8_t overflow_buf[1] = {0xFF};
    CHECK(!persist_slots_save("one-too-many", PERSIST_KIND_VARS, overflow_buf, sizeof overflow_buf),
          "the table is full: the 64th name's save is refused");

    uint8_t rbuf[64];
    size_t rlen = 0;
    CHECK(persist_slots_load("one-too-many", PERSIST_KIND_VARS, rbuf, sizeof rbuf, &rlen) ==
              PERSIST_LOAD_NONE,
          "the 64th name has no saved state either");

    /* Everyone who did get a slot still has their own data. */
    for (unsigned i = 0; i < SCENE_BUCKETS; i++) {
        CHECK(persist_slots_load(names[i], PERSIST_KIND_VARS, rbuf, sizeof rbuf, &rlen) == PERSIST_LOAD_OK &&
                  rlen == 1 && rbuf[0] == (uint8_t)(i + 1),
              "%s still has its own data after the table filled up", names[i]);
    }
}

/* The shadow cache must not make a second save of unchanged content
 * write a redundant record -- or, the common case, must skip it when
 * nothing changed. A blank sector never needs erasing just to append
 * its first record (append-only: the whole point is that most saves,
 * changed or not, cost no erase at all -- see test_fill_and_wrap below
 * for when one actually does). */
static void test_shadow_skips_unchanged_save(void)
{
    fake_flash_reset();
    const char *name = "repeat";
    uint8_t buf[4] = {7, 7, 7, 7};
    CHECK(persist_slots_save(name, PERSIST_KIND_VARS, buf, sizeof buf), "first save");
    CHECK(fake_erase_count == 0, "appending to a blank sector costs no erase");

    CHECK(persist_slots_save(name, PERSIST_KIND_VARS, buf, sizeof buf), "second save, same content");
    CHECK(fake_erase_count == 0, "unchanged re-save costs no erase (and no new record)");

    buf[0] = 8;
    CHECK(persist_slots_save(name, PERSIST_KIND_VARS, buf, sizeof buf), "third save, changed content");
    CHECK(fake_erase_count == 0, "a changed save appends a new record, still no erase (room in the sector)");

    uint8_t rbuf[64];
    size_t rlen = 0;
    CHECK(persist_slots_load(name, PERSIST_KIND_VARS, rbuf, sizeof rbuf, &rlen) == PERSIST_LOAD_OK &&
              rlen == sizeof buf && memcmp(rbuf, buf, sizeof buf) == 0,
          "load sees the latest (changed) content");
}

/* Saving the same (name, kind) repeatedly appends a new record each time
 * content changes, filling the sector; once a record wouldn't fit, the
 * sector is erased and the new record starts over at offset 0. For a
 * fixed small record size, this should cost exactly one erase per
 * "sector's worth" of saves, not one erase per save. */
static void test_fill_and_wrap(void)
{
    fake_flash_reset();
    const char *name = "filler";
    uint8_t payload[24];
    uint32_t record_size = persist_record_size(sizeof payload);
    uint32_t records_per_sector = FLASH_SECTOR_SIZE / record_size;
    CHECK(records_per_sector > 1, "sanity: more than one record fits a sector at this size");

    /* Fill the sector exactly: records_per_sector distinct-content saves
     * fit with no erase at all (the sector starts blank). */
    for (uint32_t i = 0; i < records_per_sector; i++) {
        memset(payload, (int)(0x30 + i), sizeof payload);
        CHECK(persist_slots_save(name, PERSIST_KIND_VARS, payload, sizeof payload), "fill save %u", i);
    }
    CHECK(fake_erase_count == 0, "filling a blank sector exactly costs no erase, got %u", fake_erase_count);

    /* One more (still-changing) save doesn't fit: exactly one erase, and
     * it still loads back correctly afterwards. */
    memset(payload, 0xAA, sizeof payload);
    CHECK(persist_slots_save(name, PERSIST_KIND_VARS, payload, sizeof payload), "the save that wraps");
    CHECK(fake_erase_count == 1, "wrapping costs exactly one erase, got %u", fake_erase_count);

    uint8_t rbuf[64];
    size_t rlen = 0;
    CHECK(persist_slots_load(name, PERSIST_KIND_VARS, rbuf, sizeof rbuf, &rlen) == PERSIST_LOAD_OK &&
              rlen == sizeof payload && memcmp(rbuf, payload, sizeof payload) == 0,
          "loads the record written just after the wrap");
}

/* A torn write (the commit word never made it to flash, as if power was
 * lost between writing the rest of the record and committing it) must be
 * ignored on load, falling back to the previous valid record in the
 * sector -- not read as garbage, and not treated as if nothing were ever
 * saved. This pokes the fake flash directly (not through flash_ops_t) to
 * simulate the reset landing exactly between those two writes, something
 * persist_slots.c itself can never do through the fake's NOR-only-clears-
 * bits write() (clearing a bit back to 1 isn't how a real interrupted
 * write behaves; a real reset just leaves it unwritten, i.e. still 1). */
static void test_torn_write_falls_back(void)
{
    fake_flash_reset();
    const char *name = "euclid";
    uint8_t rec1[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    uint8_t rec2[8] = {2, 2, 2, 2, 2, 2, 2, 2};
    CHECK(persist_slots_save(name, PERSIST_KIND_VARS, rec1, sizeof rec1), "save record 1");
    CHECK(persist_slots_save(name, PERSIST_KIND_VARS, rec2, sizeof rec2), "save record 2");

    /* Find record 2's offset the same way persist_slots.c computes it:
     * right after record 1, which is the sector's first (and so far
     * only) record. */
    uint32_t record1_size = persist_record_size(sizeof rec1);
    /* The vars sector address isn't exposed directly, but corrupting via
     * persist_slots_corrupt_vars() tells us nothing about *offset* --
     * so instead, locate it by scanning the fake flash for rec2's known
     * byte pattern, which is unique within it. */
    uint32_t rec2_off = UINT32_MAX;
    for (uint32_t off = 0; off + PERSIST_HDR_SIZE + sizeof rec2 <= FAKE_SIZE; off++) {
        if (memcmp(fake_flash + off + PERSIST_HDR_SIZE, rec2, sizeof rec2) == 0) {
            rec2_off = off;
            break;
        }
    }
    CHECK(rec2_off != UINT32_MAX, "found record 2's bytes in the fake flash");
    if (rec2_off == UINT32_MAX) {
        return;
    }
    /* Record 2 must immediately follow record 1 (append-only, same
     * sector, nothing else saved in between). */
    CHECK(rec2_off % FLASH_SECTOR_SIZE == record1_size, "record 2 lands right after record 1 in its sector");

    /* Simulate the reset: the commit word (the header's last 4 bytes)
     * never got written, so it's still at its erased value. */
    memset(fake_flash + rec2_off + PERSIST_COMMIT_OFFSET, 0xFF, 4);

    /* A fresh "boot" (its own shadow state) must still fall back to
     * record 1, not read record 2's torn bytes or report no saved
     * state. */
    persist_slots_init(&fake_ops, NULL);
    uint8_t rbuf[64];
    size_t rlen = 0;
    CHECK(persist_slots_load(name, PERSIST_KIND_VARS, rbuf, sizeof rbuf, &rlen) == PERSIST_LOAD_OK,
          "torn record 2 falls back to loading OK");
    CHECK(rlen == sizeof rec1 && memcmp(rbuf, rec1, sizeof rec1) == 0,
          "the record loaded is record 1's, not record 2's torn one");

    /* A later save of changed content appends after the torn record's
     * space (not reusing it), and that new record is what loads now. */
    uint8_t rec3[8] = {3, 3, 3, 3, 3, 3, 3, 3};
    CHECK(persist_slots_save(name, PERSIST_KIND_VARS, rec3, sizeof rec3), "save record 3, after the torn one");
    CHECK(persist_slots_load(name, PERSIST_KIND_VARS, rbuf, sizeof rbuf, &rlen) == PERSIST_LOAD_OK &&
              rlen == sizeof rec3 && memcmp(rbuf, rec3, sizeof rec3) == 0,
          "record 3 loads correctly");
}

/* 100 saves of a real-sized record (217 bytes, a plausible vars/store
 * payload) should cost roughly 100 / (records-per-sector) erases, not
 * 100: this is the whole point of append-only records. At this payload
 * size, about 15 records fit a 4 KB sector (4096 / persist_record_size
 * (217) = 4096/272 = 15), so ~7 erases are expected; assert a safety
 * margin of 8 rather than the exact figure, in case the record layout
 * ever changes slightly. */
static void test_erase_budget_for_many_saves(void)
{
    fake_flash_reset();
    const char *name = "heavy";
    uint8_t payload[217];
    for (int i = 0; i < 100; i++) {
        memset(payload, (uint8_t)i, sizeof payload);
        CHECK(persist_slots_save(name, PERSIST_KIND_VARS, payload, sizeof payload), "save %d/100", i);
    }
    uint32_t record_size = persist_record_size(sizeof payload);
    uint32_t per_sector = FLASH_SECTOR_SIZE / record_size;
    fprintf(stderr, "erase_budget_for_many_saves: %u erases for 100 saves (%u bytes/record, %u/sector)\n",
            fake_erase_count, (unsigned)record_size, per_sector);
    CHECK(fake_erase_count <= 8, "100 saves of a %u-byte record cost at most 8 erases, got %u",
          (unsigned)sizeof payload, fake_erase_count);

    uint8_t rbuf[sizeof payload];
    size_t rlen = 0;
    memset(payload, 99, sizeof payload);
    CHECK(persist_slots_load(name, PERSIST_KIND_VARS, rbuf, sizeof rbuf, &rlen) == PERSIST_LOAD_OK &&
              rlen == sizeof payload && memcmp(rbuf, payload, sizeof payload) == 0,
          "the last of the 100 saves is what loads");
}

/* Every address persist_slots.c ever hands the fake flash must fall
 * inside the reserve -- fake_offset_ok() (an assert, like blockdev_
 * test.c's) would already abort the test if it didn't; this just drives
 * every public entry point once so that assert gets exercised. */
static void test_every_access_in_reserve(void)
{
    fake_flash_reset();
    uint8_t buf[8] = {0};
    size_t len = 0;
    persist_slots_save("bounds-a", PERSIST_KIND_VARS, buf, sizeof buf);
    persist_slots_save("bounds-a", PERSIST_KIND_STORE, buf, sizeof buf);
    persist_slots_load("bounds-a", PERSIST_KIND_VARS, buf, sizeof buf, &len);
    persist_slots_load("bounds-a", PERSIST_KIND_STORE, buf, sizeof buf, &len);
    persist_slots_save_shared(buf, sizeof buf);
    persist_slots_load_shared(buf, sizeof buf, &len);
    persist_slots_corrupt_vars("bounds-a");
    persist_slots_load("bounds-a", PERSIST_KIND_VARS, buf, sizeof buf, &len);
    CHECK(fake_write_violations == 0, "no NOR write violations exercising every entry point");
    CHECK(true, "every call above stayed inside the reserve (fake_offset_ok didn't abort)");
}

/* Saving nothing for a scene that never saved claims no slot; once a
 * scene has a slot, an empty save is written (so clearing sticks). */
static void test_empty_save_claims_nothing(void)
{
    fake_flash_reset();
    uint8_t empty_vars[1] = {0};
    CHECK(persist_slots_save_unless_empty("never", PERSIST_KIND_VARS, empty_vars, 1, true), "empty save: true");
    CHECK(persist_slots_save_unless_empty("never", PERSIST_KIND_STORE, NULL, 0, true), "empty store: true");
    CHECK(!persist_slots_claimed("never"), "but no slot claimed");
    CHECK(fake_erase_count == 0, "and nothing erased");
    for (size_t i = 0; i < FAKE_SIZE; i++) {
        if (fake_flash[i] != 0xFF) {
            CHECK(false, "nothing written (byte 0x%zx)", i);
            break;
        }
    }

    uint8_t v[3] = {1, 2, 3};
    CHECK(persist_slots_save_unless_empty("kept", PERSIST_KIND_VARS, v, sizeof v, false), "a real save");
    CHECK(persist_slots_claimed("kept"), "claims its slot");
    CHECK(persist_slots_save_unless_empty("kept", PERSIST_KIND_VARS, empty_vars, 1, true),
          "then clearing it");
    uint8_t rbuf[16];
    size_t rlen = 0;
    CHECK(persist_slots_load("kept", PERSIST_KIND_VARS, rbuf, sizeof rbuf, &rlen) == PERSIST_LOAD_OK && rlen == 1 &&
              rbuf[0] == 0,
          "is written: the cleared vars load back");

    /* A scene with only a store owns its slot too. */
    CHECK(persist_slots_save("storeonly", PERSIST_KIND_STORE, v, sizeof v), "store-only save");
    CHECK(persist_slots_claimed("storeonly"), "the store record names the slot");
}

static const char *const *g_keep;
static bool keep_listed(void *ud, const char *name)
{
    (void)ud;
    for (const char *const *k = g_keep; *k; k++) {
        if (strcmp(*k, name) == 0) {
            return true;
        }
    }
    return false;
}

/* The boot's reclaim frees what keep() doesn't list, keeps the rest, and
 * a freed slot in the middle of another name's probe sequence doesn't
 * hide that name's own slot further along. */
static void test_reclaim(void)
{
    fake_flash_reset();
    /* Three names on one bucket: a gets it, b and c probe on. */
    char a[32] = "", b[32] = "", c[32] = "";
    unsigned target = bucket_of("scene0");
    strcpy(a, "scene0");
    for (int i = 1; i < 200000 && !c[0]; i++) {
        char cand[32];
        snprintf(cand, sizeof cand, "scene%d", i);
        if (bucket_of(cand) == target) {
            strcpy(b[0] ? c : b, cand);
        }
    }
    CHECK(b[0] && c[0], "three names sharing a bucket");
    uint8_t va[2] = {0xA, 0xA}, vb[2] = {0xB, 0xB}, vc[2] = {0xC, 0xC};
    CHECK(persist_slots_save(a, PERSIST_KIND_VARS, va, 2) && persist_slots_save(b, PERSIST_KIND_VARS, vb, 2) &&
              persist_slots_save(c, PERSIST_KIND_STORE, vc, 2),
          "save a, b (vars) and c (store only)");
    CHECK(persist_slots_save_shared(va, 2), "and the shared store");

    const char *keep_bc[] = {b, c, NULL};
    g_keep = keep_bc;
    bool deferred = true;
    int freed = persist_slots_reclaim(keep_listed, NULL, &deferred);
    CHECK(freed == 1 && !deferred, "a's slot freed (freed %d)", freed);
    CHECK(!persist_slots_claimed(a), "a has no slot now");
    uint8_t rbuf[16];
    size_t rlen = 0;
    CHECK(persist_slots_load(b, PERSIST_KIND_VARS, rbuf, sizeof rbuf, &rlen) == PERSIST_LOAD_OK && rbuf[0] == 0xB,
          "b still loads past the freed slot in front of it");
    CHECK(persist_slots_load(c, PERSIST_KIND_STORE, rbuf, sizeof rbuf, &rlen) == PERSIST_LOAD_OK && rbuf[0] == 0xC,
          "c too");
    CHECK(persist_slots_save(b, PERSIST_KIND_VARS, va, 2), "b saves again");
    CHECK(persist_slots_load(b, PERSIST_KIND_VARS, rbuf, sizeof rbuf, &rlen) == PERSIST_LOAD_OK && rbuf[0] == 0xA,
          "into its own slot, not the freed one");
    CHECK(persist_slots_load_shared(rbuf, sizeof rbuf, &rlen) == PERSIST_LOAD_OK, "the shared store is untouched");

    /* Nothing kept: everything but the shared slot goes. */
    static const char *const none[] = {NULL};
    g_keep = none;
    freed = persist_slots_reclaim(keep_listed, NULL, &deferred);
    CHECK(freed == 2, "b and c freed (freed %d)", freed);
    CHECK(persist_slots_load_shared(rbuf, sizeof rbuf, &rlen) == PERSIST_LOAD_OK, "the shared store still there");
    CHECK(persist_slots_reclaim(keep_listed, NULL, &deferred) == 0, "nothing left to free");

    /* A full table: reclaim makes room. */
    fake_flash_reset();
    char names[SCENE_BUCKETS][16];
    for (unsigned i = 0; i < SCENE_BUCKETS; i++) {
        snprintf(names[i], sizeof names[i], "old%u", i);
        uint8_t one[1] = {1};
        persist_slots_save(names[i], PERSIST_KIND_VARS, one, 1);
    }
    uint8_t one[1] = {1};
    CHECK(!persist_slots_save("newcomer", PERSIST_KIND_VARS, one, 1), "the table is full");
    const char *keep_some[] = {names[0], names[1], NULL};
    g_keep = keep_some;
    freed = persist_slots_reclaim(keep_listed, NULL, &deferred);
    CHECK(freed == (int)SCENE_BUCKETS - 2, "all but the two kept freed (%d)", freed);
    CHECK(persist_slots_save("newcomer", PERSIST_KIND_VARS, one, 1), "now there's room");
    CHECK(persist_slots_load(names[1], PERSIST_KIND_VARS, rbuf, sizeof rbuf, &rlen) == PERSIST_LOAD_OK,
          "a kept name still loads");
}

int main(void)
{
    test_save_load_roundtrip();
    test_blank_flash_is_no_saved_state();
    test_corrupt_record();
    test_shared_slot();
    test_hash_collision();
    test_full_table();
    test_shadow_skips_unchanged_save();
    test_fill_and_wrap();
    test_torn_write_falls_back();
    test_erase_budget_for_many_saves();
    test_every_access_in_reserve();
    test_empty_save_claims_nothing();
    test_reclaim();

    if (g_failures == 0) {
        printf("ALL PASSED\n");
        return 0;
    }
    printf("%d FAILURES\n", g_failures);
    return 1;
}
