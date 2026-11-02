#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Where does our synthesis differ from the AMBE-3000R's on the AMBE-3000R's own frames?

  python tools/quality/synth_diag.py [--rate ambe2450] [--only NAME...] [--tool mbe-cli]

Decodes the AMBE-3000R's golden frames with our decoder and compares with the AMBE-3000R's audio of the
same frames: long-term average spectrum ratio (ours / AMBE-3000R, dB per band) overall and for
all-voiced, mixed and unvoiced frames; per-frame level by voicing class; smoothed
(third-octave) spectral distance by class; and PESQ of both against the original speech.
Frames are matched one to one (frame k of both decoders is frame k of the stream, the AMBE-3000R's
output being a few ms later; the sub-frame lag is found by envelope correlation).
"""
import argparse
import subprocess
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare import find_tool, read_wav, align, FS, NFFT  # noqa: E402
from encoder_compare import parse_parms  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
BANDS = [100, 200, 300, 400, 500, 630, 800, 1000, 1250, 1600, 2000, 2500, 3150, 3800]


def band_energies(spec_pow):
    k = np.arange(NFFT // 2 + 1) * FS / NFFT
    out = np.zeros((spec_pow.shape[0], len(BANDS) - 1))
    for i in range(len(BANDS) - 1):
        sel = (k >= BANDS[i]) & (k < BANDS[i + 1])
        out[:, i] = spec_pow[:, sel].mean(axis=1)
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rate", default="ambe2450")
    ap.add_argument("--only", nargs="*", default=[])
    ap.add_argument("--tool", type=Path, default=None)
    ap.add_argument("--workdir", type=Path, default=ROOT / "build/quality")
    ap.add_argument("--pesq", action="store_true", help="also PESQ both decoders against the original")
    args = ap.parse_args()
    tool = args.tool or find_tool()
    rate = args.rate
    classes = {"voiced": [], "mixed": [], "unvoiced": []}
    ltas = {c: [np.zeros(NFFT // 2 + 1), np.zeros(NFFT // 2 + 1), 0] for c in list(classes) + ["all"]}
    level = {c: [] for c in classes}
    pesq_rows = []
    win = np.hanning(NFFT)
    files = 0
    for hw in sorted((ROOT / "testdata/golden" / rate).glob("*.hw.wav")):
        if hw.name.endswith(".ours.hw.wav"): continue
        name = hw.name[:-len(".hw.wav")]
        if args.only and name not in args.only: continue
        if name.startswith("syn_"): continue
        golden = ROOT / "corpus/golden" / rate / f"{name}.dvsi"
        if not golden.exists(): continue
        ours = args.workdir / f"{name}.{rate}.synthdiag.wav"
        parms_p = args.workdir / f"{name}.{rate}.synthdiag.parms"
        subprocess.run([str(tool), "decode", "--rate", rate, str(golden), str(ours)], check=True, capture_output=True)
        subprocess.run([str(tool), "parms", "--rate", rate, str(golden), str(parms_p)], check=True, capture_output=True)
        parms = parse_parms(parms_p)
        x, r = read_wav(ours), read_wav(hw)
        # sub-frame lag: the AMBE-3000R's output is a fixed few ms behind ours
        _, _, lag = align(x, r, max_lag=200)
        if lag > 0: x, r = x[lag:], r
        elif lag < 0: x, r = x, r[-lag:]
        n = min(len(x), len(r), 160 * len(parms)) // 160
        gx = 20 * np.log10(np.sqrt(np.mean(x[:n * 160] ** 2)) / np.sqrt(np.mean(r[:n * 160] ** 2)) + 1e-12)
        x = x * 10 ** (-gx / 20)   # global level match
        files += 1
        for k in range(1, n):
            fx, fr = x[k * 160:(k + 1) * 160], r[k * 160:(k + 1) * 160]
            rr = np.sqrt(np.mean(fr ** 2))
            if rr < 10 ** (-60 / 20): continue
            b, w0, L, g, t, ml, vv = parms[k]
            if t or L == 0: continue
            nv = int(vv[:L].sum())
            c = "voiced" if nv == L else ("unvoiced" if nv == 0 else "mixed")
            px = np.abs(np.fft.rfft(np.r_[fx * win[:160], np.zeros(NFFT - 160)])) ** 2
            pr = np.abs(np.fft.rfft(np.r_[fr * win[:160], np.zeros(NFFT - 160)])) ** 2
            for cc in (c, "all"):
                ltas[cc][0] += px; ltas[cc][1] += pr; ltas[cc][2] += 1
            level[c].append(20 * np.log10(np.sqrt(np.mean(fx ** 2)) / rr + 1e-12))
            ex, er = band_energies(px[None, :]), band_energies(pr[None, :])
            classes[c].append(float(np.sqrt(np.mean((10 * np.log10(ex + 1e-12) - 10 * np.log10(er + 1e-12)) ** 2))))
        if args.pesq:
            from pesq import pesq
            inp = read_wav(ROOT / "testdata/corpus" / f"{name}.wav")
            m = min(len(inp), len(x), len(r))
            pesq_rows.append((pesq(8000, inp[:m], x[:m], "nb"), pesq(8000, inp[:m], r[:m], "nb")))
    print(f"{files} files, {rate}, chip frames: our decoder against the chip's audio")
    print("frame class   n      level ours-chip (mean / std)   third-octave spectral distance")
    for c in classes:
        if classes[c]:
            print(f"  {c:9s} {len(classes[c]):6d}   {np.mean(level[c]):+6.2f} / {np.std(level[c]):5.2f} dB          {np.mean(classes[c]):5.2f} dB")
    k = np.arange(NFFT // 2 + 1) * FS / NFFT
    print("\nlong-term spectrum ratio ours / chip (dB), per band:")
    print("  band Hz     " + " ".join(f"{BANDS[i]:>5d}" for i in range(len(BANDS) - 1)))
    for c in ("all", "voiced", "mixed", "unvoiced"):
        sx, sr, cnt = ltas[c]
        if cnt == 0: continue
        ratio = []
        for i in range(len(BANDS) - 1):
            sel = (k >= BANDS[i]) & (k < BANDS[i + 1])
            ratio.append(10 * np.log10(sx[sel].sum() / sr[sel].sum()))
        print(f"  {c:9s}   " + " ".join(f"{v:+5.1f}" for v in ratio))
    if pesq_rows:
        a = np.array(pesq_rows)
        print(f"\nPESQ vs original: ours {a[:, 0].mean():.3f}, chip {a[:, 1].mean():.3f} ({len(a)} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
