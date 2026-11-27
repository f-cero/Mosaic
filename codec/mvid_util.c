/* SPDX-License-Identifier: Apache-2.0 — MVID shared helpers. */
#include "mvid.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint16_t mvid_rd16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
uint32_t mvid_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
void mvid_wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void mvid_wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

unsigned mvid_next_pow2(unsigned v)
{
    unsigned r = 1;
    while (r < v && r < 0x8000u) r <<= 1;
    return r;
}

uint32_t mvid_crc32(const void *data, size_t bytes)
{
    static uint32_t table[256];
    static int init;
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = 0xFFFFFFFFu;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t x = i;
            for (int k = 0; k < 8; k++) x = (x & 1) ? (0xEDB88320u ^ (x >> 1)) : (x >> 1);
            table[i] = x;
        }
        init = 1;
    }
    for (size_t i = 0; i < bytes; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

uint16_t mvid_scale565(uint16_t c, unsigned brightness)
{
    if (brightness >= 16) return c;
    const unsigned r = (c >> 11) & 31u, g = (c >> 5) & 63u, b = c & 31u;
    return (uint16_t)(((r * brightness / 16u) << 11) |
                      ((g * brightness / 16u) << 5) |
                      (b * brightness / 16u));
}

const char *mvid_err_str(int e)
{
    switch (e) {
    case MVID_E_OK: return "ok";
    case MVID_E_PARAM: return "bad parameter";
    case MVID_E_MAGIC: return "not an MVID file";
    case MVID_E_VERSION: return "unsupported MVID version";
    case MVID_E_SIZE: return "truncated or overlapping data";
    case MVID_E_FRAME: return "bad frame record";
    case MVID_E_CRC: return "checksum mismatch";
    case MVID_E_MEM: return "out of memory";
    case MVID_E_IO: return "i/o error";
    case MVID_E_FULL: return "too many frames";
    default: return "unknown error";
    }
}

uint8_t *mvid_read_file(const char *path, size_t *bytes)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    uint8_t *b = (uint8_t *)malloc((size_t)n ? (size_t)n : 1u);
    if (!b) { fclose(f); return NULL; }
    if (n && fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f);
    if (bytes) *bytes = (size_t)n;
    return b;
}
