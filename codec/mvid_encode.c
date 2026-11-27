/* SPDX-License-Identifier: Apache-2.0 — MVID v1 encoder.
 *
 * Turns RGB565 frames into an 8x8 tile atlas + tilemap + palette, then
 * delta-codes the map. The atlas is LRU so it survives panning and fades;
 * a frame becomes INTRA when the map changes too much. Pan shots bypass
 * pixel work entirely: the source map is emitted once and each frame is a
 * scroll (MVID_FRAME_PAN), which is why a 12-second pan costs ~40 bytes per
 * frame instead of 40 kilobytes.
 */
#include "mvid.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CELLS (MVID_MAX_MAP * MVID_MAX_MAP)

struct mvid_enc {
    mvid_enc_opts_t o;
    unsigned tb;                 /* tile bytes */
    unsigned lw, lh;             /* logical map dims */
    unsigned cells;
    uint16_t pal[256];
    int pal_n, sc;               /* palette entries / 4bpp slots */
    uint16_t pal_cur[256];
    int pal_cur_n;
    int pal_dirty;
    uint8_t  lut[32768];         /* 5:5:5 -> palette index */
    uint8_t  mt[128][8];         /* palette idx -> entry within slot */
    uint32_t dt[128][8];         /* palette idx -> distance to slot */
    uint8_t *sheet;
    uint8_t *tslot;              /* palette slot per atlas tile */
    uint32_t *use;               /* frame stamp per atlas tile */
    int used;
    uint16_t *map, *prev;
    uint8_t *tilebuf;            /* MAX_CELLS * 64 quantised tiles */
    int32_t *cellidx;            /* per-cell atlas match / new-tile ref */
    uint8_t *needed;             /* atlas slots this frame references   */
    int     *reps;               /* representative cell per new tile    */
    int force_intra;
    int pan_ready;
    const uint8_t *src_tiles; int src_ntiles;
    const uint16_t *src_map; int src_w, src_h;
    /* output */
    uint8_t *buf; size_t cap, len;
    uint32_t *frm; int nframes;
    uint16_t pal0[256]; int pal0_n;
};

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1u);
    return p;
}

static inline unsigned r5(unsigned c) { return (c >> 11) & 31u; }
static inline unsigned g6(unsigned c) { return (c >> 5) & 63u; }
static inline unsigned b5(unsigned c) { return c & 31u; }

static uint32_t dist565(uint16_t a, uint16_t b)
{
    const int dr = (int)r5(a) - (int)r5(b);
    const int dg = (int)g6(a) - (int)g6(b);
    const int db = (int)b5(a) - (int)b5(b);
    return (uint32_t)(dr * dr * 4 + dg * dg + db * db * 4);
}

/* ---------------- palette ---------------- */
typedef struct { int lo[3], hi[3]; uint32_t count; } box_t;

