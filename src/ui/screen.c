/*
 * OLED screens: running (title + 4x4 labels), menu, error. See screen.h.
 */
#include "screen.h"
#include "font.h"
#include "port_e16.h"
#include "port_e16_audio.h"
#include "canvas.h"
#include "rt.h"
#include "rt_api_ctl.h"
#include "scenes.h"

#include "display.h"
#include "ccm.h"

#include <string.h>

#define W DISPLAY_WIDTH
#define PAGES DISPLAY_PAGES
#define FRAME_MS 20u
#define BAR_H 10            /* inverted title bar: rows 0-9, text at y = 1 */
#define SLOT_W 32
#define ERR_COLS 21
#define ERR_LINES 6
#define MENU_ROWS 5
#define MENU_Y 12
#define MENU_PITCH 10

typedef enum { MODE_NONE, MODE_MENU, MODE_RUNNING, MODE_ERROR, MODE_TEXT, MODE_CUSTOM } screen_mode_t;

static uint8_t fb[PAGES][W] CCM_BSS; /* cleared by screen_init() */
static uint32_t sent_hash[PAGES];
static bool sent_valid;

static screen_mode_t g_mode;
static bool g_dirty;
static uint32_t g_last_frame;

static int g_count, g_selected;     /* menu: scenes.h's entries */
static int g_menu_top;              /* first visible menu row */
static const rt_scene_t *g_scene;    /* running */
static bool g_picker;
static const char *g_error;
static const char *g_text_title, *g_text_lines[3];
static const char *const *g_menu_extra; /* menu entries after the scenes */
static int g_menu_extras;
static screen_draw_fn g_custom;   /* MODE_CUSTOM */

/* What the running screen last drew from (to skip identical redraws). */
static uint32_t g_drawn_version;
static int g_drawn_page;
static bool g_err_shown;     /* rt_error_count() > 0 when last drawn */
static uint32_t g_drawn_canvas;  /* port_e16_canvas_version() when last drawn */
static bool g_audio_shown;       /* port_e16_audio_running() when last drawn */

/* --- drawing --- */

static void clear(void)
{
    memset(fb, 0, sizeof fb);
}

static void pixel_column(int x, int y, uint8_t bits, bool invert)
{
    if (x < 0 || x >= W) {
        return;
    }
    for (int r = 0; r < FONT_HEIGHT; r++) {
        int py = y + r;
        if (py < 0 || py >= PAGES * 8 || !(bits & (1u << r))) {
            continue;
        }
        uint8_t m = (uint8_t)(1u << (py & 7));
        if (invert) {
            fb[py >> 3][x] &= (uint8_t)~m;
        } else {
            fb[py >> 3][x] |= m;
        }
    }
}

static void fill_rect(int x, int y, int w, int h)
{
    for (int py = y; py < y + h && py < PAGES * 8; py++) {
        if (py < 0) {
            continue;
        }
        for (int px = x; px < x + w && px < W; px++) {
            if (px >= 0) {
                fb[py >> 3][px] |= (uint8_t)(1u << (py & 7));
            }
        }
    }
}

/* Proportional text at (x, y), at most n bytes; invert = dark on lit. */
static void text(int x, int y, const char *s, int n, bool invert)
{
    bool any = false;
    for (int i = 0; i < n && s[i]; i++) {
        const uint8_t *cols;
        int gw = font_glyph((uint8_t)s[i], &cols);
        if (gw <= 0) {
            continue;
        }
        if (any) {
            x += FONT_GAP;
        }
        for (int c = 0; c < gw; c++) {
            pixel_column(x + c, y, cols[c], invert);
        }
        x += gw;
        any = true;
    }
}

static void text_centered(int x0, int w, int y, const char *s, int n, bool invert)
{
    int tw = font_text_width(s, n);
    text(x0 + (w - tw) / 2, y, s, n, invert);
}

