#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Gain step response of the AMBE-3000R decoder against ours, at unclipped levels.

  python tools/quality/probe_gain_step.py --port COM3 --rate dstar|ambe2450 [--hold 20]

Sends all-voiced flat-shape frames whose gain index steps between a base value and a set of
targets (up and down, several sizes), holding each for `hold` frames, and prints the level of
every frame after each step relative to the old and new steady states for both decoders: 1.0
means the frame is already at the new level. mbelib's decoders reconstruct the gain as
dg[b2] + 0.5 * previous gamma, which gives 0.5, 0.75, 0.875 ... in the log domain; a
different response on the AMBE-3000R means a different gain structure in its decoder.
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

import numpy as np
from scipy.io import wavfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare import find_tool  # noqa: E402
from hil_encoder import find_ambe3000  # noqa: E402


def db(x):
    return 20 * np.log10(np.sqrt(np.mean(x ** 2)) + 1e-9)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--rate", default="dstar", choices=["dstar", "ambe2450"])
    ap.add_argument("--hold", type=int, default=20)
    ap.add_argument("--cached", action="store_true", help="reuse the chip capture")
    ap.add_argument("--impulse", type=int, default=0, help="excursion length in frames (0: full steps)")
    args = ap.parse_args()
    tool, chip = find_tool(), find_ambe3000()
    work = ROOT / "build/probe"
    work.mkdir(parents=True, exist_ok=True)
    dstar = args.rate == "dstar"
    b1 = 15 if dstar else 0
    base = 16 if dstar else 12
    targets = ([20, 24, 28, 12, 8, 4, 32, 2] if dstar else [14, 16, 18, 10, 8, 6, 20, 4])
    seq = [base] * args.hold
    for t in targets:
        seq += [t] * (args.impulse or args.hold) + [base] * args.hold
    tag = f"{args.rate}_imp{args.impulse}" if args.impulse else args.rate
    spec = work / f"probe_gain_step_{tag}.txt"
    spec.write_text("".join(f"60 {b1} {b2} 256 64 0 0 0 0\n" for b2 in seq))
    dvsi = work / f"probe_gain_step_{tag}.dvsi"
    subprocess.run([str(tool), "frames", "--rate", args.rate, str(spec), str(dvsi)], check=True, capture_output=True)
    hw = work / f"probe_gain_step_{tag}.hw.wav"
    if not (args.cached and hw.exists()):
        r = subprocess.run([str(chip), "--port", args.port, "--rate", args.rate, "decode", str(dvsi), str(hw)], capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stderr, file=sys.stderr)
            return 1
    ours = work / f"probe_gain_step_{tag}.ours.wav"
    # our decoder renders these flat all-voiced frames much louder than the AMBE-3000R; keep it
    # well below full scale so the response is measured unclipped
    subprocess.run([str(tool), "decode", "--rate", args.rate, "--gain", "0.33", str(dvsi), str(ours)], check=True, capture_output=True)
    _, x = wavfile.read(hw)
    _, y = wavfile.read(ours)
    x = x.astype(np.float64); y = y.astype(np.float64)
    n = min(len(x), len(y)) // 160
    lx = np.array([db(x[k * 160:(k + 1) * 160]) for k in range(n)])
    ly = np.array([db(y[k * 160:(k + 1) * 160]) for k in range(n)])
    # sub-frame levels (40 samples) around the first step, to see where each decoder's
    # frame boundary lies relative to the capture
    k0 = args.hold
    sub = 40
    sx = [db(x[i:i + sub]) for i in range(k0 * 160 - 320, k0 * 160 + 480, sub)]
    sy = [db(y[i:i + sub]) for i in range(k0 * 160 - 320, k0 * 160 + 480, sub)]
    print("sub-frame levels (5 ms) from two frames before the first step; chip / ours:")
    print("  chip:", " ".join(f"{v:6.1f}" for v in sx))
    print("  ours:", " ".join(f"{v:6.1f}" for v in sy))
    print("\nstep      chip old/new dB  ours old/new dB | normalised response, frames 0..5 after the step (chip) | (ours)")
    rows = []
    if args.impulse:
        # excursion of `impulse` frames: level of frames -1..+5 relative to the base level, in
        # units of the full step (from the steady-state sweep of the step run, or the same run)
        ref = json.loads((work / f"probe_gain_step_{args.rate}.json").read_text()) if (work / f"probe_gain_step_{args.rate}.json").exists() else None
        print("excursion  frames -1..+5 relative to base, in units of the steady step (chip) | (ours)")
        pos = args.hold
        for t in targets:
            k = pos
            base_c, base_o = np.mean(lx[k - 8:k - 1]), np.mean(ly[k - 8:k - 1])
            step_c = step_o = None
            if ref:
                for r in ref:
                    if r["from"] == base and r["to"] == t:
                        step_c, step_o = r["chip_new"] - r["chip_old"], r["ours_new"] - r["ours_old"]
            if step_c is None:
                step_c = step_o = 1.0
            rc = [(lx[k + f] - base_c) / step_c for f in range(-1, 6)]
            ro = [(ly[k + f] - base_o) / step_o for f in range(-1, 6)]
            print(f"{base:3d}->{t:3d}x{args.impulse}  | " + " ".join(f"{v:5.2f}" for v in rc) + " | " + " ".join(f"{v:5.2f}" for v in ro))
            pos += args.impulse + args.hold
        return 0
    edges = []
    pos = args.hold
    for t in targets:
        edges.append((base, t, pos)); pos += args.hold
        edges.append((t, base, pos)); pos += args.hold
    for a, b, k in edges:
        old_c, new_c = np.mean(lx[k - 6:k - 1]), np.mean(lx[k + args.hold - 8:k + args.hold - 2])
        old_o, new_o = np.mean(ly[k - 6:k - 1]), np.mean(ly[k + args.hold - 8:k + args.hold - 2])
        rc = [(lx[k + f] - old_c) / (new_c - old_c) for f in range(6)]
        ro = [(ly[k + f] - old_o) / (new_o - old_o) for f in range(6)]
        rows.append({"from": a, "to": b, "chip_old": old_c, "chip_new": new_c, "ours_old": old_o, "ours_new": new_o, "resp_chip": rc, "resp_ours": ro})
        print(f"{a:3d}->{b:3d}   {old_c:6.1f}/{new_c:6.1f}   {old_o:6.1f}/{new_o:6.1f}  | "
              + " ".join(f"{v:5.2f}" for v in rc) + " | " + " ".join(f"{v:5.2f}" for v in ro))
    (work / f"probe_gain_step_{tag}.json").write_text(json.dumps(rows, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
