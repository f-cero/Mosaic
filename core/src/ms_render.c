/* SPDX-License-Identifier: Apache-2.0 */
/* Mosaic — ms_render.c: software PPU. 8x8 tiles, SNES-style maps, OAM
 * sprites, affine layers (per-scanline stepping), colour math, RGB565
 * scanout. Pure integer. */
#include "mosaic/ms_render.h"
#include <string.h>

#define RANK_BACKDROP 9

/* ---- matrices ---------------------------------------------------------- */
ms_matrix_t ms_matrix_identity(void)
{
    ms_matrix_t m = { MS_FIX_ONE, 0, 0, MS_FIX_ONE };
    return m;
}

ms_matrix_t ms_matrix_scale(ms_fix sx, ms_fix sy)
{
    ms_matrix_t m = { sx, 0, 0, sy };
    return m;
}

ms_matrix_t ms_matrix_rot(ms_fix turns)
{
    /* ms_sin/ms_cos reach full scale at a quarter turn but not 16.16's 1.0, so
     * normalise against that peak: a rotation must compose with the same unit
     * as scale/shear or a rotate-after-scale would silently shrink. */
    const ms_fix peak = ms_sin(MS_FIX_ONE / 4);
    ms_fix c, s;
    ms_matrix_t m;
    if (peak == 0) return ms_matrix_identity();
    c = (ms_fix)(((int64_t)ms_cos(turns) * MS_FIX_ONE) / peak);
    s = (ms_fix)(((int64_t)ms_sin(turns) * MS_FIX_ONE) / peak);
    m.a = c; m.b = -s; m.c = s; m.d = c;
    return m;
}

ms_matrix_t ms_matrix_shear(ms_fix kx, ms_fix ky)
{
    ms_matrix_t m = { MS_FIX_ONE, kx, ky, MS_FIX_ONE };
    return m;
}

/* rhs is applied first: (lhs * rhs) * v == lhs * (rhs * v). */
ms_matrix_t ms_matrix_mul(ms_matrix_t l, ms_matrix_t r)
{
    ms_matrix_t o;
    o.a = (ms_fix)(((int64_t)l.a * r.a + (int64_t)l.b * r.c) >> 16);
    o.b = (ms_fix)(((int64_t)l.a * r.b + (int64_t)l.b * r.d) >> 16);
    o.c = (ms_fix)(((int64_t)l.c * r.a + (int64_t)l.d * r.c) >> 16);
    o.d = (ms_fix)(((int64_t)l.c * r.b + (int64_t)l.d * r.d) >> 16);
    return o;
}

static void ppu_clear(ms_ppu_t *p)
{
    memset(p, 0, sizeof(*p));
    p->fx.brightness = 16;
}

void ms_ppu_reset(ms_ppu_t *ppu, ms_mode_t mode, uint16_t w, uint16_t h)
{
    ppu_clear(ppu);
    ppu->mode = mode;
    ppu->width = w;
    ppu->height = h;
    for (unsigned i = 0; i < MS_MAX_SPRITES; i++) ppu->oam[i].affine = -1;
    for (unsigned i = 0; i < MS_MAX_BG; i++) ppu->bg[i].bpp = 4;
}

/* One tile pixel: packed rows, x<<bpp bits. */
static inline unsigned tile_px(const uint8_t *tiles, unsigned bpp,
                               unsigned tile, unsigned x, unsigned y)
{
    const uint8_t *t = tiles + tile * (bpp == 8 ? 64u : bpp == 4 ? 32u : 16u);
    if (bpp == 8) return t[y * 8 + x];
    if (bpp == 4) {
        uint8_t b = t[y * 4 + (x >> 1)];
        return (x & 1) ? (b >> 4) : (b & 0xF);
    }
    uint8_t b = t[y * 2 + (x >> 2)];
    return (b >> ((x & 3) * 2)) & 3;
}

static int is_affine_mode(ms_mode_t m) { return m == MS_MODE_AFFINE || m == MS_MODE_AFFINE2; }

