/* SPDX-License-Identifier: Apache-2.0 */
/* Mosaic — ms_cart.h: the cart16 v2 cartridge format (shared with the
 * ESP-16 reference platform) plus Mosaic's extension kinds.
 *
 * cart16 v2 layout, all little-endian:
 *   header 20 B : u32 magic 'RT16' | u16 version = 2 | u16 flags
 *                 | u32 count | u32 toc_off (= 20) | u32 crc32(TOC bytes)
 *   TOC count*48: char name[32] | u32 kind | u32 payload_offset
 *                 | u32 payload_size | u32 crc32(payload)
 *   payloads follow the TOC, in TOC order.
 *
 * Kinds 1..7 are canonical. Kinds 8..10 are Mosaic extensions and are
 * only accepted when flags bit 0 (MS_CART_F_EXT) is set — a strict
 * cart16 v2 reader (the reference one) rejects them by design.
 */
#ifndef MOSAIC_MS_CART_H
#define MOSAIC_MS_CART_H
#include "mosaic/ms_limits.h"
#include "mosaic/ms_types.h"

typedef enum {
    MS_A_BG_TILES = 1,   /* u8 bpp | u16 tile_count | tiles             */
    MS_A_BG_MAP   = 2,   /* u16 entry_count (power of two) | entries    */
    MS_A_PALETTE  = 3,   /* u16 entry_count | u16 RGB565 entries        */
    MS_A_SPRITES  = 4,   /* SPR2 sheet (SPR1 readable)                  */
    MS_A_AFFINE   = 5,   /* raw, self-describing                        */
    MS_A_BANK     = 6,   /* .sbn instrument bank                        */
    MS_A_MIDI     = 7,   /* Standard MIDI File                          */
    /* --- Mosaic extensions (MS_CART_F_EXT) --- */
    MS_A_PROGRAM  = 8,   /* MVM1 bytecode                               */
    MS_A_MANIFEST = 9,   /* key=value text                              */
    MS_A_ANIM     = 10   /* u16 count | 8-byte animation records        */
} ms_asset_kind_t;

#define MS_CART_KIND_CANON_MAX 7u
#define MS_CART_KIND_MAX      10u

#define MS_CART_MAGIC     0x31365452u   /* 'RT16' little-endian */
#define MS_CART_VERSION   2u
#define MS_CART_HDR       20u
#define MS_CART_TOC_ENTRY 48u
#define MS_CART_MAX_RECORDS 64u
#define MS_CART_F_EXT     0x0001u       /* Mosaic extension kinds present */

/* Writer staging area: payloads are appended here, then shifted down to
 * their final contiguous position in ms_cart_finish(). */
#define MS_CART_PAYLOAD_STAGING (MS_CART_HDR + MS_CART_MAX_RECORDS * MS_CART_TOC_ENTRY)

typedef enum {
    MS_CART_OK = 0,
    MS_CART_E_PARAM = -1, MS_CART_E_MAGIC = -2, MS_CART_E_SIZE = -3,
    MS_CART_E_CRC = -4,   MS_CART_E_RECORD = -5, MS_CART_E_ASSET = -6,
    MS_CART_E_DUP = -7,   MS_CART_E_BUDGET = -8, MS_CART_E_LAYOUT = -9,
    MS_CART_E_NOSPACE = -10
} ms_cart_err_t;
const char *ms_cart_err_str(int e);

typedef struct {
    const char *name;
    const void *data;
    uint32_t    size;
    uint32_t    kind;
    uint32_t    crc;
} ms_asset_t;

typedef struct {
    const uint8_t *blob;
    size_t   blob_size;
    uint32_t count;
    uint32_t toc_off;
    uint16_t version;
    uint16_t flags;
    uint32_t budget;
    char     title[33];          /* from the manifest asset, "" if none */
} ms_cart_t;

/* Mount validates header, TOC CRC, every record CRC, names, kinds, payload
 * validators, layout order and the byte budget. `budget` is an exact byte
 * count (no size-suffix parsing in engine code). */
int  ms_cart_mount(ms_cart_t *c, const void *blob, size_t bytes);
int  ms_cart_mount_budget(ms_cart_t *c, const void *blob, size_t bytes, size_t budget);
void ms_cart_unmount(ms_cart_t *c);
int  ms_cart_find(const ms_cart_t *c, const char *name, ms_asset_t *out);
int  ms_cart_find_kind(const ms_cart_t *c, uint32_t kind, ms_asset_t *out);
const char *ms_cart_title(const ms_cart_t *c);

/* Per-kind validators. Shared by mount, the packer and the tests. */
int ms_check_tiles(size_t bytes, unsigned bpp, unsigned tiles);
int ms_check_map(const void *data, size_t bytes);
int ms_check_palette(const void *data, size_t bytes);
int ms_check_sheet(const void *data, size_t bytes);   /* SPR2 canonical, SPR1 readable */
int ms_check_bank(size_t bytes);
int ms_check_midi(const void *data, size_t bytes);
int ms_check_program(const void *data, size_t bytes);
int ms_check_manifest(const char *text, size_t bytes);
int ms_check_anim(const void *data, size_t bytes);

/* Largest power-of-two divisor split of a power-of-two entry count,
 * orientated landscape (used when the cart carries no map-dimension hint). */
void ms_map_dims(uint32_t count, int *w, int *h);

/* ---- writer ---- */
typedef struct {
    uint8_t *buf;
    size_t   cap, len, budget;
    uint32_t count;
    uint16_t flags;
    uint8_t  names[MS_CART_MAX_RECORDS][32];
    uint32_t offsets[MS_CART_MAX_RECORDS], sizes[MS_CART_MAX_RECORDS];
    uint32_t kinds[MS_CART_MAX_RECORDS], crcs[MS_CART_MAX_RECORDS];
} ms_cart_writer_t;

void ms_cart_writer_begin(ms_cart_writer_t *w, void *buf, size_t cap);
void ms_cart_writer_set_budget(ms_cart_writer_t *w, size_t budget);
int  ms_cart_add(ms_cart_writer_t *w, const char *name, uint32_t kind,
                 const void *data, size_t size);
int  ms_cart_finish(ms_cart_writer_t *w);   /* bytes written, or negative */

#endif
