/* SPDX-License-Identifier: Apache-2.0 */
/* Mosaic — ms_build.c: project parser, node-graph compiler, PNG asset
 * pipeline and cartridge packer. Pure C99 + stb_image. */
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb/stb_image.h"
#include "stb/stb_image_write.h"
#include "mosaic/ms_build.h"
#include "mosaic/ms_cart.h"
#include "mosaic/ms_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

/* ---------------- project model ---------------- */
typedef struct { char key[24], val[96]; } prop_t;
typedef struct { int id, x, y; char type[24]; prop_t p[6]; int np; } node_t;
typedef struct { int src, dst; char sp[12], dp[12]; } link_t;
typedef struct {
    char name[32]; char path[192]; int gw, gh, bpp;
} sheetdef_t;
typedef struct {
    char name[32]; int sheet, x, y, anim, priority; char graph[32];
} entdef_t;
typedef struct {
    char title[33], author[33], mode[12], level[192], music[192], project_dir[192];
    int qfa, voices, mv, sv; unsigned backdrop, level_bpp;
    sheetdef_t sheets[8]; int nsheets;
    entdef_t ents[16]; int nents;
    node_t nodes[160]; int nnodes;
    link_t links[320]; int nlinks;
} project_t;

static const char *prop(const node_t *n, const char *k, const char *def)
{
    for (int i = 0; i < n->np; i++) if (!strcmp(n->p[i].key, k)) return n->p[i].val;
    return def;
}
static node_t *find_node(project_t *p, int id)
{
    for (int i = 0; i < p->nnodes; i++) if (p->nodes[i].id == id) return &p->nodes[i];
    return NULL;
}
static int link_dst(project_t *p, int src, const char *pin)
{
    for (int i = 0; i < p->nlinks; i++)
        if (p->links[i].src == src && !strcmp(p->links[i].sp, pin)) return p->links[i].dst;
    return -1;
}
static int link_src(project_t *p, int dst, const char *pin)
{
    for (int i = 0; i < p->nlinks; i++)
        if (p->links[i].dst == dst && !strcmp(p->links[i].dp, pin)) return p->links[i].src;
    return -1;
}

static int parse_project(const char *path, project_t *p)
{
    memset(p, 0, sizeof(*p));
    strcpy(p->mode, "rich"); p->voices = MS_VOICES; p->mv = 12; p->sv = 4;
    p->backdrop = 0x0841; p->level_bpp = 4;
    const char *slash = strrchr(path, '/');
    size_t dl = slash ? (size_t)(slash - path) : 0;
    if (dl >= sizeof(p->project_dir)) return -1;
    if (!slash) { strcpy(p->project_dir, "."); }
    else { memcpy(p->project_dir, path, dl); p->project_dir[dl] = 0; }

    FILE *f = fopen(path, "rb");
    if (!f) return -2;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        char *nl = strpbrk(s, "\r\n"); if (nl) *nl = 0;
        if (!*s || *s == '#') continue;
        if (!strncmp(s, "title=", 6)) sscanf(s + 6, "%32[^\n]", p->title);
        else if (!strncmp(s, "author=", 7)) sscanf(s + 7, "%32[^\n]", p->author);
        else if (!strncmp(s, "mode=", 5)) sscanf(s + 5, "%11s", p->mode);
        else if (!strncmp(s, "qfa=", 4)) p->qfa = atoi(s + 4);
        else if (!strncmp(s, "voices=", 7)) p->voices = atoi(s + 7);
        else if (!strncmp(s, "music_voices=", 13)) p->mv = atoi(s + 13);
        else if (!strncmp(s, "sfx_voices=", 11)) p->sv = atoi(s + 11);
        else if (!strncmp(s, "backdrop=", 9)) p->backdrop = (unsigned)strtoul(s + 9, NULL, 0);
        else if (!strncmp(s, "level=", 6)) sscanf(s + 6, "%191s %u", p->level, &p->level_bpp);
        else if (!strncmp(s, "music=", 6)) sscanf(s + 6, "%191s", p->music);
        else if (!strncmp(s, "sheet ", 6)) {
            sheetdef_t *sh = &p->sheets[p->nsheets];
            if (p->nsheets >= 8) { fclose(f); return -3; }
            if (sscanf(s + 6, "%31s %191s %d %d %d", sh->name, sh->path, &sh->gw, &sh->gh, &sh->bpp) < 3) { fclose(f); return -3; }
            if (sh->gw <= 0) sh->gw = 16;
            if (sh->gh <= 0) sh->gh = sh->gw;
            if (sh->bpp != 8) sh->bpp = 4;
            p->nsheets++;
        } else if (!strncmp(s, "entity ", 7)) {
            entdef_t *e = &p->ents[p->nents];
            if (p->nents >= 16) { fclose(f); return -3; }
            memset(e, 0, sizeof(*e));
            char nm[32] = "", gr[32] = ""; int sh = 0;
            if (sscanf(s + 7, "%31s %31s sheet=%d x=%d y=%d anim=%d priority=%d", nm, gr, &sh, &e->x, &e->y, &e->anim, &e->priority) < 2) { fclose(f); return -3; }
            strcpy(e->name, nm); strcpy(e->graph, gr); e->sheet = sh;
            p->nents++;
        } else if (!strncmp(s, "node ", 5)) {
            node_t *n = &p->nodes[p->nnodes];
            if (p->nnodes >= 160) { fclose(f); return -3; }
            memset(n, 0, sizeof(*n));
            if (sscanf(s + 5, "%d %23s %d %d", &n->id, n->type, &n->x, &n->y) < 4) { fclose(f); return -3; }
            char *rest = s + 5;
            int fields = 4;
            for (int i = 0; i < fields; i++) { while (*rest && !isspace((unsigned char)*rest)) rest++; while (isspace((unsigned char)*rest)) rest++; }
            while (*rest && n->np < 6) {
                char *eq = strchr(rest, '=');
                if (!eq) break;
                char key[24]; int kl = 0;
                char *ks = rest;
                while (ks < eq && kl < 23) key[kl++] = *ks++;
                key[kl] = 0;
                char *vs = eq + 1, *ve = vs;
                while (*ve && !isspace((unsigned char)*ve)) ve++;
                int vl = (int)(ve - vs); if (vl > 95) vl = 95;
                memcpy(n->p[n->np].key, key, (size_t)kl + 1);
                memcpy(n->p[n->np].val, vs, (size_t)vl); n->p[n->np].val[vl] = 0;
                n->np++;
                rest = ve;
                while (isspace((unsigned char)*rest)) rest++;
            }
            p->nnodes++;
        } else if (!strncmp(s, "link ", 5)) {
            link_t *l = &p->links[p->nlinks];
            if (p->nlinks >= 320) { fclose(f); return -3; }
            int a, b; char pa[12], pb[12];
            if (sscanf(s + 5, "%d.%11s %d.%11s", &a, pa, &b, pb) != 4) { fclose(f); return -3; }
            l->src = a; l->dst = b; strcpy(l->sp, pa); strcpy(l->dp, pb);
            p->nlinks++;
        }
    }
    fclose(f);
    return 0;
}

