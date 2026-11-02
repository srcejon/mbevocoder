#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Decoder quality versus the AMBE-3000R: decode the golden frames with mbe-cli and compare
the PCM with the AMBE-3000R's own decode (testdata/golden/<rate>/<name>.hw.wav).

  python tools/quality/compare.py [--rates dstar ambe2450] [--out corpus/baselines/decoder_vs_chip.json]
                                  [--tool build/.../mbe-cli.exe] [--only NAME...]

Metrics (waveform SNR is meaningless for MBE codecs, so everything is spectral, after
cross-correlation alignment and restricted to active frames of the AMBE-3000R output):
  lsd_db   mean log-spectral distance per 20 ms frame, 100..3800 Hz, 256-point Hann FFT, after gain compensation
  mcd_db   mean mel-cepstral distortion (24-band mel filterbank, 12 cepstral coefficients)
  level_db active-level difference, ours minus AMBE-3000R
  align_ms alignment offset found (ours relative to AMBE-3000R)
Writes a JSON report and prints aggregate mean/median per rate. With --baseline FILE it also
prints the change against a previous report.
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

import numpy as np
from scipy.io import wavfile
from scipy.signal import fftconvolve

ROOT = Path(__file__).resolve().parents[2]
FS = 8000
FRAME = 160
NFFT = 256


def find_tool() -> Path:
    for p in [ROOT / "build/vs2026/tools/mbe-cli/Release/mbe-cli.exe", ROOT / "build/release/tools/mbe-cli/mbe-cli",
              ROOT / "build/release/tools/mbe-cli/mbe-cli.exe", ROOT / "build/debug/tools/mbe-cli/mbe-cli"]:
        if p.exists():
            return p
    sys.exit("mbe-cli not found; build the project first")


def read_wav(p: Path) -> np.ndarray:
    fs, x = wavfile.read(p)
    assert fs == FS, f"{p}: {fs} Hz"
    return x.astype(np.float64) / 32768.0


def align(x: np.ndarray, ref: np.ndarray, max_lag: int = 4000):
    """Return (x aligned to ref, lag) using envelope cross-correlation."""
    n = min(len(x), len(ref))
    ex = np.sqrt(np.maximum(fftconvolve(x[:n] ** 2, np.ones(16) / 16, "same"), 0))
    er = np.sqrt(np.maximum(fftconvolve(ref[:n] ** 2, np.ones(16) / 16, "same"), 0))
    ex -= ex.mean(); er -= er.mean()
    c = fftconvolve(ex, er[::-1], "full")
    mid = n - 1
    lo, hi = mid - max_lag, mid + max_lag
    lag = int(np.argmax(c[lo:hi]) + lo - mid)   # positive: x is delayed relative to ref
    if lag >= 0:
        xa, ra = x[lag:], ref
    else:
        xa, ra = x, ref[-lag:]
    m = min(len(xa), len(ra))
    return xa[:m], ra[:m], lag


def frames(x: np.ndarray) -> np.ndarray:
    n = len(x) // FRAME * FRAME
    return x[:n].reshape(-1, FRAME)


def spectra(fr: np.ndarray) -> np.ndarray:
    win = np.hanning(NFFT)
    padded = np.zeros((fr.shape[0], NFFT))
    padded[:, :FRAME] = fr
    return np.abs(np.fft.rfft(padded * win, axis=1)) + 1e-9


