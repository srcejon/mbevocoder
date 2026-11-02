#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Encoder comparison for any rate, averaged over input framings (docs/encoder-quality.md).

  python tools/quality/rate_ladder.py --rate ambe2450 --row default=build/vs2026/tools/mbe-cli/Release/mbe-cli.exe \\
        --row candidate=build/step4/ambe2450/tools/mbe-cli/Release/mbe-cli.exe [--hardware] [--chip-encoder] [--jmbe]
        [--framings 0 40 80 120] [--out build/step4/ambe2450.json]

Uses the inputs of dstar_ladder_inputs.py (build/dstar-ladder/aligned: the 70 development
recordings and the four new-speaker sets at four framings, which are input delays and so
apply to every rate). Each row's executable encodes every input (tone detection off, as the
ladder); frames are decoded by our decoder, and with --hardware by the AMBE-3000R (AMBE rates),
with --jmbe by JMBE (IMBE, tools/quality/jmbe_crosscheck.py's harness). --chip-encoder adds
the AMBE-3000R's own encoder as a row. Scores are PESQ and ESTOI against the original,
averaged over each recording's framings before paired differences are taken.
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare import read_wav  # noqa: E402
from perceptual import score  # noqa: E402

LADDER = ROOT / 'build/dstar-ladder'
SOURCE_DIRS = {'dev': ROOT / 'testdata/corpus', 'new1': ROOT / 'build/dstar-pitch/heldout', 'new2': ROOT / 'build/dstar-ideas/heldout',
               'new3': ROOT / 'build/dstar-analysis/heldout', 'new4': ROOT / 'build/dstar-perception/heldout',
               'fresh': ROOT / 'build/step5/fresh'}
GROUPS = (('dev', ['dev']), ('new', ['new1', 'new2', 'new3', 'new4']), ('fresh', ['fresh']))
DECODE_RATE = {'ambe2450': 'ambe2450', 'dstar': 'dstar', 'imbe7200': 'imbe7200'}


def run(*args, env=None):
    r = subprocess.run([str(a) for a in args], capture_output=True, text=True, timeout=600, env=env)
    if r.returncode:
        raise RuntimeError(f'{args}: {r.stderr}')
    return r.stdout


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--rate', required=True, choices=list(DECODE_RATE))
    ap.add_argument('--row', action='append', default=[], help='name=path to an mbe-cli executable')
    ap.add_argument('--framings', type=int, nargs='+', default=[0, 40, 80, 120])
    ap.add_argument('--hardware', action='store_true')
    ap.add_argument('--chip-encoder', action='store_true')
    ap.add_argument('--jmbe', action='store_true')
    ap.add_argument('--port', default='COM3')
    ap.add_argument('--decoder', type=Path, default=ROOT / 'build/vs2026/tools/mbe-cli/Release/mbe-cli.exe')
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--inputs', type=Path, default=LADDER / 'aligned',
                    help='framed inputs with an index.json (default: the ladder; build/step5/aligned for the fresh set)')
    args = ap.parse_args()
    rows = dict(r.split('=', 1) for r in args.row)
    rows = {k: Path(v).resolve() for k, v in rows.items()}
    work = args.out.with_suffix('')
    work.mkdir(parents=True, exist_ok=True)
    files = [f for f in json.loads((args.inputs / 'index.json').read_text())['files'] if f['framing'] in args.framings]
    cache_path = work / 'scores.json'
    cache = json.loads(cache_path.read_text()) if cache_path.exists() else {}
    names = list(rows) + (['chip'] if args.chip_encoder else [])
    decoders = ['ours'] + (['chip'] if args.hardware else []) + (['jmbe'] if args.jmbe else [])

    # frames (software rows in parallel; the AMBE-3000R's encoder serially below)
    def enc(f):
        for row, exe in rows.items():
            fr = work / f"{f['name']}.{row}.frames"
            if not fr.exists():
                run(exe, 'encode', '--rate', args.rate, '--no-tone-detect', args.inputs / f"{f['name']}.wav", fr)
    with ThreadPoolExecutor(6) as ex:
        list(ex.map(enc, files))
    chip = None
    if args.hardware or args.chip_encoder:
        from hil_encoder import find_ambe3000
        chip = find_ambe3000()
    if args.chip_encoder:
        for f in files:
            fr = work / f"{f['name']}.chip.frames"
            if not fr.exists():
                run(chip, '--port', args.port, '--rate', args.rate, 'encode', args.inputs / f"{f['name']}.wav", fr)

    jmbe_cp = None
    if args.jmbe:
        sys.path.insert(0, str(ROOT / 'tools/quality'))
        from jmbe_crosscheck import classpath, java_bin
        jmbe_cp = classpath(ROOT.parent / 'jmbe')
        jw = ROOT / 'build/jmbe'
        if not (jw / 'JmbeDecode.class').exists():
            run(java_bin('javac'), '-cp', jmbe_cp, '-d', jw, ROOT / 'tools/quality/jmbe/JmbeDecode.java')

    def decode_ours(f):
        rec = cache.get(f['name'], {})
        ref = read_wav(SOURCE_DIRS[f['set']] / f"{f['source']}.wav")
        for row in names:
            fr = work / f"{f['name']}.{row}.frames"
            key = f'{row}.ours'
            if key not in rec or rec[key]['frames_sha256'] != sha(fr):
                wav = work / f"{f['name']}.{row}.ours.wav"
                run(args.decoder, 'decode', '--rate', DECODE_RATE[args.rate], fr, wav)
                s = score(ref, read_wav(wav), None)
                rec[key] = {'pesq': s['pesq'], 'estoi': s['estoi'], 'frames_sha256': sha(fr)}
            if args.jmbe:
                key = f'{row}.jmbe'
                if key not in rec or rec[key]['frames_sha256'] != sha(fr):
                    wav = work / f"{f['name']}.{row}.jmbe.wav"
                    run(java_bin('java'), '-cp', f"{ROOT / 'build/jmbe'};{jmbe_cp}", 'JmbeDecode', fr, wav)
                    s = score(ref, read_wav(wav), None)
                    rec[key] = {'pesq': s['pesq'], 'estoi': s['estoi'], 'frames_sha256': sha(fr)}
        return f['name'], rec
    with ThreadPoolExecutor(6) as ex:
        for n, rec in ex.map(decode_ours, files):
            cache[n] = rec
    cache_path.write_text(json.dumps(cache))
    if args.hardware:
        for i, f in enumerate(files):
            rec = cache.setdefault(f['name'], {})
            ref = None
            for row in names:
                fr = work / f"{f['name']}.{row}.frames"
                key = f'{row}.chip'
                if key in rec and rec[key]['frames_sha256'] == sha(fr):
                    continue
                wav = work / f"{f['name']}.{row}.chip.wav"
                run(chip, '--port', args.port, '--rate', args.rate, 'decode', fr, wav)
                ref = ref if ref is not None else read_wav(SOURCE_DIRS[f['set']] / f"{f['source']}.wav")
                s = score(ref, read_wav(wav), None)
                rec[key] = {'pesq': s['pesq'], 'estoi': s['estoi'], 'frames_sha256': sha(fr)}
            if i % 20 == 0:
                cache_path.write_text(json.dumps(cache))
                print(f'hardware {i + 1}/{len(files)}', flush=True)
        cache_path.write_text(json.dumps(cache))

    report = {'rate': args.rate, 'framings': args.framings, 'rows': {k: {'path': str(v), 'sha256': sha(v)} for k, v in rows.items()}, 'groups': {}}
    base = next(iter(rows))
    for decoder in decoders:
        for g, sets in GROUPS:
            recs = sorted({f['source'] for f in files if f['set'] in sets})
            per = {}
            for r in recs:
                fr = [f['name'] for f in files if f['source'] == r and f['set'] in sets]
                if all(f'{row}.{decoder}' in cache.get(n, {}) for n in fr for row in names):
                    per[r] = {row: {m: float(np.mean([cache[n][f'{row}.{decoder}'][m] for n in fr])) for m in ('pesq', 'estoi')} for row in names}
            if not per:
                continue
            out = {'recordings': len(per), 'mean': {row: {m: round(float(np.mean([v[row][m] for v in per.values()])), 4) for m in ('pesq', 'estoi')} for row in names}, 'differences': {}}
            line = f"{g:4s} {decoder:5s} n={len(per):3d} " + ' '.join(f"{row} {out['mean'][row]['pesq']:.3f}/{out['mean'][row]['estoi']:.3f}" for row in names)
            for row in names:
                if row == base:
                    continue
                for m in ('pesq', 'estoi'):
                    d = np.array([v[row][m] - v[base][m] for v in per.values()])
                    out['differences'][f'{row}-{base}.{m}'] = {'mean': round(float(d.mean()), 4), 'se': round(float(d.std(ddof=1) / np.sqrt(len(d))), 4),
                                                               'better': int((d > 0).sum()), 'worse': int((d < 0).sum()), 'worst': round(float(d.min()), 3)}
                x = out['differences'][f'{row}-{base}.pesq']
                line += f" | {row}-{base} {x['mean']:+.3f}±{x['se']:.3f} ({x['better']}/{x['worse']}, worst {x['worst']:+.2f})"
            report['groups'][f'{g} / {decoder}'] = out
            print(line)
    report['files'] = cache
    args.out.write_text(json.dumps(report, indent=1))
    print(f'wrote {args.out}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
