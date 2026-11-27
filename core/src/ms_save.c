/* SPDX-License-Identifier: Apache-2.0 */
/* Mosaic — ms_save.c: the save image, its identity, and its two homes. */
#include "mosaic/ms_save.h"
#include <stdio.h>
#include <string.h>

const char *ms_save_err_str(int e)
{
    switch (e) {
    case MS_SAVE_OK:        return "ok";
    case MS_SAVE_E_PARAM:   return "bad argument";
    case MS_SAVE_E_MAGIC:   return "not a save image";
    case MS_SAVE_E_SIZE:    return "image size does not match";
    case MS_SAVE_E_CRC:     return "integrity check failed";
    case MS_SAVE_E_CART:    return "save belongs to a different cartridge build";
    case MS_SAVE_E_EMPTY:   return "slot is empty";
    case MS_SAVE_E_SLOT:    return "slot out of range";
    case MS_SAVE_E_BODY:    return "body too large for the slot";
    case MS_SAVE_E_NOSPACE: return "output buffer too small";
    case MS_SAVE_E_DEV:     return "save belongs to another developer";
    case MS_SAVE_E_APP:     return "save belongs to another title and is not linked";
    case MS_SAVE_E_RO:      return "save was opened read-only";
    case MS_SAVE_E_LOC:     return "save lives in the other location";
    case MS_SAVE_E_LEGACY:  return "v1 image is read-only; re-init it to upgrade";
    }
    return "unknown";
}

static uint64_t ms_rd64le_(const uint8_t *p)
{
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}
static void ms_wr64le_(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (i * 8));
}

/* ---------------- record layout ---------------- */

static unsigned hdr_of(const ms_save_t *s) { return s->ver == 1u ? MS_SAVE_HDR_V1 : MS_SAVE_HDR; }
static unsigned copy_hdr_of(const ms_save_t *s)
{
    return s->ver == 1u ? MS_SAVE_COPY_HDR_V1 : MS_SAVE_COPY_HDR;
}

static uint8_t *rec_at(const ms_save_t *s, unsigned slot, unsigned which)
{
    return s->region + hdr_of(s) + (size_t)(slot * 2u + which) * s->stride;
}

static uint64_t rec_tag(const ms_save_t *s, const uint8_t *r)
{
    ms_sip_t c;
    ms_sip_init(&c, s->key);
    ms_sip_update(&c, r, 28);
    ms_sip_update(&c, r + MS_SAVE_COPY_HDR, ms_rd16(r + 8));
    return ms_sip_final(&c);
}

/* 0 unused, 1 body, 2 tombstone, 3 present but failed a check. */
static int rec_state(const ms_save_t *s, const uint8_t *r, uint32_t *seq, uint16_t *len,
                     uint16_t *fmt)
{
    if (ms_rd32(r) != MS_SAVE_COPY_MAGIC) return 0;
    *fmt = 0;
    if (s->ver == 1u) {
        if (ms_rd16(r + 26) != 0u) return 3;
        if (ms_crc16(r, 24) != ms_rd16(r + 24)) return 3;
    } else {
        if (ms_rd16(r + 26) != 0u) return 3;
        if (ms_crc16(r, 36) != ms_rd16(r + 36)) return 3;   /* commit word */
    }
    const uint16_t l = ms_rd16(r + 8);
    if (l > s->max_body) return 3;
    if (l != 0 && ms_crc16(r + copy_hdr_of(s), l) != ms_rd16(r + 10)) return 3;
    if (s->ver != 1u && !s->unverified) {
        if (ms_rd64le_(r + 28) != rec_tag(s, r)) return 3;
        *fmt = ms_rd16(r + 24);
    }
    *seq = ms_rd32(r + 4);
    *len = l;
    return l == 0 ? 2 : 1;
}

static unsigned pick_newest(const ms_save_t *s, unsigned slot, int states[2],
                            uint32_t seqs[2], uint16_t lens[2], uint16_t fmts[2],
                            unsigned *corrupt)
{
    *corrupt = 0;
    unsigned best = 2, best_seq = 0;
    for (unsigned w = 0; w < 2; w++) {
        const uint8_t *r = rec_at(s, slot, w);
        states[w] = rec_state(s, r, &seqs[w], &lens[w], &fmts[w]);
        if (states[w] == 3) { *corrupt = 1; continue; }
        if (states[w] == 0) continue;
        if (best == 2 || seqs[w] > best_seq) { best = w; best_seq = seqs[w]; }
    }
    return best;
}

