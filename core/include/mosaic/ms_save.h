/* SPDX-License-Identifier: Apache-2.0 */
/* Mosaic — ms_save.h: the save image and where it lives.
 *
 * Two homes, one format. A save is a byte image; the bytes are identical
 * whether they sit in a cartridge's SRAM, in the console's store, in a host
 * .msav file or in AI Drive. What differs is the *link*: what the image is
 * bound to.
 *
 *   MS_SAVE_LOC_CART     bound to dev_id + app_id + this build's cart_crc.
 *                        It is the cartridge's own SRAM: a rebuild is a new
 *                        cartridge, so its saves are new too.
 *   MS_SAVE_LOC_CONSOLE  bound to dev_id + app_id only. It survives rebuilds
 *                        and follows the save across every cart that shares
 *                        the same save_id — the memory-card model.
 *
 * Identity is a developer key (`dev=`), which namespaces a studio, plus a
 * save id (`save_id=`), which names the *save*, not the game. A trilogy that
 * wants one continuous save declares the same save_id in all three titles and
 * needs nothing else. A sequel that keeps its own save but imports the
 * predecessor's declares `save_reads=<predecessor id>`, which grants
 * read-only access to that one save and nothing else.
 *
 * image, little-endian (v2):
 *   MOS1 | u16 ver=2 | u16 bytes | u8 slots | u8 loc | u16 flags
 *        | u32 app_id | u32 dev_id | u32 cart_crc
 *   copy[slot*2 + which] for every slot and both copies, fixed stride
 *
 * copy record (v2), body at +40:
 *   u32 'SLT1' | u32 seq | u16 len | u16 crc16(body) | u8 name[12]
 *   | u16 fmt | u16 0 | u64 tag | u16 hcrc | u16 0 | body[len]
 *
 * hcrc is crc16 of the first 36 bytes and is written last, so a power cut at
 * any point leaves the other copy authoritative. tag is SipHash-2-4 keyed by
 * the developer key over the first 28 bytes followed by the body. v1 images
 * (14-byte header, 28-byte record, cart_crc only) are still read, flagged
 * MS_SAVE_F_LEGACY, so images written before this revision keep working. */
#ifndef MOSAIC_MS_SAVE_H
#define MOSAIC_MS_SAVE_H
#include "mosaic/ms_limits.h"
#include "mosaic/ms_types.h"
#include "mosaic/ms_cart.h"

#define MS_SAVE_MAGIC      0x31534F4Du   /* "MOS1" little-endian */
#define MS_SAVE_COPY_MAGIC 0x31544C53u   /* "SLT1" little-endian */
#define MS_SAVE_VERSION    2u
#define MS_SAVE_HDR        24u
#define MS_SAVE_COPY_HDR   40u
#define MS_SAVE_BYTES      (8u * 1024u)
#define MS_SAVE_BYTES_MIN  512u
#define MS_SAVE_BYTES_MAX  65532u
#define MS_SAVE_SLOTS      4u
#define MS_SAVE_SLOTS_MAX  8u
#define MS_SAVE_NAME       12u
#define MS_SAVE_READS_MAX  4u

/* v1 layout constants — read-only compatibility. */
#define MS_SAVE_HDR_V1      14u
#define MS_SAVE_COPY_HDR_V1 28u

#define MS_SAVE_LOC_CONSOLE 1u
#define MS_SAVE_LOC_CART    2u
#define MS_SAVE_LOC_ANY     0u

/* Image flags: where a cart says it can save. */
#define MS_SAVE_DECL_CONSOLE 0x01u
#define MS_SAVE_DECL_CART    0x02u

/* Status bits. */
#define MS_SAVE_F_INIT      0x01u   /* blank image was formatted              */
#define MS_SAVE_F_FELLBACK  0x02u   /* an older copy was used                 */
#define MS_SAVE_F_TORN      0x04u   /* a newer copy failed its checks         */
#define MS_SAVE_F_LINKED    0x08u   /* opened read-only through save_reads    */
#define MS_SAVE_F_LEGACY    0x10u   /* v1 image: cart_crc binding only        */
#define MS_SAVE_F_UNVERIFIED 0x20u  /* raw open: identity and MAC not checked */
#define MS_SAVE_F_UPGRADED  0x40u   /* a legacy image was promoted on write   */

