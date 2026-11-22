/* SPDX-License-Identifier: Apache-2.0 */
/* Mosaic — ms_render.c: software PPU. 8x8 tiles, SNES-style maps, OAM
 * sprites, affine layers, RGB565 scanout. Pure integer. */
#include "mosaic/ms_render.h"
#include <string.h>

#define RANK_BACKDROP 9

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
        const int mw = bg->map_w * 8, mh = bg->map_h * 8;
        if (mw <= 0 || mh <= 0) return;
        const int cy = y - ppu->height / 2;
        for (int x = 0; x < w; x++) {
            int cx = x - ppu->width / 2;
            int64_t u = ((int64_t)m->a * cx + (int64_t)m->b * cy) >> 16;
            int64_t v = ((int64_t)m->c * cx + (int64_t)m->d * cy) >> 16;
            int sx = (int)((u + scroll_x) % mw); if (sx < 0) sx += mw;
            int sy = (int)((v + scroll_y) % mh); if (sy < 0) sy += mh;
            unsigned mt = (unsigned)((sy >> 3) * bg->map_w + (sx >> 3));
            uint16_t e = bg->map[mt];
            unsigned idx = tile_px(bg->tiles, bg->bpp, e & 0x3FF, (unsigned)(sx & 7), (unsigned)(sy & 7));
            if (!idx) continue;
            if (rank_bg < rank[x]) { rank[x] = rank_bg; color[x] = ppu->pal_bg[idx & 0xFF]; }
        }
        return;
    }

    if (!bg->map || !bg->tiles || bg->map_w == 0 || bg->map_h == 0) return;
    const int mask_x = bg->map_w - 1, mask_y = bg->map_h - 1;
    for (int x = 0; x < w; x++) {
        int sx = x + scroll_x, sy = y + scroll_y;
        if (bg->mosaic > 1) { sx &= ~(bg->mosaic - 1); sy &= ~(bg->mosaic - 1); }
        int tx = (sx >> 3) & mask_x, ty = (sy >> 3) & mask_y;
        uint16_t e = bg->map[ty * bg->map_w + tx];
        unsigned px = (unsigned)(sx & 7), py = (unsigned)(sy & 7);
        if (e & 0x4000) px = 7 - px;
        if (e & 0x8000) py = 7 - py;
        unsigned idx = tile_px(bg->tiles, bg->bpp, e & 0x3FF, px, py);
        if (!idx) continue;
        unsigned pal_slot = (e >> 10) & 7;
        uint16_t c;
        if (bg->bpp == 8) c = ppu->pal_bg[idx & 0xFF];
        else if (bg->bpp == 4) c = ppu->pal_bg[((pal_slot & 0xF) * 16 + idx) & 0xFF];
        else c = ppu->pal_bg[((pal_slot & 0x3F) * 4 + idx) & 0xFF];
        uint8_t rk = (e & 0x2000) ? (uint8_t)(rank_bg - 1) : rank_bg;
        if (rk < rank[x]) { rank[x] = rk; color[x] = c; }
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
    const ms_matrix_t *m = (o->affine >= 0 && o->affine < (int)MS_MAX_MATRICES) ? &ppu->mat[o->affine] : NULL;
    int x0 = o->x, x1 = o->x + o->w;
    if (x1 <= 0 || x0 >= w) return;
    if (x0 < 0) x0 = 0;
    if (x1 > w) x1 = w;

    for (int x = x0; x < x1; x++) {
        int px = x - o->x, py = sy;
        if (m) {
            int cx = px - o->w / 2, cy = py - o->h / 2;
            px = (int)(((int64_t)m->a * cx + (int64_t)m->b * cy) >> 16) + o->w / 2;
            py = (int)(((int64_t)m->c * cx + (int64_t)m->d * cy) >> 16) + o->h / 2;
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
        if (rk <= rank[x]) { rank[x] = rk; color[x] = c; }
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
        for (int x = 0; x < w; x++) { color[x] = ppu->fx.backdrop; rank[x] = RANK_BACKDROP; }

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
