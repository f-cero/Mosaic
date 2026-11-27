/* SPDX-License-Identifier: Apache-2.0 */
/* Mosaic — ms_cart.c: cart16 v2 mount / validate / write. */
#include "mosaic/ms_cart.h"
#include <string.h>

const char *ms_cart_err_str(int e)
{
    switch (e) {
    case MS_CART_OK:         return "ok";
    case MS_CART_E_PARAM:    return "bad argument";
    case MS_CART_E_MAGIC:    return "bad magic or version";
    case MS_CART_E_SIZE:     return "truncated or oversized cart";
    case MS_CART_E_CRC:      return "crc32 mismatch";
    case MS_CART_E_RECORD:   return "bad toc record";
    case MS_CART_E_ASSET:    return "asset failed its validator";
    case MS_CART_E_DUP:      return "duplicate asset name";
    case MS_CART_E_BUDGET:   return "over budget";
    case MS_CART_E_LAYOUT:   return "payload layout out of order";
    case MS_CART_E_NOSPACE:  return "writer buffer too small";
    }
    return "unknown";
}

/* ---------------- per-kind validators ---------------- */

int ms_check_tiles(size_t bytes, unsigned bpp, unsigned tiles)
{
    if (bpp != 2 && bpp != 4 && bpp != 8) return 0;
    if (tiles == 0 || tiles > MS_MAX_TILES) return 0;
    const size_t per = bpp == 8 ? 64u : bpp == 4 ? 32u : 16u;
    return bytes == (size_t)tiles * per;
}

/* bg_map payload.
 *   v2.0  u16 entry_count (power of two) | u16 entries[count]
 *   v2.1  u16 entry_count | u16 w | u16 h | u16 entries[count]
 * The two forms are told apart by exact length (2+n*2 vs 6+n*2), so a v2.0
 * reader and a v2.1 reader both accept a valid payload and the count can
 * never be confused with the width. Dimensions are in tiles and describe
 * the *visible* map; the entry count is the padded power-of-two stride area,
 * i.e. count == next_pow2(w) * next_pow2(h). Without them a non-pow2 level
 * can only be guessed, which wrapped 40x30 maps at 64x32. */
static unsigned next_pow2u(unsigned v)
{
    unsigned s = 1;
    while (s < v) s <<= 1;
    return s;
}

int ms_check_map(const void *data, size_t bytes)
{
    const uint8_t *p = (const uint8_t *)data;
    if (!p || bytes < 2) return 0;
    const unsigned n = ms_rd16(p);
    if (n == 0 || n > 32768u) return 0;
    if (n & (n - 1)) return 0;                       /* power of two */
    if (bytes == 2u + (size_t)n * 2u) return 1;
    if (bytes == 6u + (size_t)n * 2u) {
        const unsigned w = ms_rd16(p + 2), h = ms_rd16(p + 4);
        if (w == 0 || h == 0 || w > 128u || h > 128u) return 0;
        if (next_pow2u(w) * next_pow2u(h) != n) return 0;
        return 1;
    }
    return 0;
}

/* palette payload.
 *   v2.0  u16 entry_count | u16 RGB565 entries[count]
 *   v2.1  u16 entry_count | u8 bank | u8 reserved(0) | u16 entries[count]
 * v2.0 leaves the bank to the manifest (palette_bg / palette_spr asset
 * names) and failing that to asset order, which cannot survive a strict
 * reference build that carries no manifest at all. v2.1 states it. */
int ms_check_palette(const void *data, size_t bytes)
{
    const uint8_t *p = (const uint8_t *)data;
    if (!p || bytes < 2) return 0;
    const unsigned n = ms_rd16(p);
    if (n < 1 || n > MS_PAL_ENTRIES) return 0;
    if (bytes == 2u + (size_t)n * 2u) return 1;
    if (bytes == 4u + (size_t)n * 2u) return p[2] <= 1u && p[3] == 0u;
    return 0;
}

int ms_check_bank(size_t bytes) { return bytes >= 16 && bytes <= MS_BANK_BYTES_MAX; }

int ms_check_midi(const void *data, size_t bytes)
{
    const uint8_t *p = (const uint8_t *)data;
    return p && bytes > 14 && p[0] == 'M' && p[1] == 'T' && p[2] == 'h' && p[3] == 'd';
}

