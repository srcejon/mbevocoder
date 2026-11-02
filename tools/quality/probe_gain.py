#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Probe the AMBE-3000R decoder's gain mapping (b2) for a rate.

  python tools/quality/probe_gain.py --port COM3 --rate dstar|ambe2450 [--frames 30]

Sends `frames` identical all-voiced frames per gain index (steady state), measures the
output level of the AMBE-3000R and of the library decoder per index, and also measures the step
response (level of the first frames after a change) to reveal the gain prediction structure.
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
    ap.add_argument("--frames", type=int, default=30)
    args = ap.parse_args()
    tool, chip = find_tool(), find_ambe3000()
    work = ROOT / "build/probe"
    work.mkdir(parents=True, exist_ok=True)
    dstar = args.rate == "dstar"
    nb2 = 64 if dstar else 32
    b1 = 15 if dstar else 0
    b0 = 60
    lines = []
    order = list(range(0, nb2, 2)) + [nb2 // 2, 4, nb2 - 4, 8]   # sweep, then a few steps for transients
    for b2 in order:
        for _ in range(args.frames):
            lines.append(f"{b0} {b1} {b2} 256 64 0 0 0 0")
    spec = work / f"probe_gain_{args.rate}.txt"
    spec.write_text("\n".join(lines) + "\n")
    dvsi = work / f"probe_gain_{args.rate}.dvsi"
    subprocess.run([str(tool), "frames", "--rate", args.rate, str(spec), str(dvsi)], check=True, capture_output=True)
    hw = work / f"probe_gain_{args.rate}.hw.wav"
    r = subprocess.run([str(chip), "--port", args.port, "--rate", args.rate, "decode", str(dvsi), str(hw)], capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr, file=sys.stderr)
        return 1
    ours = work / f"probe_gain_{args.rate}.ours.wav"
    subprocess.run([str(tool), "decode", "--rate", args.rate, str(dvsi), str(ours)], check=True, capture_output=True)
    _, x = wavfile.read(hw)
    _, y = wavfile.read(ours)
    x = x.astype(np.float64); y = y.astype(np.float64)
    seg = args.frames * 160
    lag = 320   # AMBE-3000R output lags by ~2 frames
    rows = []
    print(" b2   chip dB   ours dB   diff   | step response chip (dB rel steady, frames 0..4) | ours")
    for k, b2 in enumerate(order):
        a = k * seg
        sx = x[a + lag + 10 * 160:a + lag + seg - 160]
        sy = y[a + 10 * 160:a + seg - 160]
        cx, cy = db(sx), db(sy)
        stepc = [round(db(x[a + lag + f * 160:a + lag + (f + 1) * 160]) - cx, 1) for f in range(5)]
        stepo = [round(db(y[a + f * 160:a + (f + 1) * 160]) - cy, 1) for f in range(5)]
        rows.append({"b2": b2, "chip_db": cx, "ours_db": cy, "step_chip": stepc, "step_ours": stepo, "order": k})
        print(f"{b2:3d}  {cx:7.2f}  {cy:7.2f}  {cx - cy:+6.2f}  | {stepc} | {stepo}")
    out = work / f"probe_gain_{args.rate}.json"
    out.write_text(json.dumps(rows, indent=1))
    sweep = [r for r in rows if r["order"] < nb2 // 2]
    c = np.array([r["chip_db"] for r in sweep]); o = np.array([r["ours_db"] for r in sweep]); b = np.array([r["b2"] for r in sweep])
    ok = (c > -60) & (o > -60)
    ac = np.polyfit(b[ok], c[ok], 1); ao = np.polyfit(b[ok], o[ok], 1)
    print(f"chip: {ac[0]:.3f} dB per index step; ours: {ao[0]:.3f} dB per index; offset at b2=0: chip {ac[1]:.1f} ours {ao[1]:.1f}")
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
