#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The decoders on DVSI's own test vectors: mbelib 1.x, mbelib-neo (the latest release, 2.6.5)
and mbevocoder decode DVSI's encoded bits (D-STAR, AMBE+2 rate 33, P25 IMBE
7200x4400), scored against the input speech (PESQ-NB, ESTOI) and against DVSI's decode of the
same bits (LSD, and PESQ with DVSI's decode as the reference). DVSI's decode is the reference row.

  python tools/quality/dvsi_decoders.py [--chip]

Speech: the 27 speech vectors, split as mbelib-neo's dvsi_scoreboard.py does (development,
validation, final); errors: dam_e{1,2,5,10}_hd (hard bits) and _sd (4-bit soft values, two per
byte, high nibble first, 0 = strong 0 .. 15 = strong 1), the soft ones through the soft-decision
APIs (mbevocoder and mbelib-neo only). --chip also decodes the D-STAR and rate-33 vectors on the
AMBE-3000R (COM3) and reports whether its output is identical to DVSI's.

Needs the DVSI vectors in build/quality/dvsi (mbelib-neo's tools/quality/fetch_dvsi_vectors.py;
local measurement only: never commit or redistribute them), the tools of decoder_lineage_all.py,
and tools/quality/dvsi_harness built in build/dvsi_harness against an mbelib-neo 2.6.5 checkout:
  cmake -S tools/quality/dvsi_harness -B build/dvsi_harness -DNEO_DIR=<checkout built in NEO_DIR/b>
  cmake --build build/dvsi_harness --config Release Writes per-file scores to build/quality/dvsi_decoders
and the aggregates to corpus/baselines/dvsi_decoders.json.
"""
import argparse, json, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import numpy as np
from scipy.io import wavfile
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/quality'))
from compare import read_wav, metrics  # noqa
from perceptual import score  # noqa
from decoder_lineage import decode_mbelib1, GAIN  # noqa
OURS = (ROOT / 'build/vs2026/tools/mbe-cli/Release/mbe-cli.exe').resolve()
M1 = (ROOT / 'build/vs2026/tools/mbelib1-decode/Release/mbelib1-decode.exe').resolve()
# tools/quality/dvsi_harness built into build/dvsi_harness against mbelib-neo 2.6.5 (NEO_DIR)
HARNESS = ROOT / 'build/dvsi_harness/Release'
NEO = (HARNESS / 'neodec.exe').resolve()
SOFT = (HARNESS / 'dvsisoft.exe').resolve()
CHIP = (ROOT / 'build/vs2026/tools/ambe3000/Release/ambe3000.exe').resolve()
TV = ROOT / 'build/quality/dvsi/tv-rc'
W = ROOT / 'build/quality/dvsi_decoders'
MODES = {'dstar': 'dstar', 'r33': 'ambe2450', 'p25': 'imbe7200'}
PARTS = {
    'development': ['dam', 'dam10', 'dam20', 'dam40', 'dam80', 'dam_10ov', 'damcar', 'lvl', 'clean', 'noisy',
                    'fambf22c', 'fambm22a', 't01', 't02', 'tia11', 'tambf22a', 'ucarm15a'],
    'validation': ['t03', 't04', 'tambf22b', 'tambf32b', 'tambf32e', 'ucarf15a', 'ucarm20a'],
    'final': ['irstia', 'p01mirs', 'mark'],
}
ERRS = [1, 2, 5, 10]


def pcm(p):
    return np.fromfile(p, dtype='<i2').astype(np.float64) / 32768.0


def f32(p, pcm16=True, gain=GAIN):
    """Raw float decoder output at mbe-cli's 16-bit scale; pcm16 rounds and clips as mbe-cli does.
    mbelib 1.x and mbelib-neo put out 25-35 dB less at that scale, and rounding would bury quiet
    vectors (dam40, dam80) in quantisation noise, so they are scored unrounded (the scores are
    level-free: PESQ and LSD normalise the level, ESTOI ignores it)."""
    x = np.fromfile(p, dtype=np.float32).astype(np.float64) * gain
    return (np.clip(np.round(x), -32768, 32767) if pcm16 else x) / 32768.0


def decode(dec, mode, bits, stem):
    """Decoded speech of one bit file (cached in W)."""
    rate = MODES[mode]
    if dec == 'dvsi':
        return pcm(bits.with_suffix('.pcm'))
    if dec == 'mbelib1':
        w = W / f'{stem}.mbelib1.wav'
        if not w.exists():
            decode_mbelib1(M1, OURS, bits, rate, w)
        return f32(w.with_suffix('.f32'), False)
    if dec == 'ours':
        w = W / f'{stem}.{dec}.wav'
        if not w.exists():
            subprocess.run([str(OURS), 'decode', '--rate', rate, str(bits), str(w)], check=True, capture_output=True)
        return read_wav(w)
    if dec == 'neo':
        txt, raw = W / f'{stem}.fr.txt', W / f'{stem}.neo.f32'
        if not raw.exists():
            subprocess.run([str(OURS), 'frbits', '--rate', rate, str(bits), str(txt)], check=True, capture_output=True)
            subprocess.run([str(NEO), rate, str(txt), str(raw)], check=True, capture_output=True)
        return f32(raw, False)
    if dec in ('ours_soft', 'neo_soft'):
        txt, raw = W / f'{stem}.soft.txt', W / f'{stem}.ours_soft.f32'
        if not raw.exists():
            subprocess.run([str(SOFT), rate, str(bits), str(raw), str(txt)], check=True, capture_output=True)
        if dec == 'ours_soft':
            return f32(raw, True, 7.0)   # ours: x7 is the 16-bit scale, the AMBE-3000R's level
        nraw = W / f'{stem}.neo_soft.f32'
        if not nraw.exists():
            subprocess.run([str(NEO), rate, str(txt), str(nraw), 'soft'], check=True, capture_output=True)
        return f32(nraw, False)
    raise ValueError(dec)


def muted_fraction(src, x):
    """Of the input's active 20 ms frames (within 40 dB of its loudest), the share silent in x."""
    n = min(len(src), len(x)) // 160
    ls = 10 * np.log10(np.mean(src[:n * 160].reshape(n, 160) ** 2, axis=1) + 1e-12)
    lx = 10 * np.log10(np.mean(x[:n * 160].reshape(n, 160) ** 2, axis=1) + 1e-12)
    act = ls > ls.max() - 40
    return float(np.mean(lx[act] < -70))