/* ---------------- image ---------------- */

void ms_save_id_from_cfg(ms_save_id_t *id, const ms_save_cfg_t *cfg, uint8_t loc,
                         uint32_t cart_crc)
{
    if (!id) return;
    memset(id, 0, sizeof *id);
    if (!cfg) return;
    id->dev_id = cfg->dev_id;
    id->app_id = cfg->app_id;
    id->dev_key = cfg->dev_key[0] ? cfg->dev_key : (cfg->dev[0] ? cfg->dev : NULL);
    id->cart_crc = cart_crc;
    id->loc = loc;
    id->n_reads = cfg->n_reads;
    for (int i = 0; i < cfg->n_reads && i < MS_SAVE_READS_MAX; i++) id->reads[i] = cfg->reads[i];
}

static int set_common(ms_save_t *s, void *region, size_t bytes, const ms_save_id_t *id)
{
    s->region = (uint8_t *)region;
    s->bytes = bytes;
    s->cart_crc = id ? id->cart_crc : 0;
    s->dev_id = id ? id->dev_id : 0;
    s->app_id = id ? id->app_id : 0;
    s->read_only = 0;
    s->unverified = 0;
    ms_mac_key(id ? id->dev_key : NULL, s->key);
    return MS_SAVE_OK;
}

int ms_save_format(ms_save_t *s, void *region, size_t bytes, const ms_save_id_t *id,
                   unsigned slots, unsigned *flags)
{
    if (flags) *flags = 0;
    if (!s || !region) return MS_SAVE_E_PARAM;
    if (slots < 1u || slots > MS_SAVE_SLOTS_MAX) return MS_SAVE_E_PARAM;
    if (bytes < MS_SAVE_BYTES_MIN || bytes > MS_SAVE_BYTES_MAX || bytes > 0xFFFFu)
        return MS_SAVE_E_SIZE;
    const size_t stride = (bytes - MS_SAVE_HDR) / (size_t)(slots * 2u);
    if (stride < MS_SAVE_COPY_HDR + 16u) return MS_SAVE_E_SIZE;

    uint8_t *p = (uint8_t *)region;
    memset(p, 0, bytes);
    ms_wr32(p, MS_SAVE_MAGIC);
    ms_wr16(p + 4, (uint16_t)MS_SAVE_VERSION);
    ms_wr16(p + 6, (uint16_t)bytes);
    p[8] = (uint8_t)slots;
    p[9] = id ? id->loc : MS_SAVE_LOC_CONSOLE;
    ms_wr16(p + 10, 0);
    ms_wr32(p + 12, id ? id->app_id : 0);
    ms_wr16(p + 16, (uint16_t)((id ? id->dev_id : 0) & 0xFFFFu));
    ms_wr16(p + 18, (uint16_t)((id ? id->dev_id : 0) >> 16));
    ms_wr32(p + 20, id ? id->cart_crc : 0);

    set_common(s, region, bytes, id);
    s->slots = slots;
    s->stride = (unsigned)stride;
    s->max_body = (unsigned)(stride - MS_SAVE_COPY_HDR);
    s->ver = MS_SAVE_VERSION;
    s->loc = id ? id->loc : MS_SAVE_LOC_CONSOLE;
    if (flags) *flags |= MS_SAVE_F_INIT;
    return MS_SAVE_OK;
}

static int image_blank(const uint8_t *p, size_t bytes)
{
    for (size_t i = 0; i < bytes; i++) if (p[i] != 0x00 && p[i] != 0xFF) return 0;
    return 1;
}

/* Identity and location checks, shared by open. Returns MS_SAVE_OK or the
 * refusal, and sets *read_only when the link is a lineage grant. */
