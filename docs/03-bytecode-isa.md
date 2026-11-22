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
STOREF, ENTCOUNT, LOADENT, SETVAR, GETVAR, LOADTIMER, LOADFRAME, SEED`.

Comparisons write `1.0` or `0`. `SIN/COS` take turns (1.0 = 360°). Entity
fields: 0 x, 1 y, 2 vx, 3 vy, 4 frame, 5 anim, 6 priority, 8 timer.
Signals are 8 latched bits; `GETSIG` reads one and clears it.

## Semantics
* On spawn, `start_pc` runs once (`WAIT` delays it across frames).
* Every frame `update_pc` runs from the top until `WAIT`, `RET` or budget.
* `SPAWN imm` returns a handle (entity index) in the destination register.
* `DESTROY` ends the entity immediately; OAM is rebuilt each frame.

## Compiler mapping (Studio nodes)
`update`/`start` become handler entry points; exec links become straight-line
code; `if` emits `JZ`/`JMP` with patched relative offsets; value nodes compile
to registers 8–15 with expression recursion; `number` emits `LDI32`.