/* ---------------- bytecode emitter ---------------- */
typedef struct { uint8_t *b; size_t len, cap; int err; } emitter_t;
static void e8(emitter_t *e, int v) { if (e->len < e->cap) e->b[e->len++] = (uint8_t)v; }
static void e16(emitter_t *e, int v) { e8(e, v & 255); e8(e, (v >> 8) & 255); }
static void eldi32(emitter_t *e, int r, int32_t v)
{
    e8(e, MSOP_LDI32); e8(e, r);
    e8(e, v & 255); e8(e, (v >> 8) & 255); e8(e, (v >> 16) & 255); e8(e, (v >> 24) & 255);
}
static size_t eop(emitter_t *e, int op, int a, int b, int c)
{
    size_t at = e->len;
    e8(e, op); e8(e, a); e8(e, b); e8(e, c);
    return at;
}
static void patch16(emitter_t *e, size_t at, int rel)
{
    if (at + 3 >= e->cap) return;
    e->b[at + 2] = (uint8_t)(rel & 255);
    e->b[at + 3] = (uint8_t)((rel >> 8) & 255);
}
static int imm_of(const node_t *n, const char *k, int def)
{ const char *v = prop(n, k, NULL); return v ? atoi(v) : def; }
static int fix_of(const node_t *n, const char *k, int def)
{
    const char *v = prop(n, k, NULL);
    return v ? (int)(atof(v) * 65536.0) : def;
}

static int compile_expr(project_t *p, int id, emitter_t *e, int depth);

static int compile_leaf(project_t *p, node_t *n, emitter_t *e)
{
    int r = 8 + (int)(e->len % 8);   /* rotate temps 8..15 */
    const char *t = n->type;
    if (!strcmp(t, "number")) { eldi32(e, r, fix_of(n, "value", 0)); }
    else if (!strcmp(t, "held") || !strcmp(t, "pressed")) {
        e8(e, !strcmp(t, "held") ? MSOP_GETBTN : MSOP_PRESSED); e8(e, r); e16(e, imm_of(n, "mask", MS_BTN_A));
    } else if (!strcmp(t, "random")) {
        e8(e, MSOP_RAND); e8(e, r); e16(e, imm_of(n, "max", 100));
    } else if (!strcmp(t, "timer")) eop(e, MSOP_LOADTIMER, r, 0, 0);
    else if (!strcmp(t, "touchx")) eop(e, MSOP_TOUCHX, r, 0, 0);
    else if (!strcmp(t, "touchy")) eop(e, MSOP_TOUCHY, r, 0, 0);
    else if (!strcmp(t, "sin") || !strcmp(t, "cos")) {
        int src = link_src(p, n->id, "value");
        int sr = src >= 0 ? compile_expr(p, src, e, 0) : 9;
        if (src < 0) eldi32(e, sr, fix_of(n, "value", 0));
        eop(e, !strcmp(t, "sin") ? MSOP_SIN : MSOP_COS, r, sr, 0);
    } else eop(e, MSOP_LDI, r, 0, 0);
    return r;
}

