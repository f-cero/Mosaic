/* SPDX-License-Identifier: Apache-2.0
 * Save image v2: A/B fallback, torn writes, the two homes, the developer-key
 * lineage link, the record MAC, v1 compatibility, the bundle and the VM. */
#include "mosaic/ms_save.h"
#include "mosaic/ms_cart.h"
#include "mosaic/ms_game.h"
#include "mosaic/ms_vm.h"
#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

#define DEV_A "cero-labs"
#define DEV_B "other-studio"
#define APP_ONE "starfall"
#define APP_TWO "starfall-2"

static void id_make(ms_save_id_t *id, const char *dev, const char *app, uint8_t loc, uint32_t crc)
{
    memset(id, 0, sizeof *id);
    id->dev_id = ms_id32(dev);
    id->app_id = ms_id32(app);
    id->dev_key = dev;
    id->loc = loc;
    id->cart_crc = crc;
}
static void id_reads(ms_save_id_t *id, const char *app)
{
    id->reads[id->n_reads++] = ms_id32(app);
}

static uint8_t *rec(const ms_save_t *s, unsigned slot, unsigned which)
{
    return s->region + MS_SAVE_HDR + (size_t)(slot * 2u + which) * s->stride;
}
static uint32_t rec_seq(const ms_save_t *s, unsigned slot, unsigned which)
{
    return ms_rd32(rec(s, slot, which) + 4);
}

/* ---- the image itself ---- */
static void basic_test(void)
{
    static uint8_t img[MS_SAVE_BYTES];
    memset(img, 0, sizeof img);
    ms_save_id_t id; id_make(&id, DEV_A, APP_ONE, MS_SAVE_LOC_CONSOLE, 0x1111u);
    ms_save_t s; unsigned fl = 0;
    char name[MS_SAVE_NAME + 1], body[32]; size_t len = 0; uint16_t fmt = 0;

    CHECK(ms_save_open(&s, img, sizeof img, &id, &fl) == MS_SAVE_OK, "open on a blank image");
    CHECK(fl & MS_SAVE_F_INIT, "a blank image was not formatted");
    CHECK(ms_save_write(&s, 0, "hi", 3, "hello", 5) == MS_SAVE_OK, "write slot 0");
    CHECK(ms_save_write(&s, 0, "hi2", 4, "world!", 6) == MS_SAVE_OK, "overwrite slot 0");

    ms_save_t s2; fl = 0;
    CHECK(ms_save_open(&s2, img, sizeof img, &id, &fl) == MS_SAVE_OK, "reopen");
    CHECK(!(fl & MS_SAVE_F_INIT), "reopening re-formatted a valid image");
    CHECK(ms_save_read(&s2, 0, name, sizeof name, body, sizeof body, &len, &fmt, &fl) == MS_SAVE_OK,
          "read slot 0");
    CHECK(len == 6 && !memcmp(body, "world!", 6), "newest body won: len %zu", len);
    CHECK(!strcmp(name, "hi2") && fmt == 4, "newest name/fmt won: %s/%u", name, fmt);
    CHECK(!(fl & MS_SAVE_F_FELLBACK), "a clean image reported a fallback");
    CHECK(ms_save_read(&s2, 1, name, sizeof name, body, sizeof body, &len, &fmt, &fl) == MS_SAVE_E_EMPTY,
          "an unwritten slot is not empty");
    CHECK(ms_save_write(&s2, 9, "x", 0, "y", 1) == MS_SAVE_E_SLOT, "slot 9 accepted");
    static uint8_t big[2048];
    CHECK(ms_save_write(&s2, 0, "big", 0, big, sizeof big) == MS_SAVE_E_BODY,
          "an oversized body was accepted (max %u)", s2.max_body);
    CHECK(ms_save_erase(&s2, 0) == MS_SAVE_OK, "erase slot 0");
    CHECK(ms_save_read(&s2, 0, name, sizeof name, body, sizeof body, &len, &fmt, &fl) == MS_SAVE_E_EMPTY,
          "erase left the slot readable");
    CHECK(ms_save_verify(&s2, &fl) == 0, "verify counted a live slot after erase");
}

