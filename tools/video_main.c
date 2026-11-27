/* SPDX-License-Identifier: Apache-2.0
 * mosaic video — transcode / compose / inspect MVID clips.
 *
 * Transcoding shells out to ffmpeg for *decoding only*. Nothing is linked
 * against libav*: the binary is an external process, so Mosaic stays
 * Apache-2.0 and the user's ffmpeg keeps its own licence (LGPL/GPL).
 *
 * Composing runs the frames through the engine's PPU (ms_convert_level +
 * ms_ppu_render), so pans, affine zoom/rotate, priority and brightness are
 * exactly what the console will show — the video is the engine's own output.
 */
#define STBI_NO_HDR
#include "stb/stb_image.h"
#include "mosaic/ms_build.h"
#include "mosaic/ms_render.h"
#include "mvid.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#define MPOPEN  _popen
#define MPCLOSE _pclose
#else
#define MPOPEN  popen
#define MPCLOSE pclose
#endif

#define PI 3.14159265358979323846

typedef struct {
    uint16_t *px; int n, cap, w, h;
} clip_t;

static int clip_push(clip_t *c, const uint16_t *src)
{
    if (c->n == c->cap) {
        int cap = c->cap ? c->cap * 2 : 32;
        if (cap > MVID_MAX_FRAMES) cap = MVID_MAX_FRAMES;
        uint16_t *np = (uint16_t *)realloc(c->px, (size_t)cap * c->w * c->h * 2u);
        if (!np) return -1;
        c->px = np; c->cap = cap;
    }
    memcpy(c->px + (size_t)c->n * c->w * c->h, src, (size_t)c->w * c->h * 2u);
    c->n++;
    return 0;
}

static uint16_t rgb888_565(unsigned r, unsigned g, unsigned b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* nearest-neighbour scale into the clip's coded size */
static void blit_scaled(clip_t *c, const uint16_t *src, int sw, int sh, uint16_t *dst)
{
    for (int y = 0; y < c->h; y++) {
        const int sy = (int)((long)y * sh / c->h);
        for (int x = 0; x < c->w; x++) {
            const int sx = (int)((long)x * sw / c->w);
            dst[(size_t)y * c->w + x] = src[(size_t)sy * sw + sx];
        }
    }
}

static int load_ffmpeg(clip_t *c, const char *in, int fps, int max_frames, const char *scratch)
{
    char vf[256], lim[64] = "";
    snprintf(vf, sizeof(vf), "scale=%d:%d:flags=area,fps=%d", c->w, c->h, fps);
    if (max_frames > 0) snprintf(lim, sizeof(lim), " -frames:v %d", max_frames);
    char cmd[1200];
    /* Decode to a scratch raw file with the ffmpeg *binary*: no libav* is
     * linked, so Mosaic's licence is unaffected. */
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -y -v error -i \"%s\" -vf \"%s\"%s -f rawvideo -pix_fmt rgb565le \"%s\"",
             in, vf, lim, scratch);
    if (system(cmd) != 0) { fprintf(stderr, "ffmpeg failed to decode %s\n", in); return -1; }
    FILE *p = fopen(scratch, "rb");
    if (!p) { fprintf(stderr, "ffmpeg produced no output\n"); return -1; }
    uint16_t *buf = (uint16_t *)malloc((size_t)c->w * c->h * 2u);
    if (!buf) { fclose(p); return -1; }
    int n = 0;
    while (n < MVID_MAX_FRAMES && fread(buf, 1, (size_t)c->w * c->h * 2u, p) == (size_t)c->w * c->h * 2u) {
        if (clip_push(c, buf) != 0) break;
        n++;
    }
    free(buf);
    fclose(p);
    remove(scratch);
    return n;
}

static int load_png_frames(clip_t *c, const char *path, int repeat)
{
    int w = 0, h = 0, ch = 0;
    unsigned char *rgba = (unsigned char *)stbi_load(path, &w, &h, &ch, 4);
    if (!rgba) return -1;
    uint16_t *src = (uint16_t *)malloc((size_t)w * h * 2u);
    uint16_t *dst = (uint16_t *)malloc((size_t)c->w * c->h * 2u);
    if (!src || !dst) { free(src); free(dst); stbi_image_free(rgba); return -1; }
    for (int i = 0; i < w * h; i++)
        src[i] = rgb888_565(rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2]);
    blit_scaled(c, src, w, h, dst);
    for (int r = 0; r < repeat && c->n < MVID_MAX_FRAMES; r++) clip_push(c, dst);
    free(src); free(dst); stbi_image_free(rgba);
    return 0;
}

