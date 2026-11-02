#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""mbelib 1.x, mbelib-neo, mbevocoder and the reference decoder (AMBE-3000R / JMBE) on the same
frames, clean and damaged (5 % BER, 5 % random frames), 70 recordings; writes
corpus/baselines/decoder_lineage_all.json.

  python tools/quality/decoder_lineage_all.py

AMBE rates: the AMBE-3000R's frames; IMBE: our encoder's frames. Needs a run of
tools/quality/encoder_errors.py first (its damaged frames and the reference decodes in
build/quality/encoder_errors), tools/mbelib1-decode built in build/vs2026 (MBELIB1_SOURCE_DIR)
and a neo-compat build's mbe-cli in build/neocheck (MBEVOCODER_CHIP_MEASURED=OFF, MBEVOCODER_FLOAT_CLIP=ON).
"""
import json, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import numpy as np
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/quality'))
from compare import read_wav  # noqa
from perceptual import score  # noqa
from decoder_lineage import decode_mbelib1  # noqa
OURS = (ROOT / 'build/vs2026/tools/mbe-cli/Release/mbe-cli.exe').resolve()
NEO = (ROOT / 'build/neocheck/tools/mbe-cli/Release/mbe-cli.exe').resolve()
M1 = (ROOT / 'build/vs2026/tools/mbelib1-decode/Release/mbelib1-decode.exe').resolve()
E = ROOT / 'build/quality/encoder_errors'
W = ROOT / 'build/quality/decoder_lineage_all'; W.mkdir(parents=True, exist_ok=True)
jobs = []
for rate, enc, ref in (('ambe2450', 'chip', 'chip'), ('dstar', 'chip', 'chip'), ('imbe7200', 'ours', 'jmbe')):
    for f in sorted((E / rate).glob(f'*.{enc}.clean.frames')):
        n = f.name.split('.')[0]
        if n.startswith('syn_'):
            continue
        for cond in ('clean', 'ber5', 'lost5'):
            jobs.append((rate, n, cond, E / rate / f'{n}.{enc}.{cond}.frames', ref))

def one(j):
    rate, n, cond, fr, ref = j
    src = read_wav(ROOT / 'testdata/corpus' / f'{n}.wav')
    out = {}
    for dec in ('mbelib1', 'neo', 'ours'):
        w = W / f'{rate}.{n}.{cond}.{dec}.wav'
        if not w.exists():
            if dec == 'mbelib1':
                decode_mbelib1(M1, OURS, fr, rate, w)
            else:
                # the neo-compat build keeps mbelib's level, 16 dB above ours: mbe-cli's former default gain
                gain = ['--gain', '0.125'] if dec == 'neo' else []
                subprocess.run([str(OURS if dec == 'ours' else NEO), 'decode', '--rate', rate, *gain, str(fr), str(w)], check=True, capture_output=True)
        out[dec] = score(src, read_wav(w), None)
    rw = fr.with_suffix(f'.{ref}.wav')
    out[ref] = score(src, read_wav(rw), None)
    return j[:3], {k: (v['pesq'], v['estoi']) for k, v in out.items()}

with ThreadPoolExecutor(6) as ex:
    R = list(ex.map(one, jobs))
res = {}
for (rate, n, cond), s in R:
    res.setdefault(rate, {}).setdefault(cond, {})[n] = s
json.dump(res, open(ROOT / 'corpus/baselines/decoder_lineage_all.json', 'w'), indent=1)
for rate, c in res.items():
    for cond, files in c.items():
        decs = next(iter(files.values())).keys()
        print(rate, cond, len(files), '  '.join(f'{d} {np.mean([v[d][0] for v in files.values()]):.3f}/{np.mean([v[d][1] for v in files.values()]):.3f}' for d in decs))
