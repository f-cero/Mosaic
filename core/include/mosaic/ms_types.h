/* SPDX-License-Identifier: Apache-2.0 */
/* Mosaic — ms_types.h: RGB565, 16.16 fixed point, small helpers.
 * No floating point anywhere below the studio/port line. */
#ifndef MOSAIC_MS_TYPES_H
#define MOSAIC_MS_TYPES_H
#include <stdint.h>
#include <stddef.h>

/* 16.16 fixed point. */
typedef int32_t ms_fix;
#define MS_FIX_ONE  65536
#define MS_FIX(i)   ((ms_fix)((i) * MS_FIX_ONE))
ms_fix ms_fix_mul(ms_fix a, ms_fix b);
ms_fix ms_fix_div(ms_fix a, ms_fix b);
ms_fix ms_fix_from_int(int32_t v);
int32_t ms_fix_to_int(ms_fix v);
ms_fix ms_sin(ms_fix turns);   /* turns: 1.0 == 360 degrees */
ms_fix ms_cos(ms_fix turns);

/* RGB565 */
static inline uint16_t ms_rgb565(unsigned r, unsigned g, unsigned b) {
    return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}
uint16_t ms_rgb565_bright(uint16_t c, unsigned scale /*0..16*/);
uint16_t ms_rgb565_mix(uint16_t a, uint16_t b, unsigned t /*0..16*/);
unsigned ms_rgb565_r(uint16_t c);
unsigned ms_rgb565_g(uint16_t c);
unsigned ms_rgb565_b(uint16_t c);

/* SipHash-2-4 as a keyed integrity tag for save records. It is a MAC, not a
 * cipher: the key travels in the cart, so it stops accidents, cross-project
 * collisions and casual editing — not a determined player. */
typedef struct { uint64_t v0, v1, v2, v3; uint8_t buf[8]; uint8_t n; uint64_t total; } ms_sip_t;
void ms_sip_init(ms_sip_t *c, const uint8_t key[16]);
void ms_sip_update(ms_sip_t *c, const void *data, size_t n);
uint64_t ms_sip_final(ms_sip_t *c);
uint64_t ms_mac64(const uint8_t key[16], const void *data, size_t n);
/* Developer key string -> 16-byte MAC key. Empty/NULL gives the zero key. */
void ms_mac_key(const char *dev_key, uint8_t key[16]);
/* Developer / save identifiers: 32-bit, stable across builds, computed from
 * the strings a project declares (`dev=` / `save_id=`). 0 means "unset". */
uint32_t ms_id32(const char *name);

uint32_t ms_crc32(const void *data, size_t n);
uint32_t ms_hash32(const char *s);
uint16_t ms_crc16(const void *data, size_t n);

static inline uint16_t ms_rd16(const uint8_t *p){ return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t ms_rd32(const uint8_t *p){ return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static inline void ms_wr16(uint8_t *p, uint16_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static inline void ms_wr32(uint8_t *p, uint32_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }

#endif