static int check_identity(ms_save_t *s, const ms_save_id_t *id, unsigned *flags,
                          int enforce_loc)
{
    if (!id) return MS_SAVE_OK;
    if (enforce_loc && id->loc && s->loc != id->loc) return MS_SAVE_E_LOC;
    if (id->dev_id && s->dev_id != id->dev_id) return MS_SAVE_E_DEV;
    if (s->app_id != id->app_id) {
        int linked = 0;
        for (int i = 0; i < id->n_reads && i < MS_SAVE_READS_MAX; i++)
            if (id->reads[i] && id->reads[i] == s->app_id) linked = 1;
        if (!linked) return MS_SAVE_E_APP;
        s->read_only = 1;
        if (flags) *flags |= MS_SAVE_F_LINKED;
    }
    /* The cartridge home is that cartridge's SRAM, so it is bound to the
     * build. The console home follows the save id across builds. */
    if (s->loc == MS_SAVE_LOC_CART && id->cart_crc && s->cart_crc != id->cart_crc)
        return MS_SAVE_E_CART;
    return MS_SAVE_OK;
}

static int open_impl(ms_save_t *s, void *region, size_t bytes, const ms_save_id_t *id,
                     unsigned *flags, int enforce)
{
    if (flags) *flags = 0;
    if (!s || !region) return MS_SAVE_E_PARAM;
    if (bytes < MS_SAVE_BYTES_MIN || bytes > MS_SAVE_BYTES_MAX) return MS_SAVE_E_SIZE;
    uint8_t *p = (uint8_t *)region;

    if (image_blank(p, bytes))
        return ms_save_format(s, region, bytes, id, MS_SAVE_SLOTS, flags);

    if (ms_rd32(p) != MS_SAVE_MAGIC) return MS_SAVE_E_MAGIC;
    const unsigned ver = ms_rd16(p + 4);
    if (ver != 1u && ver != MS_SAVE_VERSION) return MS_SAVE_E_MAGIC;
    if (ms_rd16(p + 6) != (uint16_t)bytes) return MS_SAVE_E_SIZE;

    s->ver = ver;
    set_common(s, region, bytes, id);
    if (ver == 1u) {
        s->slots = p[8];
        if (s->slots < 1u || s->slots > MS_SAVE_SLOTS_MAX) return MS_SAVE_E_MAGIC;
        s->stride = (unsigned)((bytes - MS_SAVE_HDR_V1) / (size_t)(s->slots * 2u));
        if (s->stride < MS_SAVE_COPY_HDR_V1 + 16u) return MS_SAVE_E_SIZE;
        s->max_body = s->stride - MS_SAVE_COPY_HDR_V1;
        s->loc = MS_SAVE_LOC_CONSOLE;      /* v1 had no location */
        s->cart_crc = ms_rd32(p + 10);
        s->dev_id = s->app_id = 0;
        s->ver = 1u;
        if (flags) *flags |= MS_SAVE_F_LEGACY;
        /* v1 had exactly one binding: the build. Keep it exact. */
        if (id && id->cart_crc && s->cart_crc != id->cart_crc) return MS_SAVE_E_CART;
        return MS_SAVE_OK;
    }

    s->slots = p[8];
    if (s->slots < 1u || s->slots > MS_SAVE_SLOTS_MAX) return MS_SAVE_E_MAGIC;
    s->loc = p[9];
    if (s->loc != MS_SAVE_LOC_CONSOLE && s->loc != MS_SAVE_LOC_CART) return MS_SAVE_E_MAGIC;
    s->app_id = ms_rd32(p + 12);
    s->dev_id = (uint32_t)ms_rd16(p + 16) | ((uint32_t)ms_rd16(p + 18) << 16);
    s->cart_crc = ms_rd32(p + 20);
    s->stride = (unsigned)((bytes - MS_SAVE_HDR) / (size_t)(s->slots * 2u));
    if (s->stride < MS_SAVE_COPY_HDR + 16u) return MS_SAVE_E_SIZE;
    s->max_body = s->stride - MS_SAVE_COPY_HDR;

    return enforce ? check_identity(s, id, flags, 1) : MS_SAVE_OK;
}

int ms_save_open(ms_save_t *s, void *region, size_t bytes, const ms_save_id_t *id,
                 unsigned *flags)
{
    return open_impl(s, region, bytes, id, flags, 1);
}

