/* SPDX-License-Identifier: Apache-2.0 */
/* Save image: round-trip, A/B fallback, torn-write recovery, cart binding,
 * transfer bundle, kind 13 validation and the SAVE/LOADSAVE opcodes. */
#include "mosaic/ms_save.h"
#include "mosaic/ms_cart.h"
#include "mosaic/ms_vm.h"
#include "mosaic/ms_game.h"
#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

#define CART_A 0xDEADBEEFu
#define CART_B 0x0BADF00Du

/* Mirrors the record layout independently of ms_save.c, so a layout change
 * on one side breaks the test instead of silently agreeing. */
static uint8_t *rec(const ms_save_t *s, unsigned slot, unsigned which)
{
    return s->region + MS_SAVE_HDR + (size_t)(slot * 2u + which) * s->stride;
}
static uint32_t rec_seq(const ms_save_t *s, unsigned slot, unsigned which)
{
    return ms_rd32(rec(s, slot, which) + 4);
}

static void basic_test(void)
{
    static uint8_t img[MS_SAVE_BYTES];
    memset(img, 0, sizeof img);
    ms_save_t s; unsigned fl = 0;
    char name[MS_SAVE_NAME + 1], body[32];
    size_t len = 0;

    CHECK(ms_save_open(&s, img, sizeof img, CART_A, &fl) == MS_SAVE_OK, "open on a blank image");
    CHECK(fl & MS_SAVE_F_INIT, "a blank image was not formatted");

    CHECK(ms_save_write(&s, 0, "hi", "hello", 5) == MS_SAVE_OK, "write slot 0");
    CHECK(ms_save_write(&s, 0, "hi2", "world!", 6) == MS_SAVE_OK, "overwrite slot 0");

    ms_save_t s2; fl = 0;
    CHECK(ms_save_open(&s2, img, sizeof img, CART_A, &fl) == MS_SAVE_OK, "reopen");
    CHECK(!(fl & MS_SAVE_F_INIT), "reopening re-formatted a valid image");
    CHECK(ms_save_read(&s2, 0, name, sizeof name, body, sizeof body, &len, &fl) == MS_SAVE_OK,
          "read slot 0");
    CHECK(len == 6 && !memcmp(body, "world!", 6), "newest body won: len %zu", len);
    CHECK(!strcmp(name, "hi2"), "newest name won: %s", name);
    CHECK(!(fl & MS_SAVE_F_FELLBACK), "a clean image reported a fallback");

    CHECK(ms_save_read(&s2, 1, name, sizeof name, body, sizeof body, &len, &fl) == MS_SAVE_E_EMPTY,
          "an unwritten slot is not empty");
    CHECK(ms_save_write(&s2, 9, "x", "y", 1) == MS_SAVE_E_SLOT, "slot 9 accepted (slots=%u)", s2.slots);
    static uint8_t big[2048];
    CHECK(ms_save_write(&s2, 0, "big", big, sizeof big) == MS_SAVE_E_BODY,
          "an oversized body was accepted (max %u)", s2.max_body);

    /* cart binding: a different build must be refused, not silently empty */
    ms_save_t s3; fl = 0;
    CHECK(ms_save_open(&s3, img, sizeof img, CART_B, &fl) == MS_SAVE_E_CART,
          "a save from another cart was accepted");
    CHECK(ms_save_verify(&s3, &fl) == MS_SAVE_E_CART, "verify ignored the cart binding");

    CHECK(ms_save_erase(&s2, 0) == MS_SAVE_OK, "erase slot 0");
    CHECK(ms_save_read(&s2, 0, name, sizeof name, body, sizeof body, &len, &fl) == MS_SAVE_E_EMPTY,
          "erase left the slot readable");
    CHECK(ms_save_verify(&s2, &fl) == 0, "verify counted a live slot after erase");
}

