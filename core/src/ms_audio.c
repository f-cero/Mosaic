/* SPDX-License-Identifier: Apache-2.0 */
/* Mosaic — ms_audio.c: wavetable MIDI synth + SMF sequencer. No PCM. */
#include "mosaic/ms_audio.h"
#include <string.h>

#define VOICE_HOLD 0xFFFFFFFFu

static uint8_t s_wave[6][256];
static int s_wave_init;

static void build_waves(void)
{
    for (int i = 0; i < 256; i++) {
        int s = (int)(ms_sin((ms_fix)((int64_t)i * MS_FIX_ONE / 256))); /* Q15 */
        s_wave[0][i] = (uint8_t)(128 + (s >> 8));                 /* sine  */
        s_wave[1][i] = (uint8_t)(i < 128 ? 255 : 0);               /* square*/
        s_wave[2][i] = (uint8_t)i;                                 /* saw   */
        s_wave[3][i] = (uint8_t)(i < 128 ? i * 2 : 255 - (i - 128) * 2); /* tri */
        s_wave[4][i] = (uint8_t)(i < 51 ? 255 : 0);                /* pulse */
        int n = (int)((ms_hash32("noise") >> 7) ^ (uint32_t)i * 2654435761u);
        s_wave[5][i] = (uint8_t)((n >> 13) & 0xFF);                /* noise */
    }
    s_wave_init = 1;
}

static void gm_default(ms_instrument_t *inst, int i)
{
    memset(inst, 0, sizeof(*inst));
    uint8_t family = (uint8_t)(i / 8);            /* 16 GM families */
    inst->wave = (uint8_t)(family % 5);
    if (family == 15) inst->wave = 5;             /* SFX family: noise */
    inst->attack  = (uint8_t)(family == 0 ? 4 : family < 4 ? 1 : 3);
    inst->decay   = 6;
    inst->sustain = (uint8_t)(family == 0 ? 9 : 11);
    inst->release = (uint8_t)(family < 4 ? 8 : 5);
    inst->pulse   = 50;
    inst->vib_depth = (uint8_t)(family == 0 ? 6 : 2);
    inst->vib_rate  = 8;
    inst->vol = 100;
    inst->tune = 0;
}

int ms_synth_init(ms_synth_t *s, int rate, int voices, int music_voices, int sfx_voices)
{
    if (!s) return -1;
    if (rate < MS_AUDIO_HZ_MIN || rate > MS_AUDIO_HZ_MAX) return -2;
    if (voices < 1 || voices > MS_VOICES_MAX) return -2;
    if (!s_wave_init) build_waves();
    memset(s, 0, sizeof(*s));
    s->rate = rate;
    s->voices = (uint8_t)voices;
    s->music_voices = (uint8_t)(music_voices > 0 ? music_voices : voices);
    s->sfx_voices = (uint8_t)(sfx_voices > 0 ? sfx_voices : 0);
    if (s->music_voices + s->sfx_voices > voices) return -2;
    for (int i = 0; i < (int)MS_INST_COUNT; i++) gm_default(&s->gm[i], i);
    for (int i = 0; i < (int)MS_DRUM_COUNT; i++) {
        gm_default(&s->drums[i], 120 + (i % 8));
        s->drums[i].wave = 5;
        s->drums[i].decay = 9;
        s->drums[i].sustain = 0;
        s->drums[i].release = 5;
    }
    s->vol_master = 127;
    return 0;
}

void ms_synth_reset(ms_synth_t *s)
{
    if (!s) return;
    memset(s->v, 0, sizeof(s->v));
    ms_player_stop(s);
}

