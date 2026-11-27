/* SPDX-License-Identifier: Apache-2.0 */
/* Mosaic — ms_game.c: cart -> live game object. */
#include "mosaic/ms_game.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

void ms_game_set_save(ms_game_t *g, ms_save_t *save)
{
    if (g) g->vm.save = save;
}

const char *ms_game_error(const ms_game_t *g) { return g ? g->error : "no game"; }

int ms_sheet_parse(ms_sheet_t *s, const void *data, size_t bytes)
{
    const uint8_t *p = (const uint8_t *)data;
    if (!s || !ms_check_sheet(p, bytes)) return -1;
    memset(s, 0, sizeof(*s));
    s->spr1 = (uint8_t)(ms_rd32(p) == MS_SPR1_MAGIC);
    s->bpp = p[5];
    s->n_pal = ms_rd16(p + 6);
    s->n_tiles = ms_rd16(p + 8);
    s->n_frames = ms_rd16(p + 10);
    s->pal = (const uint16_t *)(const void *)(p + 14);
    size_t off = 14u + (size_t)s->n_pal * 2u;
    s->tiles = p + off;
    off += (size_t)s->n_tiles * (s->bpp == 8 ? 64u : 32u);
    s->frames_raw = p + off;
    return 0;
}

void ms_sheet_frame(const ms_sheet_t *s, unsigned idx, ms_frame_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!s || idx >= s->n_frames) return;
    const uint8_t *f = s->frames_raw + (size_t)idx * (s->spr1 ? 6u : 8u);
    out->tile_base = ms_rd16(f);
    out->wt = f[2];
    out->ht = f[3];
    out->px = (int8_t)f[4];
    out->py = (int8_t)f[5];
    out->flags = s->spr1 ? 0 : f[6];
}

static int key_val(const char *text, const char *key, char *out, size_t cap)
{
    size_t klen = strlen(key);
    const char *p = text;
    while (p && *p) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len > klen && p[klen] == '=' && strncmp(p, key, klen) == 0) {
            size_t n = len - klen - 1;
            if (n >= cap) n = cap - 1;
            memcpy(out, p + klen + 1, n);
            out[n] = 0;
            return 0;
        }
        p = e ? e + 1 : NULL;
    }
    return -1;
}

int ms_manifest_parse(ms_manifest_t *m, const char *text, size_t bytes)
{
    if (!m || !text) return -1;
    (void)bytes;
    memset(m, 0, sizeof(*m));
    m->mode = MS_MODE_RICH; m->voices = MS_VOICES; m->music_voices = 12;
    m->sfx_voices = 4; m->brightness = 16; m->backdrop = 0x0841;
    char tmp[128];
    if (key_val(text, "title", tmp, sizeof(tmp)) == 0) snprintf(m->title, sizeof(m->title), "%.32s", tmp);
    if (key_val(text, "author", tmp, sizeof(tmp)) == 0) snprintf(m->author, sizeof(m->author), "%.32s", tmp);
    if (key_val(text, "mode", tmp, sizeof(tmp)) == 0) {
        if (!strcmp(tmp, "text")) m->mode = MS_MODE_TEXT;
        else if (!strcmp(tmp, "rich")) m->mode = MS_MODE_RICH;
        else if (!strcmp(tmp, "affine")) m->mode = MS_MODE_AFFINE;
        else if (!strcmp(tmp, "affine2")) m->mode = MS_MODE_AFFINE2;
        else return -1;
    }
    if (key_val(text, "qfa", tmp, sizeof(tmp)) == 0) m->qfa = (uint8_t)atoi(tmp);
    if (key_val(text, "voices", tmp, sizeof(tmp)) == 0) m->voices = (uint8_t)atoi(tmp);
    if (key_val(text, "music_voices", tmp, sizeof(tmp)) == 0) m->music_voices = (uint8_t)atoi(tmp);
    if (key_val(text, "sfx_voices", tmp, sizeof(tmp)) == 0) m->sfx_voices = (uint8_t)atoi(tmp);
    if (key_val(text, "brightness", tmp, sizeof(tmp)) == 0) m->brightness = (uint8_t)atoi(tmp);
    if (key_val(text, "backdrop", tmp, sizeof(tmp)) == 0) m->backdrop = (uint16_t)strtoul(tmp, NULL, 0);
    if (key_val(text, "music", tmp, sizeof(tmp)) == 0) snprintf(m->boot_song, sizeof(m->boot_song), "%s", tmp);
    if (key_val(text, "palette_bg", tmp, sizeof(tmp)) == 0) snprintf(m->palette_bg, sizeof(m->palette_bg), "%s", tmp);
    if (key_val(text, "palette_spr", tmp, sizeof(tmp)) == 0) snprintf(m->palette_spr, sizeof(m->palette_spr), "%s", tmp);
    if (key_val(text, "bg_map_w", tmp, sizeof(tmp)) == 0) m->map_w = atoi(tmp);
    if (key_val(text, "bg_map_h", tmp, sizeof(tmp)) == 0) m->map_h = atoi(tmp);
    if (key_val(text, "splash", tmp, sizeof(tmp)) == 0) snprintf(m->splash, sizeof(m->splash), "%s", tmp);
    if (!m->voices || m->voices > MS_VOICES_MAX) return -1;
    if ((unsigned)m->music_voices + m->sfx_voices > m->voices) return -1;
    return 0;
}

