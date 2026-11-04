/* The leave prompt (src/ui/exit_prompt.h). */
#include <stdio.h>
#include <string.h>
#include "exit_prompt.h"

static int g_fail, g_checks;
#define CHECK(cond, ...)                                          \
    do {                                                           \
        g_checks++;                                                \
        if (!(cond)) {                                             \
            g_fail++;                                              \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);   \
            fprintf(stderr, __VA_ARGS__);                          \
            fprintf(stderr, "\n");                                 \
        }                                                          \
    } while (0)

static int n_show, n_hide, n_leave;

/* ms milliseconds of the same input, 1 ms steps. */
static void run(exit_prompt_t *p, uint32_t ms, bool shift, uint32_t pressed, bool turned)
{
    for (uint32_t i = 0; i < ms; i++) {
        switch (exit_prompt_step(p, 1, shift, pressed, turned)) {
            case EXIT_PROMPT_SHOW: n_show++; break;
            case EXIT_PROMPT_HIDE: n_hide++; break;
            case EXIT_PROMPT_LEAVE: n_leave++; break;
            default: break;
        }
    }
}

static void fresh(exit_prompt_t *p)
{
    exit_prompt_reset(p);
    n_show = n_hide = n_leave = 0;
}

int main(void)
{
    exit_prompt_t p;

    /* Shift alone 5 s: the prompt; encoder 1: leave */
    fresh(&p);
    run(&p, EXIT_PROMPT_HOLD_MS, true, 0, false);
    CHECK(n_show == 0 && !exit_prompt_active(&p), "not before 5 s");
    run(&p, 1, true, 0, false);
    CHECK(n_show == 1 && exit_prompt_active(&p), "prompt at 5 s");
    run(&p, 300, true, 0, false);
    run(&p, 1, true, 0x0001u, false);
    CHECK(n_leave == 1 && p.state == EXIT_PROMPT_CONFIRMED, "encoder 1 confirms");
    run(&p, 5000, false, 0, false);
    CHECK(n_leave == 1 && n_hide == 0 && exit_prompt_active(&p), "confirmed stays confirmed");

    /* any push or turn during the hold: no prompt, however long */
    fresh(&p);
    run(&p, 1000, true, 0, false);
    run(&p, 1, true, 0x0100u, false);
    run(&p, 30000, true, 0, false);
    CHECK(n_show == 0, "a push during the hold cancels it");
    fresh(&p);
    run(&p, 1000, true, 0, false);
    run(&p, 1, true, 0, true);
    run(&p, 30000, true, 0, false);
    CHECK(n_show == 0, "a turn during the hold cancels it");
    /* an encoder already down when Shift goes down */
    fresh(&p);
    run(&p, 1, false, 0x8000u, false);
    run(&p, 30000, true, 0x8000u, false);
    CHECK(n_show == 0, "Shift + an encoder already down: no prompt");
    /* the next clean hold works again */
    run(&p, 1, false, 0, false);
    run(&p, EXIT_PROMPT_HOLD_MS + 1, true, 0, false);
    CHECK(n_show == 1, "a new clean hold prompts");

    /* let go of Shift: cancelled */
    fresh(&p);
    run(&p, EXIT_PROMPT_HOLD_MS + 1, true, 0, false);
    run(&p, 1, false, 0, false);
    CHECK(n_hide == 1 && !exit_prompt_active(&p) && n_leave == 0, "Shift up cancels");
    /* encoder 1 after that does nothing */
    run(&p, 1, false, 0x0001u, false);
    CHECK(n_leave == 0, "encoder 1 with no prompt: nothing");

    /* 4 s with no answer: hidden, and the same hold doesn't prompt again */
    fresh(&p);
    run(&p, EXIT_PROMPT_HOLD_MS + 1, true, 0, false);
    run(&p, EXIT_PROMPT_SHOW_MS, true, 0, false);
    CHECK(n_hide == 1 && !exit_prompt_active(&p), "timed out");
    run(&p, 30000, true, 0, false);
    CHECK(n_show == 1, "no second prompt in the same hold");

    /* encoder 1 held from before the prompt: needs a fresh push */
    fresh(&p);
    run(&p, EXIT_PROMPT_HOLD_MS + 1, true, 0, false);
    CHECK(n_show == 1, "prompt");
    p.enc1_was = true; /* as if it was down already */
    run(&p, 10, true, 0x0001u, false);
    CHECK(n_leave == 0, "a held encoder 1 isn't a push");
    run(&p, 1, true, 0, false);
    run(&p, 1, true, 0x0001u, false);
    CHECK(n_leave == 1, "a fresh push is");

    /* other encoders during the prompt: ignored */
    fresh(&p);
    run(&p, EXIT_PROMPT_HOLD_MS + 1, true, 0, false);
    run(&p, 100, true, 0xFFFEu, true);
    CHECK(exit_prompt_active(&p) && n_leave == 0 && n_hide == 0, "others ignored");

    /* drawing stays on the screen and leaves the corners */
    uint8_t fb[1024];
    memset(fb, 0xFF, sizeof fb);
    exit_prompt_draw(fb, "swell", false);
    CHECK(fb[0] == 0xFF && fb[1023] == 0xFF, "corners untouched");
    memset(fb, 0, sizeof fb);
    exit_prompt_draw(fb, "a-very-long-name-that-goes-on-and-on", true);
    exit_prompt_draw_release(fb);
    int lit = 0;
    for (int i = 0; i < 1024; i++) {
        for (int b = 0; b < 8; b++) {
            lit += fb[i] >> b & 1;
        }
    }
    CHECK(lit > 100 && lit < 2000, "release screen: some text on black (%d lit)", lit);

    printf("%d checks, %d failures\n", g_checks, g_fail);
    printf(g_fail ? "FAILED\n" : "ALL PASSED\n");
    return g_fail ? 1 : 0;
}