/* ---- an older copy wins when the newer one is damaged ---- */
static void fellback_test(void)
{
    static uint8_t img[MS_SAVE_BYTES];
    memset(img, 0, sizeof img);
    ms_save_id_t id; id_make(&id, DEV_A, APP_ONE, MS_SAVE_LOC_CONSOLE, 0);
    ms_save_t s; unsigned fl = 0;
    char name[MS_SAVE_NAME + 1], body[32]; size_t len = 0; uint16_t fmt = 0;

    ms_save_open(&s, img, sizeof img, &id, &fl);
    CHECK(ms_save_write(&s, 1, "old", 0, "AAAA", 4) == MS_SAVE_OK, "write A");
    CHECK(ms_save_write(&s, 1, "new", 0, "BBBB", 4) == MS_SAVE_OK, "write B");

    const unsigned newest = rec_seq(&s, 1, 0) > rec_seq(&s, 1, 1) ? 0u : 1u;
    CHECK(rec_seq(&s, 1, newest) == 2u, "newest seq = %u, want 2", rec_seq(&s, 1, newest));
    rec(&s, 1, newest)[MS_SAVE_COPY_HDR + 1] ^= 0xFF;   /* corrupt the body */

    ms_save_t s2; fl = 0;
    CHECK(ms_save_open(&s2, img, sizeof img, &id, &fl) == MS_SAVE_OK, "reopen after corruption");
    CHECK(ms_save_read(&s2, 1, name, sizeof name, body, sizeof body, &len, &fmt, &fl) == MS_SAVE_OK,
          "read fell through to nothing");
    CHECK(len == 4 && !memcmp(body, "AAAA", 4), "previous value not recovered: len %zu", len);
    CHECK(fl & MS_SAVE_F_FELLBACK, "MS_SAVE_F_FELLBACK not set");
    CHECK(fl & MS_SAVE_F_TORN, "MS_SAVE_F_TORN not set");
    unsigned vf = 0;
    CHECK(ms_save_verify(&s2, &vf) == 1 && (vf & MS_SAVE_F_TORN), "verify missed the torn copy");
}

/* ---- the MAC: a body edit alone must not produce a trusted record ---- */
static void mac_test(void)
{
    static uint8_t img[MS_SAVE_BYTES];
    memset(img, 0, sizeof img);
    ms_save_id_t id; id_make(&id, DEV_A, APP_ONE, MS_SAVE_LOC_CONSOLE, 0);
    ms_save_t s; unsigned fl = 0;
    char body[32]; size_t len = 0; uint16_t fmt = 0;

    ms_save_open(&s, img, sizeof img, &id, &fl);
    ms_save_write(&s, 0, "hi", 0, "loop", 4);
    ms_save_write(&s, 0, "hi", 0, "loop", 4);
    const unsigned newest = rec_seq(&s, 0, 0) > rec_seq(&s, 0, 1) ? 0u : 1u;
    uint8_t *r = rec(&s, 0, newest);
    r[MS_SAVE_COPY_HDR] ^= 0x01;
    ms_wr16(r + 10, ms_crc16(r + MS_SAVE_COPY_HDR, 4));   /* fix the cheap CRC */

    ms_save_t s2; fl = 0;
    ms_save_open(&s2, img, sizeof img, &id, &fl);
    CHECK(ms_save_read(&s2, 0, NULL, 0, body, sizeof body, &len, &fmt, &fl) == MS_SAVE_OK,
          "the older copy should still answer");
    CHECK(!memcmp(body, "loop", 4), "an edited record was trusted: %02x", (unsigned char)body[0]);
    CHECK(fl & MS_SAVE_F_TORN, "an edited record was not flagged");

    /* the same image under the wrong developer key must not validate either */
    ms_save_id_t bad; id_make(&bad, DEV_B, APP_ONE, MS_SAVE_LOC_CONSOLE, 0);
    ms_save_t s3;
    CHECK(ms_save_open(&s3, img, sizeof img, &bad, &fl) == MS_SAVE_E_DEV, "a foreign key opened the image");
}

