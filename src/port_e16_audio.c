/*
 * The synth voice, over USB audio and on TRS 2: see port_e16_audio.h. The
 * audio interrupt is TIM6's (the SDK leaves TIM6 and TIM8 free: TIM3
 * scans the encoders, TIM7 services USB); the PWM setup follows the SDK's
 * pwm_audio example.
 */
#include "port_e16_audio.h"
#include "port_e16_usbaudio.h"
#include "rt_port.h"
#include "voice.h"
#include "app_stream.h"
#include "log.h"

#include "board.h"
#include "midi.h"
#include "system.h"

/*
 * 48 kHz, always: USB audio needs it (usb_audio.h), and one rate for both
 * outputs keeps the voice (whose coefficients depend on the rate) from
 * having to be rebuilt when the computer starts or stops recording. It
 * costs about half again the CPU of the old 32 kHz (the "audio: load"
 * line logs it). 84 MHz / 48 kHz = 1750 timer ticks exactly.
 */
#define SAMPLE_RATE 48000u
#define PWM_PERIOD 256u             /* 8-bit: 168 MHz / 256 = 656 kHz */
/*
 * Above everything the SDK sets up (USB and its service timer, the LED
 * DMA: 6; MIDI UARTs, LED UARTs: 5; encoders: 7), so neither USB work
 * nor a busy main loop delays a sample. The handler is short: drain the
 * note queue, render one sample, push it to the USB ring and/or write
 * the compare register.
 */
#define AUDIO_IRQ_PRIORITY 2
#define FADE_MS 20u                 /* DC ramp on TRS 2 when switching */
#define LOAD_LOG_MS 10000u
#define STOP_AFTER_MS 50u           /* the voice's release tail before the interrupt stops */
#define RELEASE_WAIT_MS 80u         /* longest an unload waits for the fade and the tail */
/* Exception entry/exit (with lazy FPU stacking) aren't inside the
 * handler's own DWT measurement; this is added to the load estimate. */
#define IRQ_OVERHEAD_CYCLES 40u
/*
 * Output level: the voice peaks at about 0.3-0.75 of full scale (a held
 * note at the default volume about 0.45, an accented acid line with high
 * resonance about 0.75) and averages far less, so its samples are
 * doubled and then soft-clipped (the cubic curve the voice's filter uses:
 * -0.3 dB at half scale, reaching full scale at 1.5) instead of wasting
 * two thirds of the 16-bit range, or clipping hard.
 */
#define OUT_GAIN 2.0f

/*
 * A native app's own stream (app_stream.h): its callback runs in this
 * interrupt, asked for by the audio interrupt (a software pend) once per
 * APP_STREAM_BLOCK frames. CAN2's TX vector: the E16 has no CAN, and
 * nothing else uses it. Priority 8: below every driver interrupt (the
 * SDK's go down to 7: encoders), so a callback that takes most of a block
 * never delays a MIDI byte, an LED frame or a USB packet; above PendSV
 * (app mode's service, 15) and the app's own code.
 */
#define RENDER_IRQn CAN2_TX_IRQn
#define RENDER_IRQ_PRIORITY 8
#define APP_BUDGET_PERMILLE 800u     /* e16_api.h's E16_AUDIO_BUDGET */
#define APP_STUCK_MS 500u            /* a call that long ends the app (app_mode.c) */

/* Main-loop -> interrupt note queue (single producer, single consumer). */
#define QUEUE_LEN 64u               /* power of two */
enum { EV_NOTE_ON = 1, EV_NOTE_OFF, EV_ALL_OFF };
#define EV(type, a, b) (((uint32_t)(type) << 16) | ((uint32_t)(a) << 8) | (uint32_t)(b))

typedef enum {
    TRS2_MIDI,     /* USART6 owns the pin */
    TRS2_DRAIN,    /* want the synth there: waiting for queued MIDI to go out */
    TRS2_AUDIO,    /* TIM8 owns the pin, the interrupt writes it */
    TRS2_FADE,     /* back to MIDI: fading the DC to 0 first */
} trs2_state_t;

/* SRAM, not CCM: apps play the synth too, and app mode gives the app all
 * of the CCM (mem_layout.h). */
static voice_t g_voice; /* voice_init() at boot */
static uint32_t g_queue[QUEUE_LEN]; /* only [tail, head) is read */
static volatile uint32_t g_qhead, g_qtail;
static uint32_t g_dropped;

