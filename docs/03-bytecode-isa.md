# Mosaic VM bytecode (MVM1)

Fixed 4-byte instructions unless noted. Registers are 16 × 32-bit (16.16
fixed point). One context per entity; code is shared. Execution is bounded by
an instruction budget per frame (512) and by `WAIT`.

| Form | Bytes | Layout |
|---|---|---|
| RRR | 4 | op, dst, src1, src2 |
| RI | 4 | op, dst, imm16 (LE) |
| REL | 4 | op, reg, rel16 (signed, from next instruction) |
| LDI32 | 6 | op, dst, imm32 |
| 4R | 5 | op, a, b, c, d (register indices) |

## Opcodes (numeric values are stable)
`NOP, LDI, LDI32, MOV, ADD, SUB, MUL, DIV, MOD, NEG, AND, OR, XOR, NOT, SHL,
SHR, EQ, LT, GT, LE, GE, JMP, JZ, JNZ, CALL, RET, WAIT, HALT, GETBTN, PRESSED,
TOUCHX, TOUCHY, TOUCHON, RAND, SIN, COS, SETX, SETY, SETVX, SETVY, ADDX, ADDY,
SETANIM, SETFRAME, SETPRIO, SHEET, HIDE, PLAYSFX, PLAYSONG, STOPSONG, BGSCROLL,
BGTILE, PALENTRY, FX, SPAWN, DESTROY, SIGNAL, GETSIG, LOADG, STOREG, LOADF,
STOREF, ENTCOUNT, LOADENT, SETVAR, GETVAR, LOADTIMER, LOADFRAME, SEED,
SETMAT, AFFINE, LINETAB`.

Comparisons write `1.0` or `0`. `SIN/COS` take turns (1.0 = 360°). Entity
fields: 0 x, 1 y, 2 vx, 3 vy, 4 frame, 5 anim, 6 priority, 8 timer.
Signals are 8 latched bits; `GETSIG` reads one and clears it.

## Matrices
| Op | Form | Bytes | Effect |
|---|---|---|---|
| `SETMAT` | 6, `op, slot, ra, rb, rc, rd` | 6 | `ppu.mat[slot & 15] = {r[a], r[b], r[c], r[d]}` |
| `AFFINE` | RI | 4 | this entity's object matrix slot (`imm` 0–15, `-1` clears) |
| `LINETAB` | RI | 4 | this entity's per-scanline table index in the cart's `MATRICES` asset (`-1` clears) |

`SETMAT` writes one of the 16 shared matrices; several entities can share a
slot, and the write is visible from the next sprite pass. `AFFINE` and
`LINETAB` are per entity and cheap enough to change every frame — that is how
an animation bends a sprite line by line without extra OAM entries. Values in
registers are 16.16; a quarter turn is `0x4000` in `SIN/COS` space but matrix
rotations should be built with the Studio's `setmat` node, which normalises
against the sine peak.

## Semantics
* On spawn, `start_pc` runs once (`WAIT` delays it across frames).
* Every frame `update_pc` runs from the top until `WAIT`, `RET` or budget.
* `SPAWN imm` returns a handle (entity index) in the destination register.
* `DESTROY` ends the entity immediately; OAM is rebuilt each frame.

## Compiler mapping (Studio nodes)
`update`/`start` become handler entry points; exec links become straight-line
code; `if` emits `JZ`/`JMP` with patched relative offsets; value nodes compile
to registers 8–15 with expression recursion; `number` emits `LDI32`.


## Save opcodes

| op | bytes | operands | effect |
|---|---|---|---|
| `SAVE` | 4 | `slot\|home, first_global, globals_n` | Write `globals_n` little-endian int32 globals starting at `first_global` as the slot body, name `"vm"`. |
| `LOADSAVE` | 4 | `slot\|home, first_global, globals_n` | Read the slot body back into those globals; a short body restores only the globals it covers. |

Bit 7 of the first operand selects the home: clear is the console store, set
is the cartridge store. Bits 0..3 are the slot. A cart that asks for the
cartridge home when the cartridge declares none gets `MS_SAVE_E_LOC` and keeps
running, so the same bytecode is valid on a console with no cartridge.

Both are inert (status `MS_SAVE_E_PARAM`, no VM error) when no image is
attached, so a cart that uses them still runs on a host without storage. The
result of the last call is in `ms_vm_t.save_status`; `MS_SAVE_OK` is 0.
`globals_n` is capped at 64 (256 bytes) per call. The image itself is bound to
the cart's TOC CRC, so a save from another build is refused rather than
reinterpreted.
