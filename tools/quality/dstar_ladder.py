#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""D-STAR reference ladder (docs/encoder-quality.md).

  python tools/quality/dstar_ladder.py frames              # our rows' frames (software)
  python tools/quality/dstar_ladder.py hardware [--port COM3] [--framings 0 40 80 120]
  python tools/quality/dstar_ladder.py score               # PESQ / ESTOI, both decoders
  python tools/quality/dstar_ladder.py report [--out corpus/baselines/dstar_ladder.json]

Inputs are prepared by dstar_ladder_inputs.py (every recording at four input framings).
Rows, all D-STAR, each scored through our decoder and the AMBE-3000R's against the original:

  chip      the AMBE-3000R's own encoder
  default   the library's default encoder
  weighted  MBEVOCODER_DSTAR_WEIGHTED_QUANTIZER (build/dstar-ladder/weighted)
  float     + floating-point band-energy amplitudes (offline, build/dstar-analysis)
  pitch     + continuity-aware pitch correction (offline, build/dstar-perception)

The offline rows come from the 22-23 September pipelines run on the `legacy` inputs, which
give the current framing (dstar_ladder_inputs.py --check). Scores are averaged over the
framings of each recording before any comparison, because a sub-frame shift alone moves a
file by up to 0.5 PESQ (docs/encoder-quality.md "Method"); paired differences are reported with
their standard error over recordings.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare import read_wav  # noqa: E402
from hil_encoder import find_ambe3000  # noqa: E402
from perceptual import score  # noqa: E402

LADDER = ROOT / 'build/dstar-ladder'
ROWS = ['chip', 'default', 'weighted', 'float', 'pitch']
TOOLS = {'default': ROOT / 'build/vs2026/tools/mbe-cli/Release/mbe-cli.exe',
         'weighted': LADDER / 'weighted/tools/mbe-cli/Release/mbe-cli.exe'}
OFFLINE = {'float': ROOT / 'build/dstar-perception/{n}.energy.dvsi',
           'pitch': ROOT / 'build/dstar-perception/{n}.tracked_pitch.dvsi'}
SOURCE_DIRS = {'dev': ROOT / 'testdata/corpus', 'new1': ROOT / 'build/dstar-pitch/heldout', 'new2': ROOT / 'build/dstar-ideas/heldout',
               'new3': ROOT / 'build/dstar-analysis/heldout', 'new4': ROOT / 'build/dstar-perception/heldout'}


def manifest():
    return json.loads((LADDER / 'aligned/index.json').read_text())['files']


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def run(*args):
    r = subprocess.run([str(a) for a in args], capture_output=True, text=True, timeout=600)
    if r.returncode:
        raise RuntimeError(f'{args}: {r.stderr}')
    return r.stdout


def frames_path(name, row):
    return LADDER / 'frames' / f'{name}.{row}.dvsi'


def cmd_frames(args):
    (LADDER / 'frames').mkdir(parents=True, exist_ok=True)
    files = manifest()

    def one(f):
        n = f['name']
        for row, tool in TOOLS.items():
            run(tool, 'encode', '--rate', 'dstar', '--no-tone-detect', LADDER / 'aligned' / f'{n}.wav', frames_path(n, row))
        for row, pattern in OFFLINE.items():
            src = Path(str(pattern).format(n=n))
            if not src.exists():
                raise SystemExit(f'{src} missing: run the offline pipelines on build/dstar-ladder/legacy first')
            shutil.copyfile(src, frames_path(n, row))
        # the offline rows were encoded from the legacy input (50 samples longer): same frames,
        # possibly one more at the end; trim to the library rows' length
        ref = frames_path(n, 'weighted').read_bytes()
        for row in OFFLINE:
            b = frames_path(n, row).read_bytes()
            frames_path(n, row).write_bytes(b[:len(ref)])
        return n
    with ThreadPoolExecutor(6) as ex:
        for i, n in enumerate(ex.map(one, files)):
            if i % 50 == 0:
                print(f'{i}/{len(files)} {n}', flush=True)
    # the offline rows start from the weighted encoder: their controls must equal it
    base_ok = sum(Path(str(ROOT / 'build/dstar-perception/{n}.base.dvsi').format(n=f['name'])).read_bytes()[:frames_path(f['name'], 'weighted').stat().st_size]
                  == frames_path(f['name'], 'weighted').read_bytes() for f in files)
    print(f'offline pipeline control equals the weighted encoder on {base_ok} of {len(files)}')


def cmd_hardware(args):
    chip = find_ambe3000()
    files = [f for f in manifest() if f['framing'] in args.framings]
    out = LADDER / 'hw'
    out.mkdir(parents=True, exist_ok=True)
    for i, f in enumerate(files):
        n = f['name']
        enc = frames_path(n, 'chip')
        if not enc.exists():
            run(chip, '--port', args.port, '--rate', 'dstar', 'encode', LADDER / 'aligned' / f'{n}.wav', enc)
        for row in ROWS:
            wav = out / f'{n}.{row}.wav'
            if not wav.exists() and frames_path(n, row).exists():   # rows not yet encoded are filled in by a later run
                run(chip, '--port', args.port, '--rate', 'dstar', 'decode', frames_path(n, row), wav)
        print(f'{i + 1}/{len(files)} {n}', flush=True)


