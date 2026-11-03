/*
 * A native app's audio stream between its callback and the audio
 * interrupt: app_stream.h.
 */
#include "app_stream.h"

void app_stream_init(app_stream_t *s, uint32_t block_cycles, uint32_t budget_permille)
{
    s->state = APP_STREAM_OFF;
    s->block_cycles = block_cycles ? block_cycles : 1u;
    s->budget_permille = budget_permille;
    s->req = s->done = 0;
    s->pos = s->play = 0;
}

void app_stream_start(app_stream_t *s)
{
    for (uint32_t b = 0; b < 2u; b++) {
        for (uint32_t i = 0; i < APP_STREAM_BLOCK; i++) {
            s->buf[b][i] = 0.0f;
        }
    }
    s->pos = 0;
    s->play = 0;
    s->done = 0;
    s->req = 1; /* block 1, while block 0's silence plays */
    s->cur = 0;
    s->win_sum = s->win_gross = 0;
    s->win_max = s->win_blocks = s->win_late0 = 0;
    s->over = 0;
    s->stop_why = APP_STREAM_WHY_NONE;
    s->last_avg_permille = s->last_peak_permille = s->last_late = 0;
    s->windows = 0;
    s->late = 0;
    s->calls = 0;
    /* Everything above before the state the audio interrupt checks first
     * (one core: a compiler barrier is all it takes). */
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    s->state = APP_STREAM_RUNNING;
}

void app_stream_stop(app_stream_t *s)
{
    s->state = APP_STREAM_OFF;
}

float app_stream_next(app_stream_t *s, bool *kick)
{
    if (s->state != APP_STREAM_RUNNING) {
        return 0.0f;
    }
    if (s->pos >= APP_STREAM_BLOCK) {
        s->pos = 0;
        if (s->done != s->req) {
            s->late++; /* played as far as the callback got */
        }
        s->play ^= 1u;
        s->req++; /* asks for (req & 1) == the block that just finished playing */
        *kick = true;
    }
    return s->buf[s->play][s->pos++];
}

float *app_stream_render_target(app_stream_t *s)
{
    if (s->state != APP_STREAM_RUNNING) {
        return 0;
    }
    uint32_t r = s->req;
    if (r == s->done) {
        return 0;
    }
    s->cur = r;
    return s->buf[r & 1u];
}

bool app_stream_rendered(app_stream_t *s, uint32_t cycles, uint32_t gross)
{
    s->done = s->cur;
    s->calls++;
    s->win_sum += cycles;
    s->win_gross += gross > cycles ? gross : cycles;
    if (cycles > s->win_max) {
        s->win_max = cycles;
    }
    ++s->win_blocks;
    /* A whole window's wall time spent in calls already (nothing below
     * the callback has run since the window began): the window ends
     * now, and so does the stream, without waiting for more windows. */
    bool starving = s->win_gross >= (uint64_t)APP_STREAM_WINDOW_BLOCKS * s->block_cycles;
    if (s->win_blocks < APP_STREAM_WINDOW_BLOCKS && !starving) {
        return true;
    }
    /* A window's end. */
    uint64_t avg_pm = s->win_sum * 1000u / s->win_blocks / s->block_cycles;
    uint64_t peak_pm = (uint64_t)s->win_max * 1000u / s->block_cycles;
    s->last_avg_permille = avg_pm > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)avg_pm;
    s->last_peak_permille = peak_pm > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)peak_pm;
    uint32_t late = s->late - s->win_late0;
    s->last_late = late;
    s->windows++;
    bool too_late = (uint64_t)late * 1000u > (uint64_t)APP_STREAM_LATE_PERMILLE * s->win_blocks;
    s->win_sum = s->win_gross = 0;
    s->win_max = s->win_blocks = 0;
    s->win_late0 = s->late;
    if (avg_pm > s->budget_permille || too_late || starving) {
        if (++s->over >= APP_STREAM_OVER_WINDOWS || starving) {
            s->stop_why = starving                          ? APP_STREAM_WHY_STARVING
                          : avg_pm > s->budget_permille     ? APP_STREAM_WHY_BUDGET
                                                            : APP_STREAM_WHY_LATE;
            s->state = APP_STREAM_STOP_OVERLOAD;
            return false;
        }
    } else {
        s->over = 0;
    }
    return true;
}
