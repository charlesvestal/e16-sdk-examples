/*
 * A test app for file_write(): logs what the last run left in
 * /apps/filewrite/state.txt, then writes the file twice, 2 s apart (the
 * second time while the first write's erases are still fresh in the
 * flash budget), and exits. Run it again: the first line it logs must be
 * the second write of the run before, intact.
 *
 *   app filewrite: previous: run 3 second
 *   app filewrite: write 1 -> 0
 *   app filewrite: write 2 -> 0
 *
 * A write may also come back E16_EBUSY (-4: the flash budget said "not
 * now", nothing changed); the file must then still hold what was there.
 */
#include "e16_app.h"

#define PATH "/apps/filewrite/state.txt"

static char *put_str(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    return p;
}

static char *put_int(char *p, int v)
{
    char tmp[12];
    int n = 0;
    unsigned u = v < 0 ? (unsigned)-v : (unsigned)v;
    if (v < 0)
        *p++ = '-';
    do {
        tmp[n++] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    while (n)
        *p++ = tmp[--n];
    return p;
}

static char state[64];
static char line[128];

/* The run number in "run N ...", or 0. */
static int run_of(const char *s)
{
    int n = 0;
    if (s[0] != 'r' || s[1] != 'u' || s[2] != 'n' || s[3] != ' ')
        return 0;
    for (s += 4; *s >= '0' && *s <= '9'; s++)
        n = n * 10 + (*s - '0');
    return n;
}

static int write_state(const e16_api_t *api, int run, const char *which)
{
    char *p = put_str(state, "run ");
    p = put_int(p, run);
    p = put_str(p, " ");
    p = put_str(p, which);
    /* Pad to a fixed length: the second write is then a same-length
     * rewrite, the cheap case. */
    while (p < state + 32)
        *p++ = '.';
    *p = '\0';
    return api->file_write("state.txt", state, (uint32_t)(p - state));
}

int app_main(const e16_api_t *api)
{
    int prev = 0;
    int fd = api->file_open(PATH, E16_FILE_READ);
    char *p = put_str(line, "previous: ");
    if (fd >= 0) {
        int n = api->file_read(fd, state, sizeof state - 1);
        api->file_close(fd);
        if (n < 0)
            n = 0;
        state[n] = '\0';
        prev = run_of(state);
        p = put_str(p, n ? state : "(empty file!)");
        p = put_str(p, " (");
        p = put_int(p, n);
        p = put_str(p, " bytes)");
    } else {
        p = put_str(p, "none");
    }
    *p = '\0';
    api->log(line);

    int r = write_state(api, prev + 1, "first");
    p = put_str(line, "write 1 -> ");
    p = put_int(p, r);
    *p = '\0';
    api->log(line);

    api->delay_ms(2000);

    r = write_state(api, prev + 1, "second");
    p = put_str(line, "write 2 -> ");
    p = put_int(p, r);
    *p = '\0';
    api->log(line);
    api->delay_ms(50);
    return 0;
}