def safe_score(src, x):
    """PESQ / ESTOI against the input, NaN where PESQ finds no utterance (a muted decode)."""
    from pesq import NoUtterancesError
    try:
        s = score(src, x, None)
        return {'pesq': s['pesq'], 'estoi': s['estoi']}
    except NoUtterancesError:
        return {'pesq': float('nan'), 'estoi': float('nan')}


def one(job):
    mode, name, src_name, bits, decs = job
    src = pcm(TV / f'{src_name}.pcm')
    ref = pcm(bits.with_suffix('.pcm'))
    stem = f'{mode}.{bits.stem}'
    out = {}
    for d in decs:
        x = decode(d, mode, bits, stem)
        r = safe_score(src, x)
        if d != 'dvsi':
            m = metrics(x, ref)
            n = min(len(ref), len(x))
            from pesq import pesq
            pd = float('nan')
            # the PESQ library crashes (not raises) on a near-silent reference: DVSI's D-STAR decode
            # at 10 % errors is mostly muted
            if muted_fraction(src, ref) < 0.5:
                from pesq import NoUtterancesError
                try:
                    pd = round(float(pesq(8000, ref[:n], x[:n], 'nb')), 3)
                except NoUtterancesError:
                    pass
            r.update(lsd=m['lsd_db'], level=m['level_db'], pesq_dvsi=pd)
        out[d] = r
    return (mode, name), out


