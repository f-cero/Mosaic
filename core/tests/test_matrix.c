/* SPDX-License-Identifier: Apache-2.0 */
/* Mosaic — sprite matrix tests: the matrix maths, and the rendered result of
 * an object matrix and a per-scanline table, asserted on exact palette
 * indices rather than on "looks about right". */
#include "mosaic/ms_render.h"
#include "mosaic/ms_vm.h"
#include "mosaic/ms_cart.h"
#include "mosaic/ms_game.h"
#include <string.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { \
    printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

#define W 320
#define H 240
static uint16_t fb[W * H];

/* One 8x8 4bpp tile whose pixel column j uses palette index j+1, so the
 * sampled source column is readable straight off the rendered colour. */
static uint8_t tilebuf[32];
static ms_matrix_t linebuf[8];

static uint16_t px(int x, int y) { return fb[(size_t)y * W + x]; }
static uint16_t pal(int idx) { return (uint16_t)(0x0800u + (unsigned)idx * 0x0100u); }

static void setup(ms_ppu_t *ppu, const ms_matrix_t *obj, const ms_matrix_t *lines, int size)
{
    ms_ppu_reset(ppu, MS_MODE_TEXT, W, H);
    ppu->fx.backdrop = 0;
    ppu->fx.brightness = 16;
    ppu->spr_tiles = tilebuf;
    for (unsigned i = 1; i <= 8; i++) ppu->pal_spr[i] = pal((int)i);
    ms_oam_t *o = &ppu->oam[0];
    o->x = 20; o->y = 10; o->w = (uint8_t)size; o->h = (uint8_t)size;
    o->bpp = 4; o->pal_slot = 0; o->priority = 0; o->flags = 0;
    o->affine = -1; o->line_mat = NULL; o->tiles = tilebuf;
    if (obj) { ppu->mat[0] = *obj; o->affine = 0; }
    if (lines) o->line_mat = lines;
    ppu->oam_count = 1;
    ms_ppu_render(ppu, fb);
}

static int drawn(void)
{
    int n = 0;
    for (size_t i = 0; i < (size_t)W * H; i++) if (fb[i]) n++;
    return n;
}


/* ---- VM: SETMAT writes a shared slot, AFFINE/LINETAB select per entity ---- */
static void vm_matrix_test(void)
{
    static const uint8_t code[] = {
        0x02, 0x01, 0x00,0x00,0x01,0x00,   /* LDI32 r1, 0x00010000 */
        0x02, 0x02, 0x00,0x00,0xFF,0xFF,   /* LDI32 r2, 0xFFFF0000 */
        0x02, 0x03, 0x00,0x80,0x00,0x00,   /* LDI32 r3, 0x00008000 */
        0x02, 0x04, 0x00,0x00,0x00,0x00,   /* LDI32 r4, 0 */
        MSOP_SETMAT, 2, 1, 2, 3, 4,
        MSOP_AFFINE, 0, 2, 0,
        MSOP_LINETAB, 0, 5, 0,
        MSOP_HALT, 0, 0, 0
    };
    uint8_t buf[160];
    ms_program_t prog;
    ms_ppu_t ppu;
    ms_game_t game;
    ms_vm_t vm;
    const size_t off = 32 + sizeof code;

    memset(buf, 0, sizeof buf);
    ms_wr32(buf + 0, MS_PROG_MAGIC);
    ms_wr32(buf + 4, 1);
    ms_wr32(buf + 8, (uint32_t)sizeof code);
    ms_wr32(buf + 12, (uint32_t)off);
    ms_wr16(buf + 16, 1);
    ms_wr16(buf + 18, 0xFFFF);          /* no start_pc */
    ms_wr16(buf + 20, 0);               /* update_pc = 0 */
    memcpy(buf + 32, code, sizeof code);
    ms_wr16(buf + off + 0, 0xFFFF);
    ms_wr16(buf + off + 2, 0);

    CHECK(ms_program_parse(&prog, buf, sizeof buf) == 0, "program parse failed");
    ms_ppu_reset(&ppu, MS_MODE_RICH, 320, 240);
    memset(&game, 0, sizeof game);
    game.ppu = &ppu;
    CHECK(ms_vm_init(&vm, &prog, &game) == 0, "vm init failed");
    ms_vm_update(&vm, NULL);
    CHECK(!vm.error, "vm error: %s", vm.errmsg);

    CHECK(ppu.mat[2].a == MS_FIX_ONE, "SETMAT a = %d", (int)ppu.mat[2].a);
    CHECK(ppu.mat[2].b == (ms_fix)-MS_FIX_ONE, "SETMAT b = %d", (int)ppu.mat[2].b);
    CHECK(ppu.mat[2].c == MS_FIX_ONE / 2, "SETMAT c = %d", (int)ppu.mat[2].c);
    CHECK(ppu.mat[2].d == 0, "SETMAT d = %d", (int)ppu.mat[2].d);
    CHECK(ppu.mat[0].a == 0, "SETMAT wrote a slot it was not told to");
    CHECK(vm.ents[0].e.affine == 2, "AFFINE = %d, want 2", vm.ents[0].e.affine);
    CHECK(vm.ents[0].e.line_tab == 5, "LINETAB = %d, want 5", vm.ents[0].e.line_tab);
}

