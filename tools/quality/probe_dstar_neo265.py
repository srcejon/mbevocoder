#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""mbelib-neo 2.6.3-2.6.5's D-STAR decoder findings (made on a ThumbDV and a DVstick 30) checked on
our AMBE-3000R against our decoder: frame levels (dB) of the AMBE-3000R and of mbe-cli on the frames
each finding is about, and the mean |difference| there.

  python tools/quality/probe_dstar_neo265.py [--port COM3] [--work build/probe_neo265] [--cli PATH]

  tones      voice -> tone frames -> voice: the 80-sample crossfades in and out (neo #152), in
             quarter-frame levels
  volume     consecutive tone frames at changing volume (#165)
  tonelost   lost frames after single, DTMF and call-progress tones (#162)
  b0_124     b0 124 frames after voice and from a fresh start (#153)
  resume     the first voice frame after 4-8 lost frames (#171)
  loud       a frame at the largest gain and b3 held 50 frames (#160)
  gainstep   in-frame level curve of gain steps (#167): sample where half the dB change is reached
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
from probe_dstar_frames import set_b0, set_tone, words, frames, level_db  # noqa: E402

SOURCE = "OSR_us_000_0059"


def run(*a):
    return subprocess.run([str(x) for x in a], check=True, capture_output=True, text=True).stdout


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="COM3")
    ap.add_argument("--work", type=Path, default=ROOT / "build/probe_neo265")
    ap.add_argument("--cli", type=Path, default=ROOT / "build/vs2026/tools/mbe-cli/Release/mbe-cli.exe")
    ap.add_argument("--only", nargs="*", default=[])
    a = ap.parse_args()
    W = a.work
    W.mkdir(parents=True, exist_ok=True)
    chip = find_ambe3000()
    CLI = a.cli
    run(CLI, "dunpack", "--rate", "dstar", ROOT / "corpus/golden/dstar" / f"{SOURCE}.dvsi", W / "base.txt")
    V = words(W / "base.txt")
    lv = level_db(frames(read_wav(ROOT / "testdata/golden/dstar" / f"{SOURCE}.hw.wav")))
    start = int(np.argmax(np.convolve(lv, np.ones(200), "valid")))
    seg = V[start:start + 200]
    run(CLI, "tone", "--rate", "dstar", "--id", "40", "--amp", "100", "--frames", "1", W / "tone.dvsi")
    run(CLI, "dunpack", "--rate", "dstar", W / "tone.dvsi", W / "tone.txt")
    T = words(W / "tone.txt")[0]
    tone = lambda idx, vol: set_b0(set_tone(T, idx, vol), 126)

    def decode(name, ws=None, lost=(), fields=None):
        dv = W / f"{name}.dvsi"
        if fields is not None:
            (W / f"{name}.f.txt").write_text("".join(" ".join(map(str, f)) + "\n" for f in fields))
            run(CLI, "frames", "--rate", "dstar", W / f"{name}.f.txt", dv)
        else:
            (W / f"{name}.txt").write_text("".join("".join(map(str, w)) + "\n" for w in ws))
            run(CLI, "dpack", "--rate", "dstar", W / f"{name}.txt", dv)
        extra = []
        if lost:
            (W / f"{name}.lost").write_text("".join(f"{k}\n" for k in lost))
            extra = ["--lost", W / f"{name}.lost"]
        cw, ow = W / f"{name}.chip.wav", W / f"{name}.ours.wav"
        run(chip, "--port", a.port, "--rate", "dstar", *extra, "decode", dv, cw)
        run(CLI, "decode", "--rate", "dstar", *extra, dv, ow)
        return read_wav(cw), read_wav(ow)

    def flv(x, k0, k1):
        return level_db(frames(x)[k0:k1])

    def qlv(x, k):   # quarter-frame levels of frame k
        f = frames(x)[k].reshape(4, 40)
        return 10 * np.log10(np.mean(f ** 2, axis=1) + 1e-12)

    def report(title, c, o, spans, quarter=()):
        print(f"\n{title}")
        diffs = []
        for k0, k1, lab in spans:
            C, O = flv(c, k0, k1), flv(o, k0, k1)
            m = (C > -70) | (O > -70)
            diffs += list(np.abs(C - O)[m])
            print(f"  {lab:28s} chip " + " ".join(f"{v:6.1f}" for v in C))
            print(f"  {'':28s} ours " + " ".join(f"{v:6.1f}" for v in O))
        for k, lab in quarter:
            print(f"  {lab:28s} quarters chip " + " ".join(f"{v:6.1f}" for v in qlv(c, k)) + "   ours " + " ".join(f"{v:6.1f}" for v in qlv(o, k)))
        print(f"  mean |chip - ours| over these frames: {np.mean(diffs):.2f} dB")

    want = lambda n: not a.only or n in a.only

    if want("tones"):
        ws = [list(w) for w in seg]
        for k in range(40, 41):
            ws[k] = tone(40, 180)
        for k in range(80, 85):
            ws[k] = tone(128, 180)
        for k in range(120, 121):
            ws[k] = tone(40, 0)
        c, o = decode("tones", ws)
        report("tones: one single tone at 40, five DTMF frames at 80-84, a volume-0 tone at 120",
               c, o, [(38, 46, "frames 38-45"), (78, 90, "frames 78-89"), (118, 126, "frames 118-125")],
               [(40, "first tone frame 40"), (41, "voice after it 41"), (80, "DTMF start 80"), (85, "voice after DTMF 85")])
    if want("volume"):
        ws = [list(w) for w in seg]
        for k, v in zip(range(60, 65), (60, 180, 120, 250, 230)):
            ws[k] = tone(32, v)
        c, o = decode("volume", ws)
        report("volume: index 32 at volumes 60 180 120 250 230 (frames 60-64)", c, o, [(59, 66, "frames 59-65")],
               [(61, "60 -> 180"), (62, "180 -> 120"), (63, "120 -> 250")])
    if want("tonelost"):
        for lab, idx in (("single 40", 40), ("DTMF 128", 128), ("call progress 144", 144)):
            ws = [list(w) for w in seg]
            for k in range(50, 55):
                ws[k] = tone(idx, 180)
            lost = list(range(55, 60))
            c, o = decode(f"tonelost{idx}", ws, lost=lost)
            report(f"tonelost: five {lab} tone frames 50-54, LOST_FRAME 55-59, voice", c, o, [(53, 63, "frames 53-62")])
    if want("b0_124"):
        ws = [list(w) for w in seg]
        ws[40] = set_b0(seg[40], 124)
        for k in range(80, 83):
            ws[k] = set_b0(seg[k], 124)
        c, o = decode("b0_124", ws)
        report("b0_124: one b0 124 frame at 40, three at 80-82 (voice fields otherwise)", c, o,
               [(39, 47, "frames 39-46"), (79, 89, "frames 79-88")])
        ws = [set_b0(seg[k], 124) for k in range(10)] + [list(w) for w in seg[10:60]]
        c, o = decode("b0_124_fresh", ws)
        report("b0_124 from a fresh start: ten frames, then voice", c, o, [(0, 16, "frames 0-15")])
    if want("resume"):
        lost = list(range(40, 44)) + list(range(90, 98))
        c, o = decode("resume", [list(w) for w in seg], lost=lost)
        report("resume: LOST_FRAME 40-43 and 90-97, then voice", c, o, [(39, 47, "frames 39-46"), (96, 102, "frames 96-101")],
               [(44, "first voice after 4"), (98, "first voice after 8")])
    if want("loud"):
        f = [[60, 15, 63, 511, 0, 0, 0, 0, 0]] * 50 + [[60, 15, 30, 100, 10, 5, 5, 5, 0]] * 10
        c, o = decode("loud", fields=f)
        report("loud: b2 63, b3 511 held 50 frames", c, o, [(0, 12, "frames 0-11"), (44, 56, "frames 44-55")])
    if want("gainstep"):
        f = []
        steps = []
        for b2a, b2b in ((20, 30), (20, 40), (20, 50), (30, 60), (50, 20), (60, 30)):
            f += [[60, 15, b2a, 100, 10, 5, 5, 5, 0]] * 12
            steps.append(len(f))
            f += [[60, 15, b2b, 100, 10, 5, 5, 5, 0]] * 12
        c, o = decode("gainstep", fields=f)
        print("\ngainstep: in-frame level (16-sample blocks) of the step frame; sample where half the dB change is reached")
        for k in steps:
            out = []
            for x in (c, o):
                F = frames(x)
                blk = 10 * np.log10(np.mean(np.concatenate([F[k - 1], F[k], F[k + 1]]).reshape(-1, 16) ** 2, axis=1) + 1e-12)
                a0, a1 = blk[:10].mean(), blk[20:].mean()
                half = (a0 + a1) / 2
                cross = np.where((blk[10:20] - half) * np.sign(a1 - a0) >= 0)[0]
                out.append((a1 - a0, 16 * (cross[0] if len(cross) else 10) + 8))
            print(f"  step at frame {k}: change chip {out[0][0]:+5.1f} dB ours {out[1][0]:+5.1f} dB; half-way at sample chip {out[0][1]:3d}, ours {out[1][1]:3d}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
