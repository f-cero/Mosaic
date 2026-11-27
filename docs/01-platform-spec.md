# Mosaic Platform Specification — revision 1

Normative. Mirrored by `core/include/mosaic/ms_limits.h`.

## 1. Display
| Item | Value |
|---|---|
| Resolution | 320×240 RGB565 @ 60 Hz |
| QFA | 160×120 render, 2× integer upscale |
| Modes | `TEXT` (2×4bpp BG), `RICH` (8bpp+4bpp BG), `AFFINE`, `AFFINE2` |
| Colour | 2 banks × 256 RGB565; index 0 transparent |
| FX | brightness 0–16, backdrop, mosaic, per-line scroll tables |

## 2. Backgrounds
2 layers max, 8×8 tiles at 2/4/8 bpp (16/32/64 B per tile). Map entries are
SNES-packed: bits 0–9 tile, 10–12 palette slot, 13 priority, 14 H-flip,
15 V-flip. Map dimensions are powers of two and wrap by mask.

## 3. Sprites
128 OAM entries; 8–64 px per axis in steps of 8; 4 bpp (16-colour slots) or
8 bpp; H/V flip; priority 0 (front)…3 (back). Index 0 of each tile is
transparent.

### 3.1 Affine matrices
Sixteen shared matrices of `ms_matrix_t { a, b, c, d }`, 16.16 fixed point. A
sprite samples through

```
src = M * (px - w/2, py - h/2) + (w/2, h/2)
```

so the matrix maps destination offsets back to source offsets about the sprite
centre and is stationary under translation. Composition is
`M_total = M_object × M_scanline` — the scanline entry applies first. Flips are
applied after the matrix, in texture space. Samples outside the sprite box are
transparent. Arithmetic is integer throughout, and a scanline composes its
matrix once, not once per pixel.

A sprite selects a slot with `affine` (0–15, or −1 for none). Matrices are
per frame: a cart writes a slot with `SETMAT` and selects it with `AFFINE`.

### 3.2 Per-scanline matrices
`line_mat` is a table of `line_rows` entries; scanline `sy = y - o->y` uses
entry `sy` and identity past the end of the table. One entry is one
`ms_matrix_t`, so a 240-line warp is 3840 bytes — under a quarter of the link
MTU. Tables live in the cart as asset kind `MATRICES` (12) and a cart selects
one with `LINETAB`. Tables may sit at any byte offset; the engine reads them
with a 16-byte copy, so no alignment is required of the packer.

## 4. Audio
32 kHz default (8–48 kHz accepted), 16 voices default (hard cap 64),
16 MIDI channels, channel 10 percussion. Music is Standard MIDI File only,
instruments are `.sbn` wavetable banks or the built-in GM-lite bank.
**PCM is forbidden** — there is no sample path. SFX are synthesised one-shots.

## 5. Budgets
Cartridge 4 MB default (device override to 16 MB), instrument bank 128 KB,
1024 tiles per sheet, program 256 KB, 64 entities, 256 globals, 64-deep VM
stack, 8 link nodes, 4 KB link MTU.

## 6. Console composition
A console is a *core* plus any number of nodes on separate SPI chip-selects:
`core`, `ppu`, `audio`, `io`, `storage`. `device.conf` declares them; link
`HELLO`/`CAPS` negotiates roles at boot. Any mix is legal as long as exactly
one core exists; a solo ESP32 with integrated display is a single-node config.

## 7. Frame contract
```c
ms_core_init(&core, &cfg, framebuffer, arena, sizeof arena);
ms_game_load(&game, &cart, &core.ppu, &core.synth);
for (;;) {
    ms_input_edge(&in, read_pad());
    ms_core_run_frame(&core, &game, &in);  /* update, render, audio block */
}
```
`ms_core_run_frame` never allocates, never blocks, and is deterministic for a
given input sequence (VM RNG is seeded and frame-counted).

## 8. Enforcement
`ms_cart_mount` validates every record (kind, size, CRC, limits) before a game
is allowed to run. `mosaic build` runs the same validators; the Studio's Build
button calls the same function, so what the editor exports is what the console
accepts.


## Save image (v1.1)

One byte image is the whole save format: a battery-backed SRAM region, a flash
partition, a `.msav` host file and an AI Drive object are the same bytes, read
and written in place through a caller-owned buffer. No heap, no I/O, no floats.
All little-endian:

```
MOS1 | u16 ver=1 | u16 bytes | u8 slots | u8 reserved | u32 cart_crc
copy[slot*2 + which]  for every slot, both copies, at a fixed stride of
                      (bytes - 14) / (slots * 2)
```

```
SLT1 | u32 seq | u16 len | u16 crc16(body) | u8 name[12]
     | u16 hcrc (crc16 of the first 24 bytes) | u16 0 | body[len]
```

Normative rules:

- A write goes to the copy with the **older `seq`**, and commits by writing
  `hcrc` **last**. A power cut at any point therefore leaves the other copy
  authoritative; a record whose `hcrc` or `body` CRC fails is never used.
- The live copy is the valid copy with the highest `seq`. When the newest copy
  fails its checks the older one is used and the reader reports
  `MS_SAVE_F_FELLBACK | MS_SAVE_F_TORN` — a fallback is never silent.
- `cart_crc` is the cart's TOC CRC32. Opening an image written by another build
  is `MS_SAVE_E_CART`, not an empty save. The image is bound to exactly one
  build on purpose: a struct-shaped save that silently reinterprets after an
  edit is worse than a refused load.
- `len == 0` with an empty name is a tombstone: an erased slot, and it is
  written through the same A/B path so an erase is as power-safe as a write.
- `h` bodies are capped by the region: `max_body = stride - 28`. A caller that
  needs more must ask for a bigger region in the cart, not shrink the body.
- A cart declares the region with asset kind 13 (§02). A cart without that
  asset is **read-only by construction**.
