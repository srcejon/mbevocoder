#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Score encoder variants by PESQ through the AMBE-3000R's decoder.

  python tools/quality/encoder_variants.py --port COM3 [--rate ambe2450] [--files NAME...]
        --variant "label:--floor 8" --variant "label2:--floor 16" ...

Each variant is a set of extra `mbe-cli encode` arguments (NAME=value tokens set environment
variables instead, for experiment knobs read by the encoder). Every file is encoded with them,
the frames are decoded on the AMBE-3000R, and the result is scored against the original
(PESQ, ESTOI) and against the AMBE-3000R's own encode + decode (LSD). The unmodified encoder
and the AMBE-3000R's own encoder are printed alongside. AMBE-3000R time: about 15 s per file
per variant.
"""
import argparse
import subprocess
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare import find_tool, metrics, read_wav  # noqa: E402
from hil_encoder import find_ambe3000  # noqa: E402
from perceptual import score  # noqa: E402

DEFAULT_FILES = ["OSR_uk_000_0020", "OSR_us_000_0010", "arctic_slt_arctic_a0009", "arctic_bdl_arctic_a0003",
                 "libri_1089-134686-0001", "libri_672-122797-0001", "OSR_uk_000_0027", "arctic_slt_arctic_a0001",
                 "libri_121-121726-0001", "arctic_bdl_arctic_a0009"]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM3")
    ap.add_argument("--rate", default="ambe2450")
    ap.add_argument("--files", nargs="+", default=DEFAULT_FILES)
    ap.add_argument("--variant", action="append", default=[], help='"label:extra mbe-cli encode args"')
    ap.add_argument("--workdir", type=Path, default=ROOT / "build/quality/variants")
    args = ap.parse_args()
    tool, chip = find_tool(), find_ambe3000()
    args.workdir.mkdir(parents=True, exist_ok=True)
    variants = [("base", [])] + [(v.split(":", 1)[0], v.split(":", 1)[1].split()) for v in args.variant]
    rows = {label: [] for label, _ in variants}
    chip_rows = []
    for name in args.files:
        wav = ROOT / "testdata/corpus" / f"{name}.wav"
        hw = ROOT / "testdata/golden" / args.rate / f"{name}.hw.wav"
        if not (wav.exists() and hw.exists()):
            continue
        ref, cc = read_wav(wav), read_wav(hw)
        sc = score(ref, cc, None)
        chip_rows.append((sc["pesq"], sc["estoi"]))
        line = f"{name:26s} AMBE-3000R {sc['pesq']:.2f}"
        for label, extra in variants:
            dvsi = args.workdir / f"{name}.{args.rate}.{label}.dvsi"
            import os
            env = dict(os.environ)
            cli = []
            for tok in extra:   # NAME=value tokens set environment variables (experiment knobs), the rest are mbe-cli arguments
                if "=" in tok and not tok.startswith("-"):
                    k, v = tok.split("=", 1); env[k] = v
                else:
                    cli.append(tok)
            subprocess.run([str(tool), "encode", "--rate", args.rate, *cli, str(wav), str(dvsi)], check=True, capture_output=True, env=env)
            dec = args.workdir / f"{name}.{args.rate}.{label}.chipdec.wav"
            r = subprocess.run([str(chip), "--port", args.port, "--rate", args.rate, "decode", str(dvsi), str(dec)], capture_output=True, text=True)
            if r.returncode != 0:
                sys.exit(r.stderr)
            x = read_wav(dec)
            s = score(ref, x, None)
            m = metrics(x, cc)
            rows[label].append((s["pesq"], s["estoi"], m["lsd_db"], m["level_db"]))
            line += f" | {label} {s['pesq']:.2f}"
        print(line, flush=True)
    print(f"\n{len(chip_rows)} files; PESQ / ESTOI against the original through the AMBE-3000R's decoder, LSD and level against its own encode + decode")
    print(f"  {'AMBE-3000R encoder':28s} PESQ {np.mean([r[0] for r in chip_rows]):.3f}  ESTOI {np.mean([r[1] for r in chip_rows]):.3f}")
    for label, _ in variants:
        r = np.array(rows[label])
        print(f"  {label:28s} PESQ {r[:, 0].mean():.3f}  ESTOI {r[:, 1].mean():.3f}  LSD {r[:, 2].mean():.2f} dB  level {r[:, 3].mean():+.2f} dB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
