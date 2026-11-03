#ifndef BOOT_RECORDS_H
#define BOOT_RECORDS_H
#include <stdint.h>

#include "mem_layout.h"

/*
 * Records that survive a reset (not a power cycle), at the top of the CCM
 * (mem_layout.h's CCM_RECORDS_BASE): outside every linker section, so
 * neither startup code, nor the launcher's .ccmram buffers, nor an app's
 * data ever overlap them. Each carries a check word so power-on garbage
 * never reads as a record.
 *
 *   crash  the fault handler's PC/LR (main.c), logged on the next boot
 *   app    "run app <path>" from the menu, read early in the next boot
 *          (src/apps/app_mode.c), and how the app ended, for the boot after
 *   noise  the noise lab's settings (src/noise_lab.h)
 *   launch the launch token the launcher writes right before its reset
 *          into app mode (src/apps/app_boot.h); every boot consumes it
 *
 * New records go at the end, so the older ones keep their addresses
 * across a reflash.
 */

#define CRASH_MAGIC 0xC0DEFA17u
typedef struct {
    uint32_t magic;
    uint32_t pc;
    uint32_t lr;
    uint32_t check; /* magic ^ pc ^ lr */
} crash_record_t;

#define APP_RECORD_MAGIC 0xE16A9A11u
#define APP_RECORD_PATH_MAX 96
#define APP_RECORD_MSG_MAX 64

typedef enum {
    APP_REC_NONE = 0,
    APP_REC_RUN,      /* launcher -> next boot: load and run path */
    APP_REC_RUNNING,  /* app mode took the request (set before loading) */
    APP_REC_ENDED,    /* app mode -> next boot: it ended (reason, code) */
    APP_REC_FAILED,   /* app mode -> next boot: it didn't load (msg) */
} app_rec_state_t;

typedef enum {
    APP_END_NONE = 0,
    APP_END_EXIT,     /* api->exit() or app_main() returned (code) */
    APP_END_SHIFT,    /* Shift held 1 s */
    APP_END_FAULT,    /* a fault handler (main.c) */
    APP_END_STACK,    /* the stack canary was hit */
    APP_END_CONSOLE,  /* the dev console's reset */
    APP_END_AUDIO,    /* the app's audio callback didn't return for 500 ms */
} app_end_t;

typedef struct {
    uint32_t magic;
    uint32_t state;   /* app_rec_state_t */
    uint32_t reason;  /* app_end_t */
    int32_t code;     /* the exit code, for APP_END_EXIT */
    char path[APP_RECORD_PATH_MAX]; /* "/apps/<file>.e16app" */
    char msg[APP_RECORD_MSG_MAX];   /* why it didn't load */
    uint32_t check;   /* app_record_check() of everything above */
} app_record_t;

#define NOISE_RECORD_MAGIC 0x401CE1ABu
typedef struct {
    uint32_t magic;
    uint8_t set;        /* NOISE_SET_* (noise_lab.c): which display values below apply */
    uint8_t contrast;   /* 0x81 */
    uint8_t clock;      /* 0xD5 */
    uint8_t precharge;  /* 0xD9 */
    uint8_t vcomh;      /* 0xDB */
    uint8_t flags;      /* NOISE_F_* (noise_lab.c) */
    uint8_t led_level;  /* 0-255, 255 = as drawn */
    uint8_t reserved;
    uint32_t check;     /* magic ^ the two words above */
} noise_record_t;

typedef struct {
    crash_record_t crash;
    app_record_t app;
    noise_record_t noise;
    uint32_t launch_token; /* app_boot_token() of the app record, 0 = none */
} boot_records_t;

_Static_assert(sizeof(boot_records_t) <= CCM_RECORDS_SIZE, "boot records fit the CCM records area");

/* Plain (not volatile) access: the records are only read at boot and
 * written right before a reset, and every reset path (NVIC_SystemReset())
 * has a compiler barrier. */
#if defined(__arm__)
#define BOOT_RECORDS ((boot_records_t *)CCM_RECORDS_BASE)
#endif

#endif