/* Newest copy carries a bad body CRC: the older one must win, and say so. */
static void fellback_test(void)
{
    static uint8_t img[MS_SAVE_BYTES];
    memset(img, 0, sizeof img);
    ms_save_t s; unsigned fl = 0;
    char name[MS_SAVE_NAME + 1], body[32]; size_t len = 0;

    ms_save_open(&s, img, sizeof img, CART_A, &fl);
    CHECK(ms_save_write(&s, 1, "old", "AAAA", 4) == MS_SAVE_OK, "write A");
    CHECK(ms_save_write(&s, 1, "new", "BBBB", 4) == MS_SAVE_OK, "write B");

    const unsigned newest = rec_seq(&s, 1, 0) > rec_seq(&s, 1, 1) ? 0u : 1u;
    CHECK(rec_seq(&s, 1, newest) == 2u, "newest seq = %u, want 2", rec_seq(&s, 1, newest));
    rec(&s, 1, newest)[MS_SAVE_COPY_HDR + 1] ^= 0xFF;   /* corrupt the body */

    ms_save_t s2; fl = 0;
    CHECK(ms_save_open(&s2, img, sizeof img, CART_A, &fl) == MS_SAVE_OK, "reopen after corruption");
    CHECK(ms_save_read(&s2, 1, name, sizeof name, body, sizeof body, &len, &fl) == MS_SAVE_OK,
          "read fell through to nothing");
    CHECK(len == 4 && !memcmp(body, "AAAA", 4), "previous value not recovered: len %zu", len);
    CHECK(!strcmp(name, "old"), "previous name not recovered: %s", name);
    CHECK(fl & MS_SAVE_F_FELLBACK, "MS_SAVE_F_FELLBACK not set");
    CHECK(fl & MS_SAVE_F_TORN, "MS_SAVE_F_TORN not set");

    unsigned vf = 0;
    CHECK(ms_save_verify(&s2, &vf) == 1, "verify live count");
    CHECK(vf & MS_SAVE_F_TORN, "verify missed the torn copy");
}

/* A cut mid-write must leave the other copy authoritative. Two cut points:
 * inside the body, and inside the header (before the commit word). */
static void powercut_test(void)
{
    static uint8_t img[MS_SAVE_BYTES], work[MS_SAVE_BYTES];
    ms_save_t s; unsigned fl = 0;
    char name[MS_SAVE_NAME + 1], body[32]; size_t len = 0;

    memset(img, 0, sizeof img);
    ms_save_open(&s, img, sizeof img, CART_A, &fl);
    ms_save_write(&s, 2, "one", "AAAA", 4);
    ms_save_write(&s, 2, "two", "BBBB", 4);

    /* the next write lands in the older copy — exactly what a power cut hits */
    memcpy(work, img, sizeof work);
    ms_save_t w; fl = 0;
    ms_save_open(&w, work, sizeof work, CART_A, &fl);
    ms_save_write(&w, 2, "cut", "CCCC", 4);
    const unsigned target = rec_seq(&w, 2, 0) == 3u ? 0u : 1u;
    CHECK(rec_seq(&w, 2, target) == 3u, "a write did not target the older copy");

    /* (a) body cut: half the body is 0xFF, header intact */
    memset(rec(&w, 2, target) + MS_SAVE_COPY_HDR + 2, 0xFF, 2);
    ms_save_t r; fl = 0;
    CHECK(ms_save_open(&r, work, sizeof work, CART_A, &fl) == MS_SAVE_OK, "open after body cut");
    CHECK(ms_save_read(&r, 2, name, sizeof name, body, sizeof body, &len, &fl) == MS_SAVE_OK,
          "read after body cut");
    CHECK(len == 4 && !memcmp(body, "BBBB", 4), "body cut lost the old state: len %zu", len);

    /* (b) header cut: everything from the seq field on is 0xFF, so hcrc
     *     cannot validate — an in-flight record must never be trusted. */
    memcpy(work, img, sizeof work);
    ms_save_open(&w, work, sizeof work, CART_A, &fl);
    ms_save_write(&w, 2, "cut", "CCCC", 4);
    memset(rec(&w, 2, target) + 4, 0xFF, w.stride - 4);
    ms_save_t r2; fl = 0;
    CHECK(ms_save_open(&r2, work, sizeof work, CART_A, &fl) == MS_SAVE_OK, "open after header cut");
    CHECK(ms_save_read(&r2, 2, name, sizeof name, body, sizeof body, &len, &fl) == MS_SAVE_OK,
          "read after header cut");
    CHECK(len == 4 && !memcmp(body, "BBBB", 4), "header cut lost the old state: len %zu", len);

    /* (c) a fully written record after the cut still commits normally */
    memcpy(work, img, sizeof work);
    ms_save_open(&w, work, sizeof work, CART_A, &fl);
    ms_save_write(&w, 2, "three", "DDDD", 4);
    ms_save_t r3;
    CHECK(ms_save_open(&r3, work, sizeof work, CART_A, &fl) == MS_SAVE_OK, "open after a clean write");
    CHECK(ms_save_read(&r3, 2, name, sizeof name, body, sizeof body, &len, &fl) == MS_SAVE_OK &&
          !memcmp(body, "DDDD", 4), "clean write after a cut did not commit");
}

