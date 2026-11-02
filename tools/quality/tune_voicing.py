#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tune the per-band voicing threshold multipliers against the AMBE-3000R's voicing decisions.

  python tools/quality/tune_voicing.py [--rate ambe2450] [--only NAME...]

Encodes the corpus with the analysis diagnostics dumped (mbe-cli encode --parms: per harmonic
the band's error ratio over its threshold and the amplitudes under both voicing hypotheses),
reads the AMBE-3000R's frames (mbe-cli parms on the goldens), aligns them, and searches the eight
multipliers m[b] (harmonic voiced when ratio < m[band]) that maximise agreement of the
quantised voicing pattern with the AMBE-3000R's, reproducing the quantiser's pattern choice
(the table row minimising the energy of mis-voiced harmonics). Prints the multipliers to put
in kVoicingThresholdDefault and the agreement before and after.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare import find_tool  # noqa: E402
from encoder_compare import parse_parms, best_offset  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]


def vuv_table(rate):
    hdr = (ROOT / ("src/decoder/ambe3600x2400_const.h" if rate == "dstar" else "src/decoder/ambe3600x2450_const.h")).read_text()
    m = re.search(r'static const int Ambe(?:Plus)?Vuv\[(\d+)\]\[8\] = \{(.*?)\};', hdr, re.S)
    rows = int(m.group(1))
    return np.array([int(x) for x in re.findall(r'\d+', m.group(2))]).reshape(rows, 8)


