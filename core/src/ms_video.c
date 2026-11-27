/* SPDX-License-Identifier: Apache-2.0 — MVID playback on the PPU. */
#include "mosaic/ms_video.h"
#include <string.h>

int ms_check_mvid(const void *data, size_t bytes)
{
    return mvid_check(data, bytes) == MVID_E_OK ? 1 : 0;   /* 1 = valid, like ms_check_* */
}

int ms_video_open(ms_video_t *v, const void *data, size_t bytes)
{
    if (!v || !data) return MVID_E_PARAM;
    memset(v, 0, sizeof(*v));
    v->layer = -1;
    mvid_info_t info;
    int rc = mvid_parse(&info, data, bytes);
    if (rc != MVID_E_OK) return rc;
    if (info.map_w > MS_VIDEO_MAP_W || info.map_h > MS_VIDEO_MAP_H) return MVID_E_PARAM;
    rc = mvid_open(&v->dec, data, bytes, v->sheet, v->map);
    if (rc != MVID_E_OK) return rc;
    v->playing = 1;
    return 0;
}

void ms_video_bind(ms_video_t *v, ms_ppu_t *ppu, int layer)
{
    if (!v || !ppu || layer < 0 || layer >= (int)MS_MAX_BG) return;
    ms_bg_t *bg = &ppu->bg[layer];
    if (!v->dec.opened) { bg->enabled = 0; return; }
    if ((int)v->dec.info.width != (int)ppu->width ||
        (int)v->dec.info.height != (int)ppu->height) {
        bg->enabled = 0;                    /* QFA clip on a full-res PPU */
        return;
    }
    bg->tiles = v->dec.sheet;
    bg->map = v->dec.map;
    bg->bpp = v->dec.info.bpp;
    bg->map_w = (uint8_t)v->dec.mw;
    bg->map_h = (uint8_t)v->dec.mh;
    bg->logical_w = (uint8_t)v->dec.info.map_w;
    bg->logical_h = (uint8_t)v->dec.info.map_h;
    bg->scroll_x = v->dec.scroll_x;
    bg->scroll_y = v->dec.scroll_y;
    bg->priority = 0;
    bg->enabled = 1;
    bg->math = 0;
    bg->mosaic = 0;
    bg->line_scroll_x = bg->line_scroll_y = NULL;
    for (unsigned i = 0; i < 256; i++) ppu->pal_bg[i] = v->dec.pal[i];
    ppu->fx.brightness = v->dec.brightness <= 16 ? v->dec.brightness : 16;
    v->ppu = ppu;
    v->layer = (int8_t)layer;
}

int ms_video_advance(ms_video_t *v)
{
    if (!v || !v->dec.opened || !v->playing) return 0;
    if (!mvid_next(&v->dec)) {
        if (v->dec.info.flags & MVID_F_LOOP) {
            mvid_seek(&v->dec, 0);
        } else {
            v->playing = 0;
            return 0;
        }
    }
    if (v->ppu && v->layer >= 0) ms_video_bind(v, v->ppu, v->layer);
    return 1;
}

void ms_video_stop(ms_video_t *v)
{
    if (!v) return;
    v->playing = 0;
    v->dec.opened = 0;
    if (v->ppu && v->layer >= 0) v->ppu->bg[v->layer].enabled = 0;
}

int ms_video_playing(const ms_video_t *v) { return v && v->playing; }
int ms_video_done(const ms_video_t *v)
{
    return !v || !v->playing || (unsigned)v->dec.cur + 1u >= v->dec.info.frames;
}
uint16_t ms_video_frame(const ms_video_t *v) { return v ? v->dec.cur : 0; }
