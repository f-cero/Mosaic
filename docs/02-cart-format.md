# cart16 v2 — the cartridge / game-object format

Shared byte-for-byte with the ESP-16 reference platform. All fields
little-endian.

## Header (20 bytes)
| Off | Size | Field |
|---|---|---|
| 0 | 4 | magic `RT16` (0x31365452) |
| 4 | 2 | version = 2 |
| 6 | 2 | flags (bit 0 = `MS_CART_F_EXT`: Mosaic extension kinds present) |
| 8 | 4 | record count (1–64) |
| 12 | 4 | TOC offset = 20 |
| 16 | 4 | CRC32 (zlib polynomial 0xEDB88320) of the TOC bytes |

## TOC (count × 48 bytes)
| Off | Size | Field |
|---|---|---|
| 0 | 32 | name, NUL-padded, 1–31 printable chars, unique |
| 32 | 4 | kind |
| 36 | 4 | payload offset |
| 40 | 4 | payload size |
| 44 | 4 | CRC32 of the payload |

Payloads follow the TOC contiguously, in TOC order. The title lives in the
manifest asset, not in the header.

## Kinds
| Kind | Name | Payload |
|---|---|---|
| 1 | BG_TILES | `u8 bpp, u16 tile_count, tiles` (16/32/64 B per tile) |
| 2 | BG_MAP | `u16 entry_count (power of two), u16 entries[])` |
| 3 | PALETTE | `u16 entry_count, u16 RGB565 entries[]` |
| 4 | SPRITES | SPR2 sheet (SPR1 accepted, deprecated) |
| 5 | AFFINE | self-describing |
| 6 | BANK | `.sbn`: `SBN1 | u16 ver | u16 count | u16 drums | u32 rate | 12 B records` |
| 7 | MIDI | Standard MIDI File (no PCM anywhere) |
| 8 | PROGRAM | `MVM1` bytecode — **Mosaic extension** |
| 9 | MANIFEST | `key=value` text — **Mosaic extension** |
| 10 | ANIM | `u16 count, {u16 first,u16 frames,u16 fps,u8 loop,u8 pad}[]` — **Mosaic extension** |

Kinds 8–10 require the `MS_CART_F_EXT` flag; a strict cart16 v2 reader
(the reference one) refuses them, which is exactly the intended behaviour:
`mosaic build --canonical` emits a cart without them.

## Two details the payload cannot carry
1. **Map dimensions.** cart16 v2 stores only a power-of-two entry count.
   Mosaic writes `bg_map_w` / `bg_map_h` into the manifest; a cart without
   that hint falls back to `ms_map_dims()` (largest power-of-two divisor
   split, landscape: 2048 → 64×32).
2. **Palette bank.** The payload has no bank byte. Mosaic uses the
   manifest (`palette_bg=`, `palette_spr=`) and otherwise asset order:
   first palette → background bank, second → sprite bank.

## Validation
Mount rejects: wrong magic/version, `toc_off != 20`, TOC CRC mismatch,
duplicate names, out-of-range or unknown kinds, extension kinds without
the flag, payload CRC mismatch, any payload failing its kind validator,
payloads out of order, and any cart above the byte budget passed to
`ms_cart_mount_budget`. The writer re-mounts its own output before
returning, so an unmountable cart is never produced.