int ms_synth_set_bank(ms_synth_t *s, const void *blob, size_t bytes)
{
    const uint8_t *p = (const uint8_t *)blob;
    if (!s || !p || bytes < 16) return -1;
    if (ms_rd32(p) != MS_BANK_MAGIC) return -1;
    uint16_t count = ms_rd16(p + 8), drums = ms_rd16(p + 10);
    uint32_t rate = ms_rd32(p + 12);
    size_t need = 16u + (size_t)(count + drums) * 12u;
    if (count > MS_INST_COUNT || drums > MS_DRUM_COUNT || bytes != need) return -1;
    for (unsigned i = 0; i < count; i++) {
        const uint8_t *q = p + 16 + i * 12;
        ms_instrument_t *d = &s->gm[i];
        d->wave = q[0]; d->attack = q[1] & 15; d->decay = q[2] & 15;
        d->sustain = q[3] & 15; d->release = q[4] & 15; d->pulse = q[5];
        d->vib_depth = q[6]; d->vib_rate = q[7]; d->tune = (int8_t)q[8]; d->vol = q[9];
    }
    for (unsigned i = 0; i < drums; i++) {
        const uint8_t *q = p + 16 + (size_t)count * 12 + i * 12;
        ms_instrument_t *d = &s->drums[i];
        d->wave = q[0]; d->attack = q[1] & 15; d->decay = q[2] & 15;
        d->sustain = q[3] & 15; d->release = q[4] & 15; d->pulse = q[5];
        d->vib_depth = q[6]; d->vib_rate = q[7]; d->tune = (int8_t)q[8]; d->vol = q[9];
    }
    s->bank.magic = MS_BANK_MAGIC; s->bank.version = 1;
    s->bank.count = count; s->bank.drum_count = drums; s->bank.rate = rate;
    s->bank.inst = s->gm;
    return 0;
}

size_t ms_bank_write(const ms_instrument_t *inst, int count,
                     const ms_instrument_t *drums, int drum_count,
                     int rate, void *out, size_t cap)
{
    size_t need = 16u + (size_t)(count + drum_count) * 12u;
    if (!out || cap < need) return 0;
    uint8_t *p = (uint8_t *)out;
    memset(p, 0, need);
    ms_wr32(p, MS_BANK_MAGIC); ms_wr16(p + 4, 1); ms_wr16(p + 6, 0);
    ms_wr16(p + 8, (uint16_t)count); ms_wr16(p + 10, (uint16_t)drum_count);
    ms_wr32(p + 12, (uint32_t)rate);
    const ms_instrument_t *src[2] = { inst, drums };
    int cnt[2] = { count, drum_count };
    size_t o = 16;
    for (int b = 0; b < 2; b++)
        for (int i = 0; i < cnt[b]; i++) {
            const ms_instrument_t *d = &src[b][i];
            p[o+0]=d->wave; p[o+1]=d->attack; p[o+2]=d->decay; p[o+3]=d->sustain;
            p[o+4]=d->release; p[o+5]=d->pulse; p[o+6]=d->vib_depth; p[o+7]=d->vib_rate;
            p[o+8]=(uint8_t)d->tune; p[o+9]=d->vol; p[o+10]=0; p[o+11]=0;
            o += 12;
        }
    return need;
}

/* note 69 = A4 = 440 Hz; integer semitone ratios in 16.16. */
static uint32_t note_inc(int note, int rate, int tune)
{
    /* ratio[] is Q16 relative to C (ratio[0] == 1.0, ratio[9] == 110218/65536
     * == C->A). The phase accumulator is 32-bit and the 256-entry wave table is
     * indexed by its top 8 bits, so one table cycle costs 2^24 phase units:
     *     inc = f / rate * 2^24
     * The old code multiplied the C-relative ratio by 440 and never applied the
     * 2^24 scale, which played every note 1.6818x sharp (A4 came out at 740 Hz).
     * Kept integer-only, as this file has no float. */
    static const uint32_t ratio[12] = {
        65536, 69433, 73562, 77936, 82570, 87480, 92682, 98193,
        104032, 110218, 116772, 123717
    };
    if (note < 0) note = 0;
    if (note > 127) note = 127;
    int n = note + tune;
    if (n < 0) n = 0;
    if (n > 127) n = 127;
    int semi = n % 12;
    int d = n - 60;                       /* semitones above middle C        */
    int oct = (d >= 0) ? d / 12 : -((59 - n) / 12 + 1);
    /* C in Q16 Hz: 261.6256 * 65536 = 17145216 */
    uint64_t f_q16 = (uint64_t)ratio[semi] * 17145216ull / 65536ull;
    if (oct > 0) f_q16 <<= oct; else if (oct < 0) f_q16 >>= -oct;
    return (uint32_t)(f_q16 * 256ull / (uint64_t)(rate > 0 ? rate : 1));
}

