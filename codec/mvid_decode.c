/* SPDX-License-Identifier: Apache-2.0 — MVID v1 decoder. No allocation, no
 * OS dependencies: the caller supplies sheet + map storage. */
#include "mvid.h"
#include <string.h>

size_t mvid_sheet_size(const mvid_info_t *i)
{
    return (size_t)MVID_MAX_TILES * (i->bpp == 8 ? 64u : 32u);
}

size_t mvid_map_entries(const mvid_info_t *i)
{
    return (size_t)mvid_next_pow2(i->map_w) * (size_t)mvid_next_pow2(i->map_h);
}

int mvid_parse(mvid_info_t *o, const void *data, size_t size)
{
    const uint8_t *p = (const uint8_t *)data;
    if (!o || !p) return MVID_E_PARAM;
    memset(o, 0, sizeof(*o));
    if (size < MVID_HDR) return MVID_E_SIZE;
    if (memcmp(p, "MVID", 4) != 0) return MVID_E_MAGIC;
    if (mvid_rd16(p + 4) != MVID_VERSION) return MVID_E_VERSION;
    const unsigned hdr = mvid_rd16(p + 6);
    if (hdr < MVID_HDR || hdr > size) return MVID_E_SIZE;
    o->width = mvid_rd16(p + 8);
    o->height = mvid_rd16(p + 10);
    o->fps = p[12];
    o->flags = p[13];
    o->frames = mvid_rd16(p + 14);
    o->map_w = mvid_rd16(p + 16);
    o->map_h = mvid_rd16(p + 18);
    o->bpp = p[20];
    o->pal_count = mvid_rd16(p + 22);
    o->pal_off = mvid_rd32(p + 24);
    o->index_off = mvid_rd32(p + 28);
    o->audio_off = mvid_rd32(p + 32);
    o->audio_len = mvid_rd32(p + 36);
    o->sheet_bytes = mvid_rd32(p + 40);

    if (!((o->width == 320 && o->height == 240) ||
          (o->width == 160 && o->height == 120))) return MVID_E_SIZE;
    if ((o->flags & (uint8_t)~0x07u) != 0) return MVID_E_SIZE;
    if (o->fps < 1 || o->fps > 60) return MVID_E_SIZE;
    if (o->bpp != 4 && o->bpp != 8) return MVID_E_SIZE;
    if (o->frames < 1 || o->frames > MVID_MAX_FRAMES) return MVID_E_SIZE;
    if (o->map_w < 1 || o->map_h < 1 ||
        o->map_w > MVID_MAX_MAP || o->map_h > MVID_MAX_MAP) return MVID_E_SIZE;
    if (o->map_w * 8u < o->width || o->map_h * 8u < o->height) return MVID_E_SIZE;
    if (o->pal_count < 1 || o->pal_count > 256) return MVID_E_SIZE;
    if ((size_t)o->pal_off + (size_t)o->pal_count * 2u > size) return MVID_E_SIZE;
    if ((size_t)o->index_off + (size_t)o->frames * 4u > size) return MVID_E_SIZE;
    if (o->audio_len && (size_t)o->audio_off + o->audio_len > size) return MVID_E_SIZE;
    if (o->sheet_bytes > (uint32_t)(MVID_MAX_TILES * MVID_TILE_MAX)) return MVID_E_SIZE;
    return MVID_E_OK;
}

/* Payload bodies, in this order: palette diffs, tile defs, map ops. */
static int check_body(const uint8_t *q, const uint8_t *end,
                      const mvid_info_t *i, unsigned *ops_out,
                      unsigned *tiles_out)
{
    const size_t tb = i->bpp == 8 ? 64u : 32u;
    if (end - q < 6) return MVID_E_FRAME;
    unsigned np = mvid_rd16(q); q += 2;
    if ((size_t)(end - q) < (size_t)np * 3u) return MVID_E_FRAME;
    q += (size_t)np * 3u;
    if (end - q < 2) return MVID_E_FRAME;
    unsigned nt = mvid_rd16(q); q += 2;
    if (nt > MVID_MAX_TILES) return MVID_E_FRAME;
    if ((size_t)(end - q) < (size_t)nt * (2u + tb)) return MVID_E_FRAME;
    q += (size_t)nt * (2u + tb);
    if (end - q < 2) return MVID_E_FRAME;
    unsigned no = mvid_rd16(q); q += 2;
    if ((size_t)(end - q) < (size_t)no * 4u) return MVID_E_FRAME;
    q += (size_t)no * 4u;
    if (ops_out) *ops_out = no;
    if (tiles_out) *tiles_out = nt;
    return (q == end) ? MVID_E_OK : MVID_E_FRAME;
}

