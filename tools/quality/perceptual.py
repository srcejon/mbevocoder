#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Perceptual scores (PESQ, ESTOI) for the encoder and decoder chains against the original speech.

  python tools/quality/perceptual.py [--rates ambe2450 dstar] [--only NAME...]
        [--out corpus/baselines/perceptual.json] [--baseline FILE]

LSD and MCD (compare.py) measure spectral distance and are blind to what a listener would
rank; this scores the same chains with ITU-T P.862 PESQ (narrowband, MOS-LQO 1..4.5, its own
time alignment) and ESTOI (Jensen & Taal 2016, 0..1, at the time lag that maximises it
around the envelope alignment). Chains per rate, all against testdata/corpus/<name>.wav:

  chip_chip   the AMBE-3000R's encoder and decoder        (testdata/golden/<rate>/<name>.hw.wav)
  ours_chip   our encoder, the AMBE-3000R's decoder       (testdata/golden/<rate>/<name>.ours.hw.wav)
  ours_ours   our encoder and decoder               (mbe-cli, build/quality)
  chip_ours   the AMBE-3000R's frames, our decoder        (mbe-cli, corpus/golden/<rate>/<name>.dvsi)

Needs `pip install pesq pystoi`.
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare import find_tool, read_wav, align  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
CHAINS = ["chip_chip", "ours_chip", "ours_ours", "chip_ours"]


def estoi_at(ref, x, lag):
    from pystoi import stoi
    if lag >= 0:
        xa, ra = x[lag:], ref
    else:
        xa, ra = x, ref[-lag:]
    n = min(len(xa), len(ra))
    return float(stoi(ra[:n], xa[:n], 8000, extended=True))


def score(ref, x, lag_hint=None):
    from pesq import pesq
    n = min(len(ref), len(x))
    p = float(pesq(8000, ref[:n], x[:n], "nb"))
    _, _, lag0 = align(x, ref)
    # ESTOI is sensitive to a few ms of misalignment; search around the envelope lag (or the
    # chain's typical lag) for the best value
    centre = lag0 if lag_hint is None else lag_hint
    step, span = 10, 60 if lag_hint is None else 30
    best, best_lag = -1.0, centre
    for lag in range(centre - span, centre + span + 1, step):
        v = estoi_at(ref, x, lag)
        if v > best:
            best, best_lag = v, lag
    return {"pesq": round(p, 3), "estoi": round(best, 4), "lag": best_lag}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rates", nargs="+", default=["ambe2450", "dstar"])
    ap.add_argument("--only", nargs="*", default=[])
    ap.add_argument("--out", type=Path, default=ROOT / "corpus/baselines/perceptual.json")
    ap.add_argument("--baseline", type=Path, default=None)
    ap.add_argument("--workdir", type=Path, default=ROOT / "build/quality")
    args = ap.parse_args()
    tool = find_tool()
    result = {"rates": {}}
    for rate in args.rates:
        files = {}
        lag_hint = {c: None for c in CHAINS}
        lags = {c: [] for c in CHAINS}
        for hw in sorted((ROOT / "testdata/golden" / rate).glob("*.hw.wav")):
            if hw.name.endswith(".ours.hw.wav"):
                continue
            name = hw.name[:-len(".hw.wav")]
            if args.only and name not in args.only: continue
            if name.startswith("syn_"): continue
            wav = ROOT / "testdata/corpus" / f"{name}.wav"
            if not wav.exists(): continue
            ref = read_wav(wav)
            chains = {}
            chains["chip_chip"] = hw
            ours_hw = ROOT / "testdata/golden" / rate / f"{name}.ours.hw.wav"
            if ours_hw.exists():
                chains["ours_chip"] = ours_hw
            dvsi = args.workdir / f"{name}.{rate}.enc.dvsi"
            if not dvsi.exists():
                subprocess.run([str(tool), "encode", "--rate", rate, str(wav), str(dvsi)], check=True, capture_output=True)
            oo = args.workdir / f"{name}.{rate}.enc.wav"
            subprocess.run([str(tool), "decode", "--rate", rate, str(dvsi), str(oo)], check=True, capture_output=True)
            chains["ours_ours"] = oo
            golden = ROOT / "corpus/golden" / rate / f"{name}.dvsi"
            if golden.exists():
                co = args.workdir / f"{name}.{rate}.chipframes.wav"
                subprocess.run([str(tool), "decode", "--rate", rate, str(golden), str(co)], check=True, capture_output=True)
                chains["chip_ours"] = co
            row = {}
            for chain, path in chains.items():
                row[chain] = score(ref, read_wav(path), lag_hint[chain])
                lags[chain].append(row[chain]["lag"])
                if lag_hint[chain] is None and len(lags[chain]) >= 5:
                    lag_hint[chain] = int(np.median(lags[chain]))
            files[name] = row
            print(f"{name:26s} " + "  ".join(f"{c} {row[c]['pesq']:.2f}/{row[c]['estoi']:.3f}" for c in CHAINS if c in row), flush=True)
        agg = {}
        for chain in CHAINS:
            for m in ("pesq", "estoi"):
                vals = [f[chain][m] for f in files.values() if chain in f]
                if vals:
                    agg[f"{chain}.{m}"] = {"mean": round(float(np.mean(vals)), 3), "median": round(float(np.median(vals)), 3),
                                           "min": round(float(np.min(vals)), 3)}
        result["rates"][rate] = {"aggregate": agg, "files": files}
        print(f"\n{rate}: {len(files)} files")
        print(f"  {'chain':10s} {'PESQ':>6s} {'ESTOI':>6s}")
        for chain in CHAINS:
            if f"{chain}.pesq" in agg:
                print(f"  {chain:10s} {agg[f'{chain}.pesq']['mean']:6.2f} {agg[f'{chain}.estoi']['mean']:6.3f}")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=1))
    print(f"wrote {args.out}")
    if args.baseline and args.baseline.exists():
        base = json.loads(args.baseline.read_text())
        for rate, r in result["rates"].items():
            b = base.get("rates", {}).get(rate, {}).get("aggregate", {})
            for k, v in r["aggregate"].items():
                if k in b:
                    print(f"  {rate} {k}: {b[k]['mean']:.3f} -> {v['mean']:.3f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
