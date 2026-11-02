#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Listening clips for pauses (docs/encoder-quality.md "Open": a listener should confirm the pauses
are free of buzz since the input gain and the voicing gate).

  python tools/quality/listening_pauses.py [--port COM3] [--out build/listening_pauses] [--clips 8]

Clips come from the corpus speech, chosen by content before any comparison: in each recording, the
3 s window with the most pause frames (20 ms frames more than 35 dB below the recording's loudest)
that still holds at least 30 % speech, so each clip has speech either side of a pause; one clip per
speaker, the recordings ranked by that count. Each clip is cut identically from every version,
aligned to the original, level-matched to the original's active level and faded 20 ms at both ends.
Labelled, not blind: the ungated encoder is there to show what the buzz sounds like.

  AMBE+2, D-STAR (all decoded by the AMBE-3000R, which decodes as DVSI's reference decoder does):
    chip      the AMBE-3000R's encoder (corpus/golden)
    gate      our encoder: input gain and voicing gate (the current build)
    nogate    our encoder with the input gain and no gate (build/agc/bin_on)
    nogain    our encoder without the input gain (build/agc/bin_off)
  IMBE (decoded by ours): openblip25 (build/audio-quality-followup/imbe) in place of chip.

Writes <out>/index.html to play them side by side, and <out>/clips.json.
"""
import argparse
import html
import json
import subprocess
import sys
from pathlib import Path

import numpy as np
from scipy.io import wavfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare import read_wav, align  # noqa: E402
from hil_encoder import find_ambe3000  # noqa: E402

CLIP = 24000          # 3 s
FADE = 160
FRAME = 160
CLI = {'gate': ROOT / 'build/vs2026/tools/mbe-cli/Release/mbe-cli.exe',
       'nogate': ROOT / 'build/agc/bin_on/mbe-cli.exe',
       'nogain': ROOT / 'build/agc/bin_off/mbe-cli.exe'}
RATES = {'ambe2450': 'AMBE+2', 'dstar': 'D-STAR', 'imbe7200': 'IMBE'}
LABEL = {'source': 'original', 'chip': 'AMBE-3000R encoder', 'openblip25': 'OpenBLIP25 encoder',
         'gate': 'ours (gain + gate)', 'nogate': 'ours, gain, no gate (buzz)', 'nogain': 'ours, no gain (previous)'}


def frame_db(x):
    n = len(x) // FRAME
    return 10 * np.log10(np.mean(x[:n * FRAME].reshape(n, FRAME) ** 2, axis=1) + 1e-12)


def best_window(x):
    db = frame_db(x)
    pause = (db < db.max() - 35).astype(int)
    w = CLIP // FRAME
    if len(pause) <= w:
        return None
    pc = np.convolve(pause, np.ones(w, int), 'valid')
    ok = (w - pc) >= 0.3 * w
    # leading and trailing silence are not pauses: keep the window off the file's ends
    ok[:5] = False; ok[-5:] = False
    if not ok.any():
        return None
    k = int(np.argmax(np.where(ok, pc, -1)))
    return k * FRAME, int(pc[k])


def speaker(name):
    if name.startswith('libri_'):
        return name.split('-')[0]
    if name.startswith('arctic_'):
        return name.split('_')[1]
    return '_'.join(name.split('_')[:3])


def active_rms(x):
    n = len(x) // FRAME
    f = np.sqrt(np.mean(x[:n * FRAME].reshape(n, FRAME) ** 2, axis=1))
    return float(np.sqrt(np.mean(f[f > f.max() * 0.03] ** 2))) + 1e-9


def cut(src, x, start):
    _, _, lag = align(x, src)
    y = x[lag + start:lag + start + CLIP] if lag >= 0 else np.concatenate([np.zeros(-lag), x])[start:start + CLIP]
    return np.pad(y, (0, CLIP - len(y)))


def fade(y):
    r = np.linspace(0, 1, FADE)
    y = y.copy(); y[:FADE] *= r; y[-FADE:] *= r[::-1]
    return y


def write(p, x):
    wavfile.write(p, 8000, np.clip(np.round(x * 32767), -32768, 32767).astype(np.int16))


def run(*a):
    subprocess.run([str(x) for x in a], check=True, capture_output=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', default='COM3')
    ap.add_argument('--out', type=Path, default=ROOT / 'build/listening_pauses')
    ap.add_argument('--clips', type=int, default=8)
    a = ap.parse_args()
    for p in CLI.values():
        if not p.exists():
            sys.exit(f'missing {p}')
    chip = find_ambe3000()
    work = a.out / 'work'; audio = a.out / 'audio'
    work.mkdir(parents=True, exist_ok=True); audio.mkdir(parents=True, exist_ok=True)

    ranked = []
    for p in sorted((ROOT / 'testdata/corpus').glob('*.wav')):
        if p.stem.startswith('syn_'):
            continue
        w = best_window(read_wav(p))
        if w:
            ranked.append((w[1], p.stem, w[0]))
    ranked.sort(reverse=True)
    chosen, seen = [], set()
    for count, name, start in ranked:
        if speaker(name) in seen:
            continue
        seen.add(speaker(name)); chosen.append((name, start, count))
        if len(chosen) == a.clips:
            break

    clips = []
    for i, (name, start, count) in enumerate(chosen, 1):
        src_path = ROOT / 'testdata/corpus' / f'{name}.wav'
        src = read_wav(src_path)
        s = np.pad(src[start:start + CLIP], (0, max(0, CLIP - len(src[start:start + CLIP]))))
        write(audio / f'c{i}-source.wav', fade(s))
        entry = {'clip': i, 'recording': name, 'speaker': speaker(name), 'start_s': start / 8000,
                 'pause_frames': count, 'rates': {}}
        for rate in RATES:
            ref = 'openblip25' if rate == 'imbe7200' else 'chip'
            bits = {ref: (ROOT / 'build/audio-quality-followup/imbe' / f'{name}.reference.imbe') if ref == 'openblip25'
                    else (ROOT / 'corpus/golden' / rate / f'{name}.dvsi')}
            for tag, cli in CLI.items():
                b = work / f'{name}.{rate}.{tag}.bits'
                if not b.exists():
                    run(cli, 'encode', '--rate', rate, src_path, b)
                bits[tag] = b
            files = {}
            for tag, b in bits.items():
                if not b.exists():
                    print(f'  {name} {rate}: no {tag} frames, skipped')
                    continue
                wav = work / f'{name}.{rate}.{tag}.wav'
                if not wav.exists():
                    if rate == 'imbe7200':
                        run(CLI['gate'], 'decode', '--rate', rate, b, wav)
                    else:
                        run(chip, '--port', a.port, '--rate', rate, 'decode', b, wav)
                y = cut(src, read_wav(wav), start)
                y *= active_rms(s) / active_rms(y)
                out = f'c{i}-{rate}-{tag}.wav'
                write(audio / out, fade(y))
                files[tag] = out
            entry['rates'][rate] = files
        clips.append(entry)
        print(f'c{i}: {name} at {start / 8000:.2f} s ({count} pause frames of {CLIP // FRAME})')

    (a.out / 'clips.json').write_text(json.dumps({'chosen': 'by pause count before any comparison', 'clips': clips}, indent=1))
    rows = []
    for c in clips:
        rows.append(f'<h2>Clip {c["clip"]}: {html.escape(c["recording"])}, {c["start_s"]:.2f} s '
                    f'<small>({c["pause_frames"]} of {CLIP // FRAME} frames pause)</small></h2>'
                    f'<p>{LABEL["source"]}<br><audio controls preload="none" src="audio/c{c["clip"]}-source.wav"></audio></p>')
        rows.append('<table><tr>' + ''.join(f'<th>{RATES[r]}</th>' for r in c['rates']) + '</tr><tr>')
        for r, files in c['rates'].items():
            rows.append('<td>' + ''.join(f'<div>{html.escape(LABEL[t])}<br><audio controls preload="none" src="audio/{f}"></audio></div>'
                                         for t, f in files.items()) + '</td>')
        rows.append('</tr></table>')
    (a.out / 'index.html').write_text(
        '<!doctype html><meta charset="utf-8"><title>Pause clips</title><style>body{font:14px system-ui;margin:16px}'
        'table{border-collapse:collapse}td{vertical-align:top;padding:4px 12px 4px 0}td div{margin-bottom:6px}'
        'h2{font-size:16px;margin:24px 0 4px}audio{height:32px;width:260px}</style>'
        '<h1>Pause clips</h1><p>Listen for buzz or hum in the gaps between phrases (turn the volume up). AMBE+2 and D-STAR '
        'are decoded by the AMBE-3000R, IMBE by mbevocoder. "no gate" is the encoder that buzzed, for comparison.</p>'
        + '\n'.join(rows), encoding='utf-8')
    print(f'{len(clips)} clips in {a.out / "index.html"}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