static int compile_expr(project_t *p, int id, emitter_t *e, int depth)
{
    node_t *n = find_node(p, id);
    if (!n || depth > 12) return 8;
    const char *t = n->type;
    int a = link_src(p, id, "a"), b = link_src(p, id, "b");
    if (!strcmp(t, "add") || !strcmp(t, "sub") || !strcmp(t, "mul") || !strcmp(t, "div")) {
        int ra = a >= 0 ? compile_expr(p, a, e, depth + 1) : 9;
        int rb = b >= 0 ? compile_expr(p, b, e, depth + 1) : 10;
        if (a < 0) eldi32(e, ra, fix_of(n, "a", 0));
        if (b < 0) eldi32(e, rb, fix_of(n, "b", 0));
        int op = !strcmp(t, "add") ? MSOP_ADD : !strcmp(t, "sub") ? MSOP_SUB : !strcmp(t, "mul") ? MSOP_MUL : MSOP_DIV;
        int r = 8 + (depth & 3);
        eop(e, op, r, ra, rb);
        return r;
    }
    return compile_leaf(p, n, e);
}

typedef struct { project_t *p; emitter_t *e; int label; } cc_t;
static void compile_chain(cc_t *c, int id, int depth);

static void compile_action(cc_t *c, node_t *n, int *stop)
{
    const char *t = n->type;
    emitter_t *e = c->e;
    int a = link_src(c->p, n->id, "value");
    int r = a >= 0 ? compile_expr(c->p, a, e, 0) : 9;
    if (a < 0 && (strstr(t, "x") || strstr(t, "y") || !strcmp(t, "setanim") || !strcmp(t, "wait")))
        { eldi32(e, r, fix_of(n, "amount", imm_of(n, "value", 0))); }
    if (!strcmp(t, "setx")) eop(e, MSOP_SETX, r, 0, 0);
    else if (!strcmp(t, "sety")) eop(e, MSOP_SETY, r, 0, 0);
    else if (!strcmp(t, "addx")) eop(e, MSOP_ADDX, r, 0, 0);
    else if (!strcmp(t, "addy")) eop(e, MSOP_ADDY, r, 0, 0);
    else if (!strcmp(t, "setvx")) eop(e, MSOP_SETVX, r, 0, 0);
    else if (!strcmp(t, "setvy")) eop(e, MSOP_SETVY, r, 0, 0);
    else if (!strcmp(t, "brightness")) eop(e, MSOP_FX, 0, r, 0);
    else if (!strcmp(t, "bgscrollx")) eop(e, MSOP_BGSCROLL, imm_of(n, "layer", 0), r, 10);
    else if (!strcmp(t, "bgscrolly")) eop(e, MSOP_BGSCROLL, imm_of(n, "layer", 0), 9, r);
    else if (!strcmp(t, "palentry")) { e8(e, MSOP_PALENTRY); e8(e, imm_of(n, "bank", 1)); e8(e, imm_of(n, "index", 1)); e8(e, r); }
    else if (!strcmp(t, "setanim")) { e8(e, MSOP_SETANIM); e8(e, 0); e16(e, imm_of(n, "id", 0)); }
    else if (!strcmp(t, "setframe")) { e8(e, MSOP_SETFRAME); e8(e, 0); e16(e, imm_of(n, "id", 0)); }
    else if (!strcmp(t, "hide")) { e8(e, MSOP_HIDE); e8(e, 0); e16(e, imm_of(n, "on", 0)); }
    else if (!strcmp(t, "wait")) { e8(e, MSOP_WAIT); e8(e, 0); e16(e, imm_of(n, "frames", 1)); }
    else if (!strcmp(t, "playsfx")) {
        e8(e, MSOP_LDI); e8(e, 9); e16(e, imm_of(n, "prog", 81));
        e8(e, MSOP_LDI); e8(e, 10); e16(e, imm_of(n, "note", 72));
        eop(e, MSOP_PLAYSFX, 0, 9, 10); }
    else if (!strcmp(t, "playsong")) { e8(e, MSOP_PLAYSONG); e8(e, 0); e16(e, imm_of(n, "slot", 0)); }
    else if (!strcmp(t, "stopsong")) { eop(e, MSOP_STOPSONG, 0, 0, 0); }
    else if (!strcmp(t, "spawn")) { e8(e, MSOP_SPAWN); e8(e, 8); e16(e, imm_of(n, "entity", 0)); }
    else if (!strcmp(t, "destroy")) { eop(e, MSOP_DESTROY, 0, 0, 0); *stop = 1; }
    else if (!strcmp(t, "signal")) { e8(e, MSOP_SIGNAL); e8(e, 0); e16(e, imm_of(n, "id", 0)); }
    else if (!strcmp(t, "end")) { eop(e, MSOP_HALT, 0, 0, 0); *stop = 1; }
}

