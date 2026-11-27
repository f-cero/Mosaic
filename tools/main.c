/* SPDX-License-Identifier: Apache-2.0 */
/* mosaic — command line: build, validate, run, inspect, convert, rom. */
#include "mosaic/ms_build.h"
#include "mosaic/ms_cart.h"
#include "mosaic/ms_core.h"
#include "mosaic/ms_game.h"
#include "mosaic/ms_input.h"
#include "stb/stb_image.h"

int mosaic_video_main(int argc, char **argv);
int mosaic_audio_main(int argc, char **argv);
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t g_fb[MS_SCREEN_W * MS_SCREEN_H];
static uint8_t g_arena[8192];

static int usage(void)
{
    printf("mosaic <command> [args]\n"
           "  build <project.mpj> [-o game.mcart]\n"
           "  validate <game.mcart>\n"
           "  run <game.mcart> [--frames N] [--left] [--right] [--jump] [--screenshot out.png]\n"
           "  inspect <game.mcart>\n"
           "  convert <in.png> <out.spr2> [--grid 16x16] [--bpp 4]\n"
           "  rom <game.mcart> [-o game.mrom]\n"
           "  video encode --input in.mp4|in.png --out out.mvid [--mode tile4|tile8|qfa4|qfa8] [--fps N]\n"
           "  video compose scene.mvj --out out.mvid\n"
           "  video info clip.mvid\n"
           "  video frames clip.mvid --out dir\n"
           "  audio ingest <in.wav> -o out.mid [--mode music|speech] [--program N] [--no-drums]\n"
           "  audio render <in.mid> -o out.wav [--seconds S] [--rate HZ]\n"
           "  audio compare <a.wav> <b.wav>   audio dump <in.mid>   audio info <in.wav>\n");
    return 1;
}

static int cmd_build(int argc, char **argv)
{
    const char *out = "game.mcart";
    for (int i = 2; i < argc - 1; i++) if (!strcmp(argv[i], "-o")) out = argv[i + 1];
    static ms_build_result_t r;
    int canonical = 0;
    for (int i = 3; i < argc; i++) if (!strcmp(argv[i], "--canonical")) canonical = 1;
    if (ms_build_project_opts(argv[2], &r, canonical) != 0) { fprintf(stderr, "build failed: %s\n", r.error); return 2; }
    FILE *f = fopen(out, "wb");
    if (!f || fwrite(r.cart, 1, r.cart_len, f) != r.cart_len) { fprintf(stderr, "cannot write %s\n", out); if (f) fclose(f); return 3; }
    fclose(f);
    printf("built %s: %zu bytes (cart16 v%d%s), %d sheets, %d songs, %d program(s)\n",
           out, r.cart_len, MS_CART_VERSION, canonical ? ", canonical" : ", +ext", r.sheets, r.songs, r.programs);
    return 0;
}

static uint8_t *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)sz); if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(b); return NULL; }
    fclose(f); *n = (size_t)sz; return b;
}

static int cmd_validate(int argc, char **argv)
{
    (void)argc; size_t n; uint8_t *b = slurp(argv[2], &n);
    if (!b) { fprintf(stderr, "cannot read %s\n", argv[2]); return 2; }
    ms_cart_t c; int rc = ms_cart_mount(&c, b, n);
    if (rc != 0) { printf("INVALID: %s (%d)\n", ms_cart_err_str(rc), rc); return 3; }
    printf("OK: \"%s\", %u assets, %zu bytes, flags=0x%04x\n", ms_cart_title(&c), c.count, n, c.flags);
    for (uint32_t i = 0; i < c.count; i++) {
        const uint8_t *e = b + c.toc_off + (size_t)i * MS_CART_TOC_ENTRY;
        printf("  %-16s kind=%u size=%u\n", (const char *)e, ms_rd32(e + 32), ms_rd32(e + 40));
    }
    return 0;
}

static int cmd_inspect(int argc, char **argv) { return cmd_validate(argc, argv); }

