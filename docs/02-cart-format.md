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
| 11 | MVID | MVID v1 tile video (`docs/10-fmv.md`) — **Mosaic extension** |
| 12 | MATRICES | `u16 tables, u16 0, u16 rows[tables] (pad to 4), Σrows × {i32 a,b,c,d}` — **Mosaic extension** |

Kinds 8–12 require the `MS_CART_F_EXT` flag; a strict cart16 v2 reader
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

---

## spec-rev 2.1 — payload headers carry what the manifest used to

Two payloads relied on manifest hints to be usable. A strict reference build
(`mosaic build --canonical`) carries no manifest at all, so those hints were
unavailable exactly where they mattered. Both payloads gained an optional
header; the **payload length selects the form**, so v2.0 carts keep mounting
byte-for-byte and the two can never be confused.

### `bg_map`

| form | layout |
|---|---|
| v2.0 | `u16 entry_count` \| `u16 entries[entry_count]` |
| v2.1 | `u16 entry_count` \| `u16 w` \| `u16 h` \| `u16 entries[entry_count]` |

`w`/`h` are the **visible** map size in tiles; `entry_count` is still the
padded power-of-two stride area, so a valid payload satisfies
`entry_count == next_pow2(w) * next_pow2(h)` and the validator rejects any
other combination. Without the dims a 40x30 level (5x4 tiles) can only be
guessed as a power-of-two split and wraps at 8x4 tiles — a visible seam.

### `palette`

| form | layout |
|---|---|
| v2.0 | `u16 entry_count` \| `u16 rgb565[entry_count]` |
| v2.1 | `u16 entry_count` \| `u8 bank` \| `u8 0` \| `u16 rgb565[entry_count]` |

`bank` is 0 (background) or 1 (sprite); any other value fails the validator.
A v2.0 palette is still banked by the manifest (`palette_bg` / `palette_spr`
asset names) and, failing that, by asset order.

Readers accept both forms; writers emit v2.1. `manifest` `bg_map_w` /
`bg_map_h` remain parsed for v2.0 carts and are no longer written.


### Kind 13 — `save` (save region declaration)

Mosaic extension (`MS_CART_F_EXT` required), payload 4 bytes:

```
u16 region_bytes | u8 slots | u8 decl
```

`region_bytes` 512..65532, `slots` 1..8, `decl` is a bitmask — bit 0 console
home, bit 1 cartridge home — and at least one bit must be set. The stride must
leave at least a v2 record header plus one body byte per copy. Declaring the region here (and mirroring it in the manifest
as `save=` / `save_slots=`) makes a cart's save capability inspectable before
it runs: `mosaic save init` sizes the image from this asset, and a cart without
it cannot be written to at all. The image bytes themselves live outside the
cart (SRAM, a flash partition, a host file) — the cart only declares the shape.

## No authentication field

cart16 v2 carries no signature, key, publisher id, nonce or usage counter, and
Mosaic will not add one. The header holds the magic, a format version, a flags
word, the record count, the TOC offset and the TOC checksum - nothing else.
`MS_CART_F_EXT` is a *format* marker: it tells a strict cart16 v2 reader that
Mosaic extension kinds are present so it rejects the cart instead of misparsing
it. It is not a permission bit. See FREEDOM.md.
