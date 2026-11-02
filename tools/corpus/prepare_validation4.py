#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prepare the fresh validation set of docs/encoder-quality.md (fixed 24 September 2026,
before any codec scoring on it).

  python tools/corpus/prepare_validation4.py [--out build/step5]

32 recordings from sources no earlier set used:
- LibriSpeech test-other (CC BY 4.0; its speakers are disjoint from test-clean's): the first
  five female and first five male speaker IDs (string order, as the earlier sets), two
  utterances each: the speaker's first utterance, and of their next nine the one with the
  highest noise floor (10th percentile of 20 ms frame levels relative to the active level),
  so the set carries some real background noise. A signal property, not a codec score.
- CMU ARCTIC speakers awb (Scottish English, male), ksp (Indian English, male), jmk (Canadian
  English, male) and clb (US English, female): arctic_b0001-b0003 each (the b sentences, so
  the text differs from the development set's arctic_a sentences).
Each is converted to 8 kHz (soxr), capped at 12 seconds, DC removed and set to -26 dBov
active level, as the other sets (tools/corpus/prepare.py). Writes `<out>/fresh/<name>.wav`
with index.json, and `<out>/aligned/<name>__fF.wav` at framings 0, 40, 80 and 120 samples
with an index in tools/quality/dstar_ladder_inputs.py's format (set 'fresh').
"""
import argparse
import hashlib
import json
import sys
import tarfile
from pathlib import Path

import numpy as np
from scipy.io import wavfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from prepare import normalise, run_ffmpeg_to_8k  # noqa: E402

LIBRI = ROOT / 'corpus/raw/librispeech_test_other/test-other.tar.gz'
ARCTIC = {'awb': ('male', 'Scottish English'), 'ksp': ('male', 'Indian English'),
          'jmk': ('male', 'Canadian English'), 'clb': ('female', 'US English')}
ARCTIC_DIR = ROOT / 'corpus/raw/arctic_extra'
FRAMINGS = [0, 40, 80, 120]
SELECTION = ('Fixed 2026-09-24 before scoring. LibriSpeech test-other: first 5 female and 5 male speaker IDs '
             '(string order); per speaker the first utterance and, of the next nine, the one with the highest '
             'noise floor; first 12 s. CMU ARCTIC awb, ksp, jmk, clb: arctic_b0001-b0003.')


def noise_floor_db(x: np.ndarray) -> float:
    """10th percentile of 20 ms frame RMS, in dB relative to the 95th percentile frame."""
    n = len(x) // 160
    rms = np.sqrt(np.mean(x[: n * 160].reshape(n, 160) ** 2, axis=1)) + 1e-9
    db = 20 * np.log10(rms)
    return float(np.percentile(db, 10) - np.percentile(db, 95))


def convert(data: bytes, suffix: str, tmp: Path):
    tmp.write_bytes(data)
    tmp_named = tmp.with_suffix(suffix)
    tmp.replace(tmp_named)
    x = run_ffmpeg_to_8k(tmp_named, 12)
    tmp_named.unlink()
    return x


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', type=Path, default=ROOT / 'build/step5')
    args = ap.parse_args()
    fresh = args.out / 'fresh'
    fresh.mkdir(parents=True, exist_ok=True)
    tmp = fresh / 'tmp.bin'
    index = []

    def write(name, x, meta):
        y, info = normalise(x, -26)
        wav = fresh / f'{name}.wav'
        wavfile.write(wav, 8000, np.round(y * 32767).astype(np.int16))
        index.append({'name': name, **meta, 'seconds': round(len(y) / 8000, 3),
                      'noise_floor_db': round(noise_floor_db(y), 1), 'sha256': hashlib.sha256(wav.read_bytes()).hexdigest(), **info})
        print(f"{name}: {meta.get('sex')} {meta.get('accent', '')} {len(y) / 8000:.1f} s, floor {noise_floor_db(y):.1f} dB")

    with tarfile.open(LIBRI) as tar:
        members = {m.name: m for m in tar.getmembers() if m.isfile()}
        spk = next(n for n in members if n.endswith('SPEAKERS.TXT'))
        sexes = {}
        for line in tar.extractfile(members[spk]).read().decode('utf-8').splitlines():
            parts = [p.strip() for p in line.split('|')]
            if len(parts) >= 3 and not line.startswith(';') and parts[2] == 'test-other':
                sexes[parts[0]] = 'male' if parts[1] == 'M' else 'female'
        flacs = sorted(n for n in members if n.endswith('.flac'))
        for sex in ('female', 'male'):
            for s in sorted(k for k, v in sexes.items() if v == sex)[:5]:
                own = [f for f in flacs if f'/test-other/{s}/' in f][:10]
                assert len(own) == 10, s
                audio = [convert(tar.extractfile(members[f]).read(), '.flac', tmp) for f in own]
                floors = [noise_floor_db(normalise(a, -26)[0]) for a in audio]
                noisy = 1 + int(np.argmax(floors[1:]))
                for k, why in ((0, 'first'), (noisy, 'noisiest of next nine')):
                    write('other_' + Path(own[k]).stem, audio[k],
                          {'source': 'librispeech_test_other', 'member': own[k], 'speaker': s, 'sex': sex, 'chosen': why})

    for s, (sex, accent) in ARCTIC.items():
        path = ARCTIC_DIR / f'cmu_us_{s}_arctic-0.95-release.tar.bz2'
        with tarfile.open(path) as tar:
            for i in (1, 2, 3):
                member = f'cmu_us_{s}_arctic/wav/arctic_b{i:04d}.wav'
                write(f'arctic_{s}_arctic_b{i:04d}', convert(tar.extractfile(member).read(), '.wav', tmp),
                      {'source': f'cmu_arctic_{s}', 'member': member, 'speaker': s, 'sex': sex, 'accent': accent})

    (fresh / 'index.json').write_text(json.dumps({'selection': SELECTION, 'files': index}, indent=1), encoding='utf-8')
    aligned = args.out / 'aligned'
    aligned.mkdir(parents=True, exist_ok=True)
    files = []
    for f in index:
        src = fresh / f"{f['name']}.wav"
        fs, x = wavfile.read(src)
        for fr in FRAMINGS:
            name = f"{f['name']}__f{fr}"
            wavfile.write(aligned / f'{name}.wav', fs, np.concatenate([np.zeros(fr, np.int16), x]))
            files.append({'name': name, 'set': 'fresh', 'source': f['name'], 'framing': fr, 'source_sha256': f['sha256']})
    (aligned / 'index.json').write_text(json.dumps({'files': files}, indent=1))
    print(f'{len(index)} recordings, {len(files)} framed inputs in {aligned}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
