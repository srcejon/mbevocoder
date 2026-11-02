#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Encoder versus the AMBE-3000R in the parameter domain (tier 4) and the audio domain (tier 5).

  python tools/quality/encoder_compare.py [--only NAME...] [--gain-adjust X]
        [--out corpus/baselines/encoder_vs_chip.json] [--baseline FILE]

For every corpus file: encode with mbe-cli (AMBE+2 3600x2450), dump quantiser indices and the
decoded model for both our frames and the AMBE-3000R's golden frames, align the two frame sequences
(AMBE-3000R frames are delayed by its algorithmic delay), then report:
  frame_match   fraction of frames whose 49 parameter bits are identical
  b_match[k]    per-field agreement of b0..b8
  f0_cents      median |fundamental difference| in cents (voiced frames both sides)
  voicing       fraction of harmonic bands with the same voicing decision
  gain_db       mean (ours - AMBE-3000R) decoded gamma, in dB (6.02 dB per log2 unit)
  shape_db      mean RMS difference of decoded log2Ml (dB) after removing the mean, voiced frames
  lsd_db/mcd_db audio metrics of our encode+decode against the original (tools/quality/compare.py),
                and against the AMBE-3000R's encode+decode
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare import find_tool, metrics, read_wav  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]


def parse_parms(path: Path):
    frames = []
    for line in path.read_text().splitlines():
        v = line.split()
        if len(v) < 13:
            continue
        b = [int(x) for x in v[:9]]
        w0, L, gamma, tone = float(v[9]), int(v[10]), float(v[11]), int(v[12])
        voiced = np.array([c == '1' for c in v[13][:L]])
        log2ml = np.array([float(x) for x in v[14:14 + L]])
        frames.append((b, w0, L, gamma, tone, log2ml, voiced))
    return frames


def best_offset(ours, chip, search=6):
    """Offset o such that ours[i] corresponds to AMBE-3000R[i + o]; chosen by b0 agreement."""
    best, best_score = 0, -1
    for o in range(-search, search + 1):
        score = 0
        n = 0
        for i in range(len(ours)):
            j = i + o
            if 0 <= j < len(chip):
                n += 1
                score += abs(ours[i][0][0] - chip[j][0][0]) <= 1
        if n and score / n > best_score:
            best_score, best = score / n, o
    return best, best_score


