/* SPDX-License-Identifier: Apache-2.0 */
/* Mosaic — tools/server.c: the dedicated server. Headless: no SDL, no window.
 * It runs the frame contract at a fixed rate and puts the PPU register stream
 * on the wire; clients are display nodes that mounted the same cartridge.
 *
 *   mosaic serve --cart game.mcart [--port 7777] [--frames N] [--saves img.msav]
 *
 * Authority: the server owns the simulation, the RNG and the save image. A
 * client's word is only its pad and its per-frame hash; a hash that disagrees
 * is counted and repaired with a keyframe, never trusted. */
#include "net.h"
#include "mosaic/ms_core.h"
#include "mosaic/ms_game.h"
#include "mosaic/ms_dump.h"
#include "mosaic/ms_net.h"
#include "mosaic/ms_save.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXC 7
#define PHASE_KEY  1
#define PHASE_MAP  2
#define PHASE_LIVE 3

typedef struct {
    ms_io_conn_t io;
    uint8_t  node, phase, resync_bg;
    uint32_t resync_at, sent, states;
    uint16_t pad;
    int16_t  tx, ty;
    char     name[MS_MSP_NAME + 1];
} sclient_t;

typedef struct { uint32_t frame; uint64_t hash; } hashslot_t;

typedef struct {
    const ms_server_opts_t *o;
    ms_server_result_t *res;
    ms_cart_t cart;
    ms_core_t core;
    ms_game_t game;
    ms_net_snap_t snap;
    uint16_t *map_snap[MS_MAX_BG];
    ms_net_buf_t map_buf[MS_MAX_BG];
    ms_save_store_t saves;
    ms_save_t console, cartsave;
    uint8_t *console_region, *file;
    size_t   console_bytes, file_len;
    ms_dump_container_t box;
    int      have_box, lfd;
    uint32_t rom_id;
    uint8_t  node_used[MS_LINK_MAX_NODES];
    sclient_t cl[MAXC];
    hashslot_t ring[256];
    uint8_t  txseq;
    size_t   payload_len;
    uint8_t  payload[MS_LINK_MTU + 16];
    uint8_t  cbuf[MS_LINK_MTU + 16];
} srv_t;

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

static int send_msg(srv_t *s, sclient_t *c, uint8_t type, const void *payload, size_t n)
{
    ms_link_frame_t f;
    if (n > MS_LINK_MTU) return -1;
    memset(&f, 0, sizeof f);
    f.ver = 1; f.type = type; f.node = c->node;
    f.seq = (uint16_t)(s->txseq++);
    f.len = (uint16_t)n;
    if (n) memcpy(f.payload, payload, n);
    if (ms_io_send(c->io.fd, &f) != 0) return -1;
    return 0;
}

static int host_load(srv_t *s)
{
    s->file = read_file(s->o->cart, &s->file_len);
    if (!s->file) return -1;
    const uint8_t *rom = s->file;
    size_t rom_n = s->file_len;
    if (ms_dump_container_open(&s->box, s->file, s->file_len) == MS_DUMP_OK) {
        rom = s->box.rom; rom_n = s->box.rom_bytes; s->have_box = 1;
    }
    if (ms_cart_mount(&s->cart, rom, rom_n) != MS_CART_OK) return -2;
    s->rom_id = rom_identity(rom, rom_n);

    ms_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.audio_hz = MS_AUDIO_HZ; cfg.voices = 16; cfg.music_voices = 12; cfg.sfx_voices = 4;
    if (ms_core_init(&s->core, &cfg, s_fb, (uint8_t[8192]){0}, 8192) != 0) return -3;
    int rc = ms_game_load(&s->game, &s->cart, &s->core.ppu, &s->core.synth);
    if (rc != 0) return -4;
    cfg.mode = s->game.man.mode; cfg.qfa = s->game.man.qfa;
    ms_core_shutdown(&s->core);
    if (ms_core_init(&s->core, &cfg, s_fb, (uint8_t[8192]){0}, 8192) != 0) return -3;
    s->core.ppu = s->game.ppu[0];
    rc = ms_game_load(&s->game, &s->cart, &s->core.ppu, &s->core.synth);
    if (rc != 0) return -4;
    for (unsigned i = 0; i < MS_MAX_BG; i++) {
        const ms_bg_t *bg = &s->core.ppu.bg[i];
        if (!bg->map || !bg->map_w) continue;
        const size_t bytes = (size_t)bg->map_w * bg->map_h * 2u;
        s->map_snap[i] = (uint16_t *)malloc(bytes);
        if (!s->map_snap[i]) return -5;
        s->map_buf[i].ptr = s->map_snap[i];
        s->map_buf[i].bytes = bytes;
    }
    return 0;
}