def mel_filterbank(nbands=24, fmin=100.0, fmax=3800.0) -> np.ndarray:
    def hz2mel(f): return 2595 * np.log10(1 + f / 700)
    def mel2hz(m): return 700 * (10 ** (m / 2595) - 1)
    edges = mel2hz(np.linspace(hz2mel(fmin), hz2mel(fmax), nbands + 2))
    bins = np.floor((NFFT + 1) * edges / FS).astype(int)
    fb = np.zeros((nbands, NFFT // 2 + 1))
    for b in range(nbands):
        lo, c, hi = bins[b], bins[b + 1], bins[b + 2]
        if c == lo: c += 1
        if hi == c: hi += 1
        fb[b, lo:c] = (np.arange(lo, c) - lo) / (c - lo)
        fb[b, c:hi] = (hi - np.arange(c, hi)) / (hi - c)
    return fb


MEL = mel_filterbank()


def metrics(x: np.ndarray, ref: np.ndarray) -> dict:
    xa, ra, lag = align(x, ref)
    fx, fr = frames(xa), frames(ra)
    n = min(len(fx), len(fr))
    fx, fr = fx[:n], fr[:n]
    rms_ref = np.sqrt(np.mean(fr ** 2, axis=1)) + 1e-9
    active = rms_ref > rms_ref.max() / 10 ** (40 / 20)
    if active.sum() < 5:
        return {"align_ms": lag / 8, "active_frames": int(active.sum()), "lsd_db": None, "mcd_db": None, "level_db": None}
    lvl_x = 20 * np.log10(np.sqrt(np.mean(fx[active] ** 2)) + 1e-9)
    lvl_r = 20 * np.log10(np.sqrt(np.mean(fr[active] ** 2)) + 1e-9)
    # Gain-compensate (TIA-102.BABC style) so the spectral metrics measure shape, not level.
    sx, sr = spectra(fx[active] * 10 ** ((lvl_r - lvl_x) / 20)), spectra(fr[active])
    k = np.arange(NFFT // 2 + 1) * FS / NFFT
    band = (k >= 100) & (k <= 3800)
    lsd = np.sqrt(np.mean((20 * np.log10(sx[:, band]) - 20 * np.log10(sr[:, band])) ** 2, axis=1))
    mx = np.log(MEL @ (sx ** 2).T + 1e-12).T
    mr = np.log(MEL @ (sr ** 2).T + 1e-12).T
    from scipy.fftpack import dct
    cx, cr = dct(mx, type=2, norm="ortho", axis=1)[:, 1:13], dct(mr, type=2, norm="ortho", axis=1)[:, 1:13]
    mcd = (10 / np.log(10)) * np.sqrt(2 * np.sum((cx - cr) ** 2, axis=1))
    return {"align_ms": lag / 8, "active_frames": int(active.sum()), "lsd_db": round(float(lsd.mean()), 3),
            "mcd_db": round(float(mcd.mean()), 3), "level_db": round(float(lvl_x - lvl_r), 2)}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rates", nargs="+", default=["dstar", "ambe2450"])
    ap.add_argument("--only", nargs="*", default=[])
    ap.add_argument("--out", type=Path, default=ROOT / "corpus/baselines/decoder_vs_chip.json")
    ap.add_argument("--baseline", type=Path, default=None)
    ap.add_argument("--tool", type=Path, default=None)
    ap.add_argument("--workdir", type=Path, default=ROOT / "build/quality")
    args = ap.parse_args()
    tool = args.tool or find_tool()
    args.workdir.mkdir(parents=True, exist_ok=True)
    report = {"tool": str(tool.name), "rates": {}}
    for rate in args.rates:
        per_file = {}
        for dvsi in sorted((ROOT / "corpus/golden" / rate).glob("*.dvsi")):
            name = dvsi.stem
            if args.only and name not in args.only:
                continue
            if name.startswith("syn_silence") or name.startswith("syn_dc"):
                continue
            hw = ROOT / "testdata/golden" / rate / f"{name}.hw.wav"
            if not hw.exists():
                continue
            ours = args.workdir / f"{name}.{rate}.wav"
            r = subprocess.run([str(tool), "decode", "--rate", rate, str(dvsi), str(ours)], capture_output=True, text=True)
            if r.returncode != 0:
                print(r.stderr, file=sys.stderr)
                return 1
            per_file[name] = metrics(read_wav(ours), read_wav(hw))
        vals = {k: [m[k] for m in per_file.values() if m[k] is not None] for k in ("lsd_db", "mcd_db", "level_db")}
        agg = {k: {"mean": round(float(np.mean(v)), 3), "median": round(float(np.median(v)), 3), "max": round(float(np.max(v)), 3)}
               for k, v in vals.items() if v}
        report["rates"][rate] = {"aggregate": agg, "files": per_file}
        print(f"{rate}: {len(per_file)} files")
        for k, a in agg.items():
            print(f"  {k:9s} mean {a['mean']:7.3f}  median {a['median']:7.3f}  max {a['max']:7.3f}")
    if args.baseline and args.baseline.exists():
        base = json.loads(args.baseline.read_text())
        for rate, d in report["rates"].items():
            b = base.get("rates", {}).get(rate, {}).get("aggregate", {})
            for k, a in d["aggregate"].items():
                if k in b:
                    print(f"  {rate} {k}: mean {a['mean']:.3f} vs baseline {b[k]['mean']:.3f} ({a['mean'] - b[k]['mean']:+.3f})")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=1))
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
