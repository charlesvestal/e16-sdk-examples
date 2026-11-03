/*
 * vars and store on SPI flash: the device-facing half (persist.h). The
 * record format and the name-keyed slot table are src/disk/persist_slots.
 * h/.c, portable over flash_ops_t; this file is just the binding to the
 * runtime's serialization (rt.h) and to storage_init()/storage_size()'s
 * availability checks, both of which only make sense on the device.
 */
#include "persist.h"

#include "flash_budget.h"
#include "log.h"
#include "persist_slots.h"
#include "rt.h"
#include "rt_scene.h"
#include "scenes.h"
#include "storage.h"
#include "system.h"

#include <stdbool.h>
#include <stddef.h>

/* Payload buffers follow the runtime's own caps (rt.h); PERSIST_MAX_PAYLOAD
 * (persist_slots.h) is what's actually left in a sector once the record
 * header is accounted for, so a serialize can only fail to fit if those
 * caps change without the reserve's sector size keeping up -- caught here
 * at compile time rather than the first time it's saved. */
#define VARS_CAP RT_VARS_SERIALIZED_MAX
#define STORE_CAP RT_STORE_SERIALIZED_MAX
_Static_assert(RT_SCENE_NAME_MAX < PERSIST_NAME_LEN, "a scene name must fit the record header");
_Static_assert(VARS_CAP <= PERSIST_MAX_PAYLOAD, "vars record must fit a sector");
_Static_assert(STORE_CAP <= PERSIST_MAX_PAYLOAD, "store record must fit a sector");

static bool g_enabled = false; /* storage present and big enough for the reserve */

void persist_init(void)
{
    if (!storage_init()) {
        log_printf("persist: no SPI flash found; vars/store won't be saved");
        return;
    }
    uint32_t size = storage_size();
    uint32_t need = PERSIST_RESERVE_BASE + PERSIST_RESERVE_SIZE;
    if (size < need) {
        log_printf("persist: SPI flash is %lu bytes, need %lu; vars/store won't be saved",
                   (unsigned long)size, (unsigned long)need);
        return;
    }
    persist_slots_init(&flash_budget_ops, system_millis);
    g_enabled = true;
}

bool persist_save_vars(const char *name)
{
    if (!g_enabled) {
        return true; /* nothing to retry: there's no flash to save to */
    }
    uint8_t buf[VARS_CAP];
    size_t len = rt_vars_serialize(buf, sizeof buf);
    return persist_slots_save_unless_empty(name, PERSIST_KIND_VARS, buf, len, rt_vars_serialized_empty(buf, len));
}

bool persist_save_store(const char *name)
{
    if (!g_enabled) {
        return true;
    }
    uint8_t buf[STORE_CAP];
    size_t len = rt_store_serialize(buf, sizeof buf);
    return persist_slots_save_unless_empty(name, PERSIST_KIND_STORE, buf, len,
                                           rt_store_serialized_empty(buf, len));
}

bool persist_save_shared(void)
{
    if (!g_enabled) {
        return true;
    }
    uint8_t buf[STORE_CAP];
    size_t len = rt_store_shared_serialize(buf, sizeof buf);
    return persist_slots_save_shared(buf, len);
}

void persist_load_vars(const char *name)
{
    uint8_t buf[VARS_CAP];
    size_t len = 0;
    if (g_enabled && persist_slots_load(name, PERSIST_KIND_VARS, buf, sizeof buf, &len) == PERSIST_LOAD_OK) {
        rt_vars_deserialize(buf, len);
    } else {
        rt_vars_deserialize(NULL, 0);
    }
}

void persist_load_store(const char *name)
{
    uint8_t buf[STORE_CAP];
    size_t len = 0;
    if (g_enabled &&
        persist_slots_load(name, PERSIST_KIND_STORE, buf, sizeof buf, &len) == PERSIST_LOAD_OK) {
        rt_store_deserialize(buf, len);
    } else {
        rt_store_deserialize(NULL, 0);
    }
}

void persist_load_shared(void)
{
    uint8_t buf[STORE_CAP];
    size_t len = 0;
    if (g_enabled && persist_slots_load_shared(buf, sizeof buf, &len) == PERSIST_LOAD_OK) {
        rt_store_shared_deserialize(buf, len);
    } else {
        rt_store_shared_deserialize(NULL, 0);
    }
}

void persist_debug_corrupt_vars(const char *name)
{
    if (!g_enabled) {
        return;
    }
    persist_slots_corrupt_vars(name);
}

static bool keep_name(void *ud, const char *name)
{
    (void)ud;
    return scenes_name_exists(name);
}

void persist_reclaim(void)
{
    if (!g_enabled) {
        return;
    }
    bool deferred = false;
    int n = persist_slots_reclaim(keep_name, NULL, &deferred);
    if (n > 0 || deferred) {
        log_printf("persist: freed %d slots%s", n, deferred ? " (the rest next boot: flash budget)" : "");
    }
}