def cmd_score(args):
    files = manifest()
    (LADDER / 'ours').mkdir(parents=True, exist_ok=True)
    cache_path = LADDER / 'scores.json'
    cache = json.loads(cache_path.read_text()) if cache_path.exists() else {}

    def one(f):
        n = f['name']
        ref = read_wav(SOURCE_DIRS[f['set']] / f"{f['source']}.wav")
        rec = cache.get(n, {})
        for row in ROWS:
            fr = frames_path(n, row)
            if not fr.exists():
                continue
            key = f'{row}.ours'
            if key not in rec or rec[key].get('frames_sha256') != sha(fr):
                wav = LADDER / 'ours' / f'{n}.{row}.wav'
                run(TOOLS['default'], 'decode', '--rate', 'dstar', fr, wav)
                s = score(ref, read_wav(wav), None)
                rec[key] = {'pesq': s['pesq'], 'estoi': s['estoi'], 'frames_sha256': sha(fr)}
            hw = LADDER / 'hw' / f'{n}.{row}.wav'
            key = f'{row}.chip'
            if hw.exists() and (key not in rec or rec[key].get('audio_sha256') != sha(hw)):
                s = score(ref, read_wav(hw), None)
                rec[key] = {'pesq': s['pesq'], 'estoi': s['estoi'], 'frames_sha256': sha(fr), 'audio_sha256': sha(hw)}
        return n, rec
    with ThreadPoolExecutor(6) as ex:
        for i, (n, rec) in enumerate(ex.map(one, files)):
            cache[n] = rec
            if i % 50 == 0:
                print(f'{i}/{len(files)}', flush=True)
                cache_path.write_text(json.dumps(cache))
    cache_path.write_text(json.dumps(cache))
    print(f'scored {len(cache)} files')


def cmd_report(args):
    files = manifest()
    cache = json.loads((LADDER / 'scores.json').read_text())
    groups = {'dev': ['dev'], 'new (all four sets)': ['new1', 'new2', 'new3', 'new4'], 'new1': ['new1'], 'new2': ['new2'], 'new3': ['new3'], 'new4': ['new4']}
    report = {'framings': sorted({f['framing'] for f in files}), 'groups': {}}
    lines = []
    for decoder in ('chip', 'ours'):
        for g, sets in groups.items():
            recs = sorted({f['source'] for f in files if f['set'] in sets})
            # per recording, the mean over the framings that have every row scored
            table = {}
            for metric in ('pesq', 'estoi'):
                per = {}
                for r in recs:
                    fr = [f for f in files if f['source'] == r and f['set'] in sets]
                    ok = [f for f in fr if all(f'{row}.{decoder}' in cache.get(f['name'], {}) for row in ROWS)]
                    if ok:
                        per[r] = {row: float(np.mean([cache[f['name']][f'{row}.{decoder}'][metric] for f in ok])) for row in ROWS}
                if not per:
                    continue
                table[metric] = {'recordings': len(per), 'mean': {row: round(float(np.mean([v[row] for v in per.values()])), 4) for row in ROWS}}
                diffs = {}
                for a, b in (('default', 'chip'), ('weighted', 'default'), ('float', 'weighted'), ('pitch', 'float'), ('pitch', 'default'), ('pitch', 'chip')):
                    d = np.array([v[a] - v[b] for v in per.values()])
                    diffs[f'{a}-{b}'] = {'mean': round(float(d.mean()), 4), 'se': round(float(d.std(ddof=1) / np.sqrt(len(d))), 4) if len(d) > 1 else None,
                                         'better': int((d > 0).sum()), 'worse': int((d < 0).sum()), 'worst': round(float(d.min()), 3)}
                table[metric]['differences'] = diffs
            if table:
                report['groups'][f'{g} / {decoder} decoder'] = table
                t = table['pesq']
                lines.append(f"{g:22s} {decoder:4s} n={t['recordings']:3d}  " + '  '.join(f"{row} {t['mean'][row]:.3f}" for row in ROWS)
                             + f"   pitch-default {t['differences']['pitch-default']['mean']:+.3f}±{t['differences']['pitch-default']['se'] or 0:.3f}"
                             + f"   pitch-chip {t['differences']['pitch-chip']['mean']:+.3f}±{t['differences']['pitch-chip']['se'] or 0:.3f}")
    print('\n'.join(lines))
    report['provenance'] = {'tools': {k: {'path': str(v.relative_to(ROOT)), 'sha256': sha(v)} for k, v in TOOLS.items()},
                            'hardware_identify': run(find_ambe3000(), '--port', args.port, 'identify').strip() if args.identify else None,
                            'inputs': 'build/dstar-ladder/aligned/index.json (dstar_ladder_inputs.py)'}
    report['files'] = cache
    args.out.write_text(json.dumps(report, indent=1))
    print(f'wrote {args.out}')


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cmd', choices=['frames', 'hardware', 'score', 'report'])
    ap.add_argument('--port', default='COM3')
    ap.add_argument('--framings', type=int, nargs='+', default=[0, 40, 80, 120])
    ap.add_argument('--identify', action='store_true', help='record the AMBE-3000R identity in the report')
    ap.add_argument('--out', type=Path, default=ROOT / 'corpus/baselines/dstar_ladder.json')
    args = ap.parse_args()
    {'frames': cmd_frames, 'hardware': cmd_hardware, 'score': cmd_score, 'report': cmd_report}[args.cmd](args)
    return 0


if __name__ == '__main__':
    sys.exit(main())
