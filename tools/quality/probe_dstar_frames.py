#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Which D-STAR fundamental codes the AMBE-3000R plays as lost frames (docs/decoder-quality.md,
"D-STAR special frames"): b0 125, tone frames (b0 126 / 127) with unplayable indices or volume 0,
b0 127 with a playable tone, and DVSI's all-ones DTX frame.

  python tools/quality/probe_dstar_frames.py [--port COM3] [--work build/probe_dstarframes]

Each case edits runs of 1, 3 and 5 frames into the AMBE-3000R's own frames of a voiced passage
(parameter words via `mbe-cli dunpack`, edited, `mbe-cli dpack`). The same passage is decoded a
second time with those frames' bits left as they were and flagged LOST_FRAME (DCMODE_IN). A code
the AMBE-3000R plays as a lost frame gives the same output in both, through the run and the frames
after it. Our decoder (the build in build/devtools, else build/vs2026) decodes both streams too.
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
RUNS = ((30, 1), (70, 3), (120, 5))
# tone index bits 7..5 -> the three bits at ambe_d[6..8] (the inverse of the decoder's t7/t6/t5 tables)
TOP3_TO_DEF = {4: 0, 0: 1, 1: 2, 2: 3, 3: 4, 7: 5, 6: 6, 5: 7}


def run(*a, capture=False):
    r = subprocess.run([str(x) for x in a], check=True, capture_output=True, text=True)
    return r.stdout if capture else None


def words(path):
    return [list(map(int, l.strip())) for l in open(path) if len(l.strip()) >= 49]


def set_b0(d, b0):
    d = list(d)
    for i in range(6):
        d[i] = (b0 >> (6 - i)) & 1
    d[48] = b0 & 1
    return d


def set_tone(d, index, volume):
    d = list(d)
    dfn = TOP3_TO_DEF[index >> 5]
    d[6], d[7], d[8] = (dfn >> 2) & 1, (dfn >> 1) & 1, dfn & 1
    for pos, bit in zip((9, 42, 43, 10, 11), (4, 3, 2, 1, 0)):
        d[pos] = (index >> bit) & 1
    for pos, bit in zip((12, 13, 14, 15, 16, 44, 45, 17), range(7, -1, -1)):
        d[pos] = (volume >> bit) & 1
    return d


def frames(x):
    n = len(x) // 160
    return x[:n * 160].reshape(n, 160)


def level_db(fr):
    return 10 * np.log10(np.mean(fr ** 2, axis=1) + 1e-12)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="COM3")
    ap.add_argument("--work", type=Path, default=ROOT / "build/probe_dstarframes")
    a = ap.parse_args()
    a.work.mkdir(parents=True, exist_ok=True)
    chip = find_ambe3000()
    base = a.work / "base.txt"
    run(CLI, "dunpack", "--rate", "dstar", ROOT / "corpus/golden/dstar" / f"{SOURCE}.dvsi", base)
    V = words(base)
    lv = level_db(frames(read_wav(ROOT / "testdata/golden/dstar" / f"{SOURCE}.hw.wav")))
    start = int(np.argmax(np.convolve(lv, np.ones(200), "valid")))
    seg = V[start:start + 200]
    run(CLI, "tone", "--rate", "dstar", "--id", "40", "--amp", "100", "--frames", "1", a.work / "tone.dvsi")
    run(CLI, "dunpack", "--rate", "dstar", a.work / "tone.dvsi", a.work / "tone.txt")
    T = words(a.work / "tone.txt")[0]

    cases = [
        ("b0 120 (voice)", lambda w: set_b0(w, 120)),
        ("b0 124 (voice)", lambda w: set_b0(w, 124)),
        ("b0 125", lambda w: set_b0(w, 125)),
        ("b0 126 tone 40 vol 100", lambda w: set_b0(set_tone(T, 40, 100), 126)),
        ("b0 126 index 4 (none)", lambda w: set_b0(set_tone(T, 4, 100), 126)),
        ("b0 126 index 150 (none)", lambda w: set_b0(set_tone(T, 150, 100), 126)),
        ("b0 126 dual 128 vol 0", lambda w: set_b0(set_tone(T, 128, 0), 126)),
        ("b0 126 tone 40 vol 0", lambda w: set_b0(set_tone(T, 40, 0), 126)),
        ("b0 127 tone 40 vol 100", lambda w: set_b0(set_tone(T, 40, 100), 127)),
        ("b0 127 dual 128 vol 0", lambda w: set_b0(set_tone(T, 128, 0), 127)),
        ("b0 127 dual 130 vol 100", lambda w: set_b0(set_tone(T, 130, 100), 127)),
        ("DTX all-ones frame", None),
    ]
    marks = [p + k for p, n in RUNS for k in range(n)]
    lost_file = a.work / "lost_frames.txt"
    lost_file.write_text("".join(f"{k}\n" for k in marks))

    def decode(name, ws, ff=False, lost=False):
        txt, dv = a.work / f"{name}.txt", a.work / f"{name}.dvsi"
        txt.write_text("".join("".join(map(str, w)) + "\n" for w in ws))
        run(CLI, "dpack", "--rate", "dstar", txt, dv)
        if ff:
            b = bytearray(dv.read_bytes())
            for k in marks:
                b[9 * k:9 * k + 9] = b"\xff" * 9
            dv.write_bytes(bytes(b))
        cw, ow = a.work / f"{name}.chip.wav", a.work / f"{name}.ours.wav"
        extra = ["--lost", lost_file] if lost else []
        run(chip, "--port", a.port, "--rate", "dstar", *extra, "decode", dv, cw)
        run(CLI, "decode", "--rate", "dstar", *extra, dv, ow)
        return frames(read_wav(cw)), frames(read_wav(ow))

    CL, OL = decode("lost", seg, lost=True)
    print(f"reference: frames {', '.join(f'{p}-{p + n - 1}' for p, n in RUNS)} flagged LOST_FRAME")
    print("per case, over the runs and the 6 frames after each: the largest per-frame level difference (dB)")
    print("and the largest sample difference (of full scale) from the LOST_FRAME stream, on the AMBE-3000R and on")
    print("ours; and the frame levels of the 5-frame run (dB), chip / ours")
    span = sorted({p + k for p, n in RUNS for k in range(n + 6)})
    for i, (label, edit) in enumerate(cases):
        ws = [list(w) for w in seg]
        if edit is not None:
            for k in marks:
                ws[k] = edit(seg[k])
        C, O = decode(f"case{i}", ws, ff=edit is None)
        n = min(len(C), len(CL), len(O), len(OL))
        sp = [k for k in span if k < n]
        dl = lambda X, Y: float(np.max(np.abs(level_db(X[sp]) - level_db(Y[sp]))))
        ds = lambda X, Y: float(np.max(np.abs(X[sp] - Y[sp])))
        p5 = RUNS[2][0]
        lv5 = " ".join(f"{c:5.1f}/{o:5.1f}" for c, o in zip(level_db(C[p5:p5 + 6]), level_db(O[p5:p5 + 6])))
        print(f"  {label:26s} chip vs lost: {dl(C, CL):5.1f} dB {ds(C, CL):.4f}   ours vs lost: {dl(O, OL):5.1f} dB {ds(O, OL):.4f}   run: {lv5}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