/* ---- cart kind 12: validator, writer, mount ---- */
static void cart_matrices_test(void)
{
    static const uint8_t payload[] = {
        2, 0, 0, 0,           /* two tables */
        2, 0, 3, 0,           /* rows: 2 + 3 = 5 */
        /* 5 x {a,b,c,d} — values only have to round-trip */
        0x00,0x00,0x01,0x00, 0,0,0,0, 0,0,0,0, 0x00,0x00,0x02,0x00,
        0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
        0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
        0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
        0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0
    };
    uint8_t cart[8192];   /* writer needs its 3 KB staging area */
    ms_cart_writer_t w;
    ms_cart_t c;
    ms_asset_t a;

    CHECK(ms_check_matrices(payload, sizeof payload) == 1, "validator rejected a good table set");
    CHECK(ms_check_matrices(payload, sizeof payload - 1) == 0, "validator took a truncated payload");
    CHECK(ms_check_matrices(payload, sizeof payload + 1) == 0, "validator took trailing junk");

    ms_cart_writer_begin(&w, cart, sizeof cart);
    const int arc = ms_cart_add(&w, "warp", MS_A_MATRICES, payload, sizeof payload);
    CHECK(arc == MS_CART_OK, "writer refused kind 12: %d (%s)", arc, ms_cart_err_str(arc));
    const int n = ms_cart_finish(&w);
    CHECK(n > 0, "cart finish failed");
    CHECK(ms_cart_mount(&c, cart, (size_t)n) == MS_CART_OK, "mount rejected kind 12");
    CHECK(ms_cart_find_kind(&c, MS_A_MATRICES, &a) == MS_CART_OK, "find_kind 12 failed");
    CHECK(a.size == sizeof payload, "payload size %u", (unsigned)a.size);
}

