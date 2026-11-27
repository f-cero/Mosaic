/* SPDX-License-Identifier: Apache-2.0 */
/* Mosaic — ms_build.h: project -> cartridge. Shared by the CLI and the
 * Studio so the editor exports exactly what the engine runs. */
#ifndef MOSAIC_MS_BUILD_H
#define MOSAIC_MS_BUILD_H
#include "mosaic/ms_limits.h"
#include <stddef.h>

#define MS_BUILD_MAX_ERR 512

typedef struct {
    char   title[33], author[33];
    char   mode[12];
    int    qfa, voices, music_voices, sfx_voices;
    unsigned backdrop;
    char   paths[16][192]; int n_paths;
    char   dir[192];
    char   cart[MS_CART_BYTES_MAX];
    size_t cart_len;
    char   error[MS_BUILD_MAX_ERR];
    int    sheets, tiles, maps, songs, programs;
    size_t splash_bytes;
} ms_build_result_t;

/* Build a cart from a .mpj project file. Returns 0 on success.
 * `canonical` omits the Mosaic extension kinds (program/manifest), i.e. it
 * emits a strict cart16 v2 cart that the reference platform can mount. */
int ms_build_project(const char *project_path, ms_build_result_t *out);
int ms_build_project_opts(const char *project_path, ms_build_result_t *out, int canonical);

/* Converters (also exposed as CLI verbs). */
int ms_convert_sprite(const char *png, const char *out_spr2, int grid_w, int grid_h, int bpp);
int ms_convert_level(const char *png, int bpp, unsigned char *tiles_out, size_t *tiles_len,
                     unsigned char *map_out, size_t *map_len,
                     unsigned short *pal_out, int *pal_count, int *map_w, int *map_h,
                     int *logical_w, int *logical_h);
int ms_write_png(const char *path, int w, int h, const unsigned short *rgb565);

#endif
