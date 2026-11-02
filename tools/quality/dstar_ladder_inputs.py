#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prepare the D-STAR reference-ladder inputs (docs/encoder-quality.md).

  python tools/quality/dstar_ladder_inputs.py [--framings 0 40 80 120] [--out build/dstar-ladder]

Every recording of the 70-file development corpus and the four new-speaker sets is written at
each framing F (the input delayed by F samples, zeros in front) as `<name>__fF.wav` into
`<out>/aligned`, and delayed by F + 50 samples into `<out>/legacy`. The library encoders read
`aligned`. The offline candidate pipelines of 22-23 September (build/dstar-analysis,
build/dstar-perception) hard-code the 48-sample framing the D-STAR encoder had before
2026-09-24; fed `legacy` through the executables built then, they see the same frames as the
current 98-sample framing (the encoder's alignment is a delay line in front of the analysis),
which `--check` verifies byte for byte. A manifest per directory lists the names and the source
hashes.
"""
import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

import numpy as np
from scipy.io import wavfile

ROOT = Path(__file__).resolve().parents[2]
LEGACY_EXTRA = 50   # samples: 98 (current D-STAR framing) - 48 (the pipelines' framing)
SETS = {
    'dev': (ROOT / 'testdata/corpus', None),
    'new1': (ROOT / 'build/dstar-pitch/heldout', ROOT / 'build/dstar-pitch/heldout/index.json'),
    'new2': (ROOT / 'build/dstar-ideas/heldout', ROOT / 'build/dstar-ideas/heldout/index.json'),
    'new3': (ROOT / 'build/dstar-analysis/heldout', ROOT / 'build/dstar-analysis/heldout/index.json'),
    'new4': (ROOT / 'build/dstar-perception/heldout', ROOT / 'build/dstar-perception/heldout/index.json'),
}


def sources():
    out = []
    for tag, (d, manifest) in SETS.items():
        if manifest is None:
            names = [p.stem for p in sorted((ROOT / 'corpus/golden/dstar').glob('*.dvsi')) if not p.stem.startswith('syn_')]
        else:
            names = [f['name'] for f in json.loads(manifest.read_text())['files']]
        out += [(tag, n, d / f'{n}.wav') for n in names]
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--framings', type=int, nargs='+', default=[0, 40, 80, 120])
    ap.add_argument('--out', type=Path, default=ROOT / 'build/dstar-ladder')
    ap.add_argument('--check', nargs=2, metavar=('NEW_WEIGHTED', 'OLD_WEIGHTED'),
                    help='mbe-cli builds with and without the 98-sample framing: encode `aligned` with the first and `legacy` with the second and compare the frames')
    args = ap.parse_args()
    rows = sources()
    for kind, extra in (('aligned', 0), ('legacy', LEGACY_EXTRA)):
        d = args.out / kind
        d.mkdir(parents=True, exist_ok=True)
        files = []
        for tag, n, src in rows:
            fs, x = wavfile.read(src)
            assert fs == 8000 and x.dtype == np.int16, src
            for f in args.framings:
                name = f'{n}__f{f}'
                wavfile.write(d / f'{name}.wav', fs, np.concatenate([np.zeros(f + extra, np.int16), x]))
                files.append({'name': name, 'set': tag, 'source': n, 'framing': f, 'source_sha256': hashlib.sha256(src.read_bytes()).hexdigest()})
        (d / 'index.json').write_text(json.dumps({'files': files}, indent=1))
        print(f'{kind}: {len(files)} files ({len(rows)} recordings x {len(args.framings)} framings)')
    if args.check:
        new, old = (Path(p).resolve() for p in args.check)
        bad = 0
        for tag, n, _ in rows:
            for f in args.framings:
                a = args.out / 'check_new.dvsi'; b = args.out / 'check_old.dvsi'
                subprocess.run([new, 'encode', '--rate', 'dstar', '--no-tone-detect', args.out / 'aligned' / f'{n}__f{f}.wav', a], check=True, capture_output=True)
                subprocess.run([old, 'encode', '--rate', 'dstar', '--no-tone-detect', args.out / 'legacy' / f'{n}__f{f}.wav', b], check=True, capture_output=True)
                fa, fb = a.read_bytes(), b.read_bytes()
                # the legacy input is 50 samples longer, which can add one frame at the end
                if fb[:len(fa)] != fa or len(fb) - len(fa) not in (0, 9):
                    bad += 1
                    print('MISMATCH', n, f)
        print(f'check: {len(rows) * len(args.framings) - bad} of {len(rows) * len(args.framings)} identical')
        return 1 if bad else 0
    return 0


if __name__ == '__main__':
    sys.exit(main())
