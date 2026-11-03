#ifndef RT_SCENE_H
#define RT_SCENE_H
#include <stdint.h>

#define RT_MAX_PAGES 12

/* Longest scene name rt_load() accepts (not counting the NUL). var and
 * store key their per-scene RAM (rt_api_var.c, rt_api_store.c) by this
 * name in a fixed buffer sized for it; rt_load() rejects a longer name
 * outright (clear load failure) rather than truncating it, which could
 * alias two different scripts' saved names onto the same vars/store. */
#define RT_SCENE_NAME_MAX 31

typedef struct {
    const char *name;                 /* script name, e.g. "euclid" */
    const char *title;                /* default header, e.g. "Euclid" */
    const char *page_names[RT_MAX_PAGES];
    uint8_t pages;                    /* number of pages */
    uint8_t pad_pages;                /* pages 1..pad_pages use ids 1-32 */
    uint8_t settings_page;            /* page using ids 33-64 (0 = none) */
    uint8_t assigned[64 / 8];         /* bit (id-1) set when --@assign declares id */
    const char *abbr[64];             /* default label per id (from --@assign abbr=) */
} rt_scene_t;

#endif