/* Inspection: no identity gate and no MAC check, read-only. The format is
 * public and a tool must be able to look inside an image it did not write —
 * otherwise a mismatched dev key turns into "cannot inspect the file" and the
 * person is left guessing. Nothing here can write. */
int ms_save_open_raw(ms_save_t *s, void *region, size_t bytes, unsigned *flags)
{
    ms_save_id_t id;
    memset(&id, 0, sizeof id);
    const int rc = open_impl(s, region, bytes, &id, flags, 0);
    if (rc != MS_SAVE_OK) return rc;
    s->read_only = 1;
    s->unverified = 1;
    if (flags) *flags |= MS_SAVE_F_UNVERIFIED;
    return MS_SAVE_OK;
}

/* ---------------- slots ---------------- */

int ms_save_read(const ms_save_t *s, unsigned slot, char *name, size_t namecap,
                 void *buf, size_t cap, size_t *len, uint16_t *fmt, unsigned *flags)
{
    if (flags) *flags = 0;
    if (!s || !s->region) return MS_SAVE_E_PARAM;
    if (slot >= s->slots) return MS_SAVE_E_SLOT;
    if (len) *len = 0;
    if (fmt) *fmt = 0;

    int states[2]; uint32_t seqs[2] = { 0, 0 }; uint16_t lens[2] = { 0, 0 }, fmts[2] = { 0, 0 };
    unsigned corrupt = 0;
    const unsigned best = pick_newest(s, slot, states, seqs, lens, fmts, &corrupt);

    if (best == 2) {
        if (corrupt) { if (flags) *flags |= MS_SAVE_F_FELLBACK | MS_SAVE_F_TORN; return MS_SAVE_E_CRC; }
        return MS_SAVE_E_EMPTY;
    }
    if (states[best] == 2) return MS_SAVE_E_EMPTY;
    if (corrupt) { if (flags) *flags |= MS_SAVE_F_FELLBACK | MS_SAVE_F_TORN; }

    const uint8_t *r = rec_at(s, slot, best);
    const uint16_t l = lens[best];
    if (name && namecap) {
        size_t k = 0;
        while (k < MS_SAVE_NAME && k + 1u < namecap && r[12 + k]) { name[k] = (char)r[12 + k]; k++; }
        name[k] = 0;
    }
    if (len) *len = l;
    if (fmt) *fmt = fmts[best];
    if (!buf || cap == 0) return MS_SAVE_OK;
    const size_t n = l < cap ? l : cap;
    if (n) memcpy(buf, r + copy_hdr_of(s), n);
    return n < l ? MS_SAVE_E_NOSPACE : MS_SAVE_OK;
}

/* A legacy image cannot carry v2 records, so a write to one promotes the
 * whole image first: re-head it and re-stamp every live slot. */
static int promote_legacy(ms_save_t *s)
{
    if (s->ver != 1u) return MS_SAVE_OK;
    /* The v2 header grows by 10 bytes and every record by 12, so a legacy
     * image cannot be promoted in place without a second buffer. Refuse
     * rather than half-migrate: `mosaic save init` rewrites it from the
     * caller-supplied identity, which is the only place that identity is
     * known. */
    return MS_SAVE_E_LEGACY;
}

static unsigned older_copy(const ms_save_t *s, unsigned slot, uint32_t *seq_out)
{
    int states[2]; uint32_t seqs[2] = { 0, 0 }; uint16_t lens[2] = { 0, 0 }, fmts[2] = { 0, 0 };
    unsigned corrupt = 0;
    (void)pick_newest(s, slot, states, seqs, lens, fmts, &corrupt);
    unsigned target;
    if (states[0] && states[0] != 3 && states[1] && states[1] != 3)
        target = seqs[0] <= seqs[1] ? 0u : 1u;
    else if (states[0] && states[0] != 3)
        target = 1u;
    else
        target = 0u;
    const uint32_t a = (states[0] && states[0] != 3) ? seqs[0] : 0u;
    const uint32_t b = (states[1] && states[1] != 3) ? seqs[1] : 0u;
    *seq_out = (a > b ? a : b) + 1u;
    return target;
}

