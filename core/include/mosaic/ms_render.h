/* SPDX-License-Identifier: Apache-2.0 */
/* Mosaic — ms_render.h: the picture processor. Targets a register file and
 * scans out RGB565. Software PPU: tile backgrounds, OAM sprites, affine
 * layers, palettes, colour math, mosaic, brightness, QFA upscale.
 * Reference behaviour extracted from the ESP-16 / PPU-16 contract. */
#ifndef MOSAIC_MS_RENDER_H
#define MOSAIC_MS_RENDER_H
#include "mosaic/ms_limits.h"
#include "mosaic/ms_types.h"

typedef enum {
    MS_MODE_TEXT = 0,   /* 2 x 4bpp tile BGs + sprites            */
    MS_MODE_RICH,       /* 8bpp BG + 4bpp BG + sprites            */
    MS_MODE_AFFINE,     /* 1 affine + 1 tile BG + sprites         */
    MS_MODE_AFFINE2     /* 2 affine + sprites                     */
} ms_mode_t;

#define MS_OAM_FLIP_H  0x01u
#define MS_OAM_FLIP_V  0x02u
#define MS_OAM_MATH    0x04u   /* this sprite is a colour-math operand */
#define MS_OAM_HIDE    0x80u

typedef struct {
    const uint8_t  *tiles;      /* tile pixel data, 8x8, bpp packed      */
    const uint16_t *map;        /* map entries, 16-bit SNES packing      */
    uint8_t  bpp;               /* 2, 4 or 8                             */
    uint8_t  map_w, map_h;      /* padded pow2 dims: row stride + mask   */
    uint8_t  logical_w, logical_h; /* visible tile dims (0 = map_w/h)    */
    int16_t  scroll_x, scroll_y;
    uint8_t  priority;          /* 0 (front) .. 3 (back)                 */
    uint8_t  enabled;
    uint8_t  mosaic;            /* 0 = off, else pixel block size        */
    uint8_t  math;              /* 1 = layer participates in colour math */
    const int16_t *line_scroll_x; /* optional h-entry raster table       */
    const int16_t *line_scroll_y;
} ms_bg_t;

/* 16.16 2x2 matrix. Maps a sampling coordinate about the sprite centre:
 * src = M * (px - w/2, py - h/2) + (w/2, h/2). Stationary scale/rotate/shear. */
typedef struct { ms_fix a, b, c, d; } ms_matrix_t;

ms_matrix_t ms_matrix_identity(void);
ms_matrix_t ms_matrix_scale(ms_fix sx, ms_fix sy);
ms_matrix_t ms_matrix_rot(ms_fix turns);          /* 1.0 == 360 degrees   */
ms_matrix_t ms_matrix_shear(ms_fix kx, ms_fix ky);/* src += k * other axis */
ms_matrix_t ms_matrix_mul(ms_matrix_t lhs, ms_matrix_t rhs); /* rhs then lhs */

typedef struct {
    int16_t  x, y;              /* top-left, screen space                */
    uint8_t  w, h;              /* 8..64, multiples of 8                 */
    uint8_t  bpp;               /* 4 or 8                                */
    uint8_t  pal_slot;          /* 4bpp: 16-colour slot in sprite bank   */
    uint8_t  priority;          /* 0 front .. 3 back                     */
    uint8_t  flags;             /* MS_OAM_*                              */
    int8_t   affine;            /* object matrix index, or -1            */
    const ms_matrix_t *line_mat;/* optional per-scanline table, line_rows  */
    uint16_t line_rows;         /* entries in line_mat; 0 = fill the height */
    const uint8_t *tiles;       /* NULL -> ppu.spr_tiles                 */
} ms_oam_t;

typedef struct {
    uint16_t backdrop;          /* RGB565                                */
    uint8_t  brightness;        /* 0..16, 16 = normal                    */
    uint8_t  mosaic;
    uint8_t  math_on;           /* 0 = colour math disabled              */
    uint8_t  color_math;        /* 0 add, 1 sub, 2 add-half, 3 sub-half  */
    ms_fix   math_factor;       /* 0..MS_FIX_ONE                         */
} ms_ppu_fx_t;

typedef struct {
    ms_mode_t   mode;
    uint16_t    width, height;  /* 320x240 or 160x120 (QFA)              */
    uint16_t    pal_bg[MS_PAL_ENTRIES];
    uint16_t    pal_spr[MS_PAL_ENTRIES];
    const uint8_t *spr_tiles;   /* default sprite tileset                */
    ms_bg_t     bg[MS_MAX_BG];
    ms_oam_t    oam[MS_MAX_SPRITES];
    uint16_t    oam_count;
    ms_matrix_t mat[MS_MAX_MATRICES];
    ms_ppu_fx_t fx;
    uint32_t    frame;
} ms_ppu_t;

void ms_ppu_reset(ms_ppu_t *ppu, ms_mode_t mode, uint16_t w, uint16_t h);
/* Render one frame to `fb` (w*h RGB565, stride = w). */
void ms_ppu_render(ms_ppu_t *ppu, uint16_t *fb);
/* QFA: render 160x120 into scratch, expand 2x into a 320x240 buffer. */
void ms_ppu_render_qfa(ms_ppu_t *ppu, uint16_t *fb, uint16_t *scratch);

/* Colour math for one pixel: blend `over` with the pixel underneath it.
 * mode: 0 add, 1 sub, 2 add-half, 3 sub-half.  factor: 0 = pure `over`,
 * MS_FIX_ONE = the full math result. Channels clamp to the RGB565 range. */
uint16_t ms_color_math(uint16_t under, uint16_t over, uint8_t mode, ms_fix factor);

#endif
