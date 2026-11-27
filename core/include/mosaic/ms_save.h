/* SPDX-License-Identifier: Apache-2.0 */
/* Mosaic — ms_save.h: the save image. One byte image is the whole file
 * format: a flash region on hardware, a .msav file on the host and an AI
 * Drive object are the same bytes, and this API reads and writes that image
 * in place through a caller-owned buffer. No heap, no I/O, no floats.
 *
 * image, all little-endian:
 *   MOS1 | u16 ver=1 | u16 bytes | u8 slots | u8 reserved | u32 cart_crc
 *   copy[slot*2 + which] for every slot and both copies
 *
 * copy record (fixed stride, so a torn write is always locatable):
 *   u32 magic 'SLT1' | u32 seq | u16 len | u16 crc16(body) | u8 name[12]
 *   | u16 hcrc (crc16 of the first 24 bytes) | u16 reserved=0 | body[len]
 *
 * A write goes to the copy with the older seq and commits by writing hcrc
 * last, so a power cut at any point leaves the other copy authoritative.
 * len == 0 with an empty name is a tombstone (an erased slot). */
#ifndef MOSAIC_MS_SAVE_H
#define MOSAIC_MS_SAVE_H
#include "mosaic/ms_limits.h"
#include "mosaic/ms_types.h"
#include "mosaic/ms_cart.h"

#define MS_SAVE_MAGIC      0x31534F4Du   /* "MOS1" little-endian */
#define MS_SAVE_COPY_MAGIC 0x31544C53u   /* "SLT1" little-endian */
#define MS_SAVE_VERSION    1u
#define MS_SAVE_HDR        14u   /* 4 magic + 2 ver + 2 bytes + 1 slots + 1 res + 4 crc */
#define MS_SAVE_COPY_HDR   28u
#define MS_SAVE_BYTES      (8u * 1024u)
#define MS_SAVE_BYTES_MIN  512u
#define MS_SAVE_BYTES_MAX  65532u
#define MS_SAVE_SLOTS      4u
#define MS_SAVE_SLOTS_MAX  8u
#define MS_SAVE_NAME       12u

/* Status bits, reported out of open/read/verify. */
#define MS_SAVE_F_INIT      0x01u   /* image was blank and got formatted    */
#define MS_SAVE_F_FELLBACK  0x02u   /* an older copy was used               */
#define MS_SAVE_F_TORN      0x04u   /* a newer copy failed its checks       */
#define MS_SAVE_F_READONLY  0x08u   /* cart declares no save region         */

enum {
    MS_SAVE_OK = 0,
    MS_SAVE_E_PARAM = -1, MS_SAVE_E_MAGIC = -2, MS_SAVE_E_SIZE = -3,
    MS_SAVE_E_CRC = -4,   MS_SAVE_E_CART = -5,  MS_SAVE_E_EMPTY = -6,
    MS_SAVE_E_SLOT = -7,  MS_SAVE_E_BODY = -8,  MS_SAVE_E_NOSPACE = -9
};
const char *ms_save_err_str(int e);

typedef struct {
    uint8_t *region;      /* caller-owned image, `bytes` long             */
    size_t   bytes;
    uint32_t cart_crc;    /* bound to exactly one cart build              */
    unsigned slots, stride, max_body;
} ms_save_t;

typedef struct {
    unsigned slot, valid;
    uint32_t seq;
    uint16_t len;
    char     name[MS_SAVE_NAME + 1];
} ms_save_ent_t;

/* Lay a fresh image over `region` (which is zeroed) bound to `cart_crc`.
 * `slots` 1..8; `bytes` MS_SAVE_BYTES_MIN..MAX and at least 2*slots copies. */
int ms_save_format(ms_save_t *s, void *region, size_t bytes, uint32_t cart_crc,
                   unsigned slots, unsigned *flags);
/* Open an existing image. A blank region is formatted (MS_SAVE_F_INIT);
 * a different cart_crc is MS_SAVE_E_CART, never a silent empty state. */
int ms_save_open(ms_save_t *s, void *region, size_t bytes, uint32_t cart_crc,
                 unsigned *flags);

/* Read the newest valid copy of `slot`. `len` receives the body length even
 * when `cap` is too small (the call then returns MS_SAVE_E_NOSPACE after
 * copying `cap` bytes, so a caller can size its buffer and retry). */
int ms_save_read(const ms_save_t *s, unsigned slot, char *name, size_t namecap,
                 void *buf, size_t cap, size_t *len, unsigned *flags);
int ms_save_write(ms_save_t *s, unsigned slot, const char *name,
                  const void *data, size_t len);
int ms_save_erase(ms_save_t *s, unsigned slot);
int ms_save_list(const ms_save_t *s, ms_save_ent_t *out, unsigned cap, unsigned *n);
/* Full re-check of every copy; returns the number of slots that hold data. */
int ms_save_verify(const ms_save_t *s, unsigned *flags);

/* Region size / slot count a cart asks for. Returns 1 if the cart carries a
 * kind 13 MS_A_SAVE asset, 0 if it does not (such a cart is read-only). */
int ms_save_cfg_from_cart(const ms_cart_t *c, unsigned *bytes, unsigned *slots);

/* .msv transfer bundle: header + integrity trailer, not a signature. */
#define MS_SAVE_BUNDLE_MAGIC 0x3156534Du   /* "MSV1" little-endian */
int ms_save_bundle_export(const ms_save_t *s, uint32_t device_id,
                          void *out, size_t cap, size_t *len);
int ms_save_bundle_import(ms_save_t *s, const void *blob, size_t bytes,
                          unsigned *flags);

#endif