static int host_saves(srv_t *s)
{
    ms_save_cfg_t cfg;
    if (ms_save_cfg_from_cart(&s->cart, &cfg) != 1) {
        if (s->o->saves) fprintf(stderr, "serve: cart declares no save region, ignoring --saves\n");
        return 0;
    }
    unsigned flags = 0;
    if (s->have_box && s->box.sram && s->box.sram_bytes >= MS_SAVE_BYTES_MIN &&
        (cfg.decl & MS_SAVE_DECL_CART)) {
        ms_save_id_t id;
        ms_save_id_from_cfg(&id, &cfg, MS_SAVE_LOC_CART, s->rom_id);
        id.loc = MS_SAVE_LOC_ANY;
        if (ms_save_open(&s->cartsave, (void *)s->box.sram, s->box.sram_bytes, &id, &flags) == MS_SAVE_OK) {
            ms_save_store_attach(&s->saves, &s->cartsave);
            if (s->o->verbose) printf("serve: cartridge home from the dump (%u bytes)\n", s->box.sram_bytes);
        }
    }
    const char *path = s->o->saves;
    if (!path) return 0;
    if (!(cfg.decl & MS_SAVE_DECL_CONSOLE)) {
        fprintf(stderr, "serve: cart declares no console save region (save_loc=)\n");
        return 0;
    }
    s->console_bytes = cfg.bytes;
    s->console_region = (uint8_t *)calloc(1, cfg.bytes);
    if (!s->console_region) return -1;
    FILE *f = fopen(path, "rb");
    size_t got = 0;
    if (f) { got = fread(s->console_region, 1, cfg.bytes, f); fclose(f); }
    ms_save_id_t open_id, locate;
    ms_save_id_from_cfg(&open_id, &cfg, MS_SAVE_LOC_CONSOLE, s->rom_id);
    ms_save_id_from_cfg(&locate, &cfg, MS_SAVE_LOC_CONSOLE, s->rom_id);
    open_id.loc = MS_SAVE_LOC_ANY;
    int rc;
    if (got >= MS_SAVE_HDR)
        rc = ms_save_open(&s->console, s->console_region, cfg.bytes, &open_id, &flags);
    else
        rc = ms_save_format(&s->console, s->console_region, cfg.bytes, &locate, cfg.slots, &flags);
    if (rc != MS_SAVE_OK) {
        fprintf(stderr, "serve: %s: %s\n", path, ms_save_err_str(rc));
        free(s->console_region); s->console_region = NULL;
        return 0;
    }
    ms_save_store_attach(&s->saves, &s->console);
    if (s->o->verbose)
        printf("serve: console home %s (%u bytes, %u slots)\n", path, cfg.bytes, cfg.slots);
    return 0;
}

static sclient_t *slot_for(srv_t *s, int fd)
{
    for (int i = 0; i < MAXC; i++)
        if (s->cl[i].io.fd < 0) {
            memset(&s->cl[i], 0, sizeof s->cl[i]);
            s->cl[i].io.fd = fd;
            s->cl[i].phase = 0;
            return &s->cl[i];
        }
    return NULL;
}

static void drop_client(srv_t *s, sclient_t *c)
{
    if (c->io.fd >= 0) ms_io_close(c->io.fd);
    if (c->node < MS_LINK_MAX_NODES) s->node_used[c->node] = 0;
    c->io.fd = -1; c->phase = 0; c->io.have = 0;
}

static int live_count(srv_t *s)
{
    int n = 0;
    for (int i = 0; i < MAXC; i++) if (s->cl[i].phase == PHASE_LIVE) n++;
    return n;
}

