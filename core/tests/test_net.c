/* SPDX-License-Identifier: Apache-2.0 */
/* Mosaic — test_net.c: the loopback acceptance test for MSP/1.
 *
 * A server and two display nodes on 127.0.0.1. The clients render from the
 * register stream alone; their per-frame hashes must equal the server's, which
 * is the only honest way to say "a thin client reproduces the frame" without
 * owning two consoles. The second case joins late and must converge through a
 * keyframe + map resync. */
#if defined(_WIN32)
#include <stdio.h>
int main(void) { printf("net: skipped on this platform (POSIX sockets)\n"); return 0; }
#else
#include "net.h"
#include "mosaic/ms_build.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail;

#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct {
    ms_server_opts_t o;
    ms_server_result_t r;
    int port;
    volatile int ready;
} sjob_t;

static void *srv_thread(void *p)
{
    sjob_t *j = (sjob_t *)p;
    ms_server_run(&j->o, &j->r);
    return NULL;
}

typedef struct { ms_client_opts_t o; ms_client_result_t r; int port; } cjob_t;

static void *cli_thread(void *p)
{
    cjob_t *j = (cjob_t *)p;
    j->o.port = j->port;
    ms_client_run(&j->o, &j->r);
    return NULL;
}

static void wait_ready(sjob_t *j)
{
    for (int i = 0; i < 2000 && !j->ready; i++) usleep(1000);
}

static int build_cart(const char *srcdir, char *out, size_t cap)
{
    char mpj[512];
    snprintf(mpj, sizeof mpj, "%s/examples/starfall/starfall.mpj", srcdir);
    static ms_build_result_t r;
    if (ms_build_project_opts(mpj, &r, 0) != 0) {
        printf("  FAIL: build %s: %s\n", mpj, r.error);
        g_fail++;
        return -1;
    }
    snprintf(out, cap, "/tmp/mosaic_net_test_%d.mcart", (int)getpid());
    FILE *f = fopen(out, "wb");
    if (!f || fwrite(r.cart, 1, r.cart_len, f) != r.cart_len) { if (f) fclose(f); g_fail++; return -1; }
    fclose(f);
    return 0;
}

/* Two clients from frame 0. */
static void case_two_clients(const char *cart)
{
    printf("two clients, 300 frames\n");
    sjob_t s;
    memset(&s, 0, sizeof s);
    s.o.cart = cart; s.o.bind_addr = "127.0.0.1"; s.o.port = 0;
    s.o.frames = 300; s.o.fps = 600; s.o.wait_clients = 2; s.o.quiet = 1;
    s.o.port_out = &s.port; s.o.ready = &s.ready;

    pthread_t st;
    pthread_create(&st, NULL, srv_thread, &s);
    wait_ready(&s);
    CHECK(s.port > 0, "server never reported a port");

    cjob_t a, b;
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    a.o.host = b.o.host = "127.0.0.1"; a.o.cart = b.o.cart = cart;
    a.o.quiet = b.o.quiet = 1; a.o.frames = b.o.frames = 0;
    a.port = b.port = s.port;
    pthread_t at, bt;
    pthread_create(&at, NULL, cli_thread, &a);
    pthread_create(&bt, NULL, cli_thread, &b);

    pthread_join(st, NULL);
    pthread_join(at, NULL);
    pthread_join(bt, NULL);

    CHECK(s.r.rc == 0, "server rc %d", s.r.rc);
    CHECK(s.r.clients == 2, "clients %d", s.r.clients);
    CHECK(s.r.crc_errors == 0, "crc errors %u", s.r.crc_errors);
    CHECK(s.r.hash_mismatches == 0, "hash mismatches %u", s.r.hash_mismatches);
    CHECK(s.r.frames == 300, "frames %u", s.r.frames);
    CHECK(a.r.rc == 0 && b.r.rc == 0, "client rc %d/%d", a.r.rc, b.r.rc);
    CHECK(a.r.frames > 290 && b.r.frames > 290, "client frames %u/%u", a.r.frames, b.r.frames);
    CHECK(a.r.final_hash == s.r.final_hash, "client A hash %016llx != server %016llx",
          (unsigned long long)a.r.final_hash, (unsigned long long)s.r.final_hash);
    CHECK(b.r.final_hash == s.r.final_hash, "client B hash %016llx != server %016llx",
          (unsigned long long)b.r.final_hash, (unsigned long long)s.r.final_hash);
    printf("  server %u frames, hash %016llx; A %u frames, B %u frames\n",
           s.r.frames, (unsigned long long)s.r.final_hash, a.r.frames, b.r.frames);
}

/* A second client arrives mid-session: keyframe + map resync, then convergence. */
static void case_late_join(const char *cart)
{
    printf("late join, 240 frames\n");
    sjob_t s;
    memset(&s, 0, sizeof s);
    s.o.cart = cart; s.o.bind_addr = "127.0.0.1"; s.o.port = 0;
    s.o.frames = 240; s.o.fps = 600; s.o.wait_clients = 1; s.o.quiet = 1;
    s.o.port_out = &s.port; s.o.ready = &s.ready;

    pthread_t st;
    pthread_create(&st, NULL, srv_thread, &s);
    wait_ready(&s);

    cjob_t a, b;
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    a.o.host = b.o.host = "127.0.0.1"; a.o.cart = b.o.cart = cart;
    a.o.quiet = b.o.quiet = 1;
    a.port = b.port = s.port;
    pthread_t at, bt;
    pthread_create(&at, NULL, cli_thread, &a);
    usleep(120000);
    pthread_create(&bt, NULL, cli_thread, &b);

    pthread_join(st, NULL);
    pthread_join(at, NULL);
    pthread_join(bt, NULL);

    CHECK(s.r.rc == 0, "server rc %d", s.r.rc);
    CHECK(s.r.hash_mismatches == 0, "hash mismatches %u", s.r.hash_mismatches);
    CHECK(s.r.keys_sent >= 2, "keyframes %u", s.r.keys_sent);
    CHECK(b.r.frames > 0, "late client applied no frames");
    CHECK(b.r.final_hash == s.r.final_hash, "late client hash %016llx != server %016llx",
          (unsigned long long)b.r.final_hash, (unsigned long long)s.r.final_hash);
    printf("  server %u frames; A %u; late B %u from frame ~%u\n",
           s.r.frames, a.r.frames, b.r.frames, s.r.frames > b.r.frames ? s.r.frames - b.r.frames : 0);
}

int main(int argc, char **argv)
{
    const char *src = argc > 1 ? argv[1] : ".";
    char cart[512];
    if (build_cart(src, cart, sizeof cart) != 0) {
        printf("net: FAILED (could not build a cart)\n");
        return 1;
    }
    case_two_clients(cart);
    case_late_join(cart);
    unlink(cart);
    printf("net: %s (%d failures)\n", g_fail ? "FAILED" : "ok", g_fail);
    return g_fail ? 1 : 0;
}
#endif
