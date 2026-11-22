/* SPDX-License-Identifier: Apache-2.0 */
/* Mosaic — ms_link.h: the SPI link between console modules.
 * Frames: A5 5A | ver | type | node | seq | len(LE16) | payload | crc16.
 * The link carries register deltas (the default), pixel bands (QFA) and
 * control traffic. Never allocate; one frame fits a 4 KB DMA buffer. */
#ifndef MOSAIC_MS_LINK_H
#define MOSAIC_MS_LINK_H
#include "mosaic/ms_limits.h"
#include "mosaic/ms_render.h"
#include <stdint.h>
#include <stddef.h>

enum {
    MS_LINK_HELLO = 1, MS_LINK_CAPS, MS_LINK_REG_DELTA, MS_LINK_BAND,
    MS_LINK_AUDIO, MS_LINK_INPUT, MS_LINK_ACK, MS_LINK_NACK, MS_LINK_PING
};
enum { MS_ROLE_CORE = 1, MS_ROLE_PPU, MS_ROLE_AUDIO, MS_ROLE_IO, MS_ROLE_STORAGE };

typedef struct {
    uint8_t ver, type, node;
    uint16_t seq;
    uint16_t len;
    uint8_t payload[MS_LINK_MTU];
} ms_frame_t;

typedef struct {
    uint32_t frames_tx, frames_rx, bytes_tx, bytes_rx, crc_errors, overruns;
    uint8_t  peers;             /* bitmask of node ids seen            */
    uint16_t last_seq;
} ms_link_stats_t;

typedef struct {
    ms_link_stats_t st;
    uint8_t  txseq;
    uint8_t  roles[MS_LINK_MAX_NODES];
} ms_link_t;

void ms_link_init(ms_link_t *l);
size_t ms_link_encode(const ms_frame_t *f, void *out, size_t cap);
int  ms_link_decode(ms_frame_t *f, const void *in, size_t n);
void ms_link_note_peer(ms_link_t *l, int node, int role);
/* Build a register-delta payload for the PPU: scroll, backdrop, palette
 * entries and OAM entries that changed. Returns bytes written. */
size_t ms_link_encode_ppu_delta(const ms_ppu_t *ppu, void *out, size_t cap);
const char *ms_link_role_name(int role);

#endif
