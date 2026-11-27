# Mosaic — a modular handheld console, engine and studio

**Mosaic** turns the standards extracted from the ESP-16 reference platform
(PPU-16 + midicore + the ESP16 Unreal plugin) into a standalone, Apache-2.0
game platform you can build from whatever parts you have: one or more ESP32
boards, a CYD display, a DAC, an SD slot — wired over SPI in any arrangement.

```
┌──────────────── Mosaic Studio (Windows / Linux) ────────────────┐
│ node graph · sequence · scene preview · asset pipeline           │
│                    │ build (ms_build)                            │
└────────────────────┼─────────────────────────────────────────────┘
                     ▼  game.mcart  (game object, flash-mapped)
┌──────────────── Mosaic Core (portable C99, no heap) ────────────┐
│ PPU (tiles/OAM/affine/RGB565) · MIDI synth · bytecode VM        │
│ cart loader · input · SPI link · device/role manager            │
└───────┬───────────────────────┬───────────────────────┬──────────┘
        ▼                       ▼                       ▼
   ESP32 CORE              CYD / PPU node           AUDIO node
   (VM, logic)             (scanout over SPI)       (I2S / PWM)
```

## Audio

Recorded sound can be transcribed into a midicore SMF, in music or
speech mode: `mosaic audio ingest take.wav -o take.mid --mode music`. See
[docs/11-audio-ingest.md](docs/11-audio-ingest.md).

## What is in the box

| Path | What |
|---|---|
| `core/` | engine + runtime: PPU, synth, VM, cart, link, device manager — pure C99, zero heap on the frame path |
| `studio/` | SDL2 GUI editor (Linux + Windows): drag/drop node graph, sequence view, scene preview, inspector, asset browser, Build/Export |
| `tools/main.c` | `mosaic` CLI: `build`, `validate`, `run`, `screenshot`, `convert`, `rom` |
| `examples/starfall/` | complete example: PNG art, SMF music, node graph, built cart + ROM |
| `docs/` | normative platform spec, formats, ISA, SPI protocol, porting guide |
| `ports/esp32/` | ESP-IDF component: display, audio, SD, Wi-Fi/BLE, SPI link |

## Build & run (host)

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/mosaic-studio examples/starfall/starfall.mpj
```

CLI end-to-end:

```sh
cd examples/starfall
../../build/mosaic build starfall.mpj -o starfall.mcart
../../build/mosaic validate starfall.mcart
../../build/mosaic run starfall.mcart --frames 90 --left --screenshot shot.png
../../build/mosaic rom starfall.mcart -o starfall.mrom
```

Windows: install SDL2 (vcpkg `sdl2`), then
`cmake -B build -DSDL2_DIR=<sdl2>/lib/cmake/SDL2` and build with MSVC.
The engine and tools have no platform dependencies; only the Studio needs SDL2.

## Design rules

1. **Limits are enforced, not advisory** — editor, builder, cart mount and
   runtime share one validator table (`core/include/mosaic/ms_limits.h`).
2. **Registers, not framebuffers** — a full 320×240 RGB565 frame is
   153.6 KB (9.2 MB/s at 60 Hz) and will not fit a 40 MHz SPI link, so the
   link streams register/OAM deltas and each display node rasterises its own
   band. QFA (160×120) halves the pixel path when streaming is unavoidable.
3. **No floats, no heap on the hot path** — 16.16 fixed point, static arenas.
4. **One format, one truth** — the builder writes exactly what the loader
   validates; `mosaic validate` re-runs the same checks in Python-free C.

## Status

Working today: engine core, cart format, node compiler, CLI, Studio, example
game, host tests, ROM packing. ESP32 port is compile-ready source that needs
ESP-IDF (not built here). See `docs/07-compatibility.md` for the ESP-16
reference deltas that Mosaic fixes.

Open work — sprite matrices, save formats, dedicated server — is planned in
[docs/ROADMAP.md](docs/ROADMAP.md).

License: Apache-2.0 (`LICENSE`, `NOTICE`).
