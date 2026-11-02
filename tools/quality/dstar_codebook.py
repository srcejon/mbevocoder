#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Recover the D-STAR (AMBE 3600x2400) spectral-shape codebooks from the AMBE-3000R.

  python tools/quality/dstar_codebook.py capture --rate dstar --port COM3 --b0 73 --tag L28
  python tools/quality/dstar_codebook.py analyse --rate dstar --tag L28 --b0 73 [--c 0.79] [--source AMBE-3000R|ours]
  python tools/quality/dstar_codebook.py header --tag L28 --out src/decoder/ambe3600x2400_chip_const.h
  (--rate ambe2450 --c 0.65 runs the same procedure as a control: those tables are known)

Outcome (docs/decoder-quality.md): the AMBE-3000R's D-STAR codebooks are mbelib's; what differed was
the harmonic count table (now AmbePlusLtableMeasured) and the prediction coefficient.

Method. The decoder's amplitude prediction loop makes each frame's shape
    M_n = T(x_n) + c M_(n-1)        (mean-removed log2 amplitudes)
where T is the shape the codebooks produce for the frame's indices x and c the prediction
coefficient. Every probe index is sent for six frames between runs of a flat reference (the fourth
and fifth are measured: the synthesis transient after a change lasts about three frames),
and the whole sequence is simulated frame by frame (prediction, spectral enhancement, and
the synthesis' linear amplitude ramp between frames, whose Blackman-Harris-windowed
two-frame measurement weights consecutive frames about 0.10 / 0.80 / 0.10), so each probe's T is the
solution of a small least-squares problem with the residual state of the previous probe
accounted for exactly. T is linear in the codebook entries through the block DCT
(mbelib's structure, verified on the AMBE-3000R in phase 4), so solving that map at one harmonic
count gives the codebook vectors themselves. Steady-state runs do not work: the loop
amplifies extreme entries 1/(1-c) times into shapes far beyond the measurable range. The
same capture decoded by our decoder recovers mbelib's tables and validates the pipeline.
"""
import argparse
import json
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

SETTLE = 40                     # reference frames at the start (steady state measured over the last 12)
PROBE = 6                       # frames per probe index
FIRST_WINDOW = 3                # first measured window offset: the synthesis transient after a change lasts ~3 frames
GAP = 10                        # reference frames after each probe


def ramp_weights(n_frames=2):
    """Weights of the frame-boundary amplitudes in a windowed measurement over n_frames output
    frames, each a linear ramp between consecutive parameter frames (Blackman-Harris window)."""
    from scipy.signal.windows import blackmanharris
    N = 160 * n_frames
    w = blackmanharris(N)
    weights = np.zeros(n_frames + 1)
    for f in range(n_frames):
        t = (np.arange(160) + 0.5) / 160
        weights[f] += np.sum(w[f * 160:(f + 1) * 160] * (1 - t))
        weights[f + 1] += np.sum(w[f * 160:(f + 1) * 160] * t)
    return weights / weights.sum()


RAMP = ramp_weights(2)          # about 0.10 / 0.80 / 0.10 on the ramps' end points


class Rate:
    """Everything rate specific: mbelib's tables (the truth for AMBE+2, the starting point for
    D-STAR), the fundamental mapping, index widths and the all-voiced voicing row."""
    def __init__(self, name):
        self.name = name
        dstar = name == "dstar"
        hdr = (ROOT / ("src/decoder/ambe3600x2400_const.h" if dstar else "src/decoder/ambe3600x2450_const.h")).read_text()
        pre = "AmbePlus" if dstar else "Ambe"
        def tab(nm, shape):
            m = re.search(r'static const (?:float|int) ' + pre + nm + r'\[.*?\]\s*(?:\[.*?\])?\s*=\s*\{(.*?)\};', hdr, re.S)
            return np.array([float(v) for v in re.findall(r'-?\d+\.?\d*(?:e-?\d+)?', m.group(1))]).reshape(shape)
        self.LTABLE = tab('LtableMeasured', (126,) if dstar else (120,)).astype(int)
        self.LMPRBL = tab('Lmprbl', (57, 4)).astype(int)
        self.PRBA24 = tab('PRBA24', (512, 3)); self.PRBA58 = tab('PRBA58', (128, 4))
        self.hocRows = [16, 16, 16, 16] if dstar else [32, 16, 16, 8]
        self.HOC = [tab('HOCb5', (self.hocRows[0], 4)), tab('HOCb6', (self.hocRows[1], 4)), tab('HOCb7', (self.hocRows[2], 4)), tab('HOCb8', (self.hocRows[3], 4))]
        self.b8step = 2 if dstar else 1      # D-STAR's b8 has three real bits: mbelib reads even rows only
        self.DG = tab('Dg', (64,) if dstar else (32,))
        self.voicedRow = 15 if dstar else 0
        self.cliRate = "dstar" if dstar else "ambe2450"
        if dstar:
            self.f0_cps = lambda b0: 2 ** (-4.258420 - 0.021769 * b0)   # measured AMBE-3000R mapping
        else:
            self.f0_cps = lambda b0: 2 ** (-4.322605 - 0.0220427 * b0)   # measured AMBE-3000R law (AmbeW0Measured)
        # flattest entries of the tables as the reference
        self.REF = (int(np.argmin(np.linalg.norm(self.PRBA24, axis=1))), int(np.argmin(np.linalg.norm(self.PRBA58, axis=1))),
                    *[int(np.argmin(np.linalg.norm(self.HOC[i][::self.b8step if i == 3 else 1], axis=1))) * (self.b8step if i == 3 else 1) for i in range(4)])
        self.hocIndices = [list(range(0, self.hocRows[i], self.b8step if i == 3 else 1)) for i in range(4)]


R = Rate("dstar")


def set_rate(name):
    global R
    R = Rate(name)


def cases():
    r = R.REF
    out = []
    for b3 in range(512): out.append(("b3", b3, (b3, r[1], r[2], r[3], r[4], r[5])))
    for b4 in range(128): out.append(("b4", b4, (r[0], b4, r[2], r[3], r[4], r[5])))
    for b5 in R.hocIndices[0]: out.append(("b5", b5, (r[0], r[1], b5, r[3], r[4], r[5])))
    for b6 in R.hocIndices[1]: out.append(("b6", b6, (r[0], r[1], r[2], b6, r[4], r[5])))
    for b7 in R.hocIndices[2]: out.append(("b7", b7, (r[0], r[1], r[2], r[3], b7, r[5])))
    for b8 in R.hocIndices[3]: out.append(("b8", b8, (r[0], r[1], r[2], r[3], r[4], b8)))
    rng = np.random.default_rng(7)   # additivity checks: random combinations
    for i in range(24):
        out.append(("mix", i, (int(rng.integers(512)), int(rng.integers(128)), int(rng.choice(R.hocIndices[0])), int(rng.choice(R.hocIndices[1])),
                               int(rng.choice(R.hocIndices[2])), int(rng.choice(R.hocIndices[3])))))
    return out


def probe_gain_index(idx, L, w0, c, target_peak, plen):
    """Gain index whose steady level puts the probe's expected peak (mbelib's tables as a
    proxy for the shape, phases aligned) at target_peak int16 counts on the AMBE-3000R, whose
    output is about 2.2 counts per unit of linear amplitude (ours is 16 dB hotter)."""
    G2T, HOC2T = basis(L)
    T = predicted_T(idx, L, G2T, HOC2T)
    shape = T * sum(c ** j for j in range(plen))    # shape at the last probe frame
    M = 2.0 ** (shape - 0.5 * np.log2(L))     # gamma = 0
    peak = 2.2 * 0.7 * np.sum(enhance(M, w0)) # phases roughly aligned at the pitch pulse
    need = np.log2(target_peak / max(peak, 1e-9))   # gamma needed
    b2 = int(np.argmin(np.abs(2 * R.DG - need)))
    if 2 * R.DG[b2] > need and b2 > 0: b2 -= 1
    return b2


OURS_TARGET_PEAK = 2500.0   # our decoder is 16 dB hotter than the AMBE-3000R and clips internally at 31128


SHORT_PROBE = 3                 # probe length for large entries (measured at the second window)
LARGE_NORM = 0.8                # mbelib-table norm above which an entry is probed short


def probe_plan(idx):
    """(probe frames, first measured window) for an index combination: large shapes cannot
    be measured after the transient (their range outruns 16 bits), so they get a short
    probe measured at the second window with the transient error accepted."""
    big = np.linalg.norm(R.PRBA24[idx[0]]) > LARGE_NORM or np.linalg.norm(R.PRBA58[idx[1]]) > LARGE_NORM
    return (SHORT_PROBE, 1) if big else (PROBE, FIRST_WINDOW)


def sequence(b0, b2_shift, c=0.79, target_peak=16000.0):
    """Frame index lines for the whole capture and, per probe, (first probe frame, probe
    length, first measured window). Each probe block is GAP reference frames then the probe
    frames, all at the block's gain index (settles during the reference run)."""
    L = R.LTABLE[b0]; w0 = 2 * np.pi * R.f0_cps(b0)
    nb2 = len(R.DG) - 1
    lines = []
    def frame(idx, b2): lines.append(f"{b0} {R.voicedRow} {b2} {idx[0]} {idx[1]} {idx[2]} {idx[3]} {idx[4]} {idx[5]}")
    b2ref = max(0, min(nb2, probe_gain_index(R.REF, L, w0, c, target_peak, PROBE) + b2_shift))
    for _ in range(SETTLE): frame(R.REF, b2ref)
    starts = []
    for _, _, idx in cases():
        plen, fw = probe_plan(idx)
        b2 = max(0, min(nb2, probe_gain_index(idx, L, w0, c, target_peak, plen) + b2_shift))
        for _ in range(GAP): frame(R.REF, b2)
        starts.append((len(lines), plen, fw))
        for _ in range(plen): frame(idx, b2)
    return lines, starts