static trs2_state_t g_state;
static bool g_want;             /* the script wants the synth */
static rt_audio_out_t g_out;    /* where it plays (audio.output()) */
static bool g_irq_on;           /* TIM6's interrupt is running the voice */
static bool g_stopping;         /* !g_want seen while it runs: since g_stop_t0 */
static uint32_t g_stop_t0;
static uint32_t g_fade_t0;
/* What the interrupt does with each sample (main loop writes). */
static volatile bool g_to_usb;  /* push it to the USB ring */
static volatile bool g_to_pwm;  /* write it to TIM8 (TRS 2) */
/* The voice renders (the interrupt may run for an app's stream alone). */
static volatile bool g_voice_run;

/* The app's stream (app mode only): its state lives in the api's buffers
 * (src/apps/api.c, SRAM after .bss in app mode), so it costs the normal
 * mode's stack nothing. NULL until an app starts one. */
static app_stream_t *volatile g_app;
static volatile bool g_app_on;            /* started and not stopped (main loop's view) */
static port_e16_audio_render_t g_app_fn;  /* set while the render interrupt is off */
static void *g_app_user;
static uint32_t g_app_r9;                 /* the app's GOT, for its callback */
static volatile bool g_app_busy;          /* the callback is running, since g_app_t0_ms */
static volatile uint32_t g_app_t0_ms;
static uint32_t g_app_log_t0;
static const char *g_on_what = "synth"; /* what started the interrupt: for the log */
static uint32_t g_app_logged_calls;
/* Every cycle the audio interrupt has measured itself (wrapping), so the
 * render interrupt can leave out the samples that preempted it. */
static volatile uint32_t g_isr_total;

/* Interrupt state: the DC level on the pin (0 = the UART's idle level,
 * 1 = mid-scale), moving towards its target by g_ramp per sample. */
static volatile float g_dc, g_dc_target;
static float g_ramp;

/* CPU load, from the interrupt's own DWT measurement. */
static volatile uint32_t g_cyc_sum, g_cyc_max, g_cyc_n;
static uint32_t g_load_t0;
static uint32_t g_period_cycles; /* CPU cycles per sample */

static uint32_t apb1_timer_clock(void)
{
    /* APB1 timers run at twice PCLK1 when APB1 is divided (it is: /2). */
    return HAL_RCC_GetPCLK1Freq() * 2u;
}

/* --- the interrupt --- */

static void apply_event(uint32_t ev)
{
    uint8_t a = (uint8_t)(ev >> 8), b = (uint8_t)ev;
    switch (ev >> 16) {
        case EV_NOTE_ON:
            voice_note_on(&g_voice, a, b);
            break;
        case EV_NOTE_OFF:
            voice_note_off(&g_voice, a);
            break;
        case EV_ALL_OFF:
            voice_all_notes_off(&g_voice);
            break;
        default:
            break;
    }
}

/* TRS 2: s (within +-1) around a DC level that fades in and out. */
static inline void write_pwm(float s)
{
    float dc = g_dc, target = g_dc_target;
    if (dc < target) {
        dc += g_ramp;
        if (dc > target) {
            dc = target;
        }
        g_dc = dc;
    } else if (dc > target) {
        dc -= g_ramp;
        if (dc < target) {
            dc = target;
        }
        g_dc = dc;
    }
    /* s is within +-1: dc = 1 spans the whole 0-255 range. */
    int duty = (int)(dc * (0.5f * (float)(PWM_PERIOD - 1u)) * (1.0f + s) + 0.5f);
    if (duty < 0) {
        duty = 0;
    } else if (duty > (int)(PWM_PERIOD - 1u)) {
        duty = (int)(PWM_PERIOD - 1u);
    }
    TIM8->CCR1 = (uint32_t)duty;
}

