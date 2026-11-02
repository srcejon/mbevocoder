#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build a decoder variant with synthesis experiment knobs and score it against the chip.

  python tools/quality/synth_experiment.py [--rate ambe2450] [--only NAME...] DEF=VALUE ...

Each argument is a compile definition for the knobs in src/decoder/mbelib.c (MBE_EXP_ENHANCE,
MBE_EXP_UNVOICED_GAIN, MBE_EXP_ENH_POWER, MBE_EXP_ENH_MAX, MBE_EXP_ENH_MIN, ...); the variant
is built in build/exp with the Visual Studio generator (Windows) or Ninja, then
synth_diag.py --pesq runs with it. Results accumulate in build/quality/synth_experiments.txt.
"""
import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rate", default="ambe2450")
    ap.add_argument("--only", nargs="*", default=[])
    ap.add_argument("--build", type=Path, default=ROOT / "build/exp")
    ap.add_argument("defs", nargs="*")
    args = ap.parse_args()
    defs = ";".join(args.defs)
    win = sys.platform == "win32"
    gen = ["-G", "Visual Studio 18 2026", "-A", "x64"] if win else ["-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"]
    subprocess.run(["cmake", "-S", str(ROOT), "-B", str(args.build), *gen, "-DMBEVOCODER_BUILD_TESTS=OFF", "-DMBEVOCODER_BUILD_BENCH=OFF",
                    f"-DMBEVOCODER_EXTRA_DEFINITIONS={defs}"], check=True, capture_output=True)
    subprocess.run(["cmake", "--build", str(args.build), "--config", "Release", "--target", "mbe-cli"], check=True, capture_output=True)
    tool = args.build / ("tools/mbe-cli/Release/mbe-cli.exe" if win else "tools/mbe-cli/mbe-cli")
    cmd = [sys.executable, str(ROOT / "tools/quality/synth_diag.py"), "--rate", args.rate, "--pesq", "--tool", str(tool)]
    if args.only:
        cmd += ["--only", *args.only]
    out = subprocess.run(cmd, check=True, capture_output=True, text=True).stdout
    print(out)
    log = ROOT / "build/quality/synth_experiments.txt"
    with log.open("a") as f:
        f.write(f"=== {args.rate} {defs or 'default'}\n{out}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
