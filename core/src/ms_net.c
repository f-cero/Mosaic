/* SPDX-License-Identifier: Apache-2.0 */
/* Mosaic — ms_net.c: MSP/1 session layer. No allocation, no stdio. */
#include "mosaic/ms_net.h"
#include <string.h>

#define HDR5 5u          /* u32 frame + u8 record count */
#define KF_FIXED 49u     /* header (23) + two 13-byte bg configs */
#define KF_OAM   13u
#define REC_MAX  MS_LINK_MTU

static uint64_t mix(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

/* ---- small payload codecs ------------------------------------------- */

size_t ms_net_encode_join(uint8_t *out, size_t cap, const ms_net_join_t *j)
{
    if (!out || !j || cap < 4u + MS_MSP_NAME) return 0;
    ms_wr16(out, j->ver ? j->ver : MS_MSP_VERSION);
    out[2] = j->node; out[3] = j->role;
    memset(out + 4, 0, MS_MSP_NAME);
    size_t n = strlen(j->name);
    if (n > MS_MSP_NAME) n = MS_MSP_NAME;
    memcpy(out + 4, j->name, n);
    return 4u + MS_MSP_NAME;
}

int ms_net_decode_join(ms_net_join_t *j, const uint8_t *in, size_t n)
{
    if (!j || !in || n < 4u + MS_MSP_NAME) return -1;
    j->ver = ms_rd16(in);
    if (j->ver != MS_MSP_VERSION) return -2;
    j->node = in[2]; j->role = in[3];
    memcpy(j->name, in + 4, MS_MSP_NAME);
    j->name[MS_MSP_NAME] = 0;
    return 0;
}

size_t ms_net_encode_session(uint8_t *out, size_t cap, const ms_net_session_t *s)
{
    if (!out || !s || cap < 48) return 0;
    out[0] = s->node; out[1] = s->flags;
    ms_wr16(out + 2, s->fps);
    ms_wr16(out + 4, s->mtu);
    ms_wr32(out + 6, s->frame);
    ms_wr32(out + 10, s->cart_crc);
    ms_wr32(out + 14, s->save_slots);
    memset(out + 18, 0, 32);
    size_t n = strlen(s->title);
    if (n > 32) n = 32;
    memcpy(out + 18, s->title, n);
    return 50;
}

int ms_net_decode_session(ms_net_session_t *s, const uint8_t *in, size_t n)
{
    if (!s || !in || n < 50) return -1;
    s->node = in[0]; s->flags = in[1];
    s->fps = ms_rd16(in + 2); s->mtu = ms_rd16(in + 4);
    s->frame = ms_rd32(in + 6); s->cart_crc = ms_rd32(in + 10);
    s->save_slots = ms_rd32(in + 14);
    memcpy(s->title, in + 18, 32); s->title[32] = 0;
    return 0;
}

size_t ms_net_encode_state(uint8_t *out, size_t cap, const ms_net_state_t *s)
{
    if (!out || !s || cap < 16) return 0;
    ms_wr32(out, s->frame); ms_wr32(out + 4, s->applied);
    ms_wr32(out + 8, (uint32_t)s->hash);
    ms_wr32(out + 12, (uint32_t)(s->hash >> 32));
    return 16;
}

int ms_net_decode_state(ms_net_state_t *s, const uint8_t *in, size_t n)
{
    if (!s || !in || n < 16) return -1;
    s->frame = ms_rd32(in); s->applied = ms_rd32(in + 4);
    s->hash = ((uint64_t)ms_rd32(in + 12) << 32) | (uint64_t)ms_rd32(in + 8);
    return 0;
}

/* ---- keyframe -------------------------------------------------------- */

size_t ms_net_keyframe_size(uint16_t oam_count)
{
    if (oam_count > MS_MAX_SPRITES) oam_count = MS_MAX_SPRITES;
    return KF_FIXED + 1024u + MS_MAX_MATRICES * 16u + (size_t)oam_count * KF_OAM;
}

static int sheet_of(const ms_game_t *g, const ms_oam_t *o)
{
    if (!g || !o->tiles) return MS_MSP_NO_SHEET;
    for (unsigned i = 0; i < g->sheet_count && i < MS_MAX_SHEETS; i++)
        if (g->sheets[i].tiles == o->tiles) return (int)i;
    return MS_MSP_NO_SHEET;
}

size_t ms_net_encode_keyframe(const ms_game_t *g, const ms_ppu_t *p, void *out, size_t cap)
{
    if (!p || !out) return 0;
    const size_t need = ms_net_keyframe_size(p->oam_count);
    if (need > cap || need > MS_LINK_MTU) return 0;
    uint8_t *b = (uint8_t *)out;
    size_t o = 0;

    ms_wr32(b, p->frame);
    ms_wr16(b + 4, (uint16_t)p->mode);
    ms_wr16(b + 6, p->width);
    ms_wr16(b + 8, p->height);
    ms_wr16(b + 10, p->fx.backdrop);
    b[12] = p->fx.brightness; b[13] = p->fx.mosaic;
    b[14] = p->fx.math_on;    b[15] = p->fx.color_math;
    ms_wr32(b + 16, (uint32_t)p->fx.math_factor);
    ms_wr16(b + 20, p->oam_count);
    b[22] = (uint8_t)((p->bg[0].enabled ? 1u : 0u) | (p->bg[1].enabled ? 2u : 0u));
    o = 23;
    for (unsigned i = 0; i < MS_MAX_BG; i++) {
        const ms_bg_t *bg = &p->bg[i];
        b[o++] = bg->enabled; b[o++] = bg->priority; b[o++] = bg->mosaic;
        b[o++] = bg->math;    b[o++] = bg->bpp;
        b[o++] = bg->map_w;   b[o++] = bg->map_h;
        b[o++] = bg->logical_w; b[o++] = bg->logical_h;
        ms_wr16(b + o, (uint16_t)bg->scroll_x); o += 2;
        ms_wr16(b + o, (uint16_t)bg->scroll_y); o += 2;
    }
    for (unsigned i = 0; i < MS_PAL_ENTRIES; i++) { ms_wr16(b + o, p->pal_bg[i]);  o += 2; }
    for (unsigned i = 0; i < MS_PAL_ENTRIES; i++) { ms_wr16(b + o, p->pal_spr[i]); o += 2; }
    for (unsigned i = 0; i < MS_MAX_MATRICES; i++) { memcpy(b + o, &p->mat[i], 16); o += 16; }
    for (uint16_t i = 0; i < p->oam_count; i++) {
        const ms_oam_t *s = &p->oam[i];
        ms_wr16(b + o, (uint16_t)s->x); o += 2;
        ms_wr16(b + o, (uint16_t)s->y); o += 2;
        b[o++] = s->w; b[o++] = s->h; b[o++] = s->bpp; b[o++] = s->pal_slot;
        b[o++] = (uint8_t)((s->priority & 3u) | (s->flags & 0xFCu));
        b[o++] = (uint8_t)(s->affine < 0 ? 0xFF : (uint8_t)s->affine);
        ms_wr16(b + o, s->line_rows); o += 2;
        b[o++] = (uint8_t)sheet_of(g, s);
    }
    return o;
}

/* ---- snapshot -------------------------------------------------------- */

static void bg_cfg_of(const ms_bg_t *bg, uint8_t out[9])
{
    out[0] = bg->enabled; out[1] = bg->priority; out[2] = bg->mosaic;
    out[3] = bg->math;    out[4] = bg->bpp;
    out[5] = bg->map_w;   out[6] = bg->map_h;
    out[7] = bg->logical_w; out[8] = bg->logical_h;
}

void ms_net_snap_init(ms_net_snap_t *s, const ms_ppu_t *p, ms_net_buf_t map_store[MS_MAX_BG])
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
    if (!p) return;
    memcpy(s->pal_bg, p->pal_bg, sizeof s->pal_bg);
    memcpy(s->pal_spr, p->pal_spr, sizeof s->pal_spr);
    for (unsigned i = 0; i < MS_MAX_BG; i++) bg_cfg_of(&p->bg[i], s->bg[i]);
    s->fx[0] = p->fx.brightness; s->fx[1] = p->fx.mosaic;
    s->fx[2] = p->fx.math_on;    s->fx[3] = p->fx.color_math;
    s->math_factor = (uint32_t)p->fx.math_factor;
    for (unsigned i = 0; i < MS_MAX_BG; i++) {
        if (!p->bg[i].map) continue;
        const size_t entries = (size_t)p->bg[i].map_w * p->bg[i].map_h;
        if (!map_store || !map_store[i].ptr || map_store[i].bytes < entries * 2u) continue;
        s->map[i] = (uint16_t *)map_store[i].ptr;
        s->map_entries[i] = (uint32_t)entries;
        memcpy(s->map[i], p->bg[i].map, entries * 2u);
    }
    for (unsigned i = 0; i < MS_MAX_SPRITES; i++) {
        const ms_oam_t *o = &p->oam[i];
        if (!o->line_mat || !o->line_rows) continue;
        s->tab_digest[i] = mix(1469598103934665603ull, o->line_mat,
                               (size_t)o->line_rows * 16u);
        if (!s->tab_digest[i]) s->tab_digest[i] = 1;
    }
    s->valid = 1;
}