/* Sheet layout (SPR1 and SPR2 share the 14-byte header field order; only
 * the version byte and the frame record size differ):
 *   u32 magic | u8 version | u8 bpp | u16 pal_count | u16 tile_count
 *   | u16 frame_count | u16 reserved
 *   palette  pal_count * u16 RGB565 (index 0 transparent)
 *   tiles    tile_count * (64 B @8bpp | 32 B @4bpp)
 *   frames   frame_count * (8 B SPR2 | 6 B SPR1)
 * The palette must be defined exactly once: no duplicate colours and no
 * index that no tile references (the SPR1 packer defect class). */
int ms_check_sheet(const void *data, size_t bytes)
{
    const uint8_t *p = (const uint8_t *)data;
    if (!p || bytes < 14) return 0;
    const uint32_t magic = ms_rd32(p);
    const unsigned ver = p[4], bpp = p[5];
    const unsigned pal = ms_rd16(p + 6), tiles = ms_rd16(p + 8), frames = ms_rd16(p + 10);
    unsigned stride;
    if (magic == MS_SPR_MAGIC)       { if (ver != 2) return 0; stride = 8; }
    else if (magic == MS_SPR1_MAGIC) { if (ver != 1) return 0; stride = 6; }
    else return 0;
    if (bpp != 4 && bpp != 8) return 0;
    if (tiles == 0 || tiles > MS_MAX_TILES) return 0;
    if (frames == 0) return 0;
    if (pal == 0 || pal > (bpp == 4 ? MS_PAL_SLOT_4BPP : MS_PAL_ENTRIES)) return 0;
    const size_t tilesz = bpp == 8 ? 64u : 32u;
    if (bytes != 14u + (size_t)pal * 2u + (size_t)tiles * tilesz + (size_t)frames * stride) return 0;

    for (unsigned i = 0; i < pal; i++)
        for (unsigned j = i + 1; j < pal; j++)
            if (ms_rd16(p + 14 + i * 2) == ms_rd16(p + 14 + j * 2)) return 0;

    uint8_t used[256];
    memset(used, 0, sizeof(used));
    const uint8_t *tilep = p + 14 + (size_t)pal * 2;
    for (unsigned t = 0; t < tiles; t++) {
        const uint8_t *q = tilep + (size_t)t * tilesz;
        if (bpp == 8) { for (unsigned k = 0; k < 64; k++) used[q[k]] = 1; }
        else { for (unsigned k = 0; k < 32; k++) { used[q[k] & 15] = 1; used[q[k] >> 4] = 1; } }
    }
    for (unsigned i = 1; i < pal; i++) if (!used[i]) return 0;
    return 1;
}

int ms_check_program(const void *data, size_t bytes)
{
    const uint8_t *p = (const uint8_t *)data;
    if (!p || bytes < 32 || ms_rd32(p) != MS_PROG_MAGIC) return 0;
    const uint32_t code = ms_rd32(p + 8), ents = ms_rd32(p + 12);
    const uint16_t n = ms_rd16(p + 16);
    if (code == 0 || code > MS_PROGRAM_MAX || bytes < 32u + code) return 0;
    if (ents < 32u + code || ents + (size_t)n * 32u > bytes) return 0;
    return 1;
}

int ms_check_manifest(const char *text, size_t bytes)
{
    return text && bytes > 0 && bytes <= 2048;
}

/* MS_A_MATRICES: u16 tables | u16 0 | tables x u16 rows (pad to 4)
 *                | sum(rows) x {i32 a, b, c, d}
 * One table is a per-scanline matrix stack: row i is composed with the
 * object matrix for scanline i of the sprite that selects it. */
int ms_check_matrices(const void *data, size_t bytes)
{
    const uint8_t *d = (const uint8_t *)data;
    if (!d || bytes < 4) return 0;
    const unsigned tables = ms_rd16(d);
    if (tables == 0 || tables > 64) return 0;
    if (ms_rd16(d + 2) != 0) return 0;
    size_t hdr = 4u + (size_t)tables * 2u;
    hdr = (hdr + 3u) & ~(size_t)3u;
    if (bytes < hdr) return 0;
    uint64_t rows = 0;
    for (unsigned i = 0; i < tables; i++) {
        const unsigned r = ms_rd16(d + 4u + i * 2u);
        if (r == 0 || r > 240) return 0;
        rows += r;
    }
    if (rows * 16u != (uint64_t)(bytes - hdr)) return 0;
    return 1;
}