int main(void)
{
    vm_matrix_test();
    cart_matrices_test();
    /* ---- matrix maths ---- */
    ms_matrix_t id = ms_matrix_identity();
    CHECK(id.a == MS_FIX_ONE && id.b == 0 && id.c == 0 && id.d == MS_FIX_ONE,
          "identity is not the unit matrix");

    ms_matrix_t s2 = ms_matrix_scale(MS_FIX(2), MS_FIX(2));
    CHECK(s2.a == 2 * MS_FIX_ONE && s2.d == 2 * MS_FIX_ONE, "scale not 2x");

    /* rhs first: scale then rotate a quarter turn takes (1,0) to (0,2). */
    ms_matrix_t comp = ms_matrix_mul(ms_matrix_rot(MS_FIX_ONE / 4), s2);
    int64_t vx = ((int64_t)comp.a * MS_FIX_ONE + (int64_t)comp.b * 0) >> 16;
    int64_t vy = ((int64_t)comp.c * MS_FIX_ONE + (int64_t)comp.d * 0) >> 16;
    CHECK(vx >= -64 && vx <= 64, "rotate(scale(1,0)).x = %lld, want 0", (long long)vx);
    CHECK(vy >= 2 * MS_FIX_ONE - 64 && vy <= 2 * MS_FIX_ONE + 64,
          "rotate(scale(1,0)).y = %lld, want 2 units", (long long)vy);
    CHECK(comp.a > -64 && comp.a < 64, "rot90 .a = %d, want 0", comp.a);
    CHECK(comp.d > -64 && comp.d < 64, "rot90 .d = %d, want 0", comp.d);

    /* a shear must not change the other axis' basis vector */
    ms_matrix_t sh = ms_matrix_shear(MS_FIX_ONE / 2, 0);
    CHECK(sh.a == MS_FIX_ONE && sh.d == MS_FIX_ONE, "shear disturbed the diagonal");

    /* ---- pixel ramp: one nibble per column, index = column + 1 ---- */
    memset(tilebuf, 0, sizeof tilebuf);
    for (int row = 0; row < 8; row++)
        for (int j = 0; j < 8; j++)
            tilebuf[row * 4 + (j >> 1)] |= (uint8_t)(((j & 1) ? ((j + 1) << 4) : (j + 1)));

    /* identity: screen column maps to the same source column */
    ms_ppu_t ppu;
    setup(&ppu, NULL, NULL, 8);
    for (int x = 0; x < 8; x++)
        CHECK(px(20 + x, 10) == pal(x + 1),
              "identity x=%d gave %04x, want %04x", x, px(20 + x, 10), pal(x + 1));
    CHECK(drawn() == 64, "identity drew %d pixels, want 64", drawn());

    /* half scale about the centre: src = (x - 4)/2 + 4 */
    ms_matrix_t half = ms_matrix_scale(MS_FIX_ONE / 2, MS_FIX_ONE / 2);
    setup(&ppu, &half, NULL, 8);
    {
        static const int expect[8] = { 2, 2, 3, 3, 4, 4, 5, 5 };
        for (int x = 0; x < 8; x++)
            CHECK(px(20 + x, 10) == pal(expect[x] + 1),
                  "scale 0.5 x=%d gave %04x, want source column %d",
                  x, px(20 + x, 10), expect[x]);
    }

    /* per-scanline shear: b = (row - 4) * 0.25 in sampling space */
    for (int row = 0; row < 8; row++) {
        linebuf[row] = ms_matrix_identity();
        linebuf[row].b = (ms_fix)((row - 4) * (MS_FIX_ONE / 4));
    }
    setup(&ppu, NULL, linebuf, 8);
    /* row 7: cy = 3, so src = (x - 4) + 0.75*3 + 4 = x + 2.25 */
    CHECK(px(24, 17) == pal(7),
          "shear row 7 x=4 gave %04x, want source column 6 (%04x)", px(24, 17), pal(7));
    /* row 0: cy = -4, b = -1, so src = (x - 4) - 4 + 4 -> x - 4, mostly clipped */
    CHECK(px(20, 10) == pal(5),
          "shear row 0 x=0 gave %04x, want source column 4 (%04x)", px(20, 10), pal(5));

    /* a quarter turn keeps the box inside itself up to the half-pixel edge */
    ms_matrix_t rot = ms_matrix_rot(MS_FIX_ONE / 4);
    setup(&ppu, &rot, NULL, 8);
    {
        const int n = drawn();
        CHECK(n >= 36 && n <= 64, "quarter turn drew %d of 64 pixels", n);
        printf("quarter turn: %d of 64 pixels drawn\n", n);
    }

    /* an out-of-range matrix index is ignored, not dereferenced */
    setup(&ppu, NULL, NULL, 8);
    ppu.oam[0].affine = 99;
    ms_ppu_render(&ppu, fb);
    CHECK(drawn() == 64, "out-of-range affine index broke the sprite");

    printf("sprite matrix: %s (%d failures)\n", fails ? "FAIL" : "ok", fails);
    return fails ? 1 : 0;
}
