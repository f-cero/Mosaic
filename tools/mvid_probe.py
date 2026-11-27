#!/usr/bin/env python3
"""MVID probe (reference encoder / budget tool for docs/10-fmv.md)

Original note: MVID probe — what tile-video and pixel-delta video cost inside Mosaic's PPU
limits: 320x240, 8x8 tiles (40x30 = 1200), 4bpp, 1024-tile sheet, 256-colour
global palette laid out as 16 slots of 16. No engine changes: pure budget."""
import sys, os, math
import numpy as np
from PIL import Image, ImageDraw

W, H, T = 320, 240, 8
GW, GH = W // T, H // T
TILES = GW * GH
SHEET, TILE_BYTES = 1024, 32

def build_palette(frames, n=256):
    mont = Image.new('RGB', (W, H * len(frames)))
    for i, f in enumerate(frames):
        mont.paste(f, (0, i * H))
    return mont.quantize(colors=n, method=Image.MEDIANCUT, dither=Image.Dither.NONE)

def slots_from_palette(pal):
    q = (pal.getpalette() or []) + [0] * 768
    p = np.array(q[:768], dtype=np.float32).reshape(256, 3)
    return p.reshape(16, 16, 3).copy()

def slot_tables(pal, slots):
    q = (pal.getpalette() or []) + [0] * 768
    p = np.array(q[:768], dtype=np.float32).reshape(256, 3)
    flat = slots.reshape(256, 3)
    D = np.linalg.norm(p[:, None, :] - flat[None, :, :], axis=2).reshape(256, 16, 16)
    return D.min(axis=2), D.argmin(axis=2)          # (256,16) dist, (256,16) entry

def quantise(frame, pal):
    return np.array(frame.quantize(palette=pal, dither=Image.Dither.NONE), dtype=np.uint8)

def tile_hist(idx):
    h = np.zeros((TILES, 256), dtype=np.float32)
    for ty in range(GH):
        for tx in range(GW):
            h[ty * GW + tx] = np.bincount(
                idx[ty*T:(ty+1)*T, tx*T:(tx+1)*T].ravel(), minlength=256)
    return h

def frames_game(n=24, src='examples/starfall/assets/level.png'):
    lvl = Image.open(src).convert('RGB')
    lw = lvl.size[0]
    out = []
    for i in range(n):
        x0 = int(i * 6) % max(1, lw - W)
        f = lvl.crop((x0, 0, x0 + W, H)).copy()
        d = ImageDraw.Draw(f)
        px, py = 140 + int(40*math.sin(i/6.0)), 150 + int(6*math.sin(i/2.0))
        d.rectangle([px, py, px+14, py+20], fill=(240, 214, 170))
        d.rectangle([px, py, px+14, py+8], fill=(80, 150, 240))
        d.rectangle([px+2, py+10, px+5, py+13], fill=(20, 20, 30))
        d.rectangle([px+9, py+10, px+12, py+13], fill=(20, 20, 30))
        out.append(f)
    return out

def frames_photo(n=24, src='photo.jpg'):
    im = Image.open(src).convert('RGB')
    out = []
    for i in range(n):
        z = 1.0 + 0.35 * i / max(1, n - 1)
        im2 = im.resize((int(W*z*1.3), int(H*z*1.3)), Image.BILINEAR)
        x0 = (im2.size[0] - W) * i // max(1, n-1)
        y0 = (im2.size[1] - H) * i // max(1, n-1)
        out.append(im2.crop((x0, y0, x0+W, y0+H)))
    return out