int ms_save_write(ms_save_t *s, unsigned slot, const char *name, uint16_t fmt,
                  const void *data, size_t len)
{
    if (!s || !s->region) return MS_SAVE_E_PARAM;
    if (slot >= s->slots) return MS_SAVE_E_SLOT;
    if (s->read_only) return MS_SAVE_E_RO;
    if (s->ver == 1u) { const int rc = promote_legacy(s); if (rc != MS_SAVE_OK) return rc; }
    if (len > s->max_body) return MS_SAVE_E_BODY;
    if (len == 0) return ms_save_erase(s, slot);

    uint32_t seq = 0;
    const unsigned target = older_copy(s, slot, &seq);
    uint8_t *r = rec_at(s, slot, target);
    memset(r, 0, MS_SAVE_COPY_HDR + len);
    if (len) memcpy(r + MS_SAVE_COPY_HDR, data, len);
    ms_wr32(r, MS_SAVE_COPY_MAGIC);
    ms_wr32(r + 4, seq);
    ms_wr16(r + 8, (uint16_t)len);
    ms_wr16(r + 10, ms_crc16(r + MS_SAVE_COPY_HDR, len));
    size_t k = 0;
    if (name) while (k < MS_SAVE_NAME - 1u && name[k]) { r[12 + k] = (uint8_t)name[k]; k++; }
    r[12 + k] = 0;
    ms_wr16(r + 24, fmt);
    ms_wr16(r + 26, 0);
    ms_wr64le_(r + 28, rec_tag(s, r));
    ms_wr16(r + 36, ms_crc16(r, 36));    /* commit point: written last */
    return MS_SAVE_OK;
}

int ms_save_erase(ms_save_t *s, unsigned slot)
{
    if (!s || !s->region) return MS_SAVE_E_PARAM;
    if (slot >= s->slots) return MS_SAVE_E_SLOT;
    if (s->read_only) return MS_SAVE_E_RO;
    if (s->ver == 1u) { const int rc = promote_legacy(s); if (rc != MS_SAVE_OK) return rc; }

    uint32_t seq = 0;
    const unsigned target = older_copy(s, slot, &seq);
    uint8_t *r = rec_at(s, slot, target);
    memset(r, 0, MS_SAVE_COPY_HDR);
    ms_wr32(r, MS_SAVE_COPY_MAGIC);
    ms_wr32(r + 4, seq);
    ms_wr64le_(r + 28, rec_tag(s, r));
    ms_wr16(r + 36, ms_crc16(r, 36));
    return MS_SAVE_OK;
}

int ms_save_list(const ms_save_t *s, ms_save_ent_t *out, unsigned cap, unsigned *n)
{
    if (!s || !s->region || !n) return MS_SAVE_E_PARAM;
    unsigned wrote = 0;
    for (unsigned slot = 0; slot < s->slots; slot++) {
        int states[2]; uint32_t seqs[2] = { 0, 0 }; uint16_t lens[2] = { 0, 0 }, fmts[2] = { 0, 0 };
        unsigned corrupt = 0;
        const unsigned best = pick_newest(s, slot, states, seqs, lens, fmts, &corrupt);
        ms_save_ent_t e;
        memset(&e, 0, sizeof e);
        e.slot = slot;
        e.valid = (best != 2 && states[best] == 1) ? 1u : 0u;
        if (e.valid) {
            const uint8_t *r = rec_at(s, slot, best);
            e.seq = seqs[best];
            e.len = lens[best];
            e.fmt = fmts[best];
            for (size_t k = 0; k < MS_SAVE_NAME && r[12 + k]; k++) e.name[k] = (char)r[12 + k];
        }
        if (out && wrote < cap) out[wrote] = e;
        wrote++;
    }
    *n = wrote;
    return MS_SAVE_OK;
}

