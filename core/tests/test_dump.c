/* SPDX-License-Identifier: Apache-2.0 */
/* Tests for the dumper: the copy is the cartridge, and copying never asks
 * anyone for permission. */
#include "mosaic/ms_dump.h"
#include "mosaic/ms_save.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %d: %s\n", __LINE__, #c); fails++; } } while (0)

static const char MAN[] = "title=Starfall Demo\ndev=alpha\nsave_id=alpha\n";

#define CAP (16u * 1024u)

static uint8_t g_cart[CAP], g_dst[CAP], g_scratch[MS_DUMP_SCRATCH], g_sram[MS_SAVE_BYTES];

static int build_cart(void)
{
    ms_cart_writer_t w;
    uint8_t pal[2 + 8];
    ms_wr16(pal, 4);
    for (int i = 0; i < 4; i++) ms_wr16(pal + 2 + i * 2, (uint16_t)(0x1111u * (uint32_t)i + 3u));
    ms_cart_writer_begin(&w, g_cart, CAP);
    if (ms_cart_add(&w, "palette", MS_A_PALETTE, pal, sizeof pal) != MS_CART_OK) return -1;
    if (ms_cart_add(&w, "manifest", MS_A_MANIFEST, MAN, sizeof MAN - 1) != MS_CART_OK) return -1;
    return ms_cart_finish(&w);
}

/* ---- memory medium, with knobs for failing ---- */

typedef struct {
    const uint8_t *src; size_t n;
    uint8_t *dst; size_t cap, len;
    size_t fail_after;      /* sink refuses once it would pass this; 0 = never  */
    uint32_t biggest_write;
    size_t read_limit;      /* source refuses reads past this offset; 0 = never */
} media_t;

static int m_read(void *ctx, uint32_t off, void *dst, uint32_t len)
{
    media_t *m = (media_t *)ctx;
    if (m->read_limit && (size_t)off + len > m->read_limit) return -1;
    if ((size_t)off + len > m->n) return -1;
    memcpy(dst, m->src + off, len);
    return 0;
}

static int m_write(void *ctx, const void *src, uint32_t len)
{
    media_t *m = (media_t *)ctx;
    if (len > m->biggest_write) m->biggest_write = len;
    if (m->fail_after && m->len + len > m->fail_after) return -1;
    if (m->len + len > m->cap) return -1;
    memcpy(m->dst + m->len, src, len);
    m->len += len;
    return 0;
}

static int dump(media_t *m, uint32_t total, ms_dump_result_t *r)
{
    ms_dump_io_t io; io.read = m_read; io.write = m_write; io.ctx = m;
    return ms_dump_cart(&io, total, g_scratch, sizeof g_scratch, r);
}