def encode_tile(frames, recon_from=None):
    pal = build_palette(frames)
    slots = slots_from_palette(pal)
    slotdist, slotnear = slot_tables(pal, slots)
    cache, lru, evicted = {}, [], 0
    prev = np.zeros(TILES, dtype=np.uint16)
    stats, recon = [], None
    for fi, f in enumerate(frames):
        idx = quantise(f, pal)
        best = np.argmin(tile_hist(idx) @ slotdist, axis=1)
        ids = np.zeros(TILES, dtype=np.uint16)
        new_bytes = new_tiles = 0
        out = np.zeros((H, W, 3), dtype=np.uint8) if fi == recon_from else None
        for t in range(TILES):
            s = int(best[t]); ty, tx = divmod(t, GW)
            blk = idx[ty*T:(ty+1)*T, tx*T:(tx+1)*T]
            q = slotnear[blk.astype(np.int32), s]
            if out is not None:
                out[ty*T:(ty+1)*T, tx*T:(tx+1)*T] = slots[s][q]
            key = (s, q.tobytes())
            if key in cache:
                tid = cache[key]
                lru.remove(tid); lru.append(tid)
            else:
                if len(cache) >= SHEET:
                    old = lru.pop(0)
                    for k, v in list(cache.items()):
                        if v == old:
                            del cache[k]; break
                    evicted += 1
                tid = len(cache); cache[key] = tid; lru.append(tid)
                new_bytes += TILE_BYTES; new_tiles += 1
            ids[t] = tid
        changed = np.nonzero(ids != prev)[0]; prev = ids
        runs = []
        if len(changed):
            a = b = changed[0]
            for c in changed[1:]:
                if c == b + 1: b = c
                else: runs.append((a, b)); a = b = c
            runs.append((a, b))
        stats.append(dict(map=1 + sum(4 + 2*(b-a+1) for a, b in runs),
                          tiles=2 + new_bytes, new=new_tiles))
        if out is not None: recon = Image.fromarray(out)
    return stats, pal, slots, recon

def rle_nibbles(block):
    out = bytearray(); run = 1; prev = int(block[0])
    for v in block[1:]:
        v = int(v)
        if v == prev and run < 15: run += 1
        else: out.append((run << 4) | prev); prev = v; run = 1
    out.append((run << 4) | prev)
    return bytes(out)

def encode_pixel(frames):
    pal = build_palette(frames); prev = None; stats = []
    for f in frames:
        idx = quantise(f, pal); nbytes = blocks = 0
        for by in range(GH):
            for bx in range(GW):
                blk = idx[by*T:(by+1)*T, bx*T:(bx+1)*T].ravel()
                if prev is not None and np.array_equal(blk, prev[by*T:(by+1)*T, bx*T:(bx+1)*T].ravel()):
                    continue
                nbytes += len(rle_nibbles(blk)); blocks += 1
        stats.append(dict(map=0, tiles=2+nbytes, new=blocks)); prev = idx
    return stats, pal

def summarise(name, st):
    avg = sum(s['map']+s['tiles'] for s in st)/len(st)
    peak = max(s['map']+s['tiles'] for s in st)
    print(f"  {name:20s} avg {avg:7.0f} B/f  peak {peak:7.0f} B/f  "
          f"new {sum(s['new'] for s in st)/len(st):6.1f}  " +
          "  ".join(f"{f}fps {avg*f/1024:6.1f}KB/s" for f in (12, 15, 20, 30)))
    return avg, peak

def psnr(a, b):
    a = np.asarray(a, np.float32); b = np.asarray(b, np.float32)
    mse = ((a-b)**2).mean()
    return 99.0 if mse < 1e-9 else float(10*math.log10(255**2/mse))

def frames_mid(n=24):
    """Antialiased, gradient-lit art: what a hand-painted modern pixel-art
    scene looks like after a bilinear upscale and a lighting pass."""
    g = frames_game(n)
    out = []
    for i, f in enumerate(g):
        f = f.resize((int(W*1.05), int(H*1.05)), Image.BILINEAR).crop((8, 8, W+8, H+8))
        d = ImageDraw.Draw(f)
        for y in range(H):
            a = int(60 * (1 - y / H) ** 1.5)
            d.line([(0, y), (W, y)], fill=None)
        grad = Image.new('L', (1, H))
        grad.putdata([int(70 * (1 - y / H) ** 1.5) for y in range(H)])
        glow = Image.new('RGB', (W, H), (120, 90, 200))
        f = Image.composite(glow, f, grad.resize((W, H)))
        out.append(f)
    return out