/* MS_A_SAVE: u16 region_bytes | u8 slots | u8 reserved(0).
 * Declaring the region makes a cart's save capability inspectable before it
 * runs; a cart without this asset is read-only by construction. */
int ms_check_save(const void *data, size_t bytes)
{
    const uint8_t *p = (const uint8_t *)data;
    if (!p || bytes != 4) return 0;
    const unsigned b = ms_rd16(p), slots = p[2];
    if (b < 512u || b > 65532u) return 0;
    if (slots < 1u || slots > 8u) return 0;
    if (p[3] != 0) return 0;
    /* two copies per slot must each fit a header and a byte of body */
    if ((b - 14u) / (slots * 2u) < 29u) return 0;
    return 1;
}

int ms_check_anim(const void *data, size_t bytes)
{
    const uint8_t *p = (const uint8_t *)data;
    if (!p || bytes < 10) return 0;
    const unsigned n = ms_rd16(p);
    return n >= 1 && bytes == 2u + (size_t)n * 8u;
}

void ms_map_dims(uint32_t count, int *w, int *h)
{
    uint32_t side = 1;
    if (count < 1) { if (w) *w = 0; if (h) *h = 0; return; }
    while (side * side < count) side <<= 1;      /* smallest pow2 with side^2 >= count */
    if (w) *w = (int)side;
    if (h) *h = (int)(count / side);
}

/* ---------------- mount ---------------- */

static int name_ok(const uint8_t *n)
{
    if (n[31] != 0) return 0;
    size_t len = 0;
    while (len < 32 && n[len]) len++;
    if (len < 1 || len > 31) return 0;
    for (size_t i = 0; i < len; i++) if (n[i] < 0x20 || n[i] == 0x7F) return 0;
    return 1;
}

static int payload_ok(const uint8_t *d, uint32_t kind, uint32_t size)
{
    switch (kind) {
    case MS_A_BG_TILES:
        return size >= 3 && ms_check_tiles(size - 3u, d[0], ms_rd16(d + 1));
    case MS_A_BG_MAP:      return ms_check_map(d, size);
    case MS_A_PALETTE:     return ms_check_palette(d, size);
    case MS_A_SPRITES:     return ms_check_sheet(d, size);
    case MS_A_AFFINE:      return size > 4 && size <= 512u * 1024u;
    case MS_A_BANK:        return ms_check_bank(size);
    case MS_A_MIDI:        return ms_check_midi(d, size);
    case MS_A_PROGRAM:     return ms_check_program(d, size);
    case MS_A_MANIFEST:    return ms_check_manifest((const char *)d, size);
    case MS_A_ANIM:        return ms_check_anim(d, size);
    case MS_A_MVID:        return ms_check_mvid(d, size);
    case MS_A_MATRICES:    return ms_check_matrices(d, size);
    case MS_A_SAVE:        return ms_check_save(d, size);
    default:               return 0;
    }
}

/* "title=<text>" from a manifest payload, first match wins. */
static void manifest_title(char *out, size_t cap, const uint8_t *d, uint32_t n)
{
    out[0] = 0;
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && d[j] != '\n') j++;
        if (j - i > 6 && !memcmp(d + i, "title=", 6)) {
            size_t k = 6;
            while (i + k < j && k - 6 < cap - 1) { out[k - 6] = (char)d[i + k]; k++; }
            out[k - 6] = 0;
            return;
        }
        i = j + 1;
    }
}

