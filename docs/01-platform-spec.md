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
8 bpp; H/V flip; priority 0 (front)…3 (back); up to 16 shared affine matrices.
Index 0 of each tile is transparent.

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