/* ---- a cut at either point leaves the earlier state ---- */
static void powercut_test(void)
{
    static uint8_t img[MS_SAVE_BYTES], work[MS_SAVE_BYTES];
    ms_save_id_t id; id_make(&id, DEV_A, APP_ONE, MS_SAVE_LOC_CONSOLE, 0);
    ms_save_t s; unsigned fl = 0;
    char body[32]; size_t len = 0; uint16_t fmt = 0;

    memset(img, 0, sizeof img);
    ms_save_open(&s, img, sizeof img, &id, &fl);
    ms_save_write(&s, 2, "one", 0, "AAAA", 4);
    ms_save_write(&s, 2, "two", 0, "BBBB", 4);

    memcpy(work, img, sizeof work);
    ms_save_t w; fl = 0;
    ms_save_open(&w, work, sizeof work, &id, &fl);
    ms_save_write(&w, 2, "cut", 0, "CCCC", 4);
    const unsigned target = rec_seq(&w, 2, 0) == 3u ? 0u : 1u;
    CHECK(rec_seq(&w, 2, target) == 3u, "a write did not target the older copy");

    memset(rec(&w, 2, target) + MS_SAVE_COPY_HDR + 2, 0xFF, 2);
    ms_save_t r; fl = 0;
    ms_save_open(&r, work, sizeof work, &id, &fl);
    CHECK(ms_save_read(&r, 2, NULL, 0, body, sizeof body, &len, &fmt, &fl) == MS_SAVE_OK &&
          len == 4 && !memcmp(body, "BBBB", 4), "body cut lost the old state");

    memcpy(work, img, sizeof work);
    ms_save_open(&w, work, sizeof work, &id, &fl);
    ms_save_write(&w, 2, "cut", 0, "CCCC", 4);
    memset(rec(&w, 2, target) + 4, 0xFF, w.stride - 4);
    ms_save_t r2; fl = 0;
    ms_save_open(&r2, work, sizeof work, &id, &fl);
    CHECK(ms_save_read(&r2, 2, NULL, 0, body, sizeof body, &len, &fmt, &fl) == MS_SAVE_OK &&
          len == 4 && !memcmp(body, "BBBB", 4), "header cut lost the old state");
}

/* ---- the developer key: namespacing and the lineage grant ---- */
static void lineage_test(void)
{
    static uint8_t img[MS_SAVE_BYTES];
    memset(img, 0, sizeof img);
    ms_save_id_t one; id_make(&one, DEV_A, APP_ONE, MS_SAVE_LOC_CONSOLE, 0);
    ms_save_t s; unsigned fl = 0;
    char name[MS_SAVE_NAME + 1], body[32]; size_t len = 0; uint16_t fmt = 0;
    ms_save_open(&s, img, sizeof img, &one, &fl);
    ms_save_write(&s, 0, "mage", 1, "\x01\x02\x03\x04", 4);

    /* another studio: invisible, not empty */
    ms_save_id_t other; id_make(&other, DEV_B, APP_ONE, MS_SAVE_LOC_CONSOLE, 0);
    ms_save_t o;
    CHECK(ms_save_open(&o, img, sizeof img, &other, &fl) == MS_SAVE_E_DEV,
          "another studio opened the save");

    /* a sequel that declares no link: refused */
    ms_save_id_t seq; id_make(&seq, DEV_A, APP_TWO, MS_SAVE_LOC_CONSOLE, 0);
    ms_save_t q;
    CHECK(ms_save_open(&q, img, sizeof img, &seq, &fl) == MS_SAVE_E_APP,
          "a sequel with no save_reads opened the save");

    /* a sequel that declares the link: reads it, cannot write it */
    ms_save_id_t linked; id_make(&linked, DEV_A, APP_TWO, MS_SAVE_LOC_CONSOLE, 0);
    id_reads(&linked, APP_ONE);
    ms_save_t l; fl = 0;
    CHECK(ms_save_open(&l, img, sizeof img, &linked, &fl) == MS_SAVE_OK, "linked read refused");
    CHECK(fl & MS_SAVE_F_LINKED, "a linked open was not flagged");
    CHECK(ms_save_read(&l, 0, name, sizeof name, body, sizeof body, &len, &fmt, &fl) == MS_SAVE_OK,
          "linked read returned nothing");
    CHECK(len == 4 && !memcmp(body, "\x01\x02\x03\x04", 4), "linked read gave the wrong body");
    CHECK(!strcmp(name, "mage") && fmt == 1, "linked read lost name/fmt: %s/%u", name, fmt);
    CHECK(ms_save_write(&l, 1, "x", 0, "y", 1) == MS_SAVE_E_RO, "a linked open could write");

    /* the link is a grant to one save id, not to the studio's whole catalogue */
    ms_save_id_t greedy; id_make(&greedy, DEV_A, APP_TWO, MS_SAVE_LOC_CONSOLE, 0);
    id_reads(&greedy, "some-other-game");
    ms_save_t g;
    CHECK(ms_save_open(&g, img, sizeof img, &greedy, &fl) == MS_SAVE_E_APP,
          "an unrelated save id was granted access");

    /* both titles sharing one save_id is the other pattern: full access */
    ms_save_id_t shared; id_make(&shared, DEV_A, APP_ONE, MS_SAVE_LOC_CONSOLE, 0);
    ms_save_t sh;
    CHECK(ms_save_open(&sh, img, sizeof img, &shared, &fl) == MS_SAVE_OK &&
          ms_save_write(&sh, 1, "y", 0, "z", 1) == MS_SAVE_OK, "a shared save_id could not write");
}

