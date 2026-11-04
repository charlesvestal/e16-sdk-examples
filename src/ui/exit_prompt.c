#include "exit_prompt.h"
#include "font.h"

int exit_prompt_step(exit_prompt_t *p, uint32_t dt, bool shift, uint32_t pressed, bool turned)
{
    bool enc1 = (pressed & 1u) != 0;
    bool enc1_edge = enc1 && !p->enc1_was;
    p->enc1_was = enc1;
    p->t += dt;

    switch (p->state) {
        case EXIT_PROMPT_IDLE:
            if (shift) {
                /* a fresh hold: it counts only if nothing else is down yet */
                p->state = EXIT_PROMPT_ARMING;
                p->t = 0;
                p->touched = pressed != 0 || turned;
            }
            return EXIT_PROMPT_NONE;
        case EXIT_PROMPT_ARMING:
            if (!shift) {
                p->state = EXIT_PROMPT_IDLE;
                return EXIT_PROMPT_NONE;
            }
            if (pressed != 0 || turned) {
                p->touched = true;
            }
            if (!p->touched && p->t >= EXIT_PROMPT_HOLD_MS) {
                p->state = EXIT_PROMPT_SHOWN;
                p->t = 0;
                return EXIT_PROMPT_SHOW;
            }
            return EXIT_PROMPT_NONE;
        case EXIT_PROMPT_SHOWN:
            if (enc1_edge) {
                p->state = EXIT_PROMPT_CONFIRMED;
                return EXIT_PROMPT_LEAVE;
            }
            if (!shift) {
                p->state = EXIT_PROMPT_IDLE;
                return EXIT_PROMPT_HIDE;
            }
            if (p->t >= EXIT_PROMPT_SHOW_MS) {
                /* timed out: back to normal, and this hold doesn't re-arm */
                p->state = EXIT_PROMPT_ARMING;
                p->touched = true;
                return EXIT_PROMPT_HIDE;
            }
            return EXIT_PROMPT_NONE;
        default:
            return EXIT_PROMPT_NONE;
    }
}

bool exit_prompt_active(const exit_prompt_t *p)
{
    return p->state == EXIT_PROMPT_SHOWN || p->state == EXIT_PROMPT_CONFIRMED;
}

void exit_prompt_reset(exit_prompt_t *p)
{
    p->state = EXIT_PROMPT_IDLE;
    p->touched = false;
    p->enc1_was = false;
    p->t = 0;
}

/* ---- drawing ---- */

#define W 128
#define H 64

static void px(uint8_t *fb, int x, int y, bool on)
{
    if (x < 0 || x >= W || y < 0 || y >= H) {
        return;
    }
    uint8_t m = (uint8_t)(1u << (y & 7));
    if (on) {
        fb[(y >> 3) * W + x] |= m;
    } else {
        fb[(y >> 3) * W + x] &= (uint8_t)~m;
    }
}

static int text(uint8_t *fb, int x, int y, const char *s, int n, bool on)
{
    for (; *s && n > 0; s++, n--) {
        const uint8_t *cols;
        int w = font_glyph((uint8_t)*s, &cols);
        for (int i = 0; i < w; i++) {
            for (int r = 0; r < FONT_HEIGHT; r++) {
                if (cols[i] >> r & 1) {
                    px(fb, x + i, y + r, on);
                }
            }
        }
        if (w) {
            x += w + FONT_GAP;
        }
    }
    return x;
}

static void centred(uint8_t *fb, int y, const char *s, bool on)
{
    int w = font_text_width(s, 64);
    text(fb, (W - w) / 2, y, s, 64, on);
}

void exit_prompt_draw(uint8_t *fb, const char *name, bool confirmed)
{
    /* the whole screen: a lit title bar, then the two answers */
    for (int i = 0; i < W * H / 8; i++) {
        fb[i] = 0;
    }
    for (int y = 0; y <= 12; y++) {
        for (int x = 0; x < W; x++) {
            px(fb, x, y, true);
        }
    }
    char t[40];
    int n = 0;
    const char *lead = confirmed ? "Leaving " : "Leave ";
    while (*lead && n < 30) {
        t[n++] = *lead++;
    }
    /* the name, cut to what fits in the bar */
    for (const char *s = name ? name : "this"; *s && n < 30; s++) {
        t[n++] = *s;
        t[n] = 0;
        if (font_text_width(t, 64) > W - 12) {
            n--;
            break;
        }
    }
    if (!confirmed) {
        t[n++] = '?';
    }
    t[n] = 0;
    centred(fb, 3, t, false);
    if (confirmed) {
        centred(fb, 30, EXIT_PROMPT_SAVING, true);
        return;
    }
    centred(fb, 24, EXIT_PROMPT_LINE1, true);
    centred(fb, 40, EXIT_PROMPT_LINE2, true);
}

void exit_prompt_draw_release(uint8_t *fb)
{
    for (int i = 0; i < W * H / 8; i++) {
        fb[i] = 0;
    }
    centred(fb, 22, "Release the buttons", true);
    centred(fb, 34, "to leave", true);
}
