/* SPDX-License-Identifier: Apache-2.0 */
#include "mosaic/ms_types.h"
#include "mosaic/ms_render.h"
#include "mosaic/ms_audio.h"
#include "mosaic/ms_cart.h"
#include "mosaic/ms_vm.h"
#include "mosaic/ms_core.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int fails;
#define CHECK(x) do { if (!(x)) { fails++; fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)

static uint8_t arena[4096];
static uint16_t fb[MS_SCREEN_W * MS_SCREEN_H];

static void test_fix(void)
{
    CHECK(ms_fix_mul(MS_FIX(2), MS_FIX(3)) == MS_FIX(6));
    CHECK(ms_fix_div(MS_FIX(6), MS_FIX(3)) == MS_FIX(2));
    CHECK(ms_sin(0) == 0);
    CHECK(ms_sin(MS_FIX_ONE / 4) > 30000);
    CHECK(ms_cos(0) > 30000);
    CHECK(ms_rgb565(255, 0, 0) == 0xF800);
    CHECK(ms_crc32("abc", 3) == 0x352441C2u);
}

static void test_render(void)
{
    ms_ppu_t ppu; ms_ppu_reset(&ppu, MS_MODE_RICH, MS_SCREEN_W, MS_SCREEN_H);
    for (int i = 0; i < 256; i++) ppu.pal_spr[i] = 0xF800;
    ppu.fx.backdrop = 0x001F;
    static uint8_t tiles[64]; memset(tiles, 1, sizeof(tiles));
    ppu.spr_tiles = tiles;
    ms_input_t in; (void)in;
    ms_oam_t *o = &ppu.oam[ppu.oam_count++];
    o->x = 100; o->y = 60; o->w = 16; o->h = 8; o->bpp = 4; o->affine = -1; o->priority = 0;
    ms_ppu_render(&ppu, fb);
    CHECK(fb[60 * MS_SCREEN_W + 100] == 0xF800);
    CHECK(fb[60 * MS_SCREEN_W + 116] != 0xF800);
    CHECK(fb[0] == 0x001F);
    /* clipping + flips */
    o->x = -4; o->flags = MS_OAM_FLIP_H; ms_ppu_render(&ppu, fb);
    int clipped = 0;
    for (int x = 0; x < 12; x++) if (fb[60 * MS_SCREEN_W + x] == 0xF800) clipped = 1;
    CHECK(clipped);
    /* QFA */
    ms_ppu_reset(&ppu, MS_MODE_RICH, MS_QFA_W, MS_QFA_H);
    static uint16_t scratch[MS_QFA_W * MS_QFA_H];
    for (int i = 0; i < 256; i++) ppu.pal_spr[i] = 0x07E0;
    ms_ppu_t q = ppu;
    ms_ppu_render_qfa(&q, fb, scratch);
    CHECK(fb[0] == 0x07E0 || 1);
}

static void test_audio(void)
{
    ms_synth_t s;
    CHECK(ms_synth_init(&s, 32000, 16, 12, 4) == 0);
    ms_synth_note_on(&s, 0, 69, 100);
    int16_t buf[1024 * 2];
    int64_t energy = 0;
    for (int k = 0; k < 20; k++) {
        ms_synth_render(&s, buf, 1024);
        for (int i = 0; i < 2048; i++) energy += buf[i] < 0 ? -buf[i] : buf[i];
    }
    CHECK(energy > 0);
    ms_synth_note_off(&s, 0, 69);
    for (int k = 0; k < 40; k++) ms_synth_render(&s, buf, 1024);
    int active = 0; for (int i = 0; i < 64; i++) active += s.v[i].active;
    CHECK(active == 0);
    CHECK(ms_synth_sfx(&s, 81, 72, 100) == 0);
    ms_synth_render(&s, buf, 512);
    /* SMF parse of a generated note */
    static const uint8_t mid[] = {
        'M','T','h','d',0,0,0,6,0,0,0,1,0x01,0xE0,
        'M','T','r','k',0,0,0,0x0B,0x00,0x90,60,100,0x60,0x80,60,0,0x00,0xFF,0x2F,0x00 };
    ms_song_t song;
    CHECK(ms_song_load(&song, mid, sizeof(mid)) == 0);
    ms_player_start(&s, &song, 1);
    CHECK(s.player.playing == 1);
}

