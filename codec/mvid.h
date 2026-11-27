/* SPDX-License-Identifier: Apache-2.0
 * Mosaic — mvid.h: MVID v1, the tile-video container (docs/10-fmv.md).
 *
 * MVID carries video as what the PPU already eats: an 8x8 tile atlas, a
 * tilemap of SNES-packed entries, and a palette. Three frame types
 * (INTRA / PATCH / PAN) plus per-frame scroll and brightness mean a pan is
 * free and a fade costs 0 bytes. The decoder is allocation-free: the caller
 * brings the buffers. Nothing here includes SDL, the engine or libc beyond
 * <string.h>/<stdio.h> for the file helper, so the same source builds for
 * Linux, Windows and Android (NDK).
 */
#ifndef MVID_H
#define MVID_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MVID_VERSION     1u
#define MVID_HDR         56u
#define MVID_TILE        8
#define MVID_MAX_TILES   1024
#define MVID_MAX_FRAMES  4096
#define MVID_MAX_MAP     128      /* logical tiles per axis */
#define MVID_TILE_MAX    64       /* 8bpp tile bytes; 4bpp = 32 */

#define MVID_F_LOOP   0x01u
#define MVID_F_AUDIO  0x02u
#define MVID_F_QFA    0x04u        /* 160x120 coded, display upscales x2 */

#define MVID_FRAME_INTRA 0u
#define MVID_FRAME_PATCH 1u
#define MVID_FRAME_PAN   2u

#define MVID_E_OK       0
#define MVID_E_PARAM   -1
#define MVID_E_MAGIC   -2
#define MVID_E_VERSION -3
#define MVID_E_SIZE    -4
#define MVID_E_FRAME   -5
#define MVID_E_CRC     -6
#define MVID_E_MEM     -7
#define MVID_E_IO      -8
#define MVID_E_FULL    -9

typedef struct {
    uint16_t width, height;      /* coded size (320x240 or 160x120)       */
    uint8_t  fps, flags, bpp;
    uint16_t frames;
    uint16_t map_w, map_h;       /* logical tile dims of the source map    */
    uint16_t pal_count;
    uint32_t pal_off, index_off, audio_off, audio_len, sheet_bytes;
} mvid_info_t;

typedef struct {
    const uint8_t *data;
    size_t   size;
    mvid_info_t info;
    uint8_t *sheet;              /* MVID_MAX_TILES * tile_bytes            */
    uint16_t *map;               /* padded map, mw*mh                      */
    uint16_t pal[256];
    uint16_t mw, mh;             /* padded pow2 map dims                   */
    uint16_t cur;                /* frame currently in the buffers         */
    int16_t  scroll_x, scroll_y;
    uint8_t  brightness;         /* 0..16, 16 = normal                     */
    uint8_t  opened;
} mvid_t;

/* ---- shared helpers (mvid_util.c) ---- */
uint32_t mvid_crc32(const void *data, size_t bytes);
unsigned mvid_next_pow2(unsigned v);
uint16_t mvid_rd16(const uint8_t *p);
uint32_t mvid_rd32(const uint8_t *p);
void     mvid_wr16(uint8_t *p, uint16_t v);
void     mvid_wr32(uint8_t *p, uint32_t v);
const char *mvid_err_str(int e);
/* PPU brightness: 16 = unchanged, 0 = black. */
uint16_t mvid_scale565(uint16_t c, unsigned brightness);
/* Read a whole file (malloc'd) — returns NULL on failure. */
uint8_t *mvid_read_file(const char *path, size_t *bytes);

/* ---- decode (mvid_decode.c) ---- */
/* Bytes the caller must provide for sheet[] / map[]. */
size_t   mvid_sheet_size(const mvid_info_t *i);
size_t   mvid_map_entries(const mvid_info_t *i);   /* padded mw*mh */
/* Structural validation: header, palette, index, every frame header and
 * every payload length/bound. Does not decode. */
int      mvid_check(const void *data, size_t size);
int      mvid_parse(mvid_info_t *out, const void *data, size_t size);
int      mvid_open(mvid_t *v, const void *data, size_t size,
                   uint8_t *sheet, uint16_t *map);
int      mvid_seek(mvid_t *v, uint16_t frame);
int      mvid_next(mvid_t *v);       /* 1 = advanced, 0 = at end */
void     mvid_close(mvid_t *v);
/* Render the decoded frame to RGB565 (w*h, brightness applied). */
void     mvid_draw(const mvid_t *v, uint16_t *fb, unsigned w, unsigned h);

/* ---- encode (mvid_encode.c) ---- */
typedef struct {
    uint16_t width, height;      /* coded size */
    uint8_t  fps, bpp;           /* bpp: 4 or 8 */
    uint8_t  loop;
    uint16_t map_w, map_h;       /* logical map dims (default: visible) */
    const uint16_t *pal;         /* 4bpp: n*16 entries, 8bpp: n entries */
    uint16_t pal_count;
    int      keyframe;           /* force INTRA every N frames, 0=auto */
} mvid_enc_opts_t;

typedef struct mvid_enc mvid_enc_t;

/* Build a palette (median cut on a 5:5:5 histogram). bpp 4 -> 128 colours,
 * bpp 8 -> 256. `frames` is n_frames consecutive w*h RGB565 images. */
int  mvid_palette_build(const uint16_t *frames, size_t n_frames,
                        size_t w, size_t h, int bpp, uint16_t *pal, int *n_out);

mvid_enc_t *mvid_enc_new(const mvid_enc_opts_t *o);
void mvid_enc_free(mvid_enc_t *e);
/* Set the palette used by the following frames. Forces a keyframe. */
int  mvid_enc_palette(mvid_enc_t *e, const uint16_t *pal, int n);
/* Append one rendered RGB565 frame (w*h). Returns 0 / negative. */
int  mvid_enc_frame(mvid_enc_t *e, const uint16_t *rgb,
                    int16_t scroll_x, int16_t scroll_y, uint8_t brightness);
/* Static-map mode, used by the composer for pan shots: hand over the whole
 * source map once, then call mvid_enc_pan per frame. Zero re-quantisation. */
int  mvid_enc_source_map(mvid_enc_t *e, const uint8_t *tiles, int n_tiles,
                         const uint16_t *map, int map_w, int map_h, int bpp);
int  mvid_enc_pan(mvid_enc_t *e, int16_t scroll_x, int16_t scroll_y,
                  uint8_t brightness);
int  mvid_enc_write(mvid_enc_t *e, const char *path);
int  mvid_enc_frames(const mvid_enc_t *e);

#ifdef __cplusplus
}
#endif
#endif /* MVID_H */
