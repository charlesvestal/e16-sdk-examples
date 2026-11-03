#ifndef SYNTH_APP_STREAM_H
#define SYNTH_APP_STREAM_H
#include <stdbool.h>
#include <stdint.h>

/*
 * A native app's own audio (e16_api_t's audio_start()), between the app's
 * render callback and the per-sample audio interrupt: pure logic, no
 * hardware, so it's host-tested (test/synth/app_stream_test.c).
 * port_e16_audio.c drives it.
 *
 * Double buffering: two blocks of APP_STREAM_BLOCK frames. The audio
 * interrupt (TIM6, every sample) plays one block, app_stream_next(), while
 * the app's callback fills the other in a lower-priority interrupt. When
 * the played block runs out, the two swap and next() asks for a render
 * (*kick): the callback then has one block's time (APP_STREAM_BLOCK / 48
 * kHz = 0.67 ms) to fill the block that was just played. A block that
 * isn't finished when it's due is played anyway, as far as it got (a
 * glitch), and counted as late.
 *
 * Load watch: app_stream_rendered() gets each call's cycle counts and
 * keeps one-second windows (APP_STREAM_WINDOW_BLOCKS calls). Two counts:
 * net, the callback's own (the audio interrupts that preempted it taken
 * out: the load an app is answerable for, reported and budgeted), and
 * gross, the call's wall time (everything that ran meanwhile included).
 * A window is overloaded when
 *   - its net average is over the budget, or
 *   - more than APP_STREAM_LATE_PERMILLE of its blocks were late: the
 *     callback plus everything above it (the audio interrupt with the
 *     built-in voice playing, USB, LEDs) doesn't fit in real time, even
 *     if the callback's own share is under budget.
 * APP_STREAM_OVER_WINDOWS overloaded windows in a row stop the stream
 * (APP_STREAM_STOP_OVERLOAD): the callback isn't called again and the
 * stream plays silence, so a callback that's too slow can't starve the
 * rest of the system for good. And as soon as a window's calls have
 * taken a whole window's worth of wall time (gross: nothing below the
 * callback -- the app, its api calls, the log -- has run since the
 * window began), the window ends and the stream stops right there: about
 * a second of starvation at most, not three or more. (A callback that
 * never returns at all is the SysTick's to catch: app_mode.c.)
 *
 * Threads: next() runs in the audio interrupt, render_target() and
 * rendered() in the render interrupt (lower priority: next() can preempt
 * them, never the other way round), start()/stop()/the readers in the main
 * loop with the interrupts' cooperation (port_e16_audio.c).
 */

#define APP_STREAM_BLOCK 32u          /* frames per callback call */
#define APP_STREAM_WINDOW_BLOCKS 1500u /* one second at 48 kHz */
#define APP_STREAM_OVER_WINDOWS 3u    /* overloaded seconds in a row before the stop */
#define APP_STREAM_LATE_PERMILLE 50u  /* late blocks in a window that make it overloaded (5 %) */

typedef enum {
    APP_STREAM_OFF = 0,        /* never started, or stopped by the app */
    APP_STREAM_RUNNING,
    APP_STREAM_STOP_OVERLOAD,  /* stopped: over budget too long (stop_why) */
} app_stream_state_t;

/* Why an overload stop happened. */
typedef enum {
    APP_STREAM_WHY_NONE = 0,
    APP_STREAM_WHY_BUDGET,     /* the callback's own load, 3 windows */
    APP_STREAM_WHY_LATE,       /* late blocks, 3 windows */
    APP_STREAM_WHY_STARVING,   /* a window's whole wall time in calls */
} app_stream_why_t;

typedef struct {
    float buf[2][APP_STREAM_BLOCK];
    volatile uint32_t state;   /* app_stream_state_t */
    uint32_t pos;              /* next frame of the playing block */
    uint32_t play;             /* the block being played, 0 or 1 */
    /* Render requests: the audio interrupt bumps req at every swap; the
     * request asks for block (req & 1), the one not playing. The render
     * side sets done = the req it finished. req != done: one is due. */
    volatile uint32_t req, done;
    uint32_t cur;              /* the req being rendered */

    /* load accounting (the render side) */
    uint32_t block_cycles;     /* CPU cycles in one block's time */
    uint32_t budget_permille;  /* of block_cycles, averaged over a window */
    uint64_t win_sum;          /* net cycles */
    uint64_t win_gross;        /* wall cycles */
    uint32_t win_max, win_blocks;
    uint32_t win_late0;        /* late at the window's start */
    uint32_t over;             /* overloaded windows in a row */
    /* the last finished window (main-loop readers) */
    volatile uint32_t last_avg_permille, last_peak_permille;
    volatile uint32_t last_late;   /* late blocks in it */
    volatile uint32_t stop_why;    /* app_stream_why_t, once stopped */
    volatile uint32_t windows;  /* finished windows so far */

    /* counters since start */
    volatile uint32_t late;     /* blocks played before the callback finished them */
    volatile uint32_t calls;    /* callback calls finished */
} app_stream_t;

/* Sets up a stopped stream. block_cycles: CPU cycles in one block's time
 * (SystemCoreClock / 48000 * APP_STREAM_BLOCK); budget_permille: the
 * sustained average the callback may use, of that. */
void app_stream_init(app_stream_t *s, uint32_t block_cycles, uint32_t budget_permille);

/* Main loop, with neither interrupt touching s: clears the blocks and the
 * counters, and starts. The first block plays silence while the second is
 * rendered: returns with a render asked for (call the callback's
 * interrupt). */
void app_stream_start(app_stream_t *s);

/* Main loop: stops (silence from the next sample). */
void app_stream_stop(app_stream_t *s);

/* Audio interrupt: the next sample (0 unless running). *kick is set when
 * a render should be asked for (left alone otherwise). */
float app_stream_next(app_stream_t *s, bool *kick);

/* Render interrupt: the block to fill, or NULL if there's nothing to do
 * (not running, or nothing asked for). */
float *app_stream_render_target(app_stream_t *s);

/* Render interrupt, after the callback filled the target: the cycles it
 * took, net (its own) and gross (wall time, preemptions included).
 * Returns false if this stopped the stream (the overload rules). */
bool app_stream_rendered(app_stream_t *s, uint32_t net_cycles, uint32_t gross_cycles);

/* The mix: the built-in voice's sample (already gained and soft-clipped
 * to within +-1) plus the app's, clamped to +-1; a NaN from the app
 * counts as 0. */
static inline float app_stream_mix(float voice, float app)
{
    /* NaN != NaN (no -ffast-math in these builds, which could drop the
     * test). An infinity clamps like any other big value. */
    if (!(app == app)) {
        app = 0.0f;
    }
    float s = voice + app;
    if (s > 1.0f) {
        s = 1.0f;
    } else if (s < -1.0f) {
        s = -1.0f;
    }
    return s;
}

#endif
