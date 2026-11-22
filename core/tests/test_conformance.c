/* SPDX-License-Identifier: Apache-2.0 */
/* Conformance tests for the shared ESP-16 <-> Mosaic formats.
 * T2: SPR2 canonical writer/reader, SPR1 deprecated-read with the
 *     palette-defined-exactly-once invariant. */
#include "mosaic/ms_types.h"
#include "mosaic/ms_render.h"
#include "mosaic/ms_cart.h"
#include "mosaic/ms_build.h"
#include "mosaic/ms_game.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Portable scratch paths: /tmp does not exist on Windows; use %TEMP%. */
static char s_tp[8][260];
static int  s_tpi;
static const char *tpath(const char *name)
{
    char *buf = s_tp[s_tpi++ & 7];
    const char *d = getenv("TEMP");
    if (!d || !*d) d = getenv("TMP");
    if (!d || !*d) d = getenv("TMPDIR");
    if (!d || !*d) d = "/tmp";
    snprintf(buf, sizeof(s_tp[0]), "%s/%s", d, name);
    return buf;
}

static int fails;
#define CHECK(x) do { if (!(x)) { fails++; fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)

/* ---- SPR1 fixture builder (deprecated layout, 6-byte frames) ---- */
static size_t build_spr1(uint8_t *p, const uint16_t *pal, int paln, uint8_t nib)
{
    size_t o = 0;
    ms_wr32(p, MS_SPR1_MAGIC);
    p[4] = 1; p[5] = 4;                 /* version 1, 4bpp */
    ms_wr16(p + 6, (uint16_t)paln);
    ms_wr16(p + 8, 1);                  /* one tile */
    ms_wr16(p + 10, 1);                 /* one frame */
    ms_wr16(p + 12, 0);
    o = 14;
    for (int i = 0; i < paln; i++, o += 2) ms_wr16(p + o, pal[i]);
    for (int i = 0; i < 32; i++) p[o++] = nib;
    ms_wr16(p + o, 0); p[o + 2] = 1; p[o + 3] = 1; p[o + 4] = 0; p[o + 5] = 0;
    o += 6;
    return o;
}

static void test_spr1(void)
{
    static uint8_t buf[512];
    const uint16_t good[] = { 0x0000, 0xF800 };
    size_t n = build_spr1(buf, good, 2, 0x11);
    CHECK(ms_check_sheet(buf, n) == 1);

    /* dead palette entry: index 2 defined but no tile references it */
    const uint16_t dead[] = { 0x0000, 0xF800, 0x001F };
    n = build_spr1(buf, dead, 3, 0x11);
    CHECK(ms_check_sheet(buf, n) == 0);

    /* duplicated palette entry */
    const uint16_t dup[] = { 0x0000, 0xF800, 0xF800 };
    n = build_spr1(buf, dup, 3, 0x11);
    CHECK(ms_check_sheet(buf, n) == 0);

    /* a valid SPR1 still parses through the runtime path */
    n = build_spr1(buf, good, 2, 0x11);
    ms_sheet_t sh;
    CHECK(ms_sheet_parse(&sh, buf, n) == 0);
    CHECK(sh.spr1 == 1 && sh.n_pal == 2 && sh.n_tiles == 1 && sh.n_frames == 1);
    ms_frame_t fr;
    ms_sheet_frame(&sh, 0, &fr);
    CHECK(fr.tile_base == 0 && fr.wt == 1 && fr.ht == 1);
    /* short/garbage blobs are refused, not misparsed */
    CHECK(ms_check_sheet(buf, 10) == 0);
    CHECK(ms_check_sheet(buf, n - 1) == 0);
}