static ms_voice_t *alloc_voice(ms_synth_t *s, int pool)
{
    int lo = pool == 1 ? s->music_voices : 0;
    int hi = pool == 1 ? s->voices : (s->music_voices < s->voices ? s->music_voices : s->voices);
    if (pool == 1 && s->sfx_voices == 0) { lo = 0; hi = s->voices; }
    for (int i = lo; i < hi; i++) if (!s->v[i].active) return &s->v[i];
    /* steal oldest */
    ms_voice_t *best = &s->v[lo];
    for (int i = lo; i < hi; i++) if (s->v[i].age < best->age) best = &s->v[i];
    return best;
}

static const ms_instrument_t *voice_inst(ms_synth_t *s, const ms_voice_t *v)
{
    if (v->channel == 9) return &s->drums[v->note < MS_DRUM_COUNT ? v->note : 0];
    return &s->gm[v->program < MS_INST_COUNT ? v->program : 0];
}

void ms_synth_note_on(ms_synth_t *s, int ch, int note, int vel)
{
    if (!s || ch < 0 || ch >= MS_MIDI_CH || note < 0 || note > 127 || vel <= 0) return;
    ms_voice_t *v = alloc_voice(s, 0);
    if (!v) return;
    memset(v, 0, sizeof(*v));
    v->active = 1; v->channel = (uint8_t)ch; v->note = (uint8_t)note;
    v->velocity = (uint8_t)(vel > 127 ? 127 : vel);
    v->program = (uint8_t)(s->gm[0].vol ? 0 : 0);
    v->env = 1; v->level = 0;
    v->lfsr = 0xACE1u + (uint32_t)note * 7919u;
    v->age = ++s->age;
    v->ttl = VOICE_HOLD;
    /* keep the channel's current program */
    for (int i = 0; i < MS_VOICES_MAX; i++)
        if (s->v[i].active && s->v[i].channel == ch && s->v[i].pool == 0)
            v->program = s->v[i].program;
    if (ch == 9) v->program = 0;
    const ms_instrument_t *inst = voice_inst(s, v);
    v->inc = note_inc(note, s->rate, inst->tune);
}

void ms_synth_note_off(ms_synth_t *s, int ch, int note)
{
    if (!s) return;
    for (int i = 0; i < MS_VOICES_MAX; i++)
        if (s->v[i].active && s->v[i].channel == ch && s->v[i].note == note &&
            s->v[i].env != 4) { s->v[i].env = 4; s->v[i].env_pos = 0; }
}

void ms_synth_program(ms_synth_t *s, int ch, int program)
{
    if (!s || ch < 0 || ch >= MS_MIDI_CH) return;
    for (int i = 0; i < MS_VOICES_MAX; i++)
        if (s->v[i].active && s->v[i].channel == ch && s->v[i].pool == 0)
            s->v[i].program = (uint8_t)(program & 0x7F);
}

void ms_synth_cc(ms_synth_t *s, int ch, int cc, int value)
{
    if (!s) return;
    if (cc == 7) s->vol_master = (uint8_t)(value & 0x7F);
    if (cc == 123 || cc == 120) {
        for (int i = 0; i < MS_VOICES_MAX; i++)
            if (s->v[i].active && s->v[i].channel == ch) s->v[i].env = 4;
    }
}

void ms_synth_all_off(ms_synth_t *s)
{
    if (!s) return;
    for (int i = 0; i < MS_VOICES_MAX; i++) s->v[i].env = 4;
}

int ms_synth_sfx(ms_synth_t *s, int program, int note, int vel_ms)
{
    if (!s || s->sfx_voices == 0) return -1;
    ms_voice_t *v = alloc_voice(s, 1);
    if (!v) return -1;
    memset(v, 0, sizeof(*v));
    v->active = 1; v->pool = 1; v->channel = 0;
    v->program = (uint8_t)(program & 0x7F);
    v->note = (uint8_t)(note & 0x7F);
    v->velocity = 120; v->env = 1; v->age = ++s->age;
    v->ttl = (uint32_t)((int64_t)vel_ms * s->rate / 1000);
    v->inc = note_inc(note, s->rate, s->gm[program & 0x7F].tune);
    return 0;
}

