# Roadmap

Three open items, in the order they unblock the most: **sprite matrix**,
**save formats**, **dedicated server**. Each is written as a work item with
normative semantics, a breakdown, and the acceptance test that decides when it
is done — not a wish list.

Order rationale:

1. **Sprite matrix** touches the renderer contract, so it must land before
   anyone authors art against it or any port implements scanout from it.
2. **Save formats** changes the cart/manifest contract and needs a stable
   asset-kind story before the server can persist sessions.
3. **Dedicated server** reuses the link framing and the save format for session
   persistence, so it comes last by dependency, not by value.

---

## 1. Sprite matrix

### Where it stands

Sprites already accept a *single per-frame* matrix: `ms_oam_t.affine` indexes
`ms_ppu->mat[]`, the mapping is sampled about the sprite centre, and it is
worked out **per pixel**. Backgrounds can additionally take a per-scanline
offset table (`line_scroll_x/y`). Sprites cannot: there is no per-scanline
matrix, no composition, and no public way to build a matrix — callers fill
`ms_matrix_t` by hand, which is why nothing in the engine, the VM or the Studio
uses it.

That gap is what the compat doc still lists:

> Affine per-scanline matrix scaling/shear (a matrix is applied per frame;
> per-scanline raster-table offsets are implemented and tested).

### Normative semantics (v1)

- A matrix is `{a,b,c,d}` in 16.16, mapping a **sampling** coordinate about the
  sprite centre: `src = M * (px - w/2, py - h/2) + (w/2, h/2)`.
- `ms_oam_t.affine` selects the object matrix (or `-1` for none).
- `ms_oam_t.line_mat`, when non-NULL, is a **per-scanline** table with exactly
  `h` entries; entry `y - o->y` applies to that screen row.
- Composition order is fixed: **object matrix first, then line matrix**, i.e.
  `M_total = ms_matrix_mul(M_affine, line_mat[sy])`. Both are optional; with
  neither the sprite is axis-aligned and the existing fast path is untouched.
- The source texture is the sprite's `w x h` box. Samples outside it are
  transparent — a matrix may shrink or rotate content, it does not add texels.
- Flips are applied *after* the matrix, in texture space, so `FLIP_H` still
  means "mirror what you sample".
- Matrix maths is integer-only; no float, no allocation, and the per-row
  composition is hoisted out of the pixel loop.

### Breakdown

- [x] Public matrix API: identity, scale, rotate, shear, multiply, apply.
- [x] `ms_oam_t.line_mat` + per-scanline composition in `sprite_scanline()`.
- [x] `mosaic_matrix_test` in ctest — exact palette-index assertions through
      identity, scale and per-scanline shear.
- [x] `MSOP_SETMAT` VM opcode (matrix index, a, b, c, d) so a cart can drive
      this without C, plus `docs/03-bytecode-isa.md`.
- [x] Manifest `matrices` block and cart kind 12 (`MS_A_MATRICES`): 16-byte
      records, count-prefixed, validated like every other asset.
- [x] Studio inspector: matrix widget — slot picker, `a b c d` with `-`/`+`,
      a live warp preview, and the selected entity's `affine`/`linetab`
      selection. Every edit rewrites the `.mpj` line and rebuilds.
- [x] `.mpj` compiler: `setmat`/`affine`/`linetab` node types and `mat set=N`
      table rows, so a project reaches the opcodes without hand-assembled
      bytecode; `examples/warp/` is the end-to-end demo.
- [ ] Port conformance: ESP32 PPU and the register-delta codec carry the matrix
      table (`REG_DELTA` already carries the per-frame matrices; the per-line
      table needs a new sub-record).
- [x] Fold §1 semantics into `docs/01-platform-spec.md` once the VM opcode and
      the asset kind exist, so the spec states the finished contract once.

### Acceptance

`mosaic_matrix_test` passes — currently 6/6 in ctest — with these assertions
made on exact palette indices, not on "looks about right": identity maps screen
column to source column, half scale maps `x` to `(x-4)/2 + 4` across the whole
row, a shear row maps to the computed source column, a quarter turn of an 8x8
ramp draws 56 of 64 pixels (the eight corners the half-pixel convention clips),
and an out-of-range `affine` index is ignored instead of dereferenced.

### Why it is worth doing

Per-scanline sprite matrices are what make mode-7 floors, rotating bosses, and
per-line "wobble" effects possible without a framebuffer. It also closes the
last read-only gap between Mosaic sprites and the reference PPU contract.

---

## 2. Save formats

### Where it stands

Nothing exists. `docs/01-platform-spec.md` has no save section, the cart has no
save region, `ms_cart_t` has no save handle and the ESP32 port has no backing
store. `docs/07-compatibility.md` lists it as "planned". A game today cannot
persist a high score.

### Design: one image, three views

The mistake to avoid is three formats for three places. Battery-backed SRAM on
hardware, a file on the host and a file in AI Drive all become **the same
byte image**, and the API reads and writes that image.

```
MOS1 | u16 ver=1 | u16 bytes | u8 slots | u8 reserved | u32 cart_crc |
     slot[0..slots-1]
```

Each slot is two copies (A/B) so a power cut mid-write cannot lose the previous
state:

```
SLT1 | u32 seq | u16 len | u16 crc16(body) | u8 name[12] | body[len]
```

- A write goes to the copy with the **older** `seq`, then bumps `seq`. The
  newest valid copy (highest `seq`, CRC ok) is the live one; a corrupt newest
  copy falls back to the other **and reports it** (`MS_SAVE_F_FELLBACK`).
