/* SPDX-License-Identifier: Apache-2.0 */
/* Mosaic — ms_vm.h: deterministic bytecode VM behind the visual scripting.
 * The Studio compiles node graphs and sequences into this ISA (docs/03). */
#ifndef MOSAIC_MS_VM_H
#define MOSAIC_MS_VM_H
#include "mosaic/ms_limits.h"
#include "mosaic/ms_types.h"
#include "mosaic/ms_input.h"
#include "mosaic/ms_render.h"

/* Opcodes — see docs/03-bytecode-isa.md. Forms: RRR (4 bytes),
 * RI (4 bytes: op,dst,i16), RRI? kept out; RRRR (5 bytes). */
enum {
    MSOP_NOP = 0, MSOP_LDI, MSOP_LDI32, MSOP_MOV,
    MSOP_ADD, MSOP_SUB, MSOP_MUL, MSOP_DIV, MSOP_MOD, MSOP_NEG,
    MSOP_AND, MSOP_OR, MSOP_XOR, MSOP_NOT, MSOP_SHL, MSOP_SHR,
    MSOP_EQ, MSOP_LT, MSOP_GT, MSOP_LE, MSOP_GE,
    MSOP_JMP, MSOP_JZ, MSOP_JNZ, MSOP_CALL, MSOP_RET, MSOP_WAIT, MSOP_HALT,
    MSOP_GETBTN, MSOP_PRESSED, MSOP_TOUCHX, MSOP_TOUCHY, MSOP_TOUCHON,
    MSOP_RAND, MSOP_SIN, MSOP_COS,
    MSOP_SETX, MSOP_SETY, MSOP_SETVX, MSOP_SETVY, MSOP_ADDX, MSOP_ADDY,
    MSOP_SETANIM, MSOP_SETFRAME, MSOP_SETPRIO, MSOP_SHEET, MSOP_HIDE,
    MSOP_PLAYSFX, MSOP_PLAYSONG, MSOP_STOPSONG,
    MSOP_BGSCROLL, MSOP_BGTILE, MSOP_PALENTRY, MSOP_FX,
    MSOP_SPAWN, MSOP_DESTROY, MSOP_SIGNAL, MSOP_GETSIG,
    MSOP_LOADG, MSOP_STOREG, MSOP_LOADF, MSOP_STOREF,
    MSOP_ENTCOUNT, MSOP_LOADENT, MSOP_SETVAR, MSOP_GETVAR,
    MSOP_LOADTIMER, MSOP_LOADFRAME, MSOP_SEED,
    MSOP_SETMAT, MSOP_AFFINE, MSOP_LINETAB,
    MSOP_COUNT
};

typedef struct {
    uint16_t start_pc, update_pc, input_pc;
    uint16_t sheet;          /* sprite sheet asset slot                */
    uint16_t anim;           /* initial animation id                  */
    int16_t  x, y;
    uint8_t  priority, flags;
    char     name[16];
} ms_entity_def_t;

typedef struct {
    uint32_t magic, version;
    uint32_t code_size;
    uint32_t entities_off;
    uint16_t entity_count;
    uint16_t boot_pc;
    uint16_t frame_pc;
    uint16_t reserved;
    const uint8_t *code;
    const ms_entity_def_t *defs;
} ms_program_t;

typedef struct {
    ms_fix x, y, vx, vy;
    uint16_t sprite, frame, anim, sheet;
    uint8_t  flags, priority, pal;
    ms_matrix_t mat;          /* object matrix written by SETMAT          */
    int16_t  affine;          /* shared matrix slot, -1 = none            */
    int16_t  line_tab;        /* MS_A_MATRICES table index, -1 = none     */
    uint32_t timer, spawn_frame;
    int16_t  def;
    char     name[16];
} ms_entity_t;

typedef struct ms_game ms_game_t;

typedef struct {
    uint16_t pc;
    int16_t  wait;
    uint8_t  started;
    uint8_t  alive, dead;
    uint8_t  r[MS_VM_REGS];
    uint32_t reg[MS_VM_REGS];
    uint32_t stack[MS_VM_STACK];
    uint8_t  sp;
    ms_entity_t e;
    int16_t  parent;
} ms_vm_entity_t;

typedef struct {
    const ms_program_t *prog;
    ms_vm_entity_t ents[MS_MAX_ENTITIES];
    uint16_t count;
    uint32_t frame;
    uint32_t rng;
    uint8_t  signal[8];
    uint32_t globals[MS_VM_GLOBALS];
    const ms_input_t *input;
    int      error;
    char     errmsg[64];
    ms_game_t *game;
} ms_vm_t;

int  ms_program_parse(ms_program_t *p, const void *data, size_t bytes);
int  ms_vm_init(ms_vm_t *vm, const ms_program_t *prog, ms_game_t *game);
int  ms_vm_spawn(ms_vm_t *vm, int def_index);
void ms_vm_update(ms_vm_t *vm, const ms_input_t *in);
void ms_vm_signal(ms_vm_t *vm, int id);

#endif