def compare_file(tool: Path, name: str, gain_adjust: float, workdir: Path, floor: float = 0.0, rate: str = "ambe2450") -> dict:
    wav = ROOT / "testdata/corpus" / f"{name}.wav"
    golden = ROOT / "corpus/golden" / rate / f"{name}.dvsi"
    ours_dvsi = workdir / f"{name}.{rate}.enc.dvsi"
    subprocess.run([str(tool), "encode", "--rate", rate, "--gain-adjust", str(gain_adjust), "--floor", str(floor), str(wav), str(ours_dvsi)],
                   check=True, capture_output=True)
    ours_p, chip_p = workdir / f"{name}.{rate}.ours.parms", workdir / f"{name}.{rate}.chip.parms"
    subprocess.run([str(tool), "parms", "--rate", rate, str(ours_dvsi), str(ours_p)], check=True, capture_output=True)
    subprocess.run([str(tool), "parms", "--rate", rate, str(golden), str(chip_p)], check=True, capture_output=True)
    ours, chip = parse_parms(ours_p), parse_parms(chip_p)
    offset, _ = best_offset(ours, chip)

    n_pairs = 0
    frame_match = 0
    b_match = np.zeros(9)
    cents = []
    voicing_agree, voicing_total = 0, 0
    gain_diff = []
    shape = []
    for i in range(len(ours)):
        j = i + offset
        if not (0 <= j < len(chip)):
            continue
        bo, w0o, Lo, go, to, mlo, vvo = ours[i]
        bc, w0c, Lc, gc, tc, mlc, vvc = chip[j]
        if tc != 0 or to != 0:
            continue
        n_pairs += 1
        if bo == bc:
            frame_match += 1
        b_match += np.array([bo[k] == bc[k] for k in range(9)])
        if w0o > 0 and w0c > 0:
            cents.append(abs(1200 * np.log2(w0o / w0c)))
        gain_diff.append(6.0206 * (go - gc))
        L = min(Lo, Lc)
        if L > 0:
            vo, vc = vvo[:L], vvc[:L]
            voicing_agree += int((vo == vc).sum())
            voicing_total += L
            ao, ac = mlo[:L], mlc[:L]
            d = 6.0206 * ((ao - ao.mean()) - (ac - ac.mean()))
            shape.append(float(np.sqrt(np.mean(d ** 2))))
    res = {
        "offset_frames": offset, "pairs": n_pairs,
        "frame_match": round(frame_match / n_pairs, 4) if n_pairs else None,
        "b_match": [round(float(x / n_pairs), 3) for x in b_match] if n_pairs else None,
        "f0_cents_median": round(float(np.median(cents)), 1) if cents else None,
        "f0_within_50c": round(float(np.mean(np.array(cents) < 50)), 3) if cents else None,
        "voicing": round(voicing_agree / voicing_total, 3) if voicing_total else None,
        "gain_db_mean": round(float(np.mean(gain_diff)), 2) if gain_diff else None,
        "gain_db_std": round(float(np.std(gain_diff)), 2) if gain_diff else None,
        "shape_db": round(float(np.mean(shape)), 2) if shape else None,
    }
    # audio domain
    ours_wav = workdir / f"{name}.{rate}.enc.wav"
    subprocess.run([str(tool), "decode", "--rate", rate, str(ours_dvsi), str(ours_wav)], check=True, capture_output=True)
    inp = read_wav(wav)
    x = read_wav(ours_wav)
    m_in = metrics(x, inp)
    res["lsd_db_vs_input"], res["mcd_db_vs_input"], res["level_db_vs_input"] = m_in["lsd_db"], m_in["mcd_db"], m_in["level_db"]
    hw = ROOT / "testdata/golden" / rate / f"{name}.hw.wav"
    if hw.exists():
        m_hw = metrics(x, read_wav(hw))
        res["lsd_db_vs_chip"], res["mcd_db_vs_chip"] = m_hw["lsd_db"], m_hw["mcd_db"]
    return res


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", nargs="*", default=[])
    ap.add_argument("--gain-adjust", type=float, default=0.0)
    ap.add_argument("--floor", type=float, default=0.0)
    ap.add_argument("--rate", default="ambe2450", choices=["ambe2450", "dstar"])
    ap.add_argument("--out", type=Path, default=ROOT / "corpus/baselines/encoder_vs_chip.json")
    ap.add_argument("--baseline", type=Path, default=None)
    ap.add_argument("--tool", type=Path, default=None)
    ap.add_argument("--workdir", type=Path, default=ROOT / "build/quality")
    ap.add_argument("--speech-only", action="store_true", help="skip synthetic signals")
    args = ap.parse_args()
    tool = args.tool or find_tool()
    args.workdir.mkdir(parents=True, exist_ok=True)
    files = {}
    for dvsi in sorted((ROOT / "corpus/golden" / args.rate).glob("*.dvsi")):
        name = dvsi.stem
        if args.only and name not in args.only:
            continue
        if name.startswith("syn_silence") or name.startswith("syn_dc"):
            continue
        if args.speech_only and name.startswith("syn_"):
            continue
        files[name] = compare_file(tool, name, args.gain_adjust, args.workdir, args.floor, args.rate)
    keys = ["frame_match", "f0_cents_median", "f0_within_50c", "voicing", "gain_db_mean", "gain_db_std", "shape_db",
            "lsd_db_vs_input", "mcd_db_vs_input", "level_db_vs_input", "lsd_db_vs_chip", "mcd_db_vs_chip"]
    agg = {}
    for k in keys:
        v = [f[k] for f in files.values() if f.get(k) is not None]
        if v:
            agg[k] = {"mean": round(float(np.mean(v)), 3), "median": round(float(np.median(v)), 3)}
    bm = [f["b_match"] for f in files.values() if f.get("b_match")]
    if bm:
        agg["b_match"] = [round(float(x), 3) for x in np.mean(np.array(bm), axis=0)]
    report = {"rate": args.rate, "gain_adjust": args.gain_adjust, "floor": args.floor, "aggregate": agg, "files": files}
    print(f"{len(files)} files, rate {args.rate}, gain_adjust {args.gain_adjust}")
    for k, a in agg.items():
        if k == "b_match":
            print(f"  b_match (b0..b8): {a}")
        else:
            print(f"  {k:20s} mean {a['mean']:8.3f}  median {a['median']:8.3f}")
    if args.baseline and args.baseline.exists():
        base = json.loads(args.baseline.read_text()).get("aggregate", {})
        for k, a in agg.items():
            if k in base and k != "b_match":
                print(f"  {k}: {a['mean']:.3f} vs baseline {base[k]['mean']:.3f} ({a['mean'] - base[k]['mean']:+.3f})")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=1))
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