/* ---- delta encoder --------------------------------------------------- */

size_t ms_net_encode_delta(const ms_game_t *g, const ms_ppu_t *p, ms_net_snap_t *s,
                           void *out, size_t cap)
{
    if (!p || !s || !out || cap < HDR5 + 1u || !s->valid) return 0;
    uint8_t *b = (uint8_t *)out;
    size_t o = HDR5;
    unsigned n = 0;

#define NEED(k) do { if (o + (k) > cap || o + (k) > REC_MAX) return 0; } while (0)

    for (unsigned i = 0; i < MS_PAL_ENTRIES; i++) {
        if (p->pal_bg[i] != s->pal_bg[i]) {
            NEED(5); b[o++] = 6; b[o++] = 0; b[o++] = (uint8_t)i;
            ms_wr16(b + o, p->pal_bg[i]); o += 2; n++;
        }
        if (p->pal_spr[i] != s->pal_spr[i]) {
            NEED(5); b[o++] = 6; b[o++] = 1; b[o++] = (uint8_t)i;
            ms_wr16(b + o, p->pal_spr[i]); o += 2; n++;
        }
    }
    for (unsigned i = 0; i < MS_MAX_BG; i++) {
        uint8_t cfg[9];
        bg_cfg_of(&p->bg[i], cfg);
        if (memcmp(cfg, s->bg[i], 9) != 0) {
            NEED(11); b[o++] = 7; b[o++] = (uint8_t)i;
            memcpy(b + o, cfg, 9); o += 9; n++;
        }
        if (!p->bg[i].enabled || !p->bg[i].map || !s->map[i]) continue;
        uint16_t *m = (uint16_t *)p->bg[i].map;
        const uint32_t ents = (uint32_t)p->bg[i].map_w * p->bg[i].map_h;
        for (uint32_t k = 0; k < ents && k < s->map_entries[i]; ) {
            if (m[k] == s->map[i][k]) { k++; continue; }
            const uint32_t first = k;
            while (k < ents && k < s->map_entries[i] && m[k] != s->map[i][k] &&
                   k - first < 2000u) k++;
            const uint32_t cnt = k - first;
            NEED(6u + (size_t)cnt * 2u);
            b[o++] = 9; b[o++] = (uint8_t)i;
            ms_wr16(b + o, (uint16_t)first); o += 2;
            ms_wr16(b + o, (uint16_t)cnt);   o += 2;
            for (uint32_t j = 0; j < cnt; j++) { ms_wr16(b + o, m[first + j]); o += 2; }
            n++;
        }
    }
    {
        uint8_t fxc[4];
        fxc[0] = p->fx.brightness; fxc[1] = p->fx.mosaic;
        fxc[2] = p->fx.math_on;    fxc[3] = p->fx.color_math;
        if (memcmp(fxc, s->fx, 4) != 0 || (uint32_t)p->fx.math_factor != s->math_factor) {
            NEED(9); b[o++] = 8; memcpy(b + o, fxc, 4); o += 4;
            ms_wr32(b + o, (uint32_t)p->fx.math_factor); o += 4; n++;
        }
    }
    for (unsigned i = 0; i < MS_MAX_BG; i++) {
        if (!p->bg[i].enabled) continue;
        NEED(6); b[o++] = 1; b[o++] = (uint8_t)i;
        ms_wr16(b + o, (uint16_t)p->bg[i].scroll_x); o += 2;
        ms_wr16(b + o, (uint16_t)p->bg[i].scroll_y); o += 2; n++;
    }
    NEED(3); b[o++] = 2; ms_wr16(b + o, p->fx.backdrop); o += 2; n++;

    NEED(2); b[o++] = 11; b[o++] = (uint8_t)(p->oam_count > 255u ? 255u : p->oam_count); n++;

    for (uint16_t i = 0; i < p->oam_count && i < MS_MAX_SPRITES; i++) {
        const ms_oam_t *sp = &p->oam[i];
        NEED(15);
        b[o++] = 10; b[o++] = (uint8_t)i;
        ms_wr16(b + o, (uint16_t)sp->x); o += 2;
        ms_wr16(b + o, (uint16_t)sp->y); o += 2;
        b[o++] = sp->w; b[o++] = sp->h; b[o++] = sp->bpp; b[o++] = sp->pal_slot;
        b[o++] = (uint8_t)((sp->priority & 3u) | (sp->flags & 0xFCu));
        b[o++] = (uint8_t)(sp->affine < 0 ? 0xFF : (uint8_t)sp->affine);
        ms_wr16(b + o, sp->line_rows); o += 2;
        b[o++] = (uint8_t)sheet_of(g, sp);
        n++;
    }
    for (unsigned m = 0; m < MS_MAX_MATRICES; m++) {
        NEED(18); b[o++] = 4; b[o++] = (uint8_t)m;
        memcpy(b + o, &p->mat[m], 16); o += 16; n++;
    }
    for (uint16_t i = 0; i < p->oam_count && i < MS_MAX_SPRITES; i++) {
        const ms_oam_t *sp = &p->oam[i];
        if (!sp->line_mat || !sp->line_rows) continue;
        const size_t bytes = (size_t)sp->line_rows * 16u;
        uint64_t d = mix(1469598103934665603ull, sp->line_mat, bytes);
        if (!d) d = 1;
        if (d == s->tab_digest[i]) continue;
        NEED(4u + bytes);
        b[o++] = 5; b[o++] = (uint8_t)i;
        ms_wr16(b + o, sp->line_rows); o += 2;
        memcpy(b + o, sp->line_mat, bytes); o += bytes;
        s->tab_digest[i] = d;
        n++;
    }
#undef NEED

    ms_wr32(b, p->frame);
    b[4] = (uint8_t)(n > 255u ? 255u : n);

    memcpy(s->pal_bg, p->pal_bg, sizeof s->pal_bg);
    memcpy(s->pal_spr, p->pal_spr, sizeof s->pal_spr);
    for (unsigned i = 0; i < MS_MAX_BG; i++) {
        bg_cfg_of(&p->bg[i], s->bg[i]);
        if (s->map[i] && p->bg[i].map) {
            const uint32_t ents = (uint32_t)p->bg[i].map_w * p->bg[i].map_h;
            memcpy(s->map[i], p->bg[i].map, (size_t)(ents < s->map_entries[i] ? ents : s->map_entries[i]) * 2u);
        }
    }
    s->fx[0] = p->fx.brightness; s->fx[1] = p->fx.mosaic;
    s->fx[2] = p->fx.math_on;    s->fx[3] = p->fx.color_math;
    s->math_factor = (uint32_t)p->fx.math_factor;
    return o;
}

