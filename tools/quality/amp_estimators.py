#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Which harmonic-amplitude estimator comes closest to the AMBE-3000R's shapes?

  python tools/quality/amp_estimators.py [--rate ambe2450] [--files NAME...] [--centre N]

The encoder's remaining gap sits in the harmonic amplitudes (docs/encoder-quality.md). This scores
candidate estimators, computed in Python from the original speech, against the AMBE-3000R's
decoded log2 amplitudes for the same frame (build/quality/<name>.<rate>.chip.parms from
encoder_compare.py), on fully voiced frames, at the AMBE-3000R's own fundamental (so the
harmonic frequencies are identical and only the estimator differs). The measure is the rms
of the mean-removed shape difference in dB, which includes the AMBE-3000R's own quantisation
noise as a floor common to every estimator; lower is closer. Our encoder's shapes (from
its quantised output, the same measure) are printed for reference. The analysis window
centre relative to the frame is scanned first with one estimator.
"""
import argparse
import sys
from pathlib import Path

import numpy as np
from scipy.io import wavfile
from scipy.signal.windows import kaiser, hann, blackmanharris

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from encoder_compare import parse_parms, best_offset  # noqa: E402

FS = 8000
DEFAULT_FILES = ["OSR_uk_000_0020", "OSR_us_000_0010", "arctic_slt_arctic_a0009", "arctic_bdl_arctic_a0003",
                 "libri_1089-134686-0001", "libri_672-122797-0001", "OSR_uk_000_0027", "arctic_slt_arctic_a0001",
                 "libri_121-121726-0001", "arctic_bdl_arctic_a0009"]


def spectrum(x, centre, n_win, win, n_fft):
    a = centre - n_win // 2
    seg = np.zeros(n_win)
    lo, hi = max(0, a), min(len(x), a + n_win)
    if hi > lo:
        seg[lo - a:hi - a] = x[lo:hi]
    return np.fft.rfft(seg * win, n_fft)


def est_band_energy(X, w0, L, n_fft, win):
    """IMBE style: energy of the band w0/2 either side of each harmonic, normalised by the window's energy."""
    out = np.zeros(L)
    binw = n_fft / (2 * np.pi)
    wnorm = np.sum(win ** 2)
    for l in range(1, L + 1):
        a, b = int(np.ceil((l - 0.5) * w0 * binw)), int(np.ceil((l + 0.5) * w0 * binw))
        b = min(b, len(X) - 1)
        out[l - 1] = np.sqrt(np.sum(np.abs(X[a:b]) ** 2) / wnorm + 1e-12)
    return out


def est_peak(X, w0, L, n_fft, win):
    """Largest magnitude within w0/2 of each harmonic, with parabolic interpolation."""
    out = np.zeros(L)
    binw = n_fft / (2 * np.pi)
    mag = np.abs(X)
    for l in range(1, L + 1):
        a, b = int(np.floor((l - 0.5) * w0 * binw)), int(np.ceil((l + 0.5) * w0 * binw))
        b = min(b, len(mag) - 1)
        k = a + int(np.argmax(mag[a:b + 1]))
        if 0 < k < len(mag) - 1:
            y0, y1, y2 = np.log(mag[k - 1] + 1e-9), np.log(mag[k] + 1e-9), np.log(mag[k + 1] + 1e-9)
            d = 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2 + 1e-12)
            out[l - 1] = np.exp(y1 - 0.25 * (y0 - y2) * d)
        else:
            out[l - 1] = mag[k]
    return out / np.sum(win) * 2


def est_at_harmonic(X, w0, L, n_fft, win):
    """Magnitude interpolated at the exact harmonic frequency (no search)."""
    binw = n_fft / (2 * np.pi)
    mag = np.abs(X)
    pos = np.arange(1, L + 1) * w0 * binw
    return np.interp(pos, np.arange(len(mag)), mag) / np.sum(win) * 2


def est_cepstral(X, w0, L, n_fft, win, lifter=30):
    """Cepstrally smoothed log envelope sampled at the harmonics."""
    logmag = np.log(np.abs(X) + 1e-9)
    c = np.fft.irfft(logmag)
    c[lifter:len(c) - lifter] = 0
    env = np.fft.rfft(c).real
    binw = n_fft / (2 * np.pi)
    pos = np.arange(1, L + 1) * w0 * binw
    return np.exp(np.interp(pos, np.arange(len(env)), env))


def est_lpc(x, centre, n_win, order, w0, L):
    """LPC envelope (autocorrelation method) sampled at the harmonics."""
    a = centre - n_win // 2
    seg = np.zeros(n_win)
    lo, hi = max(0, a), min(len(x), a + n_win)
    if hi > lo:
        seg[lo - a:hi - a] = x[lo:hi]
    seg = seg * hann(n_win)
    r = np.correlate(seg, seg, "full")[n_win - 1:n_win + order]
    r[0] *= 1.0001
    if r[0] <= 0:
        return np.ones(L)
    from scipy.linalg import solve_toeplitz
    coef = solve_toeplitz(r[:order], r[1:order + 1])
    a_poly = np.concatenate([[1.0], -coef])
    g = np.sqrt(max(r[0] - np.dot(coef, r[1:order + 1]), 1e-9))
    w = np.arange(1, L + 1) * w0
    resp = g / np.abs(np.polyval(a_poly[::-1], np.exp(-1j * w)))
    return resp


