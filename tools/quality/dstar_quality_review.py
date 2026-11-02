#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reproducible D-STAR outlier capture and parameter-substitution experiments.

Uses an explicit baseline JSON to select encoder/decoder outliers plus controls.
Captures are serialised on one hardware port; existing golden audio is never changed.
Each run records executable/input hashes and writes results after every recording.
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

os.environ.setdefault('OPENBLAS_NUM_THREADS', '1')
os.environ.setdefault('OMP_NUM_THREADS', '1')
import numpy as np

from compare import read_wav, find_tool
from encoder_compare import parse_parms, best_offset
from frontend_oracle import oracle_lines
from hil_encoder import find_ambe3000
from perceptual import score

ROOT = Path(__file__).resolve().parents[2]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(*args):
    result = subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=120)
    if result.returncode:
        raise RuntimeError(f'{args[0]} failed: {result.stderr}')
    return result.stdout


def select(rows):
    enc = sorted(rows, key=lambda n: rows[n]['dstar_ours_chip_recorded']['pesq'] - rows[n]['dstar_chip_chip']['pesq'])[:5]
    dec = sorted(rows, key=lambda n: rows[n]['dstar_chip_ours']['pesq'] - rows[n]['dstar_chip_chip']['pesq'])[:5]
    controls = ['OSR_uk_000_0020', 'arctic_slt_arctic_a0009', 'libri_672-122797-0001']
    return list(dict.fromkeys(enc + dec + controls))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--baseline', type=Path, default=ROOT/'build/audio-quality-review/current_scores.json')
    ap.add_argument('--workdir', type=Path, default=ROOT/'build/audio-quality-followup')
    ap.add_argument('--tool', type=Path, default=None)
    ap.add_argument('--port', default='COM3')
    ap.add_argument('--only', nargs='+')
    ap.add_argument('--modes', nargs='+', default=['pitch', 'voicing', 'gain', 'shape', 'shape_keep', 'all'],
                    choices=['pitch', 'voicing', 'gain', 'shape', 'shape_keep', 'all'])
    ap.add_argument('--frames', default='all', choices=['all', 'voiced', 'mixed', 'unvoiced', 'quiet', 'onset'])
    ap.add_argument('--software-only', action='store_true')
    args = ap.parse_args()
    rows = json.loads(args.baseline.read_text(encoding='utf-8'))['files']
    names = args.only or select(rows)
    tool = (args.tool or find_tool()).resolve()
    chip = None if args.software_only else find_ambe3000().resolve()
    out = args.workdir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    tag = args.frames + ('_software' if args.software_only else '_hardware')
    report = {'tool': str(tool), 'tool_sha256': sha(tool), 'frame_filter': args.frames,
              'selected': names, 'files': {}}
    if chip:
        report.update(hardware_tool_sha256=sha(chip), hardware_identify=run(chip, '--port', args.port, 'identify'))
    print(f'{len(names)} files; modes {args.modes}; frames {args.frames}', flush=True)
    for name in names:
        wav = ROOT/'testdata/corpus'/f'{name}.wav'
        golden = ROOT/'corpus/golden/dstar'/f'{name}.dvsi'
        frames = out/f'{name}.base.dvsi'
        op, cp = out/f'{name}.ours.parms', out/f'{name}.chip.parms'
        run(tool, 'encode', '--rate', 'dstar', wav, frames)
        run(tool, 'parms', '--rate', 'dstar', frames, op)
        run(tool, 'parms', '--rate', 'dstar', golden, cp)
        ours, reference = parse_parms(op), parse_parms(cp)
        offset, agreement = best_offset(ours, reference)
        original = read_wav(wav)
        row = {'source_sha256': sha(wav), 'base_frames_sha256': sha(frames), 'golden_sha256': sha(golden),
               'offset_frames': offset, 'offset_pitch_agreement': agreement, 'modes': {}}
        variants = [('base', frames), ('reference', golden)]
        for mode in args.modes:
            parts = ['shape'] if mode == 'shape_keep' else [mode]
            basic_filter = args.frames if args.frames in ['all', 'voiced', 'mixed', 'unvoiced'] else 'all'
            lines, forced = oracle_lines(ours, reference, offset, parts, basic_filter, mode == 'shape_keep')
            if args.frames in ['quiet', 'onset']:
                # Filter by the reference model. Gamma is a model-domain quantity, not dBFS.
                for i in range(len(lines)):
                    j = i + offset
                    use = False
                    if 0 <= j < len(reference):
                        cur = reference[j]
                        if args.frames == 'quiet':
                            use = cur[3] * 6.0206 < 20
                        elif j > 0:
                            use = cur[3] - reference[j-1][3] > 1.0
                    if not use:
                        lines[i] = '0 -1 nan 0'
                forced = sum(line != '0 -1 nan 0' and line != '0.0000 -1 nan 0' for line in lines)
            oracle = out/f'{name}.{tag}.{mode}.oracle'
            oracle.write_text('\n'.join(lines)+'\n', encoding='ascii')
            dst = out/f'{name}.{tag}.{mode}.dvsi'
            run(tool, 'encode', '--rate', 'dstar', '--oracle-file', oracle, wav, dst)
            variants.append((mode, dst))
            row['modes'][mode] = {'forced_frames': forced, 'frames': len(lines), 'oracle_sha256': sha(oracle)}
        for mode, data in variants:
            rec = row['modes'].setdefault(mode, {})
            rec['frames_sha256'] = sha(data)
            sw = out/f'{name}.{tag}.{mode}.software.wav'
            run(tool, 'decode', '--rate', 'dstar', data, sw)
            rec['software'] = score(original, read_wav(sw), None)
            if chip:
                hw = out/f'{name}.{tag}.{mode}.hardware.wav'
                run(chip, '--port', args.port, '--rate', 'dstar', 'decode', data, hw)
                rec['hardware'] = score(original, read_wav(hw), None)
                rec['hardware_audio_sha256'] = sha(hw)
            print(f'{name} {mode}: '+str(rec.get('hardware', rec['software'])), flush=True)
        report['files'][name] = row
        (out/f'oracle_{tag}.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    for decoder in ['software'] + (['hardware'] if chip else []):
        for mode in ['base', 'reference'] + args.modes:
            print(decoder, mode, {m: round(float(np.mean([r['modes'][mode][decoder][m] for r in report['files'].values()])), 4)
                                 for m in ['pesq', 'estoi']}, flush=True)


if __name__ == '__main__':
    main()