size_t ms_net_encode_map_chunk(const ms_ppu_t *p, int bg, uint32_t first,
                               uint32_t max_entries, uint32_t frame,
                               void *out, size_t cap)
{
    if (!p || !out || bg < 0 || bg >= (int)MS_MAX_BG) return 0;
    const ms_bg_t *b = &p->bg[bg];
    if (!b->map) return 0;
    const uint32_t ents = (uint32_t)b->map_w * b->map_h;
    if (first >= ents || !max_entries) return 0;
    uint32_t cnt = ents - first;
    if (cnt > max_entries) cnt = max_entries;
    size_t room = cap < REC_MAX ? cap : REC_MAX;
    if (room < HDR5 + 6u) return 0;
    if (cnt > (room - HDR5 - 6u) / 2u) cnt = (uint32_t)((room - HDR5 - 6u) / 2u);
    if (!cnt) return 0;
    uint8_t *o8 = (uint8_t *)out;
    ms_wr32(o8, frame);
    o8[4] = 1;
    size_t o = HDR5;
    o8[o++] = 9; o8[o++] = (uint8_t)bg;
    ms_wr16(o8 + o, (uint16_t)first); o += 2;
    ms_wr16(o8 + o, (uint16_t)cnt);   o += 2;
    const uint16_t *m = (const uint16_t *)b->map;
    for (uint32_t i = 0; i < cnt; i++) { ms_wr16(o8 + o, m[first + i]); o += 2; }
    return o;
}

