# 11 — Audio ingest: recorded sound into midicore

midicore has no PCM. Sixteen wavetable voices behind an SMF sequencer, a
GM-lite instrument set and 48 noise drums: that is the whole audio budget. So
"play this recording" can only mean **transcribe it into notes**. `mosaic audio`
does that, in two modes, and the notes it writes are the same SMF the engine
already plays from a cart.

```sh
mosaic audio ingest take.wav -o take.mid --mode music     # polyphonic
mosaic audio ingest vox.wav  -o vox.mid  --mode speech    # monophonic + consonants
mosaic audio render take.mid -o out.wav                   # hear it through midicore
mosaic audio compare take.wav out.wav                     # score the round trip
mosaic audio dump   take.mid                              # note list
mosaic audio info   take.wav                              # rate, pitch, voicing
```

Input must be a WAV (PCM 8/16/24/32-bit or 32-bit float, any rate, mono or
stereo). Everything is analysed at 22 050 Hz mono; the file is trimmed of leading
and trailing silence first, so dead air does not become notes.

## 11.1 What the output is

Format 0 SMF, one track, 120 BPM, 100 ticks per quarter note — which is **5 ms
per tick**, so the whole clip quantises to a 5 ms grid. Chords share channel 0
and 1 (register split at middle C), drums use channel 9. Every note-on carries a
velocity derived from its own salience, so the quiet parts stay quiet.

Because the grid is 5 ms and `ms_player` computes `tick_us = tempo / tpqn`, the
MIDI you get back is exactly the timing the console plays.

## 11.2 Music mode

1. **STFT** — 2048-point Hann window, 256-sample hop (93 ms window, 10.8 Hz
   bins, 11.6 ms frames) at 22 050 Hz.
2. **Harmonic-sum salience** — for each MIDI note 36..96, sum the magnitude at
   *f*, 2*f*, … 8*f* weighted by 1/h.
3. **Iterative harmonic cancellation** — take the strongest fundamental, then
   multiply its harmonic bins by 0.1 so the fifth or octave *ghost* of a note is
   not picked as a second note. Up to six picks per frame.
4. **Note tracking** — a note starts after two consecutive frames above
   `0.16 × p95(salience)` and ends after three frames below it. Onsets and
   releases therefore cannot chatter on a single noisy frame.
5. **Register split** — notes below middle C go to channel 0, the rest to
   channel 1. Each channel's program is chosen by the spectral centroid of the
   frames it played: <500 Hz → 33 (pulse bass), <1.2 kHz → 24 (triangle),
   <2.4 kHz → 16 (saw), else 8 (square).
6. **Drums** (on by default, `--no-drums` to disable) — spectral-flux onsets,
   at least 40 ms apart, classified by band energy into kick (36), snare (38)
   or closed hat (42), played on channel 9.

## 11.3 Speech mode

1. **YIN** — difference function over 55..420 Hz, cumulative-mean-normalised,
   absolute threshold 0.20 with parabolic interpolation, 10 ms hop.
2. **Voicing** — aperiodicity < 0.25, frame RMS above the greater of 2.5× the
   10th-percentile noise floor and −48 dBFS.
3. **Contour → notes** — F0 is quantised to the nearest semitone (24..100); a
   voiced run of at least three frames becomes one note, with a one-frame blip
   absorbed into the running note so vibrato does not split it.
4. **Consonants** — aperiodic bursts that are neither silence nor vowel become
   noise-drum hits on channel 9, chosen by spectral centroid: <900 Hz kick,
   <2.6 kHz snare, <5.2 kHz closed hat, else open hat.
5. **Program** — one melodic channel, default 33 (pulse, the most voice-like
   wavetable family), overridable with `--program`.

Speech mode transfers *prosody* — pitch contour, rhythm, stress — not words. With
no formant synthesis and no PCM there is nothing to carry phonemes: the result is
a sing-song robot that follows the speaker's melody. That is the format's
ceiling, not a tuning issue.

## 11.4 Measured, on this tree

Two synthetic signals with known ground truth (`tools/audio_test.py`, wired into
ctest as `mosaic_audio_test`): an eight-note C-major scale played with six
harmonics, and three "syllables" of a 120→150, 170→165, 150→110 Hz contour with
50 ms noise consonants.

| | music | speech |
|---|---|---|
| analysis frames | 231 | 135 |
| notes emitted | 13 | 4 |
| ground truth recovered | **7/7 pitch classes** | 4 voiced notes |
| round-trip F0 correlation | 0.667 | 0.697 |
| mean pitch error | **0.25 semitone** | 1.74 semitone |
| voicing F1 | — | 0.667 |
| RMS-envelope correlation | 0.088 | 0.650 |

Read honestly:

- **Pitch content survives** in both modes: every pitch class of the scale, and
  the speech contour to within about a quarter tone in music mode.
- **Music mode emits five spurious notes** — semitone neighbours of real notes
  (61, 63, 66, 68 alongside 60, 62, 64, 65). They come from leakage between
  adjacent bins at 10.8 Hz resolution, and they are the main thing to fix.
- **Music envelope correlation is poor (0.088)** because the synth sustains
  notes while the source articulates 320 ms ones. Envelope fidelity is not what
  this path delivers; pitch and rhythm are.
- **Speech consonants did not fire** on the synthetic fixture: the 50 ms noise
  bursts sit below the consonant gate relative to the vowels. Real speech has a
  better voiced/unvoiced ratio; the classification is implemented but unproven
  on real audio.

## 11.5 Two engine bugs found on the way

Both were found by testing the round trip, and both affected every cart, not
just this tool.

1. **Every note played 1.6818× sharp.** `note_inc()` multiplied a C-relative Q16
   ratio table by 440 and skipped the 2²⁴ phase scale the wave table needs, so
   A4 came out at 740 Hz. Fixed with integer-only maths; measured A4 is now
   438.7 Hz by zero-crossing (±0.3 %).
2. **The sequencer stalled in block mode.** `ms_synth_render()` compared
   `next_us` against the *block-local* frame number, so once the first block had
   been rendered the player stopped advancing in real time. It now keeps a
   monotonic `audio_pos` sample clock in `ms_player_t`.

## 11.6 Limits

- **No audio quality beyond the synth.** The output is wavetable voices: it can
  sound like a melody played on a game console, never like the recording.
- **Polyphony is capped at six notes per frame** and the synth has 12 music
  voices; dense mixes lose inner voices.
- **No pitch bend, no portamento, no velocity layers** — `ms_synth_cc` handles
  CC 7 and 120/123 only, so speech glides become semitone steps.
- **No tempo changes** in the transcription; one tempo, 120 BPM.
- **Analysis needs a WAV.** MP3/OGG/M4A must be decoded externally first
  (`ffmpeg -i in.mp3 -ac 1 -ar 22050 out.wav`), which keeps the tool free of
  libav licences.

## 11.7 Next

1. **Bin leakage** — switch to a 4096-point window for the low register, or
   penalise a candidate whose fundamental bin is weaker than its neighbours, to
   kill the semitone ghosts.
2. **Percussive/harmonic separation** (median filtering across frames) before
   salience, so drums stop voting on pitch.
3. **A purpose-built `.sbn` voice bank** for speech mode: a pulse wave with
   mild formant-ish filtering beats a GM default.
4. **Real-voice evaluation**: the current fixtures are synthetic; the next step
   is a small corpus of read speech scored on F0 correlation and voicing.
