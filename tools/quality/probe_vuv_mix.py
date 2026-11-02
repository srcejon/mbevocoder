#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Measure the AMBE-3000R's mixed excitation per voicing index and band, and write the table.

  python tools/quality/probe_vuv_mix.py --port COM3 [--rate ambe2450|dstar] [--b0 30 45 60 75 90 105]
        [--cached] [--header src/decoder/<rate>_vuvmix.h]

For every voicing index the AMBE-3000R is sent 20 identical frames (flat shape, moderate gain) at
each fundamental, and its output is analysed per AMBE band (harmonic l is in band
int(16 l f0) for AMBE+2, the D-STAR table's 8 bands the same way): energy at the harmonic
bins and half way between them. The all-voiced index gives the harmonic-bin and leakage
energies of a pure voiced band (a, a/H), the all-unvoiced index the per-bin energy of a pure
noise band (beta), and a band whose energy is a fraction v voiced then shows
  HNR = (v a + (1-v) beta) / (v a / H + (1-v) beta)
which is solved for v. The band's total energy against v Tv + (1-v) Tu (the pure levels)
gives the level offset G. v and G are averaged over the fundamentals (weighted by the
harmonics in the band) and written as a header for the decoder (MBEVOCODER_CHIP_MEASURED).
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


def vuv_table(rate):
    if rate == "ambe2450":
        s = (ROOT / "src/decoder/ambe3600x2450_const.h").read_text()
        m = re.search(r"AmbeVuv\[32\]\[8\]\s*=\s*\{(.*?)\};", s, re.S)
    else:
        s = (ROOT / "src/decoder/ambe3600x2400_const.h").read_text()
        m = re.search(r"AmbePlusVuv\[16\]\[8\]\s*=\s*\{(.*?)\};", s, re.S)
    body = m.group(1)
    body = re.sub(r"AMBE_VUV_ROW\d+", lambda mm: {"AMBE_VUV_ROW1": "{1,1,1,1,1,1,0,0}", "AMBE_VUV_ROW13": "{1,1,0,0,0,0,0,0}", "AMBE_VUV_ROW15": "{0,0,0,0,0,0,0,0}"}[mm.group(0)], body)
    return [[int(v) for v in r.split(",")] for r in re.findall(r"\{([^{}]*)\}", body)]


def band_energies(x, f0, L):
    """Per band: (harmonic-bin energy, valley-bin energy, total energy, harmonic count)."""
    S = np.abs(np.fft.rfft(x * np.hanning(len(x)), 8192)) ** 2
    fr = np.arange(len(S)) * 8000 / 8192
    out = []
    for b in range(8):
        hs = [l for l in range(1, L + 1) if min(7, int(16 * l * f0 / 8000)) == b]
        if not hs:
            out.append(None); continue
        pk = sum(S[(fr >= l * f0 - 8) & (fr <= l * f0 + 8)].sum() for l in hs)
        va = sum(S[(fr >= (l + 0.5) * f0 - 8) & (fr <= (l + 0.5) * f0 + 8)].sum() for l in hs)
        lo, hi = (hs[0] - 0.5) * f0, (hs[-1] + 0.5) * f0
        tot = S[(fr >= lo) & (fr < hi)].sum()
        out.append((pk, va, tot, len(hs)))
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--rate", default="ambe2450", choices=["ambe2450", "dstar"])
    ap.add_argument("--b0", nargs="+", type=int, default=[30, 45, 60, 75, 90, 105])
    ap.add_argument("--cached", action="store_true")
    ap.add_argument("--header", type=Path, default=None)
    ap.add_argument("--shape", default=None, help="b3..b8 (default: the flattest steady-state shape found for the rate)")
    args = ap.parse_args()
    tool, chip = find_tool(), find_ambe3000()
    work = ROOT / "build/probe"
    work.mkdir(parents=True, exist_ok=True)
    rows = 32 if args.rate == "ambe2450" else 16
    all_v, all_u = (0, 16) if args.rate == "ambe2450" else (15, 0)
    vuv = vuv_table(args.rate)
    # the flattest steady-state shapes (build/probe/flatsearch.py): every harmonic within
    # 12 dB (AMBE+2) / 32 dB (D-STAR) of the mean, so the harmonic-to-noise ratio is
    # measurable in every band
    shape = args.shape or ("308 78 8 14 13 1" if args.rate == "ambe2450" else "164 41 10 8 9 6")
    lines = []
    for b0 in args.b0:
        for b1 in range(rows):
            lines += [f"{b0} {b1} 20 {shape}"] * HOLD
    spec = work / f"probe_vuv_mix_{args.rate}.txt"
    spec.write_text("\n".join(lines) + "\n")
    dvsi = work / f"probe_vuv_mix_{args.rate}.dvsi"
    subprocess.run([str(tool), "frames", "--rate", args.rate, str(spec), str(dvsi)], check=True, capture_output=True)
    hw = work / f"probe_vuv_mix_{args.rate}.hw.wav"
    if not (args.cached and hw.exists()):
        r = subprocess.run([str(chip), "--port", args.port, "--rate", args.rate, "decode", str(dvsi), str(hw)], capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stderr, file=sys.stderr); return 1
    parms_p = work / f"probe_vuv_mix_{args.rate}.parms"
    subprocess.run([str(tool), "parms", "--rate", args.rate, str(dvsi), str(parms_p)], check=True, capture_output=True)
    parms = parse_parms(parms_p)
    x = wavfile.read(hw)[1].astype(float)
    vsum = np.zeros((rows, 8)); gsum = np.zeros((rows, 8)); wsum = np.zeros((rows, 8))
    for bi, b0 in enumerate(args.b0):
        base = bi * rows * HOLD
        p = parms[base]
        f0 = p[1] * 8000 / (2 * np.pi); L = p[2]
        meas = []
        for b1 in range(rows):
            k0 = (base + b1 * HOLD + 6) * 160
            k1 = (base + b1 * HOLD + HOLD - 1) * 160
            meas.append(band_energies(x[k0:k1], f0, L))
        print(f"b0 {b0}: f0 {f0:.1f} Hz, L {L}")
        for b1 in range(rows):
            cells = []
            for b in range(8):
                mv, mu, mm = meas[all_v][b], meas[all_u][b], meas[b1][b]
                if not (mv and mu and mm):
                    cells.append("   -   "); continue
                a, av, Tv, n = mv; bu, _, Tu, _ = mu; pk, va, T, _ = mm
                H = a / max(av, 1e-9)
                h = pk / max(va, 1e-9)
                beta = bu / n   # per harmonic slot, same for peak and valley bins in noise
                # solve HNR = (v a' + (1-v) beta) / (v a'/H + (1-v) beta), a' = a (per slot)
                a1 = a / n
                if h >= H * 0.98:
                    v = 1.0
                elif h <= 1.05:
                    v = 0.0
                else:
                    q = beta * (h - 1) / max(a1 * (1 - h / H), 1e-12)
                    v = q / (1 + q)
                G = T / max(v * Tv + (1 - v) * Tu, 1e-12)
                vsum[b1, b] += v * n; gsum[b1, b] += 10 * np.log10(G) * n; wsum[b1, b] += n
                cells.append(f"{v:4.2f}/{10 * np.log10(G):+3.0f}")
            print(f"  {b1:2d} {''.join(str(t) for t in vuv[b1])} " + " ".join(cells))
    v_avg = np.where(wsum > 0, vsum / np.maximum(wsum, 1), 0.0)
    g_avg = np.where(wsum > 0, gsum / np.maximum(wsum, 1), 0.0)
    # Clean-up for the table: the estimator's floor on pure noise (the all-unvoiced row's
    # own v per band) is subtracted, fractions within 0.15 of 0 or 1 snap to them, level
    # offsets under 1 dB are dropped and the rest rounded to 0.5 dB; the pure rows are exact.
    floor = v_avg[all_u].copy()
    v_avg = np.clip((v_avg - floor) / np.maximum(1 - floor, 1e-6), 0, 1)
    v_avg[v_avg < 0.15] = 0.0
    v_avg[v_avg > 0.85] = 1.0
    v_avg[all_v] = 1.0; v_avg[all_u] = 0.0
    g_avg = np.where(np.abs(g_avg) < 1.0, 0.0, np.round(g_avg * 2) / 2)
    g_avg[all_v] = 0.0; g_avg[all_u] = 0.0
    print("\naverage over fundamentals: voiced fraction / level offset dB")
    for b1 in range(rows):
        print(f"  {b1:2d} {''.join(str(t) for t in vuv[b1])} " + " ".join(f"{v_avg[b1, b]:4.2f}/{g_avg[b1, b]:+3.0f}" for b in range(8)))
    if args.rate == "ambe2450":
        # Indices 18-31 are rendered on a band grid of their own (w0 = 2.81 / L, docs/decoder-quality.md
        # "Unvoiced rows 18-31"), so this probe's harmonic bins do not read them: all bands unvoiced,
        # no level offset (the values the bins gave were the misregistration, and for 20, 24, 25 and 28
        # the frame-periodic lines of a restarted noise sequence).
        v_avg[18:] = 0.0; g_avg[18:] = 0.0
    if args.header:
        name = "AmbeVuvMix" if args.rate == "ambe2450" else "AmbePlusVuvMix"
        gname = "AmbeVuvGainDb" if args.rate == "ambe2450" else "AmbePlusVuvGainDb"
        with args.header.open("w", newline="\n") as f:
            f.write("/* SPDX-License-Identifier: GPL-3.0-or-later */\n")
            f.write(f"/* Mixed excitation of the AMBE-3000R per {args.rate} voicing index and band: voiced energy\n"
                    f" * fraction and level offset (dB) against a pure voiced/unvoiced rendering, measured with\n"
                    f" * tools/quality/probe_vuv_mix.py at b0 {' '.join(str(b) for b in args.b0)}. Generated; do not edit. */\n")
            f.write(f"static const float {name}[{rows}][8] = {{\n")
            for b1 in range(rows):
                f.write("    {" + ", ".join(f"{v_avg[b1, b]:.3f}f" for b in range(8)) + "},\n")
            f.write("};\n")
            f.write(f"static const float {gname}[{rows}][8] = {{\n")
            for b1 in range(rows):
                f.write("    {" + ", ".join(f"{g_avg[b1, b]:.2f}f" for b in range(8)) + "},\n")
            f.write("};\n")
        print(f"wrote {args.header}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
