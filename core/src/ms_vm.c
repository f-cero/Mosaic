/* SPDX-License-Identifier: Apache-2.0 */
/* Mosaic — ms_vm.c: the bytecode VM. Deterministic, no heap, fixed stack. */
#include "mosaic/ms_vm.h"
#include "mosaic/ms_game.h"
#include <string.h>
#include <stdio.h>

int ms_program_parse(ms_program_t *p, const void *data, size_t bytes)
{
    const uint8_t *d = (const uint8_t *)data;
    if (!p || !d || bytes < 32 || ms_rd32(d) != MS_PROG_MAGIC) return -1;
    memset(p, 0, sizeof(*p));
    p->magic = ms_rd32(d);
    p->version = ms_rd32(d + 4);
    p->code_size = ms_rd32(d + 8);
    p->entities_off = ms_rd32(d + 12);
    p->entity_count = ms_rd16(d + 16);
    p->boot_pc = ms_rd16(d + 18);
    p->frame_pc = ms_rd16(d + 20);
    p->code = d + 32;
    if (p->code_size == 0 || p->code_size > MS_PROGRAM_MAX) return -2;
    if (32u + p->code_size > bytes) return -2;
    if (p->entity_count > MS_MAX_ENTITIES) return -3;
    if (p->entities_off < 32u + p->code_size ||
        p->entities_off + (size_t)p->entity_count * 32u > bytes) return -3;
    p->defs = (const ms_entity_def_t *)(d + p->entities_off);
    return 0;
}

static void set_err(ms_vm_t *vm, const char *msg)
{
    vm->error = 1;
    snprintf(vm->errmsg, sizeof(vm->errmsg), "%s", msg);
}

