/* SPDX-License-Identifier: Apache-2.0 */
/* Mosaic — ms_save.c: the save image, in place, no I/O. */
#include "mosaic/ms_save.h"
#include <string.h>

const char *ms_save_err_str(int e)
{
    switch (e) {
    case MS_SAVE_OK:        return "ok";
    case MS_SAVE_E_PARAM:   return "bad argument";
    case MS_SAVE_E_MAGIC:   return "not a save image";
    case MS_SAVE_E_SIZE:    return "image size does not match";
    case MS_SAVE_E_CRC:     return "integrity check failed";
    case MS_SAVE_E_CART:    return "save belongs to a different cart";
    case MS_SAVE_E_EMPTY:   return "slot is empty";
    case MS_SAVE_E_SLOT:    return "slot out of range";
    case MS_SAVE_E_BODY:    return "body too large for the slot";
    case MS_SAVE_E_NOSPACE: return "output buffer too small";
    }
    return "unknown";
}

/* ---------------- copy records ---------------- */

static uint8_t *rec_at(const ms_save_t *s, unsigned slot, unsigned which)
{
    return s->region + MS_SAVE_HDR + (size_t)(slot * 2u + which) * s->stride;
}

/* 0 unused, 1 body, 2 tombstone, 3 present but failed a check. */
static int rec_state(const ms_save_t *s, const uint8_t *r, uint32_t *seq, uint16_t *len)
{
    if (ms_rd32(r) != MS_SAVE_COPY_MAGIC) return 0;
    if (ms_rd16(r + 26) != 0u) return 3;
    if (ms_crc16(r, 24) != ms_rd16(r + 24)) return 3;
    const uint16_t l = ms_rd16(r + 8);
    if (l > s->max_body) return 3;
    if (l != 0 && ms_crc16(r + MS_SAVE_COPY_HDR, l) != ms_rd16(r + 10)) return 3;
    *seq = ms_rd32(r + 4);
    *len = l;
    return l == 0 ? 2 : 1;
}

static unsigned pick_newest(const ms_save_t *s, unsigned slot, int states[2],
                            uint32_t seqs[2], uint16_t lens[2], unsigned *corrupt)
{
    *corrupt = 0;
    unsigned best = 2, best_seq = 0;
    for (unsigned w = 0; w < 2; w++) {
        const uint8_t *r = rec_at(s, slot, w);
        states[w] = rec_state(s, r, &seqs[w], &lens[w]);
        if (states[w] == 3) { *corrupt = 1; continue; }
        if (states[w] == 0) continue;
        if (best == 2 || seqs[w] > best_seq) { best = w; best_seq = seqs[w]; }
    }
    return best;
}

/* ---------------- image ---------------- */

int ms_save_format(ms_save_t *s, void *region, size_t bytes, uint32_t cart_crc,
                   unsigned slots, unsigned *flags)
{
    if (flags) *flags = 0;
    if (!s || !region) return MS_SAVE_E_PARAM;
    if (slots < 1u || slots > MS_SAVE_SLOTS_MAX) return MS_SAVE_E_PARAM;
    if (bytes < MS_SAVE_BYTES_MIN || bytes > MS_SAVE_BYTES_MAX) return MS_SAVE_E_SIZE;
    if (bytes > 0xFFFFu) return MS_SAVE_E_SIZE;
    const size_t stride = (bytes - MS_SAVE_HDR) / (size_t)(slots * 2u);
    if (stride < MS_SAVE_COPY_HDR + 16u) return MS_SAVE_E_SIZE;

    uint8_t *p = (uint8_t *)region;
    memset(p, 0, bytes);
    ms_wr32(p, MS_SAVE_MAGIC);
    ms_wr16(p + 4, (uint16_t)MS_SAVE_VERSION);
    ms_wr16(p + 6, (uint16_t)bytes);
    p[8] = (uint8_t)slots;
    p[9] = 0;
    ms_wr32(p + 10, cart_crc);

    s->region = p; s->bytes = bytes; s->cart_crc = cart_crc;
    s->slots = slots; s->stride = (unsigned)stride;
    s->max_body = (unsigned)(stride - MS_SAVE_COPY_HDR);
    if (flags) *flags |= MS_SAVE_F_INIT;
    return MS_SAVE_OK;
}

static int image_blank(const uint8_t *p, size_t bytes)
{
    for (size_t i = 0; i < bytes; i++) if (p[i] != 0x00 && p[i] != 0xFF) return 0;
    return 1;
}

