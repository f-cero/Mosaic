/* SPDX-License-Identifier: Apache-2.0 */
/* Mosaic — ms_net.h: MSP/1, the session layer above the link framing.
 *
 * A session turns the module link into a network protocol. One process is
 * authoritative: it runs the frame contract at a fixed rate, owns the save
 * image and the RNG, and puts the PPU register stream on the wire. Peers are
 * display nodes: they mount the same cartridge for tiles and maps, apply the
 * stream to a local ms_ppu_t, and render their own scanlines. No pixels are
 * sent, which is why a 25 kB/s link can carry a 60 Hz game.
 *
 * Frame types (in the 8-byte ms_link framing):
 *   JOIN      client -> server: want a node, a role, a display name
 *   SESSION   server -> client: node id, fps, cart crc, save slots, title
 *   KEYFRAME  server -> client: whole PPU register file (no tiles, no maps)
 *   SYNC      server -> client: keyframe + map resync complete, deltas follow
 *   REG_DELTA server -> client: u32 frame, u8 records, register records
 *   INPUT     client -> server: pad + touch for one node
 *   STATE     client -> server: u32 frame, u32 applied, u64 frame hash
 *   BYE       either direction: leave
 *
 * REG_DELTA records, each starting with a type byte:
 *   1  background scroll     bg, i16 x, i16 y                  (6)
 *   2  backdrop              u16 rgb565                        (3)
 *   3  sprite (v1.1)         idx, i16 x, i16 y, w, h, prio|flags,
 *                            pal, affine, line_rows            (12)
 *   4  matrix                slot, 16 B                        (18)
 *   5  line table            idx, u16 rows, rows x 16 B        (4 + 16n)
 *   6  palette entry         bank, idx, u16 rgb565             (5)
 *   7  background config     slot, 9 fields                    (11)
 *   8  fx                    brightness, mosaic, math_on,
 *                            color_math, u32 factor            (9)
 *   9  map patch             bg, u16 first, u16 count,
 *                            count x u16                       (6 + 2n)
 *   11 sprite count          u8 count                          (2)
 *   10 sprite v2 (MSP/1)     idx, i16 x, i16 y, w, h, bpp, pal,
 *                            prio|flags, affine, u16 line_rows,
 *                            sheet                             (15)
 *
 * Type 3 is what an ESP32 PPU receives from a console it is wired to; type 10
 * is the same record with the sprite's sheet index (0xFF = the PPU default
 * tileset) so a display node that mounted the cartridge can resolve the same
 * pixels. Type 0xFF in a decode is never produced here.
 *
 * The stream is a *register* stream, not a video stream: tiles are cart data
 * and map patches carry only what the VM wrote (MSOP_BGTILE). A receiver that
 * does not mount the same cartridge cannot render, and the session says so by
 * carrying cart_crc — a mismatch is refused, which is also how two builds of
 * the same title meet cleanly.
 *
 * Core never allocates: every buffer (map snapshots, map copies) is caller
 * storage passed in a ms_net_buf_t. */
#ifndef MOSAIC_MS_NET_H
#define MOSAIC_MS_NET_H
#include "mosaic/ms_link.h"
#include "mosaic/ms_game.h"
#include "mosaic/ms_render.h"
#include <stdint.h>
#include <stddef.h>

#define MS_MSP_VERSION  1u
#define MS_MSP_NAME     16u
#define MS_MSP_NO_SHEET 0xFFu

typedef struct { void *ptr; size_t bytes; } ms_net_buf_t;

typedef struct {
    uint16_t ver;
    uint8_t  node;              /* 0 = assign me one */
    uint8_t  role;              /* MS_ROLE_*          */
    char     name[MS_MSP_NAME];
} ms_net_join_t;

typedef struct {
    uint8_t  node, flags;
    uint16_t fps, mtu;
    uint32_t frame;
    uint32_t cart_crc;
    uint32_t save_slots;
    char     title[33];
} ms_net_session_t;

typedef struct {
    uint32_t frame;
    uint32_t applied;
    uint64_t hash;
} ms_net_state_t;

