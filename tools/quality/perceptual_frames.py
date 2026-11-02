#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A per-frame perceptual disturbance, to locate where our decoder loses PESQ against the AMBE-3000R's.

  python tools/quality/perceptual_frames.py [--rate ambe2450] [--only NAME...] [--top 12]

PESQ gives one number per file; this reimplements its core roughly (32 ms frames at 16 ms
hop, 24 Bark-spaced bands from 100 to 3700 Hz, global gain and frequency-response
compensation of the degraded signal, Zwicker loudness P^0.23, symmetric disturbance
|Ld - Lr| above a dead zone of a quarter of the smaller loudness, and an asymmetric
disturbance that weights bands where the degraded signal is louder than the reference)
and applies it to both decoders' renderings of the AMBE-3000R's frames against the original
speech, frame by frame. Reports the disturbance by frame class (voicing, transition,
level) for both decoders and the frames where ours is worst relative to the AMBE-3000R's.
"""
import argparse
import subprocess
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare import find_tool, read_wav, align  # noqa: E402
from encoder_compare import parse_parms  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
N = 256
HOP = 128
NB = 24


def bark(f):
    return 13 * np.arctan(0.00076 * f) + 3.5 * np.arctan((f / 7500) ** 2)


def filterbank():
    edges_b = np.linspace(bark(100), bark(3700), NB + 1)
    k = np.arange(N // 2 + 1) * 8000 / N
    kb = bark(k)
    fb = np.zeros((NB, N // 2 + 1))
    for b in range(NB):
        sel = (kb >= edges_b[b]) & (kb < edges_b[b + 1])
        if sel.sum() == 0:
            sel[np.argmin(np.abs(kb - 0.5 * (edges_b[b] + edges_b[b + 1])))] = True
        fb[b, sel] = 1.0 / sel.sum()
    return fb


FB = filterbank()
WIN = np.hanning(N)


def band_powers(x):
    nfr = (len(x) - N) // HOP + 1
    frames = np.stack([x[i * HOP:i * HOP + N] * WIN for i in range(nfr)])
    S = np.abs(np.fft.rfft(frames, axis=1)) ** 2
    return S @ FB.T   # (frames, bands)


def loudness(P):
    return np.power(np.maximum(P, 1e-12), 0.23)


def disturbance(ref, deg):
    """Per-frame symmetric and asymmetric disturbance of deg against ref (aligned, equal length)."""
    Pr, Pd = band_powers(ref), band_powers(deg)
    n = min(len(Pr), len(Pd)); Pr, Pd = Pr[:n], Pd[:n]
    er = Pr.sum(axis=1)
    active = er > er.max() * 10 ** (-40 / 10)
    # global gain, then frequency-response compensation of the degraded signal (clipped +-20 dB)
    g = Pr[active].sum() / max(Pd[active].sum(), 1e-12)
    Pd = Pd * g
    resp = Pr[active].mean(axis=0) / np.maximum(Pd[active].mean(axis=0), 1e-12)
    resp = np.clip(resp, 0.01, 100.0)
    Pd = Pd * resp
    Lr, Ld = loudness(Pr), loudness(Pd)
    d = Ld - Lr
    dead = 0.25 * np.minimum(Lr, Ld)
    dz = np.where(d > dead, d - dead, np.where(d < -dead, d + dead, 0.0))
    sym = np.abs(dz).sum(axis=1)
    asym_w = np.power((Pd + 50) / (Pr + 50), 1.2)
    asym_w = np.where(asym_w < 3, 0.0, np.minimum(asym_w, 12.0))
    asym = (np.abs(dz) * asym_w).sum(axis=1)
    return sym, asym, active, Pr, Pd


def best_lag(ref, x):
    _, _, lag0 = align(x, ref)
    best = None
    for lag in range(lag0 - 60, lag0 + 61, 8):
        xa = x[lag:] if lag >= 0 else x
        ra = ref if lag >= 0 else ref[-lag:]
        m = min(len(xa), len(ra))
        s, _, act, _, _ = disturbance(ra[:m], xa[:m])
        v = s[act].mean()
        if best is None or v < best[0]:
            best = (v, lag)
    return best[1]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rate", default="ambe2450")
    ap.add_argument("--only", nargs="*", default=[])
    ap.add_argument("--workdir", type=Path, default=ROOT / "build/quality")
    ap.add_argument("--top", type=int, default=12)
    args = ap.parse_args()
    tool = find_tool()
    rate = args.rate
    rows = []   # (name, frame, cls, dyn, level, sym_ours, sym_chip, asym_ours, asym_chip)
    tot = {"ours": [0.0, 0.0, 0], "chip": [0.0, 0.0, 0]}
    for hw in sorted((ROOT / "testdata/golden" / rate).glob("*.hw.wav")):
        if hw.name.endswith(".ours.hw.wav"): continue
        name = hw.name[:-len(".hw.wav")]
        if args.only and name not in args.only: continue
        if name.startswith("syn_"): continue
        golden = ROOT / "corpus/golden" / rate / f"{name}.dvsi"
        if not golden.exists(): continue
        ours = args.workdir / f"{name}.{rate}.pf.wav"
        parms_p = args.workdir / f"{name}.{rate}.pf.parms"
        subprocess.run([str(tool), "decode", "--rate", rate, str(golden), str(ours)], check=True, capture_output=True)
        subprocess.run([str(tool), "parms", "--rate", rate, str(golden), str(parms_p)], check=True, capture_output=True)
        parms = parse_parms(parms_p)
        inp = read_wav(ROOT / "testdata/corpus" / f"{name}.wav")
        sig = {"ours": read_wav(ours), "chip": read_wav(hw)}
        res = {}
        for tag, x in sig.items():
            lag = best_lag(inp, x)
            xa = x[lag:] if lag >= 0 else x
            ra = inp if lag >= 0 else inp[-lag:]
            m = min(len(xa), len(ra))
            sym, asym, act, Pr, Pd = disturbance(ra[:m], xa[:m])
            res[tag] = (sym, asym, act, lag)
            tot[tag][0] += sym[act].sum(); tot[tag][1] += asym[act].sum(); tot[tag][2] += int(act.sum())
        so, ao, act, lag_o = res["ours"]; sc, ac, _, lag_c = res["chip"]
        n = min(len(so), len(sc))
        # frame index in the codec stream for analysis frame i: the analysis frame is centred
        # at (i * HOP + N/2) samples into the aligned decoder output, which starts lag samples
        # into the decoder's stream
        lv = np.array([20 * np.log10(np.sqrt(np.mean(inp[k * 160:(k + 1) * 160] ** 2)) + 1e-9) for k in range(len(inp) // 160)])
        for i in range(n):
            if not act[i]: continue
            k = int((i * HOP + N // 2 + max(lag_o, 0)) // 160)
            if k < 2 or k >= len(parms) - 2: continue
            b, w0, L, g, t, ml, vv = parms[k]
            if t or L == 0: continue
            nv = int(vv[:L].sum())
            cls = "voiced" if nv == L else ("unvoiced" if nv == 0 else "mixed")
            bp, bn = parms[k - 1], parms[k + 1]
            chg = "vuv-change" if (bp[0][1] != b[1] or bn[0][1] != b[1]) else "steady"
            kk = int((i * HOP + N // 2 + max(lag_c, 0)) // 160)
            lvl = lv[min(kk, len(lv) - 1)] - lv.max()
            rows.append((name, k, cls, chg, lvl, so[i], sc[i], ao[i], ac[i]))
    for tag in tot:
        print(f"{tag}: symmetric disturbance {tot[tag][0] / tot[tag][2]:.3f}, asymmetric {tot[tag][1] / tot[tag][2]:.3f} per active frame ({tot[tag][2]} frames)")
    print("\nby class (frames, symmetric ours / chip, asymmetric ours / chip):")
    def report(keyfn, title):
        print(f"  {title}")
        for key in sorted(set(keyfn(r) for r in rows)):
            sel = [r for r in rows if keyfn(r) == key]
            if len(sel) < 20: continue
            print(f"    {str(key):12s} {len(sel):6d}  sym {np.mean([r[5] for r in sel]):6.3f} / {np.mean([r[6] for r in sel]):6.3f}"
                  f"  asym {np.mean([r[7] for r in sel]):6.3f} / {np.mean([r[8] for r in sel]):6.3f}"
                  f"  ours-chip sym {np.mean([r[5] - r[6] for r in sel]):+6.3f}")
    report(lambda r: r[2], "voicing")
    report(lambda r: r[3], "transition")
    report(lambda r: (r[2], r[3]), "voicing x transition")
    report(lambda r: int(r[4] // 6 * 6), "level (dB rel. max)")
    rows.sort(key=lambda r: (r[5] - r[6]), reverse=True)
    print(f"\ntop {args.top} frames where ours is worse than the chip (name, codec frame, class, transition, level, sym ours/chip):")
    for r in rows[:args.top]:
        print(f"  {r[0]:24s} {r[1]:5d} {r[2]:8s} {r[3]:10s} {r[4]:6.1f} dB  {r[5]:.3f} / {r[6]:.3f}")
    worst = np.array([r[5] - r[6] for r in rows])
    print(f"\nshare of the total excess held by the worst 10 % of frames: {worst[:len(worst) // 10].sum() / max(worst.sum(), 1e-9) * 100:.0f} %")
    return 0


if __name__ == "__main__":
    sys.exit(main())