/* ---- display node ---------------------------------------------------- */

int ms_net_client_init(ms_net_client_t *c, const ms_game_t *g, const ms_ppu_t *base,
                       uint16_t *fb, uint16_t w, uint16_t h,
                       ms_net_buf_t map_store[MS_MAX_BG])
{
    if (!c || !base || !fb) return -1;
    memset(c, 0, sizeof(*c));
    c->ppu = *base;
    c->fb = fb; c->w = w; c->h = h;
    c->qfa = (uint8_t)(w == MS_QFA_W ? 1 : 0);
    if (g) {
        c->sheet_count = g->sheet_count;
        for (unsigned i = 0; i < g->sheet_count && i < MS_MAX_SHEETS; i++)
            c->sheet_tiles[i] = g->sheets[i].tiles;
    }
    for (unsigned i = 0; i < MS_MAX_BG; i++) {
        if (!base->bg[i].map || !map_store || !map_store[i].ptr) continue;
        const size_t bytes = (size_t)base->bg[i].map_w * base->bg[i].map_h * 2u;
        if (map_store[i].bytes < bytes) return -2;
        memcpy(map_store[i].ptr, base->bg[i].map, bytes);
        c->ppu.bg[i].map = (const uint16_t *)map_store[i].ptr;
    }
    for (unsigned i = 0; i < MS_MAX_SPRITES; i++) c->ppu.oam[i].line_mat = NULL;
    return 0;
}

