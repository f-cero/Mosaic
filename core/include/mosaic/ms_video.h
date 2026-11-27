/* SPDX-License-Identifier: Apache-2.0 */
/* Mosaic — ms_video.h: MVID playback inside the engine.
 *
 * The PPU *is* the video decoder: the file carries a tile atlas, a tilemap
 * and a palette, so playback is `bg.tiles = sheet; bg.map = map;` plus one
 * scroll write per frame. No pixels, no blitter, no extra render path.
 *
 * Cost: one ms_video_t is ~73 KB of static buffers (64 KB atlas + 8 KB map).
 * A cart that ships a splash pays it once; the ESP32 port can move it to
 * heap if the linker complains. */
#ifndef MOSAIC_MS_VIDEO_H
#define MOSAIC_MS_VIDEO_H
#include "mosaic/ms_limits.h"
#include "mosaic/ms_render.h"
#include "mvid.h"

#define MS_VIDEO_MAP_W 64u          /* atlas map ceiling (512x512 px)     */
#define MS_VIDEO_MAP_H 64u

typedef struct {
    mvid_t   dec;
    uint8_t  sheet[MVID_MAX_TILES * MVID_TILE_MAX];
    uint16_t map[MS_VIDEO_MAP_W * MS_VIDEO_MAP_H];
    ms_ppu_t *ppu;
    int8_t   layer;
    uint8_t  playing;
} ms_video_t;

/* Open a clip. Returns 0, or MVID_E_* (MVID_E_PARAM if the map is too big
 * for MS_VIDEO_MAP_W/H). */
int  ms_video_open(ms_video_t *v, const void *data, size_t bytes);
/* Point a PPU background layer at the current frame. Call after open and
 * after every advance. */
void ms_video_bind(ms_video_t *v, ms_ppu_t *ppu, int layer);
/* 1 = a new frame is in the buffers, 0 = the clip ended. */
int  ms_video_advance(ms_video_t *v);
void ms_video_stop(ms_video_t *v);
int  ms_video_playing(const ms_video_t *v);
int  ms_video_done(const ms_video_t *v);
uint16_t ms_video_frame(const ms_video_t *v);
/* Cart validator for kind 11. */
int  ms_check_mvid(const void *data, size_t bytes);

#endif
