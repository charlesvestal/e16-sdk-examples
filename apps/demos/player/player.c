/*
 * Player: plays .e16v animations (tools/gif2e16.py) on the OLED, with each
 * frame's colours on the 16 LED rings.
 *
 * Files: the app API can't list a folder, so the player reads its
 * playlist, /apps/player/playlist.txt: one file name per line (relative
 * to /apps/player/, or an absolute path), blank lines and lines starting
 * with '#' skipped. `tools/gif2e16.py --playlist /Volumes/E16/apps/player`
 * writes one listing every .e16v file in that folder. It plays the first,
 * looping, until another is picked.
 *
 *   encoder 1   next / previous file (turn)
 *   encoder 2   speed, 25% - 400% (turn)
 *   encoder 3   LED brightness, 0 (off) - 8 (turn)
 *   encoder 4   pause / resume (press)
 *   Shift held 1 s: back to the launcher (the launcher's own watch)
 *
 * A change shows a one-line note at the bottom of the screen for about a
 * second. Every 5 s it logs "player: <file> N frames, X fps" (frames
 * actually shown per second over those 5 s).
 *
 * Frames are streamed: one 1796-byte record (delay, framebuffer, LED
 * colours) is read per frame, sequentially, from the open file; the file
 * format is described in tools/gif2e16.py.
 */
#include "e16_app.h"
#include "font.h"

#define W E16_DISPLAY_WIDTH
#define H E16_DISPLAY_HEIGHT
#define FOLDER "/apps/player/"
#define PLAYLIST FOLDER "playlist.txt"
#define LOG_EVERY_MS 5000u
#define NOTE_MS 1200u
#define MAX_FILES 32
#define NAME_MAX 64

/* .e16v (tools/gif2e16.py) */
#define HDR_SIZE 32u
#define LEDS_SIZE (E16_LED_RINGS * E16_LEDS_PER_RING * 3)
#define REC_SIZE (4u + E16_FB_SIZE + LEDS_SIZE)
#define FLAG_LEDS 1u
#define MIN_DELAY 20u

static char names[MAX_FILES][NAME_MAX]; /* full paths */
static int n_files;
static unsigned char playlist_buf[2048];

static unsigned char rec[REC_SIZE]; /* the frame on screen */
static unsigned char hdr[HDR_SIZE];

static int cur = -1, fd = -1;
static unsigned n_frames, frame, rec_size, flags, data_start;
static int have_frame;

static const unsigned short speeds[] = {25, 33, 50, 67, 75, 100, 125, 150, 200, 300, 400};
#define N_SPEEDS ((int)(sizeof speeds / sizeof speeds[0]))
static int speed_i = 5; /* 100% */
static int bright = 3;  /* 0..8 */
static int paused;

static char note[40];
static unsigned note_until;

/* ---- small helpers (no libc) ---- */

static void copy(void *dst, const void *src, unsigned n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--)
        *d++ = *s++;
}

static unsigned rd16(const unsigned char *p) { return p[0] | (unsigned)p[1] << 8; }
static unsigned rd32(const unsigned char *p) { return rd16(p) | rd16(p + 2) << 16; }

static char *put_str(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    return p;
}

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

/* The file name part of a path. */
static const char *base(const char *path)
{
    const char *b = path;
    for (; *path; path++)
        if (*path == '/')
            b = path + 1;
    return b;
}

/* ---- drawing ---- */

static void pixel(unsigned char *fb, int x, int y, int on)
{
    if (x < 0 || x >= W || y < 0 || y >= H)
        return;
    unsigned char m = (unsigned char)(1u << (y & 7));
    if (on)
        fb[(y >> 3) * W + x] |= m;
    else
        fb[(y >> 3) * W + x] &= (unsigned char)~m;
}

static int text_width(const char *s) { return font_text_width(s, 64); }

/* Text at (x, y) (top of the 8-pixel cell); returns the x after it. */
static int text(unsigned char *fb, int x, int y, const char *s)
{
    for (; *s; s++) {
        const unsigned char *cols;
        int gw = font_glyph((unsigned char)*s, &cols);
        for (int c = 0; c < gw; c++)
            for (int r = 0; r < FONT_HEIGHT; r++)
                if (cols[c] >> r & 1)
                    pixel(fb, x + c, y + r, 1);
        if (gw > 0)
            x += gw + FONT_GAP;
    }
    return x;
}

static void centered(unsigned char *fb, int y, const char *s)
{
    text(fb, (W - text_width(s)) / 2, y, s);
}

/* The note in a dark box with a lit outline along the bottom edge. */
static void draw_note(unsigned char *fb)
{
    int w = text_width(note) + 6;
    int x0 = (W - w) / 2, y0 = H - 11;
    for (int y = y0; y < H; y++)
        for (int x = x0; x < x0 + w; x++)
            pixel(fb, x, y, y == y0 || x == x0 || x == x0 + w - 1);
    text(fb, x0 + 3, y0 + 2, note);
}