int ms_net_apply_keyframe(ms_net_client_t *c, const void *payload, size_t n)
{
    if (!c || !payload || n < KF_FIXED) return -1;
    const uint8_t *b = (const uint8_t *)payload;
    ms_ppu_t *p = &c->ppu;
    p->frame = ms_rd32(b);
    p->mode = (ms_mode_t)ms_rd16(b + 4);
    p->width = ms_rd16(b + 6);
    p->height = ms_rd16(b + 8);
    p->fx.backdrop = ms_rd16(b + 10);
    p->fx.brightness = b[12]; p->fx.mosaic = b[13];
    p->fx.math_on = b[14];    p->fx.color_math = b[15];
    p->fx.math_factor = (ms_fix)ms_rd32(b + 16);
    const uint16_t oam_count = ms_rd16(b + 20);
    if (oam_count > MS_MAX_SPRITES) return -2;
    const size_t need = ms_net_keyframe_size(oam_count);
    if (n < need) return -3;
    size_t o = 23;
    for (unsigned i = 0; i < MS_MAX_BG; i++) {
        ms_bg_t *bg = &p->bg[i];
        bg->enabled = b[o]; bg->priority = b[o + 1]; bg->mosaic = b[o + 2];
        bg->math = b[o + 3]; bg->bpp = b[o + 4];
        bg->map_w = b[o + 5]; bg->map_h = b[o + 6];
        bg->logical_w = b[o + 7]; bg->logical_h = b[o + 8];
        bg->scroll_x = (int16_t)ms_rd16(b + o + 9);
        bg->scroll_y = (int16_t)ms_rd16(b + o + 11);
        o += 13;
    }
    for (unsigned i = 0; i < MS_PAL_ENTRIES; i++) { p->pal_bg[i]  = ms_rd16(b + o); o += 2; }
    for (unsigned i = 0; i < MS_PAL_ENTRIES; i++) { p->pal_spr[i] = ms_rd16(b + o); o += 2; }
    for (unsigned i = 0; i < MS_MAX_MATRICES; i++) { memcpy(&p->mat[i], b + o, 16); o += 16; }
    for (uint16_t i = 0; i < MS_MAX_SPRITES; i++) {
        ms_oam_t *s = &p->oam[i];
        if (i >= oam_count) { s->w = s->h = 0; s->flags = MS_OAM_HIDE; s->line_mat = NULL; continue; }
        s->x = (int16_t)ms_rd16(b + o);
        s->y = (int16_t)ms_rd16(b + o + 2);
        s->w = b[o + 4]; s->h = b[o + 5]; s->bpp = b[o + 6]; s->pal_slot = b[o + 7];
        s->priority = (uint8_t)(b[o + 8] & 3u);
        s->flags = (uint8_t)(b[o + 8] & 0xFCu);
        s->affine = (b[o + 9] == 0xFF) ? -1 : (int8_t)b[o + 9];
        s->line_rows = ms_rd16(b + o + 10);
        const uint8_t sheet = b[o + 12];
        s->tiles = (sheet < c->sheet_count) ? c->sheet_tiles[sheet] : NULL;
        s->line_mat = s->line_rows ? c->line_tab[i] : NULL;
        if (!s->line_rows) s->line_mat = NULL;
        o += KF_OAM;
    }
    p->oam_count = oam_count;
    c->frame = p->frame;
    c->synced = 1;
    return 0;
}