static void mode_dims(const char *mode, int *w, int *h, int *bpp)
{
    *w = 320; *h = 240; *bpp = 4;
    if (mode && (!strcmp(mode, "qfa4") || !strcmp(mode, "qfa8"))) { *w = 160; *h = 120; }
    if (mode && (!strcmp(mode, "tile8") || !strcmp(mode, "qfa8"))) *bpp = 8;
}

static int cmd_encode(int argc, char **argv)
{
    const char *in = NULL, *out = NULL, *mode = "tile4";
    int fps = 12, maxf = 0, loop = 1;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--input") && i + 1 < argc) in = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) mode = argv[++i];
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc) fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) maxf = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--once")) loop = 0;
    }
    if (!in || !out) { fprintf(stderr, "usage: mosaic video encode --input in.mp4|in.png --out out.mvid [--mode tile4|tile8|qfa4|qfa8] [--fps N] [--frames N] [--once]\n"); return 1; }

    clip_t c; memset(&c, 0, sizeof(c));
    mode_dims(mode, &c.w, &c.h, (int *)&c.cap);   /* bpp borrowed below */
    int bpp = c.cap; c.cap = 0;

    const size_t ilen = strlen(in);
    if (ilen > 4 && (!strcmp(in + ilen - 4, ".png") || !strcmp(in + ilen - 4, ".PNG"))) {
        if (load_png_frames(&c, in, maxf > 0 ? maxf : 1) != 0) {
            fprintf(stderr, "cannot read %s\n", in); return 2;
        }
    } else {
        char scratch[512];
        snprintf(scratch, sizeof(scratch), "%s.raw", out);
        int n = load_ffmpeg(&c, in, fps, maxf, scratch);
        if (n <= 0) { fprintf(stderr, "decoded no frames from %s\n", in); return 2; }
    }
    if (!c.n) { fprintf(stderr, "no frames\n"); return 2; }

    uint16_t pal[256]; int pn = 0;
    if (mvid_palette_build(c.px, (size_t)c.n, (size_t)c.w, (size_t)c.h, bpp, pal, &pn) != 0) {
        fprintf(stderr, "palette build failed\n"); return 3;
    }
    mvid_enc_opts_t o; memset(&o, 0, sizeof(o));
    o.width = (uint16_t)c.w; o.height = (uint16_t)c.h; o.fps = (uint8_t)fps;
    o.bpp = (uint8_t)bpp; o.loop = (uint8_t)loop;
    o.pal = pal; o.pal_count = (uint16_t)pn;
    mvid_enc_t *e = mvid_enc_new(&o);
    if (!e) { fprintf(stderr, "encoder init failed\n"); return 3; }
    for (int i = 0; i < c.n; i++)
        if (mvid_enc_frame(e, c.px + (size_t)i * c.w * c.h, 0, 0, 16) != 0) {
            fprintf(stderr, "encode failed at frame %d\n", i); mvid_enc_free(e); return 4;
        }
    int rc = mvid_enc_write(e, out);
    mvid_enc_free(e);
    if (rc != 0) { fprintf(stderr, "write failed: %s\n", mvid_err_str(rc)); return 5; }
    struct stat st; long bytes = (stat(out, &st) == 0) ? (long)st.st_size : 0;
    printf("wrote %s: %d frames %dx%d %dbpp %d fps, %ld bytes (%.1f B/frame)\n",
           out, c.n, c.w, c.h, bpp, fps, bytes, bytes / (double)c.n);
    free(c.px);
    return 0;
}

/* ---------------- compose ---------------- */
typedef struct {
    char type[12], img[192];
    int frames;
    double x0, y0, x1, y1, z0, z1, r0, r1;
    int b0, b1;
} shot_t;

static void kv(const char *tok, const char *key, char *out, size_t cap)
{
    const size_t kl = strlen(key);
    if (out && cap) out[0] = 0;
    if (!strncmp(tok, key, kl) && tok[kl] == '=') snprintf(out, cap, "%s", tok + kl + 1);
}
static double kv_num(const char *tok, const char *key, double def)
{
    char v[64]; v[0] = 0; kv(tok, key, v, sizeof(v));
    return v[0] ? atof(v) : def;
}
static void kv_span(const char *tok, const char *key, double *a, double *b)
{
    char v[64]; v[0] = 0; kv(tok, key, v, sizeof(v));
    if (!v[0]) return;
    const char *arrow = strstr(v, "->");
    if (arrow) { *a = atof(v); *b = atof(arrow + 2); }
    else { *a = *b = atof(v); }
}