/* Fixed 6-px cells (the error screen's 21 columns). */
static void text_mono(int x, int y, const char *s, int n)
{
    for (int i = 0; i < n && s[i]; i++, x += 6) {
        const uint8_t *cols;
        int gw = font_glyph((uint8_t)s[i], &cols);
        int off = gw < 5 ? (5 - gw) / 2 : 0;
        for (int c = 0; c < gw && c < 6; c++) {
            pixel_column(x + off + c, y, cols[c], false);
        }
    }
}

static void title_bar(const char *s)
{
    fill_rect(0, 0, W, BAR_H);
    text_centered(0, W, 1, s, 15, true);
}

/* --- screens --- */

static bool assigned(const rt_scene_t *sc, int id)
{
    return id >= 1 && id <= 64 && ((sc->assigned[(id - 1) / 8] >> ((id - 1) % 8)) & 1);
}

/* The stock default for an unset slot: the abbr of the control wired to
 * encoder `enc` on `page` (its turn id, else its push id), per the
 * runtime's own wiring (rt_ctl_ids()). */
static const char *default_label(const rt_scene_t *sc, int page, int enc)
{
    int ids[2];
    rt_ctl_ids(page, enc, &ids[0], &ids[1]);
    for (int k = 0; k < 2; k++) {
        if (assigned(sc, ids[k]) && sc->abbr[ids[k] - 1]) {
            return sc->abbr[ids[k] - 1];
        }
    }
    return "";
}

/* A speaker, 7 columns (bit 0 = top), at the bar's left end while the
 * synth runs (on USB audio or TRS 2). */
static const uint8_t SPEAKER[] = {0x18, 0x18, 0x3C, 0x7E, 0x00, 0x24, 0x42};

/* The draw.* canvas, rows 0..h-1, onto the framebuffer from row y0. */
static void blit_canvas(int y0, int h)
{
    const uint8_t *bits = port_e16_canvas_bits();
    for (int y = 0; y < h; y++) {
        const uint8_t *row = bits + y * RT_CANVAS_STRIDE;
        int py = y0 + y;
        uint8_t m = (uint8_t)(1u << (py & 7));
        uint8_t *dst = fb[py >> 3];
        for (int xb = 0; xb < RT_CANVAS_STRIDE; xb++) {
            uint8_t b = row[xb];
            if (!b) {
                continue;
            }
            for (int k = 0; k < 8; k++) {
                if (b & (1u << k)) {
                    dst[xb * 8 + k] |= m;
                }
            }
        }
    }
}

static void draw_running(void)
{
    /* The canvas replaces the label grid while the script draws (the page
     * picker still takes the grid's place while it's open); fullscreen,
     * it replaces the title bar too. */
    bool canvas = port_e16_canvas_active() && !g_picker;
    if (canvas && port_e16_canvas_fullscreen()) {
        blit_canvas(0, RT_CANVAS_HEIGHT_FULL);
        return;
    }
    const char *t = port_e16_title();
    title_bar(t ? t : (g_scene && g_scene->title ? g_scene->title : ""));
    if (g_err_shown) {
        /* The script has had runtime errors (rt_error_count(); the log
         * has the first one in full): a "!" at the bar's right end. */
        text(W - 5, 1, "!", 1, true);
    }
    if (g_audio_shown) {
        for (int c = 0; c < (int)sizeof SPEAKER; c++) {
            pixel_column(2 + c, 1, SPEAKER[c], true);
        }
    }
    if (canvas) {
        blit_canvas(BAR_H, RT_CANVAS_HEIGHT_NORMAL);
        return;
    }
    int page = rt_page();
    for (int i = 0; i < 16; i++) {
        int x0 = (i % 4) * SLOT_W;
        int y = 16 + (i / 4) * 12;
        if (g_picker) {
            if (g_scene && i < g_scene->pages) {
                const char *name = g_scene->page_names[i] ? g_scene->page_names[i] : "";
                bool cur = (i + 1 == page);
                if (cur) {
                    fill_rect(x0 + 1, y - 1, SLOT_W - 2, 10);
                }
                /* Page names may be longer than a label ("L13-16"):
                 * show as much as fits the slot. */
                int n = (int)strlen(name);
                while (n > 0 && font_text_width(name, n) > SLOT_W - 1) {
                    n--;
                }
                text_centered(x0, SLOT_W, y, name, n, cur);
            }
            continue;
        }
        const char *s = port_e16_label(i + 1);
        if (!s) {
            s = g_scene ? default_label(g_scene, page, i + 1) : "";
        }
        text_centered(x0, SLOT_W, y, s, 4, false);
    }
}

