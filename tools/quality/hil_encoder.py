#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Hardware-in-the-loop encoder test: our AMBE+2 frames decoded by the AMBE-3000R.

  python tools/quality/hil_encoder.py --port COM3 [--only NAME...] [--speech-only]
        [--out corpus/baselines/encoder_hil.json] [--listening build/listening]

For each corpus file: encode with mbe-cli, decode the frames on the AMBE-3000R (stored as
testdata/golden/ambe2450/<name>.ours.hw.wav), then score against the original speech and
against the AMBE-3000R's own encode+decode (testdata/golden/ambe2450/<name>.hw.wav). With
--listening, writes a folder per file with input, AMBE-3000R encode+AMBE-3000R decode, ours encode+AMBE-3000R
decode and ours encode+our decode (level matched to the AMBE-3000R) for A/B listening.
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

import numpy as np
from scipy.io import wavfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare import find_tool, metrics, read_wav  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]


def find_ambe3000() -> Path:
    for p in [ROOT / "build/vs2026/tools/ambe3000/Release/ambe3000.exe", ROOT / "build/release/tools/ambe3000/ambe3000",
              ROOT / "build/release/tools/ambe3000/ambe3000.exe"]:
        if p.exists():
            return p
    sys.exit("ambe3000 tool not found")


def write_wav(p: Path, x: np.ndarray):
    wavfile.write(p, 8000, np.clip(np.round(x * 32767), -32768, 32767).astype(np.int16))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--only", nargs="*", default=[])
    ap.add_argument("--speech-only", action="store_true")
    ap.add_argument("--out", type=Path, default=None)
    ap.add_argument("--listening", type=Path, default=None)
    ap.add_argument("--workdir", type=Path, default=ROOT / "build/quality")
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--rate", default="ambe2450", choices=["ambe2450", "dstar"])
    args = ap.parse_args()
    tool, chip = find_tool(), find_ambe3000()
    args.workdir.mkdir(parents=True, exist_ok=True)
    files = {}
    for wav in sorted((ROOT / "testdata/corpus").glob("*.wav")):
        name = wav.stem
        if args.only and name not in args.only:
            continue
        if name.startswith("syn_silence") or name.startswith("syn_dc"):
            continue
        if args.speech_only and name.startswith("syn_"):
            continue
        rate = args.rate
        hw_chip = ROOT / "testdata/golden" / rate / f"{name}.hw.wav"
        if not hw_chip.exists():
            continue
        dvsi = args.workdir / f"{name}.{rate}.enc.dvsi"
        hw_ours = ROOT / "testdata/golden" / rate / f"{name}.ours.hw.wav"
        subprocess.run([str(tool), "encode", "--rate", rate, str(wav), str(dvsi)], check=True, capture_output=True)
        if args.force or not hw_ours.exists():
            r = subprocess.run([str(chip), "--port", args.port, "--rate", rate, "decode", str(dvsi), str(hw_ours)],
                               capture_output=True, text=True)
            if r.returncode != 0:
                print(r.stderr, file=sys.stderr)
                return 1
        inp, cc, oc = read_wav(wav), read_wav(hw_chip), read_wav(hw_ours)
        m_in, m_cc = metrics(oc, inp), metrics(oc, cc)
        m_ref = metrics(cc, inp)
        files[name] = {"ours_chipdec_vs_input": m_in, "ours_chipdec_vs_chip_chipdec": m_cc, "chip_chipdec_vs_input": m_ref}
        print(f"{name:32s} ours->chip vs input LSD {m_in['lsd_db']:.2f} (chip {m_ref['lsd_db']:.2f})  vs chip decode LSD {m_cc['lsd_db']:.2f}  level {m_in['level_db']:+.2f} dB")
        if args.listening:
            d = args.listening / rate / name
            d.mkdir(parents=True, exist_ok=True)
            write_wav(d / "1_input.wav", inp)
            write_wav(d / "2_chip_enc_chip_dec.wav", cc)
            write_wav(d / "3_ours_enc_chip_dec.wav", oc)
            ours_wav = args.workdir / f"{name}.{rate}.enc.wav"
            subprocess.run([str(tool), "decode", "--rate", rate, str(dvsi), str(ours_wav)], check=True, capture_output=True)
            oo = read_wav(ours_wav)
            oo *= (np.sqrt(np.mean(cc ** 2)) / (np.sqrt(np.mean(oo ** 2)) + 1e-9))
            write_wav(d / "4_ours_enc_ours_dec_levelmatched.wav", oo)
    agg = {}
    for key in ("ours_chipdec_vs_input", "ours_chipdec_vs_chip_chipdec", "chip_chipdec_vs_input"):
        for k in ("lsd_db", "mcd_db", "level_db"):
            v = [f[key][k] for f in files.values() if f[key][k] is not None]
            if v:
                agg[f"{key}.{k}"] = {"mean": round(float(np.mean(v)), 3), "median": round(float(np.median(v)), 3), "max": round(float(np.max(v)), 3)}
    print(f"{len(files)} files")
    for k, a in agg.items():
        print(f"  {k:42s} mean {a['mean']:7.3f}  median {a['median']:7.3f}  max {a['max']:7.3f}")
    out = args.out or (ROOT / "corpus/baselines" / f"encoder_hil_{args.rate}.json")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps({"rate": args.rate, "aggregate": agg, "files": files}, indent=1))
    args.out = out
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
