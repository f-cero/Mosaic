/* SPDX-License-Identifier: Apache-2.0 */
/* Mosaic — ms_dump.c: copy a cartridge out, byte for byte.
 *
 * One forward pass. The header and TOC are read into the caller's scratch,
 * then everything from the end of the TOC to the end of the cart is streamed
 * through a 256-byte stack buffer, so the source may be unseekable and the
 * sink may be a card smaller than the game. Payload crc32s are accumulated as
 * the bytes go past, and the copy is never abandoned because a check failed:
 * the player asked for the bytes, so the player gets the bytes. */
#include "mosaic/ms_dump.h"
#include <string.h>

static uint32_t crc_table[256];
static int crc_ready;

static uint32_t raw_crc(uint32_t c, const void *data, size_t n)
{
    if (!crc_ready) {
        for (uint32_t i = 0; i < 256u; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = (v & 1u) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
        crc_ready = 1;
    }
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c;
}

static void take_title(const uint8_t *man, uint32_t n, char *out)
{
    out[0] = 0;
    for (uint32_t i = 0; i + 6u <= n; i++) {
        if (i && man[i - 1] != '\n') continue;
        if (memcmp(man + i, "title=", 6) != 0) continue;
        uint32_t j = i + 6u, k = 0;
        while (j < n && man[j] != '\n' && man[j] != '\r' && k < 32u)
            out[k++] = (char)man[j++];
        out[k] = 0;
        return;
    }
}

int ms_dump_cart(const ms_dump_io_t *io, uint32_t total, void *scratch,
                 uint32_t scratch_len, ms_dump_result_t *out)
{
    ms_dump_result_t r;
    memset(&r, 0, sizeof r);
    if (out) *out = r;
    if (!io || !io->read || !io->write) return MS_DUMP_E_PARAM;
    if (!scratch || scratch_len < MS_DUMP_SCRATCH) return MS_DUMP_E_SCRATCH;

    uint8_t *sc = (uint8_t *)scratch;
    uint8_t hdr[MS_CART_HDR];
    if (io->read(io->ctx, 0, hdr, MS_CART_HDR)) return MS_DUMP_E_IO;
    if (ms_rd32(hdr) != MS_CART_MAGIC) return MS_DUMP_E_MAGIC;
    if (ms_rd16(hdr + 4) != MS_CART_VERSION) return MS_DUMP_E_MAGIC;

    const uint16_t cflags = ms_rd16(hdr + 6);
    const uint32_t count = ms_rd32(hdr + 8);
    const uint32_t toc_off = ms_rd32(hdr + 12);
    const uint32_t toc_crc = ms_rd32(hdr + 16);
    if (count < 1u || count > MS_CART_MAX_RECORDS) return MS_DUMP_E_LAYOUT;
    if (toc_off != MS_CART_HDR) return MS_DUMP_E_LAYOUT;

    const uint32_t toc_bytes = count * MS_CART_TOC_ENTRY;
    if (io->read(io->ctx, toc_off, sc, toc_bytes)) return MS_DUMP_E_IO;
    if (ms_crc32(sc, toc_bytes) != toc_crc) return MS_DUMP_E_CRC;

    uint32_t roff[MS_CART_MAX_RECORDS], rsize[MS_CART_MAX_RECORDS];
    uint32_t rkind[MS_CART_MAX_RECORDS], rcrc[MS_CART_MAX_RECORDS];
    uint32_t seen[MS_CART_MAX_RECORDS];
    uint32_t end = toc_off + toc_bytes, prev = end;

    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = sc + i * MS_CART_TOC_ENTRY;
        uint32_t nlen = 0;
        while (nlen < 32u && e[nlen]) nlen++;
        if (nlen == 0u || nlen == 32u) return MS_DUMP_E_LAYOUT;
        rkind[i] = ms_rd32(e + 32);
        roff[i]  = ms_rd32(e + 36);
        rsize[i] = ms_rd32(e + 40);
        rcrc[i]  = ms_rd32(e + 44);
        if (rkind[i] < 1u || rkind[i] > MS_CART_KIND_MAX) return MS_DUMP_E_LAYOUT;
        if (rkind[i] > MS_CART_KIND_CANON_MAX && !(cflags & MS_CART_F_EXT))
            return MS_DUMP_E_LAYOUT;
        if (roff[i] < prev) return MS_DUMP_E_LAYOUT;          /* TOC order, no overlap */
        if ((uint64_t)roff[i] + rsize[i] > 0xFFFFFFFFull) return MS_DUMP_E_LAYOUT;
        prev = roff[i] + rsize[i];
        if (prev > end) end = prev;
        seen[i] = 0;
    }

    if (total == 0u) total = end;
    const int truncated = total < end;

    if (io->write(io->ctx, hdr, MS_CART_HDR)) return MS_DUMP_E_IO;
    if (io->write(io->ctx, sc, toc_bytes)) return MS_DUMP_E_IO;
    uint32_t all = 0xFFFFFFFFu;
    all = raw_crc(all, hdr, MS_CART_HDR);
    all = raw_crc(all, sc, toc_bytes);
    r.bytes = MS_CART_HDR + toc_bytes;

    uint32_t pcrc[MS_CART_MAX_RECORDS];
    for (uint32_t i = 0; i < count; i++) pcrc[i] = 0xFFFFFFFFu;
    uint8_t man[256];
    uint32_t manlen = 0;
    uint8_t chunk[256];
    uint32_t pos = toc_off + toc_bytes;
    int ioerr = 0;

    while (pos < total) {
        uint32_t n = total - pos;
        if (n > sizeof chunk) n = sizeof chunk;
        if (io->read(io->ctx, pos, chunk, n)) { ioerr = 1; break; }
        if (io->write(io->ctx, chunk, n)) { ioerr = 1; break; }
        all = raw_crc(all, chunk, n);
        r.bytes += n;
        for (uint32_t i = 0; i < count; i++) {
            const uint32_t s = roff[i], e2 = roff[i] + rsize[i];
            const uint32_t a = pos > s ? pos : s;
            const uint32_t b = pos + n < e2 ? pos + n : e2;
            if (a >= b) continue;
            pcrc[i] = raw_crc(pcrc[i], chunk + (a - pos), b - a);
            seen[i] += b - a;
            if (rkind[i] == MS_A_MANIFEST && manlen < sizeof man) {
                uint32_t take = b - a;
                if (take > sizeof man - manlen) take = (uint32_t)(sizeof man - manlen);
                memcpy(man + manlen, chunk + (a - pos), take);
                manlen += take;
            }
        }
        pos += n;
    }

    r.records = count;
    for (uint32_t i = 0; i < count; i++) {
        if (seen[i] == rsize[i] && (pcrc[i] ^ 0xFFFFFFFFu) == rcrc[i]) r.verified++;
        else r.failed++;
    }
    r.crc = all ^ 0xFFFFFFFFu;
    r.ext = (cflags & MS_CART_F_EXT) ? 1u : 0u;
    r.ok = (r.failed == 0u && !truncated) ? 1u : 0u;
    take_title(man, manlen, r.title);
    if (out) *out = r;

    if (ioerr) return MS_DUMP_E_IO;
    if (truncated) return MS_DUMP_E_SIZE;
    if (r.failed) return MS_DUMP_E_VERIFY;
    return MS_DUMP_OK;
}