int mvid_check(const void *data, size_t size)
{
    const uint8_t *p = (const uint8_t *)data;
    mvid_info_t i;
    int rc = mvid_parse(&i, data, size);
    if (rc != MVID_E_OK) return rc;
    if (mvid_rd32(p + 44) != 0 &&
        mvid_crc32(p + i.pal_off, size - i.pal_off) != mvid_rd32(p + 44))
        return MVID_E_CRC;
    for (unsigned f = 0; f < i.frames; f++) {
        uint32_t off = mvid_rd32(p + i.index_off + (size_t)f * 4u);
        if ((size_t)off + 16u > size) return MVID_E_FRAME;
        const unsigned type = p[off];
        if (type > MVID_FRAME_PAN) return MVID_E_FRAME;
        const unsigned len = mvid_rd16(p + off + 2);
        if ((size_t)off + 16u + len > size) return MVID_E_FRAME;
        if (p[off + 12] > 16) return MVID_E_FRAME;
        int brc = check_body(p + off + 16, p + off + 16 + len, &i, NULL, NULL);
        if (brc != MVID_E_OK) return brc;
    }
    return MVID_E_OK;
}

static void pad_fill(mvid_t *v)
{
    const unsigned lw = v->info.map_w, lh = v->info.map_h;
    const unsigned mw = v->mw, mh = v->mh;
    if (mw == lw && mh == lh) return;
    for (unsigned y = 0; y < mh; y++)
        for (unsigned x = 0; x < mw; x++)
            if (x >= lw || y >= lh)
                v->map[y * mw + x] = v->map[(y % lh) * mw + (x % lw)];
}

static int decode_frame(mvid_t *v, unsigned idx)
{
    const uint8_t *p = v->data;
    const size_t tb = v->info.bpp == 8 ? 64u : 32u;
    if (idx >= v->info.frames) return MVID_E_FRAME;
    uint32_t off = mvid_rd32(p + v->info.index_off + (size_t)idx * 4u);
    const unsigned type = p[off];
    const unsigned flags = p[off + 1];
    const unsigned len = mvid_rd16(p + off + 2);
    v->scroll_x = (int16_t)mvid_rd16(p + off + 4);
    v->scroll_y = (int16_t)mvid_rd16(p + off + 6);
    v->brightness = p[off + 12];
    (void)flags;
    const uint8_t *q = p + off + 16;
    const uint8_t *end = q + len;

    if (type == MVID_FRAME_INTRA) {
        memset(v->sheet, 0, (size_t)MVID_MAX_TILES * tb);
        memset(v->map, 0, (size_t)v->mw * v->mh * sizeof(uint16_t));
    }
    if (end - q < 6) return MVID_E_FRAME;
    unsigned np = mvid_rd16(q); q += 2;
    if ((size_t)(end - q) < (size_t)np * 3u) return MVID_E_FRAME;
    for (unsigned k = 0; k < np; k++, q += 3) {
        const unsigned ei = q[0];
        if (ei < 256) v->pal[ei] = mvid_rd16(q + 1);
    }
    if (end - q < 2) return MVID_E_FRAME;
    unsigned nt = mvid_rd16(q); q += 2;
    if ((size_t)(end - q) < (size_t)nt * (2u + tb)) return MVID_E_FRAME;
    for (unsigned k = 0; k < nt; k++, q += 2u + tb) {
        const unsigned slot = mvid_rd16(q);
        if (slot < MVID_MAX_TILES) memcpy(v->sheet + (size_t)slot * tb, q + 2, tb);
    }
    if (end - q < 2) return MVID_E_FRAME;
    unsigned no = mvid_rd16(q); q += 2;
    if ((size_t)(end - q) < (size_t)no * 4u) return MVID_E_FRAME;
    const unsigned lw = v->info.map_w, lh = v->info.map_h;
    for (unsigned k = 0; k < no; k++, q += 4) {
        const unsigned cell = mvid_rd16(q);
        const unsigned entry = mvid_rd16(q + 2);
        if (cell >= lw * lh) return MVID_E_FRAME;
        v->map[(cell / lw) * v->mw + (cell % lw)] = (uint16_t)entry;
    }
    pad_fill(v);
    v->cur = (uint16_t)idx;
    return MVID_E_OK;
}