static int cmd_compose(int argc, char **argv)
{
    const char *scene = NULL, *out = NULL;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else scene = argv[i];
    }
    if (!scene || !out) { fprintf(stderr, "usage: mosaic video compose scene.mvj --out out.mvid\n"); return 1; }
    FILE *f = fopen(scene, "r");
    if (!f) { fprintf(stderr, "cannot read %s\n", scene); return 2; }
    char dir[192]; snprintf(dir, sizeof(dir), "%s", scene);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0; else snprintf(dir, sizeof(dir), ".");

    int fps = 12, loop = 1, bpp = 4, W = 320, H = 240;
    shot_t shots[32]; int ns = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#'); if (hash) *hash = 0;
        char *tok = strtok(line, " \t\r\n");
        if (!tok) continue;
        if (!strcmp(tok, "video")) {
            while ((tok = strtok(NULL, " \t\r\n"))) {
                char v[64];
                kv(tok, "fps", v, sizeof(v)); if (v[0]) fps = atoi(v);
                kv(tok, "loop", v, sizeof(v)); if (v[0]) loop = atoi(v);
                kv(tok, "bpp", v, sizeof(v)); if (v[0]) bpp = atoi(v);
                kv(tok, "w", v, sizeof(v)); if (v[0]) W = atoi(v);
                kv(tok, "h", v, sizeof(v)); if (v[0]) H = atoi(v);
            }
        } else if (!strcmp(tok, "shot") && ns < 32) {
            shot_t *s = &shots[ns];

            memset(s, 0, sizeof(*s));
            char *t = strtok(NULL, " \t\r\n");
            if (!t) continue;
            snprintf(s->type, sizeof(s->type), "%s", t);
            s->frames = 12; s->b0 = s->b1 = 16; s->z0 = s->z1 = 1.0;
            while ((tok = strtok(NULL, " \t\r\n"))) {
                char v[192];
                kv(tok, "image", v, sizeof(v));
                if (v[0]) snprintf(s->img, sizeof(s->img), "%s", v);
                s->frames = (int)kv_num(tok, "frames", s->frames);
                s->x0 = kv_num(tok, "x", s->x0);   s->x1 = kv_num(tok, "tox", s->x1);
                s->y0 = kv_num(tok, "y", s->y0);   s->y1 = kv_num(tok, "toy", s->y1);
                s->b0 = (int)kv_num(tok, "brightness", s->b0);
                s->b1 = (int)kv_num(tok, "tobri", s->b1);
                kv_span(tok, "zoom", &s->z0, &s->z1);
                kv_span(tok, "rot", &s->r0, &s->r1);
            }
            ns++;
        }
    }
    fclose(f);
    if (!ns) { fprintf(stderr, "scene has no shots\n"); return 2; }

    /* one map grid for the whole clip: the largest shot, and at least the
     * screen, so pans and rendered shots share a single map_w/map_h. */
    int max_lw = W / 8, max_lh = H / 8;
    for (int i = 0; i < ns; i++) {
        char path[400];
        if (shots[i].img[0] == '/') snprintf(path, sizeof(path), "%s", shots[i].img);
        else snprintf(path, sizeof(path), "%s/%s", dir, shots[i].img);
        int iw = 0, ih = 0, ic = 0;
        if (stbi_info(path, &iw, &ih, &ic)) {
            const int lw = (iw + 7) / 8, lh = (ih + 7) / 8;
            if (lw > max_lw) max_lw = lw;
            if (lh > max_lh) max_lh = lh;
        }
    }
    if (max_lw > 64 || max_lh > 64) { fprintf(stderr, "scene needs a %dx%d tile map (max 64x64)\n", max_lw, max_lh); return 3; }

    static uint16_t pmap[64 * 64];
    static uint8_t tiles[MVID_MAX_TILES * 64];
    static uint16_t map[128 * 128];
    static uint16_t pal[256];
    static uint16_t fb[320 * 240];
    mvid_enc_t *e = NULL;
    ms_ppu_t ppu;

    for (int i = 0; i < ns; i++) {
        shot_t *s = &shots[i];
        char path[400];
        if (s->img[0] == '/') snprintf(path, sizeof(path), "%s", s->img);
        else snprintf(path, sizeof(path), "%s/%s", dir, s->img);
        size_t tl = 0, ml = 0; int pn = 0, pw = 0, ph = 0, lw = 0, lh = 0;
        int rc = ms_convert_level(path, bpp, tiles, &tl, (unsigned char *)map, &ml,
                                 pal, &pn, &pw, &ph, &lw, &lh);
        if (rc != 0) { fprintf(stderr, "shot %d: cannot convert %s (%d)\n", i, path, rc); if (e) mvid_enc_free(e); return 3; }
        const int ntiles = (int)(tl / (bpp == 8 ? 64u : 32u));
        if (lw > 64 || lh > 64) { fprintf(stderr, "shot %d: image map %dx%d exceeds 64x64 tiles\n", i, lw, lh); if (e) mvid_enc_free(e); return 3; }
        if (!e) {
            mvid_enc_opts_t o; memset(&o, 0, sizeof(o));
            o.width = (uint16_t)W; o.height = (uint16_t)H; o.fps = (uint8_t)fps;
            o.bpp = (uint8_t)bpp; o.loop = (uint8_t)loop;
            o.pal = pal; o.pal_count = (uint16_t)pn;
            o.map_w = (uint16_t)max_lw; o.map_h = (uint16_t)max_lh;
            e = mvid_enc_new(&o);
            if (!e) {
                fprintf(stderr, "encoder init failed: %dx%d bpp=%d pal=%d map=%dx%d\n",
                        o.width, o.height, o.bpp, o.pal_count, o.map_w, o.map_h);
                return 3;
            }
        } else {
            mvid_enc_palette(e, pal, pn);
        }
        const int frames = s->frames < 1 ? 1 : s->frames;

        if (!strcmp(s->type, "affine")) {
            ms_ppu_reset(&ppu, MS_MODE_AFFINE, (uint16_t)W, (uint16_t)H);
            ppu.fx.backdrop = pal[0];
            for (unsigned k = 0; k < 256; k++) ppu.pal_bg[k] = pal[k & 0xFF];
            ppu.bg[0].tiles = tiles;
            ppu.bg[0].map = map;
            ppu.bg[0].bpp = (uint8_t)bpp;
            ppu.bg[0].map_w = (uint8_t)pw;
            ppu.bg[0].map_h = (uint8_t)ph;
            ppu.bg[0].logical_w = (uint8_t)lw;
            ppu.bg[0].logical_h = (uint8_t)lh;
            ppu.bg[0].enabled = 1;
            ppu.bg[0].priority = 0;
            for (int k = 0; k < frames; k++) {
                const double t = frames > 1 ? (double)k / (frames - 1) : 0.0;
                const double z = s->z0 + (s->z1 - s->z0) * t;
                const double r = (s->r0 + (s->r1 - s->r0) * t) * PI / 180.0;
                ppu.mat[0].a = (ms_fix)(cos(r) * z * (double)MS_FIX_ONE);
                ppu.mat[0].b = (ms_fix)(-sin(r) * z * (double)MS_FIX_ONE);
                ppu.mat[0].c = (ms_fix)(sin(r) * z * (double)MS_FIX_ONE);
                ppu.mat[0].d = (ms_fix)(cos(r) * z * (double)MS_FIX_ONE);
                ppu.bg[0].scroll_x = (int16_t)(W / 2);
                ppu.bg[0].scroll_y = (int16_t)(H / 2);
                ppu.fx.brightness = (uint8_t)(s->b0 + (s->b1 - s->b0) * t);
                ms_ppu_render(&ppu, fb);
                if (mvid_enc_frame(e, fb, 0, 0, 16) != 0) { fprintf(stderr, "encode failed\n"); mvid_enc_free(e); return 4; }
            }
        } else {
            for (int y = 0; y < max_lh; y++)
                for (int x = 0; x < max_lw; x++)
                    pmap[y * max_lw + x] = map[(y % lh) * lw + (x % lw)];
            if (mvid_enc_source_map(e, tiles, ntiles, pmap, max_lw, max_lh, bpp) != 0) {
                fprintf(stderr, "shot %d: source map rejected\n", i); mvid_enc_free(e); return 4;
            }
            for (int k = 0; k < frames; k++) {
                const double t = frames > 1 ? (double)k / (frames - 1) : 0.0;
                const int sx = (int)(s->x0 + (s->x1 - s->x0) * t);
                const int sy = (int)(s->y0 + (s->y1 - s->y0) * t);
                const int br = (int)(s->b0 + (s->b1 - s->b0) * t);
                if (mvid_enc_pan(e, (int16_t)sx, (int16_t)sy, (uint8_t)br) != 0) {
                    fprintf(stderr, "shot %d: pan failed\n", i); mvid_enc_free(e); return 4;
                }
            }
        }
    }
    int rc = mvid_enc_write(e, out);
    const int n = mvid_enc_frames(e);
    mvid_enc_free(e);
    if (rc != 0) { fprintf(stderr, "write failed: %s\n", mvid_err_str(rc)); return 5; }
    struct stat st; long bytes = (stat(out, &st) == 0) ? (long)st.st_size : 0;
    printf("composed %s: %d shots, %d frames %dx%d, %ld bytes (%.1f B/frame)\n",
           out, ns, n, W, H, bytes, bytes / (double)n);
    return 0;
}