def parse_encode_dump(path):
    """Per frame: b indices, f0 (cycles/sample), ratio[L], sa_v[L], sa_u[L]."""
    out = []
    for line in path.read_text().splitlines():
        v = line.split()
        if 'A' not in v or 'V' not in v: continue
        ia, iv = v.index('A'), v.index('V')
        b = [int(x) for x in v[:ia]]
        w0 = float(v[ia + 1]); L = int(v[ia + 2])
        ep = float(v[v.index('E') + 1]) if 'E' in v else 0.0
        stats = np.array([float(x) for x in v[iv + 1:iv + 1 + 3 * L]]).reshape(L, 3)
        out.append((b, w0 / (2 * np.pi), stats[:, 0], stats[:, 1], stats[:, 2], ep))
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rate", default="ambe2450", choices=["ambe2450", "dstar"])
    ap.add_argument("--only", nargs="*", default=[])
    ap.add_argument("--workdir", type=Path, default=ROOT / "build/quality")
    ap.add_argument("--cached", action="store_true", help="reuse build/probe/vt_frames.pkl from a previous run")
    args = ap.parse_args()
    tool = find_tool()
    vuv = vuv_table(args.rate)
    nrows = 17 if args.rate == "ambe2450" else 16
    frames = []   # (ratio, sa_v, sa_u, f0, chip_pattern(8), nbands)
    for wav in sorted((ROOT / "testdata/corpus").glob("*.wav")) if not args.cached else []:
        name = wav.stem
        if name.startswith("syn_") or (args.only and name not in args.only): continue
        golden = ROOT / "corpus/golden" / args.rate / f"{name}.dvsi"
        if not golden.exists(): continue
        dvsi = args.workdir / f"{name}.{args.rate}.vt.dvsi"; dump = args.workdir / f"{name}.{args.rate}.vt.txt"
        subprocess.run([str(tool), "encode", "--rate", args.rate, "--parms", str(dump), str(wav), str(dvsi)], check=True, capture_output=True)
        ours_p = args.workdir / f"{name}.{args.rate}.vt.parms"; chip_p = args.workdir / f"{name}.{args.rate}.chip.parms"
        subprocess.run([str(tool), "parms", "--rate", args.rate, str(dvsi), str(ours_p)], check=True, capture_output=True)
        subprocess.run([str(tool), "parms", "--rate", args.rate, str(golden), str(chip_p)], check=True, capture_output=True)
        ours, chip, enc = parse_parms(ours_p), parse_parms(chip_p), parse_encode_dump(dump)
        offset, _ = best_offset(ours, chip)
        for i, (b, f0, ratio, sv, su, ep) in enumerate(enc):
            j = i + offset
            if not (0 <= j < len(chip)) or i >= len(ours): continue
            if chip[j][4] or ours[i][4]: continue
            row = chip[j][0][1]
            if row >= vuv.shape[0]: continue
            L = len(ratio)
            nb = min(8, int(L * 16 * f0) + 1)
            frames.append((ratio, sv, su, f0, vuv[row], nb))
    if args.cached:
        import pickle
        for ratio, sv, su, f0, cp, ep, orow, gc, go, f0c in pickle.load(open(ROOT / "build/probe/vt_frames.pkl", "rb")):
            frames.append((ratio, sv, su, f0, cp, min(8, int(len(ratio) * 16 * f0) + 1)))
    print(f"{len(frames)} frame pairs")

    def score(m, lowF0=0.0, boost=1.0, detail=False):
        """m: per-band multipliers; lowF0: below this analysed fundamental (Hz) a frame whose
        chosen pattern voices only the lowest one or two bands is made all-unvoiced; boost:
        extra multiplier on bands 4..7 when bands 0..3 are all voiced."""
        agree = np.zeros(8); total = np.zeros(8); rowagree = 0
        for ratio, sv, su, f0, chipPat, nb in frames:
            L = len(ratio)
            ab = np.minimum(7, ((np.arange(1, L + 1)) * 16 * f0).astype(int))
            mm = m[ab].copy()
            if boost != 1.0:
                low = ratio[ab < 4] < m[ab[ab < 4]]
                if low.size and low.all(): mm[ab >= 4] *= boost
            voiced = ratio < mm
            sa = np.where(voiced, sv, su)
            en = sa * sa
            # the quantiser's row choice: minimise energy of mis-voiced harmonics
            best, bestE = 0, None
            for r in range(nrows):
                mis = vuv[r][ab] != voiced
                e = float(np.sum(en[mis]))
                if bestE is None or e < bestE: best, bestE = r, e
            pat = vuv[best]
            if lowF0 > 0 and f0 * 8000 < lowF0 and pat[0] == 1 and pat[2:].sum() == 0:
                pat = np.zeros(8, int)
            agree[:nb] += (pat[:nb] == chipPat[:nb]); total[:nb] += 1
            rowagree += int(np.array_equal(pat, chipPat))
        if detail:
            print("   per band agreement:", " ".join("%.0f%%" % (100 * a / t) for a, t in zip(agree, total)), " rows equal %.1f%%" % (100 * rowagree / len(frames)))
        return agree.sum() / total.sum()

    m = np.ones(8)
    print("default: band agreement %.2f%%" % (100 * score(m, detail=True)))
    lowF0, boost = 85.0, 1.0
    print("low-pitch rule (85 Hz): %.2f%%" % (100 * score(m, lowF0, boost, detail=True)))
    grid = [0.35, 0.5, 0.7, 0.85, 1.0, 1.2, 1.5, 2.0, 3.0]
    for sweep in range(2):
        for b in range(8):
            best, bestS = m[b], score(m, lowF0, boost)
            for g in grid:
                mm = m.copy(); mm[b] = g
                s_ = score(mm, lowF0, boost)
                if s_ > bestS + 1e-6: best, bestS = g, s_
            m[b] = best
        bestB, bestS = boost, score(m, lowF0, boost)
        for g in (1.0, 1.5, 2.0, 3.0, 5.0):
            s_ = score(m, lowF0, g)
            if s_ > bestS + 1e-6: bestB, bestS = g, s_
        boost = bestB
        bestF, bestS = lowF0, score(m, lowF0, boost)
        for g in (0.0, 75.0, 85.0, 95.0, 105.0):
            s_ = score(m, g, boost)
            if s_ > bestS + 1e-6: bestF, bestS = g, s_
        lowF0 = bestF
        print("sweep %d: m=%s boost=%.1f lowF0=%.0f -> %.2f%%" % (sweep, np.round(m, 2).tolist(), boost, lowF0, 100 * bestS))
    print("tuned: multipliers %s, boost %.1f, low-pitch rule %.0f Hz" % (", ".join("%.2ff" % v for v in m), boost, lowF0))
    score(m, lowF0, boost, detail=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