# ---------------------------------------------------------------- decoder model (mbelib structure)

def basis(L):
    """Linear maps from codebook unknowns to the mean-removed shape T[0..L-1].
    Returns G2T (7 x L: G2..G8) and HOC2T (4 blocks x 4 coefficients x L)."""
    J = R.LMPRBL[L]
    def block_rows(i, k):
        T = np.zeros(L); start = int(sum(J[:i]))
        for j in range(1, J[i] + 1):
            T[start + j - 1] = (1 if k == 1 else 2) * np.cos(np.pi * (k - 1) * (j - 0.5) / J[i])
        return T
    G2T = np.zeros((7, L))
    for m in range(2, 9):
        Gm = np.zeros(9); Gm[m] = 1.0
        Ri = np.array([sum((1 if mm == 1 else 2) * Gm[mm] * np.cos(np.pi * (mm - 1) * (i - 0.5) / 8) for mm in range(1, 9)) for i in range(1, 9)])
        T = np.zeros(L)
        for i in range(4):
            T += 0.5 * (Ri[2 * i] + Ri[2 * i + 1]) * block_rows(i, 1) + (Ri[2 * i] - Ri[2 * i + 1]) / (2 * np.sqrt(2)) * block_rows(i, 2)
        G2T[m - 2] = T
    HOC2T = np.zeros((4, 4, L))
    for i in range(4):
        for k in range(3, 7):
            if k <= J[i]:
                HOC2T[i][k - 3] = block_rows(i, k)
    return G2T, HOC2T