/* ---- SPR2 round-trip through the real writer ---- */
static void test_spr2_roundtrip(void)
{
    enum { W = 32, H = 16 };
    static uint16_t px[W * H];
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        uint16_t c = 0x0000;
        if ((x / 4 + y / 4) % 3 == 0) c = 0xF800;       /* red   */
        else if ((x / 4) % 2) c = 0x07E0;               /* green */
        else c = 0x001F;                                /* blue  */
        px[y * W + x] = c;
    }
    const char *png = tpath("mosaic_spr2_roundtrip.png");
    const char *spr = tpath("mosaic_spr2_roundtrip.spr2");
    CHECK(ms_write_png(png, W, H, px) == 0);
    int rc = ms_convert_sprite(png, spr, 16, 16, 4);
    CHECK(rc == 0);
    if (rc != 0) return;

    FILE *f = fopen(spr, "rb");
    CHECK(f != NULL);
    if (!f) return;
    static uint8_t blob[8192];
    size_t n = fread(blob, 1, sizeof(blob), f);
    fclose(f);

    CHECK(ms_rd32(blob) == MS_SPR_MAGIC);
    CHECK(blob[4] == 2);                    /* canonical version */
    CHECK(blob[5] == 4);
    CHECK(ms_check_sheet(blob, n) == 1);

    ms_sheet_t sh;
    CHECK(ms_sheet_parse(&sh, blob, n) == 0);
    CHECK(sh.spr1 == 0);
    CHECK(sh.n_frames == 2);                /* 32x16 at 16x16 -> 2 frames */
    CHECK(sh.n_tiles == 8);                 /* 2 tiles per frame, contiguous */
    CHECK(sh.pal[0] == 0x0000);             /* index 0 transparent */

    /* every palette entry referenced: rebuild the usage table from tiles */
    uint8_t used[256]; memset(used, 0, sizeof(used));
    for (unsigned t = 0; t < sh.n_tiles; t++) {
        const uint8_t *q = sh.tiles + (size_t)t * 32u;
        for (int k = 0; k < 32; k++) { used[q[k] & 15] = 1; used[q[k] >> 4] = 1; }
    }
    for (unsigned i = 1; i < sh.n_pal; i++) CHECK(used[i] == 1);
    /* no duplicate colours */
    for (unsigned i = 0; i < sh.n_pal; i++)
        for (unsigned j = i + 1; j < sh.n_pal; j++) CHECK(sh.pal[i] != sh.pal[j]);

    /* frame 1 must own the second tile, not alias frame 0's */
    ms_frame_t f0, f1;
    ms_sheet_frame(&sh, 0, &f0);
    ms_sheet_frame(&sh, 1, &f1);
    CHECK(f0.tile_base == 0 && f1.tile_base == 4);
    CHECK(f0.wt == 2 && f0.ht == 2);

    /* a packed-but-unreferenced palette entry is refused */
    static uint8_t bad[8192];
    memcpy(bad, blob, n);
    ms_wr16(bad + 6, (uint16_t)(sh.n_pal + 1));       /* claim an extra entry */
    /* shifting is not needed to prove rejection: size no longer matches */
    CHECK(ms_check_sheet(bad, n) == 0);
}

/* ---- colour-count enforcement ---- */
static void test_spr2_too_many_colours(void)
{
    enum { W = 16, H = 16 };
    static uint16_t px[W * H];
    for (int i = 0; i < W * H; i++)
        px[i] = (uint16_t)(0x8000u | (unsigned)(i * 137u) & 0x7FFFu);  /* 256 distinct */
    const char *png = tpath("mosaic_spr2_many.png");
    const char *spr = tpath("mosaic_spr2_many.spr2");
    CHECK(ms_write_png(png, W, H, px) == 0);
    CHECK(ms_convert_sprite(png, spr, 16, 16, 4) == -7);   /* 4bpp allows 15 colours */
}


/* ================= T1: cart16 v2 reader ================= */
typedef struct { const char *name; uint32_t kind; const void *data; uint32_t size; } fix_t;

/* Lay out a cart exactly as the reference packer does: 20-byte header,
 * TOC immediately after, payloads contiguous in TOC order. */
static size_t pack_like_reference(uint8_t *out, const fix_t *a, int n)
{
    const size_t toc = MS_CART_HDR;
    size_t off = MS_CART_HDR + (size_t)n * MS_CART_TOC_ENTRY;
    for (int i = 0; i < n; i++) {
        memcpy(out + off, a[i].data, a[i].size);
        uint8_t *e = out + toc + (size_t)i * MS_CART_TOC_ENTRY;
        memset(e, 0, MS_CART_TOC_ENTRY);
        memcpy(e, a[i].name, strlen(a[i].name));
        ms_wr32(e + 32, a[i].kind);
        ms_wr32(e + 36, (uint32_t)off);
        ms_wr32(e + 40, a[i].size);
        ms_wr32(e + 44, ms_crc32(a[i].data, a[i].size));
        off += a[i].size;
    }
    memset(out, 0, MS_CART_HDR);
    ms_wr32(out, MS_CART_MAGIC);
    ms_wr16(out + 4, MS_CART_VERSION);
    ms_wr16(out + 6, 0);
    ms_wr32(out + 8, (uint32_t)n);
    ms_wr32(out + 12, MS_CART_HDR);
    ms_wr32(out + 16, ms_crc32(out + toc, (size_t)n * MS_CART_TOC_ENTRY));
    return off;
}
static void fix_toc_crc(uint8_t *b, int n)
{
    ms_wr32(b + 16, ms_crc32(b + MS_CART_HDR, (size_t)n * MS_CART_TOC_ENTRY));
}