if __name__ == '__main__':
    out = {}
    g = frames_game()
    st, pal, slots, rg = encode_tile(g, recon_from=12)
    print("game art, %d frames, %d unique tiles in sheet" % (len(g), len(set())))
    out['game'] = summarise("tile 4bpp (game)", st)
    print("  quality %.2f dB" % psnr(g[12], rg))
    stp, _ = encode_pixel(g)
    out['gamep'] = summarise("pixel 4bpp (game)", stp)
    print("  intra (frame 0) %d B" % (st[0]['map']+st[0]['tiles']))
    m = frames_mid()
    stm, palm, slotsm, rm = encode_tile(m, recon_from=12)
    print("antialiased + gradient art")
    out['mid'] = summarise("tile 4bpp (mid)", stm)
    print("  quality %.2f dB  intra %d B" % (psnr(m[12], rm), stm[0]['map']+stm[0]['tiles']))
    if os.path.exists('photo.jpg'):
        ph = frames_photo()
        st2, pal2, slots2, rp = encode_tile(ph, recon_from=12)
        print("photographic (ken burns), %d frames" % len(ph))
        out['photo'] = summarise("tile 4bpp (photo)", st2)
        print("  quality %.2f dB" % psnr(ph[12], rp))
        stp2, _ = encode_pixel(ph)
        out['photop'] = summarise("pixel 4bpp (photo)", stp2)
        grid = Image.new('RGB', (W*2+8, H*2+8), (24, 26, 32))
        grid.paste(g[12], (0, 0)); grid.paste(rg, (W+8, 0))
        grid.paste(ph[12], (0, H+8)); grid.paste(rp, (W+8, H+8))
        grid.save('preview.png'); print("preview.png written")
    print("\nLCD scanout, full 320x240 RGB565 frame:")
    for hz in (20, 27, 40, 55, 80):
        ms = W*H*16/hz*1000.0
        print(f"  {hz:3d} MHz  {ms:6.2f} ms/frame  ceiling {1000/ms:5.1f} fps")
    print("link, 40 MHz SPI, 4 KB payload:")
    per = 4096*8/40e6 + 4e-6
    print(f"  {4096/per/1e6:5.2f} MB/s usable")

def encode_tile_mc(frames, maxs=2):
    """Tile video with map-level motion compensation: pick the tile shift that
    minimises changed cells, emit it plus only the changed cells. This is what
    a real MVID-T encoder does; the PPU gets the shift for free via scroll."""
    pal = build_palette(frames); slots = slots_from_palette(pal)
    slotdist, slotnear = slot_tables(pal, slots)
    cache, lru = {}, []
    prev = np.zeros(TILES, dtype=np.uint16)
    stats = []
    for fi, f in enumerate(frames):
        idx = quantise(f, pal)
        best = np.argmin(tile_hist(idx) @ slotdist, axis=1)
        ids = np.zeros(TILES, dtype=np.uint16)
        newb = 0
        for t in range(TILES):
            s = int(best[t]); ty, tx = divmod(t, GW)
            q = slotnear[idx[ty*T:(ty+1)*T, tx*T:(tx+1)*T].astype(np.int32), s]
            key = (s, q.tobytes())
            if key in cache:
                tid = cache[key]; lru.remove(tid); lru.append(tid)
            else:
                if len(cache) >= SHEET:
                    old = lru.pop(0)
                    for k, v in list(cache.items()):
                        if v == old: del cache[k]; break
                tid = len(cache); cache[key] = tid; lru.append(tid); newb += TILE_BYTES
            ids[t] = tid
        # best tile-grid shift against the previous frame
        bestshift, bestchanged = (0, 0), TILES + 1
        for sy in range(-maxs, maxs+1):
            for sx in range(-maxs, maxs+1):
                cand = np.full(TILES, 0xFFFF, dtype=np.uint16)
                for ty in range(GH):
                    sy2 = ty - sy
                    if not (0 <= sy2 < GH): continue
                    for tx in range(GW):
                        sx2 = tx - sx
                        if 0 <= sx2 < GW:
                            cand[ty*GW+tx] = prev[sy2*GW+sx2]
                ch = int((cand != ids).sum()) + (1 if (sx or sy) else 0)
                if ch < bestchanged: bestchanged, bestshift, bestcand = ch, (sx, sy), cand
        ref = bestcand if bestchanged <= TILES else prev
        changed = np.nonzero(ids != ref)[0]
        prev = ids
        runs = []
        if len(changed):
            a = b = changed[0]
            for c in changed[1:]:
                if c == b + 1: b = c
                else: runs.append((a, b)); a = b = c
            runs.append((a, b))
        stats.append(dict(map=1 + sum(4 + 2*(b-a+1) for a, b in runs), tiles=2+newb,
                          new=len(changed), shift=bestshift))
    return stats

def frames_photo_pan(n=24, src='photo.jpg'):
    im = Image.open(src).convert('RGB')
    im2 = im.resize((int(W*1.6), int(H*1.6)), Image.BILINEAR)
    out = []
    for i in range(n):
        x0 = int(6*i); y0 = int(2*i)
        out.append(im2.crop((x0, y0, x0+W, y0+H)))
    return out
