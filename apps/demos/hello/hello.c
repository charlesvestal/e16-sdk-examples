/*
 * The smallest useful app: exercises every kind of address the loader has
 * to get right, then logs a line and exits.
 *
 *   counts[]    a global array in .bss       (GOT entry -> data)
 *   seed        an initialized global        (GOT entry -> data)
 *   squares[]   a const table in .rodata     (GOT entry -> text)
 *   words[]     const pointers to strings    (fixups -> text)
 *   ops[]       initialized function pointers (fixups -> text, Thumb bit)
 *   pick        a function pointer taken at run time (GOT entry -> text)
 */
#include "e16_app.h"

static int counts[8];
int seed = 7;
static const unsigned char squares[8] = { 0, 1, 4, 9, 16, 25, 36, 49 };
static const char *const words[] = { "zero", "one", "two", "three" };

static int add(int a, int b) { return a + b; }
static int mul(int a, int b) { return a * b; }
static int (*const ops[])(int, int) = { add, mul };

/* Write a small non-negative number in decimal; returns the end. */
static char *put_uint(char *p, unsigned v)
{
    char tmp[10];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        *p++ = tmp[--n];
    return p;
}

static char *put_str(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    return p;
}

int app_main(const e16_api_t *api)
{
    int (*pick)(int, int) = (api->millis() & 1) ? add : mul;

    int total = 0;
    for (int i = 0; i < 8; i++) {
        counts[i] = ops[i & 1](squares[i], seed) + pick(i, 1);
        total += counts[i];
    }

    char line[64];
    char *p = put_str(line, "hello from an app: ");
    p = put_str(p, words[total & 3]);
    p = put_str(p, " total=");
    p = put_uint(p, (unsigned)total);
    p = put_str(p, " c5=");
    p = put_uint(p, (unsigned)counts[5]);
    *p = '\0';
    api->log(line);

    api->exit(0);
}