int ms_save_open(ms_save_t *s, void *region, size_t bytes, uint32_t cart_crc,
                 unsigned *flags)
{
    if (flags) *flags = 0;
    if (!s || !region) return MS_SAVE_E_PARAM;
    if (bytes < MS_SAVE_BYTES_MIN || bytes > MS_SAVE_BYTES_MAX) return MS_SAVE_E_SIZE;
    uint8_t *p = (uint8_t *)region;

    if (image_blank(p, bytes))
        return ms_save_format(s, region, bytes, cart_crc, MS_SAVE_SLOTS, flags);

    if (ms_rd32(p) != MS_SAVE_MAGIC) return MS_SAVE_E_MAGIC;
    if (ms_rd16(p + 4) != MS_SAVE_VERSION) return MS_SAVE_E_MAGIC;
    if (ms_rd16(p + 6) != (uint16_t)bytes) return MS_SAVE_E_SIZE;
    const unsigned slots = p[8];
    if (slots < 1u || slots > MS_SAVE_SLOTS_MAX) return MS_SAVE_E_MAGIC;
    if (bytes > 0xFFFFu) return MS_SAVE_E_SIZE;
    const size_t stride = (bytes - MS_SAVE_HDR) / (size_t)(slots * 2u);
    if (stride < MS_SAVE_COPY_HDR + 16u) return MS_SAVE_E_SIZE;

    s->region = p; s->bytes = bytes; s->cart_crc = cart_crc;
    s->slots = slots; s->stride = (unsigned)stride;
    s->max_body = (unsigned)(stride - MS_SAVE_COPY_HDR);

    /* The cart binding is checked after the structural fields, so a wrong
     * cart reports "different cart" rather than "corrupt image". */
    if (ms_rd32(p + 10) != cart_crc) return MS_SAVE_E_CART;
    return MS_SAVE_OK;
}

/* ---------------- slots ---------------- */

int ms_save_read(const ms_save_t *s, unsigned slot, char *name, size_t namecap,
                 void *buf, size_t cap, size_t *len, unsigned *flags)
{
    if (flags) *flags = 0;
    if (!s || !s->region) return MS_SAVE_E_PARAM;
    if (slot >= s->slots) return MS_SAVE_E_SLOT;
    if (len) *len = 0;

    int states[2]; uint32_t seqs[2] = { 0, 0 }; uint16_t lens[2] = { 0, 0 };
    unsigned corrupt = 0;
    const unsigned best = pick_newest(s, slot, states, seqs, lens, &corrupt);

    if (best == 2) {
        if (corrupt) { if (flags) *flags |= MS_SAVE_F_FELLBACK | MS_SAVE_F_TORN; return MS_SAVE_E_CRC; }
        return MS_SAVE_E_EMPTY;
    }
    if (states[best] == 2) return MS_SAVE_E_EMPTY;     /* tombstone */
    if (corrupt) { if (flags) *flags |= MS_SAVE_F_FELLBACK | MS_SAVE_F_TORN; }

    const uint8_t *r = rec_at(s, slot, best);
    const uint16_t l = lens[best];
    if (name && namecap) {
        size_t k = 0;
        while (k < MS_SAVE_NAME && k + 1u < namecap && r[12 + k]) { name[k] = (char)r[12 + k]; k++; }
        name[k] = 0;
    }
    if (len) *len = l;
    if (!buf || cap == 0) return MS_SAVE_OK;
    const size_t n = l < cap ? l : cap;
    if (n) memcpy(buf, r + MS_SAVE_COPY_HDR, n);
    return n < l ? MS_SAVE_E_NOSPACE : MS_SAVE_OK;
}

int ms_save_write(ms_save_t *s, unsigned slot, const char *name,
                  const void *data, size_t len)
{
    if (!s || !s->region) return MS_SAVE_E_PARAM;
    if (slot >= s->slots) return MS_SAVE_E_SLOT;
    if (len > s->max_body) return MS_SAVE_E_BODY;
    if (len == 0) return ms_save_erase(s, slot);

    int states[2]; uint32_t seqs[2] = { 0, 0 }; uint16_t lens[2] = { 0, 0 };
    unsigned corrupt = 0;
    (void)pick_newest(s, slot, states, seqs, lens, &corrupt);

    /* Write to the older (or unused) copy so the newest survives a cut. */
    unsigned target;
    if (states[0] && states[0] != 3 && states[1] && states[1] != 3)
        target = seqs[0] <= seqs[1] ? 0u : 1u;
    else if (states[0] && states[0] != 3)
        target = 1u;
    else
        target = 0u;

    const uint32_t a = (states[0] && states[0] != 3) ? seqs[0] : 0u;
    const uint32_t b = (states[1] && states[1] != 3) ? seqs[1] : 0u;
    const uint32_t seq = (a > b ? a : b) + 1u;

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
    ms_wr16(r + 26, 0);
    ms_wr16(r + 24, ms_crc16(r, 24));   /* commit point: written last */
    return MS_SAVE_OK;
}