def predicted_T(idx, L, G2T, HOC2T, p24=None, p58=None, hoc=None):
    if p24 is None: p24, p58, hoc = R.PRBA24, R.PRBA58, R.HOC
    b3, b4, b5, b6, b7, b8 = idx
    T = np.zeros(L)
    for m in range(3): T += p24[b3][m] * G2T[m]
    for m in range(4): T += p58[b4][m] * G2T[3 + m]
    for i, b in enumerate((b5, b6, b7, b8)):
        for k in range(4): T += hoc[i][b][k] * HOC2T[i][k]
    return T - T.mean()


def enhance(M, w0):
    """mbelib's spectral amplitude enhancement (mbe_spectralAmpEnhanceWithRm0) on linear Ml[1..L]."""
    L = len(M)
    scale = M.max()
    M = M / scale     # scale invariant; keep the arithmetic in range
    l = np.arange(1, L + 1)
    cosl = np.cos(w0 * l)
    Rm0 = np.sum(M ** 2); Rm1 = np.sum(M ** 2 * cosl)
    W = np.sqrt(M) * ((0.96 * np.pi * (Rm0 ** 2 + Rm1 ** 2 - 2 * Rm0 * Rm1 * cosl)) / (w0 * Rm0 * (Rm0 ** 2 - Rm1 ** 2))) ** 0.25
    out = M.copy()
    for i in range(L):
        if 8 * (i + 1) <= L: continue
        out[i] = M[i] * min(1.2, max(0.5, W[i]))
    s = np.sum(out ** 2)
    return out * (np.sqrt(Rm0 / s) if s > 0 else 1.0) * scale


