/* SPDX-License-Identifier: Apache-2.0 */
/* Mosaic — ms_audio.h: pure-synthesis MIDI audio. No PCM, no floats.
 * 16 voices, GM-lite wavetable generated deterministically at init, plus
 * optional .sbn banks and Standard MIDI File sequencing. */
#ifndef MOSAIC_MS_AUDIO_H
#define MOSAIC_MS_AUDIO_H
#include "mosaic/ms_limits.h"
#include "mosaic/ms_types.h"

#define MS_INST_COUNT 128u
#define MS_DRUM_COUNT 48u

typedef struct {
    uint8_t wave;       /* 0 sine 1 square 2 saw 3 tri 4 pulse 5 noise  */
    uint8_t attack;     /* 0..15  */
    uint8_t decay;      /* 0..15  */
    uint8_t sustain;    /* 0..15  */
    uint8_t release;    /* 0..15  */
    uint8_t pulse;      /* duty 1..15 for wave 4                        */
    uint8_t vib_depth;  /* 0..15  */
    uint8_t vib_rate;   /* 0..15  */
    int8_t  tune;       /* semitone offset                              */
    uint8_t vol;        /* 0..127 */
} ms_instrument_t;

typedef struct {
    uint32_t magic, version;
    uint16_t count, drum_count;
    uint32_t rate;
    const ms_instrument_t *inst;
} ms_bank_t;

typedef struct {
    const uint8_t *data; size_t size;
    uint32_t tempo_us; uint32_t ticks_per_qn; uint16_t format, tracks;
    const uint8_t *track[16]; uint32_t track_len[16];
} ms_song_t;

typedef struct {
    uint8_t  active;
    uint8_t  channel;
    uint8_t  note;
    uint8_t  program;
    uint8_t  velocity;
    uint8_t  pool;       /* 0 music, 1 sfx                          */
    uint8_t  env;        /* 0 off 1 attack 2 decay 3 sustain 4 rel   */
    uint8_t  level;      /* 0..127                                  */
    uint8_t  pan;
    uint8_t  noise;
    int16_t  env_pos;
    uint32_t phase;      /* 16.16 phase, 256-entry table            */
    uint32_t inc;
    uint32_t age;
    uint32_t ttl;        /* sfx: samples until release              */
    uint32_t lfsr;
} ms_voice_t;

#define MS_SONG_TRACKS 4
typedef struct {
    const ms_song_t *song;
    uint8_t  playing, loop;
    uint8_t  track;
    uint32_t tick, tick_us, next_us, tempo_us, tpqn;
    uint8_t  running[MS_SONG_TRACKS];
    uint8_t  last_status[MS_SONG_TRACKS];
    /* monotonic sample clock: ms_synth_render() is called in blocks, so the
     * sequencer must not compare against block-local frame numbers. */
    uint64_t audio_pos;
} ms_player_t;

typedef struct {
    int      rate;
    uint8_t  voices;
    uint8_t  music_voices, sfx_voices;
    ms_bank_t bank;
    ms_instrument_t gm[MS_INST_COUNT];
    ms_instrument_t drums[MS_DRUM_COUNT];
    ms_voice_t v[MS_VOICES_MAX];
    uint32_t age;
    uint8_t  vol_master;   /* 0..127 */
    uint8_t  wave_phase[256][2]; /* not used; kept for ABI padding      */
    ms_player_t player;
} ms_synth_t;

int  ms_synth_init(ms_synth_t *s, int rate, int voices, int music_voices, int sfx_voices);
void ms_synth_reset(ms_synth_t *s);
int  ms_synth_set_bank(ms_synth_t *s, const void *blob, size_t bytes); /* .sbn  */
void ms_synth_note_on(ms_synth_t *s, int ch, int note, int vel);
void ms_synth_note_off(ms_synth_t *s, int ch, int note);
void ms_synth_program(ms_synth_t *s, int ch, int program);
void ms_synth_cc(ms_synth_t *s, int ch, int cc, int value);
void ms_synth_all_off(ms_synth_t *s);
int  ms_synth_sfx(ms_synth_t *s, int program, int note, int vel_ms);

int  ms_song_load(ms_song_t *song, const void *data, size_t bytes);
void ms_player_start(ms_synth_t *s, const ms_song_t *song, int loop);
void ms_player_stop(ms_synth_t *s);
/* Render `frames` stereo sample pairs into `out` (interleaved L,R int16). */
void ms_synth_render(ms_synth_t *s, int16_t *out, int frames);

/* .sbn bank writer (host tool) */
size_t ms_bank_write(const ms_instrument_t *inst, int count, const ms_instrument_t *drums,
                     int drum_count, int rate, void *out, size_t cap);

#endif