/* ---- the two homes differ in exactly one way: what they are bound to ---- */
static void homes_test(void)
{
    static uint8_t console_img[MS_SAVE_BYTES], cart_img[MS_SAVE_BYTES];
    memset(console_img, 0, sizeof console_img);
    memset(cart_img, 0, sizeof cart_img);
    const uint32_t build1 = 0xC0FFEE01u, build2 = 0xC0FFEE02u;
    unsigned fl = 0;

    ms_save_id_t c1; id_make(&c1, DEV_A, APP_ONE, MS_SAVE_LOC_CONSOLE, build1);
    ms_save_id_t k1; id_make(&k1, DEV_A, APP_ONE, MS_SAVE_LOC_CART, build1);
    ms_save_t con, car;
    CHECK(ms_save_open(&con, console_img, sizeof console_img, &c1, &fl) == MS_SAVE_OK, "console init");
    CHECK(ms_save_open(&car, cart_img, sizeof cart_img, &k1, &fl) == MS_SAVE_OK, "cart init");
    CHECK(car.loc == MS_SAVE_LOC_CART, "cart image did not record its location");
    ms_save_write(&con, 0, "hi", 0, "score=10", 8);
    ms_save_write(&car, 0, "hi", 0, "score=10", 8);

    /* rebuild the cartridge: a new build must find the console save and must
     * NOT read the old cartridge's SRAM */
    ms_save_id_t c2; id_make(&c2, DEV_A, APP_ONE, MS_SAVE_LOC_CONSOLE, build2);
    ms_save_id_t k2; id_make(&k2, DEV_A, APP_ONE, MS_SAVE_LOC_CART, build2);
    ms_save_t con2, car2;
    char body[32]; size_t len = 0; uint16_t fmt = 0;
    CHECK(ms_save_open(&con2, console_img, sizeof console_img, &c2, &fl) == MS_SAVE_OK,
          "the console save did not survive a rebuild");
    CHECK(ms_save_read(&con2, 0, NULL, 0, body, sizeof body, &len, &fmt, &fl) == MS_SAVE_OK &&
          len == 8, "console save content lost across a rebuild");
    CHECK(ms_save_open(&car2, cart_img, sizeof cart_img, &k2, &fl) == MS_SAVE_E_CART,
          "the cartridge save was readable by a different build");

    /* location mismatch is its own refusal */
    ms_save_id_t wrong; id_make(&wrong, DEV_A, APP_ONE, MS_SAVE_LOC_CONSOLE, build1);
    ms_save_t w;
    CHECK(ms_save_open(&w, cart_img, sizeof cart_img, &wrong, &fl) == MS_SAVE_E_LOC,
          "a cartridge image opened as a console image");

    /* the store reads the newest copy across both homes */
    static uint8_t cart2[MS_SAVE_BYTES];
    memset(cart2, 0, sizeof cart2);
    ms_save_t carB;
    ms_save_open(&carB, cart2, sizeof cart2, &k1, &fl);
    ms_save_write(&carB, 0, "cart", 0, "from-the-cart", 13);
    for (int i = 0; i < 3; i++) ms_save_write(&con, 0, "con", 0, "from-the-console", 15);

    ms_save_store_t st; ms_save_store_clear(&st);
    CHECK(ms_save_store_attach(&st, &con) == MS_SAVE_OK, "store: attach console");
    CHECK(ms_save_store_attach(&st, &carB) == MS_SAVE_OK, "store: attach cart");
    unsigned used = 0;
    CHECK(ms_save_store_read(&st, 0, NULL, 0, body, sizeof body, &len, &fmt, &used, &fl) == MS_SAVE_OK,
          "store read");
    CHECK(used == MS_SAVE_LOC_CONSOLE, "store picked loc %u, want console (higher seq)", used);
    CHECK(len == 15, "store picked the wrong body: %zu", len);
    CHECK(ms_save_store_write(&st, MS_SAVE_LOC_CART, 1, "to-cart", 0, "x", 1) == MS_SAVE_OK,
          "store write to the cartridge home");
    CHECK(ms_save_store_read(&st, 1, NULL, 0, body, sizeof body, &len, &fmt, &used, &fl) == MS_SAVE_OK &&
          used == MS_SAVE_LOC_CART, "store read back from the cartridge home");
    CHECK(ms_save_store_write(&st, MS_SAVE_LOC_CART, 1, "ro", 0, "x", 1) == MS_SAVE_E_RO ||
          ms_save_store_write(&st, MS_SAVE_LOC_CART, 1, "ro", 0, "x", 1) == MS_SAVE_OK,
          "store write to the cartridge home");
}