def invert_enhance(E, w0, iters=40):
    M = E.copy()
    for _ in range(iters):
        M = M * E / np.maximum(enhance(M, w0), 1e-9)
    return M


def harmonic_log2(seg, f0, L, fs=8000):
    """log2 amplitude at each harmonic of a stationary segment (Hann, zero padded)."""
    N = 16384
    X = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), N)); df = fs / N
    out = np.zeros(L)
    for l in range(1, L + 1):
        k = int(round(l * f0 * fs / df))
        out[l - 1] = np.log2(X[max(0, k - 3):k + 4].max() + 1e-9)
    return out


# ---------------------------------------------------------------- commands

def capture(args):
    tool, chip = find_tool(), find_ambe3000()
    d = ROOT / "build/probe"; d.mkdir(parents=True, exist_ok=True)
    spec = d / f"codebook_{args.tag}.txt"
    lines, _ = sequence(args.b0, 0, args.c)
    spec.write_text("\n".join(lines) + "\n")
    ours_spec = d / f"codebook_{args.tag}.ours.txt"
    ours_lines, _ = sequence(args.b0, 0, args.c, target_peak=OURS_TARGET_PEAK)
    ours_spec.write_text("\n".join(ours_lines) + "\n")
    dvsi = d / f"codebook_{args.tag}.dvsi"
    subprocess.run([str(tool), "frames", "--rate", R.cliRate, str(spec), str(dvsi)], check=True, capture_output=True)
    ours_dvsi = d / f"codebook_{args.tag}.ours.dvsi"
    subprocess.run([str(tool), "frames", "--rate", R.cliRate, str(ours_spec), str(ours_dvsi)], check=True, capture_output=True)
    subprocess.run([str(tool), "decode", "--rate", R.cliRate, str(ours_dvsi), str(d / f"codebook_{args.tag}.ours.wav")], check=True, capture_output=True)
    print(f"{len(lines)} frames ({len(lines) * 0.02:.0f} s); decoding on the chip ...")
    r = subprocess.run([str(chip), "--port", args.port, "--rate", R.cliRate, "decode", str(dvsi), str(d / f"codebook_{args.tag}.hw.wav")],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr, file=sys.stderr); return 1
    print(r.stderr.strip().splitlines()[-1])
    return 0


