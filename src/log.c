#include <stdarg.h>
#include <stdio.h>
#include "log.h"

#define LOG_SIZE 2048
/* A single line (between two '\n's, or from the start/last truncation) is
 * capped at LOG_SIZE - 64 bytes of original content so one unterminated,
 * oversized write can't occupy (and eventually fall out of) the whole ring
 * without ever becoming a complete, sendable line. Past the cap the line is
 * ended with "...\n" and the rest of the original write is discarded up to
 * its own terminating '\n' (if any), so it doesn't produce a second,
 * spurious truncated line. */
#define LINE_MAX (LOG_SIZE - 64)

/* SRAM, not CCM: app mode logs too, and gives the app all of the CCM
 * (mem_layout.h). */
static char buf[LOG_SIZE];

/*
 * head, oldest and send are monotonically increasing byte offsets (wrapped
 * into buf with % LOG_SIZE only when indexing it), so oldest <= send <= head
 * always holds without any modular-arithmetic ambiguity.
 *
 *   head:   next byte to write.
 *   oldest: oldest byte still retained -- the drop boundary. Only advanced
 *           by log_write() when it needs the room; never by sending, so the
 *           last LOG_SIZE bytes of history survive being sent.
 *   send:   next byte to hand out via log_take(). log_replay() rewinds it
 *           to 'oldest' to redeliver the whole retained history.
 */
static size_t head, oldest, send;

/* Bytes stored for the current line so far, carried across log_write()
 * calls in case a line is ever built up over more than one call. */
static size_t cur_line_len;

static void ring_putc(char c)
{
    if (head - oldest == LOG_SIZE) {            /* full: drop the oldest line */
        size_t new_oldest = oldest;
        while (new_oldest != head && buf[new_oldest % LOG_SIZE] != '\n') {
            new_oldest++;
        }
        if (new_oldest != head) {
            new_oldest++;                       /* skip the '\n' */
        }
        if (send - oldest <= new_oldest - oldest) {
            send = new_oldest;                  /* can't (re)send what we just dropped */
        }
        oldest = new_oldest;
    }
    buf[head % LOG_SIZE] = c;
    head++;
}

void log_write(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c != '\n' && cur_line_len >= LINE_MAX) {
            /* Over the cap: end the line here and drop the rest of this
             * call's data (in practice always the same original write,
             * since every caller hands the whole line to one log_write()
             * call) instead of letting it sit forever as an incomplete,
             * unsendable line that only falls out of the ring unseen. */
            ring_putc('.');
            ring_putc('.');
            ring_putc('.');
            ring_putc('\n');
            cur_line_len = 0;
            return;
        }
        ring_putc(c);
        if (c == '\n') {
            cur_line_len = 0;
        } else {
            cur_line_len++;
        }
    }
}

void log_printf(const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if (n > (int)sizeof(line) - 2) {
        n = sizeof(line) - 2;
    }
    line[n++] = '\n';
    log_write(line, (size_t)n);
}

size_t log_take(char *out, size_t max)
{
    size_t i = send, n = 0;
    while (i != head && buf[i % LOG_SIZE] != '\n') {
        i++;
    }
    if (i == head) {
        return 0;                           /* no complete line */
    }
    while (send != i) {
        if (n < max) {
            out[n++] = buf[send % LOG_SIZE] & 0x7F;
        }
        send++;
    }
    send++;                                 /* skip '\n' */
    return n;
}

void log_replay(void)
{
    send = oldest;
}

size_t log_unsent(void)
{
    return head - send;
}
