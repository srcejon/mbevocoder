#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Probe the AMBE-3000R decoder's fundamental-frequency mapping.

  python tools/quality/probe_f0.py --port COM3 --rate dstar|ambe2450 [--frames 40] [--out build/probe_f0_<rate>.json]

For every fundamental index b0 (0..119) synthesises `frames` identical frames: b0, all bands
voiced, a steady mid gain, flat spectral shape; decodes them on the AMBE-3000R and measures the
fundamental of the steady part of each segment by autocorrelation with parabolic
interpolation. Prints and stores f0 per index next to the library's own mapping.
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


def measure_f0(x: np.ndarray, fmin=55.0, fmax=450.0):
    """Autocorrelation pitch with parabolic interpolation, Hz."""
    x = x - x.mean()
    if np.max(np.abs(x)) < 1e-4:
        return None
    n = len(x)
    ac = np.correlate(x, x, "full")[n - 1:]
    ac /= ac[0] + 1e-12
    lo, hi = int(8000 / fmax), int(8000 / fmin)
    seg = ac[lo:hi]
    k = int(np.argmax(seg)) + lo
    if k <= 0 or k + 1 >= len(ac):
        return None
    y0, y1, y2 = ac[k - 1], ac[k], ac[k + 1]
    denom = (y0 - 2 * y1 + y2)
    delta = 0.5 * (y0 - y2) / denom if abs(denom) > 1e-12 else 0.0
    return 8000.0 / (k + delta)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--rate", default="dstar", choices=["dstar", "ambe2450"])
    ap.add_argument("--frames", type=int, default=40)
    ap.add_argument("--out", type=Path, default=None)
    args = ap.parse_args()
    tool, chip = find_tool(), find_ambe3000()
    work = ROOT / "build/probe"
    work.mkdir(parents=True, exist_ok=True)
    spec = work / f"probe_{args.rate}.txt"
    # one line per frame: b0..b8
    lines = []
    for b0 in range(120):
        for _ in range(args.frames):
            if args.rate == "dstar":
                lines.append(f"{b0} 15 30 256 64 0 0 0 0")    # b1=15 all voiced (D-STAR table), b2 mid gain
            else:
                lines.append(f"{b0} 0 20 256 64 0 0 0 0")
    spec.write_text("\n".join(lines) + "\n")
    dvsi = work / f"probe_{args.rate}.dvsi"
    subprocess.run([str(tool), "frames", "--rate", args.rate, str(spec), str(dvsi)], check=True, capture_output=True)
    wav = work / f"probe_{args.rate}.hw.wav"
    r = subprocess.run([str(chip), "--port", args.port, "--rate", args.rate, "decode", str(dvsi), str(wav)], capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr, file=sys.stderr)
        return 1
    ours_wav = work / f"probe_{args.rate}.ours.wav"
    subprocess.run([str(tool), "decode", "--rate", args.rate, str(dvsi), str(ours_wav)], check=True, capture_output=True)
    fs, x = wavfile.read(wav)
    _, y = wavfile.read(ours_wav)
    x = x.astype(np.float64); y = y.astype(np.float64)
    # AMBE-3000R decode lags by ~2 frames; use the middle of each segment
    seg = args.frames * 160
    result = []
    print(" b0   chip Hz   ours Hz   ratio cents")
    for b0 in range(120):
        a, b = b0 * seg + seg // 4 + 320, (b0 + 1) * seg - seg // 8 + 320
        fc = measure_f0(x[a:b])
        fo = measure_f0(y[a - 320:b - 320])
        cents = 1200 * np.log2(fc / fo) if fc and fo else None
        result.append({"b0": b0, "chip_hz": fc, "ours_hz": fo, "cents": cents})
        if b0 % 10 == 0 or b0 == 119:
            print(f"{b0:3d}  {fc if fc else 0:8.2f}  {fo if fo else 0:8.2f}  {cents if cents is not None else 0:+7.1f}")
    out = args.out or (work / f"probe_f0_{args.rate}.json")
    out.write_text(json.dumps(result, indent=1))
    c = np.array([r["cents"] for r in result if r["cents"] is not None])
    print(f"median chip/ours offset {np.median(c):+.1f} cents, std {c.std():.1f}; wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