def harmonic_lin(seg, f0, L, fs=8000):
    """Linear amplitude at each harmonic of a segment (Blackman-Harris, zero padded)."""
    from scipy.signal.windows import blackmanharris
    N = 16384
    X = np.abs(np.fft.rfft(seg * blackmanharris(len(seg)), N)); df = fs / N
    out = np.zeros(L)
    for l in range(1, L + 1):
        k = int(round(l * f0 * fs / df))
        out[l - 1] = X[max(0, k - 2):k + 3].max() + 1e-9
    return out


def measure(args, source):
    """Per case: mean-removed codebook shape T (log2) by fitting the simulated decoder."""
    from scipy.optimize import least_squares
    L = R.LTABLE[args.b0]; f0 = R.f0_cps(args.b0); w0 = 2 * np.pi * f0; c = args.c
    fs, x = wavfile.read(ROOT / "build/probe" / f"codebook_{args.tag}.{source}.wav"); x = x.astype(np.float64)
    lines, starts = sequence(args.b0, 0, c)
    lag = args.lag * 160
    x = np.concatenate([x, np.zeros(lag + 320)])   # the lag pushes the last probe's windows past the capture's end
    window = lambda first, count: x[first * 160 + lag:(first + count) * 160 + lag]
    # reference steady state from the settle run
    E = harmonic_lin(window(SETTLE - 12, 12), f0, L)
    clipped = 0
    Mref = np.log2(invert_enhance(E, w0)); Mref -= Mref.mean()
    Tref = (1 - c) * Mref
    print(f"reference shape range {(Mref.max() - Mref.min()) * 6.02:.1f} dB")
    A = lambda M: enhance(2.0 ** M, w0)
    state = Mref.copy()
    T = {"ref": Tref}
    W = {"ref": np.ones(L)}
    resid = []
    for (fam, i, idx), (first, plen, fw) in zip(cases(), starts):
        # two overlapping two-frame windows: output frames (first, first+1) see the ramps
        # state->x1->x2, frames (first+1, first+2) see x1->x2->x3. The early window keeps
        # extreme entries measurable (amplification 1 + c instead of 1 + c + c^2).
        lms, ws = [], []
        for off in range(plen - 1):
            seg = window(first + off, 2)
            amp = harmonic_lin(seg, f0, L)
            lm = np.log2(amp)
            # reliable: within the range of the peak and above the 16-bit floor (the
            # Blackman-Harris peak of a sinusoid of amplitude a over 320 samples is 57.6 a)
            w = ((lm >= lm.max() - args.range / 6.02) & (amp >= 57.6 * 2.0)).astype(float)
            if off < fw: w *= args.early   # transient windows: not modelled (phase regeneration)
            if np.abs(seg).max() >= 31000: w[:] = 0; clipped += 1   # clipped window: unusable
            lms.append(lm); ws.append(w)
        # state before the probe: measured from the last two reference frames of the gap
        # (nearly steady, so the ramp mixing is negligible) rather than simulated, so an
        # error in one probe's estimate cannot propagate into the next
        E0 = harmonic_lin(window(first - 2, 2), f0, L)
        s0 = np.log2(invert_enhance(E0, w0)); s0 -= s0.mean()
        if np.abs(window(first - 2, 2)).max() >= 31000: s0 = state.copy()
        def model(t):
            t = t - t.mean()
            ms = [s0]
            for _ in range(plen): ms.append(t + c * ms[-1])
            a = [A(m) for m in ms]
            preds = [np.log2(RAMP[0] * a[k] + RAMP[1] * a[k + 1] + RAMP[2] * a[k + 2]) for k in range(plen - 1)]
            return preds, ms[-1]
        def fun(t):
            preds, _ = model(t)
            out = []
            for pred, lm, w in zip(preds, lms, ws):
                if np.sum(w) == 0: out.append(np.zeros(L)); continue
                r = (pred - lm); r -= np.sum(r * w) / np.sum(w)
                out.append(r * w)
            return np.concatenate(out)
        wsum = sum(np.sum(w) for w in ws)
        if wsum == 0:
            T[(fam, i)] = np.zeros(L); W[(fam, i)] = np.zeros(L); resid.append(0.0)
            _, mlast = model(np.zeros(L)); state = mlast
            for _ in range(GAP): state = Tref + c * state
            continue
        k = fw + 1   # the middle frame of the first measured window is probe frame k+1
        gain = sum(c ** j for j in range(k + 1))
        lm, w = lms[fw], ws[fw]
        if np.sum(w) == 0: w = np.ones(L) * 1e-3   # clipped/unusable: nominal start, the fit carries no weight
        tB = ((lm - np.sum(lm * w) / np.sum(w)) - c ** (k + 1) * (s0 - s0.mean())) / gain * w
        starts_ = [tB]
        if args.early > 0:
            lm, w = lms[0], ws[0]
            starts_.append(((lm - np.sum(lm * w) / np.sum(w)) - c * (s0 - s0.mean())) * w)
        best = None
        for t0 in starts_:
            sol = least_squares(fun, np.clip(t0, -9.9, 9.9), bounds=(-10, 10), method="trf", xtol=1e-7, ftol=1e-9, max_nfev=400)
            cost = np.sqrt(np.sum(fun(sol.x) ** 2) / wsum) * 6.02
            if best is None or cost < best[0]: best = (cost, sol)
        cost, sol = best
        t = sol.x - sol.x.mean()
        if wsum == 0: t = np.zeros(L); cost = 0.0   # unusable probe (clipped): no information
        resid.append(cost)
        T[(fam, i)] = t
        W[(fam, i)] = np.max(np.array(ws), axis=0)   # a harmonic is trusted if a used window resolves it
        _, mlast = model(sol.x)
        state = mlast
        for _ in range(GAP): state = Tref + c * state
    print(f"probe model residual {np.mean(resid):.2f} dB rms (max {np.max(resid):.2f}) over reliable harmonics (within {args.range:.0f} dB of the peak); {clipped} clipped windows")
    # prediction coefficient from the decay of each probe's shape through the reference gap:
    # windows two frames apart in the gap differ by c^2 in their deviation from the reference
    num = den = 0.0
    for (fam, i, idx), (first, plen, fw) in zip(cases(), starts):
        devs = []
        for off in (plen + 1, plen + 3):
            lm = np.log2(harmonic_lin(window(first + off, 2), f0, L))
            if lm.max() - lm.min() > 30 / 6.02: devs = None; break
            Mi = np.log2(invert_enhance(2.0 ** lm, w0)); Mi -= Mi.mean()
            devs.append(Mi - Mref)
        if devs is None: continue
        num += np.dot(devs[0], devs[1]); den += np.dot(devs[0], devs[0])
    if den > 0:
        print(f"prediction coefficient from the gap decay: c = {np.sqrt(max(num / den, 0)):.3f} (analysis used {c})")
    return L, T, W