/* Visible tile dims; 0 means "same as the padded map". */
static inline int bg_lw(const ms_bg_t *bg) { return bg->logical_w ? bg->logical_w : bg->map_w; }
static inline int bg_lh(const ms_bg_t *bg) { return bg->logical_h ? bg->logical_h : bg->map_h; }

/* Palette lookup for a map entry, shared by the tile and affine paths so the
 * two cannot drift apart (4bpp: 16-colour slot, 2bpp: 4-colour slot). */
static inline uint16_t map_colour(const ms_ppu_t *ppu, const ms_bg_t *bg,
                                  uint16_t e, unsigned idx)
{
    const unsigned slot = (e >> 10) & 7;
    if (bg->bpp == 8) return ppu->pal_bg[idx & 0xFF];
    if (bg->bpp == 4) return ppu->pal_bg[((slot & 0xF) * 16 + idx) & 0xFF];
    return ppu->pal_bg[((slot & 0x3F) * 4 + idx) & 0xFF];
}

uint16_t ms_color_math(uint16_t under, uint16_t over, uint8_t mode, ms_fix factor)
{
    if (factor <= 0) return over;
    if (factor > MS_FIX_ONE) factor = MS_FIX_ONE;
    const int ro = (over >> 11) & 31, go = (over >> 5) & 63, bo = over & 31;
    const int ru = (under >> 11) & 31, gu = (under >> 5) & 63, bu = under & 31;
    int r, g, b;
    switch (mode & 3) {
    case 0:  r = ro + ru;        g = go + gu;        b = bo + bu;        break;
    case 1:  r = ro - ru;        g = go - gu;        b = bo - bu;        break;
    case 2:  r = (ro + ru) >> 1; g = (go + gu) >> 1; b = (bo + bu) >> 1; break;
    default: r = (ro - ru) >> 1; g = (go - gu) >> 1; b = (bo - bu) >> 1; break;
    }
    r = ro + (int)(((int64_t)(r - ro) * factor) >> 16);
    g = go + (int)(((int64_t)(g - go) * factor) >> 16);
    b = bo + (int)(((int64_t)(b - bo) * factor) >> 16);
    if (r < 0) r = 0; else if (r > 31) r = 31;
    if (g < 0) g = 0; else if (g > 63) g = 63;
    if (b < 0) b = 0; else if (b > 31) b = 31;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* Composite one finished pixel. Colour math blends this layer's colour with
 * whatever was already on that pixel (backdrop or a lower layer). */
static inline void put_pixel(const ms_ppu_t *ppu, int math_on, uint16_t c, int x,
                             uint16_t *color, uint8_t *rank, uint8_t rk)
{
    if (rk >= rank[x]) return;
    if (math_on && ppu->fx.math_on)
        c = ms_color_math(color[x], c, ppu->fx.color_math, ppu->fx.math_factor);
    rank[x] = rk;
    color[x] = c;
}

/* Draw one tile background scanline (or an affine layer when layer is
 * affine). rank[] carries the compositing order; lower is in front. */
static void bg_scanline(const ms_ppu_t *ppu, int layer, int y,
                        uint16_t *color, uint8_t *rank, int w)
{
    const ms_bg_t *bg = &ppu->bg[layer];
    const uint8_t rank_bg = (uint8_t)(bg->priority + 4);
    if (!bg->enabled) return;

    int scroll_x = bg->scroll_x, scroll_y = bg->scroll_y;
    if (bg->line_scroll_x) scroll_x += bg->line_scroll_x[y];
    if (bg->line_scroll_y) scroll_y += bg->line_scroll_y[y];

    if (is_affine_mode(ppu->mode) && layer < (int)MS_MAX_AFFINE) {
        const ms_matrix_t *m = &ppu->mat[layer];
        if (!bg->map || !bg->tiles) return;
        const int mw = bg_lw(bg) * 8, mh = bg_lh(bg) * 8;
        if (mw <= 0 || mh <= 0) return;
        const int cy = y - ppu->height / 2;
        /* Per-scanline stepping: scroll_x/scroll_y above already fold in this
         * line's raster-table entry, so one matrix plus a per-line offset
         * shears or parallaxes down the screen (Mode-7 style). */
        for (int x = 0; x < w; x++) {
            const int cx = x - ppu->width / 2;
            const int64_t u = ((int64_t)m->a * cx + (int64_t)m->b * cy) >> 16;
            const int64_t v = ((int64_t)m->c * cx + (int64_t)m->d * cy) >> 16;
            int sx = (int)((u + scroll_x) % mw); if (sx < 0) sx += mw;
            int sy = (int)((v + scroll_y) % mh); if (sy < 0) sy += mh;
            const unsigned mt = (unsigned)((sy >> 3) * bg->map_w + (sx >> 3));
            const uint16_t e = bg->map[mt];
            const unsigned idx = tile_px(bg->tiles, bg->bpp, e & 0x3FF,
                                        (unsigned)(sx & 7), (unsigned)(sy & 7));
            if (!idx) continue;
            put_pixel(ppu, bg->math, map_colour(ppu, bg, e, idx), x, color, rank, rank_bg);
        }
        return;
    }

    if (!bg->map || !bg->tiles || bg->map_w == 0 || bg->map_h == 0) return;
    const int lw = bg_lw(bg), lh = bg_lh(bg);
    const int mask_x = bg->map_w - 1, mask_y = bg->map_h - 1;
    const int pow_x = (lw & (lw - 1)) == 0, pow_y = (lh & (lh - 1)) == 0;
    for (int x = 0; x < w; x++) {
        int sx = x + scroll_x, sy = y + scroll_y;
        if (bg->mosaic > 1) { sx &= ~(bg->mosaic - 1); sy &= ~(bg->mosaic - 1); }
        int tx = (sx >> 3), ty = (sy >> 3);
        if (pow_x) tx &= mask_x; else { tx %= lw; if (tx < 0) tx += lw; }
        if (pow_y) ty &= mask_y; else { ty %= lh; if (ty < 0) ty += lh; }
        const uint16_t e = bg->map[ty * bg->map_w + tx];
        unsigned px = (unsigned)(sx & 7), py = (unsigned)(sy & 7);
        if (e & 0x4000) px = 7 - px;
        if (e & 0x8000) py = 7 - py;
        const unsigned idx = tile_px(bg->tiles, bg->bpp, e & 0x3FF, px, py);
        if (!idx) continue;
        const uint8_t rk = (e & 0x2000) ? (uint8_t)(rank_bg - 1) : rank_bg;
        put_pixel(ppu, bg->math, map_colour(ppu, bg, e, idx), x, color, rank, rk);
    }
}

static void sprite_scanline(const ms_ppu_t *ppu, const ms_oam_t *o, int y,
                            uint16_t *color, uint8_t *rank, int w)
{
    if ((o->flags & MS_OAM_HIDE) || o->h == 0 || o->w == 0) return;
    const int sy = y - o->y;
    if (sy < 0 || sy >= o->h) return;
    const uint8_t *tiles = o->tiles ? o->tiles : ppu->spr_tiles;
    if (!tiles) return;
    const uint8_t rk = o->priority;
    const int wt = o->w >> 3;
    /* Effective sampling matrix for this row: the object matrix first, then the
     * per-scanline table entry. Composed once per row, not per pixel. */
    int has_m = 0;
    ms_matrix_t mm;
    if (o->affine >= 0 && o->affine < (int)MS_MAX_MATRICES) {
        mm = ppu->mat[o->affine];
        has_m = 1;
    }
    if (o->line_mat) {
        const int rows = o->line_rows ? (int)o->line_rows : (int)o->h;
        if (sy < rows) {
            /* Cart tables are blobs at an arbitrary offset: read by memcpy so
             * an unaligned table is still well defined. */
            ms_matrix_t lm;
            memcpy(&lm, (const uint8_t *)o->line_mat + (size_t)sy * sizeof(lm), sizeof(lm));
            mm = has_m ? ms_matrix_mul(mm, lm) : lm;
            has_m = 1;
        }
    }
    int x0 = o->x, x1 = o->x + o->w;
    if (x1 <= 0 || x0 >= w) return;
    if (x0 < 0) x0 = 0;
    if (x1 > w) x1 = w;

    for (int x = x0; x < x1; x++) {
        int px = x - o->x, py = sy;
        if (has_m) {
            int cx = px - o->w / 2, cy = py - o->h / 2;
            px = (int)(((int64_t)mm.a * cx + (int64_t)mm.b * cy) >> 16) + o->w / 2;
            py = (int)(((int64_t)mm.c * cx + (int64_t)mm.d * cy) >> 16) + o->h / 2;
            if (px < 0 || py < 0 || px >= o->w || py >= o->h) continue;
        }
        if (o->flags & MS_OAM_FLIP_H) px = o->w - 1 - px;
        if (o->flags & MS_OAM_FLIP_V) py = o->h - 1 - py;
        unsigned tx = (unsigned)(px >> 3), ty = (unsigned)(py >> 3);
        unsigned tile = ty * (unsigned)wt + tx;
        unsigned idx = tile_px(tiles, o->bpp ? o->bpp : 4, tile, (unsigned)(px & 7), (unsigned)(py & 7));
        if (!idx) continue;
        uint16_t c;
        if ((o->bpp ? o->bpp : 4) == 8) c = ppu->pal_spr[idx & 0xFF];
        else c = ppu->pal_spr[((o->pal_slot & 0xF) * 16 + idx) & 0xFF];
        if (rk <= rank[x]) {
            if ((o->flags & MS_OAM_MATH) && ppu->fx.math_on)
                c = ms_color_math(color[x], c, ppu->fx.color_math, ppu->fx.math_factor);
            rank[x] = rk; color[x] = c;
        }
    }
}

void ms_ppu_render(ms_ppu_t *ppu, uint16_t *fb)
{
    const int w = ppu->width, h = ppu->height;
    uint16_t color[MS_SCREEN_W];
    uint8_t  rank[MS_SCREEN_W];
    uint16_t backdrop = ppu->fx.backdrop;
    if (ppu->fx.brightness < 16) backdrop = ms_rgb565_bright(backdrop, ppu->fx.brightness);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) { color[x] = backdrop; rank[x] = RANK_BACKDROP; }

        bg_scanline(ppu, 1, y, color, rank, w);
        bg_scanline(ppu, 0, y, color, rank, w);

        /* sprites: draw back-to-front so ties keep the higher-priority one */
        for (int pr = 3; pr >= 0; pr--)
            for (uint16_t i = 0; i < ppu->oam_count && i < MS_MAX_SPRITES; i++)
                if ((ppu->oam[i].priority & 3) == (unsigned)pr)
                    sprite_scanline(ppu, &ppu->oam[i], y, color, rank, w);

        uint16_t *row = fb + (size_t)y * w;
        if (ppu->fx.brightness < 16) {
            for (int x = 0; x < w; x++) row[x] = ms_rgb565_bright(color[x], ppu->fx.brightness);
        } else {
            memcpy(row, color, (size_t)w * sizeof(uint16_t));
        }
    }
    ppu->frame++;
}

void ms_ppu_render_qfa(ms_ppu_t *ppu, uint16_t *fb, uint16_t *scratch)
{
    const int w = ppu->width, h = ppu->height;
    ms_ppu_render(ppu, scratch);
    for (int y = 0; y < h; y++) {
        uint16_t *src = scratch + (size_t)y * w;
        uint16_t *d0 = fb + (size_t)(y * 2) * MS_SCREEN_W;
        uint16_t *d1 = d0 + MS_SCREEN_W;
        for (int x = 0; x < w; x++) {
            d0[x * 2] = d0[x * 2 + 1] = src[x];
            d1[x * 2] = d1[x * 2 + 1] = src[x];
        }
    }
}