void TIM6_DAC_IRQHandler(void)
{
    uint32_t t0 = DWT->CYCCNT;
    TIM6->SR = 0;

    uint32_t tail = g_qtail, head = g_qhead;
    while (tail != head) {
        apply_event(g_queue[tail & (QUEUE_LEN - 1)]);
        tail++;
    }
    g_qtail = tail;

    float s = 0.0f;
    if (g_voice_run) {
        voice_render(&g_voice, &s, 1);
        s *= OUT_GAIN;
        if (s > 1.5f) {
            s = 1.0f;
        } else if (s < -1.5f) {
            s = -1.0f;
        } else {
            s -= (4.0f / 27.0f) * s * s * s;
        }
    }
    app_stream_t *as = g_app; /* NULL outside app mode: the voice alone, as it always was */
    if (as) {
        bool kick = false;
        float a = app_stream_next(as, &kick);
        if (kick) {
            NVIC->ISPR[(uint32_t)RENDER_IRQn >> 5] = 1u << ((uint32_t)RENDER_IRQn & 31u);
        }
        s = app_stream_mix(s, a);
    }

    if (g_to_usb) {
        port_e16_usbaudio_push((int16_t)(s * 32767.0f));
    }
    if (g_to_pwm) {
        write_pwm(s);
    }

    uint32_t c = DWT->CYCCNT - t0;
    g_isr_total += c + IRQ_OVERHEAD_CYCLES;
    g_cyc_sum += c;
    g_cyc_n++;
    if (c > g_cyc_max) {
        g_cyc_max = c;
    }
}

/* The app's callback with r9 = its GOT (apps are built
 * -msingle-pic-base: their code reaches every global through r9, which
 * in here holds whatever the interrupted code had). r9 is callee-saved,
 * so it's restored on the way out. */
typedef struct {
    port_e16_audio_render_t fn;
    uint32_t r9;
    float *out;
    uint32_t frames;
    void *user;
} app_call_t;

__attribute__((naked)) static void app_call(const app_call_t *c)
{
    (void)c;
    __asm volatile("push {r4, r9, r10, lr}\n"
                   "mov r4, r0\n"
                   "ldr r9, [r4, #4]\n"
                   "ldr r0, [r4, #8]\n"
                   "ldr r1, [r4, #12]\n"
                   "ldr r2, [r4, #16]\n"
                   "ldr r3, [r4, #0]\n"
                   "blx r3\n"
                   "pop {r4, r9, r10, pc}\n");
}
_Static_assert(offsetof(app_call_t, r9) == 4 && offsetof(app_call_t, out) == 8 &&
                   offsetof(app_call_t, frames) == 12 && offsetof(app_call_t, user) == 16,
               "app_call()'s offsets");

/* The render interrupt: one block from the app's callback. Its own DWT
 * measurement leaves out the audio interrupts that preempted it (other
 * drivers' interrupts, short, stay in). */
void CAN2_TX_IRQHandler(void)
{
    app_stream_t *as = g_app;
    float *out = as ? app_stream_render_target(as) : NULL;
    if (!out) {
        return;
    }
    app_call_t call = {g_app_fn, g_app_r9, out, APP_STREAM_BLOCK, g_app_user};
    g_app_t0_ms = HAL_GetTick();
    g_app_busy = true;
    uint32_t isr0 = g_isr_total;
    uint32_t t0 = DWT->CYCCNT;
    app_call(&call);
    uint32_t c = DWT->CYCCNT - t0;
    uint32_t isr = g_isr_total - isr0;
    g_app_busy = false;
    app_stream_rendered(as, c > isr ? c - isr : 0u, c);
}

/* --- notes: queued while the interrupt runs, applied directly otherwise --- */

static void post(uint32_t ev)
{
    if (!g_irq_on) {
        apply_event(ev); /* nothing renders concurrently */
        return;
    }
    uint32_t head = g_qhead;
    if (head - g_qtail >= QUEUE_LEN) {
        g_dropped++;
        return;
    }
    g_queue[head & (QUEUE_LEN - 1)] = ev;
    __DMB(); /* the entry before the index that publishes it */
    g_qhead = head + 1;
}

/* --- TRS 2: MIDI <-> synth --- */

static void pin_af(uint32_t af)
{
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = MIDI_OUT2_PIN;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = af == GPIO_AF3_TIM8 ? GPIO_SPEED_FREQ_HIGH : GPIO_SPEED_FREQ_LOW;
    gpio.Alternate = af;
    HAL_GPIO_Init(MIDI_OUT2_PORT, &gpio);
}

/* The last MIDI byte for TRS 2 has left the shift register: the SDK's
 * USART6 interrupt clears TXEIE when its queue runs dry, and TC follows
 * once the final stop bit is out. */
