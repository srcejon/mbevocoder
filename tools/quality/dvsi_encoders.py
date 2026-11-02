#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The encoders on DVSI's test-vector speech: mbevocoder's encoder (and, for D-STAR and AMBE+2, the
AMBE-3000R's) against DVSI's encoded bits, each through a common decoder, scored against the
input (PESQ-NB, ESTOI) with paired differences against DVSI's encoder.

  python tools/quality/dvsi_encoders.py

D-STAR / AMBE+2 rate 33: every encoder's bits decoded by the AMBE-3000R (its decode of DVSI's
bits is identical to DVSI's, dvsi_decoders.py --chip) and by mbevocoder; P25 IMBE 7200x4400: by
mbevocoder and JMBE, plus DVSI's own decode of its bits as the reference row. Needs the DVSI
vectors in build/quality/dvsi (local measurement only: never commit or redistribute them), the
AMBE-3000R on COM3 and JMBE as for encoder_errors.py. Writes per-file scores to
build/quality/dvsi_encoders and the aggregates to corpus/baselines/dvsi_encoders.json.
"""
import argparse, json, subprocess, sys, threading
from concurrent.futures import ProcessPoolExecutor, ThreadPoolExecutor
from pathlib import Path
import numpy as np
from scipy.io import wavfile
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/quality'))
from compare import read_wav  # noqa
from perceptual import score  # noqa
from dvsi_decoders import PARTS, TV, pcm, safe_score  # noqa
OURS = (ROOT / 'build/vs2026/tools/mbe-cli/Release/mbe-cli.exe').resolve()
CHIP = (ROOT / 'build/vs2026/tools/ambe3000/Release/ambe3000.exe').resolve()
W = ROOT / 'build/quality/dvsi_encoders'
MODES = {'dstar': ('dstar', ('dvsi', 'ours', 'chip'), ('chip', 'ours')),
         'r33': ('ambe2450', ('dvsi', 'ours', 'chip'), ('chip', 'ours')),
         'p25': ('imbe7200', ('dvsi', 'ours'), ('ours', 'jmbe'))}


def score_one(j):
    mode, n, e, d = j
    src = pcm(TV / f'{n}.pcm')
    if d in ('chip', 'dvsi') and e == 'dvsi':   # DVSI's decode of its bits (the chip's is identical)
        x = pcm(TV / mode / f'{n}.pcm')
    else:
        x = read_wav(W / f'{mode}.{n}.{e}.{d}.wav')
    s = safe_score(src, x)
    return j, (s['pesq'], s['estoi'])


def run(*a):
    r = subprocess.run([str(x) for x in a], capture_output=True, text=True, timeout=1800)
    if r.returncode:
        raise RuntimeError(f'{a}: {r.stderr[-400:]}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', default='COM3')
    args = ap.parse_args()
    W.mkdir(parents=True, exist_ok=True)
    names = [n for p in PARTS.values() for n in p]
    from jmbe_crosscheck import classpath, java_bin
    cp = classpath(ROOT.parent / 'jmbe')
    jw = ROOT / 'build/jmbe'
    if not (jw / 'JmbeDecode.class').exists():
        run(java_bin('javac'), '-cp', cp, '-d', jw, ROOT / 'tools/quality/jmbe/JmbeDecode.java')
    jmbe = (java_bin('java'), f'{jw};{cp}')

    for n in names:
        w = W / f'{n}.wav'
        if not w.exists():
            wavfile.write(w, 8000, np.fromfile(TV / f'{n}.pcm', dtype='<i2'))

    def bits_of(mode, n, enc):
        return TV / mode / f'{n}.bit' if enc == 'dvsi' else W / f'{mode}.{n}.{enc}.bits'

    # encode: ours in parallel, the AMBE-3000R serially
    def enc_ours(j):
        mode, n = j
        b = bits_of(mode, n, 'ours')
        if not b.exists():
            run(OURS, 'encode', '--rate', MODES[mode][0], W / f'{n}.wav', b)
    with ThreadPoolExecutor(6) as ex:
        list(ex.map(enc_ours, [(m, n) for m in MODES for n in names]))
    chip_jobs = []
    for mode, (rate, encs, decs) in MODES.items():
        for n in names:
            if 'chip' in encs:
                chip_jobs.append(('encode', rate, W / f'{n}.wav', bits_of(mode, n, 'chip')))
            if 'chip' in decs:
                for e in encs:
                    if e != 'dvsi':   # DVSI's bits through the chip = DVSI's decode (dvsi_decoders.py --chip)
                        chip_jobs.append(('decode', rate, bits_of(mode, n, e), W / f'{mode}.{n}.{e}.chip.wav'))

    def chip_all():
        for cmd, rate, i, o in chip_jobs:
            if not o.exists():
                run(CHIP, '--port', args.port, '--rate', rate, *(['--flush', '0'] if cmd == 'encode' else []), cmd, i, o)
    t = threading.Thread(target=chip_all)
    t.start()

    def dec_sw(j):
        mode, n, e, d = j
        o = W / f'{mode}.{n}.{e}.{d}.wav'
        if o.exists():
            return
        if d == 'ours':
            run(OURS, 'decode', '--rate', MODES[mode][0], bits_of(mode, n, e), o)
        elif d == 'jmbe':
            run(jmbe[0], '-cp', jmbe[1], 'JmbeDecode', bits_of(mode, n, e), o)
    sw = [(m, n, e, d) for m, (_, encs, decs) in MODES.items() for n in names for e in encs for d in decs if d != 'chip']
    # the chip encodes must exist before their software decodes
    t.join()
    with ThreadPoolExecutor(6) as ex:
        list(ex.map(dec_sw, sw))

    jobs = [(m, n, e, d) for m, (_, encs, decs) in MODES.items() for n in names for e in encs for d in decs]
    jobs += [('p25', n, 'dvsi', 'dvsi') for n in names]
    with ProcessPoolExecutor(8) as ex:   # the native PESQ / STOI code is not thread-safe
        R = dict(ex.map(score_one, jobs))
    (W / 'scores.json').write_text(json.dumps({'/'.join(k): v for k, v in R.items()}, indent=1))

    agg = {'source': 'DVSI tv-rc.zip test vectors (https://www.dvsinc.com/get-usb/tv-rc.zip), local measurement only', 'modes': {}}
    for mode, (_, encs, decs) in MODES.items():
        out = {}
        for pname, pn in [('all', names)] + list(PARTS.items()):
            blk = {}
            for d in decs + (('dvsi',) if mode == 'p25' else ()):
                for e in (encs if d != 'dvsi' else ('dvsi',)):
                    v = np.array([R[(mode, n, e, d)] for n in pn])
                    row = {'pesq': round(float(np.nanmean(v[:, 0])), 3), 'estoi': round(float(np.nanmean(v[:, 1])), 4)}
                    if e != 'dvsi':
                        diff = v[:, 0] - np.array([R[(mode, n, 'dvsi', d)][0] for n in pn])
                        row['pesq_minus_dvsi_enc'] = round(float(np.nanmean(diff)), 3)
                        row['se'] = round(float(np.nanstd(diff, ddof=1) / np.sqrt(len(diff))), 3) if len(diff) > 1 else None
                    blk[f'{e}->{d}'] = row
            out[pname] = {'files': len(pn), 'chains': blk}
        agg['modes'][mode] = out
    (ROOT / 'corpus/baselines/dvsi_encoders.json').write_text(json.dumps(agg, indent=1) + '\n')
    for mode, parts in agg['modes'].items():
        for pname, blk in parts.items():
            print(f'{mode:5s} {pname:11s} n={blk["files"]:2d}  ' + '  '.join(
                f'{c} {v["pesq"]:.3f}/{v["estoi"]:.3f}' + (f' ({v["pesq_minus_dvsi_enc"]:+.3f}±{v["se"] or 0:.3f})' if 'pesq_minus_dvsi_enc' in v else '')
                for c, v in blk['chains'].items()))


if __name__ == '__main__':
    main()