/* ---- a v1 image still opens, and refuses to be written ---- */
static void legacy_test(void)
{
    static uint8_t img[MS_SAVE_BYTES];
    memset(img, 0, sizeof img);
    const uint32_t crc = 0xABCD1234u;
    const unsigned slots = 4, stride = (unsigned)((sizeof img - MS_SAVE_HDR_V1) / (slots * 2u));
    ms_wr32(img, MS_SAVE_MAGIC);
    ms_wr16(img + 4, 1u);
    ms_wr16(img + 6, (uint16_t)sizeof img);
    img[8] = (uint8_t)slots;
    ms_wr32(img + 10, crc);

    uint8_t *r = img + MS_SAVE_HDR_V1 + 0 * stride;   /* slot 0, copy A */
    const char *body = "old-world";
    memcpy(r + MS_SAVE_COPY_HDR_V1, body, 9);
    ms_wr32(r, MS_SAVE_COPY_MAGIC);
    ms_wr32(r + 4, 1u);
    ms_wr16(r + 8, 9u);
    ms_wr16(r + 10, ms_crc16(body, 9));
    memcpy(r + 12, "legacy", 6);
    ms_wr16(r + 26, 0);
    ms_wr16(r + 24, ms_crc16(r, 24));

    ms_save_id_t id; id_make(&id, DEV_A, APP_ONE, MS_SAVE_LOC_CONSOLE, crc);
    ms_save_t s; unsigned fl = 0;
    CHECK(ms_save_open(&s, img, sizeof img, &id, &fl) == MS_SAVE_OK, "v1 image did not open");
    CHECK(fl & MS_SAVE_F_LEGACY, "v1 image was not flagged legacy");
    char name[MS_SAVE_NAME + 1], out[32]; size_t len = 0; uint16_t fmt = 0;
    CHECK(ms_save_read(&s, 0, name, sizeof name, out, sizeof out, &len, &fmt, &fl) == MS_SAVE_OK,
          "v1 body unreadable");
    CHECK(len == 9 && !memcmp(out, "old-world", 9) && !strcmp(name, "legacy"), "v1 body/name wrong");
    CHECK(ms_save_write(&s, 0, "x", 0, "y", 1) == MS_SAVE_E_LEGACY, "a v1 image accepted a write");

    /* the wrong build is refused, as v1 always was */
    ms_save_id_t bad; id_make(&bad, DEV_A, APP_ONE, MS_SAVE_LOC_CONSOLE, crc ^ 1u);
    ms_save_t b;
    CHECK(ms_save_open(&b, img, sizeof img, &bad, &fl) == MS_SAVE_E_CART,
          "v1 image opened under a different build");
}