static void compile_chain(cc_t *c, int id, int depth)
{
    while (id >= 0 && depth < 64) {
        node_t *n = find_node(c->p, id);
        if (!n) return;
        if (!strcmp(n->type, "if")) {
            int cond = link_src(c->p, n->id, "cond");
            int r = cond >= 0 ? compile_expr(c->p, cond, c->e, 0) : 9;
            size_t jz = eop(c->e, MSOP_JZ, r, 0, 0);
            int then_id = link_dst(c->p, n->id, "then");
            int else_id = link_dst(c->p, n->id, "else");
            if (then_id >= 0) compile_chain(c, then_id, depth + 1);
            size_t jmp = eop(c->e, MSOP_JMP, 0, 0, 0);
            patch16(c->e, jz, (int)(c->e->len - (jz + 4)));
            if (else_id >= 0) compile_chain(c, else_id, depth + 1);
            patch16(c->e, jmp, (int)(c->e->len - (jmp + 4)));
            id = link_dst(c->p, n->id, "next");
            continue;
        }
        int stop = 0;
        if (strcmp(n->type, "update") && strcmp(n->type, "start")) compile_action(c, n, &stop);
        if (stop) return;
        id = link_dst(c->p, n->id, "next");
        if (id < 0) id = link_dst(c->p, n->id, "exec");
    }
}