int main(void)
{
    const int n = build_cart();
    printf("dump: built %d-byte cart\n", n);
    CHECK(n > 0);
    if (n <= 0) { printf("dump: FAIL (no cart)\n"); return 1; }
    const uint32_t total = (uint32_t)n;

    /* 1. the copy is the cartridge */
    {
        media_t m; memset(&m, 0, sizeof m);
        m.src = g_cart; m.n = total; m.dst = g_dst; m.cap = sizeof g_dst;
        ms_dump_result_t r;
        CHECK(dump(&m, total, &r) == MS_DUMP_OK);
        CHECK(m.len == total);
        CHECK(memcmp(g_dst, g_cart, total) == 0);
        CHECK(r.bytes == total && r.crc == ms_crc32(g_cart, total));
        CHECK(r.records == 2 && r.verified == 2 && r.failed == 0 && r.ok == 1);
        CHECK(strcmp(r.title, "Starfall Demo") == 0);
        CHECK(m.biggest_write <= 256u);            /* streams, never buffers */
    }

    /* 2. total = 0 copies exactly what the TOC declares */
    {
        media_t m; memset(&m, 0, sizeof m);
        m.src = g_cart; m.n = total; m.dst = g_dst; m.cap = sizeof g_dst;
        ms_dump_result_t r;
        CHECK(dump(&m, 0, &r) == MS_DUMP_OK);
        CHECK(m.len == total);
    }

    /* 3. a short source is copied as far as it goes, and reported */
    {
        media_t m; memset(&m, 0, sizeof m);
        m.src = g_cart; m.n = total; m.dst = g_dst; m.cap = sizeof g_dst;
        ms_dump_result_t r;
        CHECK(dump(&m, total - 5u, &r) == MS_DUMP_E_SIZE);
        CHECK(r.bytes == total - 5u && r.ok == 0);
        CHECK(m.len == total - 5u);
    }

    /* 4. a damaged payload is still copied — refusing is not a safety feature */
    {
        static uint8_t bad[CAP];
        memcpy(bad, g_cart, total);
        bad[total - 1] ^= 0xFF;                    /* last byte of the manifest */
        media_t m; memset(&m, 0, sizeof m);
        m.src = bad; m.n = total; m.dst = g_dst; m.cap = sizeof g_dst;
        ms_dump_result_t r;
        CHECK(dump(&m, total, &r) == MS_DUMP_E_VERIFY);
        CHECK(m.len == total);
        CHECK(memcmp(g_dst, bad, total) == 0);     /* every byte still came out */
        CHECK(r.verified == 1 && r.failed == 1 && r.ok == 0);
    }

    /* 5. a full card stops the copy cleanly */
    {
        media_t m; memset(&m, 0, sizeof m);
        m.src = g_cart; m.n = total; m.dst = g_dst; m.cap = sizeof g_dst;
        m.fail_after = 100;
        ms_dump_result_t r;
        CHECK(dump(&m, total, &r) == MS_DUMP_E_IO);
        CHECK(m.len == MS_CART_HDR);        /* stopped at the TOC */
    }

    /* 6. an unreadable cartridge stops it too */
    {
        media_t m; memset(&m, 0, sizeof m);
        m.src = g_cart; m.n = total; m.dst = g_dst; m.cap = sizeof g_dst;
        m.read_limit = 24;                  /* the TOC read fails */
        ms_dump_result_t r;
        CHECK(dump(&m, total, &r) == MS_DUMP_E_IO);
    }

    /* 7. structure errors are named, and nothing is written for them */
    {
        static uint8_t bad[CAP];
        ms_dump_result_t r;
        memcpy(bad, g_cart, total); bad[0] = 'X';
        media_t m; memset(&m, 0, sizeof m);
        m.src = bad; m.n = total; m.dst = g_dst; m.cap = sizeof g_dst;
        CHECK(dump(&m, total, &r) == MS_DUMP_E_MAGIC);

        memcpy(bad, g_cart, total); bad[MS_CART_HDR + 5] ^= 0x10;   /* a TOC byte */
        memset(&m, 0, sizeof m);
        m.src = bad; m.n = total; m.dst = g_dst; m.cap = sizeof g_dst;
        CHECK(dump(&m, total, &r) == MS_DUMP_E_CRC);

        CHECK(ms_dump_blob(g_cart, total, &r) == MS_DUMP_OK);
        CHECK(ms_dump_blob(bad, total, &r) == MS_DUMP_E_CRC);
        CHECK(ms_dump_blob(g_cart, 8, &r) != MS_DUMP_OK);
        CHECK(ms_dump_blob(g_cart, total, &r) == MS_DUMP_OK);   /* no identity, no key */
    }

    /* 8. the on-device container: ROM mounts, SRAM opens, title survives */
    {
        ms_save_id_t id; memset(&id, 0, sizeof id);
        id.dev_id = ms_id32("alpha"); id.app_id = ms_id32("alpha");
        id.cart_crc = ms_crc32(g_cart, total);
        id.dev_key = "alpha";
        id.loc = MS_SAVE_LOC_CONSOLE;
        ms_save_t s; unsigned fl = 0;
        CHECK(ms_save_format(&s, g_sram, sizeof g_sram, &id, 4, &fl) == MS_SAVE_OK);
        const char body[] = "score=9120\n";
        CHECK(ms_save_write(&s, 0, "highscore", 1, body, sizeof body - 1) == MS_SAVE_OK);

        static uint8_t box[CAP * 2];
        size_t blen = 0;
        CHECK(ms_dump_container_need(total, MS_SAVE_BYTES) == MS_DUMP_HDR + total +
              (((total + 3u) & ~3u) - total) + MS_SAVE_BYTES);
        CHECK(ms_dump_container_build(box, sizeof box, g_cart, total, g_sram,
                                      MS_SAVE_BYTES, "Starfall Demo", &blen) == MS_DUMP_OK);
        CHECK(blen == ms_dump_container_need(total, MS_SAVE_BYTES));

        ms_dump_container_t c;
        CHECK(ms_dump_container_open(&c, box, blen) == MS_DUMP_OK);
        CHECK(c.rom_bytes == total && memcmp(c.rom, g_cart, total) == 0);
        CHECK(c.sram_bytes == MS_SAVE_BYTES);
        CHECK(strcmp(c.title, "Starfall Demo") == 0);

        ms_cart_t cart;
        CHECK(ms_cart_mount(&cart, c.rom, c.rom_bytes) == MS_CART_OK);
        CHECK(strcmp(ms_cart_title(&cart), "Starfall Demo") == 0);

        /* a reader with no key at all can still read the SRAM */
        ms_save_t raw;
        int rrc = ms_save_open_raw(&raw, (void *)(uintptr_t)c.sram, c.sram_bytes, &fl);
        if (rrc) printf("  open_raw rc=%d (%s)\n", rrc, ms_save_err_str(rrc));
        CHECK(rrc == MS_SAVE_OK);
        CHECK(raw.read_only == 1);       /* inspect-only: cannot modify */
        ms_save_ent_t e[MS_SAVE_SLOTS_MAX]; unsigned cnt = 0;
        CHECK(ms_save_list(&raw, e, MS_SAVE_SLOTS_MAX, &cnt) == MS_SAVE_OK);
        CHECK(cnt == 4u && e[0].valid && strcmp(e[0].name, "highscore") == 0);

        /* a damaged container is caught before it is played */
        box[MS_DUMP_HDR + 3] ^= 0xFF;
        CHECK(ms_dump_container_open(&c, box, blen) == MS_DUMP_E_VERIFY);
        CHECK(ms_dump_container_open(&c, box, 8) == MS_DUMP_E_SIZE);
    }

    printf("dump: %s (%d failures)\n", fails ? "FAIL" : "ok", fails);
    return fails ? 1 : 0;
}