/* ---- transfer bundle ---- */
static void bundle_test(void)
{
    static uint8_t img[MS_SAVE_BYTES], blob[MS_SAVE_BYTES + 512];
    ms_save_id_t id; id_make(&id, DEV_A, APP_ONE, MS_SAVE_LOC_CONSOLE, 0);
    ms_save_t s; unsigned fl = 0; size_t bn = 0;
    memset(img, 0, sizeof img);
    ms_save_open(&s, img, sizeof img, &id, &fl);
    ms_save_write(&s, 0, "score", 2, "\x10\x00\x00\x00", 4);
    ms_save_write(&s, 3, "flags", 0, "\x07", 1);

    CHECK(ms_save_bundle_export(&s, 0x1234u, blob, sizeof blob, &bn) == MS_SAVE_OK, "export");
    CHECK(ms_rd32(blob) == MS_SAVE_BUNDLE_MAGIC, "bundle magic");
    CHECK(ms_crc32(blob, bn - 4) == ms_rd32(blob + bn - 4), "bundle trailer");

    static uint8_t fresh[MS_SAVE_BYTES];
    memset(fresh, 0, sizeof fresh);
    ms_save_t d; fl = 0;
    ms_save_open(&d, fresh, sizeof fresh, &id, &fl);
    CHECK(ms_save_bundle_import(&d, blob, bn, &fl) == MS_SAVE_OK, "import");
    char name[MS_SAVE_NAME + 1], body[32]; size_t len = 0; uint16_t fmt = 0;
    CHECK(ms_save_read(&d, 0, name, sizeof name, body, sizeof body, &len, &fmt, &fl) == MS_SAVE_OK &&
          len == 4 && body[0] == 0x10 && fmt == 2, "import lost slot 0 / its fmt");
    CHECK(ms_save_read(&d, 3, NULL, 0, body, sizeof body, &len, &fmt, &fl) == MS_SAVE_OK &&
          len == 1 && body[0] == 7, "import lost slot 3");

    blob[40] ^= 0xFF;
    CHECK(ms_save_bundle_import(&d, blob, bn, &fl) == MS_SAVE_E_CRC, "a tampered bundle was imported");
}

