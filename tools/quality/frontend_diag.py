#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Where does the encoder's analysis front end differ from the AMBE-3000R's?

  python tools/quality/frontend_diag.py [--rate ambe2450] [--workdir build/quality] [--only NAME...]

Uses the *.ours.parms / *.chip.parms dumps written by encoder_compare.py (decoded models of our
frames and the AMBE-3000R's frames, frame-aligned by b0 agreement) and breaks the differences down:
pitch error distribution and octave-class errors, voicing agreement per band and its
asymmetry, per-harmonic amplitude bias against frequency on frames where pitch and voicing
agree, and gain error against level.
"""
import argparse
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from encoder_compare import parse_parms, best_offset  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rate", default="ambe2450")
    ap.add_argument("--workdir", type=Path, default=ROOT / "build/quality")
    ap.add_argument("--only", nargs="*", default=[])
    args = ap.parse_args()
    cents_all, cents_by_gain = [], []
    band_agree = np.zeros(8); band_total = np.zeros(8); ours_v_chip_u = np.zeros(8); ours_u_chip_v = np.zeros(8)
    freq_bins = np.arange(0, 4001, 250); bias_sum = np.zeros(len(freq_bins) - 1); bias_sq = np.zeros(len(freq_bins) - 1); bias_n = np.zeros(len(freq_bins) - 1)
    gain_pairs = []
    matched_shape = []
    files = 0
    for ours_p in sorted(args.workdir.glob(f"*.{args.rate}.ours.parms")):
        name = ours_p.name.split(f".{args.rate}.")[0]
        if args.only and name not in args.only: continue
        if name.startswith("syn_"): continue
        chip_p = args.workdir / f"{name}.{args.rate}.chip.parms"
        if not chip_p.exists(): continue
        ours, chip = parse_parms(ours_p), parse_parms(chip_p)
        offset, _ = best_offset(ours, chip)
        files += 1
        for i in range(len(ours)):
            j = i + offset
            if not (0 <= j < len(chip)): continue
            bo, w0o, Lo, go, to, mlo, vvo = ours[i]
            bc, w0c, Lc, gc, tc, mlc, vvc = chip[j]
            if to or tc or w0o <= 0 or w0c <= 0: continue
            gain_pairs.append((6.02 * gc, 6.02 * (go - gc)))
            L = min(Lo, Lc)
            vo, vc = vvo[:L], vvc[:L]
            f0o, f0c = w0o / (2 * np.pi), w0c / (2 * np.pi)
            # voicing per band (AMBE+2 voicing table bands: int(l * 16 * f0))
            for l in range(1, L + 1):
                band = min(7, int(l * 16 * f0c))
                band_total[band] += 1
                if vo[l - 1] == vc[l - 1]: band_agree[band] += 1
                elif vo[l - 1]: ours_v_chip_u[band] += 1
                else: ours_u_chip_v[band] += 1
            both_voiced = vo.sum() >= L / 2 and vc.sum() >= L / 2
            if both_voiced:
                c = 1200 * np.log2(w0o / w0c)
                cents_all.append(c); cents_by_gain.append((6.02 * gc, c))
                # amplitude bias: same pitch (within 25 cents) and same voicing everywhere
                if abs(c) < 25 and np.array_equal(vo, vc):
                    ao, ac = mlo[:L], mlc[:L]
                    d = 6.02 * ((ao - ao.mean()) - (ac - ac.mean()))
                    matched_shape.append(float(np.sqrt(np.mean(d ** 2))))
                    for l in range(1, L + 1):
                        f = l * f0c * 8000
                        k = min(len(freq_bins) - 2, int(f // 250))
                        bias_sum[k] += d[l - 1]; bias_sq[k] += d[l - 1] ** 2; bias_n[k] += 1
    cents = np.array(cents_all); ac = np.abs(cents)
    print(f"{files} files")
    print("pitch (frames voiced on both sides, %d): within 10c %.0f%%, 25c %.0f%%, 50c %.0f%%, 100c %.0f%%; median |cents| %.0f; ours higher on %.0f%%"
          % (len(cents), np.mean(ac < 10) * 100, np.mean(ac < 25) * 100, np.mean(ac < 50) * 100, np.mean(ac < 100) * 100, np.median(ac), np.mean(cents > 0) * 100))
    oct_ = np.mean(np.abs(ac - 1200) < 60) * 100; fifth = np.mean(np.abs(ac - 702) < 60) * 100; half = np.mean(np.abs(ac - 1902) < 60) * 100
    print("   octave-class errors: octave %.1f%%, fifth %.1f%%, octave+fifth %.1f%%; other >100c: %.1f%%" % (oct_, fifth, half, np.mean(ac >= 100) * 100 - oct_ - fifth - half))
    g = np.array(cents_by_gain)
    for lo, hi in ((-100, 10), (10, 20), (20, 30), (30, 100)):
        m = (g[:, 0] >= lo) & (g[:, 0] < hi)
        if m.sum(): print("   chip gain %4d..%3d dB: %5d frames, within 50c %.0f%%" % (lo, hi, m.sum(), np.mean(np.abs(g[m, 1]) < 50) * 100))
    print("voicing per band (agreement %, ours voiced/chip unvoiced %, ours unvoiced/chip voiced %):")
    for b in range(8):
        if band_total[b]:
            print("   band %d: %.0f%%  %.0f%%  %.0f%%  (n=%d)" % (b, band_agree[b] / band_total[b] * 100, ours_v_chip_u[b] / band_total[b] * 100, ours_u_chip_v[b] / band_total[b] * 100, band_total[b]))
    print("   overall %.1f%%" % (band_agree.sum() / band_total.sum() * 100))
    print("amplitude bias (ours - chip, dB, mean-removed shapes) on %d frames with matching pitch and voicing (shape rms %.2f dB):" % (len(matched_shape), np.mean(matched_shape)))
    for k in range(len(freq_bins) - 1):
        if bias_n[k] > 50:
            m = bias_sum[k] / bias_n[k]; s = np.sqrt(bias_sq[k] / bias_n[k] - m * m)
            print("   %4d..%4d Hz: bias %+5.2f dB, std %.2f (n=%d)" % (freq_bins[k], freq_bins[k + 1], m, s, bias_n[k]))
    gp = np.array(gain_pairs)
    print("gain error (ours - chip) by chip level:")
    for lo, hi in ((-100, 0), (0, 10), (10, 20), (20, 30), (30, 100)):
        m = (gp[:, 0] >= lo) & (gp[:, 0] < hi)
        if m.sum(): print("   chip gamma %4d..%3d dB: mean %+5.2f dB, std %.2f (n=%d)" % (lo, hi, gp[m, 1].mean(), gp[m, 1].std(), m.sum()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
