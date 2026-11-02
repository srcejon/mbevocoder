#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Where do our decoder and the AMBE-3000R's disagree, and on which frames?

  python tools/quality/decoder_mismatch.py [--rate dstar] [--only NAME...]

For every corpus file, decodes two frame streams with our decoder and compares each with the
AMBE-3000R's decode of the same stream (testdata/golden/<rate>/<name>.hw.wav for the AMBE-3000R's own
frames, <name>.ours.hw.wav for ours), frame by frame: LSD and level difference per 20 ms
frame, then grouped by the frame's quantiser indices (from the parms dump). A parameter
class where the two decoders disagree on our frames but not on the AMBE-3000R's frames points at
a decoder table or rule the AMBE-3000R's encoder never exercises.
"""
import argparse
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare import find_tool, read_wav, align, frames, spectra, FS, NFFT  # noqa: E402
from encoder_compare import parse_parms  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]


def per_frame(x, ref):
    """(frame index in x, lsd, level diff dB, ref level dB) for active frames of ref."""
    xa, ra, lag = align(x, ref)
    fx, fr = frames(xa), frames(ra)
    n = min(len(fx), len(fr))
    fx, fr = fx[:n], fr[:n]
    rms_r = np.sqrt(np.mean(fr ** 2, axis=1)) + 1e-9
    rms_x = np.sqrt(np.mean(fx ** 2, axis=1)) + 1e-9
    active = rms_r > rms_r.max() / 10 ** (40 / 20)
    lvl = 20 * np.log10(np.sqrt(np.mean(fx[active] ** 2)) / np.sqrt(np.mean(fr[active] ** 2)))
    sx, sr = spectra(fx * 10 ** (-lvl / 20)), spectra(fr)
    k = np.arange(NFFT // 2 + 1) * FS / NFFT
    band = (k >= 100) & (k <= 3800)
    lsd = np.sqrt(np.mean((20 * np.log10(sx[:, band]) - 20 * np.log10(sr[:, band])) ** 2, axis=1))
    out = []
    for m in range(n):
        if not active[m]:
            continue
        src = lag + 160 * m if lag >= 0 else 160 * m   # sample index in x
        out.append((int(round(src / 160)), float(lsd[m]), float(20 * np.log10(rms_x[m] / rms_r[m]) - lvl), float(20 * np.log10(rms_r[m]))))
    return out, lag


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rate", default="dstar")
    ap.add_argument("--workdir", type=Path, default=ROOT / "build/quality")
    ap.add_argument("--only", nargs="*", default=[])
    args = ap.parse_args()
    tool = find_tool()
    rate = args.rate
    # per set: list of (lsd, level diff, ref level, parms tuple, prev parms tuple)
    sets = {"chip": [], "ours": []}
    lags = {"chip": [], "ours": []}
    for hw in sorted((ROOT / "testdata/golden" / rate).glob("*.hw.wav")):
        if hw.name.endswith(".ours.hw.wav"):
            continue
        name = hw.name[:-len(".hw.wav")]
        if args.only and name not in args.only: continue
        if name.startswith("syn_"): continue
        streams = {"chip": (ROOT / "corpus/golden" / rate / f"{name}.dvsi", hw),
                   "ours": (args.workdir / f"{name}.{rate}.enc.dvsi", ROOT / "testdata/golden" / rate / f"{name}.ours.hw.wav")}
        for tag, (dvsi, chipwav) in streams.items():
            if not (dvsi.exists() and chipwav.exists()):
                continue
            ourwav = args.workdir / f"{name}.{rate}.{tag}.ourdec.wav"
            parms_p = args.workdir / f"{name}.{rate}.{tag}.mm.parms"
            subprocess.run([str(tool), "decode", "--rate", rate, str(dvsi), str(ourwav)], check=True, capture_output=True)
            subprocess.run([str(tool), "parms", "--rate", rate, str(dvsi), str(parms_p)], check=True, capture_output=True)
            parms = parse_parms(parms_p)
            rows, lag = per_frame(read_wav(ourwav), read_wav(chipwav))
            lags[tag].append(lag)
            for k, lsd, dl, lr in rows:
                if 0 < k < len(parms):
                    sets[tag].append((lsd, dl, lr, parms[k], parms[k - 1], name, k))
    for tag in sets:
        s = sets[tag]
        if not s: continue
        print(f"{tag} frames: {len(s)} active frames, LSD ours-vs-chip decoder {np.mean([r[0] for r in s]):.2f} dB, "
              f"level diff std {np.std([r[1] for r in s]):.2f} dB, lags {min(lags[tag])}..{max(lags[tag])}")

    def report(title, keyfn, width=14, minn=30):
        print(f"\n{title:<{width}} {'chip frames':>22}   {'our frames':>22}")
        keys = sorted(set(keyfn(r) for t in sets for r in sets[t] if keyfn(r) is not None), key=lambda v: (isinstance(v, str), v))
        for key in keys:
            cells = []
            for t in ("chip", "ours"):
                rows = [r for r in sets[t] if keyfn(r) == key]
                if len(rows) < minn:
                    cells.append(f"{'':>22}")
                else:
                    cells.append(f"{len(rows):5d} {np.mean([r[0] for r in rows]):5.2f} dB {np.mean([r[1] for r in rows]):+5.2f} dB")
            print(f"{str(key):<{width}} {cells[0]}   {cells[1]}")

    def b(r, i): return r[3][0][i]
    def vcount(r): return int(r[3][6][:r[3][2]].sum())
    report("b0 (10s)", lambda r: b(r, 0) // 10 * 10 if b(r, 0) < 120 else "sil/erase")
    report("b1 voicing", lambda r: b(r, 1))
    report("voiced/L", lambda r: "unvoiced" if vcount(r) == 0 else ("all" if vcount(r) == r[3][2] else "mixed"))
    report("b2 gain", lambda r: b(r, 2) // 4 * 4)
    report("L", lambda r: r[3][2] // 4 * 4)
    report("level dB", lambda r: int(r[2] // 6 * 6))
    report("b3", lambda r: b(r, 3) // 64 * 64)
    report("b4", lambda r: b(r, 4) // 16 * 16)
    report("b5", lambda r: b(r, 5))
    report("b6", lambda r: b(r, 6))
    report("b7", lambda r: b(r, 7))
    report("b8", lambda r: b(r, 8))
    report("prev b1", lambda r: r[4][0][1])
    report("voicing chg", lambda r: "same" if r[4][0][1] == b(r, 1) else "changed")
    report("gain step", lambda r: int(np.clip((r[3][3] - r[4][3]) * 6.02 // 6 * 6, -24, 24)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
