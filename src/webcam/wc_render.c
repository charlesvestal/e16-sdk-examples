/*
 * Webcam mode's video synth: wc_render.h.
 */
#include "wc_render.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "font.h"

const wc_param_info_t wc_param_info[WC_PARAMS] = {
    [WC_PATTERN] = {"PAT", 0, WC_PATTERNS - 1, WC_PLASMA, true},
    [WC_HUE] = {"HUE", 0, 63, 0, true},
    [WC_SPEED] = {"SPEED", 0, 32, 4, false},
    [WC_ZOOM] = {"ZOOM", 1, 8, 2, false},
    [WC_PATTERN_B] = {"PAT B", 0, WC_PATTERNS - 1, WC_RINGS, true},
    [WC_MIX] = {"MIX", 0, 16, 0, false},
    [WC_MIX_MODE] = {"MODE", 0, WC_MIX_MODES - 1, WC_MIX_BLEND, true},
    [WC_SYMMETRY] = {"SYMM", 0, WC_SYMS - 1, WC_SYM_OFF, true},
    [WC_PALETTE] = {"PAL", 0, WC_PALETTES - 1, WC_PAL_RAINBOW, true},
    [WC_CYCLE] = {"CYCLE", -16, 16, 0, false},
    [WC_POSTERIZE] = {"POST", 0, 16, 0, false},
    [WC_CONTRAST] = {"CONTR", 0, 16, 8, false},
    [WC_BRIGHT] = {"BRITE", -8, 8, 0, false},
    [WC_WARP] = {"WARP", 0, 16, 0, false},
    [WC_TEXT] = {"TEXT", 0, WC_TEXTS - 1, WC_TEXT_OFF, true},
    [WC_SCANLINES] = {"SCAN", 0, 3, 0, false},
};

static const char *const pattern_names[WC_PATTERNS] = {"PLASMA", "TUNNEL", "RINGS", "BARS", "SPIRAL", "CHECKS"};
static const char *const mix_names[WC_MIX_MODES] = {"BLEND", "ADD", "XOR", "WIPE"};
static const char *const sym_names[WC_SYMS] = {"OFF", "MIRROR", "QUAD", "KALEID"};
static const char *const pal_names[WC_PALETTES] = {"RAINBW", "FIRE", "ICE", "NEON", "MONO"};
static const char *const text_names[WC_TEXTS] = {"OFF", "NAME", "NOTE", "E16"};

const char *wc_value_text(int param, int value, char *buf)
{
    switch (param) {
        case WC_PATTERN:
        case WC_PATTERN_B:
            return pattern_names[wc_param_limit(param, value)];
        case WC_MIX_MODE:
            return mix_names[wc_param_limit(param, value)];
        case WC_SYMMETRY:
            return sym_names[wc_param_limit(param, value)];
        case WC_PALETTE:
            return pal_names[wc_param_limit(param, value)];
        case WC_TEXT:
            return text_names[wc_param_limit(param, value)];
        case WC_POSTERIZE:
            if (value < 2) {
                return "OFF";
            }
            break;
        default:
            break;
    }
    snprintf(buf, 8, "%d", value);
    return buf;
}

int wc_param_limit(int param, int v)
{
    const wc_param_info_t *p = &wc_param_info[param];
    if (p->wraps) {
        int n = p->max - p->min + 1;
        return p->min + (((v - p->min) % n) + n) % n;
    }
    return v < p->min ? p->min : v > p->max ? p->max : v;
}

int wc_param_from_cc(int param, int cc_value)
{
    const wc_param_info_t *p = &wc_param_info[param];
    if (cc_value < 0) {
        cc_value = 0;
    } else if (cc_value > 127) {
        cc_value = 127;
    }
    int n = p->max - p->min + 1;
    return p->min + cc_value * n / 128;
}

/* ---- Tables ---- */