static const uint8_t k_midi[] = {
    'M','T','h','d',0,0,0,6,0,0,0,1,0x01,0xE0,
    'M','T','r','k',0,0,0,4,0x00,0xFF,0x2F,0x00
};

static size_t make_assets(uint8_t *tiles, uint8_t *map, uint8_t *pal, uint8_t *bank)
{
    tiles[0] = 4; ms_wr16(tiles + 1, 2);
    memset(tiles + 3, 0x11, 64);
    ms_wr16(map, 4);
    for (int i = 0; i < 4; i++) ms_wr16(map + 2 + i * 2, (uint16_t)i);
    ms_wr16(pal, 2); ms_wr16(pal + 2, 0x0000); ms_wr16(pal + 4, 0xF800);
    memset(bank, 0, 16);
    ms_wr32(bank, MS_BANK_MAGIC); ms_wr16(bank + 4, 1); ms_wr32(bank + 12, MS_AUDIO_HZ);
    return 3 + 64;   /* tile payload size */
}

static void test_cart16_reference(void)
{
    static uint8_t blob[65536];
    uint8_t tiles[3 + 64], map[2 + 8], pal[2 + 4], bank[16];
    const size_t tsz = make_assets(tiles, map, pal, bank);
    fix_t a[] = {
        { "bg_tiles", MS_A_BG_TILES, tiles, (uint32_t)tsz },
        { "bg_map",   MS_A_BG_MAP,   map,   sizeof(map) },
        { "pal",      MS_A_PALETTE,  pal,   sizeof(pal) },
        { "song",     MS_A_MIDI,     k_midi, (uint32_t)sizeof(k_midi) },
        { "bank",     MS_A_BANK,     bank,  sizeof(bank) },
    };
    const size_t n = pack_like_reference(blob, a, 5);
    ms_cart_t c;
    CHECK(ms_cart_mount(&c, blob, n) == MS_CART_OK);
    CHECK(c.version == 2 && c.toc_off == MS_CART_HDR && c.count == 5);
    ms_asset_t out;
    CHECK(ms_cart_find(&c, "bg_map", &out) == MS_CART_OK && out.kind == MS_A_BG_MAP);
    CHECK(ms_cart_find(&c, "song", &out) == MS_CART_OK && out.kind == MS_A_MIDI);
    CHECK(ms_cart_find(&c, "nope", &out) != MS_CART_OK);

    /* payload corruption must fail on CRC */
    blob[n - 1] ^= 0xFF;
    CHECK(ms_cart_mount(&c, blob, n) == MS_CART_E_CRC);
    blob[n - 1] ^= 0xFF;
    CHECK(ms_cart_mount(&c, blob, n) == MS_CART_OK);

    /* truncation */
    CHECK(ms_cart_mount(&c, blob, n - 1) == MS_CART_E_SIZE);
    CHECK(ms_cart_mount(&c, blob, 10) == MS_CART_E_SIZE);
}