static bool trs2_drained(void)
{
    return !(MIDI_OUT2_UART->CR1 & USART_CR1_TXEIE) && (MIDI_OUT2_UART->SR & USART_SR_TC);
}

/* TIM8 takes the pin; the interrupt (already running) fades the DC in. */
static void trs2_attach(void)
{
    MIDI_OUT2_UART->CR1 &= ~USART_CR1_UE;

    TIM8->CR1 = TIM_CR1_ARPE;
    TIM8->PSC = 0;
    TIM8->ARR = PWM_PERIOD - 1u;
    TIM8->CCR1 = 0; /* low, like the idle UART line; the fade brings it up */
    TIM8->CCMR1 = TIM_CCMR1_OC1PE | (6u << TIM_CCMR1_OC1M_Pos);
    TIM8->CCER = TIM_CCER_CC1E;
    TIM8->BDTR = TIM_BDTR_MOE;
    TIM8->EGR = TIM_EGR_UG;
    TIM8->CR1 |= TIM_CR1_CEN;
    pin_af(GPIO_AF3_TIM8);

    g_dc = 0.0f;
    g_dc_target = 1.0f;
    __DMB();
    g_to_pwm = true;
}

/* The pin back to the UART (the DC has faded out, or the interrupt is off). */
static void trs2_detach(void)
{
    g_to_pwm = false;
    __DSB(); /* a sample in flight may still write CCR1: harmless, TIM8 stops next */
    pin_af(GPIO_AF8_USART6);
    TIM8->CCER = 0;
    TIM8->CR1 = 0;
    MIDI_OUT2_UART->CR1 |= USART_CR1_UE;
}

static void irq_start(uint32_t now_ms)
{
    g_qhead = g_qtail = 0; /* empty: post() applied everything while it was off */
    g_cyc_sum = g_cyc_max = g_cyc_n = 0;
    g_load_t0 = now_ms;
    g_stopping = false;
    g_voice_run = g_want;
    g_irq_on = true;
    port_e16_usbaudio_producer(true);
    TIM6->CNT = 0;
    TIM6->SR = 0;
    NVIC_ClearPendingIRQ(TIM6_DAC_IRQn);
    NVIC_EnableIRQ(TIM6_DAC_IRQn);
    TIM6->CR1 = TIM_CR1_CEN;
}

static void irq_stop(void)
{
    TIM6->CR1 = 0;
    NVIC_DisableIRQ(TIM6_DAC_IRQn);
    __DSB();
    __ISB();
    g_irq_on = false;
    g_stopping = false;
    g_voice_run = false;
    g_to_usb = false;
    port_e16_usbaudio_producer(false);
    /* Anything still queued: apply it now, in order (the interrupt is off). */
    while (g_qtail != g_qhead) {
        apply_event(g_queue[g_qtail & (QUEUE_LEN - 1)]);
        g_qtail++;
    }
    voice_all_notes_off(&g_voice);
}

static void log_load(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    uint32_t sum = g_cyc_sum, max = g_cyc_max, n = g_cyc_n;
    g_cyc_sum = g_cyc_max = g_cyc_n = 0;
    __set_PRIMASK(primask);
    if (n == 0) {
        log_printf("audio: no samples rendered in the last %u s", (unsigned)(LOAD_LOG_MS / 1000u));
        return;
    }
    uint32_t avg = (uint32_t)((sum + n / 2u) / n);
    /* tenths of a percent of the CPU, entry/exit included */
    uint32_t load = (avg + IRQ_OVERHEAD_CYCLES) * 1000u / g_period_cycles;
    uint32_t peak = (max + IRQ_OVERHEAD_CYCLES) * 1000u / g_period_cycles;
    log_printf("audio: load %lu.%lu%% (avg %lu, max %lu cycles of %lu per sample, +~%u entry/exit), "
               "%lu samples, max %lu.%lu%%, %lu notes dropped",
               (unsigned long)(load / 10u), (unsigned long)(load % 10u), (unsigned long)avg,
               (unsigned long)max, (unsigned long)g_period_cycles, (unsigned)IRQ_OVERHEAD_CYCLES,
               (unsigned long)n, (unsigned long)(peak / 10u), (unsigned long)(peak % 10u),
               (unsigned long)g_dropped);
}

