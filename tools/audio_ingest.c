/* SPDX-License-Identifier: Apache-2.0 */
/* Mosaic — audio_ingest.c: recorded audio -> a midicore Standard MIDI File.
 *
 * midicore has no PCM. Its voices are wavetables driven by an SMF sequencer
 * (16 voices, GM-lite programs, noise drums), so "playing a recording" can
 * only mean *transcribing* it into notes. Two modes:
 *
 *   music   polyphonic. Harmonic-sum salience over MIDI 36..96, octave
 *           suppression, per-note onset/release tracking with hysteresis,
 *           register split across two programs, optional drum track from
 *           spectral-flux onsets.
 *   speech  monophonic. YIN F0 -> quantised note contour with syllable
 *           segmentation; unvoiced bursts become noise-drum consonants.
 *
 * Everything is float-free on the output side: the SMF is integer, ticks are
 * 5 ms (120 BPM, 100 tpqn), and the timing grid is the same one midicore's
 * player computes. Analysis uses floats; the console never sees them.
 */
#include "mosaic/ms_audio.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define AI_RATE     22050                 /* analysis rate                    */
#define AI_NMAX     2048                  /* largest FFT                      */
#define AI_N_MUSIC  2048                  /* ~93 ms window, 10.8 Hz bins      */
#define AI_HOP_M    256                   /* 11.6 ms                          */
#define AI_N_SPEECH 1024
#define AI_HOP_S    220                   /* 10 ms                            */
#define AI_TPQN     100u
#define AI_TICK_MS  5.0                   /* 120 BPM / 100 tpqn -> 5 ms       */
#define AI_NOTE_MIN 36
#define AI_NOTE_MAX 96
#define AI_NN       (AI_NOTE_MAX - AI_NOTE_MIN + 1)
#define AI_MAXPOLY  6

#define AI_DRUM_KICK 36
#define AI_DRUM_SNARE 38
#define AI_DRUM_HAT  42
#define AI_DRUM_OPEN 46

/* ------------------------------------------------------------------ bytes */
static uint16_t ai_rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t ai_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
/* SMF is big-endian; the WAV writer above is little-endian. */
static void ai_wr16be(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void ai_wr32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static void ai_wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void ai_wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static float ai_clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static int   ai_clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* -------------------------------------------------------------------- wav */
typedef struct { int rate, n; float *x; } af_t;

typedef struct { float *x; int n, rate; } afh_t;

static int ai_load_wav(const char *path, int target, af_t *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "audio: cannot open %s\n", path); return -1; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 44) { fclose(f); fprintf(stderr, "audio: %s is too short to be a WAV\n", path); return -1; }
    fseek(f, 0, SEEK_SET);
    uint8_t *b = (uint8_t *)malloc((size_t)sz);
    if (!b) { fclose(f); return -1; }
    size_t got = fread(b, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(b); return -1; }

    if (memcmp(b, "RIFF", 4) != 0 || memcmp(b + 8, "WAVE", 4) != 0) {
        free(b);
        fprintf(stderr, "audio: %s is not RIFF/WAVE (decode it to 16-bit WAV first)\n", path);
        return -1;
    }
    int tag = 0, ch = 0, rate = 0, bits = 0;
    const uint8_t *data = NULL;
    uint32_t dlen = 0;
    size_t off = 12;
    while (off + 8 <= (size_t)sz) {
        const uint8_t *c = b + off;
        uint32_t clen = ai_rd32(c + 4);
        if (memcmp(c, "fmt ", 4) == 0 && clen >= 16 && off + 8 + 16 <= (size_t)sz) {
            tag = ai_rd16(c + 8); ch = ai_rd16(c + 10);
            rate = (int)ai_rd32(c + 12); bits = ai_rd16(c + 22);
            if (tag == 0xFFFE && clen >= 40) tag = ai_rd16(c + 32);
        } else if (memcmp(c, "data", 4) == 0) {
            data = c + 8; dlen = clen;
        }
        off += 8 + (size_t)clen + (clen & 1u);
    }
    if (!data || !ch || !rate || !bits || (tag != 1 && tag != 3)) {
        fprintf(stderr, "audio: %s: unsupported WAV (tag %d, %d ch, %d bit)\n", path, tag, ch, bits);
        free(b);
        return -1;
    }
    int bytes = bits / 8;
    size_t frames = (size_t)dlen / (size_t)(bytes * ch);
    if (frames == 0) { free(b); fprintf(stderr, "audio: %s holds no samples\n", path); return -1; }

    float *mono = (float *)malloc(frames * sizeof(float));
    if (!mono) { free(b); return -1; }
    for (size_t i = 0; i < frames; i++) {
        float acc = 0.0f;
        for (int c = 0; c < ch; c++) {
            const uint8_t *p = data + (i * (size_t)ch + (size_t)c) * (size_t)bytes;
            float v = 0.0f;
            if (tag == 3 && bits == 32) { float t; memcpy(&t, p, 4); v = t; }
            else if (bits == 8)  v = ((float)p[0] - 128.0f) / 128.0f;
            else if (bits == 16) v = (float)(int16_t)ai_rd16(p) / 32768.0f;
            else if (bits == 24) {
                int32_t s = (int32_t)(((uint32_t)p[2] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[0] << 8)) >> 8;
                v = (float)s / 8388608.0f;
            } else if (bits == 32) v = (float)(int32_t)ai_rd32(p) / 2147483648.0f;
            acc += v;
        }
        mono[i] = acc / (float)ch;
    }
    free(b);

    /* trim leading/trailing near-silence so dead air does not become notes */
    size_t a = 0, z = frames;
    while (a < frames && fabsf(mono[a]) < 0.002f) a++;
    while (z > a && fabsf(mono[z - 1]) < 0.002f) z--;
    if (z <= a + 64) { a = 0; z = frames; }

    int src = rate;
    size_t n = z - a;
    float *x = mono + a;
    if (src != target) {
        double step = (double)src / (double)target;
        size_t on = (size_t)((double)n / step);
        if (on < 64) on = 64;
        float *o = (float *)malloc(on * sizeof(float));
        if (!o) { free(mono); return -1; }
        for (size_t i = 0; i < on; i++) {
            double s = (double)i * step;
            size_t i0 = (size_t)s;
            double fr = s - (double)i0;
            float s0 = (i0 < n) ? x[i0] : 0.0f;
            float s1 = (i0 + 1 < n) ? x[i0 + 1] : 0.0f;
            o[i] = (float)((1.0 - fr) * s0 + fr * s1);
        }
        out->x = o; out->n = (int)on; out->rate = target;
        free(mono);
    } else {
        out->x = x; out->n = (int)n; out->rate = target;
        /* x points inside mono: keep it simple and own a copy */
        float *c = (float *)malloc(n * sizeof(float));
        if (!c) { free(mono); return -1; }
        memcpy(c, x, n * sizeof(float));
        free(mono);
        out->x = c;
    }
    return 0;
}