enum {
    MS_SAVE_OK = 0,
    MS_SAVE_E_PARAM = -1, MS_SAVE_E_MAGIC = -2, MS_SAVE_E_SIZE = -3,
    MS_SAVE_E_CRC = -4,   MS_SAVE_E_CART = -5,  MS_SAVE_E_EMPTY = -6,
    MS_SAVE_E_SLOT = -7,  MS_SAVE_E_BODY = -8,  MS_SAVE_E_NOSPACE = -9,
    MS_SAVE_E_DEV = -10,  MS_SAVE_E_APP = -11,  MS_SAVE_E_RO = -12,
    MS_SAVE_E_LOC = -13,  MS_SAVE_E_LEGACY = -14
};
const char *ms_save_err_str(int e);

/* Who is asking. `dev_key` keys the record MAC; NULL or "" is the zero key.
 * `reads` is what this cart declares it may read across save ids. */
typedef struct {
    uint32_t dev_id, app_id, cart_crc;
    uint8_t  loc;                       /* MS_SAVE_LOC_*, 0 = accept either */
    const char *dev_key;
    uint32_t reads[MS_SAVE_READS_MAX];
    int      n_reads;
} ms_save_id_t;

typedef struct {
    uint8_t *region;      /* caller-owned image, `bytes` long                   */
    size_t   bytes;
    uint32_t cart_crc;
    uint32_t dev_id, app_id;
    unsigned slots, stride, max_body, ver;
    uint8_t  loc, read_only, unverified;
    uint8_t  key[16];
} ms_save_t;

typedef struct {
    unsigned slot, valid;
    uint32_t seq;
    uint16_t len, fmt;
    char     name[MS_SAVE_NAME + 1];
} ms_save_ent_t;

int ms_save_format(ms_save_t *s, void *region, size_t bytes, const ms_save_id_t *id,
                   unsigned slots, unsigned *flags);
int ms_save_open(ms_save_t *s, void *region, size_t bytes, const ms_save_id_t *id,
                 unsigned *flags);
/* Inspection only: skips identity and MAC, marks the image read-only. */
int ms_save_open_raw(ms_save_t *s, void *region, size_t bytes, unsigned *flags);

int ms_save_read(const ms_save_t *s, unsigned slot, char *name, size_t namecap,
                 void *buf, size_t cap, size_t *len, uint16_t *fmt, unsigned *flags);
/* `fmt` is the body format version the game assigns; core never interprets it. */
int ms_save_write(ms_save_t *s, unsigned slot, const char *name, uint16_t fmt,
                  const void *data, size_t len);
int ms_save_erase(ms_save_t *s, unsigned slot);
int ms_save_list(const ms_save_t *s, ms_save_ent_t *out, unsigned cap, unsigned *n);
int ms_save_verify(const ms_save_t *s, unsigned *flags);

/* ---- both homes at once ---- */
typedef struct {
    ms_save_t img[2];      /* [0] console, [1] cart */
    unsigned  present;
} ms_save_store_t;

void ms_save_store_clear(ms_save_store_t *st);
/* Attach an already-opened image at its own location. */
int  ms_save_store_attach(ms_save_store_t *st, const ms_save_t *img);
int  ms_save_store_has(const ms_save_store_t *st, unsigned loc);
/* Read whichever home holds the newest copy of `slot`. `loc_used` reports
 * which one answered, so the caller can tell the player where the save is. */
int  ms_save_store_read(const ms_save_store_t *st, unsigned slot, char *name,
                        size_t namecap, void *buf, size_t cap, size_t *len,
                        uint16_t *fmt, unsigned *loc_used, unsigned *flags);
int  ms_save_store_write(ms_save_store_t *st, unsigned loc, unsigned slot,
                         const char *name, uint16_t fmt, const void *data, size_t len);
int  ms_save_store_erase(ms_save_store_t *st, unsigned slot);

/* ---- what a cart declares ---- */
typedef struct {
    unsigned bytes, slots, decl;      /* decl: MS_SAVE_DECL_* bits */
    uint32_t dev_id, app_id, reads[MS_SAVE_READS_MAX];
    int      n_reads;
    char     dev[33], save_id[25], dev_key[33];
} ms_save_cfg_t;

/* 1 if the cart carries a kind 13 region declaration, 0 if not (read-only). */
int ms_save_cfg_from_cart(const ms_cart_t *c, ms_save_cfg_t *out);
void ms_save_id_from_cfg(ms_save_id_t *id, const ms_save_cfg_t *cfg, uint8_t loc,
                         uint32_t cart_crc);

/* .msv transfer bundle: header + integrity trailer. Not a signature. */
#define MS_SAVE_BUNDLE_MAGIC 0x3156534Du   /* "MSV1" little-endian */
int ms_save_bundle_export(const ms_save_t *s, uint32_t device_id,
                          void *out, size_t cap, size_t *len);
int ms_save_bundle_import(ms_save_t *s, const void *blob, size_t bytes,
                          unsigned *flags);

#endif
