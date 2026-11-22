# Format conformance (ESP-16 ↔ Mosaic)

Implements `HANDOFF-mosaic-format-sync.md`. Limits, affine matrices,
colour math, save memory, rumble and link multiplayer were not touched.

## What converges
| Format | Status |
|---|---|
| cart16 v2 header/TOC/CRC/layout | identical; readers on both sides accept the same bytes |
| Kinds 1–7 payloads | identical (bg_tiles, bg_map, palette, sprites, affine, bank, midi) |
| SPR2 sheet | canonical; Mosaic's writer is the reference implementation |
| SPR1 sheet | read-only, deprecated; rejected if the palette has dead or duplicate entries |
| Audio sink | Mosaic already delivered every block to a registered sink; unchanged |
| CRC32 | zlib polynomial, init/final 0xFFFFFFFF, reimplemented per side (no shared objects) |

## Mosaic extensions (opt-in, flagged)
The engine needs a program and metadata that the reference format has no
kind for. They ride in kinds 8–10 and the header sets `MS_CART_F_EXT`.
A strict cart16 v2 reader rejects them by design, so `mosaic build
--canonical` produces a reference-mountable cart containing only kinds
1–7 (the example builds to 5 assets, 8,611 bytes that way).

## Where the format is lossy, and what Mosaic does
* **Map size** — cart16 v2 stores a power-of-two entry count, not w×h.
  The packer pads each axis to a power of two (40×30 → 64×32 = 2048) and
  records `bg_map_w`/`bg_map_h` in the manifest. Without the hint the
  loader derives a landscape split. Non-power-of-two counts are refused.
* **Palette bank** — no bank byte. Bank comes from the manifest names,
  else asset order.

## Test coverage (`core/tests/test_conformance.c`)
* reference-style cart synthesised to §2.1 → mounts, every asset resolves;
  corrupted payload → `E_CRC`; truncation → `E_SIZE`; duplicated TOC name
  → `E_DUP`; unknown kind → `E_RECORD`; extension kind without flag →
  `E_RECORD`; over-budget → `E_BUDGET`; non-power-of-two map → `E_ASSET`.
* SPR2 round-trip: palette referenced exactly once, index 0 transparent,
  contiguous per-frame tile runs; 4bpp overflow → error.
* SPR1: valid sheet parses; dead palette entry rejected; duplicate colour
  rejected.
* Cross-mount: SPR2 + bank + SMF + tiles + map + palette packed by
  Mosaic's writer mounts with `flags == 0` and canonical layout
  (`payload[0].offset == 20 + count*48`).

**Fixture provenance:** every sheet, bank and SMF is produced by Mosaic's
own writer; the reference-style cart is synthesised byte-for-byte from the
handoff layout (`pack_like_reference`) because the reference packer is not
vendored in this repository.
