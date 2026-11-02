#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""IMBE 7200x4400 encoder baseline: encode the corpus, decode with the library, score against
the original speech and against the AMBE+2 encoder on the same files.

  python tools/quality/imbe_baseline.py [--only NAME...] [--out corpus/baselines/encoder_imbe.json]
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


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", nargs="*", default=[])
    ap.add_argument("--out", type=Path, default=ROOT / "corpus/baselines/encoder_imbe.json")
    ap.add_argument("--workdir", type=Path, default=ROOT / "build/quality")
    args = ap.parse_args()
    tool = find_tool()
    args.workdir.mkdir(parents=True, exist_ok=True)
    files = {}
    for wav in sorted((ROOT / "testdata/corpus").glob("*.wav")):
        name = wav.stem
        if args.only and name not in args.only:
            continue
        if name.startswith("syn_"):
            continue
        dvsi = args.workdir / f"{name}.imbe7200.dvsi"
        out = args.workdir / f"{name}.imbe7200.wav"
        r = subprocess.run([str(tool), "encode", "--rate", "imbe7200", str(wav), str(dvsi)], capture_output=True, text=True)
        subprocess.run([str(tool), "decode", "--rate", "imbe7200", str(dvsi), str(out)], check=True, capture_output=True)
        inp = read_wav(wav); x = read_wav(out)
        m = metrics(x, inp)
        rec = {"imbe_vs_input": m, "range_errors": int(r.stderr.split("out-of-range")[0].split(",")[-1].strip() or 0) if "out-of-range" in r.stderr else None}
        ambe = args.workdir / f"{name}.enc.wav"
        if ambe.exists():
            rec["ambe2450_vs_input"] = metrics(read_wav(ambe), inp)
        files[name] = rec
    agg = {}
    for key in ("imbe_vs_input", "ambe2450_vs_input"):
        for k in ("lsd_db", "mcd_db", "level_db"):
            v = [f[key][k] for f in files.values() if key in f and f[key][k] is not None]
            if v:
                agg[f"{key}.{k}"] = {"mean": round(float(np.mean(v)), 3), "median": round(float(np.median(v)), 3), "max": round(float(np.max(v)), 3)}
    print(f"{len(files)} files")
    for k, a in agg.items():
        print(f"  {k:28s} mean {a['mean']:7.3f}  median {a['median']:7.3f}  max {a['max']:7.3f}")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps({"aggregate": agg, "files": files}, indent=1))
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