static void handle_join(srv_t *s, sclient_t *c, const ms_link_frame_t *f)
{
    ms_net_join_t j;
    if (ms_net_decode_join(&j, f->payload, f->len) != 0) { drop_client(s, c); return; }
    if (j.node && j.node < MS_LINK_MAX_NODES && !s->node_used[j.node]) c->node = j.node;
    else {
        for (int i = 1; i < (int)MS_LINK_MAX_NODES; i++)
            if (!s->node_used[i]) { c->node = (uint8_t)i; break; }
    }
    if (!c->node) { send_msg(s, c, MS_LINK_BYE, NULL, 0); drop_client(s, c); return; }
    s->node_used[c->node] = 1;
    memcpy(c->name, j.name, MS_MSP_NAME); c->name[MS_MSP_NAME] = 0;

    ms_net_session_t ses;
    memset(&ses, 0, sizeof ses);
    ses.node = c->node;
    ses.fps = (uint16_t)(s->o->fps > 0 ? s->o->fps : 60);
    ses.mtu = MS_LINK_MTU;
    ses.frame = s->core.ppu.frame;
    ses.cart_crc = s->rom_id;
    ses.save_slots = MS_SAVE_SLOTS;
    snprintf(ses.title, sizeof ses.title, "%s", ms_cart_title(&s->cart));
    uint8_t buf[64];
    size_t n = ms_net_encode_session(buf, sizeof buf, &ses);
    if (send_msg(s, c, MS_LINK_SESSION, buf, n) != 0) { drop_client(s, c); return; }
    c->phase = PHASE_MAP;
    s->res->clients++;
    s->res->client_frames[c->node] = 0;
    if (s->o->verbose) printf("serve: node %u joined as \"%s\"\n", c->node, c->name);
}

static void handle_frame(srv_t *s, sclient_t *c, const ms_link_frame_t *f)
{
    switch (f->type) {
    case MS_LINK_JOIN:
        if (c->phase == 0) handle_join(s, c, f);
        break;
    case MS_LINK_INPUT:
        if (f->len >= 7) {
            c->pad = (uint16_t)(f->payload[1] | ((uint16_t)f->payload[2] << 8));
            c->tx = (int16_t)(f->payload[3] | ((uint16_t)f->payload[4] << 8));
            c->ty = (int16_t)(f->payload[5] | ((uint16_t)f->payload[6] << 8));
        }
        break;
    case MS_LINK_STATE: {
        ms_net_state_t st;
        if (ms_net_decode_state(&st, f->payload, f->len) != 0) break;
        c->states++;
        const hashslot_t *h = &s->ring[st.frame & 0xFFu];
        if (h->frame == st.frame) {
            if (h->hash != st.hash) {
                s->res->hash_mismatches++;
                if (s->o->verbose && s->res->hash_mismatches <= 5)
                    fprintf(stderr, "serve: node %u frame %u hash %016llx != %016llx\n",
                            c->node, st.frame, (unsigned long long)st.hash,
                            (unsigned long long)h->hash);
                c->phase = PHASE_MAP;      /* repair: map, keyframe, sync */
                c->resync_bg = 0; c->resync_at = 0;
            } else {
                s->res->client_frames[c->node] = st.applied;
                s->res->client_hash[c->node] = st.hash;
            }
        }
        break;
    }
    case MS_LINK_BYE:
        drop_client(s, c);
        break;
    default:
        break;
    }
}

static void pump(srv_t *s, int accept_new)
{
    if (accept_new) {
        for (;;) {
            int fd = ms_io_accept(s->lfd);
            if (fd < 0) break;
            sclient_t *c = slot_for(s, fd);
            if (!c) { ms_io_close(fd); continue; }
            ms_io_nonblock(fd, 1);
        }
    }
    for (int i = 0; i < MAXC; i++) {
        sclient_t *c = &s->cl[i];
        if (c->io.fd < 0) continue;
        int k = ms_io_fill(&c->io);
        if (k < 0) { drop_client(s, c); continue; }
        ms_link_frame_t f;
        while (c->io.fd >= 0) {
            int r = ms_io_shift(&c->io, &f);
            if (r == 0) break;
            if (r < 0) { s->res->crc_errors++; continue; }
            handle_frame(s, c, &f);
        }
    }
}