static int cmd_info(int argc, char **argv)
{
    const char *in = argc > 3 ? argv[3] : NULL;
    if (!in) { fprintf(stderr, "usage: mosaic video info clip.mvid\n"); return 1; }
    size_t sz = 0;
    uint8_t *b = mvid_read_file(in, &sz);
    if (!b) { fprintf(stderr, "cannot read %s\n", in); return 2; }
    mvid_info_t i;
    int rc = mvid_check(b, sz);
    if (rc != MVID_E_OK) { printf("INVALID: %s\n", mvid_err_str(rc)); free(b); return 3; }
    mvid_parse(&i, b, sz);
    unsigned types[3] = { 0, 0, 0 };
    for (unsigned f = 0; f < i.frames; f++) {
        uint32_t off = mvid_rd32(b + i.index_off + (size_t)f * 4u);
        if (b[off] < 3) types[b[off]]++;
    }
    printf("%s: MVID v1 %dx%d %dbpp %d fps, %u frames, map %ux%u tiles, %zu bytes\n",
           in, i.width, i.height, i.bpp, i.fps, i.frames, i.map_w, i.map_h, sz);
    printf("  INTRA %u  PATCH %u  PAN %u   %.1f B/frame\n",
           types[0], types[1], types[2], sz / (double)i.frames);
    free(b);
    return 0;
}

