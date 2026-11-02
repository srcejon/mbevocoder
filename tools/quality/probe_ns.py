#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""How much does the AMBE-3000R's noise suppression take out? Speech in noise through the chip.

  python tools/quality/probe_ns.py --port COM3 [--rate ambe2450|dstar] [--cached] [--ours]

Builds one wav: a speech file (with its natural pauses) mixed with white and with pink
noise at several SNRs, then noise alone. The AMBE-3000R encodes it with NS off and with NS on
(ECMODE 0x0040), decodes both on the AMBE-3000R, and the script reports per section the level of
the pauses and of the speech with and without suppression and PESQ against the clean speech
(pip install pesq). With --ours the library's encoder with noise_suppress is run and
measured the same way (decoded by our decoder).
"""
import argparse
import subprocess
import sys
from pathlib import Path

import numpy as np
from scipy.io import wavfile
from scipy.signal import lfilter

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare import find_tool  # noqa: E402
from hil_encoder import find_ambe3000  # noqa: E402

FS = 8000


def pink(n, rng):
    w = rng.normal(0, 1, n)
    b = [0.049922035, -0.095993537, 0.050612699, -0.004408786]
    a = [1, -2.494956002, 2.017265875, -0.522189400]
    return lfilter(b, a, w)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--rate", default="ambe2450", choices=["ambe2450", "dstar"])
    ap.add_argument("--cached", action="store_true")
    ap.add_argument("--ours", action="store_true")
    args = ap.parse_args()
    tool, chip = find_tool(), find_ambe3000()
    work = ROOT / "build/probe"
    work.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(3)
    sp = wavfile.read(ROOT / "testdata/corpus/OSR_uk_000_0020.wav")[1].astype(float)[:int(6.0 * FS)]
    sp_rms = np.sqrt(np.mean(sp[sp != 0] ** 2))
    sections = []
    gap = np.zeros(int(0.5 * FS))
    for kind in ("white", "pink"):
        for snr in (25, 15, 5):
            nz = rng.normal(0, 1, len(sp)) if kind == "white" else pink(len(sp), rng)
            nz = nz / np.sqrt(np.mean(nz ** 2)) * sp_rms * 10 ** (-snr / 20)
            sections.append((f"{kind} {snr} dB", sp + nz, nz))
    for kind in ("white", "pink"):
        nz = rng.normal(0, 1, 2 * FS) if kind == "white" else pink(2 * FS, rng)
        nz = nz / np.sqrt(np.mean(nz ** 2)) * 10 ** (-35 / 20) * 32767
        sections.append((f"{kind} only -35 dBFS", nz, nz))
    x = np.concatenate([np.concatenate([s[1], gap]) for s in sections])
    wav = work / f"probe_ns_{args.rate}.wav"
    wavfile.write(wav, FS, np.clip(x, -32768, 32767).astype(np.int16))
    outs = {}
    for name, ecmode in (("off", None), ("on", "0x0040")):
        dvsi = work / f"probe_ns_{args.rate}.{name}.dvsi"
        dec = work / f"probe_ns_{args.rate}.{name}.chip.wav"
        if not (args.cached and dec.exists()):
            cmd = [str(chip), "--port", args.port, "--rate", args.rate]
            if ecmode:
                cmd += ["--ecmode", ecmode]
            r = subprocess.run(cmd + ["encode", str(wav), str(dvsi)], capture_output=True, text=True)
            if r.returncode != 0:
                print(r.stderr, file=sys.stderr); return 1
            subprocess.run([str(chip), "--port", args.port, "--rate", args.rate, "decode", str(dvsi), str(dec)], capture_output=True, text=True)
        outs[name] = wavfile.read(dec)[1].astype(float)
    if args.ours:
        for name, flag in (("ours-off", []), ("ours-on", ["--noise-suppress"])):
            dvsi = work / f"probe_ns_{args.rate}.{name}.dvsi"
            dec = work / f"probe_ns_{args.rate}.{name}.ours.wav"
            subprocess.run([str(tool), "encode", "--rate", args.rate] + flag + [str(wav), str(dvsi)], check=True, capture_output=True)
            subprocess.run([str(tool), "decode", "--rate", args.rate, str(dvsi), str(dec)], check=True, capture_output=True)
            outs[name] = wavfile.read(dec)[1].astype(float)
    delay = 160 * 2   # AMBE-3000R frames start two frames in (our decoded output the same; the front end one more)
    try:
        from pesq import pesq as pesq_fn
    except ImportError:
        pesq_fn = None
    def frame_db(v):
        n = len(v) // 160
        return np.array([20 * np.log10(np.sqrt(np.mean(v[i * 160:(i + 1) * 160] ** 2)) / 32767 + 1e-9) for i in range(n)])
    clean_db = frame_db(sp)
    pauses = clean_db < -34    # the file's own pauses (room noise about -38 dBFS)
    speech = clean_db > -28
    def score(seg):
        db = frame_db(seg)
        n = min(len(db), len(clean_db))
        p, s_ = db[:n][pauses[:n]].mean(), db[:n][speech[:n]].mean()
        m = min(len(seg), len(sp))
        q = pesq_fn(FS, sp[:m], seg[:m].astype(np.int16).astype(float), "nb") if pesq_fn else float("nan")
        return f"{p:6.1f}/{s_:6.1f}/{q:4.2f}"
    print(f"{'section':22s} " + " ".join(f"{k:>20s}" for k in outs) + "   (pause dBFS / speech dBFS / PESQ vs clean)")
    pos = 0
    for label, samples, noise in sections:
        line = f"{label:22s} "
        for k, v in outs.items():
            seg = v[pos + delay:pos + delay + len(samples)]
            if len(samples) == len(sp):
                line += f"{score(seg):>20s} "
            else:
                line += f"{frame_db(seg).mean():6.1f}/  ---- / ---- "
        print(line)
        pos += len(samples) + len(gap)
    return 0


if __name__ == "__main__":
    sys.exit(main())
