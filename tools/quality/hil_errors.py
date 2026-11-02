#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Behaviour under bit errors: damaged AMBE-3000R frames decoded by the AMBE-3000R and by us.

  python tools/quality/hil_errors.py --port COM3 [--rates ambe2450 dstar] [--only NAME...]
        [--cached] [--out corpus/baselines/decoder_errors.json]
  python tools/quality/hil_errors.py --port COM3 --sweeps [--cached]
  python tools/quality/hil_errors.py --port COM3 --golay [--cached]

Corpus mode: for each golden file the 72 on-air bits of every frame are damaged in several
ways (uniform BER 0.5..5 %, 2-8 bit bursts, whole frames replaced by random bits at 2..10 %),
the damaged frames are decoded on the AMBE-3000R and by our decoder, and both are scored
against the AMBE-3000R's decode of the clean frames (LSD, level) and against the original
speech (PESQ, ESTOI). Our decoder's repeat/mute flags per frame are kept for the sweeps.

Sweep mode: targeted experiments on one steady voiced passage to read the AMBE-3000R's rules:
runs of 1..8 consecutive random frames (what does it repeat, for how long, and what follows),
and 1..12 flipped bits in one frame's Golay-protected part (where it stops trusting a frame),
reported as a per-frame level track of the AMBE-3000R's output next to ours with our flags.