static int ai_write_wav(const char *path, const int16_t *inter, int frames, int rate, int ch)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "audio: cannot write %s\n", path); return -1; }
    uint32_t data = (uint32_t)frames * (uint32_t)ch * 2u;
    uint8_t h[44];
    memcpy(h, "RIFF", 4); ai_wr32(h + 4, 36u + data); memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4); ai_wr32(h + 16, 16); ai_wr16(h + 20, 1);
    ai_wr16(h + 22, (uint32_t)ch); ai_wr32(h + 24, (uint32_t)rate);
    ai_wr32(h + 28, (uint32_t)(rate * ch * 2));
    ai_wr16(h + 32, (uint32_t)(ch * 2)); ai_wr16(h + 34, 16);
    memcpy(h + 36, "data", 4); ai_wr32(h + 40, data);
    fwrite(h, 1, 44, f);
    fwrite(inter, 2, (size_t)frames * (size_t)ch, f);
    fclose(f);
    return 0;
}

/* -------------------------------------------------------------- fft / stft */
static float g_cos[AI_NMAX / 2], g_sin[AI_NMAX / 2];
static float g_win_m[AI_N_MUSIC], g_win_s[AI_N_SPEECH];
static int   g_init_done;

static void ai_init(void)
{
    if (g_init_done) return;
    for (int i = 0; i < AI_NMAX / 2; i++) {
        double a = 2.0 * M_PI * (double)i / (double)AI_NMAX;
        g_cos[i] = (float)cos(a);
        g_sin[i] = (float)sin(a);
    }
    for (int i = 0; i < AI_N_MUSIC; i++)
        g_win_m[i] = (float)(0.5 - 0.5 * cos(2.0 * M_PI * (double)i / (double)(AI_N_MUSIC - 1)));
    for (int i = 0; i < AI_N_SPEECH; i++)
        g_win_s[i] = (float)(0.5 - 0.5 * cos(2.0 * M_PI * (double)i / (double)(AI_N_SPEECH - 1)));
    g_init_done = 1;
}

static void ai_fft(float *re, float *im, int n, int nbits)
{
    for (int i = 0; i < n; i++) {
        unsigned v = (unsigned)i, r = 0;
        for (int b = 0; b < nbits; b++) { r = (r << 1) | (v & 1u); v >>= 1; }
        if ((int)r > i) {
            float t = re[i]; re[i] = re[(int)r]; re[(int)r] = t;
            t = im[i]; im[i] = im[(int)r]; im[(int)r] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        int half = len >> 1, step = AI_NMAX / len;
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < half; k++) {
                float wr = g_cos[k * step], wi = -g_sin[k * step];
                int a = i + k, b = a + half;
                float xr = re[b] * wr - im[b] * wi;
                float xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr; im[b] = im[a] - xi;
                re[a] += xr;        im[a] += xi;
            }
        }
    }
}

/* magnitude spectrum of one windowed frame, into mag[0..n/2) */
static void ai_spectrum(const float *x, int n, int pos, int win, float *mag)
{
    static float re[AI_NMAX], im[AI_NMAX];
    const float *w = (win == AI_N_SPEECH) ? g_win_s : g_win_m;
    for (int i = 0; i < win; i++) {
        int s = pos + i;
        float v = (s >= 0 && s < n) ? x[s] : 0.0f;
        re[i] = v * w[i]; im[i] = 0.0f;
    }
    int nbits = 0;
    while ((1 << nbits) < win) nbits++;
    ai_fft(re, im, win, nbits);
    for (int k = 0; k < win / 2; k++)
        mag[k] = sqrtf(re[k] * re[k] + im[k] * im[k]);
}

static float ai_rms(const float *x, int n, int pos, int win)
{
    double acc = 0.0;
    int cnt = 0;
    for (int i = 0; i < win; i++) {
        int s = pos + i;
        float v = (s >= 0 && s < n) ? x[s] : 0.0f;
        acc += (double)v * v; cnt++;
    }
    return cnt ? (float)sqrt(acc / cnt) : 0.0f;
}

/* harmonic-sum salience for one fundamental */
static float ai_salience(const float *mag, int nb, float bin_hz, float f0)
{
    float s = 0.0f;
    for (int h = 1; h <= 8; h++) {
        float f = f0 * (float)h;
        if (f > 5000.0f) break;
        int b = (int)(f / bin_hz + 0.5f);
        if (b < 1 || b >= nb - 1) break;
        float m = mag[b];
        if (mag[b + 1] > m) m = mag[b + 1];
        if (mag[b - 1] > m) m = mag[b - 1];
        s += m / (float)h;
    }
    return s;
}

static float ai_centroid(const float *mag, int nb, float bin_hz)
{
    double num = 0.0, den = 0.0;
    for (int k = 1; k < nb; k++) {
        float f = (float)k * bin_hz;
        if (f < 80.0f || f > 6000.0f) continue;
        num += (double)f * mag[k];
        den += mag[k];
    }
    return den > 0.0 ? (float)(num / den) : 0.0f;
}

static float ai_band_energy(const float *mag, int nb, float bin_hz, float lo, float hi)
{
    float s = 0.0f;
    int a = (int)(lo / bin_hz), b = (int)(hi / bin_hz);
    if (a < 1) a = 1;
    if (b > nb - 1) b = nb - 1;
    for (int k = a; k <= b; k++) s += mag[k];
    return s;
}

/* --------------------------------------------------------------------- yin */
typedef struct { float f0, aper, rms, centroid; } ai_pitch_t;

