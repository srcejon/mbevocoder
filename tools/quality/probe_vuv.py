#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Probe the AMBE-3000R decoder's voicing patterns (b1) for a rate.

  python tools/quality/probe_vuv.py --port COM3 --rate dstar|ambe2450 [--frames 30]

For every voicing index: fixed fundamental, gain and shape; measures AMBE-3000R and library
output level and a harmonicity score per spectral band (energy at harmonics of f0 versus
energy between them) so each band's voiced/unvoiced state on the AMBE-3000R can be read off and
compared with the library's table.
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


def band_harmonicity(sig, f0, nbands=8):
    """Per band (equal splits of 0..4 kHz): 10 log10 (energy at harmonic bins / energy at half-harmonic bins)."""
    sig = sig - sig.mean()
    n = len(sig)
    w = np.hanning(n)
    N = 1 << int(np.ceil(np.log2(n * 4)))
    X = np.abs(np.fft.rfft(sig * w, N)) ** 2
    df = 8000 / N
    out = []
    for b in range(nbands):
        lo, hi = b * 4000 / nbands, (b + 1) * 4000 / nbands
        on = off = 0.0
        k = 1
        while k * f0 < hi:
            f = k * f0
            if f >= lo:
                i = int(round(f / df)); on += X[max(0, i - 2):i + 3].max()
                j = int(round((f + f0 / 2) / df)); off += X[max(0, j - 2):j + 3].max()
            k += 1
        out.append(10 * np.log10((on + 1e-9) / (off + 1e-9)))
    return out


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
    nb1 = 16 if dstar else 17
    b0, b2 = 60, (16 if dstar else 12)
    f0 = 8000 * (2 ** (-4.258420 - 0.021769 * b0) if dstar else None)
    if not dstar:
        import re
        s = (ROOT / "src/decoder/ambe3600x2450_const.h").read_text()
        m = re.search(r'static const float AmbeW0table\[120\]\s*=\s*\{(.*?)\};', s, re.S)
        f0 = 8000 * float(re.findall(r'-?\d+\.?\d*(?:e-?\d+)?', m.group(1))[b0])
    lines = []
    for b1 in range(nb1):
        for _ in range(args.frames):
            lines.append(f"{b0} {b1} {b2} 256 64 0 0 0 0")
    spec = work / f"probe_vuv_{args.rate}.txt"
    spec.write_text("\n".join(lines) + "\n")
    dvsi = work / f"probe_vuv_{args.rate}.dvsi"
    subprocess.run([str(tool), "frames", "--rate", args.rate, str(spec), str(dvsi)], check=True, capture_output=True)
    hw = work / f"probe_vuv_{args.rate}.hw.wav"
    r = subprocess.run([str(chip), "--port", args.port, "--rate", args.rate, "decode", str(dvsi), str(hw)], capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr, file=sys.stderr)
        return 1
    ours = work / f"probe_vuv_{args.rate}.ours.wav"
    subprocess.run([str(tool), "decode", "--rate", args.rate, str(dvsi), str(ours)], check=True, capture_output=True)
    _, x = wavfile.read(hw)
    _, y = wavfile.read(ours)
    x = x.astype(np.float64); y = y.astype(np.float64)
    seg = args.frames * 160
    rows = []
    print(f"f0 = {f0:.1f} Hz.  b1 | chip dB ours dB | chip band harmonicity (dB, 8 bands 0..4k) | ours")
    for b1 in range(nb1):
        a = b1 * seg
        sx = x[a + 320 + 8 * 160:a + 320 + seg - 160]
        sy = y[a + 8 * 160:a + seg - 160]
        hc, ho = band_harmonicity(sx, f0), band_harmonicity(sy, f0)
        rows.append({"b1": b1, "chip_db": db(sx), "ours_db": db(sy), "chip_h": hc, "ours_h": ho})
        print(f"{b1:3d} | {db(sx):6.1f} {db(sy):6.1f} | {' '.join(f'{v:5.1f}' for v in hc)} | {' '.join(f'{v:5.1f}' for v in ho)}")
    (work / f"probe_vuv_{args.rate}.json").write_text(json.dumps(rows, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