int ms_net_apply_delta(ms_net_client_t *c, const void *payload, size_t n)
{
    if (!c || !payload || n < HDR5) return -1;
    const uint8_t *b = (const uint8_t *)payload;
    ms_ppu_t *p = &c->ppu;
    const unsigned nrec = b[4];
    size_t o = HDR5;
    for (unsigned r = 0; r < nrec; r++) {
        if (o >= n) { c->overflow++; return -2; }
        const uint8_t t = b[o++];
        switch (t) {
        case 1: {
            if (o + 5 > n) { c->overflow++; return -2; }
            const unsigned bg = b[o];
            if (bg >= MS_MAX_BG) return -3;
            p->bg[bg].scroll_x = (int16_t)ms_rd16(b + o + 1);
            p->bg[bg].scroll_y = (int16_t)ms_rd16(b + o + 3);
            o += 5; break;
        }
        case 2:
            if (o + 2 > n) { c->overflow++; return -2; }
            p->fx.backdrop = ms_rd16(b + o); o += 2; break;
        case 3: {
            if (o + 11 > n) { c->overflow++; return -2; }
            const unsigned i = b[o];
            if (i >= MS_MAX_SPRITES) return -3;
            ms_oam_t *s = &p->oam[i];
            s->x = (int16_t)ms_rd16(b + o + 1);
            s->y = (int16_t)ms_rd16(b + o + 3);
            s->w = b[o + 5]; s->h = b[o + 6];
            s->priority = (uint8_t)(b[o + 7] & 3u);
            s->flags = (uint8_t)(b[o + 7] & 0xFCu);
            s->pal_slot = b[o + 8];
            s->affine = (b[o + 9] == 0xFF) ? -1 : (int8_t)b[o + 9];
            s->line_rows = b[o + 10];
            s->line_mat = s->line_rows ? c->line_tab[i] : NULL;
            o += 11; break;
        }
        case 11:
            if (o + 1 > n) { c->overflow++; return -2; }
            if (b[o] > MS_MAX_SPRITES) return -3;
            p->oam_count = b[o];
            o += 1; break;
        case 10: {
            if (o + 14 > n) { c->overflow++; return -2; }
            const unsigned i = b[o];
            if (i >= MS_MAX_SPRITES) return -3;
            ms_oam_t *s = &p->oam[i];
            s->x = (int16_t)ms_rd16(b + o + 1);
            s->y = (int16_t)ms_rd16(b + o + 3);
            s->w = b[o + 5]; s->h = b[o + 6]; s->bpp = b[o + 7]; s->pal_slot = b[o + 8];
            s->priority = (uint8_t)(b[o + 9] & 3u);
            s->flags = (uint8_t)(b[o + 9] & 0xFCu);
            s->affine = (b[o + 10] == 0xFF) ? -1 : (int8_t)b[o + 10];
            s->line_rows = ms_rd16(b + o + 11);
            const uint8_t sheet = b[o + 13];
            s->tiles = (sheet < c->sheet_count) ? c->sheet_tiles[sheet] : NULL;
            s->line_mat = s->line_rows ? c->line_tab[i] : NULL;
            o += 14; break;
        }
        case 4:
            if (o + 17 > n) { c->overflow++; return -2; }
            if (b[o] >= MS_MAX_MATRICES) return -3;
            memcpy(&p->mat[b[o]], b + o + 1, 16); o += 17; break;
        case 5: {
            if (o + 3 > n) { c->overflow++; return -2; }
            const unsigned i = b[o];
            const uint16_t rows = ms_rd16(b + o + 1);
            if (i >= MS_MAX_SPRITES || rows > MS_SPRITE_MAX) return -3;
            if (o + 3 + (size_t)rows * 16u > n) { c->overflow++; return -2; }
            memcpy(c->line_tab[i], b + o + 3, (size_t)rows * 16u);
            p->oam[i].line_mat = c->line_tab[i];
            p->oam[i].line_rows = rows;
            o += 3 + (size_t)rows * 16u;
            break;
        }
        case 6:
            if (o + 4 > n) { c->overflow++; return -2; }
            if (b[o + 1] >= MS_PAL_ENTRIES) return -3;
            if (b[o]) p->pal_spr[b[o + 1]] = ms_rd16(b + o + 2);
            else      p->pal_bg[b[o + 1]]  = ms_rd16(b + o + 2);
            o += 4; break;
        case 7: {
            if (o + 10 > n) { c->overflow++; return -2; }
            if (b[o] >= MS_MAX_BG) return -3;
            ms_bg_t *bg = &p->bg[b[o]];
            bg->enabled = b[o + 1]; bg->priority = b[o + 2]; bg->mosaic = b[o + 3];
            bg->math = b[o + 4]; bg->bpp = b[o + 5];
            bg->map_w = b[o + 6]; bg->map_h = b[o + 7];
            bg->logical_w = b[o + 8]; bg->logical_h = b[o + 9];
            o += 10; break;
        }
        case 8:
            if (o + 8 > n) { c->overflow++; return -2; }
            p->fx.brightness = b[o]; p->fx.mosaic = b[o + 1];
            p->fx.math_on = b[o + 2]; p->fx.color_math = b[o + 3];
            p->fx.math_factor = (ms_fix)ms_rd32(b + o + 4);
            o += 8; break;
        case 9: {
            if (o + 5 > n) { c->overflow++; return -2; }
            const unsigned bg = b[o];
            const uint16_t first = ms_rd16(b + o + 1);
            const uint16_t cnt = ms_rd16(b + o + 3);
            if (bg >= MS_MAX_BG) return -3;
            if (o + 5 + (size_t)cnt * 2u > n) { c->overflow++; return -2; }
            uint16_t *m = (uint16_t *)p->bg[bg].map;
            const uint32_t ents = (uint32_t)p->bg[bg].map_w * p->bg[bg].map_h;
            if (m)
                for (uint16_t i = 0; i < cnt; i++)
                    if ((uint32_t)(first + i) < ents) m[first + i] = ms_rd16(b + o + 5 + (size_t)i * 2u);
            o += 5 + (size_t)cnt * 2u;
            break;
        }
        default:
            c->overflow++;
            return -3;
        }
    }
    p->frame = ms_rd32(b);
    c->frame = p->frame;
    c->applied++;
    return 0;
}