/* Where the synth goes now (audio.output(), "auto" resolved).
 *
 * E16_TRS_AUDIO (CMake option, off by default): TRS 2's jack sits on the
 * E16's internal supply, which carries a loud whine from the OLED's boost
 * converter, so audio on it isn't usable on this
 * hardware. Off, every routing resolves to USB audio and TRS 2 always
 * stays MIDI: with no computer connected (or in webcam mode) the synth
 * and app audio play nowhere. audio.output() still accepts "trs2" and
 * "both" so scripts keep working; they mean USB. */
static rt_audio_out_t routing(void)
{
#if !E16_TRS_AUDIO
    return RT_AUDIO_OUT_USB;
#endif
    if (g_out == RT_AUDIO_OUT_AUTO) {
        /* Not to USB while it has no audio input (webcam mode). */
        return midi_usb_connected() && port_e16_usbaudio_available() ? RT_AUDIO_OUT_USB : RT_AUDIO_OUT_TRS2;
    }
    return g_out;
}

static bool want_trs2(void)
{
    rt_audio_out_t r = routing();
    return (g_want || g_app_on) && (r == RT_AUDIO_OUT_TRS2 || r == RT_AUDIO_OUT_BOTH);
}

/* TRS 2's side: MIDI <-> synth, with the drain and the fade. */
static void trs2_poll(uint32_t now_ms, bool want)
{
    switch (g_state) {
        case TRS2_MIDI:
            if (want) {
                g_state = TRS2_DRAIN;
            } else {
                break;
            }
            /* fall through */
        case TRS2_DRAIN:
            if (!want) {
                g_state = TRS2_MIDI;
            } else if (trs2_drained() && g_irq_on) {
                trs2_attach();
                g_state = TRS2_AUDIO;
                log_printf("audio: TRS 2 -> synth");
            }
            break;
        case TRS2_AUDIO:
            if (!want) {
                g_dc_target = 0.0f;
                g_fade_t0 = now_ms;
                g_state = TRS2_FADE;
            }
            break;
        case TRS2_FADE:
            if (want) {
                g_dc_target = 1.0f;
                g_state = TRS2_AUDIO;
            } else if (g_dc <= 0.0f || (int32_t)(now_ms - g_fade_t0) >= (int32_t)(2u * FADE_MS)) {
                trs2_detach();
                g_state = TRS2_MIDI;
                log_printf("audio: TRS 2 -> MIDI");
            }
            break;
    }
}

/* The app's stream: its load every LOAD_LOG_MS, and the overload stop. */
/* The overload stop's log line (once: port_e16_audio_app_stop() calls
 * this while g_app_on, whether app_poll() or a restart got there first). */
static void log_overload(const app_stream_t *as)
{
    uint32_t pm = as->last_avg_permille;
    switch (as->stop_why) {
        case APP_STREAM_WHY_STARVING:
            log_printf("audio: app callback took all the CPU for a second (own load %lu.%lu%%, %lu late): stopped",
                       (unsigned long)(pm / 10u), (unsigned long)(pm % 10u), (unsigned long)as->last_late);
            break;
        case APP_STREAM_WHY_LATE:
            log_printf("audio: app blocks late (%lu in the last second, own load %lu.%lu%%) for %lu s: stopped",
                       (unsigned long)as->last_late, (unsigned long)(pm / 10u), (unsigned long)(pm % 10u),
                       (unsigned long)APP_STREAM_OVER_WINDOWS);
            break;
        default:
            log_printf("audio: app callback over budget (%lu.%lu%% of the CPU > %lu%%) for %lu s: stopped",
                       (unsigned long)(pm / 10u), (unsigned long)(pm % 10u), (unsigned long)(APP_BUDGET_PERMILLE / 10u),
                       (unsigned long)APP_STREAM_OVER_WINDOWS);
            break;
    }
}

