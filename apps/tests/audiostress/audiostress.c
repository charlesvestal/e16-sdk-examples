/*
 * audiostress: exercises the app audio stream's guards (e16_api.h's
 * audio_start()) in a scripted run (docs/hw-acceptance.md). A quiet
 * 440 Hz triangle plays throughout; the callback also burns CPU:
 *
 *   0-6 s    ~30%: running
 *   6-12 s   ~90%: over the 80% budget (and blocks late); stopped
 *            after 3 s ("audio: app callback over budget ...")
 *   12 s     audio_start() again at ~30%: running
 *   15 s     ~150%: stopped within about a second, when the calls have
 *            taken a second's wall time ("audio: app callback took all
 *            the CPU for a second ...")
 *   19 s     audio_start() again at ~30%
 *   22 s     ~78% while the app also plays the synth voice (the audio
 *            interrupt then costs ~16% more): the callback's own load is
 *            under budget but blocks are late; stopped after 3 s
 *            ("audio: app blocks late ...")
 *   28 s     the voice off, audio_start() again at ~30%
 *   31 s     the callback never returns: the launcher ends the app
 *            ("back from app audiostress (audio callback stuck)")
 *
 * Each second it logs audio_info(): state, load, late, calls.
 */
#include "e16_app.h"

static const e16_api_t *api;
static volatile int burn;     /* busy-loop iterations per block */
static volatile int hang;     /* the callback never returns */
static float phase;

static void render(float *out, uint32_t frames, void *user)
{
    (void)user;
    for (uint32_t i = 0; i < frames; i++) {
        phase += 440.0f / (float)E16_AUDIO_RATE;
        if (phase >= 1.0f)
            phase -= 1.0f;
        float t = phase < 0.5f ? phase : 1.0f - phase; /* triangle */
        out[i] = (t * 4.0f - 1.0f) * 0.1f;
    }
    for (volatile int i = 0; i < burn; i++) {
    }
    while (hang) {
    }
}

static char *put_s(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    *p = 0;
    return p;
}

static char *put_u(char *p, uint32_t v)
{
    char tmp[10];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        *p++ = tmp[--n];
    *p = 0;
    return p;
}

static void report(uint32_t t)
{
    e16_audio_info_t info;
    char s[120], *p = s;
    if (api->audio_info(&info, sizeof info) != 0) {
        api->log("audiostress: audio_info failed");
        return;
    }
    static const char *const st[3] = {"off", "running", "OVERLOAD"};
    p = put_s(p, "audiostress: t=");
    p = put_u(p, t);
    p = put_s(p, "s burn=");
    p = put_u(p, (uint32_t)burn);
    p = put_s(p, " state=");
    p = put_s(p, info.state < 3 ? st[info.state] : "?");
    p = put_s(p, " load=");
    p = put_u(p, info.load / 10);
    p = put_s(p, ".");
    p = put_u(p, info.load % 10);
    p = put_s(p, "% peak=");
    p = put_u(p, info.load_peak / 10);
    p = put_s(p, "% late=");
    p = put_u(p, info.late);
    p = put_s(p, " calls=");
    put_u(p, info.calls);
    api->log(s);
}

int app_main(const e16_api_t *a)
{
    api = a;
    if (!E16_API_HAS(api, audio_start)) {
        api->log("audiostress: no audio_start in this launcher");
        return 1;
    }
    /* about 14 cycles an iteration (code runs from SRAM1's alias at 0,
     * on the I-code/D-code buses; it was 17 from 0x2000xxxx); a block is
     * 112000 cycles: 81 iterations per percent (measured: 1980 -> 24.3%,
     * 5940 -> 73.5%) */
    const int per_pct = 81;
    burn = 30 * per_pct;
    api->audio_start(render, 0);
    uint32_t t0 = api->millis(), last = 0;
    int step = 0;
    for (;;) {
        uint32_t t = (api->millis() - t0) / 1000u;
        if (t != last) {
            last = t;
            report(t);
        }
        if (step == 0 && t >= 6) {
            step++;
            burn = 90 * per_pct;
            api->log("audiostress: ~90% now");
        } else if (step == 1 && t >= 12) {
            step++;
            burn = 30 * per_pct;
            api->log("audiostress: audio_start again at ~30%");
            api->audio_start(render, 0);
        } else if (step == 2 && t >= 15) {
            step++;
            burn = 150 * per_pct;
            api->log("audiostress: ~150% now");
        } else if (step == 3 && t >= 19) {
            step++;
            burn = 30 * per_pct;
            api->log("audiostress: audio_start again at ~30%");
            api->audio_start(render, 0);
        } else if (step == 4 && t >= 22) {
            step++;
            burn = 78 * per_pct;
            api->log("audiostress: ~78% plus the synth voice now");
            api->synth_param(3, 127); /* VOICE_P_DECAY (src/synth/voice.h): the longest */
            api->synth_note(45, 90);  /* held: the voice renders throughout */
        } else if (step == 5 && t >= 28) {
            step++;
            api->synth_note(45, 0);
            burn = 30 * per_pct;
            api->log("audiostress: voice off, audio_start again at ~30%");
            api->audio_start(render, 0);
        } else if (step == 6 && t >= 31) {
            step++;
            api->log("audiostress: the callback hangs now");
            hang = 1;
        }
        api->delay_ms(10);
    }
}