static ai_pitch_t ai_yin(const float *x, int n, int pos, int win, int rate,
                         const float *mag, int nb, float bin_hz)
{
    ai_pitch_t r;
    memset(&r, 0, sizeof r);
    r.rms = ai_rms(x, n, pos, win);
    r.centroid = mag ? ai_centroid(mag, nb, bin_hz) : 0.0f;
    if (r.rms < 0.0005f) { r.aper = 1.0f; return r; }

    int tmin = rate / 420;
    int tmax = rate / 65;
    if (tmin < 2) tmin = 2;
    if (tmax > win / 2) tmax = win / 2;
    int W = win - tmax;
    if (W < 64) { r.aper = 1.0f; return r; }

    static float d[1024];
    for (int tau = 0; tau <= tmax; tau++) d[tau] = 0.0f;
    for (int tau = tmin; tau <= tmax; tau++) {
        double acc = 0.0;
        const float *p = x + pos;
        int lim = (pos + W < n) ? W : (n - pos > 0 ? n - pos : 0);
        if (lim > W) lim = W;
        for (int i = 0; i < lim; i++) {
            float df = p[i] - p[i + tau];
            acc += (double)df * df;
        }
        d[tau] = (float)acc;
    }
    double run = 0.0;
    d[0] = 1.0f;
    for (int t = 1; t <= tmax; t++) {
        run += d[t];
        d[t] = (run > 0.0) ? (float)(d[t] * (double)t / run) : 1.0f;
    }
    int best = -1;
    for (int t = tmin; t < tmax; t++) {
        if (d[t] < 0.20f && d[t] <= d[t - 1] && d[t] <= d[t + 1]) { best = t; break; }
    }
    if (best < 0) {
        float lo = 1e9f;
        for (int t = tmin; t < tmax; t++) if (d[t] < lo) { lo = d[t]; best = t; }
    }
    if (best <= 0) { r.aper = 1.0f; return r; }
    float delta = 0.0f;
    if (best > 1 && best + 1 <= tmax) {
        float a = d[best - 1], b = d[best], c = d[best + 1];
        float den = (a - 2.0f * b + c);
        if (fabsf(den) > 1e-9f) delta = 0.5f * (a - c) / den;
        if (delta > 1.0f) delta = 1.0f;
        if (delta < -1.0f) delta = -1.0f;
    }
    r.aper = ai_clampf(d[best], 0.0f, 1.0f);
    r.f0 = (float)rate / ((float)best + delta);
    if (r.f0 < 50.0f || r.f0 > 500.0f) { r.f0 = 0.0f; r.aper = 1.0f; }
    return r;
}

/* --------------------------------------------------------------------- smf */
typedef struct { uint32_t tick, seq; uint8_t msg[4]; uint8_t len; } ai_ev_t;
typedef struct { ai_ev_t *e; int n, cap; } ai_evl_t;

static void ai_ev(ai_evl_t *l, uint32_t tick, uint8_t a, uint8_t b, uint8_t c, int len)
{
    if (l->n >= l->cap) {
        int cap = l->cap ? l->cap * 2 : 256;
        ai_ev_t *e = (ai_ev_t *)realloc(l->e, (size_t)cap * sizeof(ai_ev_t));
        if (!e) return;
        l->e = e; l->cap = cap;
    }
    ai_ev_t *e = &l->e[l->n];
    e->tick = tick; e->seq = (uint32_t)l->n; e->len = (uint8_t)len;
    e->msg[0] = a; e->msg[1] = b; e->msg[2] = c; e->msg[3] = 0;
    l->n++;
}

static int ai_ev_cmp(const void *pa, const void *pb)
{
    const ai_ev_t *a = (const ai_ev_t *)pa, *b = (const ai_ev_t *)pb;
    if (a->tick != b->tick) return a->tick < b->tick ? -1 : 1;
    return a->seq < b->seq ? -1 : (a->seq > b->seq ? 1 : 0);
}

static int ai_vlq(uint8_t *o, uint32_t v)
{
    uint8_t tmp[5];
    int n = 0;
    tmp[n++] = (uint8_t)(v & 0x7Fu);
    v >>= 7;
    while (v) { tmp[n++] = (uint8_t)((v & 0x7Fu) | 0x80u); v >>= 7; }
    for (int i = 0; i < n; i++) o[i] = tmp[n - 1 - i];
    return n;
}

static int ai_smf_write(const char *path, ai_evl_t *l, const char *title, uint32_t *end_tick)
{
    qsort(l->e, (size_t)l->n, sizeof(ai_ev_t), ai_ev_cmp);

    size_t cap = (size_t)l->n * 8u + 256u;
    uint8_t *body = (uint8_t *)malloc(cap);
    if (!body) return -1;
    size_t o = 0;
    body[o++] = 0x00; body[o++] = 0xFF; body[o++] = 0x51; body[o++] = 0x03;
    body[o++] = 0x07; body[o++] = 0xA1; body[o++] = 0x20;          /* 120 BPM */
    if (title && title[0]) {
        size_t tl = strlen(title);
        if (tl > 120) tl = 120;
        body[o++] = 0x00; body[o++] = 0xFF; body[o++] = 0x01;
        body[o++] = (uint8_t)tl;
        memcpy(body + o, title, tl);
        o += tl;
    }
    uint32_t last = 0;
    for (int i = 0; i < l->n; i++) {
        ai_ev_t *e = &l->e[i];
        uint8_t d[5];
        int dn = ai_vlq(d, e->tick - last);
        last = e->tick;
        if (o + (size_t)dn + 4u + 4u > cap) {
            cap *= 2;
            uint8_t *nb = (uint8_t *)realloc(body, cap);
            if (!nb) { free(body); return -1; }
            body = nb;
        }
        memcpy(body + o, d, (size_t)dn);
        o += (size_t)dn;
        memcpy(body + o, e->msg, e->len);
        o += e->len;
    }
    body[o++] = 0x00; body[o++] = 0xFF; body[o++] = 0x2F; body[o++] = 0x00;
    if (end_tick) *end_tick = last;

    FILE *f = fopen(path, "wb");
    if (!f) { free(body); fprintf(stderr, "audio: cannot write %s\n", path); return -1; }
    uint8_t h[14 + 8];
    memcpy(h, "MThd", 4); ai_wr32be(h + 4, 6); ai_wr16be(h + 8, 0); ai_wr16be(h + 10, 1);
    ai_wr16be(h + 12, AI_TPQN);
    memcpy(h + 14, "MTrk", 4); ai_wr32be(h + 18, (uint32_t)o);
    fwrite(h, 1, 22, f);
    fwrite(body, 1, o, f);
    fclose(f);
    free(body);
    return 0;
}

/* walk one track, calling back with (tick, status, d1, d2) */
typedef void (*ai_evfn)(void *ud, uint32_t tick, uint8_t st, uint8_t d1, uint8_t d2);

