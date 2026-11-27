# Module link protocol (SPI)

```
A5 5A | ver | type | node | seq | len LE16 | payload[len] | CRC16-CCITT
```
8-byte header, 2-byte CRC over header+payload, MTU 4096. Receiver resets on
`A5 5A` and drops frames with bad CRC (counted in `crc_errors`).

## Types
`HELLO, CAPS, REG_DELTA, BAND, AUDIO, INPUT, ACK, NACK, PING`.

## Bandwidth (the reason for the register model)
| Stream | Bytes/frame | @60 Hz |
|---|---|---|
| Full 320×240 RGB565 | 153,600 | 9.2 MB/s |
| QFA 160×120 RGB565 | 38,400 | 2.3 MB/s |
| Register delta (typical 40 sprites + 2 BG) | ~420 | 25 kB/s |

A 40 MHz SPI link carries roughly 4 MB/s of payload after framing, so full
pixel streaming is impossible. Mosaic's default topology therefore sends
`REG_DELTA` packets: BG scroll, backdrop, palette writes and OAM entries.
Each display node owns a raster band and renders it locally from the same
register stream, which also load-balances across multiple CYDs. `BAND` exists
for modes where a node cannot render (e.g. a dumb panel driven by another MCU);
use QFA and split the frame across links.

## Roles and discovery
Each node answers `HELLO` with `CAPS` (role bitmask, display size, audio rate).
The core assigns roles and records peers in `ms_link_t` (`ms_link_note_peer`).
`device.conf` declares the expected wiring so a missing node is a hard error,
not a silent black screen.

## Framing implementation
`core/src/ms_link.c` — `ms_link_encode`, `ms_link_decode`, `ms_link_note_peer`,
`ms_link_encode_ppu_delta`. The ESP32 port drives it over `spi_master` with one
CS per node and a DMA-capable bounce buffer.

## PPU register-delta payload (v1.1)

`REG_DELTA` payload byte 0 is a sub-record count; records follow, each starting
with a type byte. Type numbers are stable.

| Type | Record | Bytes |
|---|---|---|
| 1 | background scroll: `bg, scroll_x LE16, scroll_y LE16` | 6 |
| 2 | backdrop: `rgb565 LE16` | 3 |
| 3 | sprite: `idx, x LE16, y LE16, w, h, priority\|flags, pal_slot, affine, line_rows` | 13 |
| 4 | matrix: `slot, a,b,c,d LE i32` | 18 |
| 5 | line table: `sprite_idx, rows LE16, rows × 16 B` | 3 + 16×rows |

Type 3 carries `affine = 0xFF` for "no matrix" and `line_rows = 0` for "no
table". Type 4 is emitted for all sixteen slots every frame — 288 bytes, small
enough that change tracking would cost more than it saves. Type 5 is emitted
per sprite that has a table, clipped to whatever remains in the 4 KB MTU; a
table that does not fit must be followed by a keyframe rather than silently
truncated. A receiver must treat a truncated type 5 as absent.