static void show(const e16_api_t *api, unsigned now)
{
    unsigned char *fb = api->display_fb();
    if (have_frame)
        copy(fb, rec + 4, E16_FB_SIZE);
    if (note[0] && (int)(note_until - now) > 0)
        draw_note(fb);
    else
        note[0] = '\0';
    api->display_present();
}

static void set_note(const char *s, unsigned now)
{
    char *p = note;
    for (int i = 0; s[i] && i < (int)sizeof note - 1; i++)
        *p++ = s[i];
    *p = '\0';
    note_until = now + NOTE_MS;
}

/* ---- LEDs ---- */

static int leds_dirty;

static void leds_frame(const e16_api_t *api)
{
    /* 0..255 per channel -> 0..bright*8 (the api caps at 85 anyway) */
    unsigned level = (unsigned)bright * 8u;
    int on = have_frame && (flags & FLAG_LEDS) && level;
    const unsigned char *c = rec + 4 + E16_FB_SIZE;
    for (int ring = 0; ring < E16_LED_RINGS; ring++)
        for (int led = 0; led < E16_LEDS_PER_RING; led++, c += 3) {
            if (on)
                api->leds_set(ring, led, (unsigned char)(c[0] * level / 255u),
                              (unsigned char)(c[1] * level / 255u), (unsigned char)(c[2] * level / 255u));
            else
                api->leds_set(ring, led, 0, 0, 0);
        }
    leds_dirty = 1;
}

/* ---- files ---- */

static void load_playlist(const e16_api_t *api)
{
    n_files = 0;
    int f = api->file_open(PLAYLIST, E16_FILE_READ);
    if (f < 0)
        return;
    int len = api->file_read(f, playlist_buf, sizeof playlist_buf - 1);
    api->file_close(f);
    if (len <= 0)
        return;
    playlist_buf[len] = '\n';
    int start = 0;
    for (int i = 0; i <= len && n_files < MAX_FILES; i++) {
        if (playlist_buf[i] != '\n' && playlist_buf[i] != '\r')
            continue;
        int a = start, b = i;
        start = i + 1;
        while (a < b && (playlist_buf[a] == ' ' || playlist_buf[a] == '\t'))
            a++;
        while (b > a && (playlist_buf[b - 1] == ' ' || playlist_buf[b - 1] == '\t'))
            b--;
        if (a == b || playlist_buf[a] == '#')
            continue;
        char *p = names[n_files];
        int room = NAME_MAX - 1;
        if (playlist_buf[a] != '/') {
            const char *dir = FOLDER;
            while (*dir && room > 0) {
                *p++ = *dir++;
                room--;
            }
        }
        if (b - a > room)
            continue; /* too long: skip rather than open a truncated name */
        for (int k = a; k < b; k++)
            *p++ = (char)playlist_buf[k];
        *p = '\0';
        n_files++;
    }
}

static void log_open_error(const e16_api_t *api, const char *path, const char *why)
{
    char line[120], *p = put_str(line, "player: ");
    p = put_str(p, path);
    p = put_str(p, ": ");
    p = put_str(p, why);
    *p = '\0';
    api->log(line);
}

/* Open file i: header checked, positioned at frame 0. 0 if unusable. */
static int open_file(const e16_api_t *api, int i)
{
    if (fd >= 0)
        api->file_close(fd);
    fd = api->file_open(names[i], E16_FILE_READ);
    if (fd < 0) {
        log_open_error(api, names[i], "can't open");
        return 0;
    }
    if (api->file_read(fd, hdr, HDR_SIZE) != (int)HDR_SIZE || hdr[0] != 'E' || hdr[1] != '1' ||
        hdr[2] != '6' || hdr[3] != 'V' || hdr[4] != 1) {
        log_open_error(api, names[i], "not an .e16v (version 1) file");
        goto bad;
    }
    flags = hdr[5];
    data_start = rd16(hdr + 6);
    n_frames = rd16(hdr + 8);
    rec_size = rd32(hdr + 16);
    if (rd16(hdr + 12) != W || rd16(hdr + 14) != H || rec_size != REC_SIZE || n_frames == 0 ||
        data_start < HDR_SIZE || api->file_seek(fd, data_start) != 0) {
        log_open_error(api, names[i], "unexpected size or format");
        goto bad;
    }
    cur = i;
    frame = 0;
    return 1;
bad:
    api->file_close(fd);
    fd = -1;
    return 0;
}

/* Open file `start`, or the next usable one in direction dir. */
static int open_from(const e16_api_t *api, int start, int dir)
{
    for (int k = 0; k < n_files; k++) {
        int i = ((start + dir * k) % n_files + n_files) % n_files;
        if (open_file(api, i))
            return 1;
    }
    cur = -1;
    return 0;
}

/* The next frame record into rec (wrapping at the end). */
static int read_frame(const e16_api_t *api)
{
    if (frame >= n_frames) {
        frame = 0;
        if (api->file_seek(fd, data_start) != 0)
            return 0;
    }
    if (api->file_read(fd, rec, REC_SIZE) != (int)REC_SIZE)
        return 0;
    frame++;
    have_frame = 1;
    return 1;
}

/* ---- screens and notes ---- */

