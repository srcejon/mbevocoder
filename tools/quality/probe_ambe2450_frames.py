#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""How the AMBE-3000R decodes AMBE+2's special frames, against our decoder (docs/decoder-quality.md,
"AMBE+2 erasures, tones and muting"): erasure frames (fundamental index 120-123), tone frames with an
unusable index or disagreeing redundant fields, the silent tone 255, tone frames under bit errors,
and muting under a steady bit-error rate.

  python tools/quality/probe_ambe2450_frames.py [--port COM3] [--work build/probe_2450frames]

Streams are built from the AMBE-3000R's own frames of a corpus recording (parameter words via
`mbe-cli dunpack`, special words edited in, `mbe-cli dpack`, bit errors flipped in the channel
bytes), decoded by the AMBE-3000R and by mbe-cli (the build in build/devtools, else build/vs2026),
and compared frame by frame: level, whether a frame repeats its predecessor's spectrum, and DTMF
energy.
"""
import argparse
import subprocess
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare import read_wav  # noqa: E402
from hil_encoder import find_ambe3000  # noqa: E402

CLI = next(p for p in (ROOT / "build/devtools/tools/mbe-cli/Release/mbe-cli.exe", ROOT / "build/vs2026/tools/mbe-cli/Release/mbe-cli.exe") if p.exists())
SOURCE = "OSR_us_000_0059"
DTMF0 = (941.0, 1336.0)


def run(*a, capture=False):
    r = subprocess.run([str(x) for x in a], check=True, capture_output=True, text=True)
    return r.stdout if capture else None


def words(path):
    return [list(map(int, l.strip())) for l in open(path) if len(l.strip()) >= 49]


def set_b0(d, b0):
    d = list(d)
    for i in range(4):
        d[i] = (b0 >> (6 - i)) & 1
    for i in range(3):
        d[37 + i] = (b0 >> (2 - i)) & 1
    return d


def set_bits(d, first, count, v):
    d = list(d)
    for i in range(count):
        d[first + i] = (v >> (count - 1 - i)) & 1
    return d


class Probe:
    def __init__(self, work, chip, port):
        self.work, self.chip, self.port = work, chip, port

    def stream(self, name, ws, flips=None, seed=1):
        """Write words, optionally flip `flips[k]` random channel bits in frame k; decode on both."""
        txt, dv = self.work / f"{name}.txt", self.work / f"{name}.dvsi"
        txt.write_text("".join("".join(map(str, w)) + "\n" for w in ws))
        run(CLI, "dpack", "--rate", "ambe2450", txt, dv)
        if flips is not None:
            b = bytearray(dv.read_bytes())
            rng = np.random.default_rng(seed)
            for k, n in enumerate(flips):
                for pos in rng.choice(72, size=n, replace=False):
                    b[9 * k + pos // 8] ^= 0x80 >> (pos % 8)   # MSB first, as the AMBE-3000R sends AMBE+2
            dv.write_bytes(bytes(b))
        cw, ow = self.work / f"{name}.chip.wav", self.work / f"{name}.ours.wav"
        run(self.chip, "--port", self.port, "--rate", "ambe2450", "decode", dv, cw)
        err = run(CLI, "decode", "--rate", "ambe2450", "--errors", dv, ow, capture=True)
        counts = []
        for l in err.splitlines():
            t = l.split()
            if len(t) >= 4 and t[1].startswith("c0="):
                counts.append((int(t[1][3:]), int(t[3][6:]), t[4] if len(t) > 4 else ""))
        return read_wav(cw), read_wav(ow), counts


def frames(x, lag=0):
    x = np.concatenate([np.zeros(max(0, -lag)), x[max(0, lag):]])
    n = len(x) // 160
    return x[:n * 160].reshape(n, 160)


def level_db(fr):
    return 10 * np.log10(np.mean(fr ** 2, axis=1) + 1e-12)


def spectrum(fr):
    return 10 * np.log10(np.abs(np.fft.rfft(fr * np.hanning(160), 256, axis=1)) ** 2 + 1e-9)


def dtmf_ratio(fr):
    f = np.arange(129) * 8000 / 256
    p = np.abs(np.fft.rfft(fr * np.hanning(160), 256, axis=1)) ** 2
    band = sum(p[:, np.abs(f - t) < 50].sum(axis=1) for t in DTMF0)
    return band / (p.sum(axis=1) + 1e-12)


def frame_lag(a, b):
    """Frames by which b's output trails a's (energy envelopes)."""
    ea, eb = level_db(frames(a)), level_db(frames(b))
    n = min(len(ea), len(eb))
    return max(range(-4, 5), key=lambda k: np.corrcoef(ea[max(0, -k):n - max(0, k)], eb[max(0, k):n - max(0, -k)])[0, 1])


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="COM3")
    ap.add_argument("--work", type=Path, default=ROOT / "build/probe_2450frames")
    a = ap.parse_args()
    a.work.mkdir(parents=True, exist_ok=True)
    P = Probe(a.work, find_ambe3000(), a.port)
    base = a.work / "base.txt"
    run(CLI, "dunpack", "--rate", "ambe2450", ROOT / "corpus/golden/ambe2450" / f"{SOURCE}.dvsi", base)
    V = words(base)
    tone_dv = a.work / "tone.dvsi"
    run(CLI, "tone", "--rate", "ambe2450", "--id", "128", "--amp", "112", "--frames", "1", tone_dv)
    run(CLI, "dunpack", "--rate", "ambe2450", tone_dv, a.work / "tone.txt")
    T = words(a.work / "tone.txt")[0]

    # voiced stretch: the frames of the recording's loudest 4 s
    lv = level_db(frames(read_wav(ROOT / "testdata/golden/ambe2450" / f"{SOURCE}.hw.wav")))
    start = int(np.argmax(np.convolve(lv, np.ones(200), "valid")))
    seg = V[start:start + 200]

    # 0. alignment: the clean stream on both decoders
    c0, o0, _ = P.stream("clean", seg)
    lag = frame_lag(o0, c0)
    C0, O0 = frames(c0, 160 * lag), frames(o0)
    print(f"alignment: the AMBE-3000R's output trails ours by {lag} frame(s)")

    def report(name, ws, marks, what, flips=None):
        c, o, cnt = P.stream(name, ws, flips)
        C, O = frames(c, 160 * lag), frames(o)
        sc, so = spectrum(C), spectrum(O)
        print(f"\n{what}")
        print("   frame  word          chip: level  vs clean  vs prev     ours: level  vs clean  vs prev  flags")
        for k in marks:
            if k >= min(len(C), len(O), len(C0)):
                continue
            dc = lambda S, R, i, j: float(np.sqrt(np.mean((S[i] - R[j]) ** 2)))
            b0 = int("".join(map(str, [ws[k][i] for i in (0, 1, 2, 3, 37, 38, 39)])), 2)
            print(f"   {k:5d}  b0 {b0:3d}{' tone' if all(ws[k][i] for i in range(6)) else '     '}"
                  f"   {level_db(C[k:k + 1])[0]:6.1f}  {dc(sc, spectrum(C0), k, k):7.1f}  {dc(sc, sc, k, k - 1):7.1f}"
                  f"      {level_db(O[k:k + 1])[0]:6.1f}  {dc(so, spectrum(O0), k, k):7.1f}  {dc(so, so, k, k - 1):7.1f}"
                  f"  {cnt[k][2] if k < len(cnt) else ''}")
        return C, O, cnt

    # 1. erasures: b0 120..123 in single frames and runs
    ws = [list(w) for w in seg]
    marks = []
    for pos, run_len, b0 in ((40, 1, 120), (80, 2, 121), (120, 4, 122), (160, 1, 123)):
        for j in range(run_len):
            ws[pos + j] = set_b0(seg[pos + j], b0)
        marks += list(range(pos - 1, pos + run_len + 2))
    report("erasure", ws, marks, "1. Erasure frames (b0 120-123); 'vs prev' small = a repeat of the frame before; levels dB")

    # 2. tone frames: valid, silent 255, invalid 200, disagreeing redundant fields
    ws = [list(w) for w in seg]
    marks = []
    variants = [("valid 128", T), ("silent 255", set_bits(T, 12, 8, 255)), ("invalid 200", set_bits(T, 12, 8, 200)),
                ("fields disagree", set_bits(set_bits(T, 45, 4, 5), 20, 4, (sum(T[12 + i] << (3 - i) for i in range(4)) + 1) & 15))]
    for i, (lab, w) in enumerate(variants):
        pos = 30 + 40 * i
        for j in range(5):
            ws[pos + j] = w
        marks += list(range(pos - 1, pos + 7))
    C, O, _ = report("tones", ws, marks, "2. Tone frames, five each at 30 (valid 128), 70 (silent 255), 110 (invalid 200), 150 (fields disagree)")
    for i, (lab, w) in enumerate(variants):
        pos = 30 + 40 * i
        print(f"   {lab:16s} DTMF energy share: chip {dtmf_ratio(C[pos + 1:pos + 4]).mean():.2f}, ours {dtmf_ratio(O[pos + 1:pos + 4]).mean():.2f}")

    # 3. tone frames under bit errors: 60 tone frames, k random channel-bit flips in each
    print("\n3. Tone frames under bit errors (DTMF 0, 60 frames per k): frames the AMBE-3000R plays as the tone,")
    print("   by our decoder's error counts (c0, total)")
    rows = []
    for k in range(0, 11):
        ws = [T] * 60
        c, o, cnt = P.stream(f"tonerr{k}", ws, flips=[k] * 60, seed=100 + k)
        C, O = frames(c, 160 * lag), frames(o)
        rc, ro = dtmf_ratio(C), dtmf_ratio(O)
        for f in range(2, min(len(C), len(O), len(cnt))):
            rows.append((k, cnt[f][0], cnt[f][1], rc[f] > 0.5, ro[f] > 0.5, level_db(C[f:f + 1])[0]))
        print(f"   k={k:2d}: chip tone {np.mean(rc[2:] > 0.5):.2f}, ours {np.mean(ro[2:] > 0.5):.2f}")
    R = np.array(rows, dtype=float)
    print("   by (c0, total): frames, chip tone share, ours")
    for c0v in range(0, 5):
        for lo, hi in ((0, 2), (2, 4), (4, 6), (6, 8), (8, 99)):
            m = (R[:, 1] == c0v) & (R[:, 2] >= lo) & (R[:, 2] < hi)
            if m.sum() >= 5:
                print(f"     c0={c0v} total {lo}-{hi - 1 if hi < 99 else '+'}: {int(m.sum()):4d}  chip {R[m, 3].mean():.2f}  ours {R[m, 4].mean():.2f}")

    # 4. muting under a steady bit-error rate: the whole recording, BER 4..12 %
    print("\n4. Steady bit errors over the recording: frames silent (< -70 dB) where the clean decode is active")
    cl, ol, _ = P.stream("ber0", V)
    act = level_db(frames(cl, 160 * lag)) > -45
    for ber in (4, 6, 8, 10, 12):
        rng = np.random.default_rng(ber)
        flips = list(rng.binomial(72, ber / 100.0, size=len(V)))
        c, o, cnt = P.stream(f"ber{ber}", V, flips=flips, seed=200 + ber)
        C, O = frames(c, 160 * lag), frames(o)
        n = min(len(C), len(O), len(cnt), len(act))
        mc, mo = level_db(C[:n]) < -70, level_db(O[:n]) < -70
        er, ers = 0.0, []
        for f in range(n):   # TIA-102.BABA-1's error-rate recursion over our decoder's counts
            er = 0.95 * er + 0.001064 * cnt[f][1]
            ers.append(er)
        ers = np.array(ers)
        a2 = act[:n]
        hi = ers > 0.096
        print(f"   BER {ber:2d} %: chip silent {mc[a2].mean():.2f}, ours {mo[a2].mean():.2f}; error rate > 0.096 on {hi[a2].mean():.2f};"
              f" chip silent where it is {mc[a2 & hi].mean() if (a2 & hi).any() else float('nan'):.2f}, where not {mc[a2 & ~hi].mean():.2f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
