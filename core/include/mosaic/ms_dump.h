/* SPDX-License-Identifier: Apache-2.0 */
/* Mosaic — ms_dump.h: reading a cartridge back out.
 *
 * A cartridge is a passive memory. Nothing in this interface authenticates it,
 * asks it for permission, counts how many times it has been copied or refuses
 * a second copy: a dump is the bytes, all of the bytes, and the same bytes
 * every time. If a reader could not copy a cart, that would be a bug in the
 * cart format, not a feature of it — see FREEDOM.md.
 *
 * The dumper is a single forward pass, so it works on a source that cannot
 * seek (an SPI flash, a link, a cartridge edge connector read out byte by
 * byte) and on a sink smaller than the cartridge. It needs MS_DUMP_SCRATCH
 * bytes of the caller's memory — the same staging area the packer already
 * uses — and one 256-byte stack buffer. No heap.
 */
#ifndef MOSAIC_MS_DUMP_H
#define MOSAIC_MS_DUMP_H
#include "mosaic/ms_cart.h"
#include "mosaic/ms_types.h"

/* Header + all possible TOC entries: the dumper's scratch requirement. */
#define MS_DUMP_SCRATCH (MS_CART_HDR + MS_CART_MAX_RECORDS * MS_CART_TOC_ENTRY)

/* A byte source and a byte sink. `read` fills `len` bytes at absolute `off`
 * and returns 0, or non-zero if the medium could not supply them. `write`
 * consumes `len` bytes and returns 0, or non-zero if the medium is full. */
typedef struct {
    int (*read)(void *ctx, uint32_t off, void *dst, uint32_t len);
    int (*write)(void *ctx, const void *src, uint32_t len);
    void *ctx;
} ms_dump_io_t;

typedef struct {
    uint32_t bytes;      /* bytes copied to the sink                          */
    uint32_t crc;        /* crc32 of those bytes                              */
    uint32_t records;    /* TOC records                                       */
    uint32_t verified;   /* records whose payload crc32 matched               */
    uint32_t failed;     /* records whose payload crc32 did not               */
    uint8_t  ok;         /* structure valid and every payload matched         */
    uint8_t  ext;        /* MS_CART_F_EXT seen                                */
    char     title[33];  /* from the manifest asset, "" if none               */
} ms_dump_result_t;

/* Copy `total` bytes of the cartridge through the sink, verifying the TOC and
 * every payload crc on the way. Pass total = 0 to copy exactly the bytes the
 * TOC declares (no trailing flash padding).
 *
 * Always copies as much as the medium allows, even when verification fails:
 * refusing to copy is not a safety feature. Returns MS_DUMP_OK only when the
 * structure is valid and every payload matched; a bad payload still yields
 * the bytes and MS_DUMP_E_VERIFY. */
int ms_dump_cart(const ms_dump_io_t *io, uint32_t total, void *scratch,
                 uint32_t scratch_len, ms_dump_result_t *out);

/* Verify a cartridge already in memory, without writing it anywhere. */
int ms_dump_blob(const void *blob, size_t bytes, ms_dump_result_t *out);

/* ---- the on-device container ------------------------------------------
 * One file holding the ROM and the cartridge's SRAM, so a dumped game can be
 * played off the console's own storage with the cartridge in a drawer, and
 * handed to a friend as a single file. The ROM sits at rom_off and is a
 * complete cart16 v2 image: mount it with ms_cart_mount(c->rom, c->rom_bytes).
 * The SRAM sits at sram_off and is a complete MOS1 save image.
 *
 *   u32 'MSDM' | u16 ver=1 | u16 flags | u32 total
 *   u32 rom_off | u32 rom_bytes | u32 rom_crc32
 *   u32 sram_off | u32 sram_bytes | u32 sram_crc32
 *   char title[32] | u32 crc32(first 68 bytes)                            */
#define MS_DUMP_MAGIC   0x4D44534Du   /* 'MSDM' little-endian */
#define MS_DUMP_VERSION 1u
#define MS_DUMP_HDR     72u
#define MS_DUMP_F_SRAM  0x0001u

typedef struct {
    const uint8_t *rom, *sram;
    uint32_t rom_bytes, rom_crc, sram_bytes, sram_crc;
    char title[33];
} ms_dump_container_t;

size_t ms_dump_container_need(uint32_t rom_bytes, uint32_t sram_bytes);
/* sram may be NULL / 0 for a ROM-only dump. */
int ms_dump_container_build(void *out, size_t cap, const void *rom,
                            uint32_t rom_bytes, const void *sram,
                            uint32_t sram_bytes, const char *title, size_t *len);
int ms_dump_container_open(ms_dump_container_t *c, const void *blob, size_t bytes);

enum {
    MS_DUMP_OK = 0,
    MS_DUMP_E_PARAM = -1, MS_DUMP_E_MAGIC = -2, MS_DUMP_E_SIZE = -3,
    MS_DUMP_E_CRC = -4,   MS_DUMP_E_LAYOUT = -5, MS_DUMP_E_IO = -6,
    MS_DUMP_E_SCRATCH = -7, MS_DUMP_E_VERIFY = -8
};
const char *ms_dump_err_str(int e);

#endif