void ms_game_music(ms_game_t *g, int slot, int loop)
{
    if (!g || !g->synth) return;
    ms_asset_t a;
    if (slot < 0) { ms_player_stop(g->synth); return; }
    if (ms_cart_find_kind(g->cart, MS_A_MIDI, &a) != 0) return;
    static ms_song_t song;
    if (ms_song_load(&song, a.data, a.size) == 0) ms_player_start(g->synth, &song, loop);
}
void ms_game_sfx(ms_game_t *g, int program, int note)
{
    if (g && g->synth) ms_synth_sfx(g->synth, program, note, 220);
}

/* Palette bank: v2.1 payloads state it in a bank byte; v2.0 payloads fall
 * back to the manifest (palette_bg / palette_spr asset names) and, failing
 * that, to asset order (first -> background, second -> sprite). The entry
 * base is derived from the payload length, so both forms load here. */
static int palette_payload_bank(const ms_asset_t *a)
{
    const uint8_t *p = (const uint8_t *)a->data;
    const unsigned n = ms_rd16(p);
    return (a->size == 4u + (size_t)n * 2u) ? (int)(p[2] & 1u) : -1;
}

static void apply_palette(ms_ppu_t *ppu, const ms_asset_t *a, int bank)
{
    const uint8_t *p = (const uint8_t *)a->data;
    const unsigned n = ms_rd16(p);
    if (n > MS_PAL_ENTRIES) return;
    const size_t base = (a->size == 4u + (size_t)n * 2u) ? 4u : 2u;
    for (unsigned i = 0; i < n; i++) {
        const uint16_t c = ms_rd16(p + base + i * 2);
        if (bank) ppu->pal_spr[i] = c; else ppu->pal_bg[i] = c;
    }
}