/* Lights (or, on the selection bar, clears) a w x h block. */
static void mark(int x, int y, int w, int h, bool invert)
{
    for (int py = y; py < y + h; py++) {
        for (int px = x; px < x + w; px++) {
            pixel_column(px, py, 1, invert);
        }
    }
}

/*
 * The menu scrolls to keep the selection on screen: the window only moves
 * when the selection would leave it, so turning back up doesn't jump.
 * Markers: a small square left of a disk script's title, "!" at the
 * right end for a broken one (its header doesn't parse), "app" at the
 * right end for a native app (an .e16app in /apps), and a thin
 * scroll bar at the right edge when there are more entries than rows.
 */
static void draw_menu(void)
{
    title_bar("E16 Launcher");
    int total = g_count + g_menu_extras;
    if (g_selected < g_menu_top) {
        g_menu_top = g_selected;
    } else if (g_selected >= g_menu_top + MENU_ROWS) {
        g_menu_top = g_selected - MENU_ROWS + 1;
    }
    if (g_menu_top > total - MENU_ROWS) {
        g_menu_top = total > MENU_ROWS ? total - MENU_ROWS : 0;
    }
    if (g_menu_top < 0) {
        g_menu_top = 0;
    }
    int top = g_menu_top;
    for (int r = 0; r < MENU_ROWS && top + r < total; r++) {
        int i = top + r;
        int y = MENU_Y + r * MENU_PITCH;
        bool sel = (i == g_selected);
        if (sel) {
            fill_rect(0, y - 1, W - 3, MENU_PITCH);
        }
        if (i < g_count) {
            scene_info_t info = scene_info(i);
            text(6, y, info.title, SCENES_TITLE_MAX, sel);
            if (info.source == SCENE_DISK) {
                mark(1, y + 2, 3, 3, sel);
            }
            if (info.broken) {
                text(W - 9, y, "!", 1, sel);
            }
            if (info.source == SCENE_APP) {
                text(W - 5 - font_text_width("app", 3), y, "app", 3, sel);
            }
        } else {
            text(6, y, g_menu_extra[i - g_count], 15, sel);
        }
    }
    if (total > MENU_ROWS) {
        /* rows MENU_Y-1 .. bottom: the bar's length and position follow
         * the visible window */
        int span = PAGES * 8 - (MENU_Y - 1);
        int len = span * MENU_ROWS / total;
        int pos = (span - len) * top / (total - MENU_ROWS);
        fill_rect(W - 2, MENU_Y - 1 + pos, 2, len);
    }
}

static void draw_error(void)
{
    title_bar("ERROR");
    const char *s = g_error ? g_error : "";
    int row = 0;
    for (int line = 0; line < 3 && *s && row < ERR_LINES; line++) {
        const char *end = strchr(s, '\n');
        int len = end ? (int)(end - s) : (int)strlen(s);
        /* Tracebacks indent with tabs; skip leading whitespace. */
        while (len > 0 && (*s == '\t' || *s == ' ')) {
            s++;
            len--;
        }
        do {
            int n = len > ERR_COLS ? ERR_COLS : len;
            text_mono(0, 11 + row * 9, s, n);
            s += n;
            len -= n;
            row++;
        } while (len > 0 && row < ERR_LINES);
        if (!end) {
            break;
        }
        s = end + 1;
    }
}

static void draw_text(void)
{
    title_bar(g_text_title ? g_text_title : "");
    int n = 0;
    while (n < 3 && g_text_lines[n]) {
        n++;
    }
    /* One line sits at y = 32; more are centered around it. */
    int y = n <= 1 ? 32 : 32 - (n - 1) * 6;
    for (int i = 0; i < n; i++, y += 12) {
        text_centered(0, W, y, g_text_lines[i], 24, false);
    }
}