int mvid_open(mvid_t *v, const void *data, size_t size, uint8_t *sheet, uint16_t *map)
{
    if (!v || !data || !sheet || !map) return MVID_E_PARAM;
    memset(v, 0, sizeof(*v));
    int rc = mvid_check(data, size);
    if (rc != MVID_E_OK) return rc;
    mvid_parse(&v->info, data, size);
    v->data = (const uint8_t *)data;
    v->size = size;
    v->sheet = sheet;
    v->map = map;
    v->mw = (uint16_t)mvid_next_pow2(v->info.map_w);
    v->mh = (uint16_t)mvid_next_pow2(v->info.map_h);
    memset(v->sheet, 0, mvid_sheet_size(&v->info));
    memset(v->map, 0, mvid_map_entries(&v->info) * sizeof(uint16_t));
    for (unsigned i = 0; i < v->info.pal_count && i < 256; i++)
        v->pal[i] = mvid_rd16(v->data + v->info.pal_off + (size_t)i * 2u);
    v->brightness = 16;
    rc = decode_frame(v, 0);
    if (rc != MVID_E_OK) return rc;
    v->opened = 1;
    return MVID_E_OK;
}

int mvid_seek(mvid_t *v, uint16_t frame)
{
    if (!v || !v->opened) return MVID_E_PARAM;
    if (frame >= v->info.frames) return MVID_E_FRAME;
    unsigned start = frame;
    while (start > 0) {
        uint32_t off = mvid_rd32(v->data + v->info.index_off + (size_t)start * 4u);
        if (v->data[off] == MVID_FRAME_INTRA) break;
        start--;
    }
    /* Restart from the nearest keyframe at or before `frame`. */
    const uint8_t *p = v->data;
    uint32_t off = mvid_rd32(p + v->info.index_off + (size_t)start * 4u);
    (void)off;
    memset(v->sheet, 0, mvid_sheet_size(&v->info));
    v->cur = 0;
    /* frame 0 of the file may not be the keyframe we want: decode forward */
    int rc = decode_frame(v, start);
    if (rc != MVID_E_OK) return rc;
    for (unsigned f = start + 1; f <= frame; f++) {
        rc = decode_frame(v, f);
        if (rc != MVID_E_OK) return rc;
    }
    return MVID_E_OK;
}

int mvid_next(mvid_t *v)
{
    if (!v || !v->opened) return MVID_E_PARAM;
    if ((unsigned)v->cur + 1u >= v->info.frames) return 0;
    return decode_frame(v, (unsigned)v->cur + 1u) == MVID_E_OK ? 1 : 0;
}

void mvid_close(mvid_t *v) { if (v) v->opened = 0; }

void mvid_draw(const mvid_t *v, uint16_t *fb, unsigned w, unsigned h)
{
    const unsigned lw = v->info.map_w, lh = v->info.map_h;
    const unsigned tb = v->info.bpp == 8 ? 64u : 32u;
    for (unsigned y = 0; y < h; y++) {
        for (unsigned x = 0; x < w; x++) {
            int sx = (int)x + v->scroll_x, sy = (int)y + v->scroll_y;
            int tx = (sx >> 3) % (int)lw; if (tx < 0) tx += (int)lw;
            int ty = (sy >> 3) % (int)lh; if (ty < 0) ty += (int)lh;
            const uint16_t e = v->map[(unsigned)ty * v->mw + (unsigned)tx];
            unsigned px = (unsigned)(sx & 7), py = (unsigned)(sy & 7);
            if (e & 0x4000) px = 7 - px;
            if (e & 0x8000) py = 7 - py;
            const uint8_t *t = v->sheet + (size_t)(e & 0x3FF) * tb;
            unsigned idx;
            if (v->info.bpp == 8) idx = t[py * 8 + px];
            else { const uint8_t b = t[py * 4 + (px >> 1)]; idx = (px & 1) ? (b >> 4) : (b & 15); }
            uint16_t c = 0;
            if (idx) {
                const unsigned slot = (e >> 10) & 7;
                c = v->info.bpp == 8 ? v->pal[idx & 0xFF] : v->pal[(slot * 16 + idx) & 0xFF];
            }
            fb[(size_t)y * w + x] = mvid_scale565(c, v->brightness);
        }
    }
}
