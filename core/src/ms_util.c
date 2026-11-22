/* SPDX-License-Identifier: Apache-2.0 */
/* Mosaic — ms_util.c: fixed point, RGB565, sine, CRC. */
#include "mosaic/ms_types.h"
#include <string.h>

ms_fix ms_fix_mul(ms_fix a, ms_fix b)
{
    int64_t p = (int64_t)a * (int64_t)b;
    return (ms_fix)(p >> 16);
}
ms_fix ms_fix_div(ms_fix a, ms_fix b)
{
    if (b == 0) return a >= 0 ? 0x7FFFFFFF : -0x7FFFFFFF;
    int64_t n = ((int64_t)a) << 16;
    return (ms_fix)(n / b);
}
ms_fix ms_fix_from_int(int32_t v) { return (ms_fix)(v * MS_FIX_ONE); }
int32_t ms_fix_to_int(ms_fix v) { return v >> 16; }

/* 65-entry quarter-wave table, int16 Q15. */
static const int16_t s_qsin[65] = {
    0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179, 7962, 8739,
    9512, 10278, 11039, 11793, 12539, 13279, 14010, 14732, 15446, 16151,
    16846, 17530, 18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594,
    23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790, 27245, 27683,
    28105, 28510, 28898, 29268, 29621, 29956, 30273, 30571, 30852, 31113,
    31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521, 32609, 32678,
    32728, 32757, 32767
};

static int qsin_interp(int idx, int frac)
{
    int a = s_qsin[idx], b = s_qsin[idx + 1];
    return a + (int)(((int64_t)(b - a) * (frac * 64 % 65536)) / 65536);
}

ms_fix ms_sin(ms_fix turns)
{
    int64_t q = (int64_t)turns * 4;      /* one turn = 4 quarters of 65536 */
    int quad = (int)((q >> 16) & 3);
    int frac = (int)(q & 0xFFFF);
    int idx = frac * 64 / 65536;
    int v;
    switch (quad) {
    case 0: v = qsin_interp(idx, frac); break;
    case 1: v = qsin_interp(63 - idx, 65535 - frac); break;
    case 2: v = -qsin_interp(idx, frac); break;
    default: v = -qsin_interp(63 - idx, 65535 - frac); break;
    }
    return (ms_fix)v;
}
ms_fix ms_cos(ms_fix turns) { return ms_sin(turns + MS_FIX_ONE / 4); }

unsigned ms_rgb565_r(uint16_t c) { return (unsigned)((c >> 11) & 0x1F) * 255u / 31u; }
unsigned ms_rgb565_g(uint16_t c) { return (unsigned)((c >> 5) & 0x3F) * 255u / 63u; }
unsigned ms_rgb565_b(uint16_t c) { return (unsigned)(c & 0x1F) * 255u / 31u; }

uint16_t ms_rgb565_bright(uint16_t c, unsigned scale)
{
    if (scale >= 16) return c;
    unsigned r = ((c >> 11) & 0x1F) * scale / 16u;
    unsigned g = ((c >> 5) & 0x3F) * scale / 16u;
    unsigned b = (c & 0x1F) * scale / 16u;
    return (uint16_t)((r << 11) | (g << 5) | b);
}
uint16_t ms_rgb565_mix(uint16_t a, uint16_t b, unsigned t)
{
    if (t >= 16) return b;
    unsigned ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    unsigned br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    unsigned r = (ar * (16 - t) + br * t) / 16;
    unsigned g = (ag * (16 - t) + bg * t) / 16;
    unsigned bl = (ab * (16 - t) + bb * t) / 16;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

uint32_t ms_crc32(const void *data, size_t n)
{
    static uint32_t tab[256];
    static int init;
    const uint8_t *p = (const uint8_t *)data;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            tab[i] = c;
        }
        init = 1;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
uint16_t ms_crc16(const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        c ^= (uint16_t)(p[i] << 8);
        for (int k = 0; k < 8; k++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
    }
    return c;
}
uint32_t ms_hash32(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}
