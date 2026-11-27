/* SPDX-License-Identifier: Apache-2.0 */
/* mosaic — command line: build, validate, run, inspect, convert, rom. */
#include "mosaic/ms_build.h"
#include "mosaic/ms_cart.h"
#include "mosaic/ms_core.h"
#include "mosaic/ms_game.h"
#include "mosaic/ms_input.h"
#include "mosaic/ms_save.h"
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
           "  save init <cart> <img.msav> [--loc console|cart]   format an image\n"
           "  save list <cart> <img.msav>       home, dev, save id, slots\n"
           "  save link <cart> <img.msav>       may this cart read this image?\n"
           "  save put  <cart> <img.msav> <slot> <name> <file> [--fmt N]\n"
           "  save get  <cart> <img.msav> <slot> <file>\n"
           "  save dump <cart> <img.msav>       hex dump of every live slot\n"
           "  save export <cart> <img.msav> -o save.msv [--device N]\n"
           "  save import <cart> <img.msav> <save.msv>\n"
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

/* ---------------- save image ---------------- */

/* Resolve a cart to its save identity. Returns 0 and fills cfg/id. */
static int save_ctx(const char *cartpath, ms_save_cfg_t *cfg, uint32_t *cart_crc)
{
    size_t n = 0; uint8_t *b = slurp(cartpath, &n);
    if (!b) { fprintf(stderr, "cannot read %s\n", cartpath); return -1; }
    ms_cart_t c;
    const int rc = ms_cart_mount(&c, b, n);
    if (rc != 0) { fprintf(stderr, "%s: %s\n", cartpath, ms_cart_err_str(rc)); free(b); return -1; }
    ms_save_cfg_from_cart(&c, cfg);
    *cart_crc = c.crc;
    free(b);
    return 0;
}

static uint8_t *img_load(const char *path, size_t bytes, int create)
{
    FILE *f = fopen(path, "rb");
    if (!f) return create ? calloc(1, bytes) : NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if ((size_t)sz != bytes) { fclose(f); return NULL; }
    uint8_t *p = malloc(bytes);
    if (!p || fread(p, 1, bytes, f) != bytes) { fclose(f); free(p); return NULL; }
    fclose(f);
    return p;
}
static int img_store(const char *path, const void *p, size_t bytes)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    const int ok = fwrite(p, 1, bytes, f) == bytes;
    fclose(f);
    return ok ? 0 : -1;
}

static const char *loc_name(unsigned loc)
{
    return loc == MS_SAVE_LOC_CART ? "cartridge" : "console";
}

