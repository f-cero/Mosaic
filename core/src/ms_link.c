/* SPDX-License-Identifier: Apache-2.0 */
#include "mosaic/ms_link.h"
#include "mosaic/ms_render.h"
#include "mosaic/ms_types.h"
#include <string.h>

#define HDR 8

void ms_link_init(ms_link_t *l) { if (l) memset(l, 0, sizeof(*l)); }

size_t ms_link_encode(const ms_frame_t *f, void *out, size_t cap)
{
    size_t need = HDR + (size_t)f->len + 2;
    if (!f || !out || f->len > MS_LINK_MTU || cap < need) return 0;
    uint8_t *p = (uint8_t *)out;
    p[0] = 0xA5; p[1] = 0x5A; p[2] = f->ver; p[3] = f->type;
    p[4] = f->node; p[5] = (uint8_t)(f->seq & 0xFF);
    ms_wr16(p + 6, f->len);
    memcpy(p + HDR, f->payload, f->len);
    uint16_t crc = ms_crc16(p, HDR + f->len);
    ms_wr16(p + HDR + f->len, crc);
    return need;
}

int ms_link_decode(ms_frame_t *f, const void *in, size_t n)
{
    const uint8_t *p = (const uint8_t *)in;
    if (!f || !in || n < HDR + 2) return -1;
    if (p[0] != 0xA5 || p[1] != 0x5A) return -2;
    uint16_t len = ms_rd16(p + 6);
    if (len > MS_LINK_MTU || n < HDR + (size_t)len + 2) return -3;
    if (ms_crc16(p, HDR + len) != ms_rd16(p + HDR + len)) return -4;
    f->ver = p[2]; f->type = p[3]; f->node = p[4]; f->seq = p[5];
    f->len = len;
    memcpy(f->payload, p + HDR, len);
    return 0;
}

void ms_link_note_peer(ms_link_t *l, int node, int role)
{
    if (!l || node < 0 || node >= (int)MS_LINK_MAX_NODES) return;
    l->st.peers |= (uint8_t)(1u << node);
    l->roles[node] = (uint8_t)role;
}

const char *ms_link_role_name(int role)
{
    switch (role) {
    case MS_ROLE_CORE: return "core";
    case MS_ROLE_PPU: return "ppu";
    case MS_ROLE_AUDIO: return "audio";
    case MS_ROLE_IO: return "io";
    case MS_ROLE_STORAGE: return "storage";
    default: return "?";
    }
}

/* Payload: u8 count, then records [tag u8, ...]. Tags keep the stream tiny
 * compared to a full framebuffer: at 320x240x2 a full pixel stream is
 * 153.6 KB/frame (9.2 MB/s at 60 Hz) which no 40 MHz SPI link can carry,
 * so the default topology streams registers and each display node renders
 * its own scanlines locally. */
size_t ms_link_encode_ppu_delta(const ms_ppu_t *ppu, void *out, size_t cap)
{
    uint8_t *p = (uint8_t *)out;
    if (!ppu || !out || cap < 64) return 0;
    size_t o = 1;
    int n = 0;
    /* 1: background scroll */
    for (int b = 0; b < 2; b++) {
        if (!ppu->bg[b].enabled) continue;
        if (o + 6 > cap) break;
        p[o++] = 1; p[o++] = (uint8_t)b;
        p[o++] = (uint8_t)(ppu->bg[b].scroll_x & 0xFF);
        p[o++] = (uint8_t)((ppu->bg[b].scroll_x >> 8) & 0xFF);
        p[o++] = (uint8_t)(ppu->bg[b].scroll_y & 0xFF);
        p[o++] = (uint8_t)((ppu->bg[b].scroll_y >> 8) & 0xFF);
        n++;
    }
    /* 2: backdrop */
    if (o + 3 <= cap) { p[o++] = 2; ms_wr16(p + o, ppu->fx.backdrop); o += 2; n++; }
    /* 3: one sprite: idx, x, y, w, h, priority|flags, pal, affine, line_rows */
    for (uint16_t i = 0; i < ppu->oam_count && i < MS_MAX_SPRITES; i++) {
        const ms_oam_t *s = &ppu->oam[i];
        if (o + 13 > cap) break;
        p[o++] = 3;
        p[o++] = (uint8_t)i;
        p[o++] = (uint8_t)(s->x & 0xFF); p[o++] = (uint8_t)((s->x >> 8) & 0xFF);
        p[o++] = (uint8_t)(s->y & 0xFF); p[o++] = (uint8_t)((s->y >> 8) & 0xFF);
        p[o++] = s->w; p[o++] = s->h;
        p[o++] = (uint8_t)((s->priority & 3) | (s->flags & 0xC0));
        p[o++] = s->pal_slot;
        p[o++] = (uint8_t)(s->affine < 0 ? 0xFF : s->affine);
        p[o++] = (uint8_t)(s->line_mat ? (s->line_rows ? s->line_rows : s->h) : 0);
        n++;
    }
    /* 4: the shared matrix set, whole (16 x 16 B) — cheap and idempotent */
    for (int m = 0; m < (int)MS_MAX_MATRICES; m++) {
        if (o + 18 > cap) break;
        p[o++] = 4; p[o++] = (uint8_t)m;
        memcpy(p + o, &ppu->mat[m], 16); o += 16; n++;
    }
    /* 5: per-scanline table contents, clipped at the frame budget */
    for (uint16_t i = 0; i < ppu->oam_count && i < MS_MAX_SPRITES; i++) {
        const ms_oam_t *s = &ppu->oam[i];
        if (!s->line_mat) continue;
        const uint16_t rows = s->line_rows ? s->line_rows : s->h;
        if (o + 3 > cap) break;
        p[o++] = 5; p[o++] = (uint8_t)i;
        size_t fit = (cap - o - 2) / 16u;
        uint16_t send = rows <= fit ? rows : (uint16_t)fit;
        ms_wr16(p + o, send); o += 2;
        memcpy(p + o, s->line_mat, (size_t)send * 16u);
        o += (size_t)send * 16u;
        n++;
    }
    p[0] = (uint8_t)n;
    return o;
}
