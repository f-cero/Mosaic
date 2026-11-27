/* SPDX-License-Identifier: Apache-2.0 — MVID codec + cart tests. */
#include "mvid.h"
#include "mosaic/ms_cart.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL %s (line %d)\n", msg, __LINE__); fails++; } } while (0)

static void render(mvid_t *v, uint16_t *fb, int w, int h)
{
    const unsigned lw = v->info.map_w, lh = v->info.map_h;
    const unsigned tb = v->info.bpp == 8 ? 64u : 32u;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int sx = x + v->scroll_x, sy = y + v->scroll_y;
            int tx = (sx >> 3) % (int)lw; if (tx < 0) tx += (int)lw;
            int ty = (sy >> 3) % (int)lh; if (ty < 0) ty += (int)lh;
            const uint16_t e = v->map[ty * v->mw + tx];
            unsigned px = (unsigned)(sx & 7), py = (unsigned)(sy & 7);
            if (e & 0x4000) px = 7 - px;
            if (e & 0x8000) py = 7 - py;
            const uint8_t *t = v->sheet + (size_t)(e & 0x3FF) * tb;
            unsigned idx;
            if (v->info.bpp == 8) idx = t[py * 8 + px];
            else { uint8_t b = t[py * 4 + (px >> 1)]; idx = (px & 1) ? (b >> 4) : (b & 15); }
            uint16_t c = 0;
            if (idx) {
                const unsigned slot = (e >> 10) & 7;
                c = v->info.bpp == 8 ? v->pal[idx & 0xFF]
                                     : v->pal[(slot * 16 + idx) & 0xFF];
            }
            fb[(size_t)y * w + x] = mvid_scale565(c, v->brightness);
        }
    }
}

