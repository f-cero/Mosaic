# 10 — FMV: the video path

Status: **design + measured envelope.** No engine code for video exists yet. The
numbers in §2–§3 were produced by `tools/mvid_probe.py` on this cart16 rev 2.1
tree; every row is labelled *measured* or *derived* so the difference is visible.

## 1. The link is not the constraint

| | value |
|---|---|
| link clock | 40 MHz (`MS_LINK_SPI_HZ`) |
| MTU | 4 KB payload + 10 B header + 2 B CRC |
| one frame on the wire | 4096·8 / 40e6 + 4 µs CS gap = **831 µs** |
| **usable throughput** | **4.98 MB/s** |

A 320×240 RGB565 frame is 153,600 B — 31 ms on the link at full rate, i.e. the
link could carry ~32 full raw frames per second and still be idle most of the
time. Nothing in this design is limited by SPI bandwidth between nodes.

## 2. Where the ceiling actually is

**LCD scanout.** The PPU node pushes RGB565 bytes to the ILI9341 over its own
SPI bus. A full-screen refresh is a fixed cost, independent of codec:

| LCD clock | full frame | ceiling |
|---|---|---|
| 20 MHz | 61.4 ms | 16.3 fps |
| 27 MHz | 45.5 ms | 22.0 fps |
| 40 MHz (typical CYD setting) | **30.7 ms** | **32.6 fps** |
| 55 MHz | 22.3 ms | 44.8 fps |
| 80 MHz (unstable on most panels) | 15.4 ms | 65.1 fps |

*Derived: 320·240·16 / clock.* **At 40 MHz a full-screen change cannot exceed
32 fps even with a free codec**, and 20 fps is the realistic target once the
frame also has to do anything else. This is the number that shapes the format:
full-motion photographic video at 320×240 is not reachable, whatever the codec.

**RAM.** Classic CYD: ESP32-WROOM-32, 240 MHz, 520 KB SRAM, **no PSRAM**,
4 MB flash (mischianti, esp32pins). Affine/tile video working set:

| item | bytes |
|---|---|
| tile sheet, 1024 × 8×8 @4bpp | 32,768 |
| map, 40 × 30 × u16 | 2,400 |
| BG palette (256 × u16) | 512 |
| link ring, 2 × 4 KB | 8,192 |
| **total** | **≈ 43.9 KB** |

A pixel-path alternative needs a full frame buffer per buffered frame — 38.4 KB
at 4bpp, 76.8 KB at 8bpp — so two buffers plus the rest is 120–200 KB. Both fit
in 520 KB; only the tile path leaves room for WiFi + a game.

## 3. Measured envelope

All clips 320×240, 24 frames, 4bpp, 1024-tile sheet, 256-colour palette in 16
slots of 16, LRU tile cache. Bytes are map deltas + new tile definitions.

| content | bytes/frame | 12 fps | 20 fps | quality |
|---|---|---|---|---|
| flat pixel art (12 colours, ≤16 per tile) | **467** avg / 5,613 intra | 5.5 KB/s | 9.1 KB/s | **lossless** (99 dB) |
| photographic, screen-aligned blocks | 38,316 | 449 KB/s | 748 KB/s | 23.4 dB — visible 8×8 blocking |
| photographic, pixel-delta (4bpp + RLE) | 46,330 | 543 KB/s | 905 KB/s | similar |
| photographic, source-aligned map + PPU scroll | ≈ 120 (*derived*) | 1.4 KB/s | 2.3 KB/s | unchanged |

The flat-art row is the important one: **stylised content costs almost nothing
because tiles repeat.** A 6-second splash at 12 fps is 72 × 467 B + 5.6 KB intra
≈ **39 KB** of cart. The photographic row is the honest limit: 16 colours per
8×8 tile cannot carry a sky gradient, and the preview image shows exactly where
it breaks (banding in smooth regions, fine detail lost).

The last row is derived, not measured, and is the real design insight: if the
map is built **aligned to the source image** and motion is expressed as PPU
`scroll_x/scroll_y`, then panning costs nothing per frame — only the newly
exposed tile column enters the sheet (30 tiles every 8 px of pan). My probe
grids each frame to the *screen*, which is why it thrashes the cache; a
production encoder must grid to the *source*.

## 4. MVID container (v1)

Asset kind 10 in the cart TOC. Little-endian, like everything else.

```
MVID  "MVID" | ver u16 | hdr_len u16
      w u16 | h u16 | fps u8 | flags u8        (flags: 1 loop, 2 has_audio)
      pal_off u32 | pal_len u16
      audio_off u32 | audio_len u32            (SMF asset, 0 = silent)
      frames u16 | index_off u32               (u32 offset per frame)
data  frame records, variable length:
      u8 type | u16 payload_len | payload
```

Frame types:

| type | payload | use |
|---|---|---|
| `0 INTRA` | palette diffs, then full map (1200 × u16), then tile defs | first frame, seek points |
| `1 PATCH` | run list (x,y,len, ids[len]) + tile defs | incremental update |
| `2 PAN` | dx i8, dy i8, then PATCH for the exposed edge | scroll-only motion |