def chip_check():
    """AMBE-3000R decode of every D-STAR / rate-33 bit file against DVSI's decode."""
    res = {}
    for mode, rate in (('dstar', 'dstar'), ('r33', 'ambe2450')):
        same = diff = 0
        names = [n for p in PARTS.values() for n in p] + [f'dam_e{e}_hd' for e in ERRS]
        for n in names:
            bits = TV / mode / f'{n}.bit'
            w = W / f'{mode}.{n}.chip.wav'
            if not w.exists():
                subprocess.run([str(CHIP), '--port', 'COM3', '--rate', rate, 'decode', str(bits), str(w)], check=True, capture_output=True)
            _, x = wavfile.read(w)
            ref = np.fromfile(bits.with_suffix('.pcm'), dtype='<i2')
            k = min(len(x), len(ref))
            if len(x) == len(ref) and np.array_equal(x[:k], ref[:k]):
                same += 1
            else:
                diff += 1
                print(f'  chip != dvsi: {mode} {n} ({len(x)} vs {len(ref)} samples, {np.mean(x[:k] != ref[:k]):.4f} differ)')
        res[mode] = {'identical': same, 'different': diff}
        print(f'chip {mode}: {same} of {same + diff} decodes identical to DVSI\'s')
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--chip', action='store_true', help='also check the AMBE-3000R against DVSI (COM3)')
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--job', type=int, help=argparse.SUPPRESS)   # internal: score one job in this process
    args = ap.parse_args()
    W.mkdir(parents=True, exist_ok=True)
    hard = ['dvsi', 'mbelib1', 'neo', 'ours']
    jobs = []
    for mode in MODES:
        for part in PARTS.values():
            for n in part:
                jobs.append((mode, n, n, TV / mode / f'{n}.bit', hard))
        for e in ERRS:
            jobs.append((mode, f'e{e}_hd', 'dam', TV / mode / f'dam_e{e}_hd.bit', hard))
            jobs.append((mode, f'e{e}_sd', 'dam', TV / mode / f'dam_e{e}_sd.bit', ['dvsi', 'neo_soft', 'ours_soft']))
    if args.job is not None:
        key, out = one(jobs[args.job])
        (W / f'job{args.job}.json').write_text(json.dumps(out))
        return

    # one process per job: the native scoring code is not thread-safe, and a crash names its input
    def run_job(k):
        f = W / f'job{k}.json'
        if not f.exists():
            r = subprocess.run([sys.executable, __file__, '--job', str(k)], capture_output=True, text=True)
            if r.returncode or not f.exists():
                print(f'job {k} {jobs[k][0]} {jobs[k][1]} failed ({r.returncode}): {r.stderr[-300:]}', flush=True)
                return None
        return (jobs[k][0], jobs[k][1]), json.loads(f.read_text())
    with ThreadPoolExecutor(args.jobs) as ex:
        R = dict(x for x in ex.map(run_job, range(len(jobs))) if x)
    if len(R) < len(jobs):
        sys.exit(f'{len(jobs) - len(R)} jobs failed')
    per = {f'{m}/{n}': v for (m, n), v in R.items()}
    (W / 'scores.json').write_text(json.dumps(per, indent=1))

    agg = {'source': 'DVSI tv-rc.zip test vectors (https://www.dvsinc.com/get-usb/tv-rc.zip), local measurement only',
           'speech': {}, 'errors': {}}
    for mode in MODES:
        for pname, names in [('all', [n for p in PARTS.values() for n in p])] + list(PARTS.items()):
            rows = {}
            for d in hard:
                vals = [R[(mode, n)][d] for n in names]
                rows[d] = {k: (round(float(np.nanmean([v[k] if v[k] is not None else np.nan for v in vals])), 3)) for k in vals[0]}
                if d != 'dvsi':
                    diff = np.array([R[(mode, n)][d]['pesq'] - R[(mode, n)]['dvsi']['pesq'] for n in names])
                    rows[d]['pesq_minus_dvsi'] = round(float(np.nanmean(diff)), 3)
                    rows[d]['pesq_minus_dvsi_se'] = round(float(np.nanstd(diff, ddof=1) / np.sqrt(len(diff))), 3) if len(diff) > 1 else None
            agg['speech'].setdefault(mode, {})[pname] = {'files': len(names), 'decoders': rows}
        for e in ERRS:
            hd, sd = R[(mode, f'e{e}_hd')], R[(mode, f'e{e}_sd')]
            agg['errors'].setdefault(mode, {})[f'{e}%'] = {**{d: hd[d] for d in hard}, 'dvsi_soft': sd['dvsi'],
                                                           'neo_soft': sd['neo_soft'], 'ours_soft': sd['ours_soft']}
    if args.chip:
        agg['chip_vs_dvsi'] = chip_check()
    (ROOT / 'corpus/baselines/dvsi_decoders.json').write_text(json.dumps(agg, indent=1) + '\n')

    for mode in MODES:
        for pname, blk in agg['speech'][mode].items():
            print(f'{mode:5s} {pname:11s} n={blk["files"]:2d}  ' + '  '.join(
                f'{d} {v["pesq"]:.3f}/{v["estoi"]:.3f}' + (f' lsd {v["lsd"]:.2f} pd {v["pesq_dvsi"]:.2f}' if d != 'dvsi' else '')
                for d, v in blk['decoders'].items()))
        for e, row in agg['errors'][mode].items():
            print(f'{mode:5s} err {e:4s}  ' + '  '.join(f'{d} {v["pesq"]:.3f}/{v["estoi"]:.3f}' for d, v in row.items()))


if __name__ == '__main__':
    main()