/* ---- in-memory verify ---- */

typedef struct { const uint8_t *src; size_t n; } mem_src_t;

static int mem_read(void *ctx, uint32_t off, void *dst, uint32_t len)
{
    mem_src_t *m = (mem_src_t *)ctx;
    if ((size_t)off + len > m->n) return -1;
    memcpy(dst, m->src + off, len);
    return 0;
}

static int mem_sink(void *ctx, const void *src, uint32_t len)
{
    (void)ctx; (void)src; (void)len;
    return 0;
}

int ms_dump_blob(const void *blob, size_t bytes, ms_dump_result_t *out)
{
    if (!blob || !bytes) return MS_DUMP_E_PARAM;
    mem_src_t m; m.src = (const uint8_t *)blob; m.n = bytes;
    ms_dump_io_t io; io.read = mem_read; io.write = mem_sink; io.ctx = &m;
    static uint8_t scratch[MS_DUMP_SCRATCH];
    return ms_dump_cart(&io, (uint32_t)bytes, scratch, sizeof scratch, out);
}

/* ---- container ---- */

size_t ms_dump_container_need(uint32_t rom_bytes, uint32_t sram_bytes)
{
    const size_t rom_end = MS_DUMP_HDR + rom_bytes;
    if (!sram_bytes) return rom_end;
    const size_t sram_off = (rom_end + 3u) & ~(size_t)3u;
    return sram_off + sram_bytes;
}