static int cmd_run(int argc, char **argv)
{
    const char *shot = NULL; int frames = 60, left = 0, right = 0, jump = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--left")) left = 1;
        else if (!strcmp(argv[i], "--right")) right = 1;
        else if (!strcmp(argv[i], "--jump")) jump = 1;
    }
    size_t n; uint8_t *b = slurp(argv[2], &n);
    if (!b) { fprintf(stderr, "cannot read %s\n", argv[2]); return 2; }
    ms_cart_t cart;
    if (ms_cart_mount(&cart, b, n) != MS_CART_OK) { fprintf(stderr, "cart invalid: %s\n", ms_cart_err_str(ms_cart_mount(&cart, b, n))); return 3; }
    ms_config_t cfg = { 0 };
    cfg.audio_hz = MS_AUDIO_HZ; cfg.voices = 16; cfg.music_voices = 12; cfg.sfx_voices = 4;
    ms_core_t core;
    if (ms_core_init(&core, &cfg, g_fb, g_arena, sizeof(g_arena)) != 0) { fprintf(stderr, "core init failed\n"); return 4; }
    ms_game_t game;
    int rc = ms_game_load(&game, &cart, &core.ppu, &core.synth);
    if (rc != 0) { fprintf(stderr, "game load failed: %s\n", ms_game_error(&game)); return 5; }
    cfg.mode = game.man.mode; cfg.qfa = game.man.qfa;
    ms_core_shutdown(&core);
    if (ms_core_init(&core, &cfg, g_fb, g_arena, sizeof(g_arena)) != 0) return 4;
    core.ppu = game.ppu[0];
    rc = ms_game_load(&game, &cart, &core.ppu, &core.synth);
    if (rc != 0) return 5;
    ms_input_t in; ms_input_clear(&in);
    int64_t audio_energy = 0;
    for (int f = 0; f < frames; f++) {
        uint16_t held = 0;
        if (left) held |= MS_BTN_LEFT;
        if (right) held |= MS_BTN_RIGHT;
        if (jump && f == 10) held |= MS_BTN_A;
        ms_input_edge(&in, held);
        in.touch_x = in.touch_y = -1;
        ms_core_run_frame(&core, &game, &in);
        for (int i = 0; i < 1152 * 2; i++) audio_energy += core.abuf[i] < 0 ? -core.abuf[i] : core.abuf[i];
    }
    printf("ran %d frames, %u entities, audio energy %lld\n", frames, game.vm.count, (long long)audio_energy);
    if (shot) {
        int w = cfg.qfa ? MS_SCREEN_W : MS_SCREEN_W, h = MS_SCREEN_H;
        if (ms_write_png(shot, w, h, g_fb) != 0) { fprintf(stderr, "screenshot failed\n"); return 6; }
        printf("wrote %s\n", shot);
    }
    return 0;
}

static int cmd_convert(int argc, char **argv)
{
    int gw = 16, gh = 16, bpp = 4;
    for (int i = 4; i < argc - 1; i++) {
        if (!strcmp(argv[i], "--grid")) sscanf(argv[i + 1], "%dx%d", &gw, &gh);
        if (!strcmp(argv[i], "--bpp")) bpp = atoi(argv[i + 1]);
    }
    int rc = ms_convert_sprite(argv[2], argv[3], gw, gh, bpp);
    if (rc != 0) { fprintf(stderr, "convert failed (%d)\n", rc); return 2; }
    printf("wrote %s\n", argv[3]);
    return 0;
}

static int cmd_rom(int argc, char **argv)
{
    const char *out = "game.mrom";
    for (int i = 3; i < argc - 1; i++) if (!strcmp(argv[i], "-o")) out = argv[i + 1];
    size_t n; uint8_t *b = slurp(argv[2], &n);
    if (!b) return 2;
    ms_cart_t c;
    if (ms_cart_mount(&c, b, n) != 0) { fprintf(stderr, "cart invalid\n"); return 3; }
    uint8_t hdr[32]; memset(hdr, 0, 32);
    memcpy(hdr, "MOSROM01", 8);
    ms_wr32(hdr + 8, (uint32_t)n);
    ms_wr32(hdr + 12, ms_crc32(b, n));
    ms_wr32(hdr + 16, 0x00200000u);   /* flash offset of the data partition */
    ms_wr32(hdr + 20, 0x00400000u);   /* partition size */
    strncpy((char *)hdr + 24, "MOZA", 4);
    FILE *f = fopen(out, "wb");
    if (!f) return 4;
    fwrite(hdr, 1, 32, f); fwrite(b, 1, n, f); fclose(f);
    printf("wrote %s (%zu bytes payload, flash at 0x00200000)\n", out, n);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) return usage();
    if (!strcmp(argv[1], "build")) return argc >= 3 ? cmd_build(argc, argv) : usage();
    if (!strcmp(argv[1], "validate")) return argc >= 3 ? cmd_validate(argc, argv) : usage();
    if (!strcmp(argv[1], "inspect")) return argc >= 3 ? cmd_inspect(argc, argv) : usage();
    if (!strcmp(argv[1], "run")) return argc >= 3 ? cmd_run(argc, argv) : usage();
    if (!strcmp(argv[1], "convert")) return argc >= 4 ? cmd_convert(argc, argv) : usage();
    if (!strcmp(argv[1], "rom")) return argc >= 3 ? cmd_rom(argc, argv) : usage();
    if (!strcmp(argv[1], "video")) return mosaic_video_main(argc, argv);
    if (!strcmp(argv[1], "audio")) return mosaic_audio_main(argc, argv);
    return usage();
}
