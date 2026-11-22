# What was extracted from ESP-16, and what changed

Mosaic is a new engine, not a fork. The normative contract, formats and
enforcement model come from the ESP-16 reference project; the code is new and
self-contained so it builds without the PPU-16/midicore checkouts.

## Kept
* 320×240 RGB565 / 60 Hz, QFA 160×120 ×2, four display modes.
* 2 tile BGs, 8×8 tiles at 2/4/8 bpp, SNES-packed map entries, mask wrapping.
* 128 OAM, 8–64 px, 4/8 bpp, 16 affine matrices, priorities and flips.
* 2×256 RGB565 palettes, index 0 transparent.
* 32 kHz, 16 voices (64 cap), 16 MIDI channels, SMF + banks, **no PCM**.
* 4 MB default cart budget, 1024 tiles/sheet, 128 KB bank.
* "Limits are enforced at every layer" — now literally one implementation
  shared by builder, loader and tests.

## Fixed in Mosaic
| Issue in reference | Mosaic |
|---|---|
| `end_frame` rendered audio into a stack buffer and discarded it | audio sink callback delivers every block to the port — **normative contract, adopted by the reference** |
| Sprite-sheet packer produced palettes it never used (index remap bug) | **SPR2 is canonical** (both repos); palette quantised once and referenced by index, SPR1 read-only with the invariant enforced |
| Map validator accepted sizes the map field could not represent | u16 entry_count, power-of-two + exact size check (+ `bg_map_w/h` manifest hint for dimensions) |
| Cart had no CRC or duplicate-name check | cart16 v2: CRC32 over the TOC + per-record CRC + duplicate rejection — **now shared with the reference platform** |
| Frame contract had no per-frame state reset for VM | entities rebuild OAM each frame from a deterministic VM |
| `validate_cart.py` size-suffix parsing was broken for `512K` | C validator, exact byte budgets, no suffix parsing |
| Only a stub for distributed consoles | SPI link framing, register-delta codec and role negotiation |

## Not covered (yet)
* Affine per-scanline matrix stepping (matrices are per-frame).
* Colour-math blending modes (brightness and backdrop are implemented).
* Cartridge save memory, rumble, link-cable multiplayer (planned, see roadmap).

## Convergence with the reference platform

`docs/08-format-conformance.md` records the reconciliation: cart16 v2
(kinds 1–7, header/TOC/CRC/layout) is byte-identical, SPR2 is canonical
with SPR1 read-only, and Mosaic-only kinds 8–10 are flagged extensions
that `mosaic build --canonical` omits for interchange.