int ms_dump_container_build(void *out, size_t cap, const void *rom,
                            uint32_t rom_bytes, const void *sram,
                            uint32_t sram_bytes, const char *title, size_t *len)
{
    if (!out || !rom || !rom_bytes) { if (len) *len = 0; return MS_DUMP_E_PARAM; }
    if (sram_bytes && !sram) { if (len) *len = 0; return MS_DUMP_E_PARAM; }
    const size_t need = ms_dump_container_need(rom_bytes, sram_bytes);
    if (len) *len = need;
    if (cap < need) return MS_DUMP_E_SIZE;

    const size_t sram_off = (MS_DUMP_HDR + rom_bytes + 3u) & ~(size_t)3u;
    uint8_t *p = (uint8_t *)out;
    memset(p, 0, MS_DUMP_HDR);
    ms_wr32(p, MS_DUMP_MAGIC);
    ms_wr16(p + 4, MS_DUMP_VERSION);
    ms_wr16(p + 6, sram_bytes ? MS_DUMP_F_SRAM : 0u);
    ms_wr32(p + 8, (uint32_t)need);
    ms_wr32(p + 12, MS_DUMP_HDR);
    ms_wr32(p + 16, rom_bytes);
    ms_wr32(p + 20, ms_crc32(rom, rom_bytes));
    ms_wr32(p + 24, sram_bytes ? (uint32_t)sram_off : 0u);
    ms_wr32(p + 28, sram_bytes);
    ms_wr32(p + 32, sram_bytes ? ms_crc32(sram, sram_bytes) : 0u);
    if (title) {
        uint32_t i = 0;
        for (; title[i] && i < 31u; i++) p[36 + i] = (uint8_t)title[i];
        p[36 + i] = 0;
    }
    ms_wr32(p + 68, ms_crc32(p, 68));
    memcpy(p + MS_DUMP_HDR, rom, rom_bytes);
    if (sram_bytes) memcpy(p + sram_off, sram, sram_bytes);
    return MS_DUMP_OK;
}

int ms_dump_container_open(ms_dump_container_t *c, const void *blob, size_t bytes)
{
    if (!c || !blob) return MS_DUMP_E_PARAM;
    memset(c, 0, sizeof *c);
    if (bytes < MS_DUMP_HDR) return MS_DUMP_E_SIZE;
    const uint8_t *p = (const uint8_t *)blob;
    if (ms_rd32(p) != MS_DUMP_MAGIC) return MS_DUMP_E_MAGIC;
    if (ms_rd16(p + 4) != MS_DUMP_VERSION) return MS_DUMP_E_MAGIC;
    if (ms_rd32(p + 68) != ms_crc32(p, 68)) return MS_DUMP_E_CRC;
    const uint32_t total = ms_rd32(p + 8);
    if (total < MS_DUMP_HDR || total > bytes) return MS_DUMP_E_SIZE;
    const uint32_t roff = ms_rd32(p + 12), rlen = ms_rd32(p + 16);
    const uint32_t soff = ms_rd32(p + 24), slen = ms_rd32(p + 28);
    if (!rlen || (uint64_t)roff + rlen > total) return MS_DUMP_E_LAYOUT;
    if (slen && ((uint64_t)soff + slen > total || soff < roff + rlen))
        return MS_DUMP_E_LAYOUT;
    c->rom = p + roff; c->rom_bytes = rlen; c->rom_crc = ms_rd32(p + 20);
    if (slen) { c->sram = p + soff; c->sram_bytes = slen; c->sram_crc = ms_rd32(p + 32); }
    memcpy(c->title, p + 36, 32);
    c->title[32] = 0;
    if (ms_crc32(c->rom, c->rom_bytes) != c->rom_crc) return MS_DUMP_E_VERIFY;
    if (c->sram && ms_crc32(c->sram, c->sram_bytes) != c->sram_crc) return MS_DUMP_E_VERIFY;
    return MS_DUMP_OK;
}

const char *ms_dump_err_str(int e)
{
    switch (e) {
    case MS_DUMP_OK:          return "ok";
    case MS_DUMP_E_PARAM:     return "bad argument";
    case MS_DUMP_E_MAGIC:     return "not a cart16 v2 image";
    case MS_DUMP_E_SIZE:      return "the source is shorter than the cart says";
    case MS_DUMP_E_CRC:       return "the table of contents does not match its checksum";
    case MS_DUMP_E_LAYOUT:    return "the table of contents is malformed";
    case MS_DUMP_E_IO:        return "the medium stopped early";
    case MS_DUMP_E_SCRATCH:   return "scratch is smaller than the dumper needs";
    case MS_DUMP_E_VERIFY:    return "copied, but a payload does not match its checksum";
    default:                  return "unknown error";
    }
}
