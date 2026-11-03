/*
 * The synth as a USB audio input: port_e16_usbaudio.h.
 */
#include "port_e16_usbaudio.h"
#include "log.h"

#include "usb_audio.h"
#include "usb_video.h"

#define SAMPLE_RATE 48000u
#define FRAMES_PER_MS (SAMPLE_RATE / 1000u)
#define RING_FRAMES 512u   /* power of two; 10.7 ms */
#define TARGET_FRAMES 96u  /* 2 ms: how full fill() keeps the ring */
#define SLACK_FRAMES 16u   /* within +-this of the target, send 48 */
#define STATS_LOG_MS 10000u

/* SRAM, not CCM (app mode gives the app all of the CCM; apps play the
 * synth too). Written by the audio interrupt at [head], read by fill()
 * at [tail]; each index has a single writer. */
static int16_t g_ring[RING_FRAMES];
static volatile uint32_t g_head; /* audio interrupt only */
static volatile uint32_t g_tail; /* fill() only */
static volatile bool g_primed;   /* fill() only: the ring reached the target since it last ran dry */
static volatile bool g_live;     /* main loop: the producer is running */

/* Packet statistics (fill() writes, the main loop reads and resets). */
static volatile uint32_t g_sent_47, g_sent_48, g_sent_49, g_sent_short;
static volatile uint32_t g_silent, g_underruns, g_resyncs;

static bool g_was_streaming;
static uint32_t g_stats_t0;

void port_e16_usbaudio_push(int16_t s)
{
    uint32_t head = g_head;
    if (head - g_tail >= RING_FRAMES) {
        return; /* full: nobody is recording */
    }
    g_ring[head & (RING_FRAMES - 1u)] = s;
    __atomic_thread_fence(__ATOMIC_RELEASE); /* the sample before the index that publishes it */
    g_head = head + 1u;
}

void port_e16_usbaudio_producer(bool on)
{
    g_live = on;
}

/* USB interrupt, every millisecond while the computer records. */
static uint16_t fill(int16_t *buf, uint16_t max_frames)
{
    uint32_t tail = g_tail;
    uint32_t avail = g_head - tail;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);

    if (!g_primed) {
        if (avail < TARGET_FRAMES) {
            g_silent++;
            return 0; /* the SDK sends a packet of silence */
        }
        g_primed = true;
    }
    /* Recording just started on a ring that filled up meanwhile (or the
     * host skipped frames): keep only the newest 2 ms. */
    if (avail > 2u * TARGET_FRAMES) {
        tail += avail - TARGET_FRAMES;
        avail = TARGET_FRAMES;
        g_resyncs++;
    }

    uint32_t n = FRAMES_PER_MS;
    if (avail > TARGET_FRAMES + SLACK_FRAMES) {
        n++;
    } else if (avail < TARGET_FRAMES - SLACK_FRAMES) {
        n--;
    }
    if (n > max_frames) {
        n = max_frames;
    }
    if (n > avail) {
        n = avail;
        if (n == 0) {
            g_primed = false;
            if (g_live) {
                g_underruns++;
            }
            g_tail = tail;
            g_silent++;
            return 0;
        }
        g_sent_short++;
    } else if (n == FRAMES_PER_MS) {
        g_sent_48++;
    } else if (n > FRAMES_PER_MS) {
        g_sent_49++;
    } else {
        g_sent_47++;
    }
    for (uint32_t i = 0; i < n; i++) {
        buf[i] = g_ring[(tail + i) & (RING_FRAMES - 1u)];
    }
    g_tail = tail + n;
    return (uint16_t)n;
}

static const usb_audio_ops_t g_ops = {
    .sample_rate = SAMPLE_RATE,
    .channels = 1,
    .fill = fill,
};
bool port_e16_usbaudio_start(void)
{
    return usb_audio_start(&g_ops);
}

bool port_e16_usbaudio_available(void)
{
    return !usb_video_active() && !usb_video_switching();
}

bool port_e16_usbaudio_streaming(void)
{
    return usb_audio_streaming();
}

static void log_stats(uint32_t ms)
{
    /* Each counter is read and cleared on its own: fill() may bump one in
     * between, which only moves a count into the next report. */
    uint32_t s47 = g_sent_47, s48 = g_sent_48, s49 = g_sent_49, sshort = g_sent_short;
    uint32_t silent = g_silent, under = g_underruns, resync = g_resyncs;
    g_sent_47 = g_sent_48 = g_sent_49 = g_sent_short = 0;
    g_silent = g_underruns = g_resyncs = 0;
    log_printf("usbaudio: %lu ms: packets 47:%lu 48:%lu 49:%lu short:%lu silent:%lu, underruns %lu, "
               "resyncs %lu, ring %lu",
               (unsigned long)ms, (unsigned long)s47, (unsigned long)s48, (unsigned long)s49,
               (unsigned long)sshort, (unsigned long)silent, (unsigned long)under, (unsigned long)resync,
               (unsigned long)(g_head - g_tail));
}

void port_e16_usbaudio_poll(uint32_t now_ms)
{
    bool on = usb_audio_streaming();
    if (on != g_was_streaming) {
        g_was_streaming = on;
        if (on) {
            g_stats_t0 = now_ms;
            g_sent_47 = g_sent_48 = g_sent_49 = g_sent_short = 0;
            g_silent = g_underruns = g_resyncs = 0;
            log_printf("usbaudio: the computer started recording (48 kHz mono)");
        } else {
            log_printf("usbaudio: the computer stopped recording");
        }
        return;
    }
    if (on && (int32_t)(now_ms - g_stats_t0) >= (int32_t)STATS_LOG_MS) {
        log_stats(now_ms - g_stats_t0);
        g_stats_t0 = now_ms;
    }
}
