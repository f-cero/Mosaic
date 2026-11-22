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