int ms_cart_mount_budget(ms_cart_t *c, const void *blob, size_t bytes, size_t budget)
{
    if (!c || !blob) return MS_CART_E_PARAM;
    memset(c, 0, sizeof(*c));
    if (bytes < MS_CART_HDR) return MS_CART_E_SIZE;
    const uint8_t *p = (const uint8_t *)blob;

    if (ms_rd32(p) != MS_CART_MAGIC) return MS_CART_E_MAGIC;
    if (ms_rd16(p + 4) != MS_CART_VERSION) return MS_CART_E_MAGIC;
    const uint16_t flags = ms_rd16(p + 6);
    const uint32_t count = ms_rd32(p + 8);
    const uint32_t toc   = ms_rd32(p + 12);
    if (toc != MS_CART_HDR) return MS_CART_E_LAYOUT;
    if (count == 0 || count > MS_CART_MAX_RECORDS) return MS_CART_E_RECORD;

    const size_t toclen = (size_t)count * MS_CART_TOC_ENTRY;
    if (MS_CART_HDR + toclen > bytes) return MS_CART_E_SIZE;
    if (ms_crc32(p + MS_CART_HDR, toclen) != ms_rd32(p + 16)) return MS_CART_E_CRC;

    if (budget == 0 || budget > MS_CART_BYTES_MAX) budget = MS_CART_BYTES_MAX;
    if (bytes > budget) return MS_CART_E_BUDGET;

    uint32_t prev_end = (uint32_t)(MS_CART_HDR + toclen);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = p + MS_CART_HDR + (size_t)i * MS_CART_TOC_ENTRY;
        if (!name_ok(e)) return MS_CART_E_RECORD;
        const uint32_t kind = ms_rd32(e + 32);
        const uint32_t off  = ms_rd32(e + 36);
        const uint32_t size = ms_rd32(e + 40);
        const uint32_t crc  = ms_rd32(e + 44);
        if (kind < 1 || kind > MS_CART_KIND_MAX) return MS_CART_E_RECORD;
        if (kind > MS_CART_KIND_CANON_MAX && !(flags & MS_CART_F_EXT)) return MS_CART_E_RECORD;
        if (size == 0) return MS_CART_E_RECORD;
        if (off < prev_end) return MS_CART_E_LAYOUT;
        if (off > bytes || size > bytes - off) return MS_CART_E_SIZE;
        prev_end = off + size;
        if (ms_crc32(p + off, size) != crc) return MS_CART_E_CRC;
        if (!payload_ok(p + off, kind, size)) return MS_CART_E_ASSET;
        for (uint32_t j = 0; j < i; j++) {
            const uint8_t *o = p + MS_CART_HDR + (size_t)j * MS_CART_TOC_ENTRY;
            if (!memcmp(o, e, 32)) return MS_CART_E_DUP;
        }
    }

    c->blob = p; c->blob_size = bytes; c->count = count; c->toc_off = toc;
    c->version = MS_CART_VERSION; c->flags = flags; c->budget = (uint32_t)budget;
    c->crc = ms_rd32(p + 16);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = p + MS_CART_HDR + (size_t)i * MS_CART_TOC_ENTRY;
        if (ms_rd32(e + 32) == MS_A_MANIFEST)
            manifest_title(c->title, sizeof(c->title), p + ms_rd32(e + 36), ms_rd32(e + 40));
    }
    return MS_CART_OK;
}

int ms_cart_mount(ms_cart_t *c, const void *blob, size_t bytes)
{
    return ms_cart_mount_budget(c, blob, bytes, MS_CART_BYTES_MAX);
}

void ms_cart_unmount(ms_cart_t *c) { if (c) memset(c, 0, sizeof(*c)); }
const char *ms_cart_title(const ms_cart_t *c) { return c ? c->title : ""; }

static int find_common(const ms_cart_t *c, const char *name, uint32_t kind, ms_asset_t *out)
{
    if (!c || !c->blob || !out) return MS_CART_E_PARAM;
    for (uint32_t i = 0; i < c->count; i++) {
        const uint8_t *e = c->blob + c->toc_off + (size_t)i * MS_CART_TOC_ENTRY;
        const uint32_t k = ms_rd32(e + 32);
        if (kind && k != kind) continue;
        if (name && strncmp(name, (const char *)e, 32) != 0) continue;
        out->name = (const char *)e;
        out->kind = k;
        out->data = c->blob + ms_rd32(e + 36);
        out->size = ms_rd32(e + 40);
        out->crc  = ms_rd32(e + 44);
        return MS_CART_OK;
    }
    return MS_CART_E_RECORD;
}

int ms_cart_find(const ms_cart_t *c, const char *name, ms_asset_t *out)
{
    if (!name) return MS_CART_E_PARAM;
    return find_common(c, name, 0, out);
}