int ms_save_verify(const ms_save_t *s, unsigned *flags)
{
    if (flags) *flags = 0;
    if (!s || !s->region) return MS_SAVE_E_PARAM;
    if (ms_rd32(s->region) != MS_SAVE_MAGIC) return MS_SAVE_E_MAGIC;
    const unsigned ver = ms_rd16(s->region + 4);
    if (ver != 1u && ver != MS_SAVE_VERSION) return MS_SAVE_E_MAGIC;
    if (ms_rd16(s->region + 6) != (uint16_t)s->bytes) return MS_SAVE_E_SIZE;
    if (ver == 1u) {
        if (ms_rd32(s->region + 10) != s->cart_crc) return MS_SAVE_E_CART;
        if (flags) *flags |= MS_SAVE_F_LEGACY;
    } else if (ms_rd32(s->region + 20) != s->cart_crc && s->loc == MS_SAVE_LOC_CART) {
        return MS_SAVE_E_CART;
    }

    int live = 0;
    for (unsigned slot = 0; slot < s->slots; slot++) {
        int states[2]; uint32_t seqs[2] = { 0, 0 }; uint16_t lens[2] = { 0, 0 }, fmts[2] = { 0, 0 };
        unsigned corrupt = 0;
        const unsigned best = pick_newest(s, slot, states, seqs, lens, fmts, &corrupt);
        if (corrupt) {
            if (flags) *flags |= MS_SAVE_F_TORN;
            if (best != 2 && flags) *flags |= MS_SAVE_F_FELLBACK;
        }
        if (best != 2 && states[best] == 1) live++;
    }
    return live;
}

/* ---------------- two homes ---------------- */

void ms_save_store_clear(ms_save_store_t *st)
{
    if (!st) return;
    memset(st, 0, sizeof *st);
}

int ms_save_store_attach(ms_save_store_t *st, const ms_save_t *img)
{
    if (!st || !img || !img->region) return MS_SAVE_E_PARAM;
    if (img->loc == MS_SAVE_LOC_CART) { st->img[1] = *img; st->present |= 2u; return MS_SAVE_OK; }
    st->img[0] = *img; st->present |= 1u;
    return MS_SAVE_OK;
}

int ms_save_store_has(const ms_save_store_t *st, unsigned loc)
{
    if (!st) return 0;
    if (loc == MS_SAVE_LOC_CART) return (st->present & 2u) != 0;
    if (loc == MS_SAVE_LOC_CONSOLE) return (st->present & 1u) != 0;
    return st->present != 0;
}

static const ms_save_t *home(const ms_save_store_t *st, unsigned loc)
{
    if (!st) return NULL;
    if (loc == MS_SAVE_LOC_CART) return (st->present & 2u) ? &st->img[1] : NULL;
    return (st->present & 1u) ? &st->img[0] : NULL;
}

int ms_save_store_read(const ms_save_store_t *st, unsigned slot, char *name,
                       size_t namecap, void *buf, size_t cap, size_t *len,
                       uint16_t *fmt, unsigned *loc_used, unsigned *flags)
{
    if (flags) *flags = 0;
    if (loc_used) *loc_used = 0;
    if (!ms_save_store_has(st, MS_SAVE_LOC_ANY)) return MS_SAVE_E_PARAM;

    const ms_save_t *pick = NULL;
    uint32_t best_seq = 0;
    for (unsigned i = 0; i < 2; i++) {
        if (!(st->present & (1u << i))) continue;
        const ms_save_t *t = &st->img[i];
        int states[2]; uint32_t sq[2] = { 0, 0 }; uint16_t ln[2] = { 0, 0 }, fx[2] = { 0, 0 };
        unsigned corrupt = 0;
        const unsigned w = pick_newest(t, slot, states, sq, ln, fx, &corrupt);
        if (w == 2 || states[w] != 1) continue;
        if (!pick || sq[w] > best_seq) { pick = t; best_seq = sq[w]; }
    }
    if (!pick) return MS_SAVE_E_EMPTY;
    if (loc_used) *loc_used = pick->loc;
    return ms_save_read(pick, slot, name, namecap, buf, cap, len, fmt, flags);
}

int ms_save_store_write(ms_save_store_t *st, unsigned loc, unsigned slot,
                        const char *name, uint16_t fmt, const void *data, size_t len)
{
    if (!st) return MS_SAVE_E_PARAM;
    ms_save_t *t = (ms_save_t *)(loc == MS_SAVE_LOC_CART ? home(st, MS_SAVE_LOC_CART)
                                                         : home(st, MS_SAVE_LOC_CONSOLE));
    if (!t) return MS_SAVE_E_LOC;
    return ms_save_write(t, slot, name, fmt, data, len);
}