static void bundle_test(void)
{
    static uint8_t img[MS_SAVE_BYTES], bundle[MS_SAVE_BYTES + 256];
    ms_save_t s; unsigned fl = 0;
    char name[MS_SAVE_NAME + 1], body[32]; size_t len = 0, bn = 0;

    memset(img, 0, sizeof img);
    ms_save_open(&s, img, sizeof img, CART_A, &fl);
    ms_save_write(&s, 0, "score", "\x10\x00\x00\x00", 4);
    ms_save_write(&s, 3, "flags", "\x07", 1);

    CHECK(ms_save_bundle_export(&s, 0x1234u, bundle, sizeof bundle, &bn) == MS_SAVE_OK, "export");
    CHECK(ms_rd32(bundle) == MS_SAVE_BUNDLE_MAGIC, "bundle magic");
    CHECK(ms_crc32(bundle, bn - 4) == ms_rd32(bundle + bn - 4), "bundle trailer");

    static uint8_t fresh[MS_SAVE_BYTES];
    memset(fresh, 0, sizeof fresh);
    ms_save_t d; fl = 0;
    ms_save_open(&d, fresh, sizeof fresh, CART_A, &fl);
    CHECK(ms_save_bundle_import(&d, bundle, bn, &fl) == MS_SAVE_OK, "import");
    CHECK(ms_save_read(&d, 0, name, sizeof name, body, sizeof body, &len, &fl) == MS_SAVE_OK &&
          len == 4 && body[0] == 0x10, "import lost slot 0");
    CHECK(!strcmp(name, "score"), "import lost the name: %s", name);
    CHECK(ms_save_read(&d, 3, name, sizeof name, body, sizeof body, &len, &fl) == MS_SAVE_OK &&
          len == 1 && body[0] == 7, "import lost slot 3");

    bundle[40] ^= 0xFF;
    CHECK(ms_save_bundle_import(&d, bundle, bn, &fl) == MS_SAVE_E_CRC, "a tampered bundle was imported");

    /* a bundle from another cart is refused before anything is written */
    memset(fresh, 0, sizeof fresh);
    ms_save_open(&d, fresh, sizeof fresh, CART_B, &fl);
    bundle[40] ^= 0xFF;
    ms_save_bundle_export(&s, 0, bundle, sizeof bundle, &bn);
    CHECK(ms_save_bundle_import(&d, bundle, bn, &fl) == MS_SAVE_E_CART, "cross-cart bundle accepted");
}

static void cart_kind13_test(void)
{
    uint8_t p[4] = { 0x00, 0x20, 4, 0 };   /* 8192 bytes, 4 slots */
    CHECK(ms_check_save(p, 4), "kind 13 rejected a valid payload");
    CHECK(!ms_check_save(p, 3), "a 3-byte kind 13 payload was accepted");
    p[2] = 9;  CHECK(!ms_check_save(p, 4), "9 slots accepted");
    p[2] = 4;  ms_wr16(p, 100);  CHECK(!ms_check_save(p, 4), "a 100-byte region accepted");
    ms_wr16(p, 0xFFFF); CHECK(!ms_check_save(p, 4), "a 65535-byte region accepted");
    ms_wr16(p, 0x2000); p[3] = 1; CHECK(!ms_check_save(p, 4), "a set reserved byte accepted");

    static uint8_t buf[8192];
    ms_cart_writer_t w;
    ms_cart_writer_begin(&w, buf, sizeof buf);
    p[3] = 0;
    CHECK(ms_cart_add(&w, "save", MS_A_SAVE, p, 4) == MS_CART_OK, "writer refused kind 13");
    const int len = ms_cart_finish(&w);
    CHECK(len > 0, "cart finish failed");

    ms_cart_t c;
    CHECK(ms_cart_mount(&c, buf, (size_t)(len > 0 ? len : 0)) == MS_CART_OK, "mount with kind 13");
    unsigned bytes = 0, slots = 0;
    CHECK(ms_save_cfg_from_cart(&c, &bytes, &slots) == 1, "kind 13 not found by the config reader");
    CHECK(bytes == 8192u && slots == 4u, "cfg read back %u/%u", bytes, slots);
    CHECK(c.crc != 0, "mount did not expose the TOC crc");

    /* a cart with no save asset reads as read-only, at the documented default */
    ms_cart_writer_begin(&w, buf, sizeof buf);
    ms_cart_add(&w, "manifest", MS_A_MANIFEST, "title=x\n", 8);
    const int l2 = ms_cart_finish(&w);
    CHECK(ms_cart_mount(&c, buf, (size_t)l2) == MS_CART_OK, "mount without kind 13");
    bytes = slots = 0;
    CHECK(ms_save_cfg_from_cart(&c, &bytes, &slots) == 0, "a cart without kind 13 claimed a region");
}

