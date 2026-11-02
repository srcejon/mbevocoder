#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""What does the AMBE-3000R render for each AMBE+2 voicing index b1, band by band?

  python tools/quality/probe_vuv_rows.py --port COM3 [--b0 40 60 90] [--cached]

Sends 20 identical frames per (b1, b0) with a flat shape and moderate gain, decodes them on
the AMBE-3000R and with our decoder, and measures per AMBE band (harmonic l belongs to band
int(16 l f0)) the harmonic-to-noise ratio (energy at the harmonic bins against energy between
them), the band level relative to the all-voiced row, the normalised autocorrelation at the
pitch lag, and any other periodicity (strongest autocorrelation peak away from the pitch).
mbelib's AmbeVuv table has only voiced/unvoiced per band; the AMBE-3000R's rows 17..31 are not
plain noise.
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
from encoder_compare import parse_parms  # noqa: E402

HOLD = 20


def vuv_table():
    s = (ROOT / "src/decoder/ambe3600x2450_const.h").read_text()
    m = re.search(r"AmbeVuv\[32\]\[8\]\s*=\s*\{(.*?)\};", s, re.S)
    return [[int(v) for v in r.split(",")] for r in re.findall(r"\{([^{}]*)\}", m.group(1))]


def analyse(x, f0, L):
    """Per-band HNR (dB), level (dB), pitch autocorrelation; plus the strongest off-pitch periodicity."""
    S = np.abs(np.fft.rfft(x * np.hanning(len(x)), 8192)) ** 2
    fr = np.arange(len(S)) * 8000 / 8192
    bands = [[] for _ in range(8)]
    for l in range(1, L + 1):
        b = min(7, int(16 * l * f0 / 8000))
        pk = S[(fr >= l * f0 - 8) & (fr <= l * f0 + 8)].sum()
        va = S[(fr >= (l + 0.5) * f0 - 8) & (fr <= (l + 0.5) * f0 + 8)].sum()
        bands[b].append((pk, va))
    hnr, lvl = [], []
    for b in range(8):
        if not bands[b]:
            hnr.append(float("nan")); lvl.append(float("nan")); continue
        p = sum(v[0] for v in bands[b]); n = sum(v[1] for v in bands[b])
        hnr.append(10 * np.log10(p / (n + 1e-12)))
        lo, hi = b * 500, (b + 1) * 500
        lvl.append(10 * np.log10(S[(fr >= lo) & (fr < hi)].sum() + 1e-12))
    ac = np.correlate(x, x, "full")[len(x) - 1:]
    ac /= ac[0] + 1e-12
    lag = int(round(8000 / f0))
    pitch_ac = max(ac[lag - 1:lag + 2])
    other = max(((ac[l], l) for l in range(20, 400) if abs(l - lag) > 3 and abs(l - 2 * lag) > 3 and ac[l] == ac[l - 3:l + 4].max()), default=(0, 0))
    return hnr, lvl, pitch_ac, other


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--b0", nargs="+", type=int, default=[40, 60, 90])
    ap.add_argument("--cached", action="store_true")
    args = ap.parse_args()
    tool, chip = find_tool(), find_ambe3000()
    work = ROOT / "build/probe"
    work.mkdir(parents=True, exist_ok=True)
    vuv = vuv_table()
    lines = []
    for b0 in args.b0:
        for b1 in range(32):
            lines += [f"{b0} {b1} 20 256 64 0 0 0 0"] * HOLD
    spec = work / "probe_vuv_rows.txt"
    spec.write_text("\n".join(lines) + "\n")
    dvsi = work / "probe_vuv_rows.dvsi"
    subprocess.run([str(tool), "frames", "--rate", "ambe2450", str(spec), str(dvsi)], check=True, capture_output=True)
    hw = work / "probe_vuv_rows.hw.wav"
    if not (args.cached and hw.exists()):
        r = subprocess.run([str(chip), "--port", args.port, "--rate", "ambe2450", "decode", str(dvsi), str(hw)], capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stderr, file=sys.stderr); return 1
    ours = work / "probe_vuv_rows.ours.wav"
    subprocess.run([str(tool), "decode", "--rate", "ambe2450", "--gain", "0.33", str(dvsi), str(ours)], check=True, capture_output=True)
    parms_p = work / "probe_vuv_rows.parms"
    subprocess.run([str(tool), "parms", "--rate", "ambe2450", str(dvsi), str(parms_p)], check=True, capture_output=True)
    parms = parse_parms(parms_p)
    sig = {"chip": wavfile.read(hw)[1].astype(float), "ours": wavfile.read(ours)[1].astype(float)}
    for bi, b0 in enumerate(args.b0):
        base = bi * 32 * HOLD
        p = parms[base]
        f0 = p[1] * 8000 / (2 * np.pi); L = p[2]
        print(f"\nb0 {b0}: f0 {f0:.1f} Hz, L {L}; per band: HNR dB | level dB rel. row 0 | pitch autocorr | other periodicity (value@lag)")
        ref = {}
        for b1 in range(32):
            k0 = (base + b1 * HOLD + 6) * 160
            k1 = (base + b1 * HOLD + HOLD - 1) * 160
            row = f"{b1:2d} {''.join(str(v) for v in vuv[b1])}"
            for tag in ("chip", "ours"):
                hnr, lvl, pac, other = analyse(sig[tag][k0:k1], f0, L)
                if b1 == 0:
                    ref[tag] = lvl
                rel = [a - b for a, b in zip(lvl, ref[tag])]
                print(f"  {row if tag == 'chip' else ' ' * len(row)} {tag:4s} "
                      f"HNR {' '.join(f'{v:4.0f}' for v in hnr)} | lvl {' '.join(f'{v:+4.0f}' for v in rel)} | ac {pac:4.2f} | other {other[0]:.2f}@{other[1]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