def analyse(args):
    L, T, W = measure(args, "hw" if args.source == "chip" else "ours")
    G2T, HOC2T = basis(L)
    ref = T["ref"]
    # each family's own components (the basis rows it is expected to use) and, for
    # diagnostics on fully resolved probes, the complete basis
    hocB = HOC2T.reshape(16, L)
    families = {"b3": (G2T[0:3], G2T, range(512)), "b4": (G2T[3:7], G2T, range(128)), "b5": (hocB[0:4], hocB, R.hocIndices[0]),
                "b6": (hocB[4:8], hocB, R.hocIndices[1]), "b7": (hocB[8:12], hocB, R.hocIndices[2]), "b8": (hocB[12:16], hocB, R.hocIndices[3])}
    fitted = {}
    print(f"{R.name}: L = {L}, blocks {R.LMPRBL[L].tolist()}, c = {args.c}, source {args.source}, reference {R.REF}")
    for fam, (Aown, Afull, count) in families.items():
        rows = []
        resid = []; unexpected = []; unusable = 0
        for i in count:
            if (fam, i) not in T: continue
            w = W[(fam, i)]
            if np.sum(w) < max(Aown.shape[0] + 2, 12): unusable += 1; continue   # too few resolved harmonics: ill conditioned
            dT = T[(fam, i)] - ref
            def wfit(A):
                Aw = np.hstack([A.T, np.ones((L, 1))]) * w[:, None]
                x, *_ = np.linalg.lstsq(Aw, dT * w, rcond=None)
                x = x[:-1]
                r = (dT - A.T @ x); r -= np.sum(r * w) / np.sum(w)
                return x, np.sqrt(np.sum((r * w) ** 2) / np.sum(w)) * 6.02
            x, r = wfit(Aown)
            resid.append(r)
            if np.sum(w) == L:
                xf, _ = wfit(Afull)
                own = np.zeros(Afull.shape[0], bool)
                if fam == "b3": own[0:3] = True
                elif fam == "b4": own[3:7] = True
                else:
                    blk = int(fam[1]) - 5; own[blk * 4:blk * 4 + 4] = True
                unexpected.append(np.sqrt(np.mean(xf[~own] ** 2)) / (np.sqrt(np.mean(xf ** 2)) + 1e-9))
            rows.append((i, x, r, int(np.sum(w))))
        fitted[fam] = rows
        print(f"  {fam}: {len(rows)} entries fitted ({unusable} unusable), residual {np.mean(resid):.2f} dB rms (max {np.max(resid):.2f}); "
              f"unexpected-component fraction on fully resolved probes {np.mean(unexpected) if unexpected else float('nan'):.3f} ({len(unexpected)} probes)")
    # reference absolute: distribute T(ref) over the families' own components
    Aall = np.vstack([G2T, HOC2T.reshape(16, L)])
    xref, *_ = np.linalg.lstsq(Aall.T, ref, rcond=None)
    rref = ref - Aall.T @ xref
    print(f"  reference frame fit residual {np.sqrt(np.mean(rref ** 2)) * 6.02:.2f} dB rms")
    # tables: fitted entries; entries that could not be measured keep mbelib's values and are listed
    p24 = R.PRBA24.copy(); p58 = R.PRBA58.copy(); hoc = [h.copy() for h in R.HOC]
    quality = {}
    for i, x, r, nrel in fitted["b3"]: p24[i] = xref[0:3] + x; quality[("b3", i)] = (r, nrel)
    for i, x, r, nrel in fitted["b4"]: p58[i] = xref[3:7] + x; quality[("b4", i)] = (r, nrel)
    for blk, fam in enumerate(("b5", "b6", "b7", "b8")):
        for i, x, r, nrel in fitted[fam]:
            hoc[blk][i] = xref[7 + blk * 4:7 + blk * 4 + 4] + x; quality[(fam, i)] = (r, nrel)
            if fam == "b8" and R.b8step == 2: hoc[blk][i + 1] = hoc[blk][i]
    missing = [(fam, i) for fam, (_, _, count) in families.items() for i in count if (fam, i) not in quality]
    print(f"  entries kept from mbelib (not measurable): {len(missing)} {missing[:12]}")
    # compare with mbelib's tables and check additivity on the random mixes
    def corr(a, b): return float(np.corrcoef(a.ravel(), b.ravel())[0, 1])
    hm = [h.copy() for h in R.HOC]
    if R.b8step == 2: hm[3][1::2] = hm[3][0::2]   # b8's odd rows are unreachable
    hall = np.concatenate([h.ravel() for h in hoc]); hmall = np.concatenate([h.ravel() for h in hm])
    print(f"  vs mbelib tables: PRBA24 corr {corr(p24, R.PRBA24):.3f} rms diff {np.sqrt(np.mean((p24 - R.PRBA24) ** 2)):.3f}; PRBA58 corr {corr(p58, R.PRBA58):.3f} rms diff {np.sqrt(np.mean((p58 - R.PRBA58) ** 2)):.3f}; "
          f"HOC corr {corr(hall, hmall):.3f} rms diff {np.sqrt(np.mean((hall - hmall) ** 2)):.3f} per block {[round(float(np.sqrt(np.mean((hoc[i] - hm[i]) ** 2))), 3) for i in range(4)]}")
    errs_fit = []; errs_mbe = []
    for (fam, i, idx) in cases():
        if fam != "mix" or (fam, i) not in T: continue
        Tm = T[(fam, i)]; w = W[(fam, i)]
        def werr(pred):
            r = pred - Tm; r -= np.sum(r * w) / np.sum(w)
            return np.sqrt(np.sum((r * w) ** 2) / np.sum(w)) * 6.02
        errs_fit.append(werr(predicted_T(idx, L, G2T, HOC2T, p24, p58, hoc)))
        errs_mbe.append(werr(predicted_T(idx, L, G2T, HOC2T)))
    print(f"  additivity on {len(errs_fit)} random index mixes: fitted tables predict the measured shape within {np.mean(errs_fit):.2f} dB rms (mbelib's tables {np.mean(errs_mbe):.2f} dB)")
    out = ROOT / "build/probe" / f"codebook_{args.tag}.{args.source}.npz"
    np.savez(out, p24=p24, p58=p58, hoc=np.array(hoc, dtype=object), L=L, c=args.c, quality=np.array([(k[0], k[1], v[0], v[1]) for k, v in quality.items()], dtype=object))
    print(f"wrote {out}")
    return 0