void ms_net_render(ms_net_client_t *c)
{
    if (!c || !c->fb) return;
    if (c->qfa) ms_ppu_render_qfa(&c->ppu, c->fb, c->qfa_scratch);
    else        ms_ppu_render(&c->ppu, c->fb);
}

uint64_t ms_net_hash(const ms_ppu_t *p, const uint16_t *fb, uint16_t w, uint16_t h)
{
    if (!p) return 0;
    uint64_t x = 1469598103934665603ull;
    if (fb) x = mix(x, fb, (size_t)w * h * 2u);
    x = mix(x, &p->mode, sizeof p->mode);
    x = mix(x, p->pal_bg, sizeof p->pal_bg);
    x = mix(x, p->pal_spr, sizeof p->pal_spr);
    x = mix(x, &p->oam_count, sizeof p->oam_count);
    x = mix(x, &p->fx, sizeof p->fx);
    for (unsigned i = 0; i < MS_MAX_BG; i++) {
        const ms_bg_t *bg = &p->bg[i];
        uint8_t cfg[13];
        cfg[0] = bg->enabled; cfg[1] = bg->priority; cfg[2] = bg->mosaic;
        cfg[3] = bg->math; cfg[4] = bg->bpp; cfg[5] = bg->map_w; cfg[6] = bg->map_h;
        cfg[7] = bg->logical_w; cfg[8] = bg->logical_h;
        ms_wr16(cfg + 9, (uint16_t)bg->scroll_x);
        ms_wr16(cfg + 11, (uint16_t)bg->scroll_y);
        x = mix(x, cfg, sizeof cfg);
        if (bg->map) {
            const size_t ents = (size_t)bg->map_w * bg->map_h;
            x = mix(x, bg->map, ents * 2u);
        }
    }
    for (unsigned i = 0; i < p->oam_count && i < MS_MAX_SPRITES; i++) {
        const ms_oam_t *s = &p->oam[i];
        uint8_t rec[14];
        rec[0] = (uint8_t)(s->x & 0xFF); rec[1] = (uint8_t)((s->x >> 8) & 0xFF);
        rec[2] = (uint8_t)(s->y & 0xFF); rec[3] = (uint8_t)((s->y >> 8) & 0xFF);
        rec[4] = s->w; rec[5] = s->h; rec[6] = s->bpp; rec[7] = s->pal_slot;
        rec[8] = s->priority; rec[9] = s->flags; rec[10] = (uint8_t)s->affine;
        ms_wr16(rec + 11, s->line_rows); rec[13] = s->tiles ? 1 : 0;
        x = mix(x, rec, sizeof rec);
        if (s->line_mat && s->line_rows)
            x = mix(x, s->line_mat, (size_t)s->line_rows * 16u);
    }
    x = mix(x, p->mat, sizeof p->mat);
    return x;
}