static uint32_t ai_vlq_rd(const uint8_t **pp, const uint8_t *end)
{
    uint32_t v = 0;
    const uint8_t *p = *pp;
    while (p < end && (*p & 0x80u)) { v = (v << 7) | (uint32_t)(*p & 0x7Fu); p++; }
    if (p < end) { v = (v << 7) | (uint32_t)*p; p++; }
    *pp = p;
    return v;
}

static void ai_smf_walk(const uint8_t *p, uint32_t len, ai_evfn fn, void *ud)
{
    const uint8_t *end = p + len;
    uint32_t tick = 0;
    uint8_t status = 0;
    while (p < end) {
        tick += ai_vlq_rd(&p, end);
        if (p >= end) break;
        if (*p & 0x80u) status = *p++;
        if (status == 0xFF) {
            if (p >= end) break;
            uint8_t type = *p++;
            uint32_t l = ai_vlq_rd(&p, end);
            if (type == 0x2F) break;
            p += l;
            continue;
        }
        if (status == 0xF0 || status == 0xF7) {
            uint32_t l = ai_vlq_rd(&p, end);
            p += l;
            continue;
        }
        int hi = status & 0xF0;
        if (hi == 0xC0 || hi == 0xD0) {
            if (p >= end) break;
            if (fn) fn(ud, tick, status, p[0], 0);
            p += 1;
        } else {
            if (p + 1 >= end) break;
            uint8_t d1 = p[0], d2 = p[1];
            p += 2;
            if (fn) fn(ud, tick, status, d1, d2);
        }
    }
}

typedef struct { uint32_t max; } ai_end_ud;
static void ai_end_cb(void *ud, uint32_t tick, uint8_t st, uint8_t d1, uint8_t d2)
{
    (void)st; (void)d1; (void)d2;
    ai_end_ud *e = (ai_end_ud *)ud;
    if (tick > e->max) e->max = tick;
}

/* --------------------------------------------------------------- playback */
static uint32_t ai_tick_of_frame(int f, int hop, int rate)
{
    double ms = (double)f * (double)hop * 1000.0 / (double)rate;
    return (uint32_t)(ms / AI_TICK_MS + 0.5);
}

static int ai_prog_for_centroid(float c)
{
    if (c < 500.0f) return 33;      /* bass family: pulse            */
    if (c < 1200.0f) return 24;     /* tri                           */
    if (c < 2400.0f) return 16;     /* saw                           */
    return 8;                       /* square                        */
}