static int run(ms_vm_t *vm, ms_vm_entity_t *en, int budget)
{
    const ms_program_t *pr = vm->prog;
    const uint8_t *code = pr->code;
    uint32_t size = pr->code_size;
    while (budget-- > 0) {
        if (en->pc + 4u > size) { set_err(vm, "pc out of range"); return -1; }
        if (code[en->pc] == MSOP_LDI32 && en->pc + 6u > size) { set_err(vm, "pc out of range"); return -1; }
        const uint8_t *ip = code + en->pc;
        uint8_t op = ip[0];
        uint32_t next = en->pc + 4;
        if (op == MSOP_LDI32) next = en->pc + 6;
        if (op == MSOP_BGTILE || op == MSOP_PALENTRY) next = en->pc + 5;
        if (op == MSOP_SETMAT) next = en->pc + 6;
        uint8_t a = ip[1], b = ip[2], c = ip[3];
        int16_t imm = (int16_t)(b | (c << 8));
        ms_fix *reg = (ms_fix *)en->reg;
        switch (op) {
        case MSOP_NOP: break;
        case MSOP_LDI: en->reg[a & 15] = (uint32_t)(int32_t)imm; break;
        case MSOP_LDI32: en->reg[a & 15] = (uint32_t)((uint32_t)ip[2] | ((uint32_t)ip[3] << 8) | ((uint32_t)ip[4] << 16) | ((uint32_t)ip[5] << 24)); break;
        case MSOP_MOV: en->reg[a & 15] = en->reg[b & 15]; break;
        case MSOP_ADD: reg[a & 15] = reg[b & 15] + reg[c & 15]; break;
        case MSOP_SUB: reg[a & 15] = reg[b & 15] - reg[c & 15]; break;
        case MSOP_MUL: reg[a & 15] = ms_fix_mul(reg[b & 15], reg[c & 15]); break;
        case MSOP_DIV: reg[a & 15] = ms_fix_div(reg[b & 15], reg[c & 15]); break;
        case MSOP_MOD: reg[a & 15] = reg[c & 15] ? reg[b & 15] % reg[c & 15] : 0; break;
        case MSOP_NEG: reg[a & 15] = -reg[b & 15]; break;
        case MSOP_AND: reg[a & 15] = (uint32_t)(reg[b & 15] & reg[c & 15]); break;
        case MSOP_OR:  reg[a & 15] = (uint32_t)(reg[b & 15] | reg[c & 15]); break;
        case MSOP_XOR: reg[a & 15] = (uint32_t)(reg[b & 15] ^ reg[c & 15]); break;
        case MSOP_NOT: reg[a & 15] = (reg[b & 15] == 0) ? MS_FIX_ONE : 0; break;
        case MSOP_SHL: reg[a & 15] = (uint32_t)((uint32_t)reg[b & 15] << (reg[c & 15] & 31)); break;
        case MSOP_SHR: reg[a & 15] = (uint32_t)((uint32_t)reg[b & 15] >> (reg[c & 15] & 31)); break;
        case MSOP_EQ: reg[a & 15] = reg[b & 15] == reg[c & 15] ? MS_FIX_ONE : 0; break;
        case MSOP_LT: reg[a & 15] = reg[b & 15] <  reg[c & 15] ? MS_FIX_ONE : 0; break;
        case MSOP_GT: reg[a & 15] = reg[b & 15] >  reg[c & 15] ? MS_FIX_ONE : 0; break;
        case MSOP_LE: reg[a & 15] = reg[b & 15] <= reg[c & 15] ? MS_FIX_ONE : 0; break;
        case MSOP_GE: reg[a & 15] = reg[b & 15] >= reg[c & 15] ? MS_FIX_ONE : 0; break;
        case MSOP_JMP: en->pc = (uint32_t)((int32_t)next + imm); continue;
        case MSOP_JZ: if (!en->reg[a & 15]) { en->pc = (uint32_t)((int32_t)next + imm); continue; } break;
        case MSOP_JNZ: if (en->reg[a & 15]) { en->pc = (uint32_t)((int32_t)next + imm); continue; } break;
        case MSOP_CALL:
            if (en->sp >= MS_VM_STACK) { set_err(vm, "stack overflow"); return -1; }
            en->stack[en->sp++] = next;
            en->pc = (uint32_t)((int32_t)next + imm); continue;
        case MSOP_RET:
            if (!en->sp) { en->pc = next; en->alive = 0; continue; }
            en->pc = en->stack[--en->sp]; continue;
        case MSOP_WAIT: en->wait = imm; en->pc = next; return 0;
        case MSOP_HALT: en->alive = 0; return 0;
        case MSOP_GETBTN: en->reg[a & 15] = (vm->input && (vm->input->held & (uint16_t)imm)) ? MS_FIX_ONE : 0; break;
        case MSOP_PRESSED: en->reg[a & 15] = (vm->input && (vm->input->pressed & (uint16_t)imm)) ? MS_FIX_ONE : 0; break;
        case MSOP_TOUCHX: en->reg[a & 15] = vm->input ? (uint32_t)((int32_t)vm->input->touch_x << 16) : 0; break;
        case MSOP_TOUCHY: en->reg[a & 15] = vm->input ? (uint32_t)((int32_t)vm->input->touch_y << 16) : 0; break;
        case MSOP_TOUCHON: en->reg[a & 15] = (vm->input && vm->input->touch_active) ? MS_FIX_ONE : 0; break;
        case MSOP_RAND:
            vm->rng ^= vm->rng << 13; vm->rng ^= vm->rng >> 17; vm->rng ^= vm->rng << 5;
            en->reg[a & 15] = (uint32_t)(vm->rng % (imm ? (uint32_t)imm : 1u)); break;
        case MSOP_SIN: en->reg[a & 15] = (uint32_t)ms_sin(reg[b & 15]); break;
        case MSOP_COS: en->reg[a & 15] = (uint32_t)ms_cos(reg[b & 15]); break;
        case MSOP_SETX: en->e.x = reg[a & 15]; break;
        case MSOP_SETY: en->e.y = reg[a & 15]; break;
        case MSOP_SETVX: en->e.vx = reg[a & 15]; break;
        case MSOP_SETVY: en->e.vy = reg[a & 15]; break;
        case MSOP_ADDX: en->e.x += reg[a & 15]; break;
        case MSOP_ADDY: en->e.y += reg[a & 15]; break;
        case MSOP_SETANIM: en->e.anim = (uint16_t)imm; en->e.frame = 0; break;
        case MSOP_SETFRAME: en->e.frame = (uint16_t)imm; break;
        case MSOP_SETPRIO: en->e.priority = (uint8_t)imm; break;
        case MSOP_SHEET: en->e.sheet = (uint16_t)imm; break;
        case MSOP_HIDE: if (imm) en->e.flags |= 0x80; else en->e.flags &= (uint8_t)~0x80; break;
        case MSOP_PLAYSFX: if (vm->game) ms_game_sfx(vm->game, reg[b & 15] >> 16, reg[c & 15] >> 16); break;
        case MSOP_PLAYSONG: if (vm->game) ms_game_music(vm->game, imm, 1); break;
        case MSOP_STOPSONG: if (vm->game) ms_game_music(vm->game, -1, 0); break;
        case MSOP_BGSCROLL: if (vm->game && a < MS_MAX_BG) {
                vm->game->ppu->bg[a].scroll_x = (int16_t)(reg[b & 15] >> 16);
                vm->game->ppu->bg[a].scroll_y = (int16_t)(reg[c & 15] >> 16); } break;
        case MSOP_BGTILE: if (vm->game && a < MS_MAX_BG) {
                ms_bg_t *bg = &vm->game->ppu->bg[a];
                int x = reg[b & 15] >> 16, y = reg[c & 15] >> 16;
                if (bg->map && x >= 0 && y >= 0 && x < bg->map_w && y < bg->map_h)
                    ((uint16_t *)bg->map)[y * bg->map_w + x] = (uint16_t)en->reg[ip[4] & 15]; } break;
        case MSOP_PALENTRY: if (vm->game) {
                int bank = a & 1, idx = b, col = en->reg[ip[4] & 15] & 0xFFFF;
                if (idx < 256) { if (bank) vm->game->ppu->pal_spr[idx] = (uint16_t)col; else vm->game->ppu->pal_bg[idx] = (uint16_t)col; } } break;
        case MSOP_FX: if (vm->game) {
                int v = reg[b & 15] >> 16;
                if (a == 0) vm->game->ppu->fx.brightness = (uint8_t)(v < 0 ? 0 : v > 16 ? 16 : v);
                else if (a == 1) vm->game->ppu->fx.backdrop = (uint16_t)v; } break;
        case MSOP_SPAWN: en->reg[a & 15] = (uint32_t)ms_vm_spawn(vm, imm); break;
        case MSOP_DESTROY: en->alive = 0; return 0;
        case MSOP_SIGNAL: ms_vm_signal(vm, imm); break;
        case MSOP_GETSIG: en->reg[a & 15] = (imm >= 0 && imm < 8) ? vm->signal[imm] : 0; break;
        case MSOP_LOADG: en->reg[a & 15] = (imm >= 0 && imm < (int)MS_VM_GLOBALS) ? vm->globals[imm] : 0; break;
        case MSOP_STOREG: if (imm >= 0 && imm < (int)MS_VM_GLOBALS) vm->globals[imm] = en->reg[a & 15]; break;
        case MSOP_LOADF: {
            int f = imm;
            ms_fix v = 0;
            switch (f) {
            case 0: v = en->e.x; break; case 1: v = en->e.y; break;
            case 2: v = en->e.vx; break; case 3: v = en->e.vy; break;
            case 4: v = (ms_fix)(en->e.frame * MS_FIX_ONE); break;
            case 5: v = (ms_fix)(en->e.anim * MS_FIX_ONE); break;
            case 6: v = (ms_fix)(en->e.priority * MS_FIX_ONE); break;
            case 8: v = (ms_fix)(int32_t)en->e.timer; break;
            default: break;
            }
            en->reg[a & 15] = (uint32_t)v; break; }
        case MSOP_STOREF: {
            ms_fix v = reg[a & 15];
            switch (imm) {
            case 0: en->e.x = v; break; case 1: en->e.y = v; break;
            case 2: en->e.vx = v; break; case 3: en->e.vy = v; break;
            case 4: en->e.frame = (uint16_t)(v >> 16); break;
            case 5: en->e.anim = (uint16_t)(v >> 16); break;
            case 6: en->e.priority = (uint8_t)(v >> 16); break;
            default: break;
            } break; }
        case MSOP_ENTCOUNT: en->reg[a & 15] = vm->count; break;
        case MSOP_LOADENT: en->reg[a & 15] = 0; break;
        case MSOP_LOADTIMER: en->reg[a & 15] = (ms_fix)(int32_t)en->e.timer; break;
        case MSOP_LOADFRAME: en->reg[a & 15] = vm->frame; break;
        case MSOP_SETMAT:
            /* op, slot, ra, rb, rc, rd: write a shared matrix slot. */
            if (vm->game && vm->game->ppu && (a & 15) < MS_MAX_MATRICES) {
                ms_matrix_t m;
                m.a = reg[ip[2] & 15]; m.b = reg[ip[3] & 15];
                m.c = reg[ip[4] & 15]; m.d = reg[ip[5] & 15];
                vm->game->ppu->mat[a & 15] = m;
            }
            break;
        case MSOP_SAVE: {
            /* op, slot, first_global, globals_n: the body is globals_n
             * little-endian int32 globals, so a cart can persist without C. */
            const unsigned slot = a & 0x0F, first = b & 255, n = c & 255;
            const unsigned loc = (a & 0x80) ? MS_SAVE_LOC_CART : MS_SAVE_LOC_CONSOLE;
            if (!vm->saves) { vm->save_status = MS_SAVE_E_PARAM; break; }
            if (n == 0 || n > 64 || first + n > MS_VM_GLOBALS) { vm->save_status = MS_SAVE_E_PARAM; break; }
            uint8_t tmp[256];
            for (unsigned i = 0; i < n; i++) ms_wr32(tmp + i * 4u, vm->globals[first + i]);
            vm->save_status = ms_save_store_write(vm->saves, loc, slot, "vm", 0, tmp, (size_t)n * 4u);
            break;
        }
        case MSOP_LOADSAVE: {
            const unsigned slot = a & 0x0F, first = b & 255, n = c & 255;
            if (!vm->saves) { vm->save_status = MS_SAVE_E_PARAM; break; }
            if (n == 0 || n > 64 || first + n > MS_VM_GLOBALS) { vm->save_status = MS_SAVE_E_PARAM; break; }
            uint8_t tmp[256]; size_t got = 0; uint16_t fmt = 0;
            const int rc = ms_save_store_read(vm->saves, slot, NULL, 0, tmp, sizeof tmp,
                                              &got, &fmt, NULL, NULL);
            vm->save_status = rc;
            if (rc == MS_SAVE_OK) {
                const unsigned k = (unsigned)(got / 4u);
                for (unsigned i = 0; i < k && i < n; i++) vm->globals[first + i] = ms_rd32(tmp + i * 4u);
            }
            break;
        }
        case MSOP_AFFINE: en->e.affine = (imm < 0) ? -1 : (int16_t)(imm & 15); break;
        case MSOP_LINETAB: en->e.line_tab = (imm < 0) ? -1 : imm; break;
        case MSOP_SEED: vm->rng = vm->rng ? vm->rng : (uint32_t)imm * 2654435761u; break;
        default:
            set_err(vm, "bad opcode");
            return -1;
        }
        en->pc = next;
    }
    return 0;
}