static void test_cart16_rules(void)
{
    static uint8_t blob[65536];
    uint8_t tiles[3 + 64], map[2 + 8], pal[2 + 4], bank[16];
    const size_t tsz = make_assets(tiles, map, pal, bank);
    fix_t a[] = {
        { "bg_tiles", MS_A_BG_TILES, tiles, (uint32_t)tsz },
        { "bg_map",   MS_A_BG_MAP,   map,   sizeof(map) },
    };
    const size_t n = pack_like_reference(blob, a, 2);
    ms_cart_t c;
    CHECK(ms_cart_mount(&c, blob, n) == MS_CART_OK);

    /* wrong version */
    ms_wr16(blob + 4, 1);
    CHECK(ms_cart_mount(&c, blob, n) == MS_CART_E_MAGIC);
    ms_wr16(blob + 4, MS_CART_VERSION);

    /* toc_off must be 20 */
    ms_wr32(blob + 12, 24);
    CHECK(ms_cart_mount(&c, blob, n) == MS_CART_E_LAYOUT);
    ms_wr32(blob + 12, MS_CART_HDR);

    /* TOC CRC is mandatory */
    blob[MS_CART_HDR] ^= 0x01;
    CHECK(ms_cart_mount(&c, blob, n) == MS_CART_E_CRC);
    blob[MS_CART_HDR] ^= 0x01;
    CHECK(ms_cart_mount(&c, blob, n) == MS_CART_OK);

    /* duplicate names are refused at mount, after the TOC CRC is fixed */
    memcpy(blob + MS_CART_HDR + MS_CART_TOC_ENTRY, "bg_tiles", 9);
    fix_toc_crc(blob, 2);
    CHECK(ms_cart_mount(&c, blob, n) == MS_CART_E_DUP);
    memcpy(blob + MS_CART_HDR + MS_CART_TOC_ENTRY, "bg_map\0\0", 8);
    fix_toc_crc(blob, 2);

    /* unknown kind */
    ms_wr32(blob + MS_CART_HDR + 32, 11);
    fix_toc_crc(blob, 2);
    CHECK(ms_cart_mount(&c, blob, n) == MS_CART_E_RECORD);
    ms_wr32(blob + MS_CART_HDR + 32, MS_A_BG_TILES);
    fix_toc_crc(blob, 2);

    /* extension kind without the EXT flag, then with it */
    ms_wr32(blob + MS_CART_HDR + MS_CART_TOC_ENTRY + 32, MS_A_PROGRAM);
    fix_toc_crc(blob, 2);
    CHECK(ms_cart_mount(&c, blob, n) == MS_CART_E_RECORD);
    ms_wr16(blob + 6, MS_CART_F_EXT);
    /* still E_ASSET: the payload is a tile blob, not a program */
    CHECK(ms_cart_mount(&c, blob, n) == MS_CART_E_ASSET);
    ms_wr16(blob + 6, 0);
    ms_wr32(blob + MS_CART_HDR + MS_CART_TOC_ENTRY + 32, MS_A_BG_MAP);
    fix_toc_crc(blob, 2);

    /* budget is an exact byte count */
    CHECK(ms_cart_mount_budget(&c, blob, n, n - 1) == MS_CART_E_BUDGET);
    CHECK(ms_cart_mount_budget(&c, blob, n, n) == MS_CART_OK);

    /* a map whose entry count is not a power of two fails its validator */
    ms_wr16(map, 3);
    const size_t bad = pack_like_reference(blob, a, 2);
    CHECK(ms_cart_mount(&c, blob, bad) == MS_CART_E_ASSET);
    ms_wr16(map, 4);
}

/* ================= T3: cross-mount through Mosaic's packer ========= */
static void test_crossmount(void)
{
    /* one SPR2 sheet (written by Mosaic), bank, SMF, tiles+map+palette */
    enum { W = 16, H = 16 };
    static uint16_t px[W * H];
    for (int i = 0; i < W * H; i++) px[i] = (uint16_t)((i % 3 == 0) ? 0xF800 : (i % 3 == 1) ? 0x07E0 : 0x001F);
    const char *xm_png = tpath("mosaic_xm.png");
    const char *xm_spr = tpath("mosaic_xm.spr2");
    CHECK(ms_write_png(xm_png, W, H, px) == 0);
    CHECK(ms_convert_sprite(xm_png, xm_spr, 16, 16, 4) == 0);

    static uint8_t spr[4096];
    FILE *f = fopen(xm_spr, "rb");
    CHECK(f != NULL);
    if (!f) return;
    const size_t sprn = fread(spr, 1, sizeof(spr), f);
    fclose(f);

    uint8_t tiles[3 + 64], map[2 + 8], pal[2 + 4], bank[16];
    const size_t tsz = make_assets(tiles, map, pal, bank);
    static uint8_t buf[8192];
    ms_cart_writer_t w;
    ms_cart_writer_begin(&w, buf, sizeof(buf));
    CHECK(ms_cart_add(&w, "hero", MS_A_SPRITES, spr, sprn) == MS_CART_OK);
    CHECK(ms_cart_add(&w, "bank", MS_A_BANK, bank, sizeof(bank)) == MS_CART_OK);
    CHECK(ms_cart_add(&w, "theme", MS_A_MIDI, k_midi, sizeof(k_midi)) == MS_CART_OK);
    CHECK(ms_cart_add(&w, "bg_tiles", MS_A_BG_TILES, tiles, tsz) == MS_CART_OK);
    CHECK(ms_cart_add(&w, "bg_map", MS_A_BG_MAP, map, sizeof(map)) == MS_CART_OK);
    CHECK(ms_cart_add(&w, "pal", MS_A_PALETTE, pal, sizeof(pal)) == MS_CART_OK);
    const int len = ms_cart_finish(&w);
    CHECK(len > 0);
    if (len <= 0) return;

    /* layout must be the canonical one: header 20, TOC, contiguous payloads */
    CHECK(ms_rd32(buf) == MS_CART_MAGIC);
    CHECK(ms_rd16(buf + 4) == 2 && ms_rd16(buf + 6) == 0);
    CHECK(ms_rd32(buf + 8) == 6 && ms_rd32(buf + 12) == MS_CART_HDR);
    CHECK(ms_rd32(buf + MS_CART_HDR + 36) == MS_CART_HDR + 6 * MS_CART_TOC_ENTRY);

    ms_cart_t c;
    CHECK(ms_cart_mount(&c, buf, (size_t)len) == MS_CART_OK);
    ms_asset_t a;
    CHECK(ms_cart_find(&c, "hero", &a) == MS_CART_OK && a.kind == MS_A_SPRITES);
    ms_sheet_t sh;
    CHECK(ms_sheet_parse(&sh, a.data, a.size) == 0 && sh.n_frames == 1);
    CHECK(ms_cart_find_kind(&c, MS_A_MIDI, &a) == MS_CART_OK);
    CHECK(ms_cart_find_kind(&c, MS_A_BANK, &a) == MS_CART_OK);
    CHECK(ms_cart_find_kind(&c, MS_A_BG_MAP, &a) == MS_CART_OK);
    CHECK(c.flags == 0);        /* canonical: no extension kinds */
}