static void serve_client_frame(srv_t *s, sclient_t *c, int key)
{
    if (c->phase == PHASE_KEY) { c->phase = PHASE_MAP; c->resync_bg = 0; c->resync_at = 0; }
    if (c->phase == PHASE_MAP) {
        int chunks = 0, guard = 0;
        while (c->phase == PHASE_MAP && chunks < 8 && guard++ < 40) {
            if (c->resync_bg >= MS_MAX_BG) {
                /* Map first, then a keyframe of the same instant, then SYNC:
                 * the client's baseline is exactly the frame deltas resume at. */
                size_t kn = ms_net_encode_keyframe(&s->game, &s->core.ppu, s->cbuf, sizeof s->cbuf);
                if (!kn || send_msg(s, c, MS_LINK_KEYFRAME, s->cbuf, kn) != 0) { drop_client(s, c); return; }
                s->res->keys_sent++;
                uint8_t b[4];
                ms_wr32(b, s->core.ppu.frame);
                if (send_msg(s, c, MS_LINK_SYNC, b, 4) != 0) { drop_client(s, c); return; }
                c->phase = PHASE_LIVE; c->sent = 0;
                return;
            }
            const ms_bg_t *bg = &s->core.ppu.bg[c->resync_bg];
            const uint32_t ents = bg->map ? (uint32_t)bg->map_w * bg->map_h : 0;
            if (!ents || c->resync_at >= ents) { c->resync_bg++; c->resync_at = 0; continue; }
            size_t n = ms_net_encode_map_chunk(&s->core.ppu, c->resync_bg, c->resync_at,
                                               2000, s->core.ppu.frame, s->cbuf, sizeof s->cbuf);
            if (!n) { c->resync_bg++; c->resync_at = 0; continue; }
            const uint16_t cnt = (uint16_t)(s->cbuf[9] | ((uint16_t)s->cbuf[10] << 8));
            if (send_msg(s, c, MS_LINK_REG_DELTA, s->cbuf, n) != 0) { drop_client(s, c); return; }
            c->resync_at += cnt ? cnt : 2000u;
            chunks++;
        }
        return;
    }
    if (c->phase != PHASE_LIVE) return;
    if (key) {
        size_t n = ms_net_encode_keyframe(&s->game, &s->core.ppu, s->cbuf, sizeof s->cbuf);
        if (n && send_msg(s, c, MS_LINK_KEYFRAME, s->cbuf, n) == 0) {
            s->res->keys_sent++;
            c->sent++;
            return;
        }
        drop_client(s, c);
        return;
    }
    if (s->payload_len && send_msg(s, c, MS_LINK_REG_DELTA, s->payload, s->payload_len) == 0)
        c->sent++;
    else
        drop_client(s, c);
}

