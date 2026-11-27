/* SPDX-License-Identifier: Apache-2.0 */
/* Mosaic — tools/client.c: a display node over MSP/1. Headless by design: it
 * applies the server's register stream to a local PPU, renders, and reports a
 * per-frame hash. That hash is the whole point — it is the only evidence that
 * a thin client reproduces the authority's frames.
 *
 *   mosaic play --connect 127.0.0.1:7777 --cart game.mcart [--frames N]
 */
#include "net.h"
#include "mosaic/ms_core.h"
#include "mosaic/ms_game.h"
#include "mosaic/ms_dump.h"
#include "mosaic/ms_net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t s_fb[MS_SCREEN_W * MS_SCREEN_H];

static uint32_t rom_identity(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static uint8_t *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t *b = (uint8_t *)malloc((size_t)sz);
    if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(b); return NULL; }
    fclose(f);
    *n = (size_t)sz;
    return b;
}

int ms_client_run(const ms_client_opts_t *o, ms_client_result_t *res)
{
    memset(res, 0, sizeof *res);
    res->rc = -1; res->node = -1;
    if (!o->cart || !o->host) return -1;

    size_t n = 0;
    uint8_t *file = read_file(o->cart, &n);
    if (!file) return 2;
    const uint8_t *rom = file;
    size_t rom_n = n;
    ms_dump_container_t box;
    if (ms_dump_container_open(&box, file, n) == MS_DUMP_OK) { rom = box.rom; rom_n = box.rom_bytes; }
    ms_cart_t cart;
    if (ms_cart_mount(&cart, rom, rom_n) != MS_CART_OK) { free(file); return 2; }
    const uint32_t rom_id = rom_identity(rom, rom_n);

    ms_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.audio_hz = MS_AUDIO_HZ; cfg.voices = 16; cfg.music_voices = 12; cfg.sfx_voices = 4;
    ms_core_t core;
    uint8_t arena[8192];
    if (ms_core_init(&core, &cfg, s_fb, arena, sizeof arena) != 0) { free(file); return 2; }
    ms_game_t game;
    if (ms_game_load(&game, &cart, &core.ppu, &core.synth) != 0) { free(file); return 2; }
    cfg.mode = game.man.mode; cfg.qfa = game.man.qfa;
    ms_core_shutdown(&core);
    if (ms_core_init(&core, &cfg, s_fb, arena, sizeof arena) != 0) { free(file); return 2; }
    core.ppu = game.ppu[0];
    if (ms_game_load(&game, &cart, &core.ppu, &core.synth) != 0) { free(file); return 2; }

    ms_net_buf_t store[MS_MAX_BG];
    memset(store, 0, sizeof store);
    for (unsigned i = 0; i < MS_MAX_BG; i++) {
        const ms_bg_t *bg = &core.ppu.bg[i];
        if (!bg->map || !bg->map_w) continue;
        store[i].bytes = (size_t)bg->map_w * bg->map_h * 2u;
        store[i].ptr = malloc(store[i].bytes);
        if (!store[i].ptr) { free(file); return 2; }
    }
    ms_net_client_t *cli = (ms_net_client_t *)malloc(sizeof(ms_net_client_t));
    if (!cli || ms_net_client_init(cli, &game, &core.ppu, s_fb,
                                   cfg.qfa ? MS_QFA_W : MS_SCREEN_W,
                                   cfg.qfa ? MS_QFA_H : MS_SCREEN_H, store) != 0) {
        free(cli); free(file); return 2;
    }

    ms_io_init();
    int fd = ms_io_connect(o->host, o->port);
    if (fd < 0) {
        fprintf(stderr, "play: cannot reach %s:%d\n", o->host, o->port);
        res->rc = 3; goto out_free;
    }
    ms_io_nonblock(fd, 1);
    ms_io_conn_t *conn = (ms_io_conn_t *)calloc(1, sizeof(ms_io_conn_t));
    conn->fd = fd;

    {
        uint8_t buf[64];
        ms_net_join_t j;
        memset(&j, 0, sizeof j);
        j.ver = MS_MSP_VERSION;
        j.node = 0;
        j.role = MS_ROLE_PPU;
        snprintf(j.name, sizeof j.name, "display");
        size_t jn = ms_net_encode_join(buf, sizeof buf, &j);
        ms_link_frame_t f;
        memset(&f, 0, sizeof f);
        f.ver = 1; f.type = MS_LINK_JOIN; f.len = (uint16_t)jn;
        memcpy(f.payload, buf, jn);
        if (ms_io_send(fd, &f) != 0) { res->rc = 3; goto out_close; }
    }

    uint32_t expect = 0;
    uint64_t deadline = ms_io_now_us() + 5000000ull;
    int got_session = 0;
    while (ms_io_now_us() < deadline) {
        int k = ms_io_fill(conn);
        if (k < 0) break;
        ms_link_frame_t f;
        int r;
        while ((r = ms_io_shift(conn, &f)) != 0) {
            if (r < 0) { res->crc_errors++; continue; }
            deadline = ms_io_now_us() + 5000000ull;
            if (f.type == MS_LINK_SESSION) {
                ms_net_session_t ses;
                if (ms_net_decode_session(&ses, f.payload, f.len) != 0) continue;
                res->node = ses.node;
                got_session = 1;
                if (ses.cart_crc != rom_id) {
                    fprintf(stderr, "play: cartridge mismatch (%08x vs %08x) — "
                                    "a display node must mount the same build\n",
                            ses.cart_crc, rom_id);
                    res->rc = 4; goto out_close;
                }
                if (!o->quiet)
                    printf("play: node %u, \"%s\", %u Hz, %u slots\n",
                           ses.node, ses.title, ses.fps, ses.save_slots);
            } else if (f.type == MS_LINK_KEYFRAME) {
                if (ms_net_apply_keyframe(cli, f.payload, f.len) != 0) { res->rc = 5; goto out_close; }
                expect = cli->frame + 1u;
            } else if (f.type == MS_LINK_SYNC) {
                if (f.len >= 4) expect = ms_rd32(f.payload) + 1u;
                cli->synced = 1;
            } else if (f.type == MS_LINK_REG_DELTA) {
                if (!cli->synced) continue;
                if (ms_net_apply_delta(cli, f.payload, f.len) != 0) { res->overflow++; continue; }
                ms_net_render(cli);
                const uint64_t h = ms_net_hash(&cli->ppu, cli->fb, cli->w, cli->h);
                res->final_hash = h;
                res->frames = cli->applied;
                if (cli->frame != expect && expect) {
                    /* a gap means the authority skipped a frame for us */
                    expect = cli->frame;
                }
                expect = cli->frame + 1u;
                {
                    ms_net_state_t st;
                    st.frame = cli->frame; st.applied = cli->applied; st.hash = h;
                    uint8_t sb[16];
                    size_t sn = ms_net_encode_state(sb, sizeof sb, &st);
                    ms_link_frame_t sf;
                    memset(&sf, 0, sizeof sf);
                    sf.ver = 1; sf.type = MS_LINK_STATE; sf.node = (uint8_t)res->node;
                    sf.len = (uint16_t)sn;
                    memcpy(sf.payload, sb, sn);
                    ms_io_send(fd, &sf);
                }
                if (o->frames && cli->applied >= (uint32_t)o->frames) { res->rc = 0; goto out_close; }
            } else if (f.type == MS_LINK_BYE) {
                res->rc = 0; goto out_close;
            }
        }
        ms_io_sleep_us(500);
    }
    res->rc = got_session ? 0 : 5;

out_close:
    ms_io_close(fd);
    free(conn);
    ms_io_shutdown();
out_free:
    for (unsigned i = 0; i < MS_MAX_BG; i++) free(store[i].ptr);
    free(cli);
    free(file);
    if (o->quiet) return res->rc;
    if (res->rc == 0)
        printf("play: node %d applied %u frames, final hash %016llx, %u overflows\n",
               res->node, res->frames, (unsigned long long)res->final_hash, res->overflow);
    return res->rc;
}

int mosaic_client_main(int argc, char **argv)
{
    ms_client_opts_t o;
    const char *addr = NULL;
    memset(&o, 0, sizeof o);
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--connect") && i + 1 < argc) addr = argv[++i];
        else if (!strcmp(argv[i], "--cart") && i + 1 < argc) o.cart = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) o.frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--hold") && i + 1 < argc) o.hold = (uint16_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--quiet")) o.quiet = 1;
    }
    if (!addr || !o.cart) {
        fprintf(stderr, "usage: mosaic play --connect HOST:PORT --cart game.mcart [--frames N]\n");
        return 1;
    }
    char host[64];
    snprintf(host, sizeof host, "%s", addr);
    char *colon = strrchr(host, ':');
    if (!colon) { fprintf(stderr, "play: --connect wants HOST:PORT\n"); return 1; }
    *colon = 0;
    o.host = host;
    o.port = atoi(colon + 1);
    ms_client_result_t r;
    (void)o.hold;
    return ms_client_run(&o, &r) == 0 ? 0 : 1;
}