/* ---------------- PNG pipeline ---------------- */
static int palette_build(const uint8_t *rgba, int w, int h, int maxc, uint16_t *pal, int *pal_n)
{
    /* nearest-colour quantisation into a frequency-ranked palette */
    typedef struct { uint32_t c; int n; } cnt_t;
    cnt_t cnt[4096]; int nc = 0;
    *pal_n = 0;
    pal[(*pal_n)++] = 0;               /* index 0 transparent */
    for (int i = 0; i < w * h; i++) {
        const uint8_t *q = rgba + i * 4;
        if (q[3] < 128) continue;
        uint32_t c = (uint32_t)(q[0] >> 3) << 11 | (uint32_t)(q[1] >> 3) << 6 | (q[2] >> 3) << 1;
        int k;
        for (k = 0; k < nc; k++) if (cnt[k].c == c) { cnt[k].n++; break; }
        if (k == nc && nc < 4096) { cnt[nc].c = c; cnt[nc].n = 1; nc++; }
    }
    for (int i = 0; i < nc && *pal_n < maxc; i++) {
        int best = i;
        for (int j = i + 1; j < nc; j++) if (cnt[j].n > cnt[best].n) best = j;
        cnt_t t = cnt[i]; cnt[i] = cnt[best]; cnt[best] = t;
        pal[(*pal_n)++] = (uint16_t)(cnt[i].c);
    }
    return nc;
}
static int palette_map(const uint16_t *pal, int n, const uint8_t *q)
{
    if (q[3] < 128) return 0;
    int r = q[0], g = q[1], b = q[2], best = 1, bd = 1 << 30;
    for (int i = 1; i < n; i++) {
        int dr = r - (int)(((pal[i] >> 11) & 31) * 255 / 31);
        int dg = g - (int)(((pal[i] >> 5) & 63) * 255 / 63);
        int db = b - (int)((pal[i] & 31) * 255 / 31);
        int d = dr * dr + dg * dg + db * db;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

static unsigned char *load_png(const char *path, int *w, int *h)
{
    int n;
    return stbi_load(path, w, h, &n, 4);
}

/* SPR2 (canonical) writer.
 *
 *   u32 'SPR2' | u8 version=2 | u8 bpp | u16 pal_count | u16 tile_count
 *   | u16 frame_count | u16 reserved
 *   palette pal_count * u16 RGB565 (index 0 always 0x0000 = transparent)
 *   tiles   tile_count * (64 B @8bpp | 32 B @4bpp)
 *   frames  frame_count * 8 B: u16 tile_base, u8 w_tiles, u8 h_tiles,
 *           i8 pivot_x, i8 pivot_y, u8 flags, u8 reserved
 *
 * Invariants enforced by the writer (and re-checked by the reader):
 *   - the palette is quantised once: every index 1..pal_count-1 is
 *     referenced by at least one tile, and no colour appears twice;
 *   - index 0 is transparent, and pixels that quantise to 0x0000 are
 *     written as index 0 (transparent) rather than as a duplicate colour;
 *   - each frame owns a contiguous run of tiles (tile_base + row-major),
 *     so no cross-frame dedupe is allowed.
 */
int ms_convert_sprite(const char *png, const char *out, int gw, int gh, int bpp)
{
    int w, h; unsigned char *rgba = load_png(png, &w, &h);
    if (!rgba) return -1;
    if (gw <= 0 || gh <= 0 || gw % 8 || gh % 8 || gw > 64 || gh > 64) { stbi_image_free(rgba); return -2; }
    if (bpp != 4 && bpp != 8) { stbi_image_free(rgba); return -2; }
    if (w % gw || h % gh) { stbi_image_free(rgba); return -3; }

    const int maxc = bpp == 8 ? 255 : 15;     /* usable colours besides index 0 */
    typedef struct { uint16_t c; uint32_t n; } col_t;
    col_t cols[256];
    int nc = 0;
    for (int i = 0; i < w * h; i++) {
        const uint8_t *q = rgba + (size_t)i * 4;
        if (q[3] < 128) continue;
        uint16_t c = ms_rgb565(q[0], q[1], q[2]);
        if (!c) continue;                     /* black == transparent (index 0) */
        int k;
        for (k = 0; k < nc; k++) if (cols[k].c == c) { cols[k].n++; break; }
        if (k == nc) { if (nc >= 256) break; cols[nc].c = c; cols[nc].n = 1; nc++; }
    }
    if (nc > maxc) { stbi_image_free(rgba); return -7; }   /* too many colours */

    /* deterministic order: frequency desc, colour asc */
    for (int i = 0; i < nc; i++) {
        int best = i;
        for (int j = i + 1; j < nc; j++)
            if (cols[j].n > cols[best].n || (cols[j].n == cols[best].n && cols[j].c < cols[best].c))
                best = j;
        col_t t = cols[i]; cols[i] = cols[best]; cols[best] = t;
    }
    uint16_t pal[256];
    int paln = 0;
    pal[paln++] = 0;
    for (int i = 0; i < nc; i++) pal[paln++] = cols[i].c;

    const int tw = gw / 8, th = gh / 8, per = tw * th;
    const int nx = w / gw, ny = h / gh;
    const int nframes = nx * ny;
    if (nframes <= 0 || nframes > 4096) { stbi_image_free(rgba); return -4; }
    const size_t tilesz = bpp == 8 ? 64u : 32u;
    const size_t tile_n = (size_t)nframes * (size_t)per;
    if (tile_n > MS_MAX_TILES || tile_n > 65535u) { stbi_image_free(rgba); return -5; }

    uint8_t *tiles = calloc(tile_n, tilesz);
    uint8_t *frs = calloc((size_t)nframes, 8);
    if (!tiles || !frs) { free(tiles); free(frs); stbi_image_free(rgba); return -4; }

    for (int f = 0; f < nframes; f++) {
        const int fx = (f % nx) * gw, fy = (f / nx) * gh;
        for (int ty = 0; ty < th; ty++) for (int tx = 0; tx < tw; tx++) {
            uint8_t *t = tiles + ((size_t)f * (size_t)per + (size_t)(ty * tw + tx)) * tilesz;
            for (int yy = 0; yy < 8; yy++) for (int xx = 0; xx < 8; xx++) {
                const uint8_t *q = rgba + ((size_t)(fy + ty * 8 + yy) * (size_t)w + (size_t)(fx + tx * 8 + xx)) * 4;
                int idx = 0;
                if (q[3] >= 128) {
                    uint16_t c = ms_rgb565(q[0], q[1], q[2]);
                    if (c) for (int k = 1; k < paln; k++) if (pal[k] == c) { idx = k; break; }
                }
                if (bpp == 8) t[yy * 8 + xx] = (uint8_t)idx;
                else {
                    int o = yy * 4 + (xx >> 1);
                    if (xx & 1) t[o] |= (uint8_t)(idx << 4); else t[o] |= (uint8_t)idx;
                }
            }
        }
        uint8_t *fr = frs + (size_t)f * 8;
        ms_wr16(fr, (uint16_t)(f * per));
        fr[2] = (uint8_t)tw; fr[3] = (uint8_t)th;
        fr[4] = 0; fr[5] = 0; fr[6] = 0; fr[7] = 0;
    }

    const size_t total = 14u + (size_t)paln * 2u + tile_n * tilesz + (size_t)nframes * 8u;
    uint8_t *outb = malloc(total);
    if (!outb) { free(tiles); free(frs); stbi_image_free(rgba); return -4; }
    ms_wr32(outb, MS_SPR_MAGIC);
    outb[4] = 2;                       /* version */
    outb[5] = (uint8_t)bpp;
    ms_wr16(outb + 6, (uint16_t)paln);
    ms_wr16(outb + 8, (uint16_t)tile_n);
    ms_wr16(outb + 10, (uint16_t)nframes);
    ms_wr16(outb + 12, 0);
    size_t o = 14;
    for (int i = 0; i < paln; i++, o += 2) ms_wr16(outb + o, pal[i]);
    memcpy(outb + o, tiles, tile_n * tilesz); o += tile_n * tilesz;
    memcpy(outb + o, frs, (size_t)nframes * 8u);

    int rc = 0;
    FILE *f = fopen(out, "wb");
    if (!f || fwrite(outb, 1, total, f) != total) rc = -6;
    if (f) fclose(f);
    free(outb); free(tiles); free(frs); stbi_image_free(rgba);
    return rc;
}

int ms_convert_level(const char *png, int bpp, unsigned char *tiles_out, size_t *tiles_len,
                     unsigned char *map_out, size_t *map_len, unsigned short *pal_out,
                     int *pal_count, int *map_w, int *map_h,
                     int *logical_w, int *logical_h)
{
    int w, h; unsigned char *rgba = load_png(png, &w, &h);
    if (!rgba) return -1;
    int tw = (w + 7) / 8, th = (h + 7) / 8;
    int pw = tw, ph = th;
    while (pw & (pw - 1)) pw = (pw | (pw - 1)) + 1;   /* next power of two */
    while (ph & (ph - 1)) ph = (ph | (ph - 1)) + 1;
    if (pw < 1) pw = 1;
    if (ph < 1) ph = 1;
    uint16_t pal[256]; int pn = 0;
    palette_build(rgba, w, h, bpp == 8 ? 256 : 16, pal, &pn);
    if (pn < 2) pal[pn++] = 0;
    uint8_t *tiles = calloc((size_t)tw * th, 64);
    uint16_t *map = calloc((size_t)pw * ph, 2);
    if (!tiles || !map) { free(tiles); free(map); stbi_image_free(rgba); return -2; }
    int tile_n = 0;
    for (int ty = 0; ty < th; ty++) for (int tx = 0; tx < tw; tx++) {
        uint8_t t[64]; memset(t, 0, sizeof(t));
        for (int yy = 0; yy < 8; yy++) for (int xx = 0; xx < 8; xx++) {
            int sx = tx * 8 + xx, sy = ty * 8 + yy;
            int idx = 0;
            if (sx < w && sy < h) idx = palette_map(pal, pn, rgba + ((size_t)sy * w + sx) * 4);
            if (bpp == 8) t[yy * 8 + xx] = (uint8_t)idx;
            else { int o = yy * 4 + (xx >> 1); if (xx & 1) t[o] |= (uint8_t)(idx << 4); else t[o] |= (uint8_t)idx; }
        }
        int found = -1;
        size_t tsz = bpp == 8 ? 64u : 32u;
        for (int k = 0; k < tile_n; k++) if (!memcmp(tiles + (size_t)k * 64, t, tsz)) { found = k; break; }
        if (found < 0) {
            if (tile_n >= (int)MS_MAX_TILES) { free(tiles); free(map); stbi_image_free(rgba); return -3; }
            found = tile_n; memcpy(tiles + (size_t)tile_n * 64, t, 64); tile_n++;
        }
        map[ty * pw + tx] = (uint16_t)found;
    }
    size_t tl = (size_t)tile_n * (bpp == 8 ? 64u : 32u);
    if (tiles_out) { memcpy(tiles_out, tiles, tl); }
    if (map_out) memcpy(map_out, map, (size_t)pw * ph * 2);
    if (pal_out) memcpy(pal_out, pal, (size_t)pn * 2);
    *tiles_len = tl; *map_len = (size_t)pw * ph * 2;
    *pal_count = pn; *map_w = pw; *map_h = ph;
    if (logical_w) *logical_w = tw;
    if (logical_h) *logical_h = th;
    free(tiles); free(map); stbi_image_free(rgba);
    return 0;
}

int ms_write_png(const char *path, int w, int h, const unsigned short *rgb565)
{
    uint8_t *rgba = malloc((size_t)w * h * 4);
    if (!rgba) return -1;
    for (int i = 0; i < w * h; i++) {
        uint16_t c = rgb565[i];
        rgba[i * 4 + 0] = (uint8_t)(((c >> 11) & 31) * 255 / 31);
        rgba[i * 4 + 1] = (uint8_t)(((c >> 5) & 63) * 255 / 63);
        rgba[i * 4 + 2] = (uint8_t)((c & 31) * 255 / 31);
        rgba[i * 4 + 3] = 255;
    }
    int ok = stbi_write_png(path, w, h, 4, rgba, w * 4);
    free(rgba);
    return ok ? 0 : -2;
}

/* ---------------- build ---------------- */
/* ---------------- build ---------------- */

static int add_manifest(ms_cart_writer_t *cw, const project_t *p, int has_pal)
{
    char text[1024];
    int n = snprintf(text, sizeof(text),
        "title=%s\nauthor=%s\nmode=%s\nqfa=%d\nvoices=%d\nmusic_voices=%d\nsfx_voices=%d\n"
        "backdrop=0x%04X\nbrightness=16\n",
        p->title[0] ? p->title : "Mosaic Game", p->author, p->mode, p->qfa,
        p->voices, p->mv, p->sv, p->backdrop);
    if (n < 0 || n >= (int)sizeof(text)) return MS_CART_E_NOSPACE;
    if (has_pal) {
        int k = snprintf(text + n, sizeof(text) - (size_t)n, "palette_bg=pal_bg\n");
        if (k < 0) return MS_CART_E_NOSPACE;
        n += k;
    }
    if (p->music[0]) {
        int k = snprintf(text + n, sizeof(text) - (size_t)n, "music=music\n");
        if (k < 0) return MS_CART_E_NOSPACE;
        n += k;
    }
    if (n >= (int)sizeof(text)) return MS_CART_E_NOSPACE;
    return ms_cart_add(cw, "manifest", MS_A_MANIFEST, text, (size_t)n);
}

int ms_build_project_opts(const char *path, ms_build_result_t *out, int canonical)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    project_t p;
    const int pr = parse_project(path, &p);
    if (pr != 0) { snprintf(out->error, sizeof(out->error), "project parse error %d", pr); return -2; }
    strncpy(out->title, p.title, 32);
    strncpy(out->author, p.author, 32);
    strncpy(out->mode, p.mode, 11);
    out->qfa = p.qfa; out->voices = p.voices; out->music_voices = p.mv; out->sfx_voices = p.sv;
    out->backdrop = p.backdrop;

    static ms_cart_writer_t cw;
    ms_cart_writer_begin(&cw, out->cart, sizeof(out->cart));

    /* ---- sprite sheets (SPR2) ---- */
    for (int i = 0; i < p.nsheets; i++) {
        char tmp[320], spr[320];
        snprintf(tmp, sizeof(tmp), "%s/%s", p.project_dir, p.sheets[i].path);
        snprintf(spr, sizeof(spr), "%s/.mosaic_%s.spr2", p.project_dir, p.sheets[i].name);
        int rc = ms_convert_sprite(tmp, spr, p.sheets[i].gw, p.sheets[i].gh, p.sheets[i].bpp);
        if (rc != 0) {
            snprintf(out->error, sizeof(out->error),
                     "sprite %s: conversion failed (%d: too many colours or bad grid) — %s",
                     p.sheets[i].name, rc, tmp);
            return -4;
        }
        FILE *f = fopen(spr, "rb");
        if (!f) { snprintf(out->error, sizeof(out->error), "cannot reopen %s", spr); return -4; }
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *b = malloc((size_t)n);
        if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return -4; }
        fclose(f);
        remove(spr);
        rc = ms_cart_add(&cw, p.sheets[i].name, MS_A_SPRITES, b, (size_t)n);
        free(b);
        if (rc != MS_CART_OK) {
            snprintf(out->error, sizeof(out->error), "sprite %s: %s", p.sheets[i].name, ms_cart_err_str(rc));
            return -4;
        }
        out->sheets++;
    }

    /* ---- level: tiles + map + palette ---- */
    int has_pal = 0;
    if (p.level[0]) {
        char lv[256];
        snprintf(lv, sizeof(lv), "%s/%s", p.project_dir, p.level);
        static uint8_t tiles[512 * 1024], map[512 * 1024];
        static uint16_t pal[256];
        size_t tl = 0, ml = 0; int pn = 0, mw = 0, mh = 0, tw = 0, th = 0;
        int rc = ms_convert_level(lv, (int)p.level_bpp, tiles, &tl, map, &ml, pal, &pn, &mw, &mh, &tw, &th);
        if (rc != 0) { snprintf(out->error, sizeof(out->error), "level conversion failed: %s", lv); return -5; }
        const size_t per = p.level_bpp == 8 ? 64u : 32u;
        const size_t tile_n = tl / per;
        uint8_t *tb = malloc(3 + tl);
        tb[0] = (uint8_t)p.level_bpp;
        ms_wr16(tb + 1, (uint16_t)tile_n);
        memcpy(tb + 3, tiles, tl);
        rc = ms_cart_add(&cw, "level_tiles", MS_A_BG_TILES, tb, 3 + tl);
        free(tb);
        if (rc != MS_CART_OK) {
            snprintf(out->error, sizeof(out->error), "level tiles: %s (%zu tiles)", ms_cart_err_str(rc), tile_n);
            return -5;
        }
        const size_t entries = ml / 2;
        uint8_t *mb = malloc(6 + ml);          /* v2.1: count | w | h | entries */
        ms_wr16(mb, (uint16_t)entries);
        ms_wr16(mb + 2, (uint16_t)tw);
        ms_wr16(mb + 4, (uint16_t)th);
        memcpy(mb + 6, map, ml);
        rc = ms_cart_add(&cw, "level_map", MS_A_BG_MAP, mb, 6 + ml);
        free(mb);
        if (rc != MS_CART_OK) {
            snprintf(out->error, sizeof(out->error), "level map: %s (%zux%zu entries)", ms_cart_err_str(rc), entries, (size_t)1);
            return -5;
        }
        uint8_t *pb = malloc(4 + (size_t)pn * 2);   /* v2.1: count | bank | rsv | entries */
        ms_wr16(pb, (uint16_t)pn);
        pb[2] = 0;                                  /* bank 0 = background */
        pb[3] = 0;                                  /* reserved */
        for (int i = 0; i < pn; i++) ms_wr16(pb + 4 + i * 2, pal[i]);
        rc = ms_cart_add(&cw, "pal_bg", MS_A_PALETTE, pb, 4 + (size_t)pn * 2);
        free(pb);
        if (rc != MS_CART_OK) {
            snprintf(out->error, sizeof(out->error), "palette: %s", ms_cart_err_str(rc));
            return -5;
        }
        has_pal = 1;
        (void)mw; (void)mh;   /* padded dims now travel inside the map payload */
        out->tiles++; out->maps++;
    }

    /* ---- music ---- */
    if (p.music[0]) {
        char mp[256];
        snprintf(mp, sizeof(mp), "%s/%s", p.project_dir, p.music);
        FILE *f = fopen(mp, "rb");
        if (f) {
            fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
            uint8_t *b = malloc((size_t)n);
            if (b && fread(b, 1, (size_t)n, f) == (size_t)n) {
                if (ms_cart_add(&cw, "music", MS_A_MIDI, b, (size_t)n) == MS_CART_OK) out->songs++;
                else snprintf(out->error, sizeof(out->error), "music rejected (SMF only, no PCM)");
            }
            fclose(f); free(b);
        }
    }

    /* ---- Mosaic extensions: manifest + compiled program ---- */
    if (!canonical) {
        int rc = add_manifest(&cw, &p, has_pal);
        if (rc != MS_CART_OK) { snprintf(out->error, sizeof(out->error), "manifest: %s", ms_cart_err_str(rc)); return -6; }

        static uint8_t code[MS_PROGRAM_MAX];
        emitter_t e = { code, 0, sizeof(code), 0 };
        static ms_entity_def_t defs[16];
        memset(defs, 0, sizeof(defs));
        for (int i = 0; i < p.nents; i++) {
            entdef_t *ed = &p.ents[i];
            int start_node = -1, upd_node = -1;
            for (int k = 0; k < p.nnodes; k++) {
                if (!strcmp(p.nodes[k].type, "start")) start_node = p.nodes[k].id;
                if (!strcmp(p.nodes[k].type, "update")) upd_node = p.nodes[k].id;
            }
            cc_t cc = { &p, &e, 0 };
            size_t sc = 0, uc = 0;
            if (start_node >= 0) { sc = e.len; compile_chain(&cc, start_node, 0); eop(&e, MSOP_RET, 0, 0, 0); }
            if (upd_node >= 0) { uc = e.len; compile_chain(&cc, upd_node, 0); eop(&e, MSOP_RET, 0, 0, 0); }
            defs[i].start_pc = start_node >= 0 ? (uint16_t)sc : 0xFFFFu;
            defs[i].update_pc = upd_node >= 0 ? (uint16_t)uc : 0xFFFFu;
            defs[i].sheet = (uint16_t)(ed->sheet >= 0 && ed->sheet < p.nsheets ? ed->sheet : 0);
            defs[i].anim = (uint16_t)ed->anim;
            defs[i].x = (int16_t)ed->x; defs[i].y = (int16_t)ed->y;
            defs[i].priority = (uint8_t)ed->priority;
            snprintf(defs[i].name, sizeof(defs[i].name), "%.15s", ed->name);
        }
        const size_t code_len = (e.len + 3) & ~(size_t)3;
        const size_t total = 32 + code_len + (size_t)p.nents * 32;
        uint8_t *prog = calloc(1, total);
        if (!prog) { snprintf(out->error, sizeof(out->error), "out of memory"); return -6; }
        ms_wr32(prog, MS_PROG_MAGIC); ms_wr32(prog + 4, 1);
        ms_wr32(prog + 8, (uint32_t)code_len); ms_wr32(prog + 12, (uint32_t)(32 + code_len));
        ms_wr16(prog + 16, (uint16_t)p.nents);
        ms_wr16(prog + 18, 0); ms_wr16(prog + 20, 0);
        memcpy(prog + 32, code, e.len);
        memcpy(prog + 32 + code_len, defs, (size_t)p.nents * 32);
        int rc2 = ms_cart_add(&cw, "program", MS_A_PROGRAM, prog, total);
        free(prog);
        if (rc2 != MS_CART_OK) {
            snprintf(out->error, sizeof(out->error), "compiled program rejected (%zu B): %s", total, ms_cart_err_str(rc2));
            return -7;
        }
        out->programs++;
    }

    const int len = ms_cart_finish(&cw);
    if (len <= 0) {
        snprintf(out->error, sizeof(out->error), "cart finalise failed: %s", ms_cart_err_str(len));
        return -8;
    }
    out->cart_len = (size_t)len;
    return 0;
}

int ms_build_project(const char *project_path, ms_build_result_t *out)
{
    return ms_build_project_opts(project_path, out, 0);
}