int ms_server_run(const ms_server_opts_t *o, ms_server_result_t *res)
{
    srv_t *s = (srv_t *)calloc(1, sizeof(srv_t));
    if (!s || !res) { free(s); return -1; }
    memset(res, 0, sizeof *res);
    res->rc = -1;
    s->o = o; s->res = res; s->lfd = -1;
    for (int i = 0; i < MAXC; i++) s->cl[i].io.fd = -1;
    for (unsigned i = 0; i < 256; i++) s->ring[i].frame = 0xFFFFFFFFu;

    ms_io_init();
    if (host_load(s) != 0) { fprintf(stderr, "serve: cannot load %s\n", o->cart); goto out; }
    if (host_saves(s) != 0) goto out;
    ms_game_set_saves(&s->game, s->saves.present ? &s->saves : NULL);
    ms_net_snap_init(&s->snap, &s->core.ppu, s->map_buf);

    s->lfd = ms_io_listen(o->bind_addr, o->port, &res->port);
    if (s->lfd < 0) { fprintf(stderr, "serve: cannot bind port %d\n", o->port); goto out; }
    if (o->port_out) *o->port_out = res->port;
    if (o->ready) *o->ready = 1;
    if (!o->quiet)
        printf("serve: \"%s\" on %s:%d, %d Hz%s\n", ms_cart_title(&s->cart),
               o->bind_addr ? o->bind_addr : "0.0.0.0", res->port,
               o->fps > 0 ? o->fps : 60, s->saves.present ? ", saves attached" : "");

    if (o->wait_clients > 0) {
        const uint64_t deadline = ms_io_now_us() + 10ull * 1000000ull;
        while (live_count(s) < o->wait_clients && ms_io_now_us() < deadline) {
            pump(s, 1);
            /* a waiting client must still be brought up to date, or it can
             * never become live and the wait deadlocks */
            for (int i = 0; i < MAXC; i++)
                if (s->cl[i].io.fd >= 0) serve_client_frame(s, &s->cl[i], 0);
            ms_io_sleep_us(2000);
        }
    }

    const int fps = o->fps > 0 ? o->fps : 60;
    const uint64_t period = 1000000ull / (uint64_t)fps;
    uint64_t next = ms_io_now_us();
    uint32_t frame = 0;

    while (!o->frames || frame < (uint32_t)o->frames) {
        if (!o->fast) {
            const uint64_t now = ms_io_now_us();
            if (now < next) ms_io_sleep_us(next - now);
            next += period;
            if (ms_io_now_us() > next + 4u * period) next = ms_io_now_us();
        }
        pump(s, 1);

        uint16_t held = 0;
        int16_t tx = -1, ty = -1;
        for (int i = 0; i < MAXC; i++) {
            if (s->cl[i].io.fd < 0) continue;
            held |= s->cl[i].pad;
            if (s->cl[i].tx >= 0) { tx = s->cl[i].tx; ty = s->cl[i].ty; }
        }
        ms_input_t in;
        ms_input_clear(&in);
        ms_input_edge(&in, held);
        in.touch_x = tx; in.touch_y = ty;
        in.touch_active = (uint8_t)(tx >= 0);
        ms_core_run_frame(&s->core, &s->game, &in);
        frame++;

        s->payload_len = ms_net_encode_delta(&s->game, &s->core.ppu, &s->snap,
                                             s->payload, sizeof s->payload);
        int key = 0;
        if (!s->payload_len) {
            key = 1;
            res->overflow++;
            s->payload_len = ms_net_encode_keyframe(&s->game, &s->core.ppu, s->payload, sizeof s->payload);
            ms_net_snap_init(&s->snap, &s->core.ppu, s->map_buf);
        }
        const uint64_t h = ms_net_hash(&s->core.ppu, s_fb,
                                       s->core.ppu.width, s->core.ppu.height);
        s->ring[s->core.ppu.frame & 0xFFu].frame = s->core.ppu.frame;
        s->ring[s->core.ppu.frame & 0xFFu].hash = h;
        res->final_hash = h;
        res->frames = frame;

        for (int i = 0; i < MAXC; i++)
            if (s->cl[i].io.fd >= 0) serve_client_frame(s, &s->cl[i], key);
        pump(s, 1);
    }

    /* Give the clients a moment to land their last STATE frame, then say bye. */
    for (int i = 0; i < 30; i++) { pump(s, 0); ms_io_sleep_us(2000); }
    for (int i = 0; i < MAXC; i++) {
        sclient_t *c = &s->cl[i];
        if (c->io.fd < 0) continue;
        send_msg(s, c, MS_LINK_BYE, NULL, 0);
        ms_io_sleep_us(2000);
        drop_client(s, c);
    }
    res->rc = (res->crc_errors == 0 && res->hash_mismatches == 0) ? 0 : 1;

out:
    if (s->console_region && o->saves) {
        FILE *f = fopen(o->saves, "wb");
        if (f) { fwrite(s->console_region, 1, s->console_bytes, f); fclose(f); }
    }
    if (s->lfd >= 0) ms_io_close(s->lfd);
    for (int i = 0; i < MAXC; i++) if (s->cl[i].io.fd >= 0) ms_io_close(s->cl[i].io.fd);
    if (o->verbose)
        for (int i = 0; i < MAXC; i++)
            if (s->cl[i].node) printf("serve: node %u sent %u frames, %u states\n",
                                      s->cl[i].node, s->cl[i].sent, s->cl[i].states);
    if (!o->quiet && res->rc == 0)
        printf("serve: %u frames, %d client(s), %u crc errors, %u hash mismatches, %u keyframes\n",
               res->frames, res->clients, res->crc_errors, res->hash_mismatches, res->keys_sent);
    free(s->console_region);
    for (unsigned i = 0; i < MS_MAX_BG; i++) free(s->map_snap[i]);
    free(s->file);
    free(s);
    ms_io_shutdown();
    return res->rc;
}

int mosaic_server_main(int argc, char **argv)
{
    ms_server_opts_t o;
    memset(&o, 0, sizeof o);
    o.bind_addr = "0.0.0.0"; o.fps = 60;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--cart") && i + 1 < argc) o.cart = argv[++i];
        else if (!strcmp(argv[i], "--saves") && i + 1 < argc) o.saves = argv[++i];
        else if (!strcmp(argv[i], "--bind") && i + 1 < argc) o.bind_addr = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) o.port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) o.frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc) o.fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--wait") && i + 1 < argc) o.wait_clients = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fast")) o.fast = 1;
        else if (!strcmp(argv[i], "--verbose")) o.verbose = 1;
        else if (!strcmp(argv[i], "--quiet")) o.quiet = 1;
    }
    if (!o.cart) { fprintf(stderr, "usage: mosaic serve --cart game.mcart [--port 7777]\n"); return 1; }
    ms_server_result_t r;
    int rc = ms_server_run(&o, &r);
    return rc == 0 ? 0 : 1;
}
