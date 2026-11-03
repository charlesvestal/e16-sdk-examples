#ifndef LOG_H
#define LOG_H
#include <stddef.h>
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_write(const char *s, size_t len);   /* raw text; '\n' ends a line */
size_t log_take(char *out, size_t max);      /* next complete line without '\n', 0 if none */
void log_replay(void);                       /* re-queue the retained history for sending */
size_t log_unsent(void);                     /* bytes not yet taken by log_take() */
#endif