int ms_save_erase(ms_save_t *s, unsigned slot)
{
    if (!s || !s->region) return MS_SAVE_E_PARAM;
    if (slot >= s->slots) return MS_SAVE_E_SLOT;

    int states[2]; uint32_t seqs[2] = { 0, 0 }; uint16_t lens[2] = { 0, 0 };
    unsigned corrupt = 0;
    (void)pick_newest(s, slot, states, seqs, lens, &corrupt);

    unsigned target;
    if (states[0] && states[0] != 3 && states[1] && states[1] != 3)
        target = seqs[0] <= seqs[1] ? 0u : 1u;
    else if (states[0] && states[0] != 3)
        target = 1u;
    else
        target = 0u;

    const uint32_t a = (states[0] && states[0] != 3) ? seqs[0] : 0u;
    const uint32_t b = (states[1] && states[1] != 3) ? seqs[1] : 0u;
    const uint32_t seq = (a > b ? a : b) + 1u;

    uint8_t *r = rec_at(s, slot, target);
    memset(r, 0, MS_SAVE_COPY_HDR);
    ms_wr32(r, MS_SAVE_COPY_MAGIC);
    ms_wr32(r + 4, seq);
    ms_wr16(r + 24, ms_crc16(r, 24));
    return MS_SAVE_OK;
}