int ms_save_store_erase(ms_save_store_t *st, unsigned slot)
{
    if (!st) return MS_SAVE_E_PARAM;
    int rc = MS_SAVE_E_EMPTY;
    for (unsigned i = 0; i < 2; i++) {
        if (!(st->present & (1u << i))) continue;
        const int r = ms_save_erase(&st->img[i], slot);
        if (r == MS_SAVE_OK) rc = MS_SAVE_OK;
        else if (rc == MS_SAVE_E_EMPTY) rc = r;
    }
    return rc;
}

/* ---------------- cart declaration ---------------- */

int ms_save_cfg_from_cart(const ms_cart_t *c, ms_save_cfg_t *out)
{
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    out->bytes = MS_SAVE_BYTES;
    out->slots = MS_SAVE_SLOTS;
    out->decl = MS_SAVE_DECL_CONSOLE;
    if (!c) return 0;

    ms_asset_t a;
    int declared = 0;
    if (ms_cart_find_kind(c, MS_A_SAVE, &a) == MS_CART_OK && a.size == 4) {
        const uint8_t *d = (const uint8_t *)a.data;
        const unsigned b = ms_rd16(d), sl = d[2], fl = d[3];
        if (b >= MS_SAVE_BYTES_MIN && b <= MS_SAVE_BYTES_MAX &&
            sl >= 1u && sl <= MS_SAVE_SLOTS_MAX && (fl & 3u)) {
            out->bytes = b; out->slots = sl; out->decl = fl & 3u;
            declared = 1;
        }
    }

    if (ms_cart_find_kind(c, MS_A_MANIFEST, &a) == MS_CART_OK) {
        /* The manifest is the text key=value asset; pull only what identity
         * needs, so this stays independent of the ms_game parser. */
        const char *t = (const char *)a.data;
        size_t n = a.size;
        char val[40];
        for (size_t i = 0; i < n; ) {
            size_t j = i;
            while (j < n && t[j] != '\n' && t[j] != '\r') j++;
            size_t eq = i;
            while (eq < j && t[eq] != '=') eq++;
            if (eq < j) {
                const size_t kl = eq - i, vl = j - eq - 1;
                if (vl && vl < sizeof val) {
                    memcpy(val, t + eq + 1, vl); val[vl] = 0;
                    if (kl == 3 && !memcmp(t + i, "dev", 3)) snprintf(out->dev, sizeof out->dev, "%s", val);
                    else if (kl == 7 && !memcmp(t + i, "save_id", 7)) snprintf(out->save_id, sizeof out->save_id, "%s", val);
                    else if (kl == 7 && !memcmp(t + i, "dev_key", 7)) snprintf(out->dev_key, sizeof out->dev_key, "%s", val);
                    else if (kl == 10 && !memcmp(t + i, "save_reads", 10)) {
                        const char *r = val;
                        while (*r && out->n_reads < MS_SAVE_READS_MAX) {
                            char one[24]; int k = 0;
                            while (*r == ' ' || *r == ',') r++;
                            while (*r && *r != ' ' && *r != ',' && k < 23) one[k++] = *r++;
                            one[k] = 0;
                            if (k) out->reads[out->n_reads++] = ms_id32(one);
                        }
                    }
                }
            }
            i = j;
            if (i < n) i++;
        }
    }
    out->dev_id = ms_id32(out->dev);
    out->app_id = ms_id32(out->save_id[0] ? out->save_id : out->dev);
    return declared;
}

/* ---------------- transfer bundle ---------------- */

#define BUNDLE_HDR 20u
#define BUNDLE_REC 22u