static int cmd_frames(int argc, char **argv)
{
    const char *in = NULL, *out = NULL;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!in) in = argv[i];
    }
    if (!in || !out) { fprintf(stderr, "usage: mosaic video frames clip.mvid --out dir\n"); return 1; }
    size_t sz = 0;
    uint8_t *b = mvid_read_file(in, &sz);
    if (!b) { fprintf(stderr, "cannot read %s\n", in); return 2; }
    static uint8_t sheet[MVID_MAX_TILES * 64];
    static uint16_t map[128 * 128];
    mvid_t v;
    int rc = mvid_open(&v, b, sz, sheet, map);
    if (rc != MVID_E_OK) { printf("open failed: %s\n", mvid_err_str(rc)); free(b); return 3; }
    const int cw = v.info.width, chh = v.info.height;
    static uint16_t small[160 * 120];
    static uint16_t big[320 * 240];
    char path[400];
#ifdef _WIN32
    _mkdir(out);
#else
    mkdir(out, 0777);
#endif
    unsigned f = 0;
    do {
        if (v.info.flags & MVID_F_QFA) {
            mvid_draw(&v, small, (unsigned)cw, (unsigned)chh);
            for (int y = 0; y < chh; y++)
                for (int x = 0; x < cw; x++) {
                    big[(size_t)(y * 2) * 320 + x * 2] = small[y * cw + x];
                    big[(size_t)(y * 2) * 320 + x * 2 + 1] = small[y * cw + x];
                    big[(size_t)(y * 2 + 1) * 320 + x * 2] = small[y * cw + x];
                    big[(size_t)(y * 2 + 1) * 320 + x * 2 + 1] = small[y * cw + x];
                }
            snprintf(path, sizeof(path), "%s/frame%03u.png", out, f);
            ms_write_png(path, 320, 240, big);
        } else {
            mvid_draw(&v, big, (unsigned)cw, (unsigned)chh);
            snprintf(path, sizeof(path), "%s/frame%03u.png", out, f);
            ms_write_png(path, cw, chh, big);
        }
        f++;
    } while (mvid_next(&v) == 1);
    mvid_close(&v);
    free(b);
    printf("wrote %u frames to %s\n", f, out);
    return 0;
}

int mosaic_video_main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "mosaic video <encode|compose|info|frames> ...\n");
        return 1;
    }
    if (!strcmp(argv[2], "encode")) return cmd_encode(argc, argv);
    if (!strcmp(argv[2], "compose")) return cmd_compose(argc, argv);
    if (!strcmp(argv[2], "info")) return cmd_info(argc, argv);
    if (!strcmp(argv[2], "frames")) return cmd_frames(argc, argv);
    fprintf(stderr, "unknown video verb %s\n", argv[2]);
    return 1;
}