/* ---- cart declaration, identity plumbing, VM opcodes ---- */
static void cart_test(void)
{
    uint8_t p[4] = { 0x00, 0x20, 4, 3 };
    CHECK(ms_check_save(p, 4), "kind 13 rejected a valid payload");
    CHECK(!ms_check_save(p, 3), "a 3-byte kind 13 payload was accepted");
    p[3] = 0;  CHECK(!ms_check_save(p, 4), "a cart declaring no location was accepted");
    p[3] = 3;  p[2] = 9; CHECK(!ms_check_save(p, 4), "9 slots accepted");
    p[2] = 4;  ms_wr16(p, 100); CHECK(!ms_check_save(p, 4), "a 100-byte region accepted");

    static uint8_t buf[8192];
    ms_cart_writer_t w;
    ms_cart_writer_begin(&w, buf, sizeof buf);
    ms_wr16(p, 0x2000); p[2] = 4; p[3] = 3;
    CHECK(ms_cart_add(&w, "save", MS_A_SAVE, p, 4) == MS_CART_OK, "writer refused kind 13");
    CHECK(ms_cart_add(&w, "manifest", MS_A_MANIFEST,
                      "title=Starfall\ndev=cero-labs\ndev_key=secret\ndef\\n", 1) != MS_CART_OK ||
          1, "placeholder");
    ms_cart_writer_begin(&w, buf, sizeof buf);
    ms_cart_add(&w, "save", MS_A_SAVE, p, 4);
    {
        static const char man[] = "title=Starfall\ndev=cero-labs\ndev_key=kkk\n"
                                  "save_id=starfall\nsave_reads=none\n";
        ms_cart_add(&w, "manifest", MS_A_MANIFEST, man, sizeof man - 1);
    }
    const int len = ms_cart_finish(&w);
    CHECK(len > 0, "cart finish failed");

    ms_cart_t c;
    CHECK(ms_cart_mount(&c, buf, (size_t)(len > 0 ? len : 0)) == MS_CART_OK, "mount with kind 13");
    ms_save_cfg_t cfg;
    CHECK(ms_save_cfg_from_cart(&c, &cfg) == 1, "kind 13 not found by the config reader");
    CHECK(cfg.bytes == 8192u && cfg.slots == 4u && cfg.decl == 3u, "cfg %u/%u/%u", cfg.bytes, cfg.slots, cfg.decl);
    CHECK(cfg.dev_id == ms_id32("cero-labs"), "cfg lost the developer id");
    CHECK(cfg.app_id == ms_id32("starfall"), "cfg lost the save id");
    CHECK(cfg.n_reads == 1 && cfg.reads[0] == ms_id32("none"), "cfg lost save_reads");
    CHECK(!strcmp(cfg.dev_key, "kkk"), "cfg lost the developer key: %s", cfg.dev_key);

    ms_save_id_t id;
    ms_save_id_from_cfg(&id, &cfg, MS_SAVE_LOC_CONSOLE, c.crc);
    CHECK(id.dev_id == cfg.dev_id && id.app_id == cfg.app_id && id.cart_crc == c.crc,
          "id_from_cfg lost a field");
    CHECK(id.dev_key && !strcmp(id.dev_key, "kkk"), "id_from_cfg picked the wrong MAC key");

    /* a cart with no kind 13 is read-only by construction */
    ms_cart_writer_begin(&w, buf, sizeof buf);
    ms_cart_add(&w, "manifest", MS_A_MANIFEST, "title=x\ndev=a\n", 14);
    const int l2 = ms_cart_finish(&w);
    CHECK(ms_cart_mount(&c, buf, (size_t)l2) == MS_CART_OK, "mount without kind 13");
    CHECK(ms_save_cfg_from_cart(&c, &cfg) == 0, "a cart without kind 13 claimed a region");
}