def shape_rms(est, ref):
    d = 6.0206 * (np.log2(est + 1e-9) - np.log2(est + 1e-9).mean()) - 6.0206 * (ref - ref.mean())
    return float(np.sqrt(np.mean(d ** 2)))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rate", default="ambe2450")
    ap.add_argument("--files", nargs="+", default=DEFAULT_FILES)
    ap.add_argument("--centre", type=int, default=None, help="analysis window centre relative to the frame start (samples); scanned if omitted")
    ap.add_argument("--workdir", type=Path, default=ROOT / "build/quality")
    args = ap.parse_args()
    # frames: (audio, chip frame index, w0, L, chip log2Ml, our quantised log2Ml) on fully voiced frames with matching b0
    frames = []
    for name in args.files:
        cp = args.workdir / f"{name}.{args.rate}.chip.parms"
        op = args.workdir / f"{name}.{args.rate}.ours.parms"
        wav = ROOT / "testdata/corpus" / f"{name}.wav"
        if not (cp.exists() and op.exists() and wav.exists()):
            continue
        x = wavfile.read(wav)[1].astype(float)
        chip, ours = parse_parms(cp), parse_parms(op)
        off, _ = best_offset(ours, chip)
        for i in range(len(ours)):
            j = i + off
            if not (0 <= j < len(chip)):
                continue
            bo, w0o, Lo, go, to, mlo, vvo = ours[i]
            bc, w0c, Lc, gc, tc, mlc, vvc = chip[j]
            if tc or to or bo[0] != bc[0] or Lc < 12 or vvc[:Lc].sum() < Lc or i * 160 + 400 > len(x) or i * 160 < 600:
                continue
            frames.append((x, i, w0c, Lc, np.array(mlc[:Lc]), np.array(mlo[:Lc])))
    print(f"{len(frames)} fully voiced frames with the same fundamental index on both sides")
    ours_rms = np.mean([shape_rms(2.0 ** mlo, mlc) for _, _, _, _, mlc, mlo in frames])
    print(f"our encoder's quantised shapes vs the AMBE-3000R's: {ours_rms:.2f} dB rms (the target to beat)")

    def run(centre, n_win, wname, n_fft, estimator, **kw):
        win = {"kaiser": kaiser(n_win, 6.0), "hann": hann(n_win), "bh": blackmanharris(n_win)}[wname]
        vals = []
        for x, i, w0, L, mlc, _ in frames:
            c = i * 160 + centre
            if estimator == "lpc":
                est = est_lpc(x, c, n_win, kw.get("order", 12), w0, L)
            else:
                X = spectrum(x, c, n_win, win, n_fft)
                est = {"band": est_band_energy, "peak": est_peak, "harm": est_at_harmonic,
                       "cep": lambda X, w0, L, n, w: est_cepstral(X, w0, L, n, w, kw.get("lifter", 30))}[estimator](X, w0, L, n_fft, win)
            vals.append(shape_rms(est, mlc))
        return float(np.mean(vals))

    if args.centre is None:
        print("\nwindow centre scan (band-energy estimator, 221-sample Kaiser, 256-point FFT):")
        best, best_c = 1e9, 0
        for centre in range(-480, 81, 20):
            v = run(centre, 221, "kaiser", 256, "band")
            print(f"  centre {centre:+5d}: {v:.2f} dB")
            if v < best:
                best, best_c = v, centre
        args.centre = best_c
        print(f"  best centre {best_c:+d}")
    c = args.centre
    print(f"\nestimators at centre {c:+d} (shape rms vs the AMBE-3000R, dB):")
    for label, n_win, wname, n_fft, est, kw in [
        ("band energy, 221 Kaiser, FFT 256 (IMBE)", 221, "kaiser", 256, "band", {}),
        ("band energy, 320 Hann, FFT 512", 320, "hann", 512, "band", {}),
        ("band energy, 400 Hann, FFT 512", 400, "hann", 512, "band", {}),
        ("band energy, 320 Blackman-Harris, FFT 512", 320, "bh", 512, "band", {}),
        ("peak, 221 Kaiser, FFT 512", 221, "kaiser", 512, "peak", {}),
        ("peak, 320 Hann, FFT 1024", 320, "hann", 1024, "peak", {}),
        ("peak, 400 Hann, FFT 1024", 400, "hann", 1024, "peak", {}),
        ("magnitude at the harmonic, 320 Hann, FFT 1024", 320, "hann", 1024, "harm", {}),
        ("magnitude at the harmonic, 400 Hann, FFT 1024", 400, "hann", 1024, "harm", {}),
        ("cepstral envelope (lifter 20), 320 Hann", 320, "hann", 512, "cep", {"lifter": 20}),
        ("cepstral envelope (lifter 30), 320 Hann", 320, "hann", 512, "cep", {"lifter": 30}),
        ("cepstral envelope (lifter 40), 320 Hann", 320, "hann", 512, "cep", {"lifter": 40}),
        ("LPC order 10, 320 Hann", 320, "hann", 512, "lpc", {"order": 10}),
        ("LPC order 16, 320 Hann", 320, "hann", 512, "lpc", {"order": 16}),
        ("LPC order 24, 400 Hann", 400, "hann", 512, "lpc", {"order": 24}),
    ]:
        print(f"  {run(c, n_win, wname, n_fft, est, **kw):5.2f}  {label}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