int ms_game_load(ms_game_t *g, const ms_cart_t *cart, ms_ppu_t *ppu, ms_synth_t *synth)
{
    if (!g || !cart || !ppu || !synth) return -1;
    memset(g, 0, sizeof(*g));
    g->cart = cart; g->ppu = ppu; g->synth = synth;
    ms_asset_t a;
    if (ms_cart_find_kind(cart, MS_A_MANIFEST, &a) == 0)
        ms_manifest_parse(&g->man, (const char *)a.data, a.size);
    else
        ms_manifest_parse(&g->man, "title=Untitled\nmode=rich\nvoices=16\n", 36);

    ms_ppu_reset(ppu, (ms_mode_t)g->man.mode,
                 g->man.qfa ? MS_QFA_W : MS_SCREEN_W,
                 g->man.qfa ? MS_QFA_H : MS_SCREEN_H);
    ppu->fx.backdrop = g->man.backdrop;
    ppu->fx.brightness = g->man.brightness;

    /* palettes: bank assignment by manifest name, else by asset order */
    int have_bg = 0, have_spr = 0;
    for (uint32_t i = 0; i < cart->count; i++) {
        const uint8_t *e = cart->blob + cart->toc_off + (size_t)i * MS_CART_TOC_ENTRY;
        if (ms_rd32(e + 32) != MS_A_PALETTE) continue;
        ms_asset_t pa = { (const char *)e, cart->blob + ms_rd32(e + 36), ms_rd32(e + 40), MS_A_PALETTE, 0 };
        const int pbank = palette_payload_bank(&pa);
        if (pbank == 0) { apply_palette(ppu, &pa, 0); have_bg = 1; }
        else if (pbank == 1) { apply_palette(ppu, &pa, 1); have_spr = 1; }
        else if (g->man.palette_bg[0] && !strcmp(pa.name, g->man.palette_bg)) { apply_palette(ppu, &pa, 0); have_bg = 1; }
        else if (g->man.palette_spr[0] && !strcmp(pa.name, g->man.palette_spr)) { apply_palette(ppu, &pa, 1); have_spr = 1; }
        else if (!have_bg) { apply_palette(ppu, &pa, 0); have_bg = 1; }
        else if (!have_spr) { apply_palette(ppu, &pa, 1); have_spr = 1; }
    }
    /* sprite sheets */
    for (uint32_t i = 0; i < cart->count && g->sheet_count < MS_MAX_SHEETS; i++) {
        const uint8_t *e = cart->blob + cart->toc_off + (size_t)i * MS_CART_TOC_ENTRY;
        if (ms_rd32(e + 32) != MS_A_SPRITES) continue;
        ms_sheet_t sh;
        if (ms_sheet_parse(&sh, cart->blob + ms_rd32(e + 36), ms_rd32(e + 40)) == 0) {
            g->sheets[g->sheet_count] = sh;
            if (!g->sheet_count) {
                ppu->spr_tiles = sh.tiles;
                /* a dedicated palette_spr asset wins over the sheet palette */
                if (!have_spr)
                    for (unsigned k = 0; k < sh.n_pal && k < MS_PAL_ENTRIES; k++) ppu->pal_spr[k] = sh.pal[k];
            }
            g->sheet_count++;
        }
    }
    /* background: first tiles + map */
    ms_asset_t tiles, map;
    if (ms_cart_find_kind(cart, MS_A_BG_TILES, &tiles) == 0) {
        const uint8_t *td = (const uint8_t *)tiles.data;
        ppu->bg[0].tiles = td + 3;
        ppu->bg[0].bpp = td[0];
        ppu->bg[0].enabled = 1;
        ppu->bg[0].priority = 2;
        if (ms_cart_find_kind(cart, MS_A_BG_MAP, &map) == 0) {
            const uint8_t *md = (const uint8_t *)map.data;
            const unsigned entries = ms_rd16(md);
            int lw = 0, lh = 0, mw = 0, mh = 0;
            if (map.size == 6u + (size_t)entries * 2u) {      /* v2.1: dims in payload */
                lw = ms_rd16(md + 2); lh = ms_rd16(md + 4);
                ppu->bg[0].map = (const uint16_t *)(const void *)(md + 6);
                for (mw = 1; mw < lw; mw <<= 1) {}
                for (mh = 1; mh < lh; mh <<= 1) {}
                if ((unsigned)mw * (unsigned)mh != entries) { lw = lh = 0; mw = mh = 0; }
            } else {
                ppu->bg[0].map = (const uint16_t *)(const void *)(md + 2);
            }
            if (!mw) {
                const int hint_w = g->man.map_w, hint_h = g->man.map_h;
                if (hint_w > 0 && hint_h > 0 && (unsigned)hint_w * (unsigned)hint_h == entries &&
                    !(hint_w & (hint_w - 1)) && !(hint_h & (hint_h - 1))) {
                    mw = hint_w; mh = hint_h;
                } else {
                    ms_map_dims(entries, &mw, &mh);   /* no hint: landscape split */
                }
            }
            ppu->bg[0].map_w = (uint8_t)mw;
            ppu->bg[0].map_h = (uint8_t)mh;
            ppu->bg[0].logical_w = (uint8_t)lw;   /* 0 = use the padded dims */
            ppu->bg[0].logical_h = (uint8_t)lh;
        }
    }
    ms_asset_t anim;
    if (ms_cart_find_kind(cart, MS_A_ANIM, &anim) == 0) {
        g->anim_count = ms_rd16((const uint8_t *)anim.data);
        g->anims = (const ms_anim_t *)(const void *)((const uint8_t *)anim.data + 2);
    }
    if (ms_cart_find_kind(cart, MS_A_BANK, &a) == 0) ms_synth_set_bank(synth, a.data, a.size);
    if (ms_cart_find_kind(cart, MS_A_PROGRAM, &a) == 0) {
        if (ms_program_parse(&g->prog, a.data, a.size) != 0) {
            snprintf(g->error, sizeof(g->error), "invalid program");
            return -2;
        }
        ms_vm_init(&g->vm, &g->prog, g);
    } else {
        snprintf(g->error, sizeof(g->error), "cart has no program asset");
        return -3;
    }
    /* optional intro movie (cart kind 11) */
    ms_asset_t clip;
    int have_clip = 0;
    if (g->man.splash[0] && ms_cart_find(cart, g->man.splash, &clip) == 0 && clip.kind == MS_A_MVID)
        have_clip = 1;
    else if (ms_cart_find_kind(cart, MS_A_MVID, &clip) == 0)
        have_clip = 1;
    if (have_clip && ms_video_open(&g->vid, clip.data, clip.size) == 0) {
        g->splash_on = 1;
        g->splash_bg0 = ppu->bg[0].enabled;
        ppu->bg[0].enabled = 0;
        ms_video_bind(&g->vid, ppu, 1);
    }
    if (g->man.boot_song[0]) ms_game_music(g, 0, 1);
    return 0;
}