int ms_vm_init(ms_vm_t *vm, const ms_program_t *prog, ms_game_t *game)
{
    if (!vm || !prog) return -1;
    memset(vm, 0, sizeof(*vm));
    vm->prog = prog;
    vm->rng = 0xC0FFEEu;
    vm->game = game;
    for (uint16_t i = 0; i < prog->entity_count; i++) ms_vm_spawn(vm, i);
    return 0;
}

int ms_vm_spawn(ms_vm_t *vm, int def_index)
{
    if (!vm || !vm->prog || def_index < 0 || def_index >= vm->prog->entity_count) return -1;
    for (int i = 0; i < (int)MS_MAX_ENTITIES; i++) {
        ms_vm_entity_t *en = &vm->ents[i];
        if (en->alive) continue;
        memset(en, 0, sizeof(*en));
        const ms_entity_def_t *d = &vm->prog->defs[def_index];
        en->alive = 1;
        en->pc = d->start_pc != 0xFFFFu ? d->start_pc : (d->update_pc != 0xFFFFu ? d->update_pc : 0);
        en->wait = 0;
        en->e.def = (int16_t)def_index;
        en->e.x = (ms_fix)((int32_t)d->x << 16);
        en->e.y = (ms_fix)((int32_t)d->y << 16);
        en->e.sprite = 0;
        en->e.sheet = d->sheet;
        en->e.anim = d->anim;
        en->e.priority = d->priority;
        en->e.flags = d->flags;
        en->e.spawn_frame = vm->frame;
        en->e.affine = -1;
        en->e.line_tab = -1;
        memcpy(en->e.name, d->name, 16);
        en->parent = -1;
        if (i >= vm->count) vm->count = (uint16_t)(i + 1);
        return i;
    }
    return -1;
}

void ms_vm_signal(ms_vm_t *vm, int id)
{
    if (vm && id >= 0 && id < 8) vm->signal[id] = 1;
}

void ms_vm_update(ms_vm_t *vm, const ms_input_t *in)
{
    if (!vm || !vm->prog) return;
    vm->frame++;
    vm->input = in;
    for (int i = 0; i < (int)vm->count; i++) {
        ms_vm_entity_t *en = &vm->ents[i];
        if (!en->alive) continue;
        if (en->wait > 0) { en->wait--; continue; }
        const ms_entity_def_t *d = &vm->prog->defs[en->e.def < 0 ? 0 : en->e.def];
        if (!en->started) {
            en->started = 1;
            if (d->start_pc != 0xFFFFu) { en->pc = d->start_pc; run(vm, en, 512); }
            if (!en->alive) continue;
        }
        if (en->wait > 0) { en->wait--; continue; }
        if (d->update_pc != 0xFFFFu) { en->pc = d->update_pc; run(vm, en, 512); }
        if (en->alive && en->e.timer != 0xFFFFFFFFu) en->e.timer++;
        (void)in;
    }
}