- `cart_crc` (the cart's TOC CRC32) binds the image to one build. Loading a
  save from another cart is a hard error, not a silent empty state — this is
  the invariant the reference project lacked.
- 8 KB default region (`MS_SAVE_BYTES`), 4 slots, 1 KB max body per slot;
  overridable per cart in the manifest (`save = 16384`), capped by the cart
  budget.
- Zero heap: `ms_save_t` is a caller-owned struct over the caller's buffer.
  This is what lets the same code run on ESP32 NVS, an SD file and host memory.

### Breakdown

- [x] `core/include/mosaic/ms_save.h` + `core/src/ms_save.c`: open, list, read,
      write, erase, verify — the whole image in RAM, no I/O.
- [x] Cart kind 13 `MS_A_SAVE` declaring the region size, validated against the
      manifest; a cart without it is read-only by construction.
- [x] Host: `mosaic save` (list/read/write/dump/import/export) writing a
      `.msav` file — byte-identical to the raw flash region, so a file can be
      moved between host and hardware without conversion.
- [x] Export: `.msv` checksum-bound bundle (`cart_crc` + `device_id` + slot payloads) so
      a save traded between consoles is checked before it is written.
- [ ] ESP32 port wiring (v2 identity): NVS for tiny carts, a flash partition or SD file for
      the 8 KB image. Core side is done — the image is written in place and a
      flush is a plain copy of the region, so the port only supplies the bytes.
- [x] VM: `MSOP_LOADG/STOREG` already exist for globals; add `MSOP_SAVE` /
      `MSOP_LOADSAVE` (slot, offset, len) so a cart can persist without C.

### Second linking mechanism (v2, shipped)

The first mechanism binds an image to one build (`cart_crc`). That is right for
a cartridge's own SRAM and wrong for everything else — a rebuild lost the save,
and a sequel could never read the predecessor's. v2 adds a **developer-key link**
and a second home:

- `dev=` namespaces the studio, `save_id=` names the save (shared by a trilogy
  that wants one continuous campaign), `save_reads=` grants explicit read-only
  access to another save id. An image from another developer is refused, not
  empty.
- Console home: bound to `dev_id` + `app_id`, so it survives rebuilds.
  Cartridge home: bound to `dev_id` + `app_id` + `cart_crc`, so it travels with
  that cartridge and only that cartridge.
- Records carry a SipHash-2-4 tag keyed by the developer key, so a hand-edited
  body fails even when its cheap CRC is recomputed. The key is in the cart: this
  is an integrity and collision guard, not DRM.
- `ms_save_store_*` reads the newest copy across both homes and reports which
  one answered.

### Acceptance

A test that writes slot 0, corrupts the newest copy's CRC, reopens and asserts:
the previous value comes back, `MS_SAVE_F_FELLBACK` is set, and the *other*
slot (a different cart's `cart_crc`) is refused. Plus a power-cut simulation
that fills the trailing half of a write with `0xFF` and expects the old state.

---

## 3. Dedicated server

### Where it stands

`ms_link` frames and decodes `REG_DELTA` payloads, and `ms_device` describes a
console of nodes — but only **in one process**. There is no transport, no
session, no headless binary, and no client. Multi-console play today means one
app rendering everything itself.

### Design: the SPI link over TCP, one authority

The link protocol is already framed with a CRC and a 4 KB MTU; it is a
transport-agnostic byte stream. So the server reuses it verbatim:

- **Transport**: TCP, one connection per node, length-prefixed frames (the same
  `A5 5A` framing). A `--udp` mode with the existing `seq`/`NACK` fields is a
  later option; TCP first because correctness is testable.
- **Modes**: `stream` (default) — the server runs the game and sends register
  deltas + audio; clients are thin displays. `lockstep` — clients run the cart
  and exchange inputs, server validates a per-frame state hash. Stream first,
  because the delta codec already exists and it is what a CYD client needs.
- **Authority**: fixed-step simulation at 60 Hz; input latched at frame
  boundary; the server owns the RNG seed and the save image. A late client
  receives a **keyframe** (full `BG` + palette + OAM snapshot) rather than
  waiting for deltas it missed.
- **Session**: `HELLO` → `CAPS` → `JOIN`; the server assigns node ids, and a
  disconnect frees the id but keeps its save slot.

### Breakdown

- [ ] `MS_LINK_JOIN`, `MS_LINK_KEYFRAME`, `MS_LINK_SESSION` frame types;
      `docs/04-spi-link.md` updated with the wire format.
- [ ] `tools/server.c`: headless `mosaic serve --cart X --device Y --port 7777`
      — no SDL, no window; rings the same `ms_core` frame contract.
- [ ] `mosaic play --connect host:7777` client that renders received deltas
      (host SDL window, and the ESP32 port's display task as the real target).
- [ ] Loopback test in ctest: server + two clients on `127.0.0.1`, 300 frames,
      assert zero `crc_errors`, equal frame counts, and byte-identical PPU state
      hashes between server and both clients.
- [ ] Reconnect + keyframe resync test; latency budget documented (delta frame
      ~420 B → 25 kB/s, so a 300 ms hiccup is still recoverable from a keyframe).
- [ ] Save/session persistence through §2 so a server restart does not wipe a
      player's progress.

### Acceptance

The loopback test above. It is the only honest way to claim "multiplayer works"
without owning two consoles: the clients' rendered frames must hash equal to
the server's, frame for frame.