static void render(void)
{
    clear();
    switch (g_mode) {
        case MODE_MENU:
            draw_menu();
            break;
        case MODE_RUNNING:
            draw_running();
            break;
        case MODE_ERROR:
            draw_error();
            break;
        case MODE_TEXT:
            draw_text();
            break;
        case MODE_CUSTOM:
            if (g_custom) {
                g_custom();
            }
            break;
        default:
            break;
    }
}

static uint32_t page_hash(const uint8_t *p)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < W; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

static void send(void)
{
    for (int p = 0; p < PAGES; p++) {
        uint32_t h = page_hash(fb[p]);
        if (!sent_valid || h != sent_hash[p]) {
            display_write(p, 0, fb[p], W);
            sent_hash[p] = h;
        }
    }
    sent_valid = true;
}

/* --- interface --- */

void screen_init(void)
{
    clear();
    sent_valid = false;
    send();
    g_mode = MODE_NONE;
}

static void set_mode(screen_mode_t m)
{
    g_mode = m;
    g_dirty = true;
}

void screen_show_menu(int count, const char *const *extra, int n_extra, int selected)
{
    g_count = count;
    g_menu_extra = extra;
    g_menu_extras = extra ? n_extra : 0;
    g_selected = selected;
    g_picker = false;
    set_mode(MODE_MENU);
}

void screen_show_running(const rt_scene_t *scene)
{
    g_scene = scene;
    g_picker = false;
    set_mode(MODE_RUNNING);
}

void screen_show_error(const char *message)
{
    g_error = message;
    g_picker = false;
    set_mode(MODE_ERROR);
}

void screen_show_page_picker(bool open)
{
    if (g_picker != open) {
        g_picker = open;
        g_dirty = true;
    }
}

void screen_show_text(const char *title, const char *line)
{
    screen_show_notice(title, line, NULL, NULL);
}

void screen_show_notice(const char *title, const char *l1, const char *l2, const char *l3)
{
    g_text_title = title;
    g_text_lines[0] = l1;
    g_text_lines[1] = l1 ? l2 : NULL;
    g_text_lines[2] = l1 && l2 ? l3 : NULL;
    set_mode(MODE_TEXT);
}

void screen_poll(uint32_t now_ms)
{
    if (g_mode == MODE_RUNNING) {
        /* Labels and title change underneath (slots.update, setTitle):
         * port_e16_ui_version() only moves when the text actually differs. */
        uint32_t v = port_e16_ui_version();
        int page = rt_page();
        bool err = rt_error_count() > 0;
        if (v != g_drawn_version || page != g_drawn_page || err != g_err_shown ||
            port_e16_canvas_version() != g_drawn_canvas || port_e16_audio_running() != g_audio_shown) {
            g_dirty = true;
        }
    }
    if (!g_dirty || now_ms - g_last_frame < FRAME_MS) {
        return;
    }
    screen_flush();
    g_last_frame = now_ms;
}

void screen_flush(void)
{
    g_drawn_version = port_e16_ui_version();
    g_drawn_page = rt_page();
    g_err_shown = rt_error_count() > 0;
    g_drawn_canvas = port_e16_canvas_version();
    g_audio_shown = port_e16_audio_running();
    render();
    send();
    g_dirty = false;
}

const uint8_t *screen_framebuffer(void)
{
    return &fb[0][0];
}

/* --- a screen drawn by someone else --- */

void screen_show_custom(screen_draw_fn draw)
{
    g_custom = draw;
    g_picker = false;
    set_mode(MODE_CUSTOM);
}

void screen_touch(void)
{
    g_dirty = true;
}

uint8_t *screen_draw_buffer(void)
{
    return &fb[0][0];
}

void screen_draw_text(int x, int y, const char *s, bool invert)
{
    text(x, y, s, 64, invert);
}

void screen_draw_title(const char *s)
{
    title_bar(s);
}

void screen_draw_rect(int x, int y, int w, int h)
{
    fill_rect(x, y, w, h);
}