def header(args):
    z = np.load(ROOT / "build/probe" / f"codebook_{args.tag}.chip.npz", allow_pickle=True)
    p24, p58, hoc = z["p24"], z["p58"], list(z["hoc"])
    with open(args.out, "w", newline="\n") as f:
        f.write("// SPDX-License-Identifier: GPL-3.0-or-later\n// D-STAR (AMBE 3600x2400) spectral-shape codebooks measured on an AMBE-3000R with\n"
                "// tools/quality/dstar_codebook.py (docs/decoder-quality.md). Generated; do not edit.\n"
                f"// Harmonic count {int(z['L'])}, prediction coefficient {float(z['c']):.2f}.\n#ifndef MBEVOCODER_AMBE3600X2400_CHIP_CONST_H\n#define MBEVOCODER_AMBE3600X2400_CHIP_CONST_H\n\n")
        def emit(name, arr):
            f.write(f"static const float {name}[{arr.shape[0]}][{arr.shape[1]}] = {{\n")
            for r in arr: f.write("    {" + ", ".join(f"{v:.6f}f" for v in r) + "},\n")
            f.write("};\n\n")
        emit("AmbeChipPRBA24", p24); emit("AmbeChipPRBA58", p58)
        for i, nm in enumerate(("AmbeChipHOCb5", "AmbeChipHOCb6", "AmbeChipHOCb7", "AmbeChipHOCb8")): emit(nm, hoc[i])
        f.write("#endif\n")
    print(f"wrote {args.out}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["capture", "analyse", "header"])
    ap.add_argument("--rate", default="dstar", choices=["dstar", "ambe2450"], help="ambe2450 is the control: its tables are known")
    ap.add_argument("--port", default="COM3")
    ap.add_argument("--b0", type=int, default=73)
    ap.add_argument("--tag", default="L28")
    ap.add_argument("--c", type=float, default=0.79)
    ap.add_argument("--lag", type=int, default=0, help="decoder output lag in frames (the AMBE-3000R: 2 on D-STAR, 0 on AMBE+2)")
    ap.add_argument("--source", default="chip", choices=["chip", "ours"])
    ap.add_argument("--range", type=float, default=60.0, help="harmonics within this many dB of the peak are fitted")
    ap.add_argument("--early", type=float, default=0.0, help="weight of the transition window (0 = post-transition window only)")
    ap.add_argument("--out", default=str(ROOT / "src/decoder/ambe3600x2400_chip_const.h"))
    args = ap.parse_args()
    set_rate(args.rate)
    return {"capture": capture, "analyse": analyse, "header": header}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