static void no_files_screen(const e16_api_t *api)
{
    unsigned char *fb = api->display_fb();
    for (int i = 0; i < E16_FB_SIZE; i++)
        fb[i] = 0;
    centered(fb, 4, "No animations");
    centered(fb, 18, "Copy .e16v files to");
    centered(fb, 28, "/apps/player/ and");
    centered(fb, 38, "list them in");
    centered(fb, 48, "playlist.txt");
    api->display_present();
    leds_frame(api);
    api->leds_show();
}

static void note_file(unsigned now)
{
    char s[40], *p = put_uint(s, (unsigned)cur + 1);
    p = put_str(p, "/");
    p = put_uint(p, (unsigned)n_files);
    p = put_str(p, " ");
    const char *b = base(names[cur]);
    for (int i = 0; b[i] && p < s + sizeof s - 1; i++)
        *p++ = b[i];
    *p = '\0';
    set_note(s, now);
}

static void note_value(const char *label, unsigned v, const char *unit, unsigned now)
{
    char s[40], *p = put_str(s, label);
    if (v == ~0u)
        p = put_str(p, "off");
    else
        p = put_uint(p, v);
    p = put_str(p, unit);
    *p = '\0';
    set_note(s, now);
}

static void log_status(const e16_api_t *api, unsigned shown, unsigned ms)
{
    char line[120], *p = put_str(line, "player: ");
    p = put_str(p, base(names[cur]));
    p = put_str(p, " ");
    p = put_uint(p, n_frames);
    p = put_str(p, " frames, ");
    unsigned fps10 = ms ? shown * 10000u / ms : 0;
    p = put_uint(p, fps10 / 10);
    p = put_str(p, ".");
    p = put_uint(p, fps10 % 10);
    p = put_str(p, " fps (speed ");
    p = put_uint(p, speeds[speed_i]);
    p = put_str(p, paused ? "%, paused)" : "%)");
    *p = '\0';
    api->log(line);
}

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

int app_main(const e16_api_t *api)
{
    api->log("player: enc 1 file, 2 speed, 3 LEDs, 4 press pause; hold Shift to leave");
    load_playlist(api);
    if (n_files == 0 || !open_from(api, 0, 1)) {
        api->log("player: no playable files (" PLAYLIST ")");
        no_files_screen(api);
        for (;;)
            api->delay_ms(50); /* Shift held 1 s leaves */
    }

    unsigned now = api->millis();
    unsigned due = now, t_log = now, shown = 0;
    int was_pressed = 0, need_show = 0;
    note_file(now);
    for (;;) {
        now = api->millis();

        int d = api->enc_read(0);
        if (d && n_files > 0) {
            int dir = d > 0 ? 1 : -1;
            int start = ((cur + d) % n_files + n_files) % n_files;
            if (open_from(api, start, dir)) {
                have_frame = 0;
                due = now;
                note_file(now);
                if (paused && read_frame(api)) { /* show the new file's first frame */
                    leds_frame(api);
                    need_show = 1;
                }
            } else {
                api->log("player: no playable files left");
                no_files_screen(api);
                for (;;)
                    api->delay_ms(50);
            }
        }
        d = api->enc_read(1);
        if (d) {
            speed_i = clamp(speed_i + d, 0, N_SPEEDS - 1);
            note_value("speed ", speeds[speed_i], "%", now);
            need_show = 1;
        }
        d = api->enc_read(2);
        if (d) {
            bright = clamp(bright + d, 0, 8);
            note_value("LEDs ", bright ? (unsigned)bright : ~0u, "", now);
            leds_frame(api);
            need_show = 1;
        }
        int pressed = api->enc_pressed(3);
        if (pressed && !was_pressed) {
            paused = !paused;
            set_note(paused ? "paused" : "playing", now);
            need_show = 1;
            due = now;
            if (paused) {
                char line[64], *p = put_str(line, "player: paused at frame ");
                p = put_uint(p, frame ? frame - 1 : 0);
                *p = '\0';
                api->log(line);
            }
        }
        was_pressed = pressed;

        if (!paused && (int)(now - due) >= 0) {
            if (!read_frame(api)) {
                log_open_error(api, names[cur], "read failed");
                if (!open_from(api, cur + 1, 1)) {
                    no_files_screen(api);
                    for (;;)
                        api->delay_ms(50);
                }
                continue;
            }
            unsigned delay = rd16(rec);
            if (delay < MIN_DELAY)
                delay = MIN_DELAY;
            due += delay * 100u / speeds[speed_i];
            if ((int)(now - due) > 250) /* fell far behind (a slow read): don't race to catch up */
                due = now;
            leds_frame(api);
            need_show = 1;
            shown++;
        }
        /* the note expiring needs a redraw too */
        if (note[0] && (int)(note_until - now) <= 0)
            need_show = 1;
        if (need_show) {
            show(api, now);
            need_show = 0;
        }
        if (leds_dirty && api->leds_show())
            leds_dirty = 0;

        if (now - t_log >= LOG_EVERY_MS) {
            log_status(api, shown, now - t_log);
            shown = 0;
            t_log = now;
        }
        if ((int)(due - api->millis()) > 1 || paused)
            api->delay_ms(1);
    }
}