void wc_tables_init(wc_tables_t *t)
{
    for (int i = 0; i < 256; i++) {
        t->sin64[i] = (int8_t)lrintf(63.0f * sinf((float)i * 6.2831853f / 256.0f));
    }
    for (int y = 0; y < WC_QH; y++) {
        for (int x = 0; x < WC_QW; x++) {
            /* Pixel centres sit half a pixel off the picture's centre. */
            float dx = (float)x + 0.5f, dy = (float)y + 0.5f;
            float d = sqrtf(dx * dx + dy * dy);
            t->qdist[y][x] = (uint8_t)d;
            t->qangle[y][x] = (uint8_t)lrintf(atan2f(dy, dx) * 256.0f / 6.2831853f);
        }
    }
    for (int d = 0; d < 256; d++) {
        t->depth[d] = (uint8_t)(2048 / (d + 4));
    }
}

/* ---- Palette ---- */

static int clamp8(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

/* A triangle wave over the index: 0 at 0, 254 at 127, back to 0. */
static int tri(uint8_t a)
{
    return a < 128 ? 2 * a : 2 * (255 - a);
}

static void base_colour(const wc_tables_t *t, int type, uint8_t a, int *r, int *g, int *b)
{
    const int8_t *s = t->sin64;
    int k = tri(a) * 3;
    switch (type) {
        case WC_PAL_FIRE:
            *r = clamp8(k), *g = clamp8(k - 255), *b = clamp8(k - 510);
            break;
        case WC_PAL_ICE:
            *b = clamp8(k), *g = clamp8(k - 255), *r = clamp8(k - 510);
            break;
        case WC_PAL_NEON: {
            /* Magenta and cyan stripes with dark gaps between. */
            int p = s[(uint8_t)(a * 2)];
            if (p >= 0) {
                *r = clamp8(p * 4), *g = 0, *b = clamp8(p * 13 / 4);
            } else {
                *r = 0, *g = clamp8(-p * 7 / 2), *b = clamp8(-p * 4);
            }
            break;
        }
        case WC_PAL_MONO:
            *r = *g = *b = tri(a);
            break;
        default: /* a smooth, looping rainbow */
            *r = clamp8(128 + 2 * s[a]);
            *g = clamp8(128 + 2 * s[(uint8_t)(a + 85)]);
            *b = clamp8(128 + 2 * s[(uint8_t)(a + 171)]);
            break;
    }
}

static int grade(int c, int contrast, int bright, int flash)
{
    c = 128 + (c - 128) * contrast / 8 + bright * 16;
    c = clamp8(c);
    return c + (255 - c) * flash / 255;
}

void wc_palette_build(wc_palette_t *p, const wc_tables_t *t, const int16_t *params, uint8_t flash)
{
    int bands = params[WC_POSTERIZE];
    for (int i = 0; i < 256; i++) {
        int q = i;
        if (bands >= 2) {
            int k = i * bands >> 8;
            q = (k * 256 + 128) / bands;
        }
        uint8_t a = (uint8_t)(q + params[WC_HUE] * 4);
        int r, g, b;
        base_colour(t, params[WC_PALETTE], a, &r, &g, &b);
        r = grade(r, params[WC_CONTRAST], params[WC_BRIGHT], flash);
        g = grade(g, params[WC_CONTRAST], params[WC_BRIGHT], flash);
        b = grade(b, params[WC_CONTRAST], params[WC_BRIGHT], flash);
        p->r[i] = (uint8_t)r, p->g[i] = (uint8_t)g, p->b[i] = (uint8_t)b;
        /* BT.601, video range */
        p->y[i] = (uint8_t)(16 + ((66 * r + 129 * g + 25 * b + 128) >> 8));
        p->u[i] = (uint8_t)(128 + ((-38 * r - 74 * g + 112 * b + 128) >> 8));
        p->v[i] = (uint8_t)(128 + ((112 * r - 94 * g - 18 * b + 128) >> 8));
    }
}

/* ---- Text overlay ---- */

void wc_text_set(wc_text_t *m, const char *s)
{
    memset(m, 0, sizeof *m);
    int col = 0;
    for (; s && *s && col < WC_TEXT_COLS; s++) {
        const uint8_t *cols;
        int w = font_glyph((uint8_t)*s, &cols);
        for (int c = 0; c < w && col < WC_TEXT_COLS; c++, col++) {
            for (int row = 0; row < 8; row++) {
                if (cols[c] & (1u << row)) {
                    m->bits[row][col >> 3] |= (uint8_t)(0x80u >> (col & 7));
                    m->on = true;
                }
            }
        }
        col += FONT_GAP;
    }
}

/* ---- The picture ---- */

void wc_view_prepare(wc_view_t *v, const wc_tables_t *t, const wc_settings_t *s, const uint8_t *py,
                     const uint8_t *pu, const uint8_t *pv)
{
    const int16_t *p = s->params;
    v->tab = t;
    v->py = py, v->pu = pu, v->pv = pv;
    v->pattern = (uint8_t)wc_param_limit(WC_PATTERN, p[WC_PATTERN]);
    v->pattern_b = (uint8_t)wc_param_limit(WC_PATTERN_B, p[WC_PATTERN_B]);
    v->mix = (uint8_t)wc_param_limit(WC_MIX, p[WC_MIX]);
    v->mix_mode = (uint8_t)wc_param_limit(WC_MIX_MODE, p[WC_MIX_MODE]);
    v->symmetry = (uint8_t)wc_param_limit(WC_SYMMETRY, p[WC_SYMMETRY]);
    v->zoom = (uint8_t)wc_param_limit(WC_ZOOM, p[WC_ZOOM]);
    static const uint8_t scan[4] = {0, 192, 128, 64};
    v->scan_k = scan[wc_param_limit(WC_SCANLINES, p[WC_SCANLINES])];
    v->t = (uint8_t)(s->clock >> 5);
    v->cyc = (uint8_t)(s->cycle >> 6);

    uint8_t t8 = v->t;
    if (v->pattern == WC_PLASMA || (v->pattern_b == WC_PLASMA && v->mix)) {
        for (int x = 0; x < WC_W; x++) {
            v->xwave[x] = t->sin64[(uint8_t)(x * v->zoom + t8)];
        }
        for (int i = 0; i < WC_W + WC_H; i++) {
            v->dwave[i] = t->sin64[(uint8_t)((i * v->zoom >> 1) - 2 * t8)];
        }
    }
    int warp = wc_param_limit(WC_WARP, p[WC_WARP]);
    for (int y = 0; y < WC_H; y++) {
        v->warp[y] = (int8_t)(t->sin64[(uint8_t)(y * 4 + 2 * t8)] * warp >> 4);
    }
    v->text = s->text;
}

/* One pattern's palette index at (fx, fy) (screen coordinates, after the
 * symmetry fold) with (qx, qy, left, up) the same point in quarter terms. */
static inline uint8_t pattern(const wc_view_t *v, int pat, int fx, int fy, int qx, int qy, bool left, bool up,
                              uint8_t *shade)
{
    const wc_tables_t *tb = v->tab;
    uint8_t t = v->t;
    uint8_t zoom = v->zoom;
    uint8_t d = tb->qdist[qy][qx];
    switch (pat) {
        case WC_PLASMA: {
            int s = v->xwave[fx] + v->dwave[fx + fy] + tb->sin64[(uint8_t)(fy * zoom + 3 * t)] +
                    tb->sin64[(uint8_t)(d * zoom * 2 - 2 * t)];
            return (uint8_t)(s + t);
        }
        case WC_TUNNEL: {
            uint8_t a = tb->qangle[qy][qx];
            uint8_t angle = left ? (up ? 128 + a : 128 - a) : (up ? 256 - a : a);
            uint8_t z = tb->depth[d];
            *shade = (uint8_t)(d >= 50 ? 255 : d * 5);
            return (uint8_t)((uint8_t)(z * zoom / 2 + 2 * t) ^ (uint8_t)(angle + t));
        }
        case WC_RINGS:
            return (uint8_t)(d * zoom * 3 - 3 * t);
        case WC_BARS: { /* eight bars, wobbling */
            int wobble = tb->sin64[(uint8_t)(fy * 2 + t)] >> 2;
            return (uint8_t)(((fx + wobble) * zoom + 2 * t) & 0xE0);
        }
        case WC_SPIRAL: { /* two arms, turning */
            uint8_t a = tb->qangle[qy][qx];
            uint8_t angle = left ? (up ? 128 + a : 128 - a) : (up ? 256 - a : a);
            return (uint8_t)(angle * 2 + d * zoom * 2 - 4 * t);
        }
        default: { /* CHECKS: a scrolling checkerboard, tinted along the diagonal */
            int cx = (fx * zoom + t) >> 4, cy = (fy * zoom + (t >> 1)) >> 4;
            return (uint8_t)(((cx ^ cy) & 1 ? 128 : 0) + ((fx + fy) >> 2) + t);
        }
    }
}

/* Palette index and brightness of screen pixel (x, y), whose line has the
 * warp shift `shift`. */
static inline uint8_t pixel(const wc_view_t *v, int x, int y, int shift, uint8_t *shade)
{
    int sx = x + shift;
    if (sx < 0) {
        sx += WC_W;
    } else if (sx >= WC_W) {
        sx -= WC_W;
    }
    bool left = sx < WC_QW, up = y < WC_QH;
    int qx = left ? WC_QW - 1 - sx : sx - WC_QW;
    int qy = up ? WC_QH - 1 - y : y - WC_QH;
    switch (v->symmetry) {
        case WC_SYM_MIRROR:
            left = false;
            break;
        case WC_SYM_QUAD:
            left = up = false;
            break;
        case WC_SYM_KALEIDO:
            left = up = false;
            if (qy > qx) { /* fold along the diagonal too: both fit, qx < qy < WC_QH <= WC_QW */
                int tmp = qx;
                qx = qy;
                qy = tmp;
            }
            break;
        default:
            break;
    }
    int fx = left ? WC_QW - 1 - qx : WC_QW + qx;
    int fy = up ? WC_QH - 1 - qy : WC_QH + qy;

    uint8_t sa = 255;
    uint8_t a = pattern(v, v->pattern, fx, fy, qx, qy, left, up, &sa);
    uint8_t m = v->mix;
    if (m) {
        if (v->mix_mode == WC_MIX_WIPE) {
            if (x * 16 < m * WC_W) { /* B left of the edge, A right of it */
                sa = 255;
                a = pattern(v, v->pattern_b, fx, fy, qx, qy, left, up, &sa);
            }
        } else {
            uint8_t sb = 255;
            uint8_t b = pattern(v, v->pattern_b, fx, fy, qx, qy, left, up, &sb);
            switch (v->mix_mode) {
                case WC_MIX_ADD:
                    a = (uint8_t)(a + (b * m >> 4));
                    break;
                case WC_MIX_XOR:
                    a = (uint8_t)(a ^ (b * m >> 4));
                    break;
                default: /* BLEND, the short way round the palette */
                    a = (uint8_t)(a + ((int8_t)(uint8_t)(b - a) * m >> 4));
                    break;
            }
            if (sb != 255) {
                sb = (uint8_t)(255 - ((255 - sb) * m >> 4));
                sa = (uint8_t)(sa * sb >> 8);
            }
        }
    }
    *shade = sa;
    return (uint8_t)(a + v->cyc);
}

uint8_t wc_sample(const wc_view_t *v, int x, int y, uint8_t *shade)
{
    return pixel(v, x, y, v->warp[y], shade);
}

static inline bool text_bit(const wc_view_t *v, int row, int x)
{
    int col = (x - WC_TEXT_X) >> 1;
    if (x < WC_TEXT_X || col >= WC_TEXT_COLS) {
        return false;
    }
    return v->text.bits[row][col >> 3] & (0x80u >> (col & 7));
}

/* The full-turn angle of a pixel from its quarter angle a and its side. */
static inline uint8_t full_angle(uint8_t a, uint8_t side)
{
    bool left = side & 1, up = side & 2;
    return (uint8_t)(left ? (up ? 128 + a : 128 - a) : (up ? 256 - a : a));
}

/* One pattern over the whole line: the same as pattern(), one loop per kind. */
static void pattern_line(const wc_view_t *v, const wc_line_t *w, int pat, uint8_t *idx, uint8_t *shade)
{
    const wc_tables_t *tb = v->tab;
    const uint8_t *qdist = &tb->qdist[0][0], *qangle = &tb->qangle[0][0];
    const int8_t *sn = tb->sin64;
    const uint8_t t = v->t, zoom = v->zoom;
    switch (pat) {
        case WC_PLASMA:
            for (int x = 0; x < WC_W; x++) {
                int fx = w->fx[x], fy = w->fy[x];
                uint8_t d = qdist[w->qy[x] * WC_QW + w->qx[x]];
                int s = v->xwave[fx] + v->dwave[fx + fy] + sn[(uint8_t)(fy * zoom + 3 * t)] +
                        sn[(uint8_t)(d * zoom * 2 - 2 * t)];
                idx[x] = (uint8_t)(s + t);
            }
            break;
        case WC_TUNNEL:
            for (int x = 0; x < WC_W; x++) {
                int q = w->qy[x] * WC_QW + w->qx[x];
                uint8_t d = qdist[q];
                uint8_t angle = full_angle(qangle[q], w->side[x]);
                uint8_t z = tb->depth[d];
                shade[x] = (uint8_t)(d >= 50 ? 255 : d * 5);
                idx[x] = (uint8_t)((uint8_t)(z * zoom / 2 + 2 * t) ^ (uint8_t)(angle + t));
            }
            break;
        case WC_RINGS:
            for (int x = 0; x < WC_W; x++) {
                uint8_t d = qdist[w->qy[x] * WC_QW + w->qx[x]];
                idx[x] = (uint8_t)(d * zoom * 3 - 3 * t);
            }
            break;
        case WC_BARS:
            for (int x = 0; x < WC_W; x++) {
                int wobble = sn[(uint8_t)(w->fy[x] * 2 + t)] >> 2;
                idx[x] = (uint8_t)(((w->fx[x] + wobble) * zoom + 2 * t) & 0xE0);
            }
            break;
        case WC_SPIRAL:
            for (int x = 0; x < WC_W; x++) {
                int q = w->qy[x] * WC_QW + w->qx[x];
                uint8_t angle = full_angle(qangle[q], w->side[x]);
                idx[x] = (uint8_t)(angle * 2 + qdist[q] * zoom * 2 - 4 * t);
            }
            break;
        default:
            for (int x = 0; x < WC_W; x++) {
                int fx = w->fx[x], fy = w->fy[x];
                int cx = (fx * zoom + t) >> 4, cy = (fy * zoom + (t >> 1)) >> 4;
                idx[x] = (uint8_t)(((cx ^ cy) & 1 ? 128 : 0) + ((fx + fy) >> 2) + t);
            }
            break;
    }
}

void wc_render_line(const wc_view_t *v, wc_line_t *w, int y, uint8_t *yuy2)
{
    /* Coordinates: the warp, then the symmetry fold (as pixel() does). */
    const int shift = v->warp[y];
    const bool up0 = y < WC_QH;
    const int qy0 = up0 ? WC_QH - 1 - y : y - WC_QH;
    const uint8_t sym = v->symmetry;
    for (int x = 0; x < WC_W; x++) {
        int sx = x + shift;
        if (sx < 0) {
            sx += WC_W;
        } else if (sx >= WC_W) {
            sx -= WC_W;
        }
        bool left = sx < WC_QW, up = up0;
        int qx = left ? WC_QW - 1 - sx : sx - WC_QW;
        int qy = qy0;
        if (sym != WC_SYM_OFF) {
            left = false;
            if (sym != WC_SYM_MIRROR) {
                up = false;
                if (sym == WC_SYM_KALEIDO && qy > qx) {
                    int tmp = qx;
                    qx = qy;
                    qy = tmp;
                }
            }
        }
        w->qx[x] = (uint8_t)qx;
        w->qy[x] = (uint8_t)qy;
        w->fx[x] = (uint8_t)(left ? WC_QW - 1 - qx : WC_QW + qx);
        w->fy[x] = (uint8_t)(up ? WC_QH - 1 - qy : WC_QH + qy);
        w->side[x] = (uint8_t)(left | up << 1);
    }

    /* Pattern A, then B mixed in. */
    uint8_t *a = w->a, *sa = w->sa;
    memset(sa, 255, WC_W);
    pattern_line(v, w, v->pattern, a, sa);
    const uint8_t m = v->mix;
    if (m) {
        uint8_t *b = w->b, *sb = w->sb;
        memset(sb, 255, WC_W);
        pattern_line(v, w, v->pattern_b, b, sb);
        switch (v->mix_mode) {
            case WC_MIX_WIPE: /* B left of the edge, A right of it */
                for (int x = 0; x < WC_W && x * 16 < m * WC_W; x++) {
                    a[x] = b[x];
                    sa[x] = sb[x];
                }
                break;
            case WC_MIX_ADD:
                for (int x = 0; x < WC_W; x++) {
                    a[x] = (uint8_t)(a[x] + (b[x] * m >> 4));
                }
                break;
            case WC_MIX_XOR:
                for (int x = 0; x < WC_W; x++) {
                    a[x] = (uint8_t)(a[x] ^ (b[x] * m >> 4));
                }
                break;
            default: /* BLEND, the short way round the palette */
                for (int x = 0; x < WC_W; x++) {
                    a[x] = (uint8_t)(a[x] + ((int8_t)(uint8_t)(b[x] - a[x]) * m >> 4));
                }
                break;
        }
        if (v->mix_mode != WC_MIX_WIPE && (v->pattern_b == WC_TUNNEL)) {
            for (int x = 0; x < WC_W; x++) {
                if (sb[x] != 255) {
                    uint8_t s2 = (uint8_t)(255 - ((255 - sb[x]) * m >> 4));
                    sa[x] = (uint8_t)(sa[x] * s2 >> 8);
                }
            }
        }
    }

    /* Colours. */
    const uint8_t *py = v->py, *pu = v->pu, *pv = v->pv;
    const uint8_t cyc = v->cyc;
    const bool shaded = v->pattern == WC_TUNNEL || (m && v->pattern_b == WC_TUNNEL);
    const int k = (y & 1) && v->scan_k ? v->scan_k : 256;
    uint32_t *out = (uint32_t *)yuy2;
    if (!shaded && k == 256) {
        for (int x = 0; x < WC_W; x += 2) {
            uint8_t i0 = (uint8_t)(a[x] + cyc), i1 = (uint8_t)(a[x + 1] + cyc);
            *out++ = (uint32_t)py[i0] | (uint32_t)pu[i0] << 8 | (uint32_t)py[i1] << 16 | (uint32_t)pv[i0] << 24;
        }
    } else {
        for (int x = 0; x < WC_W; x += 2) {
            uint8_t i0 = (uint8_t)(a[x] + cyc), i1 = (uint8_t)(a[x + 1] + cyc);
            int y0 = py[i0], y1 = py[i1];
            int k0 = sa[x] * k >> 8, k1 = sa[x + 1] * k >> 8;
            if (k0 < 255) {
                y0 = 16 + ((y0 - 16) * k0 >> 8);
            }
            if (k1 < 255) {
                y1 = 16 + ((y1 - 16) * k1 >> 8);
            }
            *out++ = (uint32_t)y0 | (uint32_t)pu[i0] << 8 | (uint32_t)y1 << 16 | (uint32_t)pv[i0] << 24;
        }
    }

    /* The text overlay: white on the picture, neutral colour for the pair. */
    if (v->text.on && y >= WC_TEXT_Y && y < WC_TEXT_Y + 16) {
        int row = (y - WC_TEXT_Y) >> 1;
        for (int x = WC_TEXT_X & ~1; x < WC_W; x += 2) {
            uint8_t *p = yuy2 + x * 2;
            bool b0 = text_bit(v, row, x), b1 = text_bit(v, row, x + 1);
            if (b0 || b1) {
                if (b0) {
                    p[0] = 235;
                }
                if (b1) {
                    p[2] = 235;
                }
                p[1] = p[3] = 128;
            }
        }
    }
}

const char *wc_note_name(int n, char *buf)
{
    static const char *const names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    if (n < 0 || n > 127) {
        buf[0] = '\0';
        return buf;
    }
    snprintf(buf, 5, "%s%d", names[n % 12], n / 12 - 1);
    return buf;
}