static const ms_program_t *prog_make(uint8_t *buf, const uint8_t *code, size_t n)
{
    static ms_program_t p;
    const size_t total = 32u + n + 32u;
    memset(buf, 0, total);
    ms_wr32(buf + 0, MS_PROG_MAGIC);
    ms_wr32(buf + 4, 1);
    ms_wr32(buf + 8, (uint32_t)n);
    ms_wr32(buf + 12, (uint32_t)(32u + n));
    ms_wr16(buf + 16, 1);
    ms_wr16(buf + 18, 0xFFFF);
    ms_wr16(buf + 20, 0);              /* update_pc = 0 */
    memcpy(buf + 32, code, n);
    ms_wr16(buf + 32u + n, 0xFFFF);
    ms_wr16(buf + 32u + n + 2, 0);
    if (ms_program_parse(&p, buf, total) != 0) return NULL;
    return &p;
}

static void vm_save_test(void)
{
    static uint8_t img[MS_SAVE_BYTES];
    memset(img, 0, sizeof img);
    ms_save_t save; unsigned fl = 0;
    CHECK(ms_save_open(&save, img, sizeof img, CART_A, &fl) == MS_SAVE_OK, "vm: open image");

    static const uint8_t save_code[] = { MSOP_SAVE, 0, 3, 2, MSOP_HALT, 0, 0, 0 };
    static const uint8_t load_code[] = { MSOP_LOADSAVE, 0, 3, 2, MSOP_HALT, 0, 0, 0 };
    uint8_t pbuf[128];
    ms_ppu_t ppu; ms_game_t game; ms_vm_t vm;

    ms_ppu_reset(&ppu, MS_MODE_RICH, 320, 240);
    memset(&game, 0, sizeof game); game.ppu = &ppu;

    const ms_program_t *p1 = prog_make(pbuf, save_code, sizeof save_code);
    CHECK(p1 != NULL, "vm: program parse (save)");
    if (!p1) return;
    CHECK(ms_vm_init(&vm, p1, &game) == 0, "vm init");
    vm.save = &save;
    vm.globals[3] = 0x11223344u; vm.globals[4] = 0xAABBCCDDu;
    ms_vm_update(&vm, NULL);
    CHECK(!vm.error, "vm error: %s", vm.errmsg);
    CHECK(vm.save_status == MS_SAVE_OK, "SAVE status %d", vm.save_status);

    uint8_t pbuf2[128];
    const ms_program_t *p2 = prog_make(pbuf2, load_code, sizeof load_code);
    CHECK(p2 != NULL, "vm: program parse (load)");
    if (!p2) return;
    ms_vm_t vm2;
    CHECK(ms_vm_init(&vm2, p2, &game) == 0, "vm2 init");
    vm2.save = &save;
    ms_vm_update(&vm2, NULL);
    CHECK(vm2.save_status == MS_SAVE_OK, "LOADSAVE status %d", vm2.save_status);
    CHECK(vm2.globals[3] == 0x11223344u, "LOADSAVE g3 = %08x", vm2.globals[3]);
    CHECK(vm2.globals[4] == 0xAABBCCDDu, "LOADSAVE g4 = %08x", vm2.globals[4]);

    /* no image attached: the opcode must be inert, not fatal */
    ms_vm_t vm3;
    CHECK(ms_vm_init(&vm3, p2, &game) == 0, "vm3 init");
    ms_vm_update(&vm3, NULL);
    CHECK(!vm3.error, "LOADSAVE without an image killed the VM");
    CHECK(vm3.save_status == MS_SAVE_E_PARAM, "unattached LOADSAVE status %d", vm3.save_status);
}

int main(void)
{
    basic_test();
    fellback_test();
    powercut_test();
    bundle_test();
    cart_kind13_test();
    vm_save_test();
    if (g_fail) { printf("save image: FAIL (%d failures)\n", g_fail); return 1; }
    printf("save image: ok (0 failures)\n");
    return 0;
}
