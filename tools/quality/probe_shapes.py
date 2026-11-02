#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Capture the AMBE-3000R's D-STAR spectral-shape response to every shape index.

  python tools/quality/probe_shapes.py --port COM3 --b0 45 [--tag L18] [--b3-step 1] [--b4-step 1]
  python tools/quality/probe_shapes.py --analyse --tag L18 --b0 45

For each probe index X (b3 sweep with the others at a reference, then b4, b5, b6, b7, b8):
send `ref` reference frames, then `run` frames of X, and measure the per-harmonic log
amplitude over the last two X frames. Against the reference frames' own shape this gives
the single-frame shape contribution of X times a known amplification (1 + c + c^2 for
run = 3, with c the shape prediction coefficient), avoiding the steady-state blow-up of
the prediction loop. The same capture is decoded by the library for method validation
(our decoder's codebook and c = 0.65 are known). Writes build/probe/shapes2_<tag>.npz.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

import numpy as np
from scipy.io import wavfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare import find_tool  # noqa: E402
from hil_encoder import find_ambe3000  # noqa: E402

REF = (256, 64, 0, 0, 0, 0)   # b3 b4 b5 b6 b7 b8 reference


def f0_of(b0):
    return 8000 * 2 ** (-4.258420 - 0.021769 * b0)


def ltable(b0):
    s = (ROOT / "src/decoder/ambe3600x2400_const.h").read_text()
    return [int(v) for v in re.findall(r'\d+', re.search(r'AmbePlusLtable\[126\]\s*=\s*\{(.*?)\};', s, re.S).group(1))][b0]


def cases(b3_step, b4_step):
    out = []
    for b3 in range(0, 512, b3_step): out.append(("b3", b3, (b3, 64, 0, 0, 0, 0)))
    for b4 in range(0, 128, b4_step): out.append(("b4", b4, (256, b4, 0, 0, 0, 0)))
    for b5 in range(16): out.append(("b5", b5, (256, 64, b5, 0, 0, 0)))
    for b6 in range(16): out.append(("b6", b6, (256, 64, 0, b6, 0, 0)))
    for b7 in range(16): out.append(("b7", b7, (256, 64, 0, 0, b7, 0)))
    for b8 in range(0, 16, 2): out.append(("b8", b8, (256, 64, 0, 0, 0, b8)))
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM3")
    ap.add_argument("--b0", type=int, default=45)
    ap.add_argument("--b2", type=int, default=8)
    ap.add_argument("--tag", default=None)
    ap.add_argument("--ref", type=int, default=5)
    ap.add_argument("--run", type=int, default=3)
    ap.add_argument("--b3-step", type=int, default=1)
    ap.add_argument("--b4-step", type=int, default=1)
    ap.add_argument("--analyse", action="store_true")
    args = ap.parse_args()
    tag = args.tag or f"L{ltable(args.b0)}"
    work = ROOT / "build/probe"; work.mkdir(parents=True, exist_ok=True)
    tool = find_tool()
    cs = cases(args.b3_step, args.b4_step)
    period = args.ref + args.run
    if not args.analyse:
        chip = find_ambe3000()
        lines = []
        for _, _, c in cs:
            for _ in range(args.ref): lines.append(f"{args.b0} 15 {args.b2} {REF[0]} {REF[1]} {REF[2]} {REF[3]} {REF[4]} {REF[5]}")
            for _ in range(args.run): lines.append(f"{args.b0} 15 {args.b2} {c[0]} {c[1]} {c[2]} {c[3]} {c[4]} {c[5]}")
        for _ in range(args.ref + 2): lines.append(f"{args.b0} 15 {args.b2} {REF[0]} {REF[1]} {REF[2]} {REF[3]} {REF[4]} {REF[5]}")
        spec = work / f"shapes2_{tag}.txt"; spec.write_text("\n".join(lines) + "\n")
        dvsi = work / f"shapes2_{tag}.dvsi"
        subprocess.run([str(tool), "frames", "--rate", "dstar", str(spec), str(dvsi)], check=True, capture_output=True)
        r = subprocess.run([str(chip), "--port", args.port, "--rate", "dstar", "decode", str(dvsi), str(work / f"shapes2_{tag}.hw.wav")], capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stderr, file=sys.stderr); return 1
        subprocess.run([str(tool), "decode", "--rate", "dstar", str(dvsi), str(work / f"shapes2_{tag}.ours.wav")], check=True, capture_output=True)
        print(f"captured {len(lines)} frames")
    f0 = f0_of(args.b0); L = ltable(args.b0)

    def harm(seg):
        seg = seg - seg.mean(); N = 1 << int(np.ceil(np.log2(len(seg) * 16)))
        X = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), N)); df = 8000 / N
        out = []
        for k in range(1, L + 1):
            i = int(round(k * f0 / df))
            out.append(20 * np.log10(X[max(0, i - 4):i + 5].max() + 1e-9) if i + 5 < len(X) else out[-1])
        return np.array(out)

    result = {}
    for name, lag in (("chip", 320), ("ours", 0)):
        _, x = wavfile.read(work / f"shapes2_{tag}.{'hw' if name == 'chip' else 'ours'}.wav"); x = x.astype(np.float64)
        probe, refs = [], []
        for k in range(len(cs)):
            base = k * period * 160 + lag
            # reference shape: last two reference frames of this period; probe: last two run frames
            rseg = x[base + (args.ref - 2) * 160:base + args.ref * 160]
            pseg = x[base + (period - 2) * 160:base + period * 160]
            refs.append(harm(rseg)); probe.append(harm(pseg))
        result[name + "_probe"] = np.array(probe); result[name + "_ref"] = np.array(refs)
    np.savez(work / f"shapes2_{tag}.npz", L=L, f0=f0, b0=args.b0, kinds=np.array([c[0] for c in cs]), index=np.array([c[1] for c in cs]), **result)
    print(f"wrote {work / f'shapes2_{tag}.npz'}: {len(cs)} probes, L={L}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