Tile defs are 32 B (8×8 @4bpp) written into sheet slots; a def carries its slot
id implicitly by order of first appearance, exactly as the probe models it.
An `INTRA` must appear at least every 240 frames (4 s at 60 Hz) so a seek costs
bounded work.

## 5. Playback engine

Small, no allocation, one new source file `core/src/ms_video.c`:

```c
typedef struct {
    const ms_asset_t *asset;
    uint16_t frame, count;
    uint8_t  type, flags;
    int16_t  scroll_x, scroll_y;
    uint8_t *sheet;          /* 32 KB, caller-provided arena */
    uint16_t *map;           /*  2.4 KB */
    uint16_t pal_bg[256];
    uint32_t audio_sample;   /* A/V clock */
} ms_video_t;

int  ms_video_open(ms_video_t *v, const ms_asset_t *a, void *arena, size_t n);
int  ms_video_apply(ms_video_t *v, uint16_t frame);   /* decode one frame  */
void ms_video_bind(ms_video_t *v, ms_ppu_t *ppu, int bg);  /* bg.tiles/map  */
int  ms_video_advance(ms_video_t *v, ms_ppu_t *ppu, uint32_t samples);
```

`ms_video_bind` is one line of real work: `ppu->bg[bg].tiles = v->sheet;
ppu->bg[bg].map = v->map;` — the renderer is already the decoder. No PPU change,
no new pixel format, no new blitter.

**A/V sync is by audio sample count, never by wall clock**: the synth already
runs at 32 kHz on the core node, so `ms_video_advance` consumes
`frames = samples * fps / 32000` and drops frames when behind. This keeps video
deterministic under QFA and on the desktop build alike, which matters for the
save-state and replay paths.

VM surface: one opcode, `MSOP_VIDEO dst_slot, asset_id, flags` — starts the clip
on background 1, does not block; the frame loop advances it. Skip is a normal
`pressed` test that calls `MSOP_VIDEO` again with the stop flag. A splash is
therefore: `update → video(asset 3, fullscreen) → if pressed(A) or video_done →
load title`.

**Audio limits, stated plainly.** There is no PCM in the engine; an MVID's audio
track is an SMF asset played by the synth. That scores a clip but cannot carry
voice-over. 8-bit mono PCM at 32 kHz is 32 KB/s (6 s ≈ 192 KB, affordable in a
4 MB cart) but needs a new asset kind and a mixer path — deliberate v2 work, not
in this spec.

## 6. Streaming over the link

For clips too large for the cart, or attract-mode served by the ESP32 dedicated
server, MVID also travels on the wire:

```
MS_LINK_VIDEO = 10        u8 clip | u16 frame | u8 chunk | u8 chunks | data
MS_LINK_VIDEO_CREDIT = 11 u8 clip | u8 credits
```

**Credit-based pull, not push.** The PPU node's LCD bus is the bottleneck and
only it knows when the next frame can be flushed, so it grants credits as it
retires frames; the core node sends at most `credits` frames ahead. Two frames
of lookahead hides link jitter (2 × 831 µs) without overrunning the 2-buffer
ring. A missing or late frame repeats the previous one — never a partial update,
because a half-applied tilemap is a different rendered image, and this engine's
whole contract is that recorded frames replay identically.

## 7. Out of scope, with reasons

- **H.264 / any DCT codec on the CYD.** Espressif's `esp_h264` component targets
  S3 (SIMD-assisted software) and P4 (hardware encode); classic ESP32 is not a
  supported target, and the LCD bus ceiling would bite even if it were.
- **Photographic full-motion at 320×240.** Not a codec problem: 153,600 B per
  full-screen change at 40 MHz is 30.7 ms before any decoding.
- **Alpha / overlay video.** The BG layers have no per-pixel alpha; a sprite
  pass would be needed for a video overlay, which costs OAM budget.
- **Arbitrary seeking.** Index is a flat u32 per frame; seeking backwards is
  "restart at the last INTRA", which is fine for splash clips and wrong for
  scrubbing. Not a goal.

## 8. Next

1. `ms_video.c` + `MSOP_VIDEO`, with `test_video`: encode a 12-frame flat-art
   clip, verify every decoded frame's tilemap matches the encoder's, and assert
   the flat-art lossless property (a 16-colour tile must round-trip exactly).
2. `mosaic video encode <frames/> <out.mvid> --fps 12 --source-aligned` in the
   CLI, reusing `tools/mvid_probe.py` as the reference implementation.
3. Splash integration in the tutorial project, which currently has no boot
   sequence at all.
4. Only then: PCM audio track, if voice-over is ever wanted.


## 10. MVID v1 — the implemented container

Shipped as `codec/` (container + allocation-free decoder + tile-atlas encoder),
`core/src/ms_video.c` (PPU playback), `tools/video_main.c` (the `mosaic video`
verbs), `tools/mvidplay/` (SDL2), `tools/mvidplay_win32/` (Win32 GDI, no runtime
dependency) and `android/` (JNI + NDK CMake). All integers little-endian.

### 10.1 Header (56 bytes)

