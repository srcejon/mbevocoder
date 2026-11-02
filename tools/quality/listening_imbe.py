#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Blind listening pairs for the IMBE voicing threshold (MBEVOCODER_IMBE_VOICING_THRESHOLD, x1.75
against the current encoder; docs/encoder-quality.md "Voicing threshold"): does voicing more bands
sound buzzy?

  python tools/quality/listening_imbe.py [--work build/imbe_vuv/fresh] [--out build/listening_imbe] [--seed 20261007]

Clips come from the fresh validation set at framing 0, chosen before any comparison by where the
change acts: in each recording, the 2.5 s window with the most harmonics whose voicing differs
between the two encoders' decoded models (mbe-cli parms); at most two clips per speaker, the
recordings ranked by that count. Each clip is aligned to the original, cut identically,
level-matched to the original's active level and faded 20 ms at both ends:
  ours  the two encoders' frames through our decoder
  jmbe  the same through JMBE (sdrtrunk's decoder, written from the standard)
A/B assignment is random; the key is <out>/key.json (do not look before rating).
"""
import argparse
import json
import random
import subprocess
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare import read_wav  # noqa: E402
from listening_fricatives import CLIP, active_rms, cut, fade, write  # noqa: E402

N_OURS, N_JMBE = 7, 3
DELAY = 189                       # the IMBE encoder's input delay in samples (mbe_encoderDelaySamples)
LABEL = {'ref': 'current IMBE encoder', 'vt175': 'IMBE encoder, voicing threshold x1.75'}


def voicing(cli, frames, parms):
    subprocess.run([str(cli), 'parms', '--rate', 'imbe7200', str(frames), str(parms)], check=True, capture_output=True)
    out = []
    for line in Path(parms).read_text().splitlines():
        v = line.split()
        out.append(v[3] if len(v) > 3 and int(v[2]) > 0 else '')
    return out


def best_window(a, b):
    """Frame-wise count of harmonics voiced differently; the 2.5 s window with the most."""
    n = min(len(a), len(b))
    diff = np.array([sum(x != y for x, y in zip(a[i], b[i])) + abs(len(a[i]) - len(b[i])) for i in range(n)])
    w = CLIP // 160
    if n <= w:
        return 0, int(diff.sum())
    c = np.convolve(diff, np.ones(w, int), 'valid')
    k = int(np.argmax(c))
    return max(0, k * 160 - DELAY), int(c[k])


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--work', type=Path, default=ROOT / 'build/imbe_vuv/fresh')
    ap.add_argument('--fresh', type=Path, default=ROOT / 'build/step5/fresh')
    ap.add_argument('--out', type=Path, default=ROOT / 'build/listening_imbe')
    ap.add_argument('--cli', type=Path, default=ROOT / 'build/vs2026/tools/mbe-cli/Release/mbe-cli.exe')
    ap.add_argument('--seed', type=int, default=20261007)
    a = ap.parse_args()
    audio = a.out / 'audio'
    audio.mkdir(parents=True, exist_ok=True)
    index = json.loads((a.fresh / 'index.json').read_text())['files']
    speaker = {f['name']: f['speaker'] for f in index}
    ranked = []
    for f in index:
        name = f['name']
        va = voicing(a.cli, a.work / f'{name}__f0.ref.frames', a.out / f'{name}.ref.parms')
        vb = voicing(a.cli, a.work / f'{name}__f0.vt175.frames', a.out / f'{name}.vt175.parms')
        start, count = best_window(va, vb)
        ranked.append((count, name, start))
    ranked.sort(reverse=True)
    chosen, seen = [], {}
    for count, name, start in ranked:
        if seen.get(speaker[name], 0) >= 2:
            continue
        seen[speaker[name]] = seen.get(speaker[name], 0) + 1
        chosen.append((name, start, count))
        if len(chosen) == N_OURS + N_JMBE:
            break
    rng = random.Random(a.seed)
    rng.shuffle(chosen)
    key = []
    for i, (name, start, count) in enumerate(chosen, 1):
        dec = 'ours' if i <= N_OURS else 'jmbe'
        src = read_wav(a.fresh / f'{name}.wav')
        x, y = read_wav(a.work / f'{name}__f0.vt175.{dec}.wav'), read_wav(a.work / f'{name}__f0.ref.{dec}.wav')
        decoder = 'mbevocoder decoder' if dec == 'ours' else 'JMBE decoder'
        xl, yl = f"{LABEL['vt175']}, {decoder}", f"{LABEL['ref']}, {decoder}"
        s = src[start:start + CLIP]
        s = np.pad(s, (0, CLIP - len(s)))
        cx, cy = cut(src, x, start), cut(src, y, start)
        cx *= active_rms(s) / active_rms(cx)
        cy *= active_rms(s) / active_rms(cy)
        swap = rng.random() < 0.5
        write(audio / f'p{i}-source.wav', fade(s))
        write(audio / f'p{i}-A.wav', fade(cy if swap else cx))
        write(audio / f'p{i}-B.wav', fade(cx if swap else cy))
        key.append({'pair': i, 'decoder': dec, 'recording': name, 'speaker': speaker[name], 'start_s': start / 8000,
                    'voicing_differences': count, 'A': yl if swap else xl, 'B': xl if swap else yl})
        print(f'p{i}: {dec:4s} {name} at {start / 8000:.2f} s ({count} harmonics voiced differently)')
    (a.out / 'key.json').write_text(json.dumps({'fixed': '2026-10-07, by voicing difference before any listening', 'seed': a.seed,
                                                'pairs': key}, indent=1))
    print(f'{len(key)} pairs in {audio}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