static void app_poll(uint32_t now_ms)
{
    if (g_app_on && g_app->state == APP_STREAM_STOP_OVERLOAD) {
        port_e16_audio_app_stop(); /* logs it */
        return;
    }
    if (!g_app_on || (int32_t)(now_ms - g_app_log_t0) < (int32_t)LOAD_LOG_MS) {
        return;
    }
    g_app_log_t0 = now_ms;
    uint32_t avg = g_app->last_avg_permille, peak = g_app->last_peak_permille, calls = g_app->calls;
    log_printf("audio: app load %lu.%lu%% (last second; slowest call %lu.%lu%% of a block), %lu calls in %lu s, "
               "%lu late since start",
               (unsigned long)(avg / 10u), (unsigned long)(avg % 10u), (unsigned long)(peak / 10u),
               (unsigned long)(peak % 10u), (unsigned long)(calls - g_app_logged_calls),
               (unsigned long)(LOAD_LOG_MS / 1000u), (unsigned long)g_app->late);
    g_app_logged_calls = calls;
}

static const char *routing_text(void)
{
    rt_audio_out_t r = routing();
    return r == RT_AUDIO_OUT_USB ? "USB audio" : r == RT_AUDIO_OUT_TRS2 ? "TRS 2" : "USB audio and TRS 2";
}

/*
 * The interrupt runs while the script wants the synth or an app streams,
 * and a little after (the release tail of the last note, TRS 2's fade).
 * The voice renders while it's wanted and for its tail. Then TRS 2
 * follows the routing, and the USB ring gets the samples while USB is
 * routed.
 *
 * now_ms may be a little behind g_load_t0/g_fade_t0/g_stop_t0: the main
 * loop takes its time at the top of a pass, and a script can switch the
 * synth (calling this with a fresher system_millis()) later in the same
 * pass. Hence the signed differences.
 */
void port_e16_audio_poll(uint32_t now_ms)
{
    app_poll(now_ms);
    bool app = g_app_on;
    if ((g_want || app) && !g_irq_on) {
        irq_start(now_ms);
        g_on_what = g_want ? "synth" : "app stream";
        log_printf("audio: %s on (%s)", g_on_what, routing_text());
    }
    if (g_want && g_irq_on && !g_voice_run) {
        g_voice_run = true;
    }
    rt_audio_out_t r = routing();
    /* Through the stop too, so USB gets the last note's release tail. */
    g_to_usb = g_irq_on && (r == RT_AUDIO_OUT_USB || r == RT_AUDIO_OUT_BOTH) && port_e16_usbaudio_available();
    trs2_poll(now_ms, want_trs2());

    if (!g_irq_on) {
        return;
    }
    if (g_want) {
        g_stopping = false;
    } else if (g_voice_run) {
        if (!g_stopping) {
            g_stopping = true;
            g_stop_t0 = now_ms;
        } else if (!voice_active(&g_voice) || (int32_t)(now_ms - g_stop_t0) >= (int32_t)STOP_AFTER_MS) {
            g_voice_run = false; /* the interrupt leaves the voice alone from its next sample */
            g_stopping = false;
            post(EV(EV_ALL_OFF, 0, 0));
        }
    }
    if (!g_want && !g_voice_run && !app && g_state == TRS2_MIDI) {
        log_load();
        irq_stop();
        log_printf("audio: %s off", g_on_what);
        return;
    }
    if ((int32_t)(now_ms - g_load_t0) >= (int32_t)LOAD_LOG_MS) {
        g_load_t0 = now_ms;
        log_load();
    }
}

bool port_e16_audio_trs2_midi(void)
{
    return g_state == TRS2_MIDI && !want_trs2();
}

bool port_e16_audio_on_trs2(void)
{
    return g_state == TRS2_AUDIO || g_state == TRS2_FADE;
}

bool port_e16_audio_running(void)
{
    return g_irq_on;
}

uint32_t port_e16_audio_pwm_value(void)
{
    return TIM8->CCR1;
}

void port_e16_audio_init(void)
{
    voice_init(&g_voice, (float)SAMPLE_RATE);

    /* DWT cycle counter: the interrupt's load measurement (and main.c's
     * loop timing). */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    g_period_cycles = SystemCoreClock / SAMPLE_RATE;
    NVIC_DisableIRQ(RENDER_IRQn);
    HAL_NVIC_SetPriority(RENDER_IRQn, RENDER_IRQ_PRIORITY, 0);

    __HAL_RCC_TIM8_CLK_ENABLE();
    __HAL_RCC_TIM6_CLK_ENABLE();
    TIM6->CR1 = 0;
    TIM6->PSC = 0;
    TIM6->ARR = apb1_timer_clock() / SAMPLE_RATE - 1u;
    TIM6->DIER = TIM_DIER_UIE;
    TIM6->EGR = TIM_EGR_UG;
    TIM6->SR = 0;
    HAL_NVIC_SetPriority(TIM6_DAC_IRQn, AUDIO_IRQ_PRIORITY, 0);

    g_ramp = 1.0f / ((float)FADE_MS * (float)SAMPLE_RATE / 1000.0f);
    g_state = TRS2_MIDI;
    log_printf("audio: synth ready, %lu Hz (timer %lu Hz / %lu)", (unsigned long)SAMPLE_RATE,
               (unsigned long)apb1_timer_clock(), (unsigned long)(TIM6->ARR + 1u));
}