| off | size | field |
|---|---|---|
| 0 | 4 | `MVID` |
| 4 | 2 | version = 1 |
| 6 | 2 | hdr_len = 56 |
| 8 | 2 | width (320 or 160) |
| 10 | 2 | height (240 or 120) |
| 12 | 1 | fps, 1..60 |
| 13 | 1 | flags: 1 loop, 2 has_audio, 4 **QFA** (coded 160x120, display x2) |
| 14 | 2 | frame count (max 4096) |
| 16 | 2 | map_w, logical tiles wide (max 128; 64 for the engine player) |
| 18 | 2 | map_h |
| 20 | 1 | bpp: 4 or 8 |
| 22 | 2 | palette entry count (max 256) |
| 24 | 4 | palette offset |
| 28 | 4 | frame-index offset (u32 file offset per frame) |
| 32 | 4 | audio offset (0 = none) |
| 36 | 4 | audio length |
| 40 | 4 | sheet bytes the player must reserve |
| 44 | 4 | CRC32 of everything from pal_off to EOF |

The tilemap is padded to next_pow2(map_w) x next_pow2(map_h) and the padding is
filled by wrapping the logical map, so a scroll past the edge repeats content
instead of going blank.

### 10.2 Frame record (16 bytes, then payload)

| off | size | field |
|---|---|---|
| 0 | 1 | type: 0 INTRA, 1 PATCH, 2 PAN |
| 2 | 2 | payload length |
| 4 | 2 | scroll_x (signed, absolute per frame) |
| 6 | 2 | scroll_y |
| 8 | 2 | map op count |
| 10 | 2 | tile-def count |
| 12 | 1 | brightness 0..16 (16 = normal, the PPU's own effect) |

Payload order is fixed: `u16 np` then np x (u8 index, u16 rgb565) palette diffs;
`u16 nt` then nt x (u16 slot, tile bytes) tile defs (32 B at 4bpp, 64 B at 8bpp);
`u16 no` then no x (u16 cell, u16 entry) map ops. Entries use the engine's SNES
packing (bits 0-9 tile, 10-12 palette slot, 13 priority, 14/15 flips). INTRA
resets the atlas. PAN has an empty payload: a scroll plus a brightness value,
so a pan frame costs 16 bytes.

### 10.3 The QFA photographic variant

Flags bit 2 means the clip is coded at 160x120 and the display doubles every
pixel — the PPU's QFA path. The map is 20x15 tiles = 300 cells, which fits the
1024-tile atlas three times over, so the atlas keeps history instead of
thrashing. Measured on a 24-frame Ken-Burns clip (ffmpeg zoompan from a
photograph, 12 fps), decoded, upscaled x2 and compared with the 320x240 source:

| mode | coded | bytes/frame | PSNR vs source |
|---|---|---|---|
| tile4 | 320x240 | 5618 | 7.85 dB |
| qfa4 | 160x120 | 4617 | 11.50 dB |
| **qfa8** | 160x120 | 18761 | **19.05 dB** |

qfa8 is therefore the photographic mode: 8bpp tiles, 256 colours per tile, no
palette slots, 220 KB/s at 12 fps. That is Mega-CD-class picture quality, not
video, and it is the honest ceiling of 1024 8x8 tiles.

Measured limits:
- **Full-resolution photographic video does not fit.** 320x240 is 1200 tiles per
  frame and a map entry has 10 bits of tile index, so 1024 is the ceiling. The
  encoder now degrades gracefully (tail-only eviction, never a dangling
  reference) but the picture collapses: see the 7.85 dB row.
- **4bpp slots are poor for photographs** (11.50 dB) because the eight 16-entry
  slots are cut from the palette in median-cut order. Clustering the palette
  into slots by luminance/occupancy is the next codec task.
- **4bpp is exact for pixel art**: `mosaic_video_test` asserts that every
  decoded frame of a 24-frame sprite clip equals its source, byte for byte.

### 10.4 Tools

    mosaic video encode --input clip.mp4 --mode qfa8 --fps 12 --out intro.mvid
    mosaic video compose scene.mvj --out splash.mvid
    mosaic video info clip.mvid
    mosaic video frames clip.mvid --out frames/
    mvidplay clip.mvid            # Linux and Windows

`encode` shells out to the **ffmpeg binary** for decoding only; nothing links
libav*, so Mosaic stays Apache-2.0 and the user's ffmpeg keeps its own licence.
`compose` renders through `ms_convert_level` + `ms_ppu_render` with affine
matrices, scrolls and brightness, so the clip is the engine's own output.

### 10.5 The tutorial splash (shipped)

`examples/hello/splash.mvj` composes five shots: a fade-in pan across a 384x240
title card, a hold, a fade to black, then an affine zoom into the level art and
a fade out. Result: 70 frames, 251403 bytes (3591.5 B/frame),
7 INTRA / 25 PATCH / 38 PAN. The Hello cart grows from 8,763 B to
260228 B. `ms_game_load` mounts cartridge kind 11, `ms_game_update` advances it
at the clip's fps, A or START skips, and playback is handed to the PPU with
`bg[1].tiles = sheet; bg[1].map = map`.