int ms_save_list(const ms_save_t *s, ms_save_ent_t *out, unsigned cap, unsigned *n)
{
    if (!s || !s->region || !n) return MS_SAVE_E_PARAM;
    unsigned wrote = 0;
    for (unsigned slot = 0; slot < s->slots; slot++) {
        int states[2]; uint32_t seqs[2] = { 0, 0 }; uint16_t lens[2] = { 0, 0 };
        unsigned corrupt = 0;
        const unsigned best = pick_newest(s, slot, states, seqs, lens, &corrupt);
        ms_save_ent_t e;
        memset(&e, 0, sizeof e);
        e.slot = slot;
        e.valid = (best != 2 && states[best] != 2) ? 1u : 0u;
        if (e.valid) {
            const uint8_t *r = rec_at(s, slot, best);
            e.seq = seqs[best];
            e.len = lens[best];
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
    if (ms_rd16(s->region + 4) != MS_SAVE_VERSION) return MS_SAVE_E_MAGIC;
    if (ms_rd16(s->region + 6) != (uint16_t)s->bytes) return MS_SAVE_E_SIZE;
    if (ms_rd32(s->region + 10) != s->cart_crc) return MS_SAVE_E_CART;

    int live = 0;
    for (unsigned slot = 0; slot < s->slots; slot++) {
        int states[2]; uint32_t seqs[2] = { 0, 0 }; uint16_t lens[2] = { 0, 0 };
        unsigned corrupt = 0;
        const unsigned best = pick_newest(s, slot, states, seqs, lens, &corrupt);
        if (corrupt) {
            if (flags) *flags |= MS_SAVE_F_TORN;
            if (best != 2) { if (flags) *flags |= MS_SAVE_F_FELLBACK; }
        }
        if (best != 2 && states[best] == 1) live++;
    }
    return live;
}

int ms_save_cfg_from_cart(const ms_cart_t *c, unsigned *bytes, unsigned *slots)
{
    if (bytes) *bytes = MS_SAVE_BYTES;
    if (slots) *slots = MS_SAVE_SLOTS;
    if (!c) return 0;
    ms_asset_t a;
    if (ms_cart_find_kind(c, MS_A_SAVE, &a) != MS_CART_OK) return 0;
    const uint8_t *d = (const uint8_t *)a.data;
    if (a.size != 4) return 0;
    const unsigned b = ms_rd16(d), sl = d[2];
    if (b < MS_SAVE_BYTES_MIN || b > MS_SAVE_BYTES_MAX) return 0;
    if (sl < 1u || sl > MS_SAVE_SLOTS_MAX) return 0;
    if (bytes) *bytes = b;
    if (slots) *slots = sl;
    return 1;
}

/* ---------------- transfer bundle ---------------- */

/* Header 20 B, then records, then a u32 crc32 of everything before it.
 * This is an integrity check, not a signature: core has no key material,
 * so a hand-edited bundle that recomputes the CRC will pass. Say so in the
 * docs rather than implying authenticity. */
#define BUNDLE_HDR 20u
#define BUNDLE_REC 18u

int ms_save_bundle_export(const ms_save_t *s, uint32_t device_id,
                          void *out, size_t cap, size_t *len)
{
    if (len) *len = 0;
    if (!s || !s->region || !out || !len) return MS_SAVE_E_PARAM;

    size_t need = BUNDLE_HDR;
    unsigned count = 0;
    for (unsigned slot = 0; slot < s->slots; slot++) {
        int states[2]; uint32_t seqs[2] = { 0, 0 }; uint16_t lens[2] = { 0, 0 };
        unsigned corrupt = 0;
        const unsigned best = pick_newest(s, slot, states, seqs, lens, &corrupt);
        if (best == 2 || states[best] != 1) continue;
        need += BUNDLE_REC + lens[best];
        count++;
    }
    need += 4u;
    *len = need;
    if (need > cap) return MS_SAVE_E_NOSPACE;

    uint8_t *p = (uint8_t *)out;
    memset(p, 0, BUNDLE_HDR);
    ms_wr32(p, MS_SAVE_BUNDLE_MAGIC);
    ms_wr16(p + 4, 1u);
    ms_wr16(p + 6, (uint16_t)count);
    ms_wr32(p + 8, s->cart_crc);
    ms_wr32(p + 12, device_id);
    size_t at = BUNDLE_HDR;
    for (unsigned slot = 0; slot < s->slots; slot++) {
        int states[2]; uint32_t seqs[2] = { 0, 0 }; uint16_t lens[2] = { 0, 0 };
        unsigned corrupt = 0;
        const unsigned best = pick_newest(s, slot, states, seqs, lens, &corrupt);
        if (best == 2 || states[best] != 1) continue;
        const uint8_t *r = rec_at(s, slot, best);
        const uint16_t l = lens[best];
        p[at] = (uint8_t)slot; p[at + 1] = 0;
        ms_wr16(p + at + 2, l);
        ms_wr16(p + at + 4, ms_crc16(r + MS_SAVE_COPY_HDR, l));
        memcpy(p + at + 6, r + 12, MS_SAVE_NAME);
        memcpy(p + at + BUNDLE_REC, r + MS_SAVE_COPY_HDR, l);
        at += BUNDLE_REC + l;
    }
    ms_wr32(p + 16, (uint32_t)((at - BUNDLE_HDR) + 4u));
    ms_wr32(p + at, ms_crc32(p, at));
    return MS_SAVE_OK;
}

int ms_save_bundle_import(ms_save_t *s, const void *blob, size_t bytes, unsigned *flags)
{
    if (flags) *flags = 0;
    if (!s || !s->region || !blob) return MS_SAVE_E_PARAM;
    const uint8_t *p = (const uint8_t *)blob;
    if (bytes < BUNDLE_HDR + 4u) return MS_SAVE_E_SIZE;
    if (ms_rd32(p) != MS_SAVE_BUNDLE_MAGIC) return MS_SAVE_E_MAGIC;
    if (ms_rd16(p + 4) != 1u) return MS_SAVE_E_MAGIC;
    const size_t body = BUNDLE_HDR + ms_rd32(p + 16);
    if (body != bytes) return MS_SAVE_E_SIZE;
    if (ms_crc32(p, bytes - 4u) != ms_rd32(p + bytes - 4u)) return MS_SAVE_E_CRC;
    if (ms_rd32(p + 8) != s->cart_crc) return MS_SAVE_E_CART;

    const unsigned count = ms_rd16(p + 6);
    size_t at = BUNDLE_HDR;
    for (unsigned i = 0; i < count; i++) {
        if (at + BUNDLE_REC > bytes - 4u) return MS_SAVE_E_SIZE;
        const unsigned slot = p[at];
        const unsigned l = ms_rd16(p + at + 2);
        if (at + BUNDLE_REC + l > bytes - 4u) return MS_SAVE_E_SIZE;
        if (slot >= s->slots) return MS_SAVE_E_SLOT;
        if (ms_crc16(p + at + BUNDLE_REC, l) != ms_rd16(p + at + 4)) return MS_SAVE_E_CRC;
        char name[MS_SAVE_NAME + 1];
        memset(name, 0, sizeof name);
        memcpy(name, p + at + 6, MS_SAVE_NAME);
        name[MS_SAVE_NAME] = 0;
        const int rc = ms_save_write(s, slot, name, p + at + BUNDLE_REC, l);
        if (rc != MS_SAVE_OK) return rc;
        at += BUNDLE_REC + l;
    }
    return MS_SAVE_OK;
}
