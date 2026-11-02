#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Transcoder check on the AMBE-3000R: the AMBE-3000R's own frames at one rate, transcoded by
mbe-cli to the other rate, decoded by the AMBE-3000R, and scored against the AMBE-3000R's decode of the
original frames (testdata/golden/<rate>/<name>.hw.wav) and against the original speech (LSD and
level; PESQ and ESTOI against both as well, with the AMBE-3000R's own encode + decode at the target
rate alongside for scale).

  python tools/quality/hil_transcode.py --port COM3 [--only NAME...] [--limit N]
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare import find_tool, metrics, read_wav  # noqa: E402
from hil_encoder import find_ambe3000  # noqa: E402
from perceptual import score  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--only", nargs="*", default=[])
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--out", type=Path, default=ROOT / "corpus/baselines/transcode_hil.json")
    ap.add_argument("--workdir", type=Path, default=ROOT / "build/transcode")
    args = ap.parse_args()
    tool, chip = find_tool(), find_ambe3000()
    args.workdir.mkdir(parents=True, exist_ok=True)
    files = {}
    names = [p.stem for p in sorted((ROOT / "corpus/golden/ambe2450").glob("*.dvsi")) if not p.stem.startswith("syn_")]
    if args.only:
        names = [n for n in names if n in args.only]
    if args.limit:
        names = names[: args.limit]
    for name in names:
        rec = {}
        inp = read_wav(ROOT / "testdata/corpus" / f"{name}.wav")
        for src, dst in (("ambe2450", "dstar"), ("dstar", "ambe2450")):
            golden = ROOT / "corpus/golden" / src / f"{name}.dvsi"
            hw_src = ROOT / "testdata/golden" / src / f"{name}.hw.wav"
            if not golden.exists() or not hw_src.exists():
                continue
            tc = args.workdir / f"{name}.{src}_to_{dst}.dvsi"
            hw = args.workdir / f"{name}.{src}_to_{dst}.hw.wav"
            subprocess.run([str(tool), "transcode", "--from", src, "--to", dst, str(golden), str(tc)], check=True, capture_output=True)
            r = subprocess.run([str(chip), "--port", args.port, "--rate", dst, "decode", str(tc), str(hw)], capture_output=True, text=True)
            if r.returncode != 0:
                print(r.stderr, file=sys.stderr)
                return 1
            x = read_wav(hw)
            src_dec = read_wav(hw_src)
            m_src = metrics(x, src_dec)
            m_in = metrics(x, inp)
            p_in = score(inp, x, None)
            p_src = score(src_dec, x, None)
            m_in.update({"pesq": p_in["pesq"], "estoi": p_in["estoi"]})
            m_src.update({"pesq": p_src["pesq"], "estoi": p_src["estoi"]})
            hw_dst = ROOT / "testdata/golden" / dst / f"{name}.hw.wav"
            chip_own = score(inp, read_wav(hw_dst), None)["pesq"] if hw_dst.exists() else None
            rec[f"{src}_to_{dst}"] = {"vs_chip_decode_of_source": m_src, "vs_input": m_in, "chip_own_pesq_at_target": chip_own}
            print(f"{name:32s} {src}->{dst}: vs chip decode of source LSD {m_src['lsd_db']:.2f} level {m_src['level_db']:+.2f} dB PESQ {m_src['pesq']:.2f}; "
                  f"vs input LSD {m_in['lsd_db']:.2f} PESQ {m_in['pesq']:.2f}" + (f" (AMBE-3000R at {dst} {chip_own:.2f})" if chip_own is not None else ""), flush=True)
        files[name] = rec
    agg = {}
    for key in ("ambe2450_to_dstar", "dstar_to_ambe2450"):
        for ref in ("vs_chip_decode_of_source", "vs_input"):
            for k in ("lsd_db", "level_db", "pesq", "estoi"):
                v = [f[key][ref][k] for f in files.values() if key in f and f[key][ref].get(k) is not None]
                if v:
                    agg[f"{key}.{ref}.{k}"] = {"mean": round(float(np.mean(v)), 3), "median": round(float(np.median(v)), 3), "max": round(float(np.max(v)), 3)}
        v = [f[key]["chip_own_pesq_at_target"] for f in files.values() if key in f and f[key].get("chip_own_pesq_at_target") is not None]
        if v:
            agg[f"{key}.chip_own_pesq_at_target"] = {"mean": round(float(np.mean(v)), 3), "median": round(float(np.median(v)), 3), "max": round(float(np.max(v)), 3)}
    print(f"{len(files)} files")
    for k, a in agg.items():
        print(f"  {k:50s} mean {a['mean']:7.3f}  median {a['median']:7.3f}  max {a['max']:7.3f}")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps({"aggregate": agg, "files": files}, indent=1))
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
