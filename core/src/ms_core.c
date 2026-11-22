/* SPDX-License-Identifier: Apache-2.0 */
/* Mosaic — ms_core.c: console init + frame contract. */
#include "mosaic/ms_core.h"
#include <string.h>

int ms_config_check(const ms_config_t *cfg)
{
    if (!cfg) return -1;
    if (cfg->mode > MS_MODE_AFFINE2) return -1;
    if (cfg->qfa != 0 && cfg->qfa != 1) return -1;
    if (cfg->voices < 1 || cfg->voices > MS_VOICES_MAX) return -1;
    if (cfg->audio_hz < MS_AUDIO_HZ_MIN || cfg->audio_hz > MS_AUDIO_HZ_MAX) return -1;
    if ((unsigned)cfg->music_voices + cfg->sfx_voices > cfg->voices) return -1;
    return 0;
}

int ms_core_init(ms_core_t *core, const ms_config_t *cfg, uint16_t *fb, void *arena, size_t arena_bytes)
{
    (void)arena;
    if (!core || !cfg || !fb) return -1;
    if (ms_config_check(cfg) != 0) return -2;
    if (arena_bytes < 1024) return -3;
    memset(core, 0, sizeof(*core));
    core->cfg = *cfg;
    core->fb = fb;
    ms_ppu_reset(&core->ppu, (ms_mode_t)cfg->mode,
                 cfg->qfa ? MS_QFA_W : MS_SCREEN_W,
                 cfg->qfa ? MS_QFA_H : MS_SCREEN_H);
    if (ms_synth_init(&core->synth, cfg->audio_hz, cfg->voices,
                      cfg->music_voices, cfg->sfx_voices) != 0) return -4;
    core->ready = 1;
    return 0;
}

void ms_core_shutdown(ms_core_t *core) { if (core) core->ready = 0; }
void ms_core_set_audio_sink(ms_core_t *core, ms_audio_sink_fn fn, void *user)
{
    if (core) { core->audio_sink = fn; core->audio_user = user; }
}

int ms_core_run_frame(ms_core_t *core, ms_game_t *game, const ms_input_t *in)
{
    if (!core || !core->ready) return -1;
    const ms_input_t *pin = in ? in : &core->input;
    if (game) { game->ppu = &core->ppu; game->synth = &core->synth; ms_game_update(game, pin); }
    if (core->cfg.qfa) ms_ppu_render_qfa(&core->ppu, core->fb, core->qfa_scratch);
    else ms_ppu_render(&core->ppu, core->fb);
    int frames = (core->cfg.audio_hz + MS_FPS - 1) / MS_FPS;
    if (frames > 1152) frames = 1152;
    ms_synth_render(&core->synth, core->abuf, frames);
    if (core->audio_sink) core->audio_sink(core->audio_user, core->abuf, frames);
    core->frame++;
    return 0;
}
