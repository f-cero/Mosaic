#!/usr/bin/env node
/* SPDX-License-Identifier: Apache-2.0
 * Proves web/mvid.js decodes bit-identically to codec/mvid_decode.c: the C CLI
 * dumps every frame as raw RGB565, the JS decoder renders the same frames, and
 * every pixel must match. No tolerance — a delta is a bug, not a rounding.
 */
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const MVID = require('../web/mvid.js');

const args = process.argv.slice(2);
const arg = (n) => { const i = args.indexOf(n); return i >= 0 ? args[i + 1] : undefined; };
const mosaic = arg('--mosaic');
const clip = arg('--clip');
if (!clip) { console.error('usage: mvid_web_test.mjs [--mosaic <mosaic>] --clip <clip.mvid>'); process.exit(2); }

const bytes = new Uint8Array(fs.readFileSync(clip));
const bad = MVID.check(bytes);
if (bad) { console.error(`${path.basename(clip)}: INVALID — ${bad}`); process.exit(1); }
const dec = new MVID.Decoder(bytes);
const info = dec.info;
const ow = info.qfa ? info.width * 2 : info.width;
const oh = info.qfa ? info.height * 2 : info.height;

let dir = null;
if (mosaic) {
  dir = fs.mkdtempSync(path.join(os.tmpdir(), 'mvid-web-'));
  const r = spawnSync(mosaic, ['video', 'frames', clip, '--out', dir, '--raw'], { encoding: 'utf8' });
  if (r.status !== 0) {
    console.error('mosaic video frames failed:', (r.stderr || r.stdout || String(r.status)).trim());
    process.exit(1);
  }
}

const js = new Uint16Array(ow * oh);
let frames = 0, diffPixels = 0, worst = 0, firstBad = -1;
for (;;) {
  dec.renderDisplay(js, ow);
  if (dir) {
    const f = path.join(dir, `frame${String(frames).padStart(3, '0')}.r16`);
    const buf = fs.readFileSync(f);
    if (buf.length !== ow * oh * 2) {
      console.error(`frame ${frames}: C dump ${buf.length} B, expected ${ow * oh * 2}`);
      process.exit(1);
    }
    const dv = new DataView(buf.buffer, buf.byteOffset, buf.length);
    for (let p = 0; p < js.length; p++) {
      const c = dv.getUint16(p * 2, true);
      if (c !== js[p]) {
        diffPixels++;
        if (firstBad < 0) firstBad = frames;
        const d = Math.abs(((c >> 11) & 31) - ((js[p] >> 11) & 31))
                + Math.abs(((c >> 5) & 63) - ((js[p] >> 5) & 63))
                + Math.abs((c & 31) - (js[p] & 31));
        if (d > worst) worst = d;
      }
    }
  }
  frames++;
  if (!dec.next()) break;
}

const kb = (bytes.length / 1024).toFixed(1);
const shape = `${frames} frames ${ow}x${oh}, ${info.bpp}bpp${info.qfa ? ', QFA' : ''}, ${kb} KB`;
if (dir) fs.rmSync(dir, { recursive: true, force: true });
if (!dir) {
  console.log(`${path.basename(clip)}: decoded by web/mvid.js — ${shape}`);
  process.exit(0);
}
if (diffPixels) {
  console.error(`MISMATCH on ${path.basename(clip)}: ${diffPixels} pixels differ, first in frame ${firstBad}, worst channel delta ${worst}`);
  process.exit(1);
}
console.log(`${path.basename(clip)}: ${shape} — JS decoder == C decoder, all pixels identical (RGB565)`);