/* ------------------------------------------------------------------- music */
static int ai_mode_music(const af_t *a, ai_evl_t *l, int drums_on, int force_prog)
{
    const int win = AI_N_MUSIC, hop = AI_HOP_M, nb = win / 2;
    const float bin_hz = (float)a->rate / (float)win;
    int nframes = (a->n - win) / hop + 1;
    if (nframes < 4) { fprintf(stderr, "audio: clip too short for music mode\n"); return -1; }

    float *mag = (float *)malloc((size_t)nb * sizeof(float));
    float *fmax = (float *)malloc((size_t)nframes * sizeof(float));
    float *rms = (float *)malloc((size_t)nframes * sizeof(float));
    float *flux = (float *)malloc((size_t)nframes * sizeof(float));
    float *prev = (float *)calloc((size_t)nb, sizeof(float));
    if (!mag || !fmax || !rms || !flux || !prev) return -1;

    float floor_rms = 0.0f;
    for (int f = 0; f < nframes; f++) {
        ai_spectrum(a->x, a->n, f * hop, win, mag);
        rms[f] = ai_rms(a->x, a->n, f * hop, win);
        float best = 0.0f;
        for (int n = 0; n < AI_NN; n++) {
            float f0 = 440.0f * powf(2.0f, (float)(AI_NOTE_MIN + n - 69) / 12.0f);
            float s = ai_salience(mag, nb, bin_hz, f0);
            if (s > best) best = s;
        }
        fmax[f] = best;
        float fl = 0.0f;
        for (int k = 1; k < nb; k++) { float d = mag[k] - prev[k]; if (d > 0.0f) fl += d; prev[k] = mag[k]; }
        flux[f] = fl;
    }
    /* reference level: 95th percentile of per-frame peak salience */
    float *sorted = (float *)malloc((size_t)nframes * sizeof(float));
    memcpy(sorted, fmax, (size_t)nframes * sizeof(float));
    for (int i = 1; i < nframes; i++) {
        float v = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j] > v) { sorted[j + 1] = sorted[j]; j--; }
        sorted[j + 1] = v;
    }
    float p95 = sorted[(int)((float)nframes * 0.95f)] + 1e-9f;
    free(sorted);
    for (int i = 0; i < nframes; i++) if (rms[i] < 0.004f && floor_rms < rms[i]) floor_rms = rms[i];
    float thr = p95 * 0.16f;

    int act[AI_NN], miss[AI_NN], seen[AI_NN], on_tick[AI_NN], last_frame[AI_NN], vel[AI_NN];
    for (int i = 0; i < AI_NN; i++) { act[i] = 0; miss[i] = 0; seen[i] = 0; on_tick[i] = 0; last_frame[i] = 0; vel[i] = 0; }

    double c_low = 0.0, w_low = 0.0, c_high = 0.0, w_high = 0.0;
    float sal[AI_NN];
    int notes_out = 0;

    for (int f = 0; f < nframes; f++) {
        ai_spectrum(a->x, a->n, f * hop, win, mag);
        float fr = ai_centroid(mag, nb, bin_hz);
        static float work[AI_NMAX / 2];
        memcpy(work, mag, (size_t)nb * sizeof(float));
        for (int n = 0; n < AI_NN; n++) sal[n] = 0.0f;
        /* Iterative harmonic cancellation: take the strongest fundamental, then
         * subtract its harmonic series so the fifth/octave ghost of a note is
         * not picked as a second note. */
        for (int pick = 0; pick < AI_MAXPOLY; pick++) {
            int best = -1;
            float bs = 0.0f;
            for (int n = 0; n < AI_NN; n++) {
                if (sal[n] > 0.0f) continue;
                float f0 = 440.0f * powf(2.0f, (float)(AI_NOTE_MIN + n - 69) / 12.0f);
                float sc = ai_salience(work, nb, bin_hz, f0);
                if (sc > bs) { bs = sc; best = n; }
            }
            if (best < 0 || bs <= thr) break;
            sal[best] = bs;
            float f0 = 440.0f * powf(2.0f, (float)(AI_NOTE_MIN + best - 69) / 12.0f);
            for (int hh = 1; hh <= 8; hh++) {
                int b = (int)(f0 * (float)hh / bin_hz + 0.5f);
                if (b < 1 || b >= nb) break;
                work[b] *= 0.10f;
                if (b + 1 < nb) work[b + 1] *= 0.30f;
                if (b - 1 > 0) work[b - 1] *= 0.30f;
            }
        }
        char hot[AI_NN];
        for (int n = 0; n < AI_NN; n++) hot[n] = (char)(sal[n] > 0.0f);

        int low_here = 0, high_here = 0;
        for (int n = 0; n < AI_NN; n++) {
            if (hot[n]) {
                if (!act[n]) {
                    if (++seen[n] >= 2) {
                        act[n] = 1; miss[n] = 0; last_frame[n] = f;
                        on_tick[n] = (int)ai_tick_of_frame(f > 1 ? f - 1 : 0, hop, a->rate);
                        vel[n] = ai_clampi((int)(6.0f + 120.0f * sqrtf(sal[n] / p95)), 8, 127);
                        notes_out++;
                        int chn = (AI_NOTE_MIN + n < 60) ? 0 : 1;
                        ai_ev(l, (uint32_t)on_tick[n], (uint8_t)(0x90 | chn),
                              (uint8_t)(AI_NOTE_MIN + n), (uint8_t)vel[n], 3);
                        if (AI_NOTE_MIN + n < 60) low_here = 1; else high_here = 1;
                    }
                } else {
                    seen[n] = 2; miss[n] = 0; last_frame[n] = f;
                }
            } else {
                seen[n] = 0;
                if (act[n]) {
                    if (++miss[n] >= 3) {
                        act[n] = 0; miss[n] = 0;
                        int ch = (AI_NOTE_MIN + n < 60) ? 0 : 1;
                        uint32_t tk = ai_tick_of_frame(last_frame[n], hop, a->rate);
                        ai_ev(l, tk, (uint8_t)(0x80 | ch), (uint8_t)(AI_NOTE_MIN + n), 0, 3);
                    }
                }
            }
        }
        if (low_here) { c_low += fr; w_low += 1.0; }
        if (high_here) { c_high += fr; w_high += 1.0; }
    }
    /* close anything still held */
    for (int n = 0; n < AI_NN; n++) {
        if (!act[n]) continue;
        int ch = (AI_NOTE_MIN + n < 60) ? 0 : 1;
        ai_ev(l, ai_tick_of_frame(last_frame[n], hop, a->rate), (uint8_t)(0x80 | ch), (uint8_t)(AI_NOTE_MIN + n), 0, 3);
        act[n] = 0;
    }
    /* re-emit onsets now that tracking settled: recompute is not needed, the
     * events above were appended in time order, so nothing to do. */

    int prog_low = force_prog >= 0 ? force_prog : ai_prog_for_centroid(w_low > 0 ? (float)(c_low / w_low) : 800.0f);
    int prog_high = force_prog >= 0 ? force_prog : ai_prog_for_centroid(w_high > 0 ? (float)(c_high / w_high) : 2000.0f);
    ai_ev(l, 0, 0xC0, (uint8_t)prog_low, 0, 2);
    ai_ev(l, 0, 0xC1, (uint8_t)prog_high, 0, 2);

    /* drums: spectral-flux onsets, classified into kick / snare / hat */
    int drum_hits = 0;
    if (drums_on) {
        float mean = 0.0f;
        for (int f = 0; f < nframes; f++) mean += flux[f];
        mean /= (float)nframes;
        uint32_t last_tick = 0;
        for (int f = 1; f < nframes - 1; f++) {
            if (flux[f] < mean * 1.8f || flux[f] < flux[f - 1] || flux[f] < flux[f + 1]) continue;
            uint32_t tk = ai_tick_of_frame(f, hop, a->rate);
            if (tk - last_tick < 8u) continue;               /* >= 40 ms apart */
            last_tick = tk;
            ai_spectrum(a->x, a->n, f * hop, win, mag);
            float lo = ai_band_energy(mag, nb, bin_hz, 30, 200);
            float mid = ai_band_energy(mag, nb, bin_hz, 200, 2500);
            float hi = ai_band_energy(mag, nb, bin_hz, 4000, 9000);
            int note = (lo > mid && lo > hi) ? AI_DRUM_KICK : (hi > mid ? AI_DRUM_HAT : AI_DRUM_SNARE);
            float e = lo + mid + hi;
            int v = ai_clampi((int)(40.0f + 80.0f * sqrtf(e / (e + p95 * 4.0f))), 30, 127);
            ai_ev(l, tk, 0x99, (uint8_t)note, (uint8_t)v, 3);
            ai_ev(l, tk + 6u, 0x89, (uint8_t)note, 0, 3);
            drum_hits++;
        }
    }

    printf("music: %d frames, %d notes, programs %d/%d, %d drums, p95 %.4f\n",
           nframes, notes_out, prog_low, prog_high, drum_hits, (double)p95);
    free(mag); free(fmax); free(rms); free(flux); free(prev);
    return 0;
}

/* ------------------------------------------------------------------ speech */
static float ai_percentile(float *v, int n, float pct)
{
    if (n <= 0) return 0.0f;
    float *t = (float *)malloc((size_t)n * sizeof(float));
    if (!t) return 0.0f;
    memcpy(t, v, (size_t)n * sizeof(float));
    for (int i = 1; i < n; i++) {
        float x = t[i];
        int j = i - 1;
        while (j >= 0 && t[j] > x) { t[j + 1] = t[j]; j--; }
        t[j + 1] = x;
    }
    int idx = (int)((float)(n - 1) * pct);
    float r = t[ai_clampi(idx, 0, n - 1)];
    free(t);
    return r;
}

static int ai_drum_for_centroid(float c)
{
    if (c < 900.0f) return AI_DRUM_KICK;
    if (c < 2600.0f) return AI_DRUM_SNARE;
    if (c < 5200.0f) return AI_DRUM_HAT;
    return AI_DRUM_OPEN;
}