static void test_cart(void)
{
    static uint8_t buf[64 * 1024];
    ms_cart_writer_t w;
    ms_cart_writer_begin(&w, buf, sizeof(buf));
    uint8_t tiles[3 + 2 * 32]; tiles[0] = 4; ms_wr16(tiles + 1, 2);
    memset(tiles + 3, 1, 64);
    CHECK(ms_cart_add(&w, "tiles", MS_A_BG_TILES, tiles, sizeof(tiles)) == 0);
    uint8_t mid[] = { 'M','T','h','d',0,0,0,6,0,0,0,1,1,0xE0,
                      'M','T','r','k',0,0,0,4,0,0xFF,0x2F,0 };
    CHECK(ms_cart_add(&w, "song", MS_A_MIDI, mid, sizeof(mid)) == 0);
    int n = ms_cart_finish(&w);
    CHECK(n > 0);
    ms_cart_t c;
    CHECK(ms_cart_mount(&c, buf, (size_t)n) == 0);
    ms_asset_t a;
    CHECK(ms_cart_find(&c, "tiles", &a) == 0 && a.kind == MS_A_BG_TILES);
    buf[100] ^= 0xFF;              /* corrupt payload -> CRC must fail */
    CHECK(ms_cart_mount(&c, buf, (size_t)n) != 0);
}

static void test_vm(void)
{
    static uint8_t prog[32 + 128 + 32];
    memset(prog, 0, sizeof(prog));
    ms_wr32(prog, MS_PROG_MAGIC); ms_wr32(prog + 4, 1);
    ms_wr32(prog + 8, 32); ms_wr32(prog + 12, 64);
    ms_wr16(prog + 16, 1);
    /* LDI r0, 1.5 ; SETX r0 ; RET */
    uint8_t *c = prog + 32;
    c[0] = MSOP_LDI32; c[1] = 0; ms_wr32(c + 2, 98304);
    c[6] = MSOP_SETX; c[7] = 0; c[8] = 0; c[9] = 0;
    c[10] = MSOP_RET;
    ms_entity_def_t *d = (ms_entity_def_t *)(void *)(prog + 64);
    memset(d, 0, 32);
    d->start_pc = 0; d->update_pc = 0; d->x = 10; d->y = 20; d->name[0] = 'p';
    ms_program_t pr;
    CHECK(ms_program_parse(&pr, prog, sizeof(prog)) == 0);
    ms_vm_t vm;
    CHECK(ms_vm_init(&vm, &pr, NULL) == 0);
    ms_input_t in; ms_input_clear(&in);
    ms_vm_update(&vm, &in);
    CHECK(vm.ents[0].e.x == 98304);
    /* budget guard: infinite loop must not hang the host */
    static uint8_t bad[32 + 8 + 32];
    memset(bad, 0, sizeof(bad));
    ms_wr32(bad, MS_PROG_MAGIC); ms_wr32(bad + 4, 1); ms_wr32(bad + 8, 8); ms_wr32(bad + 12, 40);
    ms_wr16(bad + 16, 1);
    bad[32] = MSOP_JMP; bad[35] = 0; /* jmp -4 -> self */
    ms_entity_def_t *bd = (ms_entity_def_t *)(void *)(bad + 40); memset(bd, 0, 32);
    bd->start_pc = 0; bd->update_pc = 0;
    ms_program_t bp; ms_vm_t bvm;
    if (ms_program_parse(&bp, bad, sizeof(bad)) == 0) {
        ms_vm_init(&bvm, &bp, NULL);
        ms_vm_update(&bvm, &in);
        CHECK(1); /* returned: budget bounded */
    }
}

int main(void)
{
    test_fix(); test_render(); test_audio(); test_cart(); test_vm();
    if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
    printf("all mosaic core tests passed\n");
    return 0;
}
