#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Does the AMBE-3000R's frame level depend on the spectral-shape indices? Measure it per index.

  python tools/quality/probe_shape_level.py --port COM3 [--rate ambe2450|dstar] [--cached]
        [--header src/decoder/<rate>_shapegain.h]

For every index of every shape family (b3..b8) the AMBE-3000R decodes 12 identical steady frames
(the other fields at the flattest shape, moderate gain, all voiced) and so does our decoder;
the level difference between the two, minus its median over all indices, is the level
offset the AMBE-3000R applies for that index and ours does not. Both decoders fix the log-mean of
the harmonic amplitudes from the gain index, so a shape's own energy is not the cause: a
non-zero offset is a level term in the AMBE-3000R's codebook entry (or its reconstruction) that
mbelib's tables lack. Written as a per-family table for the decoder.
"""
import argparse
import subprocess
import sys
from pathlib import Path

import numpy as np
from scipy.io import wavfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare import find_tool  # noqa: E402
from hil_encoder import find_ambe3000  # noqa: E402

HOLD = 12


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--rate", default="ambe2450", choices=["ambe2450", "dstar"])
    ap.add_argument("--cached", action="store_true")
    ap.add_argument("--header", type=Path, default=None)
    ap.add_argument("--b0", type=int, default=60)
    args = ap.parse_args()
    tool, chip = find_tool(), find_ambe3000()
    work = ROOT / "build/probe"
    work.mkdir(parents=True, exist_ok=True)
    dstar = args.rate == "dstar"
    widths = [9, 7, 5, 4, 4, 3] if not dstar else [9, 7, 4, 4, 4, 3]
    flat = [308, 78, 8, 14, 13, 1] if not dstar else [164, 41, 10, 8, 9, 6]
    b0, b1, b2 = args.b0, (0 if not dstar else 15), (12 if not dstar else 20)
    lines, tags = [], []
    for fam in range(6):
        step = 2 if (dstar and fam == 5) else 1
        for v in range(0, 1 << widths[fam], step):
            b = list(flat); b[fam] = v
            lines += [f"{b0} {b1} {b2} {' '.join(map(str, b))}"] * HOLD
            tags.append((fam, v))
    tag = f"{args.rate}_b0{args.b0}"
    spec = work / f"probe_shape_level_{tag}.txt"
    spec.write_text("\n".join(lines) + "\n")
    dvsi = work / f"probe_shape_level_{tag}.dvsi"
    subprocess.run([str(tool), "frames", "--rate", args.rate, str(spec), str(dvsi)], check=True, capture_output=True)
    hw = work / f"probe_shape_level_{tag}.hw.wav"
    if not (args.cached and hw.exists()):
        r = subprocess.run([str(chip), "--port", args.port, "--rate", args.rate, "decode", str(dvsi), str(hw)], capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stderr, file=sys.stderr); return 1
    ours = work / f"probe_shape_level_{tag}.ours.wav"
    subprocess.run([str(tool), "decode", "--rate", args.rate, "--gain", "0.33", str(dvsi), str(ours)], check=True, capture_output=True)
    xc = wavfile.read(hw)[1].astype(float)
    xo = wavfile.read(ours)[1].astype(float)

    def level(x, i):
        seg = x[(i * HOLD + 5) * 160:(i * HOLD + HOLD - 1) * 160]
        return 20 * np.log10(np.sqrt(np.mean(seg ** 2)) + 1e-9)
    d = np.array([level(xc, i) - level(xo, i) for i in range(len(tags))])
    clipped = np.array([np.abs(xc[(i * HOLD + 5) * 160:(i * HOLD + HOLD - 1) * 160]).max() > 30000 for i in range(len(tags))])
    med = np.median(d[~clipped])
    tables = {}
    print(f"{args.rate}: {len(tags)} indices, chip - ours level offset median {med:.2f} dB, {clipped.sum()} clipped on the chip (excluded)")
    for fam in range(6):
        sel = [i for i, t in enumerate(tags) if t[0] == fam]
        off = d[sel] - med
        ok = ~clipped[sel]
        tab = np.zeros(1 << widths[fam])
        for i, k in enumerate(sel):
            if ok[i]:
                tab[tags[k][1]] = off[i]
        if dstar and fam == 5:
            tab[1::2] = tab[0::2]
        tables[fam] = tab
        print(f"  b{fam + 3}: {len(sel)} indices, offset mean {off[ok].mean():+.2f} std {off[ok].std():.2f} min {off[ok].min():+.2f} max {off[ok].max():+.2f} dB")
    if args.header:
        name = "AmbeShapeGainDb" if not dstar else "AmbePlusShapeGainDb"
        with args.header.open("w", newline="\n") as f:
            f.write("/* SPDX-License-Identifier: GPL-3.0-or-later */\n")
            f.write(f"/* Level offset (dB) the AMBE-3000R applies per {args.rate} spectral-shape index, measured with\n"
                    f" * tools/quality/probe_shape_level.py (steady frames, chip against our decoder, median removed).\n"
                    f" * Generated; do not edit. */\n")
            for fam in range(6):
                tab = tables[fam]
                f.write(f"static const float {name}{fam + 3}[{len(tab)}] = {{\n")
                for i in range(0, len(tab), 8):
                    f.write("    " + ", ".join(f"{v:.2f}f" for v in tab[i:i + 8]) + ",\n")
                f.write("};\n")
        print(f"wrote {args.header}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