int ms_save_bundle_export(const ms_save_t *s, uint32_t device_id,
                          void *out, size_t cap, size_t *len)
{
    if (len) *len = 0;
    if (!s || !s->region || !out || !len) return MS_SAVE_E_PARAM;

    size_t need = BUNDLE_HDR;
    unsigned count = 0;
    ms_save_ent_t e[MS_SAVE_SLOTS_MAX];
    unsigned n = 0;
    ms_save_list(s, e, MS_SAVE_SLOTS_MAX, &n);
    for (unsigned i = 0; i < n; i++) if (e[i].valid) { need += BUNDLE_REC + e[i].len; count++; }
    need += 4u;
    *len = need;
    if (need > cap) return MS_SAVE_E_NOSPACE;

    uint8_t *p = (uint8_t *)out;
    memset(p, 0, BUNDLE_HDR);
    ms_wr32(p, MS_SAVE_BUNDLE_MAGIC);
    ms_wr16(p + 4, 2u);
    ms_wr16(p + 6, (uint16_t)count);
    ms_wr32(p + 8, s->app_id);
    ms_wr32(p + 12, device_id);
    ms_wr16(p + 16, (uint16_t)s->dev_id);
    ms_wr16(p + 18, (uint16_t)(s->dev_id >> 16));
    size_t at = BUNDLE_HDR;
    int states[2]; uint32_t seqs[2]; uint16_t lens[2], fmts[2];
    for (unsigned slot = 0; slot < s->slots; slot++) {
        unsigned corrupt = 0;
        const unsigned w = pick_newest(s, slot, states, seqs, lens, fmts, &corrupt);
        if (w == 2 || states[w] != 1) continue;
        const uint8_t *r = rec_at(s, slot, w);
        p[at] = (uint8_t)slot;
        p[at + 1] = 0;
        ms_wr16(p + at + 2, lens[w]);
        ms_wr16(p + at + 4, ms_rd16(r + 10));
        ms_wr16(p + at + 6, fmts[w]);
        memcpy(p + at + 8, r + 12, MS_SAVE_NAME);
        p[at + 20] = (uint8_t)seqs[w];
        p[at + 21] = 0;
        memcpy(p + at + BUNDLE_REC, r + MS_SAVE_COPY_HDR, lens[w]);
        at += BUNDLE_REC + lens[w];
    }
    ms_wr32(p + at, ms_crc32(p, at));
    return MS_SAVE_OK;
}

int ms_save_bundle_import(ms_save_t *s, const void *blob, size_t bytes, unsigned *flags)
{
    if (flags) *flags = 0;
    if (!s || !s->region || !blob) return MS_SAVE_E_PARAM;
    if (s->read_only) return MS_SAVE_E_RO;
    const uint8_t *p = (const uint8_t *)blob;
    if (bytes < BUNDLE_HDR + 4u) return MS_SAVE_E_SIZE;
    if (ms_rd32(p) != MS_SAVE_BUNDLE_MAGIC) return MS_SAVE_E_MAGIC;
    const unsigned ver = ms_rd16(p + 4);
    if (ver != 1u && ver != 2u) return MS_SAVE_E_MAGIC;
    const unsigned rec = ver == 1u ? 18u : BUNDLE_REC;
    if (ms_crc32(p, bytes - 4u) != ms_rd32(p + bytes - 4u)) return MS_SAVE_E_CRC;
    if (ver == 2u) {
        const uint32_t dev = (uint32_t)ms_rd16(p + 16) | ((uint32_t)ms_rd16(p + 18) << 16);
        if (dev && s->dev_id && dev != s->dev_id) return MS_SAVE_E_DEV;
        if (ms_rd32(p + 8) != s->app_id) {
            int linked = 0;
            (void)linked;   /* a bundle import is a write: never linked */
            return MS_SAVE_E_APP;
        }
    }

    const unsigned count = ms_rd16(p + 6);
    size_t at = BUNDLE_HDR;
    for (unsigned i = 0; i < count; i++) {
        if (at + rec > bytes - 4u) return MS_SAVE_E_SIZE;
        const unsigned slot = p[at];
        const unsigned l = ms_rd16(p + at + 2);
        if (at + rec + l > bytes - 4u) return MS_SAVE_E_SIZE;
        if (slot >= s->slots) return MS_SAVE_E_SLOT;
        if (ms_crc16(p + at + rec, l) != ms_rd16(p + at + 4)) return MS_SAVE_E_CRC;
        char name[MS_SAVE_NAME + 1];
        memset(name, 0, sizeof name);
        memcpy(name, p + at + (ver == 1u ? 6u : 8u), MS_SAVE_NAME);
        const uint16_t fmt = ver == 1u ? 0u : ms_rd16(p + at + 6);
        const int rc = ms_save_write(s, slot, name, fmt, p + at + rec, l);
        if (rc != MS_SAVE_OK) return rc;
        at += rec + l;
    }
    return MS_SAVE_OK;
}