/* ====== spec-rev 2.1: dims inside bg_map, bank inside palette ======== */
static void test_payload_v21(void)
{
    static uint8_t blob[65536];
    uint8_t tiles[3 + 64];
    tiles[0] = 4; ms_wr16(tiles + 1, 2); memset(tiles + 3, 0x11, 64);

    /* logical 3x2 tiles -> padded 4x2 = 8 entries */
    uint8_t map[6 + 16];
    ms_wr16(map, 8); ms_wr16(map + 2, 3); ms_wr16(map + 4, 2);
    for (int i = 0; i < 8; i++) ms_wr16(map + 6 + i * 2, 0);

    uint8_t pal[4 + 4];                        /* bank 1 = sprites */
    ms_wr16(pal, 2); pal[2] = 1; pal[3] = 0;
    ms_wr16(pal + 4, 0x0000); ms_wr16(pal + 6, 0xF800);

    fix_t a[] = {
        { "bg_tiles", MS_A_BG_TILES, tiles, (uint32_t)sizeof(tiles) },
        { "bg_map",   MS_A_BG_MAP,   map,   sizeof(map) },
        { "pal",      MS_A_PALETTE,  pal,   sizeof(pal) },
    };
    size_t n = pack_like_reference(blob, a, 3);
    ms_cart_t c;
    CHECK(ms_cart_mount(&c, blob, n) == MS_CART_OK);
    ms_asset_t out;
    CHECK(ms_cart_find(&c, "bg_map", &out) == MS_CART_OK);
    const uint8_t *md2 = (const uint8_t *)out.data;
    CHECK(ms_rd16(md2 + 2) == 3 && ms_rd16(md2 + 4) == 2);
    CHECK(ms_cart_find(&c, "pal", &out) == MS_CART_OK);
    const uint8_t *pd2 = (const uint8_t *)out.data;
    CHECK(pd2[2] == 1);

    /* the padded count must agree with the declared logical dims */
    ms_wr16(map, 4);
    n = pack_like_reference(blob, a, 3);
    CHECK(ms_cart_mount(&c, blob, n) == MS_CART_E_ASSET);
    ms_wr16(map, 8);

    /* only banks 0 and 1 exist */
    pal[2] = 2;
    n = pack_like_reference(blob, a, 3);
    CHECK(ms_cart_mount(&c, blob, n) == MS_CART_E_ASSET);
    pal[2] = 0;
    n = pack_like_reference(blob, a, 3);
    CHECK(ms_cart_mount(&c, blob, n) == MS_CART_OK);

    /* the v2.0 forms still mount unchanged */
    uint8_t lmap[2 + 8], lpal[2 + 4];
    ms_wr16(lmap, 4);
    for (int i = 0; i < 4; i++) ms_wr16(lmap + 2 + i * 2, 0);
    ms_wr16(lpal, 2); ms_wr16(lpal + 2, 0x0000); ms_wr16(lpal + 4, 0x001F);
    fix_t b[] = {
        { "bg_tiles", MS_A_BG_TILES, tiles, (uint32_t)sizeof(tiles) },
        { "bg_map",   MS_A_BG_MAP,   lmap,  sizeof(lmap) },
        { "pal",      MS_A_PALETTE,  lpal,  sizeof(lpal) },
    };
    n = pack_like_reference(blob, b, 3);
    CHECK(ms_cart_mount(&c, blob, n) == MS_CART_OK);
}

