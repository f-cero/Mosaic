/* SPDX-License-Identifier: Apache-2.0
 * Mosaic — mvid.js: MVID v1 decoder, player and <mosaic-video> element.
 *
 * The same container codec/mvid_decode.c gives the console, in dependency-free
 * JavaScript: one header, N frame records, a tile atlas, a tilemap of
 * SNES-packed entries and a 256-entry palette. Scroll, flips, 4bpp/8bpp and the
 * PPU brightness ramp are applied exactly as the engine applies them, so a clip
 * looks the same in a page as on the handheld.
 *
 * Works in browsers and in Node (module.exports), so it can also back an
 * Electron app, a build step, or a thumbnail generator.
 */
(function (root, factory) {
  if (typeof module === 'object' && module.exports) module.exports = factory();
  else root.MVID = factory();
})(typeof globalThis !== 'undefined' ? globalThis : this, function () {
  'use strict';

  var HDR = 56, MAX_TILES = 1024, MAX_FRAMES = 4096, MAX_MAP = 128;
  var F_LOOP = 1, F_AUDIO = 2, F_QFA = 4;
  var T_INTRA = 0;
  var BLACK = 0xFF000000;

  function fail(code, msg) { var e = new Error(msg); e.code = code; return e; }
  function rd16(b, o) { return b[o] | (b[o + 1] << 8); }
  function rd32(b, o) {
    return (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;
  }
  function nextPow2(v) { var r = 1; while (r < v && r < 0x8000) r <<= 1; return r; }

  var CRC_T = (function () {
    var t = new Uint32Array(256);
    for (var i = 0; i < 256; i++) {
      var x = i;
      for (var k = 0; k < 8; k++) x = (x & 1) ? (0xEDB88320 ^ (x >>> 1)) : (x >>> 1);
      t[i] = x >>> 0;
    }
    return t;
  })();

  function crc32(bytes) {
    var c = 0xFFFFFFFF;
    for (var i = 0; i < bytes.length; i++) c = (CRC_T[(c ^ bytes[i]) & 0xFF] ^ (c >>> 8)) >>> 0;
    return (c ^ 0xFFFFFFFF) >>> 0;
  }

  /* PPU brightness ramp: 16 = unchanged, 0 = black. Integer maths, like the C. */
  function scale565(c, br) {
    if (br >= 16) return c;
    var r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return ((((r * br / 16) | 0) << 11) | (((g * br / 16) | 0) << 5) | ((b * br / 16) | 0)) & 0xFFFF;
  }

  /* RGB565 -> 0xAABBGGRR, which is RGBA byte order in a little-endian Uint32 view. */
  function rgba(c) {
    var r = (((c >> 11) & 31) * 255 / 31) | 0;
    var g = (((c >> 5) & 63) * 255 / 63) | 0;
    var b = ((c & 31) * 255 / 31) | 0;
    return (0xFF000000 | (b << 16) | (g << 8) | r) >>> 0;
  }

  function toBytes(src) {
    if (src instanceof Uint8Array) return src;
    if (src instanceof ArrayBuffer) return new Uint8Array(src);
    if (src && src.buffer instanceof ArrayBuffer) return new Uint8Array(src.buffer, src.byteOffset || 0, src.byteLength);
    throw fail('PARAM', 'expected an ArrayBuffer or Uint8Array');
  }

  function parse(b) {
    if (!b || b.length < HDR) throw fail('SIZE', 'truncated header');
    if (b[0] !== 0x4D || b[1] !== 0x56 || b[2] !== 0x49 || b[3] !== 0x44)
      throw fail('MAGIC', 'not an MVID file');
    if (rd16(b, 4) !== 1) throw fail('VERSION', 'unsupported MVID version');
    var hdr = rd16(b, 6);
    if (hdr < HDR || hdr > b.length) throw fail('SIZE', 'bad header length');
    var i = {
      width: rd16(b, 8), height: rd16(b, 10), fps: b[12], flags: b[13],
      frames: rd16(b, 14), mapW: rd16(b, 16), mapH: rd16(b, 18), bpp: b[20],
      palCount: rd16(b, 22), palOff: rd32(b, 24), indexOff: rd32(b, 28),
      audioOff: rd32(b, 32), audioLen: rd32(b, 36), sheetBytes: rd32(b, 40),
      crc: rd32(b, 44), size: b.length, hdr: hdr
    };
    if (!((i.width === 320 && i.height === 240) || (i.width === 160 && i.height === 120)))
      throw fail('SIZE', 'coded size must be 320x240 or 160x120');
    if (i.flags & ~7) throw fail('SIZE', 'reserved flag bits set');
    if (i.fps < 1 || i.fps > 60) throw fail('SIZE', 'fps out of range');
    if (i.bpp !== 4 && i.bpp !== 8) throw fail('SIZE', 'bpp must be 4 or 8');
    if (i.frames < 1 || i.frames > MAX_FRAMES) throw fail('SIZE', 'frame count out of range');
    if (i.mapW < 1 || i.mapH < 1 || i.mapW > MAX_MAP || i.mapH > MAX_MAP)
      throw fail('SIZE', 'map dims out of range');
    if (i.mapW * 8 < i.width || i.mapH * 8 < i.height)
      throw fail('SIZE', 'map smaller than the screen');
    if (i.palCount < 1 || i.palCount > 256) throw fail('SIZE', 'palette size out of range');
    if (i.palOff + i.palCount * 2 > i.size) throw fail('SIZE', 'palette past EOF');
    if (i.indexOff + i.frames * 4 > i.size) throw fail('SIZE', 'frame index past EOF');
    if (i.audioLen && i.audioOff + i.audioLen > i.size) throw fail('SIZE', 'audio past EOF');
    if (i.sheetBytes > MAX_TILES * 64) throw fail('SIZE', 'atlas too large');
    i.tileBytes = i.bpp === 8 ? 64 : 32;
    i.mw = nextPow2(i.mapW); i.mh = nextPow2(i.mapH);
    i.qfa = !!(i.flags & F_QFA);
    i.loop = !!(i.flags & F_LOOP);
    return i;
  }

  /* Structural check of every frame, exactly what mvid_check() walks. */
  function walk(b, i) {
    var tb = i.tileBytes;
    for (var f = 0; f < i.frames; f++) {
      var off = rd32(b, i.indexOff + f * 4);
      if (off + 16 > i.size) throw fail('FRAME', 'frame ' + f + ' record past EOF');
      if (b[off] > 2) throw fail('FRAME', 'frame ' + f + ' bad type');
      var len = rd16(b, off + 2);
      if (off + 16 + len > i.size) throw fail('FRAME', 'frame ' + f + ' payload past EOF');
      if (b[off + 12] > 16) throw fail('FRAME', 'frame ' + f + ' brightness out of range');
      var q = off + 16, end = q + len;
      if (end - q < 6) throw fail('FRAME', 'frame ' + f + ' body too short');
      var np = rd16(b, q); q += 2;
      if (end - q < np * 3) throw fail('FRAME', 'frame ' + f + ' palette overflow');
      q += np * 3;
      if (end - q < 2) throw fail('FRAME', 'frame ' + f + ' tile count missing');
      var nt = rd16(b, q); q += 2;
      if (nt > MAX_TILES) throw fail('FRAME', 'frame ' + f + ' too many tiles');
      if (end - q < nt * (2 + tb)) throw fail('FRAME', 'frame ' + f + ' tile overflow');
      q += nt * (2 + tb);
      if (end - q < 2) throw fail('FRAME', 'frame ' + f + ' op count missing');
      var no = rd16(b, q); q += 2;
      if (end - q < no * 4) throw fail('FRAME', 'frame ' + f + ' map op overflow');
      q += no * 4;
      if (q !== end) throw fail('FRAME', 'frame ' + f + ' trailing bytes');
    }
  }

  function check(src) {
    var b = toBytes(src);
    try {
      var i = parse(b);
      if (i.crc && crc32(b.subarray(i.palOff)) !== i.crc) throw fail('CRC', 'checksum mismatch');
      walk(b, i);
      return null;
    } catch (e) { return e.message; }
  }

  /* ---------------------------------------------------------------- decoder */

  function Decoder(src) {
    var b = toBytes(src);
    var i = parse(b);
    if (i.crc && crc32(b.subarray(i.palOff)) !== i.crc) throw fail('CRC', 'checksum mismatch');
    walk(b, i);
    this.bytes = b;
    this.info = i;
    this.sheet = new Uint8Array(MAX_TILES * i.tileBytes);
    this.map = new Uint16Array(i.mw * i.mh);
    this.pal = new Uint16Array(256);
    for (var k = 0; k < i.palCount; k++) this.pal[k] = rd16(b, i.palOff + k * 2);
    this.scrollX = 0; this.scrollY = 0; this.brightness = 16; this.cur = 0;
    this._lut = new Uint32Array(256);
    this._lut565 = new Uint16Array(256);
    this._lutDirty = true;
    this.decode(0);
  }

  Decoder.prototype.decode = function (idx) {
    var b = this.bytes, i = this.info, tb = i.tileBytes;
    if (idx < 0 || idx >= i.frames) throw fail('FRAME', 'frame index out of range');
    var off = rd32(b, i.indexOff + idx * 4);
    var type = b[off], len = rd16(b, off + 2);
    this.scrollX = (rd16(b, off + 4) << 16) >> 16;
    this.scrollY = (rd16(b, off + 6) << 16) >> 16;
    this.brightness = b[off + 12];

    if (type === T_INTRA) { this.sheet.fill(0); this.map.fill(0); }

    var q = off + 16, end = q + len;
    var np = rd16(b, q); q += 2;
    if (end - q < np * 3) throw fail('FRAME', 'palette overflow');
    for (var k = 0; k < np; k++, q += 3) this.pal[b[q]] = rd16(b, q + 1);

    var nt = rd16(b, q); q += 2;
    if (end - q < nt * (2 + tb)) throw fail('FRAME', 'tile overflow');
    for (k = 0; k < nt; k++, q += 2 + tb) {
      var slot = rd16(b, q);
      if (slot < MAX_TILES) this.sheet.set(b.subarray(q + 2, q + 2 + tb), slot * tb);
    }

    var no = rd16(b, q); q += 2;
    if (end - q < no * 4) throw fail('FRAME', 'map overflow');
    var lw = i.mapW, lh = i.mapH;
    for (k = 0; k < no; k++, q += 4) {
      var cell = rd16(b, q), e = rd16(b, q + 2);
      if (cell >= lw * lh) throw fail('FRAME', 'map cell out of range');
      this.map[((cell / lw) | 0) * i.mw + (cell % lw)] = e;
    }
    this._pad();
    this.cur = idx;
    this._lutDirty = true;
    return this;
  };

  /* Padding repeats the source map so a wrapped scroll can read any cell. */
  Decoder.prototype._pad = function () {
    var i = this.info, lw = i.mapW, lh = i.mapH, mw = i.mw, mh = i.mh;
    if (mw === lw && mh === lh) return;
    for (var y = 0; y < mh; y++)
      for (var x = 0; x < mw; x++)
        if (x >= lw || y >= lh) this.map[y * mw + x] = this.map[(y % lh) * mw + (x % lw)];
  };

  Decoder.prototype.next = function () {
    if (this.cur + 1 >= this.info.frames) return false;
    this.decode(this.cur + 1);
    return true;
  };

  /* Restart from the nearest INTRA at or before `frame`, then decode forward. */
  Decoder.prototype.seek = function (frame) {
    var i = this.info;
    if (frame < 0 || frame >= i.frames) throw fail('FRAME', 'seek out of range');
    var start = frame;
    while (start > 0) {
      var off = rd32(this.bytes, i.indexOff + start * 4);
      if (this.bytes[off] === T_INTRA) break;
      start--;
    }
    this.sheet.fill(0);
    this.cur = 0;
    for (var f = start; f <= frame; f++) this.decode(f);
    return this;
  };

  Decoder.prototype._rebuildLut = function () {
    var pal = this.pal, lut = this._lut, br = this.brightness;
    var lut565 = this._lut565;
    for (var k = 0; k < 256; k++) {
      var v = scale565(pal[k], br);
      lut[k] = rgba(v);
      lut565[k] = v;
    }
    this._lutDirty = false;
  };

  /* Render the current frame into a Uint32 RGBA buffer of w*h pixels.
   * Scroll and flips are applied in coded-pixel space, like mvid_draw(). */
  Decoder.prototype.renderInto = function (out, w, h) {
    if (this._lutDirty) this._rebuildLut();
    var i = this.info, tb = i.tileBytes, lw = i.mapW, lh = i.mapH, mw = i.mw;
    var sheet = this.sheet, map = this.map, lut = this._lut, pal = this.pal;
    var sx0 = this.scrollX, sy0 = this.scrollY, bpp8 = i.bpp === 8;
    var o = 0;
    for (var y = 0; y < h; y++) {
      var sy = y + sy0, py0 = sy & 7, tyb = (sy >> 3) % lh;
      if (tyb < 0) tyb += lh;
      var row = tyb * mw;
      for (var x = 0; x < w; x++, o++) {
        var sx = x + sx0;
        var tx = (sx >> 3) % lw; if (tx < 0) tx += lw;
        var e = map[row + tx];
        var px = sx & 7, py = py0;
        if (e & 0x4000) px = 7 - px;
        if (e & 0x8000) py = 7 - py;
        var t = (e & 0x3FF) * tb;
        var idx;
        if (bpp8) idx = sheet[t + py * 8 + px];
        else { var byte = sheet[t + py * 4 + (px >> 1)]; idx = (px & 1) ? (byte >> 4) : (byte & 15); }
        out[o] = idx ? lut[bpp8 ? idx : ((((e >> 10) & 7) * 16 + idx) & 0xFF)] : BLACK;
      }
    }
    return out;
  };

  /* The same walk in RGB565: for hashing, exactness tests, or a consumer with
   * no canvas. Scroll, flips and brightness land where mvid_draw() puts them. */
  Decoder.prototype.render565 = function (out, w, h) {
    if (this._lutDirty) this._rebuildLut();
    var i = this.info, tb = i.tileBytes, lw = i.mapW, lh = i.mapH, mw = i.mw;
    var sheet = this.sheet, map = this.map, lut = this._lut565, bpp8 = i.bpp === 8;
    var sx0 = this.scrollX, sy0 = this.scrollY, o = 0;
    for (var y = 0; y < h; y++) {
      var sy = y + sy0, py0 = sy & 7, tyb = (sy >> 3) % lh;
      if (tyb < 0) tyb += lh;
      var row = tyb * mw;
      for (var x = 0; x < w; x++, o++) {
        var sx = x + sx0;
        var tx = (sx >> 3) % lw; if (tx < 0) tx += lw;
        var e = map[row + tx];
        var px = sx & 7, py = py0;
        if (e & 0x4000) px = 7 - px;
        if (e & 0x8000) py = 7 - py;
        var t = (e & 0x3FF) * tb, idx;
        if (bpp8) idx = sheet[t + py * 8 + px];
        else { var byte = sheet[t + py * 4 + (px >> 1)]; idx = (px & 1) ? (byte >> 4) : (byte & 15); }
        out[o] = idx ? lut[bpp8 ? idx : ((((e >> 10) & 7) * 16 + idx) & 0xFF)] : 0;
      }
    }
    return out;
  };

  /* Full display frame in RGB565: QFA clips are coded at half size and doubled
   * here. This is the buffer `mosaic video frames --raw` writes, so it is what
   * the exactness test compares against. */
  Decoder.prototype.renderDisplay = function (out, stride) {
    var i = this.info;
    if (!i.qfa) { this.render565(out, i.width, i.height); return out; }
    if (!this._small) this._small = new Uint16Array(i.width * i.height);
    var small = this._small;
    this.render565(small, i.width, i.height);
    var w = i.width, h = i.height, dw = stride || w * 2;
    for (var y = 0; y < h; y++) {
      var s0 = y * w, d0 = (y * 2) * dw, d1 = d0 + dw;
      for (var x = 0; x < w; x++) {
        var v = small[s0 + x];
        out[d0 + x * 2] = v; out[d0 + x * 2 + 1] = v;
        out[d1 + x * 2] = v; out[d1 + x * 2 + 1] = v;
      }
    }
    return out;
  };

  /* Convert the current frame to an ImageData-compatible RGBA byte array. */
  Decoder.prototype.toImageData = function (ctx) {
    var i = this.info;
    var img = ctx ? ctx.createImageData(i.width, i.height) : { width: i.width, height: i.height, data: null };
    if (!img.data) img.data = new Uint8ClampedArray(i.width * i.height * 4);
    this.renderInto(new Uint32Array(img.data.buffer), i.width, i.height);
    return img;
  };

  /* ----------------------------------------------------------------- player */

  function Player(canvas, src, opts) {
    opts = opts || {};
    this.canvas = canvas;
    this.dec = new Decoder(src);
    var i = this.dec.info;
    this.info = i;
    canvas.width = i.width;
    canvas.height = i.height;
    this.ctx = canvas.getContext('2d', { alpha: false });
    this.img = this.ctx.createImageData(i.width, i.height);
    this.u32 = new Uint32Array(this.img.data.buffer);
    this.loop = opts.loop === undefined ? i.loop : !!opts.loop;
    this.fps = opts.fps || i.fps;
    this.playing = false;
    this.frame = 0;
    this.onframe = null;
    this.onended = null;
    this._raf = 0; this._last = 0; this._acc = 0;
    this._tick = this._tick.bind(this);
    this.draw();
    if (opts.autoplay !== false) this.play();
  }

  Player.prototype.draw = function () {
    this.dec.renderInto(this.u32, this.info.width, this.info.height);
    this.ctx.putImageData(this.img, 0, 0);
    if (this.onframe) this.onframe(this.frame, this.dec);
  };

  Player.prototype._tick = function (ts) {
    if (!this.playing) return;
    var step = 1000 / this.fps;
    if (!this._last) this._last = ts;
    this._acc += ts - this._last;
    this._last = ts;
    if (this._acc > 250) this._acc = step; /* tab was hidden: don't fast-forward */
    var moved = false;
    while (this._acc >= step) {
      this._acc -= step;
      if (!this._advance()) return;
      moved = true;
    }
    if (moved) this.draw();
    this._raf = requestAnimationFrame(this._tick);
  };

  Player.prototype._advance = function () {
    if (this.dec.next()) { this.frame++; return true; }
    if (this.loop) { this.dec.seek(0); this.frame = 0; return true; }
    this.pause();
    if (this.onended) this.onended();
    return false;
  };

  Player.prototype.play = function () {
    if (this.playing) return this;
    if (this.frame + 1 >= this.info.frames && !this.loop) { this.seek(0); }
    this.playing = true;
    this._last = 0; this._acc = 0;
    this._raf = requestAnimationFrame(this._tick);
    return this;
  };

  Player.prototype.pause = function () {
    this.playing = false;
    if (this._raf) cancelAnimationFrame(this._raf);
    this._raf = 0;
    return this;
  };

  Player.prototype.seek = function (frame) {
    var n = ((frame % this.info.frames) + this.info.frames) % this.info.frames;
    this.dec.seek(n);
    this.frame = n;
    this._acc = 0;
    this.draw();
    return this;
  };

  Player.prototype.step = function (d) {
    this.pause();
    return this.seek(this.frame + (d || 1));
  };

  Player.prototype.destroy = function () { this.pause(); this.dec = null; };

  /* --------------------------------------------------------- web component */

  var TAG = 'mosaic-video';
  function defineElement() {
    if (typeof HTMLElement === 'undefined' || typeof customElements === 'undefined') return;
    if (customElements.get(TAG)) return;

    var Element = function () { return Reflect.construct(HTMLElement, [], Element); };
    Element.prototype = Object.create(HTMLElement.prototype);
    Element.prototype.constructor = Element;
    Object.setPrototypeOf(Element, HTMLElement);
    Element.observedAttributes = ['src', 'loop', 'autoplay'];

    Element.prototype.connectedCallback = function () {
      var self = this;
      if (!this._root) {
        this._root = this.attachShadow({ mode: 'open' });
        this._root.innerHTML =
          '<style>' +
          ':host{display:block;line-height:0}' +
          'canvas{display:block;width:100%;height:auto;image-rendering:pixelated;' +
          'background:#000;border-radius:6px}' +
          '.err{font:12px/1.4 ui-monospace,Menlo,monospace;color:#c66;' +
          'padding:12px;border:1px solid #522;border-radius:6px;background:#1a0e0e}' +
          '</style><canvas part="canvas"></canvas>';
        this._canvas = this._root.querySelector('canvas');
        this._canvas.addEventListener('click', function () {
          if (!self._player) return;
          if (self._player.playing) self._player.pause(); else self._player.play();
        });
      }
      if (this.getAttribute('src')) this.load();
    };

    Element.prototype.attributeChangedCallback = function (name, old, val) {
      if (name === 'src' && old !== val && this._canvas) this.load();
      else if (name === 'loop' && this._player) this._player.loop = val !== null;
    };

    Element.prototype.load = function () {
      var self = this, src = this.getAttribute('src');
      if (!src) return Promise.resolve(null);
      return fetch(src)
        .then(function (r) {
          if (!r.ok) throw new Error('HTTP ' + r.status + ' for ' + src);
          return r.arrayBuffer();
        })
        .then(function (buf) {
          if (self._player) self._player.destroy();
          self._player = new Player(self._canvas, new Uint8Array(buf), {
            loop: self.hasAttribute('loop'),
            autoplay: self.hasAttribute('autoplay')
          });
          self.dispatchEvent(new CustomEvent('mosaic-load', { detail: self._player.info }));
          return self._player;
        })
        .catch(function (e) {
          self._root.querySelector('canvas').style.display = 'none';
          var box = self._root.querySelector('.err') || document.createElement('div');
          box.className = 'err';
          box.textContent = 'mosaic-video: ' + e.message;
          self._root.appendChild(box);
          self.dispatchEvent(new CustomEvent('mosaic-error', { detail: e }));
          return null;
        });
    };

    customElements.define(TAG, Element);
  }

  defineElement();

  return {
    VERSION: 1,
    check: check,
    parse: function (src) { return parse(toBytes(src)); },
    Decoder: Decoder,
    Player: Player,
    play: function (canvas, src, opts) { return new Player(canvas, src, opts); },
    fromBase64: function (s) {
      if (typeof atob === 'function') {
        var bin = atob(s), a = new Uint8Array(bin.length);
        for (var i = 0; i < bin.length; i++) a[i] = bin.charCodeAt(i);
        return a;
      }
      return new Uint8Array(Buffer.from(s, 'base64'));
    }
  };
});