Golay mode: one damaged frame with a chosen number of errors in the C0 Golay(24,12) and C1
Golay(23,12) words (three random patterns each), to read off where the AMBE-3000R repeats
instead of decoding, against our repeat rule.
"""
import argparse
import json
import zlib
import subprocess
import sys
from pathlib import Path

import numpy as np
from scipy.io import wavfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare import find_tool, metrics, read_wav  # noqa: E402
from hil_encoder import find_ambe3000  # noqa: E402
from perceptual import score  # noqa: E402

FS = 8000
FRAME_BYTES = 9
CONDITIONS = [("ber0.5", "ber", 0.005), ("ber1", "ber", 0.01), ("ber2", "ber", 0.02), ("ber3", "ber", 0.03), ("ber5", "ber", 0.05),
              ("burst2", "burst", 0.02), ("burst5", "burst", 0.05),
              ("lost2", "lost", 0.02), ("lost5", "lost", 0.05), ("lost10", "lost", 0.10)]


def damage(frames: np.ndarray, kind: str, p: float, seed: int) -> np.ndarray:
    """frames: (n, 9) uint8. Returns a damaged copy."""
    rng = np.random.default_rng(seed)
    bits = np.unpackbits(frames, axis=1)[:, :72]
    n = len(bits)
    if kind == "ber":
        flips = rng.random(bits.shape) < p
        bits = bits ^ flips.astype(np.uint8)
    elif kind == "burst":
        # a burst of 2..8 consecutive bit flips starts in a fraction p of the frames
        for i in np.nonzero(rng.random(n) < p)[0]:
            length = int(rng.integers(2, 9))
            start = int(rng.integers(0, 72 - length))
            bits[i, start:start + length] ^= 1
    elif kind == "lost":
        for i in np.nonzero(rng.random(n) < p)[0]:
            bits[i] = rng.integers(0, 2, 72, dtype=np.uint8)
    out = np.packbits(bits, axis=1)
    return out


def write_dvsi(p: Path, frames: np.ndarray):
    p.write_bytes(frames.astype(np.uint8).tobytes())


def read_dvsi(p: Path) -> np.ndarray:
    raw = np.frombuffer(p.read_bytes(), dtype=np.uint8)
    return raw[:len(raw) // FRAME_BYTES * FRAME_BYTES].reshape(-1, FRAME_BYTES).copy()


def chip_decode(chip: Path, port: str, rate: str, dvsi: Path, wav: Path, cached: bool):
    if cached and wav.exists():
        return
    r = subprocess.run([str(chip), "--port", port, "--rate", rate, "decode", str(dvsi), str(wav)], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(r.stderr)


def our_decode(tool: Path, rate: str, dvsi: Path, wav: Path, lost_file: Path = None):
    cmd = [str(tool), "decode", "--rate", rate, "--errors"]
    if lost_file is not None:
        cmd += ["--lost", str(lost_file)]
    r = subprocess.run(cmd + [str(dvsi), str(wav)], capture_output=True, text=True, check=True)
    flags = []
    for ln in r.stdout.splitlines():
        parts = ln.split()
        if len(parts) >= 4 and parts[0].isdigit():
            bar = parts[4] if len(parts) > 4 else ""
            flags.append({"total": int(parts[3].split("=")[1]), "repeat": "R" in bar, "mute": "M" in bar, "erasure": "E" in bar, "tone": "T" in bar})
    return flags


def frame_db(x: np.ndarray) -> np.ndarray:
    n = len(x) // 160
    return np.array([20 * np.log10(np.sqrt(np.mean(x[i * 160:(i + 1) * 160] ** 2)) / 32767 + 1e-9) for i in range(n)])


def corpus(args, tool, chip):
    result = {"rates": {}}
    for rate in args.rates:
        rows = {}
        for dvsi in sorted((ROOT / "corpus/golden" / rate).glob("*.dvsi")):
            name = dvsi.stem
            if name.startswith("syn_") or (args.only and name not in args.only):
                continue
            hw = ROOT / "testdata/golden" / rate / f"{name}.hw.wav"
            orig = ROOT / "testdata/corpus" / f"{name}.wav"
            if not hw.exists() or not orig.exists():
                continue
            clean_chip = read_wav(hw)
            ref = read_wav(orig)
            frames = read_dvsi(dvsi)
            row = {}
            for cond, kind, p in CONDITIONS:
                d = args.workdir / f"{name}.{rate}.{cond}.dvsi"
                write_dvsi(d, damage(frames, kind, p, seed=zlib.crc32(f"{name}.{cond}".encode()) & 0xffff))
                cw = args.workdir / f"{name}.{rate}.{cond}.chip.wav"
                ow = args.workdir / f"{name}.{rate}.{cond}.ours.wav"
                chip_decode(chip, args.port, rate, d, cw, args.cached)
                flags = our_decode(tool, rate, d, ow)
                c = read_wav(cw); o = read_wav(ow)
                mc, mo = metrics(c, clean_chip), metrics(o, clean_chip)
                sc, so = score(ref, c, None), score(ref, o, None)
                row[cond] = {"chip": {"lsd_db": mc["lsd_db"], "level_db": mc["level_db"], "pesq": sc["pesq"], "estoi": sc["estoi"]},
                             "ours": {"lsd_db": mo["lsd_db"], "level_db": mo["level_db"], "pesq": so["pesq"], "estoi": so["estoi"],
                                      "repeats": sum(f["repeat"] for f in flags), "mutes": sum(f["mute"] for f in flags),
                                      "erasures": sum(f["erasure"] for f in flags)}}
                print(f"{rate} {name:26s} {cond:8s} chip PESQ {sc['pesq']:.2f} LSD {mc['lsd_db']:.2f} | ours PESQ {so['pesq']:.2f} LSD {mo['lsd_db']:.2f} "
                      f"repeats {row[cond]['ours']['repeats']} mutes {row[cond]['ours']['mutes']}", flush=True)
            rows[name] = row
        agg = {}
        for cond, _, _ in CONDITIONS:
            for who in ("chip", "ours"):
                vals = [r[cond][who] for r in rows.values() if cond in r]
                if vals:
                    agg[f"{cond}.{who}"] = {m: round(float(np.mean([v[m] for v in vals if v[m] is not None])), 3) for m in ("lsd_db", "level_db", "pesq", "estoi")}
        result["rates"][rate] = {"aggregate": agg, "files": rows}
        print(f"\n{rate}: {len(rows)} files")
        print(f"  {'condition':9s} {'chip PESQ':>9s} {'ours PESQ':>9s} {'chip ESTOI':>10s} {'ours ESTOI':>10s} {'chip LSD':>8s} {'ours LSD':>8s}")
        for cond, _, _ in CONDITIONS:
            c, o = agg.get(f"{cond}.chip"), agg.get(f"{cond}.ours")
            if c and o:
                print(f"  {cond:9s} {c['pesq']:9.2f} {o['pesq']:9.2f} {c['estoi']:10.3f} {o['estoi']:10.3f} {c['lsd_db']:8.2f} {o['lsd_db']:8.2f}")
    args.out.write_text(json.dumps(result, indent=1))


def sweeps(args, tool, chip):
    """Runs of lost frames (known-lost through DCMODE LOST_FRAME, and random bits), and
    single-frame error counts, on a steady voiced passage; levels relative to the clean decode."""
    for rate in args.rates:
        dvsi = ROOT / "corpus/golden" / rate / "OSR_uk_000_0020.dvsi"
        frames = read_dvsi(dvsi)
        hw = read_wav(ROOT / "testdata/golden" / rate / "OSR_uk_000_0020.hw.wav")
        lvl = frame_db(hw)
        # the 40-frame stretch whose quietest frame is loudest (steady, no pauses), away from the ends
        best, best_i = -1e9, 0
        for i in range(10, len(lvl) - 60):
            m = lvl[i:i + 40].min()
            if m > best:
                best, best_i = m, i
        block0 = frames[best_i:best_i + 40].copy()
        rng = np.random.default_rng(1)
        seq, marks, lost_idx = [], [], []
        pos = 0
        def add(block, mark):
            nonlocal pos
            seq.append(block); marks.append(mark + (pos + 10,)); pos += 40
        add(block0.copy(), ("clean", 0))
        for k in range(1, 9):   # known-lost frames: bits unchanged, LOST_FRAME set for k frames
            add(block0.copy(), ("lostflag", k)); lost_idx += [pos - 40 + 10 + n for n in range(k)]
        for k in range(1, 9):   # random bits for k frames
            b = block0.copy(); b[10:10 + k] = rng.integers(0, 256, (k, FRAME_BYTES), dtype=np.uint8); add(b, ("random", k))
        for nflip in (1, 2, 3, 4, 5, 6, 8, 10, 12):   # n flipped bits in the first 24 on-air bits of one frame
            b = block0.copy(); bits = np.unpackbits(b[10:11], axis=1)[:, :72]
            bits[0, rng.choice(24, nflip, replace=False)] ^= 1
            b[10] = np.packbits(bits, axis=1)[0]; add(b, ("flip", nflip))
        for nflip in (6, 12, 18, 24, 36):   # n flipped bits anywhere in the 72
            b = block0.copy(); bits = np.unpackbits(b[10:11], axis=1)[:, :72]
            bits[0, rng.choice(72, nflip, replace=False)] ^= 1
            b[10] = np.packbits(bits, axis=1)[0]; add(b, ("flip72", nflip))
        all_frames = np.concatenate(seq)
        d = args.workdir / f"sweep_{rate}.dvsi"
        write_dvsi(d, all_frames)
        lost_file = args.workdir / f"sweep_{rate}.lost.txt"
        lost_file.write_text("\n".join(str(i) for i in lost_idx) + "\n")
        cw = args.workdir / f"sweep_{rate}.chip.wav"
        ow = args.workdir / f"sweep_{rate}.ours.wav"
        if not (args.cached and cw.exists()):
            r = subprocess.run([str(chip), "--port", args.port, "--rate", rate, "--lost", str(lost_file), "decode", str(d), str(cw)], capture_output=True, text=True)
            if r.returncode != 0:
                sys.exit(r.stderr)
        # ours: the LOST_FRAME frames go through mbe_processLostFramef (mbe-cli decode --lost)
        flags = our_decode(tool, rate, d, ow, lost_file)
        c = frame_db(read_wav(cw)); o = frame_db(read_wav(ow))
        # align the AMBE-3000R's output to ours by the level track of the clean block
        lag, best_corr = 0, -1
        for L in range(0, 6):
            m = min(len(c) - L, len(o))
            r = np.corrcoef(c[L:L + m], o[:m])[0, 1]
            if r > best_corr:
                best_corr, lag = r, L
        cref = c[lag:lag + 40]; oref = o[:40]   # the clean block's own levels
        print(f"\n{rate}: steady passage from frame {best_i} (quietest frame {best:.1f} dBFS), AMBE-3000R lag {lag} frames (r={best_corr:.3f})")
        print("  levels are dB relative to the same frame of the clean decode; columns: 2 frames before the damage .. 12 after")
        for kind, k, at in marks[1:]:
            base = (at // 40) * 40
            span = range(at - 2, at + 13)
            print(f"  {kind:8s} {k:2d}:")
            print("    chip " + " ".join(f"{c[i + lag] - cref[i - base]:6.1f}" for i in span))
            print("    ours " + " ".join(f"{o[i] - oref[i - base]:6.1f}" for i in span))
            print("    flag " + " ".join(f"{(('R' if flags[i]['repeat'] else '') + ('M' if flags[i]['mute'] else '') + ('E' if flags[i]['erasure'] else '')) or str(flags[i]['total']):>6s}" for i in span))


def fr_map(tool: Path, rate: str, workdir: Path):
    """On-air bit index (as damage() numbers them) -> (row, col) of mbelib's ambe_fr[4][24]."""
    frames = np.zeros((72, FRAME_BYTES), dtype=np.uint8)
    for b in range(72):
        bits = np.zeros(72, dtype=np.uint8); bits[b] = 1
        frames[b] = np.packbits(bits)
    d = workdir / f"map_{rate}.dvsi"; write_dvsi(d, frames)
    t = workdir / f"map_{rate}.txt"
    subprocess.run([str(tool), "frbits", "--rate", rate, str(d), str(t)], check=True, capture_output=True)
    m = {}
    for b, line in enumerate(t.read_text().split()):
        ones = [k for k, ch in enumerate(line) if ch == "1"]
        assert len(ones) == 1
        m[b] = (ones[0] // 24, ones[0] % 24)
    return m


def frame_lsd(a: np.ndarray, b: np.ndarray, i: int, n: int = 1) -> float:
    """Mean log-spectral distance (dB, 100..3800 Hz) over frames i..i+n-1 of two signals."""
    vals = []
    for k in range(i, i + n):
        x = a[k * 160:(k + 1) * 160]; y = b[k * 160:(k + 1) * 160]
        if len(x) < 160 or len(y) < 160:
            break
        w = np.hanning(160)
        X = np.abs(np.fft.rfft(x * w, 256)) + 1e-6; Y = np.abs(np.fft.rfft(y * w, 256)) + 1e-6
        f = np.fft.rfftfreq(256, 1 / FS); sel = (f >= 100) & (f <= 3800)
        vals.append(np.sqrt(np.mean((20 * np.log10(X[sel] / Y[sel])) ** 2)))
    return float(np.mean(vals)) if vals else float("nan")


def golay(args, tool, chip):
    """Where does the AMBE-3000R stop trusting a frame? c0 errors in the Golay(24,12) word and
    c1 errors in the Golay(23,12) word, three random patterns each, one damaged frame in a
    steady passage. A frame the AMBE-3000R repeats renders the same for every pattern (the
    previous model) and differs from the clean decode; a decoded frame with corrected errors
    equals the clean decode; a miscorrected one differs and differs between patterns."""
    for rate in args.rates:
        m = fr_map(tool, rate, args.workdir)
        row_bits = {r: [b for b, (rr, _) in m.items() if rr == r] for r in range(4)}
        dvsi = ROOT / "corpus/golden" / rate / "OSR_uk_000_0020.dvsi"
        frames = read_dvsi(dvsi)
        hw = read_wav(ROOT / "testdata/golden" / rate / "OSR_uk_000_0020.hw.wav")
        lvl = frame_db(hw)
        best, best_i = -1e9, 0
        for i in range(10, len(lvl) - 60):
            if lvl[i:i + 40].min() > best:
                best, best_i = lvl[i:i + 40].min(), i
        block0 = frames[best_i:best_i + 40].copy()
        rng = np.random.default_rng(3)
        cases = [(c0, c1) for c0 in range(0, 8) for c1 in (0, 1, 2, 3, 4, 6)]
        seq, marks = [block0.copy()], []
        pos = 40
        for c0, c1 in cases:
            for draw in range(3):
                b = block0.copy()
                bits = np.unpackbits(b[10:11], axis=1)[:, :72]
                for r, n in ((0, c0), (1, c1)):
                    for k in rng.choice(row_bits[r], n, replace=False):
                        bits[0, k] ^= 1
                b[10] = np.packbits(bits, axis=1)[0]
                seq.append(b); marks.append((c0, c1, draw, pos + 10)); pos += 40
        all_frames = np.concatenate(seq)
        d = args.workdir / f"golay_{rate}.dvsi"; write_dvsi(d, all_frames)
        cw = args.workdir / f"golay_{rate}.chip.wav"; ow = args.workdir / f"golay_{rate}.ours.wav"
        chip_decode(chip, args.port, rate, d, cw, args.cached)
        flags = our_decode(tool, rate, d, ow)
        c = read_wav(cw); o = read_wav(ow)
        # align the AMBE-3000R's output (it may lag a frame or two on this path)
        lag, best_corr = 0, -1
        cl, ol = frame_db(c), frame_db(o)
        for L in range(0, 6):
            n = min(len(cl) - L, len(ol))
            r = np.corrcoef(cl[L:L + n], ol[:n])[0, 1]
            if r > best_corr:
                best_corr, lag = r, L
        c = c[lag * 160:]
        clean_c, clean_o = c[:40 * 160], o[:40 * 160]
        print(f"\n{rate}: one damaged frame in a steady passage (from frame {best_i}); LSD in dB of the damaged frame and the two after")
        print("  it against the clean decode (0 = corrected), and between the three patterns (0 = the same output for any pattern: a repeat)")
        print(f"  {'c0':>2s} {'c1':>2s} | {'AMBE-3000R vs clean':>19s} {'between patterns':>16s} | {'ours vs clean':>13s} {'our flags':>10s}")
        for c0, c1 in cases:
            outs_c, outs_o, fl, d_c, d_o = [], [], [], [], []
            for (cc0, cc1, draw, at) in marks:
                if (cc0, cc1) != (c0, c1):
                    continue
                base = (at // 40) * 40
                seg_c = c[base * 160:(base + 40) * 160]; seg_o = o[base * 160:(base + 40) * 160]
                outs_c.append(seg_c); outs_o.append(seg_o)
                d_c.append(frame_lsd(seg_c, clean_c, 10, 3)); d_o.append(frame_lsd(seg_o, clean_o, 10, 3))
                f = flags[at]
                fl.append(("R" if f["repeat"] else "") + ("M" if f["mute"] else "") + ("E" if f["erasure"] else "") or f"d{f['total']}")
            between = np.mean([frame_lsd(outs_c[a], outs_c[b], 10, 3) for a in range(3) for b in range(a + 1, 3)])
            print(f"  {c0:2d} {c1:2d} | {np.mean(d_c):19.1f} {between:16.1f} | {np.mean(d_o):13.1f} {' '.join(fl):>10s}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--rates", nargs="+", default=["ambe2450", "dstar"])
    ap.add_argument("--only", nargs="*", default=[])
    ap.add_argument("--cached", action="store_true")
    ap.add_argument("--sweeps", action="store_true")
    ap.add_argument("--golay", action="store_true")
    ap.add_argument("--out", type=Path, default=ROOT / "corpus/baselines/decoder_errors.json")
    ap.add_argument("--workdir", type=Path, default=ROOT / "build/quality/errors")
    args = ap.parse_args()
    tool, chip = find_tool(), find_ambe3000()
    args.workdir.mkdir(parents=True, exist_ok=True)
    if args.sweeps:
        sweeps(args, tool, chip)
    elif args.golay:
        golay(args, tool, chip)
    else:
        corpus(args, tool, chip)
    return 0


if __name__ == "__main__":
    sys.exit(main())
