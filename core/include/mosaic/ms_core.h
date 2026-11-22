/* SPDX-License-Identifier: Apache-2.0 */
/* Mosaic — ms_core.h: the console object and frame contract. */
#ifndef MOSAIC_MS_CORE_H
#define MOSAIC_MS_CORE_H
#include "mosaic/ms_render.h"
#include "mosaic/ms_audio.h"
#include "mosaic/ms_input.h"
#include "mosaic/ms_game.h"

typedef struct {
    uint8_t  mode, qfa;
    uint16_t voices;
    int      audio_hz, music_voices, sfx_voices;
} ms_config_t;

typedef void (*ms_audio_sink_fn)(void *user, const int16_t *stereo, int frames);

typedef struct {
    ms_config_t cfg;
    ms_ppu_t    ppu;
    ms_synth_t  synth;
    ms_input_t  input;
    uint16_t   *fb;          /* 320x240 RGB565 output                 */
    uint16_t    qfa_scratch[MS_QFA_W * MS_QFA_H];
    int16_t     abuf[1152 * 2];
    ms_audio_sink_fn audio_sink;
    void       *audio_user;
    uint32_t    frame;
    uint8_t     ready;
} ms_core_t;

int  ms_config_check(const ms_config_t *cfg);
int  ms_core_init(ms_core_t *core, const ms_config_t *cfg, uint16_t *framebuffer, void *arena, size_t arena_bytes);
void ms_core_shutdown(ms_core_t *core);
void ms_core_set_audio_sink(ms_core_t *core, ms_audio_sink_fn fn, void *user);
/* One frame: update the game, render video, render+emit one audio block. */
int  ms_core_run_frame(ms_core_t *core, ms_game_t *game, const ms_input_t *in);

#endif