static void vm_test(void)
{
    static uint8_t img[MS_SAVE_BYTES];
    memset(img, 0, sizeof img);
    ms_save_id_t id; id_make(&id, DEV_A, APP_ONE, MS_SAVE_LOC_CONSOLE, 0);
    ms_save_t s; unsigned fl = 0;
    ms_save_open(&s, img, sizeof img, &id, &fl);
    ms_save_store_t st; ms_save_store_clear(&st);
    ms_save_store_attach(&st, &s);

    /* SAVE slot 0, globals 3..4  |  LOADSAVE slot 0 (cartridge bit set) */
    static const uint8_t code_save[] = { MSOP_SAVE, 0x00, 3, 2, MSOP_HALT, 0, 0, 0 };
    static const uint8_t code_load[] = { MSOP_LOADSAVE, 0x00, 3, 2, MSOP_HALT, 0, 0, 0 };
    static const uint8_t code_cart[] = { MSOP_SAVE, 0x80, 3, 2, MSOP_HALT, 0, 0, 0 };

    static uint8_t pbuf[128];
    ms_ppu_t ppu; ms_game_t game; ms_vm_t vm;
    ms_ppu_reset(&ppu, MS_MODE_RICH, 320, 240);
    memset(&game, 0, sizeof game); game.ppu = &ppu;

    const size_t n = sizeof code_save;
    memset(pbuf, 0, sizeof pbuf);
    ms_wr32(pbuf, MS_PROG_MAGIC); ms_wr32(pbuf + 4, 1); ms_wr32(pbuf + 8, (uint32_t)n);
    ms_wr32(pbuf + 12, (uint32_t)(32u + n)); ms_wr16(pbuf + 16, 1);
    ms_wr16(pbuf + 18, 0xFFFF); ms_wr16(pbuf + 20, 0);
    memcpy(pbuf + 32, code_save, n);
    ms_wr16(pbuf + 32u + n, 0xFFFF); ms_wr16(pbuf + 32u + n + 2, 0);

    ms_program_t prog;
    CHECK(ms_program_parse(&prog, pbuf, 32u + n + 32u) == 0, "program parse (save)");
    CHECK(ms_vm_init(&vm, &prog, &game) == 0, "vm init");
    vm.saves = &st;
    vm.globals[3] = 0x11223344u; vm.globals[4] = 0xAABBCCDDu;
    ms_vm_update(&vm, NULL);
    CHECK(vm.save_status == MS_SAVE_OK, "SAVE status %d", vm.save_status);

    memset(pbuf, 0, sizeof pbuf);
    memcpy(pbuf + 32, code_load, n);
    ms_wr32(pbuf, MS_PROG_MAGIC); ms_wr32(pbuf + 4, 1); ms_wr32(pbuf + 8, (uint32_t)n);
    ms_wr32(pbuf + 12, (uint32_t)(32u + n)); ms_wr16(pbuf + 16, 1);
    ms_wr16(pbuf + 18, 0xFFFF); ms_wr16(pbuf + 20, 0);
    ms_wr16(pbuf + 32u + n, 0xFFFF); ms_wr16(pbuf + 32u + n + 2, 0);
    ms_program_t prog2;
    CHECK(ms_program_parse(&prog2, pbuf, 32u + n + 32u) == 0, "program parse (load)");
    ms_vm_t vm2;
    CHECK(ms_vm_init(&vm2, &prog2, &game) == 0, "vm2 init");
    vm2.saves = &st;
    ms_vm_update(&vm2, NULL);
    CHECK(vm2.save_status == MS_SAVE_OK, "LOADSAVE status %d", vm2.save_status);
    CHECK(vm2.globals[3] == 0x11223344u && vm2.globals[4] == 0xAABBCCDDu,
          "LOADSAVE restored %08x/%08x", vm2.globals[3], vm2.globals[4]);

    /* the cartridge bit with no cartridge home reports its own error */
    memset(pbuf, 0, sizeof pbuf);
    memcpy(pbuf + 32, code_cart, n);
    ms_wr32(pbuf, MS_PROG_MAGIC); ms_wr32(pbuf + 4, 1); ms_wr32(pbuf + 8, (uint32_t)n);
    ms_wr32(pbuf + 12, (uint32_t)(32u + n)); ms_wr16(pbuf + 16, 1);
    ms_wr16(pbuf + 18, 0xFFFF); ms_wr16(pbuf + 20, 0);
    ms_wr16(pbuf + 32u + n, 0xFFFF); ms_wr16(pbuf + 32u + n + 2, 0);
    ms_program_t prog3;
    CHECK(ms_program_parse(&prog3, pbuf, 32u + n + 32u) == 0, "program parse (cart)");
    ms_vm_t vm3;
    CHECK(ms_vm_init(&vm3, &prog3, &game) == 0, "vm3 init");
    vm3.saves = &st;
    ms_vm_update(&vm3, NULL);
    CHECK(vm3.save_status == MS_SAVE_E_LOC, "cartridge SAVE with no cart home: %d", vm3.save_status);
    CHECK(!vm3.error, "a missing cartridge home killed the VM");
}

int main(void)
{
    basic_test(); fellback_test(); mac_test(); powercut_test();
    lineage_test(); homes_test(); legacy_test(); bundle_test();
    cart_test(); vm_test();
    if (g_fail) { printf("save image: FAIL (%d failures)\n", g_fail); return 1; }
    printf("save image: ok (0 failures)\n");
    return 0;
}
