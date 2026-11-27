/* SPDX-License-Identifier: Apache-2.0 */
/* Mosaic — ms_game.h: load a cart, bind its assets to the PPU/synth and
 * run the frame contract. This is the object the editor exports. */
#ifndef MOSAIC_MS_GAME_H
#define MOSAIC_MS_GAME_H
#include "mosaic/ms_cart.h"
#include "mosaic/ms_render.h"
#include "mosaic/ms_audio.h"
#include "mosaic/ms_vm.h"
#include "mosaic/ms_video.h"

typedef struct {
    char title[33], author[33], boot_song[32], start_map[32];
    char palette_bg[32], palette_spr[32], splash[32];
    int  map_w, map_h;
    uint8_t mode, qfa, voices, music_voices, sfx_voices, brightness;
    uint16_t backdrop;
    /* Save identity: the developer key namespaces the studio, save_id names
     * the save (not the title), save_reads lists the save ids this title may
     * open read-only. See ms_save.h. */
    char     dev[33], save_id[25], dev_key[33];
    uint32_t dev_id, app_id;
    uint32_t reads[MS_SAVE_READS_MAX];
    int      n_reads;
} ms_manifest_t;

typedef struct {
    uint16_t first, count, fps;
    uint8_t loop, pad;
} ms_anim_t;

typedef struct {
    uint16_t tile_base; uint8_t wt, ht; int8_t px, py; uint8_t flags, res;
} ms_frame_t;

typedef struct {
    const uint8_t *tiles; uint16_t n_tiles, n_frames, n_pal;
    uint8_t bpp, spr1;                 /* spr1: deprecated 6-byte frames */
    const uint16_t *pal;
    const uint8_t  *frames_raw;        /* 8 B (SPR2) or 6 B (SPR1) each */
} ms_sheet_t;

/* Decode frame `idx` into the revision-independent form. */
void ms_sheet_frame(const ms_sheet_t *s, unsigned idx, ms_frame_t *out);

typedef struct ms_game {
    const ms_cart_t *cart;
    ms_ppu_t *ppu;
    ms_synth_t *synth;
    ms_manifest_t man;
    ms_program_t prog;
    ms_vm_t vm;
    ms_sheet_t sheets[MS_MAX_SHEETS];
    uint8_t sheet_count;
    const ms_anim_t *anims; uint16_t anim_count;
    const uint8_t *anim_blob;
    int song_slot;
    uint32_t frame;
    ms_video_t vid;
    uint8_t splash_on, splash_bg0;
    char error[96];
} ms_game_t;

int  ms_manifest_parse(ms_manifest_t *m, const char *text, size_t bytes);
int  ms_game_load(ms_game_t *g, const ms_cart_t *cart, ms_ppu_t *ppu, ms_synth_t *synth);
void ms_game_update(ms_game_t *g, const ms_input_t *in);
void ms_game_render(ms_game_t *g);
void ms_game_free(ms_game_t *g);
void ms_game_sfx(ms_game_t *g, int program, int note);
void ms_game_music(ms_game_t *g, int slot, int loop);
const char *ms_game_error(const ms_game_t *g);

/* Bind the save store (<stdio.h>-free, caller-owned) so SAVE / LOADSAVE
 * reach it. Pass NULL to detach. Call after ms_game_load. */
void ms_game_set_saves(ms_game_t *g, ms_save_store_t *saves);
/* Fill an identity from the manifest, so a host attaches the right store. */
void ms_game_save_id(const ms_game_t *g, ms_save_id_t *id, uint8_t loc,
                     uint32_t cart_crc);

/* Sprite sheet decode, shared with tools. */
int ms_sheet_parse(ms_sheet_t *s, const void *data, size_t bytes);

#endif