static int ai_mode_speech(const af_t *a, ai_evl_t *l, int prog, int drums_on)
{
    const int win = AI_N_SPEECH, hop = AI_HOP_S, nb = win / 2;
    const float bin_hz = (float)a->rate / (float)win;
    int nframes = (a->n - win) / hop + 1;
    if (nframes < 8) { fprintf(stderr, "audio: clip too short for speech mode\n"); return -1; }

    float *mag = (float *)malloc((size_t)nb * sizeof(float));
    float *rms = (float *)malloc((size_t)nframes * sizeof(float));
    float *f0 = (float *)malloc((size_t)nframes * sizeof(float));
    float *cen = (float *)malloc((size_t)nframes * sizeof(float));
    char *voiced = (char *)calloc((size_t)nframes, 1);
    if (!mag || !rms || !f0 || !cen || !voiced) return -1;

    for (int f = 0; f < nframes; f++) {
        ai_spectrum(a->x, a->n, f * hop, win, mag);
        ai_pitch_t p = ai_yin(a->x, a->n, f * hop, win, a->rate, mag, nb, bin_hz);
        rms[f] = p.rms; f0[f] = p.f0; cen[f] = p.centroid;
        voiced[f] = (char)(p.aper < 0.25f && p.f0 > 55.0f && p.f0 < 420.0f);
    }
    float floor_rms = ai_percentile(rms, nframes, 0.10f);
    float gate = floor_rms * 2.5f;
    if (gate < 0.004f) gate = 0.004f;
    for (int f = 0; f < nframes; f++) if (rms[f] < gate) voiced[f] = 0;

    float *vr = (float *)malloc((size_t)nframes * sizeof(float));
    int nv = 0;
    for (int f = 0; f < nframes; f++) if (voiced[f]) vr[nv++] = rms[f];
    float ref = nv ? ai_percentile(vr, nv, 0.90f) : 0.05f;
    free(vr);
    if (ref < 0.004f) ref = 0.004f;

    int cur = -1, start_f = 0, last_v = 0;
    double vsum = 0.0; int vcount = 0;
    int notes = 0, cons = 0;
    int prev_cons = 0;

    for (int f = 0; f < nframes; f++) {
        int note = -1;
        if (voiced[f]) {
            double semi = 69.0 + 12.0 * log2((double)f0[f] / 440.0);
            note = (int)floor(semi + 0.5);
            if (note < 24) note = 24;
            if (note > 100) note = 100;
        }
        if (note >= 0) {
            prev_cons = 0;
            if (cur < 0) {
                cur = note; start_f = f; last_v = f; vsum = rms[f]; vcount = 1;
            } else if (note == cur) {
                last_v = f; vsum += rms[f]; vcount++;
            } else if (f - last_v <= 1) {
                /* one-frame blip: keep the running note */
                last_v = f; vsum += rms[f]; vcount++;
            } else {
                /* pitch change: close the old note, open the new */
                if (last_v - start_f >= 2) {
                    int vel = ai_clampi((int)(18.0f + 105.0f * sqrtf((float)(vsum / vcount) / ref)), 16, 127);
                    uint32_t t0 = ai_tick_of_frame(start_f, hop, a->rate);
                    uint32_t t1 = ai_tick_of_frame(last_v, hop, a->rate);
                    ai_ev(l, t0, 0x90, (uint8_t)cur, (uint8_t)vel, 3);
                    ai_ev(l, t1, 0x80, (uint8_t)cur, 0, 3);
                    notes++;
                }
                cur = note; start_f = f; last_v = f; vsum = rms[f]; vcount = 1;
            }
        } else {
            if (cur >= 0 && f - last_v > 2) {
                if (last_v - start_f >= 2) {
                    int vel = ai_clampi((int)(18.0f + 105.0f * sqrtf((float)(vsum / vcount) / ref)), 16, 127);
                    uint32_t t0 = ai_tick_of_frame(start_f, hop, a->rate);
                    uint32_t t1 = ai_tick_of_frame(last_v, hop, a->rate);
                    ai_ev(l, t0, 0x90, (uint8_t)cur, (uint8_t)vel, 3);
                    ai_ev(l, t1, 0x80, (uint8_t)cur, 0, 3);
                    notes++;
                }
                cur = -1;
            }
            /* consonants: an aperiodic burst that is neither silence nor a vowel */
            if (drums_on && rms[f] > ref * 0.35f && rms[f] < ref * 3.0f && !prev_cons) {
                if (f > 2 && f + 2 < nframes && !voiced[f - 2] && !voiced[f + 2]) {
                    int d = ai_drum_for_centroid(cen[f]);
                    uint32_t tk = ai_tick_of_frame(f, hop, a->rate);
                    int vel = ai_clampi((int)(30.0f + 70.0f * sqrtf(rms[f] / ref)), 24, 110);
                    ai_ev(l, tk, 0x99, (uint8_t)d, (uint8_t)vel, 3);
                    ai_ev(l, tk + 3u, 0x89, (uint8_t)d, 0, 3);
                    cons++;
                    prev_cons = 1;
                }
            }
        }
    }
    if (cur >= 0 && last_v - start_f >= 2) {
        int vel = ai_clampi((int)(18.0f + 105.0f * sqrtf((float)(vsum / vcount) / ref)), 16, 127);
        ai_ev(l, ai_tick_of_frame(start_f, hop, a->rate), 0x90, (uint8_t)cur, (uint8_t)vel, 3);
        ai_ev(l, ai_tick_of_frame(last_v, hop, a->rate), 0x80, (uint8_t)cur, 0, 3);
        notes++;
    }
    ai_ev(l, 0, 0xC0, (uint8_t)ai_clampi(prog, 0, 127), 0, 2);

    int vf = 0;
    for (int f = 0; f < nframes; f++) vf += voiced[f] ? 1 : 0;
    printf("speech: %d frames (%.2f s), %d voiced frames (%.0f%%), %d notes, %d consonants, ref rms %.4f\n",
           nframes, (double)nframes * hop / (double)a->rate, vf,
           100.0 * (double)vf / (double)nframes, notes, cons, (double)ref);
    free(mag); free(rms); free(f0); free(cen); free(voiced);
    return 0;
}