/* ====== affine raster stepping + colour math ======== */
static void test_render_math(void)
{
    CHECK(ms_color_math(0x0000, 0xF800, 0, MS_FIX_ONE) == 0xF800);   /* add */
    CHECK(ms_color_math(0xF800, 0x0000, 1, MS_FIX_ONE) == 0x0000);   /* sub clamps at 0 */
    CHECK(ms_color_math(0x07E0, 0xF800, 2, MS_FIX_ONE) == 0x7BE0);   /* add-half */
    CHECK(ms_color_math(0xFFFF, 0x1234, 0, 0) == 0x1234);            /* factor 0 = off */

    static uint16_t fb[32 * 16];
    static int16_t rowshift[16];
    static uint8_t tiles[4 * 32];
    static uint16_t map[4];
    memset(tiles, 0x11, sizeof(tiles));          /* every pixel = index 1 */
    map[0] = 0x0001; map[1] = 0x0401; map[2] = 0x0801; map[3] = 0x0C01;

    ms_ppu_t ppu;
    ms_ppu_reset(&ppu, MS_MODE_AFFINE, 32, 16);
    ppu.bg[0].tiles = tiles; ppu.bg[0].map = map;
    ppu.bg[0].map_w = 4; ppu.bg[0].map_h = 1;
    ppu.bg[0].logical_w = 4; ppu.bg[0].logical_h = 1;
    ppu.bg[0].enabled = 1; ppu.bg[0].bpp = 4; ppu.bg[0].priority = 2;
    ppu.mat[0].a = MS_FIX_ONE; ppu.mat[0].d = MS_FIX_ONE;
    ppu.pal_bg[1] = 0xF800; ppu.pal_bg[17] = 0x07E0;
    ppu.pal_bg[33] = 0x001F; ppu.pal_bg[49] = 0xFFFF;
    for (int i = 0; i < 16; i++) rowshift[i] = (i >= 8) ? 16 : 0;
    ppu.bg[0].line_scroll_x = rowshift;

    ms_ppu_render(&ppu, fb);
    CHECK(fb[0 * 32 + 16] == 0xF800);    /* identity: x=16 samples tile 0 */
    CHECK(fb[0 * 32 + 24] == 0x07E0);    /* tile 1 via palette slot 1 */
    CHECK(fb[8 * 32 + 16] == 0x001F);    /* raster table shifts the origin */
    CHECK(fb[8 * 32 + 8]  == 0x07E0);

    /* colour math blends the layer with what is underneath it */
    ms_ppu_reset(&ppu, MS_MODE_AFFINE, 32, 16);
    ppu.bg[0].tiles = tiles; ppu.bg[0].map = map;
    ppu.bg[0].map_w = 4; ppu.bg[0].map_h = 1;
    ppu.bg[0].logical_w = 4; ppu.bg[0].logical_h = 1;
    ppu.bg[0].enabled = 1; ppu.bg[0].bpp = 4; ppu.bg[0].priority = 2;
    ppu.bg[0].math = 1;
    ppu.mat[0].a = MS_FIX_ONE; ppu.mat[0].d = MS_FIX_ONE;
    ppu.pal_bg[1] = 0xF800;
    ppu.fx.backdrop = 0x07E0;
    ppu.fx.math_on = 1; ppu.fx.color_math = 2; ppu.fx.math_factor = MS_FIX_ONE;
    ms_ppu_render(&ppu, fb);
    CHECK(fb[0 * 32 + 16] == 0x7BE0);    /* add-half(red over green) */
    ppu.fx.math_on = 0;
    ms_ppu_render(&ppu, fb);
    CHECK(fb[0 * 32 + 16] == 0xF800);    /* math off: plain layer colour */
}

int main(void)
{
    test_payload_v21();
    test_render_math();
    test_spr1();
    test_spr2_roundtrip();
    test_spr2_too_many_colours();
    test_cart16_reference();
    test_cart16_rules();
    test_crossmount();
    if (fails) { fprintf(stderr, "%d conformance failure(s)\n", fails); return 1; }
    printf("all conformance tests passed\n");
    return 0;
}
