#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Blind listening pairs for the fresh-set decision in docs/encoder-quality.md, from the fresh set's scoring runs.

  python tools/quality/listening_step5.py [--work build/step5] [--seed 20260924]

The pairs were fixed on 24 September 2026 before any score on the fresh set existed (a rule
on the files' descriptions: noisy, accented and clean recordings of both sexes, framing 0).
Audio comes from tools/quality/rate_ladder.py's work directories (<work>/dstar, <work>/ambe2450,
<work>/imbe7200). Each pair is written as pNN-source.wav, pNN-A.wav, pNN-B.wav in
<work>/listening/audio (pairs whose audio does not exist yet are skipped), A and B level-matched to the source (active-speech RMS) and assigned at
random; the key is <work>/listening/key.json (do not look before rating).
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
from compare import read_wav  # noqa: E402

# (rate, decoder, candidate row, other row, recording)
PAIRS = [
    ('dstar', 'chip', 'candidate', 'default', 'other_1998-15444-0006'),
    ('dstar', 'chip', 'candidate', 'default', 'other_2609-156975-0008'),
    ('dstar', 'chip', 'candidate', 'default', 'arctic_awb_arctic_b0002'),
    ('dstar', 'chip', 'candidate', 'default', 'arctic_ksp_arctic_b0002'),
    ('dstar', 'chip', 'candidate', 'default', 'other_3080-5032-0009'),
    ('dstar', 'chip', 'candidate', 'chip', 'other_3528-168656-0002'),
    ('dstar', 'chip', 'candidate', 'chip', 'other_1688-142285-0000'),
    ('dstar', 'chip', 'candidate', 'chip', 'arctic_clb_arctic_b0002'),
    ('ambe2450', 'chip', 'candidate', 'default', 'other_3331-159605-0000'),
    ('imbe7200', 'ours', 'candidate', 'default', 'other_3005-163389-0006'),
]


def active_rms(x):
    n = len(x) // 160
    f = np.sqrt(np.mean(x[: n * 160].reshape(n, 160) ** 2, axis=1))
    return float(np.sqrt(np.mean(f[f > f.max() * 0.01] ** 2))) + 1e-9


def write(p, x):
    wavfile.write(p, 8000, np.clip(np.round(x * 32767), -32768, 32767).astype(np.int16))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--work', type=Path, default=ROOT / 'build/step5')
    ap.add_argument('--seed', type=int, default=20260924)
    args = ap.parse_args()
    out = args.work / 'listening' / 'audio'
    out.mkdir(parents=True, exist_ok=True)
    missing = []
    rng = random.Random(args.seed)
    key = []
    for i, (rate, dec, cand, other, rec) in enumerate(PAIRS, 1):
        pa, pb = (args.work / rate / f'{rec}__f0.{row}.{dec}.wav' for row in (cand, other))
        swap = rng.random() < 0.5   # drawn for every pair, so a partial build keeps the same key
        if not (pa.exists() and pb.exists()):
            missing.append(i)
            continue
        src = read_wav(args.work / 'fresh' / f'{rec}.wav')
        a, b = read_wav(pa), read_wav(pb)
        a, b = (x * (active_rms(src) / active_rms(x)) for x in (a, b))
        write(out / f'p{i}-source.wav', src)
        write(out / f'p{i}-A.wav', b if swap else a)
        write(out / f'p{i}-B.wav', a if swap else b)
        key.append({'pair': i, 'rate': rate, 'decoder': dec, 'recording': rec,
                    'A': other if swap else cand, 'B': cand if swap else other})
    (out.parent / 'key.json').write_text(json.dumps({'fixed': '2026-09-24, before scoring', 'seed': args.seed, 'pairs': key}, indent=1))
    print(f'{len(key)} pairs in {out}' + (f'; not ready yet: {missing}' if missing else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