size_t ms_net_encode_join(uint8_t *out, size_t cap, const ms_net_join_t *j);
int    ms_net_decode_join(ms_net_join_t *j, const uint8_t *in, size_t n);
size_t ms_net_encode_session(uint8_t *out, size_t cap, const ms_net_session_t *s);
int    ms_net_decode_session(ms_net_session_t *s, const uint8_t *in, size_t n);
size_t ms_net_encode_state(uint8_t *out, size_t cap, const ms_net_state_t *s);
int    ms_net_decode_state(ms_net_state_t *s, const uint8_t *in, size_t n);

/* Whole register file. `g` may be NULL (then every sprite is sheet 0xFF). */
size_t ms_net_encode_keyframe(const ms_game_t *g, const ms_ppu_t *p, void *out, size_t cap);
/* Keyframe bytes for an empty OAM of the given count, for size estimates. */
size_t ms_net_keyframe_size(uint16_t oam_count);

/* Change-tracked snapshot. `map_store` may be NULL per background; when it is
 * present it is filled with a copy of the map the snapshot compares against. */
typedef struct {
    uint16_t pal_bg[MS_PAL_ENTRIES], pal_spr[MS_PAL_ENTRIES];
    uint8_t  bg[MS_MAX_BG][9];
    uint8_t  fx[4];
    uint32_t math_factor;
    uint64_t tab_digest[MS_MAX_SPRITES];
    uint16_t *map[MS_MAX_BG];
    uint32_t map_entries[MS_MAX_BG];
    uint8_t  valid;
} ms_net_snap_t;

void   ms_net_snap_init(ms_net_snap_t *s, const ms_ppu_t *p, ms_net_buf_t map_store[MS_MAX_BG]);
/* Build one REG_DELTA payload. Returns 0 if it does not fit `cap`/MTU — the
 * caller must then send a keyframe rather than a truncated delta. */
size_t ms_net_encode_delta(const ms_game_t *g, const ms_ppu_t *p, ms_net_snap_t *s,
                           void *out, size_t cap);
/* A REG_DELTA payload holding up to `max_entries` map entries from `first`.
 * Returns 0 when the background has no map or the range is empty. */
size_t ms_net_encode_map_chunk(const ms_ppu_t *p, int bg, uint32_t first,
                               uint32_t max_entries, uint32_t frame,
                               void *out, size_t cap);

/* Display node: a PPU plus the storage a stream needs (per-sprite line tables
 * and writable map copies). The struct is large by MCU standards (~140 kB with
 * 128 sprites); allocate it in PSRAM on a constrained port. */
typedef struct {
    ms_ppu_t    ppu;
    uint16_t   *fb;
    uint16_t    w, h;
    uint8_t     qfa;
    uint16_t    qfa_scratch[MS_QFA_W * MS_QFA_H];
    ms_matrix_t line_tab[MS_MAX_SPRITES][MS_SPRITE_MAX];
    const uint8_t *sheet_tiles[MS_MAX_SHEETS];
    uint8_t     sheet_count;
    uint32_t    frame;          /* last frame applied                    */
    uint32_t    applied;
    uint32_t    overflow;       /* records rejected as malformed         */
    uint8_t     synced;
} ms_net_client_t;

int  ms_net_client_init(ms_net_client_t *c, const ms_game_t *g, const ms_ppu_t *base,
                        uint16_t *fb, uint16_t w, uint16_t h,
                        ms_net_buf_t map_store[MS_MAX_BG]);
int  ms_net_apply_keyframe(ms_net_client_t *c, const void *payload, size_t n);
int  ms_net_apply_delta(ms_net_client_t *c, const void *payload, size_t n);
void ms_net_render(ms_net_client_t *c);
/* FNV-1a over the pixels, the register file and the map contents. Pointers
 * and the frame counter are excluded, so two processes that agree hash equal. */
uint64_t ms_net_hash(const ms_ppu_t *p, const uint16_t *fb, uint16_t w, uint16_t h);

#endif