/* ------------------------------------------------------------------ render */
static int ai_cmd_render(int argc, char **argv)
{
    const char *in = NULL, *out = "out.wav";
    int rate = 32000;
    double seconds = -1.0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--rate") && i + 1 < argc) rate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (argv[i][0] != '-') in = argv[i];
    }
    if (!in) { fprintf(stderr, "audio render: need an .mid\n"); return 2; }
    if (rate < MS_AUDIO_HZ_MIN || rate > MS_AUDIO_HZ_MAX) rate = 32000;

    FILE *f = fopen(in, "rb");
    if (!f) { fprintf(stderr, "audio render: cannot open %s\n", in); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *blob = (uint8_t *)malloc((size_t)sz);
    if (!blob) { fclose(f); return 1; }
    if (fread(blob, 1, (size_t)sz, f) != (size_t)sz) { free(blob); fclose(f); return 1; }
    fclose(f);

    ms_song_t song;
    if (ms_song_load(&song, blob, (size_t)sz) != 0) {
        fprintf(stderr, "audio render: %s is not a Standard MIDI File midicore can read\n", in);
        free(blob);
        return 1;
    }
    ai_end_ud eu;
    eu.max = 0;
    for (uint32_t t = 0; t < song.tracks && t < MS_SONG_TRACKS; t++)
        ai_smf_walk(song.track[t], song.track_len[t], ai_end_cb, &eu);

    double dur = seconds > 0.0 ? seconds : (double)eu.max * AI_TICK_MS / 1000.0 + 2.5;
    int frames = (int)(dur * (double)rate) + 1;

    static ms_synth_t synth;
    ms_synth_init(&synth, rate, 16, 12, 4);
    ms_player_start(&synth, &song, 0);

    int16_t *pcm = (int16_t *)malloc((size_t)frames * 2u * sizeof(int16_t));
    if (!pcm) { free(blob); return 1; }
    int block = 512, done = 0;
    while (done < frames) {
        int n = frames - done < block ? frames - done : block;
        ms_synth_render(&synth, pcm + (size_t)done * 2u, n);
        done += n;
    }
    int rc = ai_write_wav(out, pcm, frames, rate, 2);
    printf("render: %s -> %s (%.2f s, %d Hz, stereo)\n", in, out, dur, rate);
    free(pcm); free(blob);
    return rc;
}

/* ----------------------------------------------------------------- compare */
typedef struct { int n; double *env; float *f0; unsigned char *v; } ai_track_t;

static void ai_track_build(const af_t *a, ai_track_t *t)
{
    const int win = 1024, hop = 512, nb = win / 2;
    const float bin_hz = (float)a->rate / (float)win;
    float *mag = (float *)malloc((size_t)nb * sizeof(float));
    memset(t, 0, sizeof(*t));
    t->n = a->n > win ? (a->n - win) / hop + 1 : 0;
    if (t->n <= 0) { free(mag); return; }
    t->env = (double *)calloc((size_t)t->n, sizeof(double));
    t->f0 = (float *)calloc((size_t)t->n, sizeof(float));
    t->v = (unsigned char *)calloc((size_t)t->n, 1);
    for (int f = 0; f < t->n; f++) {
        ai_spectrum(a->x, a->n, f * hop, win, mag);
        float r = ai_rms(a->x, a->n, f * hop, win);
        t->env[f] = 20.0 * log10((double)r + 1e-7);
        ai_pitch_t p = ai_yin(a->x, a->n, f * hop, win, a->rate, mag, nb, bin_hz);
        t->f0[f] = p.f0;
        t->v[f] = (unsigned char)(p.aper < 0.25f && p.f0 > 55.0f && p.f0 < 500.0f && r > 0.004f);
        if (!t->v[f]) t->f0[f] = 0.0f;
    }
    free(mag);
}

static double ai_pearson(const double *a, const double *b, int n)
{
    if (n < 2) return 0.0;
    double ma = 0.0, mb = 0.0;
    for (int i = 0; i < n; i++) { ma += a[i]; mb += b[i]; }
    ma /= n; mb /= n;
    double num = 0.0, da = 0.0, db = 0.0;
    for (int i = 0; i < n; i++) {
        double x = a[i] - ma, y = b[i] - mb;
        num += x * y; da += x * x; db += y * y;
    }
    if (da <= 0.0 || db <= 0.0) return 0.0;
    return num / sqrt(da * db);
}

static int ai_cmd_compare(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "audio compare <a.wav> <b.wav>\n"); return 2; }
    af_t a, b;
    if (ai_load_wav(argv[2], AI_RATE, &a) != 0) return 1;
    if (ai_load_wav(argv[3], AI_RATE, &b) != 0) { free(a.x); return 1; }
    ai_track_t ta, tb;
    ai_track_build(&a, &ta);
    ai_track_build(&b, &tb);
    int n = ta.n < tb.n ? ta.n : tb.n;
    if (n < 4) { fprintf(stderr, "audio compare: clips too short\n"); free(a.x); free(b.x); return 1; }

    double *ea = (double *)malloc((size_t)n * sizeof(double));
    double *eb = (double *)malloc((size_t)n * sizeof(double));
    for (int i = 0; i < n; i++) { ea[i] = ta.env[i]; eb[i] = tb.env[i]; }
    double env_corr = ai_pearson(ea, eb, n);

    double *fa = (double *)malloc((size_t)n * sizeof(double));
    double *fb = (double *)malloc((size_t)n * sizeof(double));
    int fn = 0, tp = 0, fp = 0, fneg = 0;
    double mae = 0.0;
    for (int i = 0; i < n; i++) {
        if (ta.v[i] && tb.v[i]) {
            double x = 12.0 * log2((double)ta.f0[i] / 440.0);
            double y = 12.0 * log2((double)tb.f0[i] / 440.0);
            fa[fn] = x; fb[fn] = y; fn++;
            mae += fabs(x - y);
            tp++;
        } else if (!ta.v[i] && tb.v[i]) fp++;
        else if (ta.v[i] && !tb.v[i]) fneg++;
    }
    double f0_corr = fn > 1 ? ai_pearson(fa, fb, fn) : 0.0;
    double f0_mae = fn ? mae / (double)fn : 0.0;
    double prec = (tp + fp) ? (double)tp / (double)(tp + fp) : 0.0;
    double rec = (tp + fneg) ? (double)tp / (double)(tp + fneg) : 0.0;
    double f1 = (prec + rec) > 0.0 ? 2.0 * prec * rec / (prec + rec) : 0.0;

    printf("frames %d\n", n);
    printf("duration_a %.3f\n", (double)a.n / (double)AI_RATE);
    printf("duration_b %.3f\n", (double)b.n / (double)AI_RATE);
    printf("env_corr %.4f\n", env_corr);
    printf("f0_corr %.4f\n", f0_corr);
    printf("f0_mae_semitone %.4f\n", f0_mae);
    printf("vad_f1 %.4f\n", f1);
    printf("voiced_a %d\nvoiced_b %d\n", tp + fneg, tp + fp);
    free(ea); free(eb); free(fa); free(fb);
    free(ta.env); free(ta.f0); free(ta.v);
    free(tb.env); free(tb.f0); free(tb.v);
    free(a.x); free(b.x);
    return 0;
}

