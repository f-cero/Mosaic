#!/usr/bin/env python3
"""Mosaic audio ingest test — stdlib only, no fixtures checked in.

Builds two synthetic signals with known ground truth (a scale with harmonics,
and a formant-free voiced/unvoiced 'speech' contour), transcribes each into an
SMF, renders the SMF back through midicore, and scores the round trip.
"""
import argparse, math, os, random, struct, subprocess, sys, tempfile, wave

RATE = 22050


def write_wav(path, samples):
    with wave.open(path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b''.join(struct.pack('<h', max(-32768, min(32767, int(s * 32767)))) for s in samples))


def env(t, dur, a=0.012, r=0.04):
    if t < a:
        return t / a
    if t > dur - r:
        return max(0.0, (dur - t) / r)
    return 1.0


def tone(f0, dur, partials=6, amp=0.5):
    n = int(dur * RATE)
    out = []
    for i in range(n):
        t = i / RATE
        s = 0.0
        for h in range(1, partials + 1):
            if f0 * h > 9000:
                break
            s += math.sin(2 * math.pi * f0 * h * t) / h
        out.append(amp * env(t, dur) * s)
    return out


def melody(path):
    notes = [60, 62, 64, 65, 67, 69, 71, 72]
    x = []
    for m in notes:
        f = 440.0 * 2 ** ((m - 69) / 12.0)
        x += tone(f, 0.32)
        x += [0.0] * int(0.03 * RATE)
    write_wav(path, x)
    return notes


def speech(path):
    """Three syllables: /a/ glide, /i/ steady, /a/ fall, with noise consonants."""
    random.seed(7)
    x = []
    for i in range(60):                       # leading silence
        x.append(0.0)
    plan = [(120, 150, 0.34), (170, 165, 0.30), (150, 110, 0.36)]
    for f0a, f0b, dur in plan:
        for i in range(int(0.05 * RATE)):     # consonant: band-limited noise
            x.append(0.22 * random.uniform(-1, 1) * (1 - i / (0.05 * RATE)))
        n = int(dur * RATE)
        for i in range(n):
            t = i / RATE
            f0 = f0a + (f0b - f0a) * (i / n) + 2.0 * math.sin(2 * math.pi * 5.5 * t)
            s = 0.0
            for h in range(1, 9):
                s += math.sin(2 * math.pi * f0 * h * t) / h
            x.append(0.42 * env(t, dur, 0.02, 0.06) * s)
        for i in range(int(0.12 * RATE)):
            x.append(0.0)
    write_wav(path, x)
    return [(f0a, f0b) for f0a, f0b, _ in plan]


def run(mosaic, *args):
    r = subprocess.run([mosaic] + list(args), capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout + r.stderr)
        raise SystemExit('failed: mosaic %s' % ' '.join(args))
    return r.stdout


def metrics(text):
    out = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) == 2:
            try:
                out[parts[0]] = float(parts[1])
            except ValueError:
                pass
    return out


def notes_of(text):
    ons = []
    for line in text.splitlines():
        p = line.split()
        if len(p) == 5 and p[0] == 'on':
            ons.append((int(p[1]), int(p[2])))
    return ons


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mosaic', required=True)
    ap.add_argument('--keep', action='store_true')
    a = ap.parse_args()
    mosaic = os.path.abspath(a.mosaic)
    tmp = tempfile.mkdtemp(prefix='mosaic-audio-')
    ok = True

    def check(name, cond, detail):
        nonlocal ok
        print(('  ok   ' if cond else '  FAIL ') + name + ': ' + detail)
        if not cond:
            ok = False

    # ---- music: a scale with harmonics, exact notes known
    mel = os.path.join(tmp, 'melody.wav')
    want = melody(mel)
    mid = os.path.join(tmp, 'melody.mid')
    print(run(mosaic, 'audio', 'ingest', mel, '-o', mid, '--mode', 'music').strip())
    ons = notes_of(run(mosaic, 'audio', 'dump', mid))
    got = sorted({n for ch, n in ons if ch in (0, 1)})
    print('  notes in: %s' % want)
    print('  notes out: %s' % got)
    hit = len(set(want) & set(got))
    pcs_want = {n % 12 for n in want}
    pcs_got = {n % 12 for n in got}
    pchit = len(pcs_want & pcs_got)
    check('music pitch classes', pchit >= 7, '%d/%d pitch classes recovered' % (pchit, len(pcs_want)))

    rw = os.path.join(tmp, 'melody_render.wav')
    run(mosaic, 'audio', 'render', mid, '-o', rw, '--rate', str(RATE))
    sc = metrics(run(mosaic, 'audio', 'compare', mel, mel))
    check('self-compare is exact', sc['f0_corr'] > 0.999, 'f0_corr %.3f' % sc['f0_corr'])
    m = metrics(run(mosaic, 'audio', 'compare', mel, rw))
    print('  compare: f0 %.3f mae %.2f st env %.3f' %
          (m['f0_corr'], m['f0_mae_semitone'], m['env_corr']))
    check('music f0 contour', m['f0_corr'] > 0.55, 'f0_corr %.3f > 0.55' % m['f0_corr'])
    check('music tuning', m['f0_mae_semitone'] < 1.0, 'mean error %.2f st < 1.0' % m['f0_mae_semitone'])

    # ---- speech: F0 glides recovered as a note contour
    sp = os.path.join(tmp, 'speech.wav')
    plan = speech(sp)
    smid = os.path.join(tmp, 'speech.mid')
    print(run(mosaic, 'audio', 'ingest', sp, '-o', smid, '--mode', 'speech').strip())
    sons = notes_of(run(mosaic, 'audio', 'dump', smid))
    mel_notes = [n for ch, n in sons if ch == 0]
    print('  speech notes: %s' % mel_notes)
    check('speech voiced notes', len(mel_notes) >= 4, '%d melodic notes' % len(mel_notes))
    srw = os.path.join(tmp, 'speech_render.wav')
    run(mosaic, 'audio', 'render', smid, '-o', srw, '--rate', str(RATE))
    sm = metrics(run(mosaic, 'audio', 'compare', sp, srw))
    print('  compare: f0 %.3f mae %.2f st vad_f1 %.3f env %.3f' %
          (sm['f0_corr'], sm['f0_mae_semitone'], sm['vad_f1'], sm['env_corr']))
    check('speech f0 contour', sm['f0_corr'] > 0.60, 'f0_corr %.3f > 0.60' % sm['f0_corr'])
    check('speech vad', sm['vad_f1'] > 0.55, 'vad_f1 %.3f > 0.55' % sm['vad_f1'])

    if not a.keep:
        for f in os.listdir(tmp):
            os.unlink(os.path.join(tmp, f))
        os.rmdir(tmp)
    else:
        print('fixtures kept in ' + tmp)
    print('audio test: ' + ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