static int cmd_save(int argc, char **argv)
{
    if (argc < 4) return usage();
    const char *verb = argv[2], *cart = argv[3], *img = argc > 4 ? argv[4] : NULL;

    ms_save_cfg_t cfg; uint32_t cart_crc = 0;
    if (save_ctx(cart, &cfg, &cart_crc) != 0) return 2;

    unsigned loc = MS_SAVE_LOC_CONSOLE;
    for (int i = 4; i < argc - 1; i++) {
        if (!strcmp(argv[i], "--loc")) {
            if (!strcmp(argv[i + 1], "cart")) loc = MS_SAVE_LOC_CART;
            else loc = MS_SAVE_LOC_CONSOLE;
        }
    }
    ms_save_id_t id;
    ms_save_id_from_cfg(&id, &cfg, loc, cart_crc);
    id.loc = MS_SAVE_LOC_ANY;              /* opened by content, not by wish */
    ms_save_id_t locate;
    ms_save_id_from_cfg(&locate, &cfg, loc, cart_crc);

    if (!strcmp(verb, "init")) {
        if (!img) return usage();
        if (!(cfg.decl & (loc == MS_SAVE_LOC_CART ? MS_SAVE_DECL_CART : MS_SAVE_DECL_CONSOLE))) {
            fprintf(stderr, "%s does not declare a %s save region (save_loc=)\n", cart, loc_name(loc));
            return 3;
        }
        uint8_t *p = calloc(1, cfg.bytes);
        if (!p) return 4;
        ms_save_t s; unsigned fl = 0;
        int rc = ms_save_format(&s, p, cfg.bytes, &locate, cfg.slots, &fl);
        if (rc == MS_SAVE_OK && img_store(img, p, cfg.bytes) != 0) rc = MS_SAVE_E_NOSPACE;
        free(p);
        if (rc != MS_SAVE_OK) { fprintf(stderr, "save init: %s\n", ms_save_err_str(rc)); return 5; }
        printf("formatted %s: %u bytes, %u slots, %s home, dev %08x save %08x, cart_crc %08x\n",
               img, cfg.bytes, cfg.slots, loc_name(loc), cfg.dev_id, cfg.app_id, cart_crc);
        return 0;
    }
    if (!img) return usage();

    if (!strcmp(verb, "import")) {
        if (argc < 6) return usage();
        size_t bn = 0; uint8_t *bb = slurp(argv[5], &bn);
        if (!bb) { fprintf(stderr, "cannot read %s\n", argv[5]); return 2; }
        uint8_t *p = img_load(img, cfg.bytes, 1);
        if (!p) { fprintf(stderr, "cannot open %s\n", img); free(bb); return 4; }
        ms_save_t s; unsigned fl = 0;
        int rc = ms_save_open(&s, p, cfg.bytes, &id, &fl);
        if (rc == MS_SAVE_OK) rc = ms_save_bundle_import(&s, bb, bn, &fl);
        if (rc == MS_SAVE_OK && img_store(img, p, cfg.bytes) != 0) rc = MS_SAVE_E_NOSPACE;
        free(bb); free(p);
        if (rc != MS_SAVE_OK) { fprintf(stderr, "save import: %s\n", ms_save_err_str(rc)); return 5; }
        printf("imported %s into %s\n", argv[5], img);
        return 0;
    }

    const int create = !strcmp(verb, "put") || !strcmp(verb, "export");
    uint8_t *p = img_load(img, cfg.bytes, create);
    if (!p) { fprintf(stderr, "cannot open %s (expected %u bytes)\n", img, cfg.bytes); return 4; }

    ms_save_t s; unsigned fl = 0;
    /* list/dump/link only need to look, and must work without the dev key. */
    const int inspect = !strcmp(verb, "list") || !strcmp(verb, "dump");
    int rc = inspect ? ms_save_open_raw(&s, p, cfg.bytes, &fl)
                     : ms_save_open(&s, p, cfg.bytes, &id, &fl);
    if (rc != MS_SAVE_OK) {
        const char *why = ms_save_err_str(rc);
        fprintf(stderr, "%s: %s\n", img, why);
        if (rc == MS_SAVE_E_APP)
            fprintf(stderr, "  this cart is save %08x; the image is save %08x — add "
                            "`save_reads=<id>` to read it, or use the same `save_id=`\n",
                    id.app_id, s.app_id);
        free(p); return 5;
    }
    int did_write = 0;

    if (!strcmp(verb, "list")) {
        ms_save_ent_t e[MS_SAVE_SLOTS_MAX]; unsigned n = 0;
        ms_save_list(&s, e, MS_SAVE_SLOTS_MAX, &n);
        printf("%s  %s home  dev %08x  save %08x  %u slots%s%s\n", img, loc_name(s.loc),
               s.dev_id, s.app_id, s.slots,
               (fl & MS_SAVE_F_INIT) ? "  (formatted)" : "",
               (fl & MS_SAVE_F_LEGACY) ? "  (v1 image, read-only)" : "");
        if (s.loc == MS_SAVE_LOC_CART && s.cart_crc != cart_crc)
            printf("  ! built against cart_crc %08x, this cart is %08x", s.cart_crc, cart_crc);
        printf("\n");
        for (unsigned i = 0; i < n; i++)
            printf("  [%u] %-12s seq=%-4u len=%-4u fmt=%-3u %s\n", e[i].slot,
                   e[i].valid ? e[i].name : "-", e[i].seq, e[i].len, e[i].fmt,
                   e[i].valid ? "ok" : "empty");
        if (fl & MS_SAVE_F_TORN) printf("  ! a newer copy failed its checks (fell back)\n");
    } else if (!strcmp(verb, "link")) {
        /* Say, without opening, whether this cart may read this image. */
        if (!cfg.dev[0] || !cfg.save_id[0]) {
            printf("%s declares no identity — add `dev=` and `save_id=` to the .mpj\n", cart);
            free(p); return 1;
        }
        printf("%s:\n  dev  %08x  %s\n  save %08x  %s\n", cart, cfg.dev_id, cfg.dev,
               cfg.app_id, cfg.save_id);
        printf("  -> image dev %08x save %08x (%s home)\n", s.dev_id, s.app_id, loc_name(s.loc));
        const unsigned probe = s.read_only ? 1u : 0u;
        if (s.dev_id != cfg.dev_id)
            printf("  REFUSED: another developer's save\n");
        else if (s.app_id != cfg.app_id && !probe)
            printf("  REFUSED: different save id and no save_reads grant\n");
        else if (probe)
            printf("  ALLOWED read-only, through save_reads → writes refused\n");
        else
            printf("  ALLOWED read/write: same save id\n");
        for (int i = 0; i < cfg.n_reads; i++) printf("  reads %08x\n", cfg.reads[i]);
    } else if (!strcmp(verb, "put")) {
        if (argc < 8) { free(p); return usage(); }
        const unsigned slot = (unsigned)atoi(argv[5]);
        unsigned fmt = 0;
        for (int i = 4; i < argc - 1; i++) if (!strcmp(argv[i], "--fmt")) fmt = (unsigned)atoi(argv[i + 1]);
        size_t dn = 0; uint8_t *db = slurp(argv[7], &dn);
        if (!db) { fprintf(stderr, "cannot read %s\n", argv[7]); free(p); return 2; }
        rc = ms_save_write(&s, slot, argv[6], (uint16_t)fmt, db, dn);
        free(db);
        if (rc != MS_SAVE_OK) { fprintf(stderr, "save put: %s\n", ms_save_err_str(rc)); free(p); return 5; }
        did_write = 1;
        printf("slot %u <- %s (%zu bytes, name \"%s\", fmt %u)\n", slot, argv[7], dn, argv[6], fmt);
    } else if (!strcmp(verb, "get")) {
        if (argc < 7) { free(p); return usage(); }
        static uint8_t body[MS_SAVE_BYTES];
        size_t len = 0; uint16_t fmt = 0; char name[MS_SAVE_NAME + 1];
        rc = ms_save_read(&s, (unsigned)atoi(argv[5]), name, sizeof name, body, sizeof body,
                          &len, &fmt, &fl);
        if (rc != MS_SAVE_OK && rc != MS_SAVE_E_NOSPACE) {
            fprintf(stderr, "save get: %s\n", ms_save_err_str(rc)); free(p); return 5;
        }
        if (img_store(argv[6], body, len) != 0) { fprintf(stderr, "cannot write %s\n", argv[6]); free(p); return 6; }
        printf("slot %s -> %s: %zu bytes, name \"%s\", fmt %u%s\n", argv[5], argv[6], len, name, fmt,
               (fl & MS_SAVE_F_FELLBACK) ? " (fell back to the older copy)" : "");
    } else if (!strcmp(verb, "dump")) {
        for (unsigned slot = 0; slot < s.slots; slot++) {
            static uint8_t body[MS_SAVE_BYTES];
            size_t len = 0; uint16_t fmt = 0; char name[MS_SAVE_NAME + 1];
            rc = ms_save_read(&s, slot, name, sizeof name, body, sizeof body, &len, &fmt, &fl);
            if (rc == MS_SAVE_E_EMPTY) continue;
            if (rc != MS_SAVE_OK && rc != MS_SAVE_E_NOSPACE) {
                printf("[%u] unreadable: %s\n", slot, ms_save_err_str(rc)); continue;
            }
            printf("[%u] \"%s\" %zu bytes fmt %u  ", slot, name, len, fmt);
            for (size_t i = 0; i < len && i < 64; i++) printf("%02x", body[i]);
            printf("%s\n", len > 64 ? "..." : "");
        }
    } else if (!strcmp(verb, "export")) {
        const char *out = "save.msv"; uint32_t dev = 0;
        for (int i = 5; i < argc - 1; i++) {
            if (!strcmp(argv[i], "-o")) out = argv[i + 1];
            if (!strcmp(argv[i], "--device")) dev = (uint32_t)strtoul(argv[i + 1], NULL, 0);
        }
        static uint8_t bundle[MS_SAVE_BYTES + 512];
        size_t bn = 0;
        rc = ms_save_bundle_export(&s, dev, bundle, sizeof bundle, &bn);
        if (rc != MS_SAVE_OK) { fprintf(stderr, "save export: %s\n", ms_save_err_str(rc)); free(p); return 5; }
        if (img_store(out, bundle, bn) != 0) { fprintf(stderr, "cannot write %s\n", out); free(p); return 6; }
        printf("exported %zu bytes to %s (save %08x, device %08x)\n", bn, out, s.app_id, dev);
    } else return usage();

    if (did_write && img_store(img, p, cfg.bytes) != 0) {
        fprintf(stderr, "cannot write %s\n", img); free(p); return 6;
    }
    free(p);
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
    if (!strcmp(argv[1], "save")) return cmd_save(argc, argv);
    if (!strcmp(argv[1], "video")) return mosaic_video_main(argc, argv);
    if (!strcmp(argv[1], "audio")) return mosaic_audio_main(argc, argv);
    return usage();
}