/* --- rt_port.h --- */

static const voice_param_id_t PARAM_MAP[RT_AUDIO_PARAMS] = {
    [RT_AUDIO_CUTOFF] = VOICE_P_CUTOFF,     [RT_AUDIO_RESONANCE] = VOICE_P_RESONANCE,
    [RT_AUDIO_ENVMOD] = VOICE_P_ENV_MOD,    [RT_AUDIO_DECAY] = VOICE_P_DECAY,
    [RT_AUDIO_WAVEFORM] = VOICE_P_WAVEFORM, [RT_AUDIO_VOLUME] = VOICE_P_VOLUME,
    [RT_AUDIO_GLIDE] = VOICE_P_GLIDE_TIME,  [RT_AUDIO_SLIDE] = VOICE_P_SLIDE,
};

void rt_port_audio_enable(bool on)
{
    g_want = on;
    if (!on) {
        post(EV(EV_ALL_OFF, 0, 0));
    }
    port_e16_audio_poll(system_millis());
}

void rt_port_audio_note(int note, int velocity)
{
    g_want = true;
    if (velocity > 0) {
        post(EV(EV_NOTE_ON, note & 0x7F, velocity & 0x7F));
    } else {
        post(EV(EV_NOTE_OFF, note & 0x7F, 0));
    }
    port_e16_audio_poll(system_millis());
}

void rt_port_audio_off(void)
{
    post(EV(EV_ALL_OFF, 0, 0));
}

void rt_port_audio_set(rt_audio_param_t param, int value)
{
    if ((unsigned)param < RT_AUDIO_PARAMS) {
        voice_param(&g_voice, PARAM_MAP[param], (uint8_t)value);
    }
}

void rt_port_audio_output(rt_audio_out_t out)
{
#if !E16_TRS_AUDIO
    if (out == RT_AUDIO_OUT_TRS2 || out == RT_AUDIO_OUT_BOTH) {
        log_printf("audio: TRS 2 audio is off in this build (E16_TRS_AUDIO), using USB");
    }
#endif
    if ((unsigned)out <= RT_AUDIO_OUT_BOTH && out != g_out) {
        g_out = out;
        port_e16_audio_poll(system_millis());
    }
}

rt_audio_out_t rt_port_audio_routing(void)
{
    return routing();
}

void rt_port_audio_release(void)
{
    if (g_want || g_irq_on || g_state != TRS2_MIDI) {
        rt_port_audio_enable(false);
        uint32_t t0 = system_millis();
        while ((g_irq_on || g_state != TRS2_MIDI) && system_millis() - t0 < RELEASE_WAIT_MS) {
            port_e16_audio_poll(system_millis());
        }
        if (g_irq_on) {
            irq_stop();
        }
        if (g_state != TRS2_MIDI) {
            trs2_detach();
            g_state = TRS2_MIDI;
            log_printf("audio: TRS 2 -> MIDI (forced)");
        }
    }
    /* The interrupt is off: back to the default sound and routing for the
     * next script. */
    voice_init(&g_voice, (float)SAMPLE_RATE);
    g_out = RT_AUDIO_OUT_AUTO;
    g_dropped = 0;
}

void port_e16_audio_voice_param(int id, uint8_t value)
{
    if (id >= 0 && id < VOICE_P_COUNT) {
        voice_param(&g_voice, (voice_param_id_t)id, value);
    }
}

