#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare mbevocoder and an independent IMBE encoder through mbevocoder and JMBE.

The reference adapter reads s16le mono 8 kHz PCM and writes packed 88-bit frames.
Dependencies are standalone test tools, not library dependencies. No hardware claim
is made for the OpenBLIP25 software reference. SHA256s record all encoded inputs.
"""
import argparse
import hashlib
import json
import os
import subprocess
import threading
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor, as_completed

os.environ.setdefault('OPENBLAS_NUM_THREADS', '1')
os.environ.setdefault('OMP_NUM_THREADS', '1')
from scipy.io import wavfile
from compare import read_wav, find_tool
from perceptual import score
from jmbe_crosscheck import java_bin, classpath

ROOT = Path(__file__).resolve().parents[2]


def run(*args):
    p = subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=120)
    if p.returncode:
        raise RuntimeError(f'{args[0]} failed: {p.stderr}')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--encoder', type=Path, default=ROOT/'build/audio-quality-followup/reference-imbe-build/release/reference-imbe.exe')
    ap.add_argument('--packer', type=Path, default=ROOT/'build/audio-quality-followup/pack-build/Release/reference-pack.exe')
    ap.add_argument('--workdir', type=Path, default=ROOT/'build/audio-quality-followup/imbe')
    ap.add_argument('--jmbe', type=Path, default=ROOT.parent/'jmbe')
    ap.add_argument('--only', nargs='+')
    ap.add_argument('--jobs', type=int, default=2)
    args = ap.parse_args()
    out = args.workdir.resolve(); out.mkdir(parents=True, exist_ok=True)
    tool = find_tool().resolve()
    cp = classpath(args.jmbe.resolve())
    run(java_bin('javac'), '-cp', cp, '-d', out, ROOT/'tools/quality/jmbe/JmbeDecode.java')
    names = args.only or sorted(json.loads((ROOT/'build/audio-quality-review/current_scores.json').read_text())['files'])
    report = {'reference': 'OpenBLIP25 blip25-vocoder 1.0.0; independent software, not hardware',
              'reference_encoder_sha256': sha(args.encoder), 'mbevocoder_tool_sha256': sha(tool),
              'packer_sha256': sha(args.packer), 'files': {}}
    score_lock = threading.Lock()  # Keep native PESQ calls serial; external decoders may overlap.

    def task(name):
        wav = ROOT/'testdata/corpus'/f'{name}.wav'
        fs, pcm = wavfile.read(wav)
        if fs != 8000 or pcm.ndim != 1 or str(pcm.dtype) != 'int16':
            raise ValueError(f'{wav}: expected 8 kHz mono int16')
        raw = out/f'{name}.s16le'; raw.write_bytes(pcm.astype('<i2').tobytes())
        rec = {'source_sha256': sha(wav)}
        own_raw = out/f'{name}.ours.raw'
        own_wire = out/f'{name}.ours.imbe'
        # Verify that the format adapter reproduces mbe-cli's own FEC/layout exactly.
        run(tool, 'encode', '--rate', 'imbe4400', wav, own_raw)
        run(args.packer, own_raw, own_wire)
        control = out/f'{name}.control.imbe'
        run(tool, 'encode', '--rate', 'imbe7200', wav, control)
        if own_wire.read_bytes() != control.read_bytes():
            raise RuntimeError(f'{name}: format adapter disagrees with mbe-cli')
        rec['packer_control_identical'] = True
        ref_raw = out/f'{name}.reference.raw'; ref_wire = out/f'{name}.reference.imbe'
        run(args.encoder, raw, ref_raw)
        if not ref_raw.stat().st_size or ref_raw.stat().st_size % 11:
            raise RuntimeError('malformed reference frame file')
        run(args.packer, ref_raw, ref_wire)
        source = read_wav(wav)
        for enc, frames in [('ours', own_wire), ('reference', ref_wire)]:
            rec[enc+'_frames_sha256'] = sha(frames)
            rec[enc+'_frame_count'] = frames.stat().st_size // 18
            for dec in ['ours', 'jmbe']:
                audio = out/f'{name}.{enc}.{dec}.wav'
                if dec == 'ours':
                    run(tool, 'decode', '--rate', 'imbe7200', frames, audio)
                else:
                    run(java_bin('java'), '-cp', str(out)+';'+cp, 'JmbeDecode', frames, audio)
                with score_lock:
                    rec[enc+'_'+dec] = score(source, read_wav(audio), None)
        return name, rec

    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for job in as_completed([pool.submit(task, name) for name in names]):
            name, rec = job.result()
            report['files'][name] = rec
            (out/'comparison.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
            print(name, {k: rec[k]['pesq'] for k in ['ours_ours', 'ours_jmbe', 'reference_ours', 'reference_jmbe']}, flush=True)


if __name__ == '__main__':
    main()