int main(void)
{
    enum { W = 320, H = 240, N = 24 };
    const uint16_t pal[16] = { 0x0000, 0xF800, 0x07E0, 0x001F,
                               0xFFFF, 0xFFE0, 0x07FF, 0xF81F };
    uint16_t *frames = (uint16_t *)calloc((size_t)W * H * N, 2);
    if (!frames) return 1;
    for (int f = 0; f < N; f++) {
        uint16_t *img = frames + (size_t)f * W * H;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                uint16_t c = 0x0000;
                if (y < 8) c = pal[3];                      /* sky band */
                if (y > 180) c = pal[2];                    /* ground    */
                if (x > 20 && x < 60 && y > 40 && y < 80) c = pal[6];
                const int bx = 32 + f * 4, by = 120;
                if (x >= bx && x < bx + 16 && y >= by && y < by + 16) c = pal[1];
                img[(size_t)y * W + x] = c;
            }
    }
    mvid_enc_opts_t o;
    memset(&o, 0, sizeof(o));
    o.width = W; o.height = H; o.fps = 12; o.bpp = 4; o.loop = 0;
    o.pal = pal; o.pal_count = 8;
    mvid_enc_t *e = mvid_enc_new(&o);
    CHECK(e != NULL, "enc_new");
    if (!e) return 1;
    for (int f = 0; f < N; f++)
        CHECK(mvid_enc_frame(e, frames + (size_t)f * W * H, 0, 0, 16) == 0, "enc_frame");
    const int bytes = (int)mvid_enc_frames(e);
    CHECK(bytes == N, "frame count");
    CHECK(mvid_enc_write(e, "mvid_test.mvid") == 0, "write");
    mvid_enc_free(e);

    size_t sz = 0;
    uint8_t *blob = mvid_read_file("mvid_test.mvid", &sz);
    CHECK(blob != NULL, "read");
    CHECK(blob && sz < (size_t)W * H * 2 * N / 4, "compressed to a quarter of raw");
    CHECK(mvid_check(blob, sz) == 0, "check");

    static uint8_t sheet[MVID_MAX_TILES * 64];
    static uint16_t map[64 * 64];
    mvid_t v;
    CHECK(mvid_open(&v, blob, sz, sheet, map) == 0, "open");
    CHECK(v.info.frames == N, "frame count in header");
    static uint16_t fb[W * H];
    int exact = 1;
    for (int f = 0; f < N; f++) {
        if (f) CHECK(mvid_next(&v) == 1, "next");
        render(&v, fb, W, H);
        const uint16_t *src = frames + (size_t)f * W * H;
        for (int i = 0; i < W * H; i++) if (fb[i] != src[i]) { exact = 0; break; }
    }
    CHECK(exact, "lossless: every decoded frame equals the source");
    CHECK(mvid_next(&v) == 0, "next returns 0 at the end");
    CHECK(mvid_seek(&v, 17) == 0 && v.cur == 17, "seek to a PATCH frame");
    mvid_close(&v);

    /* ---- pan mode: one map, scroll only ---- */
    static uint8_t ptiles[MVID_MAX_TILES * 32];
    static uint16_t pmap[64 * 32];
    for (int i = 0; i < 16; i++)
        for (int k = 0; k < 32; k++) ptiles[i * 32 + k] = (uint8_t)((i + k) & 3);
    for (int y = 0; y < 30; y++)
        for (int x = 0; x < 48; x++) pmap[y * 48 + x] = (uint16_t)((x + y) & 15);
    mvid_enc_opts_t po = o;
    po.map_w = 48; po.map_h = 30;
    mvid_enc_t *pe = mvid_enc_new(&po);
    CHECK(pe != NULL, "pan enc_new");
    if (pe) {
        CHECK(mvid_enc_source_map(pe, ptiles, 16, pmap, 48, 30, 4) == 0, "source_map");
        for (int f = 0; f < 12; f++)
            CHECK(mvid_enc_pan(pe, (int16_t)(f * 8), 0, 16) == 0, "pan frame");
        CHECK(mvid_enc_write(pe, "mvid_pan.mvid") == 0, "pan write");
        mvid_enc_free(pe);
        size_t psz = 0;
        uint8_t *pb = mvid_read_file("mvid_pan.mvid", &psz);
        CHECK(pb && psz < 40000, "pan clip is small");
        mvid_t pv;
        CHECK(mvid_open(&pv, pb, psz, sheet, map) == 0, "pan open");
        CHECK(mvid_seek(&pv, 11) == 0, "pan seek");
        CHECK(pv.scroll_x == 88 && pv.scroll_y == 0, "pan scroll applied");
        CHECK(pv.map[0] == 0 && pv.map[1] == 1, "pan map intact");
        mvid_close(&pv);
        free(pb);
    }

    /* ---- cart packaging (kind 11) ---- */
    static uint8_t cart[1 << 20];
    ms_cart_writer_t w;
    ms_cart_writer_begin(&w, cart, sizeof(cart));
    w.flags = MS_CART_F_EXT;
    const int arc = ms_cart_add(&w, "splash", MS_A_MVID, blob, sz);
        CHECK(arc == MS_CART_OK, "cart add mvid");
    const int n = ms_cart_finish(&w);
    CHECK(n > 0, "cart finish");
    ms_cart_t c;
    CHECK(ms_cart_mount(&c, cart, (size_t)n) == MS_CART_OK, "cart mounts with an MVID asset");
    ms_asset_t a;
    CHECK(ms_cart_find(&c, "splash", &a) == 0 && a.kind == MS_A_MVID, "mvid asset found");
    cart[n - 8] ^= 0xFF;                     /* break the payload CRC */
    CHECK(ms_cart_mount(&c, cart, (size_t)n) != MS_CART_OK, "corrupt mvid is rejected");

    free(blob);
    free(frames);
    printf(fails ? "mosaic_video_test: %d FAILURE(S)\n" : "mosaic_video_test: ok (lossless, pan, cart)\n", fails);
    return fails ? 1 : 0;
}