/* --------------------------------------------------------------- dump/info */
typedef struct { int count; } ai_dump_ud;
static void ai_dump_cb(void *ud, uint32_t tick, uint8_t st, uint8_t d1, uint8_t d2)
{
    ai_dump_ud *u = (ai_dump_ud *)ud;
    int hi = st & 0xF0, ch = st & 0x0F;
    if (hi == 0x90 && d2) printf("on %u %u %u %u\n", (unsigned)ch, (unsigned)d1, (unsigned)d2, (unsigned)tick);
    else if (hi == 0x80 || (hi == 0x90 && !d2)) printf("off %u %u %u\n", (unsigned)ch, (unsigned)d1, (unsigned)tick);
    else if (hi == 0xC0) printf("prog %u %u %u\n", (unsigned)ch, (unsigned)d1, (unsigned)tick);
    u->count++;
}

static int ai_cmd_dump(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "audio dump <in.mid>\n"); return 2; }
    FILE *f = fopen(argv[2], "rb");
    if (!f) { fprintf(stderr, "audio dump: cannot open %s\n", argv[2]); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *blob = (uint8_t *)malloc((size_t)sz);
    if (!blob || fread(blob, 1, (size_t)sz, f) != (size_t)sz) { if (blob) free(blob); fclose(f); return 1; }
    fclose(f);
    ms_song_t song;
    if (ms_song_load(&song, blob, (size_t)sz) != 0) { free(blob); fprintf(stderr, "audio dump: not an SMF\n"); return 1; }
    ai_dump_ud u;
    u.count = 0;
    for (uint32_t t = 0; t < song.tracks && t < MS_SONG_TRACKS; t++)
        ai_smf_walk(song.track[t], song.track_len[t], ai_dump_cb, &u);
    fprintf(stderr, "audio dump: %d events\n", u.count);
    free(blob);
    return 0;
}

static int ai_cmd_info(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "audio info <in.wav>\n"); return 2; }
    af_t a;
    if (ai_load_wav(argv[2], AI_RATE, &a) != 0) return 1;
    const int win = 1024, hop = 512, nb = win / 2;
    const float bin_hz = (float)a.rate / (float)win;
    float *mag = (float *)malloc((size_t)nb * sizeof(float));
    int nframes = a.n > win ? (a.n - win) / hop + 1 : 0;
    float *f0s = (float *)malloc((size_t)(nframes > 0 ? nframes : 1) * sizeof(float));
    int nf = 0, peak_n = 0;
    float peak = 0.0f;
    for (int i = 0; i < a.n; i++) { float v = fabsf(a.x[i]); if (v > peak) { peak = v; peak_n = i; } }
    for (int f = 0; f < nframes; f++) {
        ai_spectrum(a.x, a.n, f * hop, win, mag);
        ai_pitch_t p = ai_yin(a.x, a.n, f * hop, win, a.rate, mag, nb, bin_hz);
        if (p.aper < 0.25f && p.f0 > 55.0f && p.f0 < 500.0f && p.rms > 0.004f) f0s[nf++] = p.f0;
    }
    float med = nf ? ai_percentile(f0s, nf, 0.5f) : 0.0f;
    printf("wav %s\n", argv[2]);
    printf("rate %d\n", a.rate);
    printf("duration %.3f\n", (double)a.n / (double)a.rate);
    printf("peak_db %.2f\n", 20.0 * log10((double)peak + 1e-9));
    printf("peak_at %.3f\n", (double)peak_n / (double)a.rate);
    printf("median_f0 %.1f\n", (double)med);
    printf("voiced_fraction %.3f\n", nframes ? (double)nf / (double)nframes : 0.0);
    free(mag); free(f0s); free(a.x);
    return 0;
}

/* --------------------------------------------------------------------- cli */
static int ai_usage(void)
{
    printf("mosaic audio <command>\n"
           "  ingest <in.wav> -o out.mid [--mode music|speech] [--program N]\n"
           "         [--no-drums] [--drums]\n"
           "  render <in.mid> -o out.wav [--seconds S] [--rate HZ]\n"
           "  compare <a.wav> <b.wav>\n"
           "  dump <in.mid>\n"
           "  info <in.wav>\n");
    return 1;
}

static int ai_cmd_ingest(int argc, char **argv, int mode_speech)
{
    const char *in = NULL, *out = "out.mid";
    int prog = -1, drums = 1;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) mode_speech = !strcmp(argv[++i], "speech");
        else if (!strcmp(argv[i], "--program") && i + 1 < argc) prog = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-drums")) drums = 0;
        else if (!strcmp(argv[i], "--drums")) drums = 1;
        else if (argv[i][0] != '-') in = argv[i];
    }
    if (!in) { fprintf(stderr, "audio ingest: need an input WAV\n"); return 2; }
    ai_init();
    af_t a;
    if (ai_load_wav(in, AI_RATE, &a) != 0) return 1;
    ai_evl_t l;
    memset(&l, 0, sizeof l);
    int rc = mode_speech ? ai_mode_speech(&a, &l, prog < 0 ? 33 : prog, drums)
                         : ai_mode_music(&a, &l, drums, prog);
    if (rc == 0) {
        char title[128];
        snprintf(title, sizeof title, "Mosaic audio ingest: %s (%s)", in, mode_speech ? "speech" : "music");
        uint32_t end = 0;
        rc = ai_smf_write(out, &l, title, &end);
        if (rc == 0)
            printf("wrote %s: %d events, %u ticks (%.2f s at 120 BPM)\n",
                   out, l.n, (unsigned)end, (double)end * AI_TICK_MS / 1000.0);
    }
    free(l.e);
    free(a.x);
    return rc;
}

int mosaic_audio_main(int argc, char **argv)
{
    /* main.c dispatches the whole argv, so drop the "audio" verb: after this
     * argv[1] is the subcommand and argv[2..] its arguments. */
    argc--; argv++;
    if (argc < 2) return ai_usage();
    if (!strcmp(argv[1], "ingest")) return ai_cmd_ingest(argc, argv, 0);
    if (!strcmp(argv[1], "music")) return ai_cmd_ingest(argc, argv, 0);
    if (!strcmp(argv[1], "speech")) return ai_cmd_ingest(argc, argv, 1);
    if (!strcmp(argv[1], "render")) return ai_cmd_render(argc, argv);
    if (!strcmp(argv[1], "compare")) return ai_cmd_compare(argc, argv);
    if (!strcmp(argv[1], "dump")) return ai_cmd_dump(argc, argv);
    if (!strcmp(argv[1], "info")) return ai_cmd_info(argc, argv);
    return ai_usage();
}
