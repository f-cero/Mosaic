# Dedicated server (MSP/1)

`mosaic serve` runs a cartridge headlessly at a fixed rate and puts the PPU
**register stream** on TCP. `mosaic play` is a display node: it mounts the same
cartridge, applies the stream to a local PPU and renders its own scanlines.
No pixels travel. A typical delta is ~500 B, so 60 Hz costs ~30 kB/s.

```
mosaic serve --cart game.mcart [--port 7777] [--saves img.msav] [--frames N]
mosaic play  --connect 127.0.0.1:7777 --cart game.mcart [--frames N]
```

## Authority

The server owns the simulation, the RNG seed and the save image. A client's
word is only its pad and a per-frame hash; a hash that disagrees is counted and
repaired with a keyframe, never trusted. Fixed step at 60 Hz by default; input
is latched at the frame boundary and the pads of every live client are OR-ed
(one shared character is `--fps`-independent, a per-node pad policy is a later
mode).

## Frames

The 8-byte link framing (`A5 5A | ver | type | node | seq | len | payload |
CRC16`) carries MSP/1 types:

| Type | Direction | Payload |
|---|---|---|
| `JOIN` (10) | C→S | `u16 ver, u8 node(0=any), u8 role, char name[16]` |
| `SESSION` (12) | S→C | `node, flags, fps, mtu, frame, cart_crc, save_slots, title[32]` |
| `KEYFRAME` (11) | S→C | whole register file: mode, size, backdrop, fx, backgrounds, 2×256 palettes, 16 matrices, every OAM entry |
| `SYNC` (14) | S→C | `u32 frame` — baseline complete, deltas resume at frame+1 |
| `REG_DELTA` (3) | S→C | `u32 frame, u8 records`, then records |
| `INPUT` (6) | C→S | `u8 node, u16 buttons, i16 touch_x, i16 touch_y` |
| `STATE` (13) | C→S | `u32 frame, u32 applied, u64 hash` |
| `BYE` (15) | both | — |

`cart_crc` in `SESSION` is an FNV-1a of the ROM bytes: a display node that
mounted a different build is refused rather than showing a plausible lie.

## Delta records

| Type | Record | Bytes |
|---|---|---|
| 1 | background scroll `bg, i16 x, i16 y` | 6 |
| 2 | backdrop `u16` | 3 |
| 3 | sprite (v1.1, no sheet) | 12 |
| 4 | matrix `slot, 16 B` | 18 |
| 5 | line table `idx, u16 rows, rows×16 B` | 4+16n |
| 6 | palette entry `bank, idx, u16` | 5 |
| 7 | background config `slot, 9 fields` | 11 |
| 8 | fx `brightness, mosaic, math_on, color_math, u32 factor` | 9 |
| 9 | map patch `bg, u16 first, u16 count, count×u16` | 6+2n |
| 10 | sprite v2 `idx, i16 x, i16 y, w, h, bpp, pal, prio\|flags, affine, u16 rows, sheet` | 15 |
| 11 | sprite count `u8` | 2 |

Sprites, matrices, scroll and the sprite count are absolute every frame; the
palette, background config, fx, map contents and line tables are change-tracked
against a server-side snapshot. Maps are diffed entry by entry, so a
`MSOP_BGTILE` write costs 8 bytes on the wire, not a tilemap.

Type 10 carries a **sheet index** (0xFF = the PPU's default tileset) because a
display node resolves pixels from its own copy of the cartridge: it needs to
know which sheet an entity drew from, not just where it was.

## Joining, and the MTU budget

A new client gets `SESSION`, then the map contents (type 9 chunks, up to
~2000 entries per frame), then a `KEYFRAME` of the *same instant*, then `SYNC`.
The baseline is therefore exactly the frame the deltas resume at. A client that
falls behind, or whose hash disagrees, re-runs the same sequence.

The delta encoder is all-or-nothing: if a frame's records do not fit the 4 KB
MTU it returns 0 and the server sends a keyframe instead. This is deliberate —
the v1.1 encoder clips a too-large line table and expects the receiver to treat
it as absent, which is fine for a wired console that receives the next frame's
table, but a silent truncation on a network is a desync.

## Save image

`--saves img.msav` attaches the console-home save image and writes it back on
exit, so a server restart does not wipe progress. Saves go through the same
`ms_game_set_saves` path a handheld uses: the server is a console, not a
special case. The cartridge home comes from a `.msdm` dump's SRAM when the cart
is run from storage. There is no account, no login and no profile store — the
save belongs to the player's image file.

## Honest limits

* **No UDP mode.** The `seq`/`NACK` fields are unused; TCP first because
  correctness is testable.
* **No audio stream.** `AUDIO` frames are not sent; a display node is silent.
* **FMV splash is not in the stream.** The splash path writes pixels directly;
  a display node shows the register stream and starts when the game does.
* **Display nodes must mount the same cartridge** (the ESP32 target: the same
  cart in its cart partition). Tiles and maps are cart data.
* **Lockstep mode is not implemented.** Only `stream`.
* **The client is headless.** `mosaic play` renders, hashes and can dump a
  screenshot; a windowed client is a wrapper over the same
  `ms_net_client_t`, and the ESP32 port's display task is the real target —
  neither has been run on hardware here.
* **Per-frame hash validation is a diagnostic, not a trust boundary.** Nothing
  authenticates a peer, and that is the point: see FREEDOM.md.