void ms_game_free(ms_game_t *g) { if (g) ms_player_stop(g->synth); }

void ms_game_update(ms_game_t *g, const ms_input_t *in)
{
    if (!g) return;
    g->frame++;
    if (g->splash_on) {
        const unsigned fps = g->vid.dec.info.fps ? g->vid.dec.info.fps : 12u;
        while ((uint32_t)((uint64_t)g->frame * fps / 60u) > g->vid.dec.cur && g->vid.playing) {
            if (!ms_video_advance(&g->vid)) break;
        }
        const int skip = in && (in->pressed & (MS_BTN_A | MS_BTN_START));
        if (skip || ms_video_done(&g->vid)) {
            ms_video_stop(&g->vid);
            g->splash_on = 0;
            g->ppu->bg[1].enabled = 0;
            g->ppu->bg[0].enabled = g->splash_bg0;
            g->ppu->fx.brightness = g->man.brightness ? g->man.brightness : 16;
        } else {
            ms_video_bind(&g->vid, g->ppu, 1);
            g->ppu->oam_count = 0;
            return;
        }
    }
    ms_vm_update(&g->vm, in);
    ms_ppu_t *ppu = g->ppu;
    ppu->oam_count = 0;
    for (int i = 0; i < (int)g->vm.count && ppu->oam_count < MS_MAX_SPRITES; i++) {
        ms_vm_entity_t *en = &g->vm.ents[i];
        if (!en->alive) continue;
        ms_entity_t *e = &en->e;
        if (e->sheet >= g->sheet_count) continue;
        ms_sheet_t *sh = &g->sheets[e->sheet];
        uint16_t frame = 0;
        if (e->anim < g->anim_count && g->anims[e->anim].count) {
            const ms_anim_t *an = &g->anims[e->anim];
            uint32_t fps = an->fps ? an->fps : 8;
            uint32_t idx = (uint32_t)((uint64_t)(g->frame - e->spawn_frame) * fps / 60u) % an->count;
            frame = (uint16_t)(an->first + idx);
        } else {
            frame = e->frame;
        }
        if (frame >= sh->n_frames) frame = 0;
        ms_frame_t frstore;
        ms_sheet_frame(sh, frame, &frstore);
        const ms_frame_t *fr = &frstore;
        ms_oam_t *o = &ppu->oam[ppu->oam_count++];
        memset(o, 0, sizeof(*o));
        o->x = (int16_t)((e->x >> 16) - fr->px);
        o->y = (int16_t)((e->y >> 16) - fr->py);
        o->w = (uint8_t)(fr->wt * 8);
        o->h = (uint8_t)(fr->ht * 8);
        o->bpp = sh->bpp;
        o->pal_slot = 0;
        o->priority = e->priority;
        o->flags = (uint8_t)(e->flags | fr->flags);
        o->affine = (int8_t)(e->affine >= 0 && e->affine < (int)MS_MAX_MATRICES ? e->affine : -1);
        if (e->line_tab >= 0) {
            ms_asset_t ma;
            if (g->cart && ms_cart_find_kind(g->cart, MS_A_MATRICES, &ma) == MS_CART_OK) {
                const uint8_t *mb = (const uint8_t *)ma.data;
                const unsigned nt = ms_rd16(mb);
                if ((unsigned)e->line_tab < nt) {
                    size_t off = 4u + (size_t)nt * 2u;
                    off = (off + 3u) & ~(size_t)3u;
                    for (int k = 0; k < e->line_tab; k++)
                        off += (size_t)ms_rd16(mb + 4u + (size_t)k * 2u) * 16u;
                    o->line_mat = (const ms_matrix_t *)(const void *)(mb + off);
                    o->line_rows = ms_rd16(mb + 4u + (size_t)e->line_tab * 2u);
                }
            }
        }
        o->tiles = sh->tiles + (size_t)fr->tile_base * (sh->bpp == 8 ? 64u : 32u);
    }
    ms_player_t *pl = &g->synth->player;
    (void)pl;
}

void ms_game_render(ms_game_t *g) { (void)g; }