void port_e16_audio_midi(const uint8_t *bytes, size_t len)
{
    if (len < 3) {
        return;
    }
    uint8_t st = bytes[0] & 0xF0, d1 = bytes[1] & 0x7F, d2 = bytes[2] & 0x7F;
    switch (st) {
        case 0x90:
            rt_port_audio_note(d1, d2);
            return;
        case 0x80:
            rt_port_audio_note(d1, 0);
            return;
        case 0xB0:
            break;
        default:
            g_want = true;
            return;
    }
    g_want = true;
    switch (d1) {
        case 74: rt_port_audio_set(RT_AUDIO_CUTOFF, d2); break;
        case 71: rt_port_audio_set(RT_AUDIO_RESONANCE, d2); break;
        case 75: rt_port_audio_set(RT_AUDIO_ENVMOD, d2); break;
        case 72: rt_port_audio_set(RT_AUDIO_DECAY, d2); break;
        case 70: rt_port_audio_set(RT_AUDIO_WAVEFORM, d2); break;
        case 7: rt_port_audio_set(RT_AUDIO_VOLUME, d2); break;
        case 65: rt_port_audio_set(RT_AUDIO_SLIDE, d2); break;
        case 5: rt_port_audio_set(RT_AUDIO_GLIDE, d2); break;
        case 120:
        case 123: rt_port_audio_off(); break;
        default: break;
    }
    port_e16_audio_poll(system_millis());
}

/* --- a native app's own stream (app mode) --- */

void port_e16_audio_app_start(app_stream_t *stream, port_e16_audio_render_t fn, void *user, uint32_t r9)
{
    port_e16_audio_app_stop();
    if (g_app != stream) {
        /* only ever one stream (the api's), set before the first start */
        app_stream_init(stream, g_period_cycles * APP_STREAM_BLOCK, APP_BUDGET_PERMILLE);
        g_app = stream;
    }
    g_app_fn = fn;
    g_app_user = user;
    g_app_r9 = r9;
    g_app_busy = false;
    app_stream_start(stream); /* the audio interrupt picks it up from its next sample */
    g_app_logged_calls = 0;
    g_app_log_t0 = system_millis();
    g_app_on = true;
    NVIC_ClearPendingIRQ(RENDER_IRQn);
    NVIC_EnableIRQ(RENDER_IRQn);
    NVIC_SetPendingIRQ(RENDER_IRQn); /* the first block, while the silent one plays */
    log_printf("audio: app stream started (%lu Hz, %lu-frame blocks, budget %lu%%)", (unsigned long)SAMPLE_RATE,
               (unsigned long)APP_STREAM_BLOCK, (unsigned long)(APP_BUDGET_PERMILLE / 10u));
    port_e16_audio_poll(system_millis());
}

void port_e16_audio_app_stop(void)
{
    /* From the main flow or PendSV: the render interrupt has a higher
     * priority, so it isn't half-way through a call now, and once
     * disabled it doesn't start another. */
    NVIC_DisableIRQ(RENDER_IRQn);
    __DSB();
    __ISB();
    if (!g_app) {
        return;
    }
    if (g_app->state == APP_STREAM_RUNNING) {
        app_stream_stop(g_app);
    }
    if (g_app_on) {
        g_app_on = false;
        if (g_app->state == APP_STREAM_STOP_OVERLOAD) {
            log_overload(g_app); /* even when a restart gets here before app_poll() */
        }
        log_printf("audio: app stream stopped after %lu calls, %lu late", (unsigned long)g_app->calls,
                   (unsigned long)g_app->late);
    }
}

void port_e16_audio_app_info(port_e16_audio_app_info_t *info)
{
    const app_stream_t *as = g_app;
    info->rate = SAMPLE_RATE;
    info->block = APP_STREAM_BLOCK;
    if (!as) {
        info->state = APP_STREAM_OFF;
        info->load_permille = info->peak_permille = info->late = info->calls = 0;
        return;
    }
    /* The render interrupt finishes a window's figures (avg, peak, then
     * windows++) all at once whenever it preempts this: if windows moved
     * while these were read, read them again, so avg and peak always come
     * from the same window. */
    uint32_t w;
    do {
        w = as->windows;
        info->load_permille = as->last_avg_permille;
        info->peak_permille = as->last_peak_permille;
    } while (w != as->windows);
    info->state = as->state;
    info->late = as->late;
    info->calls = as->calls;
}

bool port_e16_audio_app_stuck(uint32_t now_ms)
{
    return g_app_busy && now_ms - g_app_t0_ms >= APP_STUCK_MS;
}