int ms_cart_find_kind(const ms_cart_t *c, uint32_t kind, ms_asset_t *out)
{
    return find_common(c, NULL, kind, out);
}

/* ---------------- writer ---------------- */

void ms_cart_writer_begin(ms_cart_writer_t *w, void *buf, size_t cap)
{
    memset(w, 0, sizeof(*w));
    w->buf = (uint8_t *)buf;
    w->cap = cap;
    w->len = MS_CART_PAYLOAD_STAGING;
    w->budget = MS_CART_BYTES_MAX;
}

void ms_cart_writer_set_budget(ms_cart_writer_t *w, size_t budget) { if (w) w->budget = budget; }

int ms_cart_add(ms_cart_writer_t *w, const char *name, uint32_t kind, const void *data, size_t size)
{
    if (!w || !w->buf || !name || !data) return MS_CART_E_PARAM;
    const size_t len = strlen(name);
    if (len < 1 || len > 31) return MS_CART_E_RECORD;
    if (w->count >= MS_CART_MAX_RECORDS) return MS_CART_E_NOSPACE;
    if (kind < 1 || kind > MS_CART_KIND_MAX) return MS_CART_E_RECORD;
    if (size == 0 || size > 0xFFFFFFFFu) return MS_CART_E_ASSET;
    if (!payload_ok((const uint8_t *)data, kind, (uint32_t)size)) return MS_CART_E_ASSET;
    for (uint32_t i = 0; i < w->count; i++)
        if (!strncmp((const char *)w->names[i], name, 32)) return MS_CART_E_DUP;
    if (w->len + size > w->cap) return MS_CART_E_NOSPACE;

    memcpy(w->buf + w->len, data, size);
    const uint32_t i = w->count++;
    memset(w->names[i], 0, 32);
    memcpy(w->names[i], name, len);
    w->offsets[i] = (uint32_t)w->len;
    w->sizes[i]   = (uint32_t)size;
    w->kinds[i]   = kind;
    w->crcs[i]    = ms_crc32(data, size);
    if (kind > MS_CART_KIND_CANON_MAX) w->flags |= MS_CART_F_EXT;
    w->len += size;
    return MS_CART_OK;
}

int ms_cart_finish(ms_cart_writer_t *w)
{
    if (!w || !w->buf || w->count == 0) return MS_CART_E_PARAM;
    const size_t payload = w->len - MS_CART_PAYLOAD_STAGING;
    const size_t base = MS_CART_HDR + (size_t)w->count * MS_CART_TOC_ENTRY;
    const size_t total = base + payload;
    if (total > w->cap) return MS_CART_E_NOSPACE;
    if (total > w->budget || total > MS_CART_BYTES_MAX) return MS_CART_E_BUDGET;

    const size_t shift = MS_CART_PAYLOAD_STAGING - base;
    memmove(w->buf + base, w->buf + MS_CART_PAYLOAD_STAGING, payload);
    memset(w->buf, 0, base);

    uint8_t *p = w->buf;
    ms_wr32(p, MS_CART_MAGIC);
    ms_wr16(p + 4, MS_CART_VERSION);
    ms_wr16(p + 6, w->flags);
    ms_wr32(p + 8, w->count);
    ms_wr32(p + 12, MS_CART_HDR);
    for (uint32_t i = 0; i < w->count; i++) {
        const uint32_t off = w->offsets[i] - (uint32_t)shift;
        uint8_t *e = p + MS_CART_HDR + (size_t)i * MS_CART_TOC_ENTRY;
        memcpy(e, w->names[i], 32);
        ms_wr32(e + 32, w->kinds[i]);
        ms_wr32(e + 36, off);
        ms_wr32(e + 40, w->sizes[i]);
        ms_wr32(e + 44, w->crcs[i]);
    }
    ms_wr32(p + 16, ms_crc32(p + MS_CART_HDR, (size_t)w->count * MS_CART_TOC_ENTRY));

    /* Self-check: a cart we cannot mount is a bug, not a deliverable. */
    ms_cart_t probe;
    const int rc = ms_cart_mount_budget(&probe, p, total, w->budget);
    if (rc != MS_CART_OK) return rc;
    return (int)total;
}