static int hist_bin(uint16_t c)
{
    return (int)((r5(c) << 10) | ((g6(c) >> 1) << 5) | b5(c));
}
static uint16_t bin_rgb(int bin)
{
    const unsigned r = (unsigned)(bin >> 10) & 31u;
    const unsigned g = ((unsigned)(bin >> 5) & 31u) << 1;
    const unsigned b = (unsigned)bin & 31u;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static uint16_t box_mean(const uint32_t *hist, const box_t *b)
{
    uint64_t sr = 0, sg = 0, sb = 0, n = 0;
    for (int r = b->lo[0]; r <= b->hi[0]; r++)
        for (int g = b->lo[1]; g <= b->hi[1]; g++)
            for (int bl = b->lo[2]; bl <= b->hi[2]; bl++) {
                const int bin = (r << 10) | (g << 5) | bl;
                const uint32_t c = hist[bin];
                if (!c) continue;
                sr += (uint64_t)c * (uint64_t)r * 255u / 31u;
                sg += (uint64_t)c * (uint64_t)g * 255u / 31u;
                sb += (uint64_t)c * (uint64_t)bl * 255u / 31u;
                n += c;
            }
    if (!n) return 0;
    const unsigned R = (unsigned)(sr / n), G = (unsigned)(sg / n), B = (unsigned)(sb / n);
    return (uint16_t)(((R * 31u / 255u) << 11) | ((G * 63u / 255u) << 5) | (B * 31u / 255u));
}

int mvid_palette_build(const uint16_t *frames, size_t n_frames,
                       size_t w, size_t h, int bpp, uint16_t *pal, int *n_out)
{
    if (!frames || !w || !h || !pal) return MVID_E_PARAM;
    const int want = bpp == 8 ? 256 : 128;
    uint32_t *hist = (uint32_t *)calloc(32768, sizeof(uint32_t));
    box_t *boxes = (box_t *)calloc((size_t)want, sizeof(box_t));
    if (!hist || !boxes) { free(hist); free(boxes); return MVID_E_MEM; }
    for (size_t f = 0; f < n_frames; f++) {
        const uint16_t *fp = frames + f * w * h;
        for (size_t i = 0; i < w * h; i++) hist[hist_bin(fp[i])]++;
    }
    boxes[0].lo[0] = boxes[0].lo[1] = boxes[0].lo[2] = 0;
    boxes[0].hi[0] = boxes[0].hi[1] = boxes[0].hi[2] = 31;
    for (size_t i = 0; i < 32768; i++) boxes[0].count += hist[i];
    int nboxes = 1;
    while (nboxes < want) {
        int best = -1; int best_axis = -1; int best_range = 0;
        for (int i = 0; i < nboxes; i++) {
            if (boxes[i].count < 2) continue;
            for (int a = 0; a < 3; a++) {
                const int range = boxes[i].hi[a] - boxes[i].lo[a];
                if (range > best_range) {
                    /* prefer boxes carrying more pixels when ranges tie */
                    if (best < 0 || boxes[i].count * (unsigned)range > boxes[best].count * (unsigned)best_range) {
                        best = i; best_axis = a; best_range = range;
                    }
                }
            }
        }
        if (best < 0 || best_axis < 0 || best_range <= 0) break;
        box_t *b = &boxes[best];
        /* median split along best_axis using a 32-level histogram */
        uint32_t level[33]; memset(level, 0, sizeof(level));
        for (int r = b->lo[0]; r <= b->hi[0]; r++)
            for (int g = b->lo[1]; g <= b->hi[1]; g++)
                for (int bl = b->lo[2]; bl <= b->hi[2]; bl++) {
                    const int bin = (r << 10) | (g << 5) | bl;
                    const uint32_t c = hist[bin];
                    if (!c) continue;
                    const int v = best_axis == 0 ? r : best_axis == 1 ? g : bl;
                    level[v] += c;
                }
        uint64_t acc = 0; int split = b->lo[best_axis];
        for (int v = b->lo[best_axis]; v <= b->hi[best_axis]; v++) {
            acc += level[v];
            if (acc * 2u >= b->count) { split = v; break; }
        }
        if (split < b->lo[best_axis]) split = b->lo[best_axis];
        if (split >= b->hi[best_axis]) split = b->hi[best_axis] - 1;
        box_t nb = *b;
        boxes[best].hi[best_axis] = split;
        nb.lo[best_axis] = split + 1;
        uint32_t c0 = 0, c1 = 0;
        for (int r = nb.lo[0]; r <= nb.hi[0]; r++)
            for (int g = nb.lo[1]; g <= nb.hi[1]; g++)
                for (int bl = nb.lo[2]; bl <= nb.hi[2]; bl++) {
                    const int bin = (r << 10) | (g << 5) | bl;
                    c1 += hist[bin];
                }
        c0 = b->count > c1 ? b->count - c1 : 0;
        boxes[best].count = c0;
        nb.count = c1;
        boxes[nboxes++] = nb;
    }
    int n = 0;
    for (int i = 0; i < nboxes && n < want; i++) {
        const uint16_t c = box_mean(hist, &boxes[i]);
        int dup = 0;
        for (int k = 0; k < n; k++) if (pal[k] == c) { dup = 1; break; }
        if (!dup) pal[n++] = c;
    }
    while (n < 2) pal[n++] = 0;
    /* pad to the slot multiple so 4bpp slot maths is uniform */
    if (bpp == 4) while (n & 15) pal[n++] = 0;
    if (n_out) *n_out = n;
    free(hist); free(boxes);
    return MVID_E_OK;
}

/* ---------------- encoder ---------------- */
static int build_palette_tables(mvid_enc_t *e)
{
    e->pal_n = e->o.pal_count;
    if (e->pal_n < 1) return MVID_E_PARAM;
    if (e->o.bpp == 4) {
        if (e->pal_n > 128) e->pal_n = 128;
        e->sc = (e->pal_n + 15) / 16;
        if (e->sc < 1) e->sc = 1;
        if (e->sc > 8) e->sc = 8;
    } else {
        if (e->pal_n > 256) e->pal_n = 256;
        e->sc = 1;
    }
    memcpy(e->pal, e->o.pal, (size_t)e->pal_n * sizeof(uint16_t));
    if (e->pal_n == 1) { e->pal[1] = 0; e->pal_n = 2; }

    /* nearest palette entry per 5:5:5 bin */
    for (int bin = 0; bin < 32768; bin++) {
        const uint16_t rgb = bin_rgb(bin);
        uint32_t best = 0xFFFFFFFFu; int bi = 0;
        for (int i = 0; i < e->pal_n; i++) {
            const uint32_t d = dist565(rgb, e->pal[i]);
            if (d < best) { best = d; bi = i; }
        }
        e->lut[bin] = (uint8_t)bi;
    }
    if (e->o.bpp == 4) {
        for (int i = 0; i < e->pal_n; i++) {
            for (int s = 0; s < e->sc; s++) {
                uint32_t best = 0xFFFFFFFFu; int bi = 0;
                for (int k = 0; k < 16; k++) {
                    const int idx = s * 16 + k;
                    if (idx >= e->pal_n) break;
                    const uint32_t d = dist565(e->pal[i], e->pal[idx]);
                    if (d < best) { best = d; bi = k; }
                }
                e->mt[i][s] = (uint8_t)bi;
                e->dt[i][s] = best;
            }
        }
    }
    return MVID_E_OK;
}

mvid_enc_t *mvid_enc_new(const mvid_enc_opts_t *o)
{
    if (!o || !o->pal || o->pal_count < 1) return NULL;
    if ((o->width != 320 || o->height != 240) && (o->width != 160 || o->height != 120))
        return NULL;
    if (o->bpp != 4 && o->bpp != 8) return NULL;
    mvid_enc_t *e = (mvid_enc_t *)calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->o = *o;
    e->tb = o->bpp == 8 ? 64u : 32u;
    e->lw = o->map_w ? o->map_w : (unsigned)(o->width / 8);
    e->lh = o->map_h ? o->map_h : (unsigned)(o->height / 8);
    if (e->lw > MVID_MAX_MAP) e->lw = MVID_MAX_MAP;
    if (e->lh > MVID_MAX_MAP) e->lh = MVID_MAX_MAP;
    e->cells = e->lw * e->lh;
    e->sheet = (uint8_t *)calloc(MVID_MAX_TILES, MVID_TILE_MAX);
    e->tslot = (uint8_t *)calloc(MVID_MAX_TILES, 1);
    e->use = (uint32_t *)calloc(MVID_MAX_TILES, sizeof(uint32_t));
    e->map = (uint16_t *)calloc(MAX_CELLS, sizeof(uint16_t));
    e->prev = (uint16_t *)calloc(MAX_CELLS, sizeof(uint16_t));
    e->tilebuf = (uint8_t *)calloc(MAX_CELLS, 64);
    e->cellidx = (int32_t *)calloc(MAX_CELLS, sizeof(int32_t));
    e->needed = (uint8_t *)calloc(MVID_MAX_TILES, 1);
    e->reps = (int *)calloc(MAX_CELLS, sizeof(int));
    e->frm = (uint32_t *)calloc(MVID_MAX_FRAMES, sizeof(uint32_t));
    e->cap = 1u << 20;
    e->buf = (uint8_t *)xmalloc(e->cap);
    if (!e->sheet || !e->tslot || !e->use || !e->map || !e->prev ||
        !e->tilebuf || !e->cellidx || !e->needed || !e->reps ||
        !e->frm || !e->buf) { mvid_enc_free(e); return NULL; }
    for (unsigned i = 0; i < MAX_CELLS; i++) e->prev[i] = 0xFFFFu;
    if (build_palette_tables(e) != MVID_E_OK) { mvid_enc_free(e); return NULL; }
    e->pal_cur_n = e->pal_n;
    memcpy(e->pal_cur, e->pal, (size_t)e->pal_n * 2u);
    e->force_intra = 1;
    return e;
}

void mvid_enc_free(mvid_enc_t *e)
{
    if (!e) return;
    free(e->sheet); free(e->tslot); free(e->use); free(e->map);
    free(e->prev); free(e->tilebuf); free(e->cellidx); free(e->needed);
    free(e->reps); free(e->frm); free(e->buf);
    free(e);
}

int mvid_enc_frames(const mvid_enc_t *e) { return e ? e->nframes : 0; }

int mvid_enc_palette(mvid_enc_t *e, const uint16_t *pal, int n)
{
    if (!e || !pal || n < 1) return MVID_E_PARAM;
    mvid_enc_opts_t save = e->o;
    e->o.pal = pal;
    e->o.pal_count = (uint16_t)(n > 256 ? 256 : n);
    int rc = build_palette_tables(e);
    e->o = save;
    if (rc != MVID_E_OK) return rc;
    e->force_intra = 1;
    return MVID_E_OK;
}

int mvid_enc_source_map(mvid_enc_t *e, const uint8_t *tiles, int n_tiles,
                        const uint16_t *map, int map_w, int map_h, int bpp)
{
    if (!e || !tiles || !map || n_tiles < 1 || map_w < 1 || map_h < 1) return MVID_E_PARAM;
    if (bpp != e->o.bpp) return MVID_E_PARAM;
    if (n_tiles > MVID_MAX_TILES || map_w > MVID_MAX_MAP || map_h > MVID_MAX_MAP)
        return MVID_E_PARAM;
    e->src_tiles = tiles; e->src_ntiles = n_tiles;
    e->src_map = map; e->src_w = map_w; e->src_h = map_h;
    e->lw = (unsigned)map_w; e->lh = (unsigned)map_h;
    e->cells = e->lw * e->lh;
    e->pan_ready = 1;
    return MVID_E_OK;
}

/* growable append with 16-byte header reserved by the caller */
static uint8_t *reserve(mvid_enc_t *e, size_t n)
{
    if (e->nframes >= MVID_MAX_FRAMES) return NULL;
    if (e->len + n > e->cap) {
        size_t cap = e->cap;
        while (e->len + n > cap) cap <<= 1;
        uint8_t *nb = (uint8_t *)realloc(e->buf, cap);
        if (!nb) return NULL;
        e->buf = nb; e->cap = cap;
    }
    uint8_t *p = e->buf + e->len;
    e->len += n;
    return p;
}

static void write_header(uint8_t *h, unsigned type, unsigned len,
                         int16_t sx, int16_t sy, unsigned ops,
                         unsigned tiles, uint8_t bright)
{
    memset(h, 0, 16);
    h[0] = (uint8_t)type;
    mvid_wr16(h + 2, (uint16_t)len);
    mvid_wr16(h + 4, (uint16_t)sx);
    mvid_wr16(h + 6, (uint16_t)sy);
    mvid_wr16(h + 8, (uint16_t)ops);
    mvid_wr16(h + 10, (uint16_t)tiles);
    h[12] = bright;
}

/* Same, but tile slots are explicit (used by INTRA and pan mode). */
static int emit_frame_slots(mvid_enc_t *e, unsigned type, int16_t sx, int16_t sy,
                            uint8_t bright, const uint8_t *pal_bytes, unsigned np,
                            const uint16_t *slots, const uint8_t *tiles_blob,
                            unsigned nt, size_t tstride,
                            const uint16_t *cells, const uint16_t *entries, unsigned no)
{
    const size_t plen = 6u + (size_t)np * 3u + (size_t)nt * (2u + e->tb) + (size_t)no * 4u;
    uint8_t *p = reserve(e, 16u + plen);
    if (!p) return MVID_E_FULL;
    e->frm[e->nframes++] = (uint32_t)(p - e->buf);
    write_header(p, type, (unsigned)plen, sx, sy, no, nt, bright);
    uint8_t *q = p + 16;
    mvid_wr16(q, (uint16_t)np); q += 2;
    if (np) { memcpy(q, pal_bytes, (size_t)np * 3u); q += (size_t)np * 3u; }
    mvid_wr16(q, (uint16_t)nt); q += 2;
    for (unsigned i = 0; i < nt; i++) {
        mvid_wr16(q, slots[i]); q += 2;
        memcpy(q, tiles_blob + (size_t)i * tstride, e->tb);
        q += e->tb;
    }
    mvid_wr16(q, (uint16_t)no); q += 2;
    for (unsigned i = 0; i < no; i++) {
        mvid_wr16(q, cells[i]); mvid_wr16(q + 2, entries[i]); q += 4;
    }
    return MVID_E_OK;
}

static void palette_diff(mvid_enc_t *e, uint8_t *out, unsigned *np)
{
    unsigned n = 0;
    for (int i = 0; i < e->pal_n; i++) {
        if (i >= e->pal_cur_n || e->pal_cur[i] != e->pal[i]) {
            out[n * 3] = (uint8_t)i;
            mvid_wr16(out + n * 3 + 1, e->pal[i]);
            n++;
        }
    }
    if (n || e->pal_cur_n != e->pal_n) {
        if (!n && e->pal_n) {                    /* keep the header contract */
            out[0] = 0; mvid_wr16(out + 1, e->pal[0]); n = 1;
        }
        memcpy(e->pal_cur, e->pal, (size_t)e->pal_n * 2u);
        e->pal_cur_n = e->pal_n;
    }
    *np = n;
}

/* Quantise one 8x8 tile into e->tilebuf[cell*64] and return its slot. */
static int quant_cell(mvid_enc_t *e, const uint16_t *rgb, unsigned stride,
                      unsigned cell, int16_t scroll_x, int16_t scroll_y)
{
    const unsigned tx = cell % e->lw, ty = cell / e->lw;
    uint8_t *t = e->tilebuf + (size_t)cell * 64u;
    uint8_t gi[64];
    memset(gi, 0, sizeof(gi));
    for (unsigned y = 0; y < 8; y++) {
        for (unsigned x = 0; x < 8; x++) {
            int sx = (int)(tx * 8 + x) + scroll_x;
            int sy = (int)(ty * 8 + y) + scroll_y;
            const unsigned w = e->o.width, h = e->o.height;
            sx %= (int)w; if (sx < 0) sx += (int)w;
            sy %= (int)h; if (sy < 0) sy += (int)h;
            gi[y * 8 + x] = e->lut[hist_bin(rgb[(size_t)sy * stride + (size_t)sx])];
        }
    }
    if (e->o.bpp == 8) {
        memcpy(t, gi, 64);
        return 0;
    }
    uint32_t hist[128]; memset(hist, 0, sizeof(hist));
    for (int i = 0; i < 64; i++) hist[gi[i]]++;
    int slot = 0; uint64_t best = ~(uint64_t)0;
    for (int s = 0; s < e->sc; s++) {
        uint64_t cost = 0;
        for (int i = 0; i < e->pal_n; i++) if (hist[i]) cost += (uint64_t)hist[i] * e->dt[i][s];
        if (cost < best) { best = cost; slot = s; }
    }
    memset(t, 0, 32);
    for (unsigned y = 0; y < 8; y++)
        for (unsigned x = 0; x < 8; x++) {
            const unsigned px = e->mt[gi[y * 8 + x]][slot] & 15u;
            t[y * 4 + (x >> 1)] |= (uint8_t)(px << ((x & 1) ? 4 : 0));
        }
    return slot;
}

static int atlas_find(mvid_enc_t *e, const uint8_t *t, int slot)
{
    for (int i = 0; i < e->used; i++)
        if (e->tslot[i] == (uint8_t)slot && !memcmp(e->sheet + (size_t)i * e->tb, t, e->tb))
            return i;
    return -1;
}

static int enc_frame(mvid_enc_t *e, const uint16_t *rgb, int16_t sx, int16_t sy,
                     uint8_t bright)
{
    if (e->nframes >= MVID_MAX_FRAMES - 1) return MVID_E_FULL;
    const unsigned frame_stamp = (unsigned)e->nframes + 1u;
    const int periodic = e->o.keyframe > 0 && e->nframes > 0 &&
                         (e->nframes % e->o.keyframe) == 0;
    int intra = e->force_intra || e->nframes == 0 || periodic;

    if (intra) {
        memset(e->sheet, 0, (size_t)MVID_MAX_TILES * e->tb);
        memset(e->use, 0, MVID_MAX_TILES * sizeof(uint32_t));
        e->used = 0;
        for (unsigned i = 0; i < e->cells; i++) e->prev[i] = 0xFFFFu;
    }
    /* pass 1: quantise every cell */
    int slots[MAX_CELLS];
    for (unsigned c = 0; c < e->cells; c++)
        slots[c] = quant_cell(e, rgb, e->o.width, c, sx, sy);
    /* pass 2: match what the atlas already holds (no mutation yet) */
    for (unsigned c = 0; c < e->cells; c++)
        e->cellidx[c] = atlas_find(e, e->tilebuf + (size_t)c * 64u, slots[c]);
    memset(e->needed, 0, MVID_MAX_TILES);
    for (unsigned c = 0; c < e->cells; c++)
        if (e->cellidx[c] >= 0) e->needed[e->cellidx[c]] = 1;

    /* pass 3: dedupe the new tiles by content, remembering one cell each */
    int nrep = 0;
    for (unsigned c = 0; c < e->cells; c++) {
        if (e->cellidx[c] >= 0) continue;
        int found = -1;
        for (int k = 0; k < nrep; k++) {
            const unsigned rc = (unsigned)e->reps[k];
            if (slots[rc] == slots[c] &&
                !memcmp(e->tilebuf + (size_t)rc * 64u, e->tilebuf + (size_t)c * 64u, e->tb)) {
                found = k; break;
            }
        }
        if (found < 0) { e->reps[nrep] = (int)c; found = nrep++; }
        e->cellidx[c] = -(2 + found);
    }

    /* pass 4: make room by trimming the tail. A trimmed slot is neither
     * referenced by this frame nor by an index already assigned above, so
     * nothing built so far can be invalidated (the old code could, which
     * scrambled saturated frames). */
    while ((int)e->used + nrep > (int)MVID_MAX_TILES && e->used > 0 && !e->needed[e->used - 1])
        e->used--;

    /* pass 5: append the new tiles that fit */
    const int base = e->used;
    int fit = 0;
    for (int k = 0; k < nrep; k++) {
        if (base + k >= (int)MVID_MAX_TILES) break;
        const unsigned rc = (unsigned)e->reps[k];
        memcpy(e->sheet + (size_t)(base + k) * e->tb, e->tilebuf + (size_t)rc * 64u, e->tb);
        e->tslot[base + k] = (uint8_t)slots[rc];
        e->use[base + k] = frame_stamp;
        fit++;
    }
    e->used = base + fit;

    /* pass 6: resolve cells to final atlas indices */
    for (unsigned c = 0; c < e->cells; c++) {
        int idx = e->cellidx[c];
        if (idx >= 0) {
            e->use[idx] = frame_stamp;
        } else {
            const int k = -(idx + 2);
            idx = (k < fit) ? base + k : 0;
            if (idx < (int)e->used) e->use[idx] = frame_stamp;
        }
        e->map[c] = (uint16_t)(idx | (slots[c] << 10));
    }
    /* changed cells */
    static uint16_t ch_cell[MAX_CELLS], ch_entry[MAX_CELLS];
    unsigned nch = 0;
    for (unsigned c = 0; c < e->cells; c++)
        if (e->map[c] != e->prev[c]) { ch_cell[nch] = (uint16_t)c; ch_entry[nch] = e->map[c]; nch++; }
    if (!intra && nch * 100u > e->cells * 60u) intra = 1;

    if (intra) {
        /* emit the whole map plus every referenced tile */
        static uint8_t used_flag[MVID_MAX_TILES];
        static uint16_t slots_out[MVID_MAX_TILES];
        static uint8_t blob[MVID_MAX_TILES * MVID_TILE_MAX];
        memset(used_flag, 0, sizeof(used_flag));
        for (unsigned c = 0; c < e->cells; c++) used_flag[e->map[c] & 0x3FF] = 1;
        unsigned nt = 0;
        for (int i = 0; i < e->used; i++) if (used_flag[i]) {
            slots_out[nt] = (uint16_t)i;
            memcpy(blob + (size_t)nt * e->tb, e->sheet + (size_t)i * e->tb, e->tb);
            nt++;
        }
        /* tile indices are the atlas indices, so the blob stride must match */
        unsigned no = 0;
        for (unsigned c = 0; c < e->cells; c++) { ch_cell[no] = (uint16_t)c; ch_entry[no] = e->map[c]; no++; }
        uint8_t palb[256 * 3]; unsigned np = 0;
        palette_diff(e, palb, &np);
        int rc = emit_frame_slots(e, MVID_FRAME_INTRA, sx, sy, bright, palb, np,
                                  slots_out, blob, nt, e->tb, ch_cell, ch_entry, no);
        if (rc != MVID_E_OK) return rc;
    } else if (nch) {
        static uint8_t blob[MVID_MAX_TILES * MVID_TILE_MAX];
        static uint16_t slots_out[MVID_MAX_TILES];
        unsigned nt = 0;
        for (unsigned c = 0; c < nch && nt < MVID_MAX_TILES; c++) {
            const int idx = ch_entry[c] & 0x3FF;
            if (e->use[idx] == frame_stamp) {
                /* new tile for this frame: emit once */
                int seen = 0;
                for (unsigned k = 0; k < nt; k++) if (slots_out[k] == (uint16_t)idx) { seen = 1; break; }
                if (!seen) {
                    slots_out[nt] = (uint16_t)idx;
                    memcpy(blob + (size_t)nt * e->tb, e->sheet + (size_t)idx * e->tb, e->tb);
                    nt++;
                }
            }
        }
        uint8_t palb[256 * 3]; unsigned np = 0;
        palette_diff(e, palb, &np);
        int rc = emit_frame_slots(e, MVID_FRAME_PATCH, sx, sy, bright, palb, np,
                                  slots_out, blob, nt, e->tb, ch_cell, ch_entry, nch);
        if (rc != MVID_E_OK) return rc;
    } else {
        /* identical map: a scroll/brightness-only frame, still a PAN */
        uint8_t palb[256 * 3]; unsigned np = 0;
        palette_diff(e, palb, &np);
        int rc = emit_frame_slots(e, MVID_FRAME_PAN, sx, sy, bright, palb, np,
                                  NULL, NULL, 0, e->tb, NULL, NULL, 0);
        if (rc != MVID_E_OK) return rc;
    }
    memcpy(e->prev, e->map, (size_t)e->cells * sizeof(uint16_t));
    e->force_intra = 0;
    return MVID_E_OK;
}

int mvid_enc_frame(mvid_enc_t *e, const uint16_t *rgb,
                   int16_t scroll_x, int16_t scroll_y, uint8_t brightness)
{
    if (!e || !rgb) return MVID_E_PARAM;
    e->pan_ready = 0;
    /* rendered frames always quantise on the clip's declared map grid */
    e->lw = e->o.map_w ? e->o.map_w : (unsigned)(e->o.width / 8u);
    e->lh = e->o.map_h ? e->o.map_h : (unsigned)(e->o.height / 8u);
    e->cells = e->lw * e->lh;
    if (brightness > 16) brightness = 16;
    return enc_frame(e, rgb, scroll_x, scroll_y, brightness);
}

int mvid_enc_pan(mvid_enc_t *e, int16_t scroll_x, int16_t scroll_y, uint8_t brightness)
{
    if (!e || !e->pan_ready || !e->src_map) return MVID_E_PARAM;
    if (brightness > 16) brightness = 16;
    if (e->nframes >= MVID_MAX_FRAMES - 1) return MVID_E_FULL;
    static uint16_t cells[MAX_CELLS], entries[MAX_CELLS], slots[MVID_MAX_TILES];
    uint8_t palb[256 * 3]; unsigned np = 0;

    if (e->nframes == 0 || e->force_intra) {
        const unsigned no = (unsigned)(e->src_w * e->src_h);
        for (unsigned i = 0; i < no; i++) {
            cells[i] = (uint16_t)i;
            entries[i] = e->src_map[i];
        }
        for (int i = 0; i < e->src_ntiles; i++) slots[i] = (uint16_t)i;
        palette_diff(e, palb, &np);
        int rc = emit_frame_slots(e, MVID_FRAME_INTRA, scroll_x, scroll_y, brightness,
                                  palb, np, slots, e->src_tiles,
                                  (unsigned)e->src_ntiles, e->tb, cells, entries, no);
        if (rc != MVID_E_OK) return rc;
        e->force_intra = 0;
        return MVID_E_OK;
    }
    palette_diff(e, palb, &np);
    return emit_frame_slots(e, MVID_FRAME_PAN, scroll_x, scroll_y, brightness,
                            palb, np, NULL, NULL, 0, e->tb, NULL, NULL, 0);
}

/* ---------------- container writer ---------------- */
int mvid_enc_write(mvid_enc_t *e, const char *path)
{
    if (!e || !path || e->nframes < 1) return MVID_E_PARAM;
    const unsigned mw = e->o.map_w ? e->o.map_w : e->lw;
    const unsigned mh = e->o.map_h ? e->o.map_h : e->lh;
    const size_t pal_bytes = (size_t)e->pal_cur_n * 2u;
    const size_t total = MVID_HDR + pal_bytes + e->len +
                         (size_t)e->nframes * 4u;
    uint8_t *out = (uint8_t *)malloc(total);
    if (!out) return MVID_E_MEM;
    memset(out, 0, MVID_HDR);
    memcpy(out, "MVID", 4);
    mvid_wr16(out + 4, MVID_VERSION);
    mvid_wr16(out + 6, MVID_HDR);
    mvid_wr16(out + 8, e->o.width);
    mvid_wr16(out + 10, e->o.height);
    out[12] = e->o.fps ? e->o.fps : 12;
    out[13] = (uint8_t)((e->o.loop ? MVID_F_LOOP : 0) |
                        (e->o.width == 160 ? MVID_F_QFA : 0));
    mvid_wr16(out + 14, (uint16_t)e->nframes);
    mvid_wr16(out + 16, (uint16_t)mw);
    mvid_wr16(out + 18, (uint16_t)mh);
    out[20] = e->o.bpp;
    mvid_wr16(out + 22, (uint16_t)e->pal_cur_n);
    mvid_wr32(out + 24, MVID_HDR);
    const size_t frames_off = MVID_HDR + pal_bytes;
    const size_t index_off = frames_off + e->len;
    mvid_wr32(out + 28, (uint32_t)index_off);
    /* sheet_bytes: peak requirement */
    mvid_wr32(out + 40, (uint32_t)(MVID_MAX_TILES * e->tb));
    for (int i = 0; i < e->pal_cur_n; i++) mvid_wr16(out + MVID_HDR + (size_t)i * 2u, e->pal_cur[i]);
    memcpy(out + frames_off, e->buf, e->len);
    for (int f = 0; f < e->nframes; f++)
        mvid_wr32(out + index_off + (size_t)f * 4u, (uint32_t)(frames_off + e->frm[f]));
    mvid_wr32(out + 44, mvid_crc32(out + MVID_HDR, total - MVID_HDR));

    FILE *f = fopen(path, "wb");
    if (!f) { free(out); return MVID_E_IO; }
    const int ok = fwrite(out, 1, total, f) == total;
    fclose(f);
    free(out);
    return ok ? MVID_E_OK : MVID_E_IO;
}