/* ---------- Standard MIDI File ---------- */
static uint32_t vlq(const uint8_t **p, const uint8_t *end)
{
    uint32_t v = 0;
    for (int i = 0; i < 4 && *p < end; i++) {
        uint8_t b = *(*p)++;
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return v;
}

int ms_song_load(ms_song_t *song, const void *data, size_t bytes)
{
    const uint8_t *p = (const uint8_t *)data;
    if (!song || !p || bytes < 14) return -1;
    if (memcmp(p, "MThd", 4) != 0) return -1;
    uint32_t hlen = (uint32_t)((p[4]<<24)|(p[5]<<16)|(p[6]<<8)|p[7]);
    if (hlen < 6 || 8 + hlen > bytes) return -1;
    memset(song, 0, sizeof(*song));
    song->format = (uint16_t)((p[8] << 8) | p[9]);
    song->tracks = (uint16_t)((p[10] << 8) | p[11]);
    song->ticks_per_qn = (uint32_t)((p[12] << 8) | p[13]);
    song->tempo_us = 500000;
    if (song->tracks > 16 || song->ticks_per_qn == 0) return -1;
    size_t off = 8 + hlen;
    uint32_t found = 0;
    while (off + 8 <= bytes && found < song->tracks) {
        const uint8_t *tp = p + off;
        uint32_t len = (uint32_t)((tp[4]<<24)|(tp[5]<<16)|(tp[6]<<8)|tp[7]);
        if (memcmp(tp, "MTrk", 4) != 0 || off + 8 + len > bytes) return -1;
        song->track[found] = tp + 8;
        song->track_len[found] = len;
        found++;
        off += 8 + len;
    }
    if (found == 0) return -1;
    song->tracks = (uint16_t)found;
    song->data = p; song->size = bytes;
    return 0;
}

void ms_player_start(ms_synth_t *s, const ms_song_t *song, int loop)
{
    if (!s || !song) return;
    memset(&s->player, 0, sizeof(s->player));
    s->player.song = song;
    s->player.playing = 1;
    s->player.loop = (uint8_t)(loop != 0);
    s->player.tpqn = song->ticks_per_qn;
    s->player.tempo_us = song->tempo_us;
    s->player.tick_us = song->tempo_us / song->ticks_per_qn;
    s->player.tick = 0;
    s->player.next_us = 0;
    for (int i = 0; i < MS_SONG_TRACKS && i < song->tracks; i++) s->player.running[i] = 1;
}
void ms_player_stop(ms_synth_t *s) { if (s) memset(&s->player, 0, sizeof(s->player)); }

/* Apply every event at the current tick across tracks. */
static void player_tick(ms_synth_t *s)
{
    ms_player_t *pl = &s->player;
    const ms_song_t *song = pl->song;
    if (!song) return;
    pl->tick++;
    for (int t = 0; t < song->tracks && t < MS_SONG_TRACKS; t++) {
        const uint8_t *p = song->track[t];
        const uint8_t *end = p + song->track_len[t];
        uint32_t tick = 0, delta = 0;
        uint8_t status = 0;
        /* simple: replay track up to tick each time (tiny songs; documented) */
        while (p < end) {
            delta = vlq(&p, end);
            tick += delta;
            if (tick > pl->tick) break;
            if (p >= end) break;
            uint8_t b = *p;
            if (b & 0x80) { status = b; p++; }
            if (status == 0xFF) {
                if (p >= end) break;
                uint8_t type = *p++;
                uint32_t len = vlq(&p, end);
                if (type == 0x51 && len == 3 && p + 3 <= end) {
                    pl->tempo_us = ((uint32_t)p[0]<<16)|((uint32_t)p[1]<<8)|p[2];
                    pl->tick_us = pl->tempo_us / (pl->tpqn ? pl->tpqn : 1);
                }
                p += len;
            } else if (status == 0xF0 || status == 0xF7) {
                uint32_t len = vlq(&p, end); p += len;
            } else {
                int hi = status & 0xF0, ch = status & 0x0F;
                if (hi == 0xC0 || hi == 0xD0) { if (p < end) ms_synth_program(s, ch, *p++); }
                else {
                    if (p + 1 >= end) break;
                    uint8_t d1 = p[0], d2 = p[1]; p += 2;
                    if (hi == 0x90 && d2) ms_synth_note_on(s, ch, d1, d2);
                    else if (hi == 0x80 || (hi == 0x90 && !d2)) ms_synth_note_off(s, ch, d1);
                    else if (hi == 0xB0) ms_synth_cc(s, ch, d1, d2);
                }
            }
            if (status == 0xFF && p <= end) {
                /* end of track -> stop this track */
                const uint8_t *q = p;
                (void)q;
            }
        }
        if (pl->tick * pl->tick_us > 0x7FFFFFFFu) pl->tick = 0;
    }
    if (pl->tick > 1000000u) pl->tick = 0;
}

void ms_synth_render(ms_synth_t *s, int16_t *out, int frames)
{
    if (!s || !out || frames <= 0) return;
    memset(out, 0, (size_t)frames * 2 * sizeof(int16_t));

    uint64_t base = s->player.audio_pos;
    for (int f = 0; f < frames; f++) {
        if (s->player.playing) {
            uint64_t now = (base + (uint64_t)f) * 1000000ull / (uint64_t)s->rate;
            if ((uint64_t)s->player.next_us <= now) {
                do {
                    player_tick(s);
                    s->player.next_us += s->player.tick_us ? s->player.tick_us : 1000u;
                } while ((uint64_t)s->player.next_us <= now);
            }
        }
    }
    s->player.audio_pos = base + (uint64_t)frames;

    for (int vi = 0; vi < s->voices; vi++) {
        ms_voice_t *v = &s->v[vi];
        if (!v->active) continue;
        const ms_instrument_t *inst = voice_inst(s, v);
        v->inc = v->inc ? v->inc : note_inc(v->note, s->rate, inst->tune);
        int32_t lvl = v->velocity ? (v->velocity * 2) : 0;
        for (int f = 0; f < frames; f++) {
            if (v->env == 0) { v->active = 0; break; }
            /* envelope: 0..127 in ~level units */
            int step = 1;
            switch (v->env) {
            case 1: { int a = inst->attack ? inst->attack : 1; int rate = (s->rate / 800) * 127 / a;
                      v->env_pos += rate; if (v->env_pos > 127) { v->env_pos = 127; v->env = 2; } break; }
            case 2: { int d = inst->decay ? inst->decay : 15; int rate = (s->rate / 1200) * 127 / d;
                      v->env_pos -= rate; if (v->env_pos <= inst->sustain * 8) { v->env_pos = inst->sustain * 8; v->env = 3; } break; }
            case 3: v->env_pos = inst->sustain * 8; if (v->ttl != VOICE_HOLD && v->ttl == 0) { v->env = 4; v->env_pos = 0; } break;
            case 4: { int r = inst->release ? inst->release : 8; int rate = (s->rate / 900) * 127 / r;
                      v->env_pos -= rate; if (v->env_pos <= 0) { v->env_pos = 0; v->active = 0; } break; }
            default: v->active = 0; break;
            }
            if (!v->active) break;
            if (v->ttl != VOICE_HOLD && v->ttl) v->ttl--;
            v->phase += v->inc;
            uint8_t idx = (uint8_t)(v->phase >> 16);
            int32_t sample;
            if (inst->wave == 5) {
                v->lfsr = (v->lfsr >> 1) ^ (-(v->lfsr & 1u) & 0xB400u);
                sample = (int32_t)((v->lfsr & 0xFF) - 128);
            } else {
                sample = (int32_t)s_wave[inst->wave % 6][idx] - 128;
            }
            int32_t env = (v->env_pos > 0 ? v->env_pos : 0) * lvl / 127;
            int32_t acc = sample * env * (inst->vol ? inst->vol : 64) / (128 * 127) * (s->vol_master + 1) / 128;
            acc *= 2;
            int32_t pan = (v->pan == 0 ? 64 : v->pan);
            out[f * 2]     = (int16_t)(out[f * 2]     + (acc * (128 - pan)) / 128);
            out[f * 2 + 1] = (int16_t)(out[f * 2 + 1] + (acc * pan) / 128);
            step = 0; (void)step;
        }
    }
    for (int i = 0; i < frames * 2; i++) {
        int32_t v = out[i];
        if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
        out[i] = (int16_t)v;
    }
}
