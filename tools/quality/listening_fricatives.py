#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Blind listening pairs for AMBE+2 fricatives (docs/encoder-quality.md "Open": the last AMBE+2
ESTOI gap sits in unvoiced segments).

  python tools/quality/listening_fricatives.py [--work build/step8] [--out build/listening_fricatives] [--seed 20260925]

Clips come from the fresh validation set at framing 0 (tools/corpus/prepare_validation4.py),
chosen by content before any comparison: in each recording, the 2.5 s window with the most
fricative frames in the original (20 ms frames within 35 dB of the loudest with more than 40 %
of their energy above 2 kHz); at most two clips per speaker, the recordings ranked by
that count. Two kinds of pair, each clip aligned to the original, cut identically, level-matched to
the original's active level and faded 20 ms at both ends:
  encoder  our AMBE+2 encoder vs the AMBE-3000R's encoder, both through the AMBE-3000R's decoder
  decoder  the AMBE-3000R's frames through our decoder vs through its own decoder
A/B assignment is random; the key is <out>/key.json (do not look before rating).
"""
import argparse
import json
import random
import sys
from pathlib import Path

import numpy as np
from scipy.io import wavfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare import read_wav, align  # noqa: E402

CLIP = 20000          # 2.5 s
FADE = 160
N_ENCODER, N_DECODER = 6, 4


def fricative_frames(x):
    n = len(x) // 160
    sp = np.abs(np.fft.rfft(x[:n * 160].reshape(n, 160) * np.hanning(160), 256, axis=1)) ** 2
    f = np.arange(sp.shape[1]) * 8000 / 256
    tot = sp.sum(axis=1) + 1e-12
    lvl = 10 * np.log10(tot)
    return (sp[:, f >= 2000].sum(axis=1) / tot > 0.4) & (lvl > lvl.max() - 35)


def best_window(x):
    fr = fricative_frames(x).astype(int)
    w = CLIP // 160
    if len(fr) <= w:
        return 0, int(fr.sum())
    c = np.convolve(fr, np.ones(w, int), 'valid')
    k = int(np.argmax(c))
    return k * 160, int(c[k])


def active_rms(x):
    n = len(x) // 160
    f = np.sqrt(np.mean(x[:n * 160].reshape(n, 160) ** 2, axis=1))
    return float(np.sqrt(np.mean(f[f > f.max() * 0.03] ** 2))) + 1e-9


def cut(src, x, start):
    """x aligned to src, the window [start, start + CLIP) of src's timeline."""
    xa, sa, lag = align(x, src)
    y = x[lag + start:lag + start + CLIP] if lag >= 0 else np.concatenate([np.zeros(-lag), x])[start:start + CLIP]
    y = np.pad(y, (0, CLIP - len(y)))
    return y


def fade(y):
    r = np.linspace(0, 1, FADE)
    y = y.copy(); y[:FADE] *= r; y[-FADE:] *= r[::-1]
    return y


def write(p, x):
    wavfile.write(p, 8000, np.clip(np.round(x * 32767), -32768, 32767).astype(np.int16))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--work', type=Path, default=ROOT / 'build/step8')
    ap.add_argument('--fresh', type=Path, default=ROOT / 'build/step5/fresh')
    ap.add_argument('--out', type=Path, default=ROOT / 'build/listening_fricatives')
    ap.add_argument('--seed', type=int, default=20260925)
    a = ap.parse_args()
    audio = a.out / 'audio'
    audio.mkdir(parents=True, exist_ok=True)
    index = json.loads((a.fresh / 'index.json').read_text())['files']
    speaker = {f['name']: f['speaker'] for f in index}
    ranked = []
    for f in index:
        src = read_wav(a.fresh / f"{f['name']}.wav")
        start, count = best_window(src)
        ranked.append((count, f['name'], start))
    ranked.sort(reverse=True)
    chosen, seen = [], {}
    for count, name, start in ranked:
        if seen.get(speaker[name], 0) >= 2:
            continue
        seen[speaker[name]] = seen.get(speaker[name], 0) + 1; chosen.append((name, start, count))
        if len(chosen) == N_ENCODER + N_DECODER:
            break
    rng = random.Random(a.seed)
    rng.shuffle(chosen)
    key = []
    for i, (name, start, count) in enumerate(chosen, 1):
        kind = 'encoder' if i <= N_ENCODER else 'decoder'
        src = read_wav(a.fresh / f'{name}.wav')
        d = a.work / 'ambe2450'
        # the AMBE-3000R's own chain is unaffected by the bit-order fix; its decodes may only exist
        # in the step 5 run
        cc = d / f'{name}__f0.chip.chip.wav'
        if not cc.exists():
            cc = ROOT / 'build/step5/ambe2450' / f'{name}__f0.chip.chip.wav'
        if kind == 'encoder':
            x, y = read_wav(d / f'{name}__f0.candidate.chip.wav'), read_wav(cc)
            xl, yl = 'mbevocoder encoder, AMBE-3000R decoder', 'AMBE-3000R encoder, AMBE-3000R decoder'
        else:
            x, y = read_wav(d / f'{name}__f0.chip.ours.wav'), read_wav(cc)
            xl, yl = 'AMBE-3000R encoder, mbevocoder decoder', 'AMBE-3000R encoder, AMBE-3000R decoder'
        s = src[start:start + CLIP]; s = np.pad(s, (0, CLIP - len(s)))
        cx, cy = cut(src, x, start), cut(src, y, start)
        cx *= active_rms(s) / active_rms(cx); cy *= active_rms(s) / active_rms(cy)
        swap = rng.random() < 0.5
        write(audio / f'p{i}-source.wav', fade(s))
        write(audio / f'p{i}-A.wav', fade(cy if swap else cx))
        write(audio / f'p{i}-B.wav', fade(cx if swap else cy))
        key.append({'pair': i, 'kind': kind, 'recording': name, 'speaker': speaker[name], 'start_s': start / 8000,
                    'fricative_frames': count, 'A': yl if swap else xl, 'B': xl if swap else yl})
        print(f'p{i}: {kind:7s} {name} at {start / 8000:.2f} s ({count} fricative frames)')
    (a.out / 'key.json').write_text(json.dumps({'fixed': '2026-09-25, by fricative count before any comparison', 'seed': a.seed, 'pairs': key}, indent=1))
    print(f'{len(key)} pairs in {audio}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
