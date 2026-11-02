#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the prepared corpus through the AMBE-3000R and store golden vectors.

  python tools/corpus/capture_goldens.py --port COM3 [--rates dstar ambe2450] [--only NAME...]

For every testdata/corpus/<name>.wav and every rate:
  corpus/golden/<rate>/<name>.dvsi (+ .json)   AMBE-3000R encoder output, committed (small)
  testdata/golden/<rate>/<name>.hw.wav          AMBE-3000R decode of those frames, not committed

Files whose .dvsi already exists are skipped unless --force is given.
"""
import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CORPUS = ROOT / "testdata" / "corpus"
GOLDEN_FRAMES = ROOT / "corpus" / "golden"
GOLDEN_PCM = ROOT / "testdata" / "golden"


def find_tool() -> Path:
    for p in [ROOT / "build/vs2026/tools/ambe3000/Release/ambe3000.exe",
              ROOT / "build/release/tools/ambe3000/ambe3000",
              ROOT / "build/release/tools/ambe3000/ambe3000.exe",
              ROOT / "build/debug/tools/ambe3000/ambe3000"]:
        if p.exists():
            return p
    sys.exit("ambe3000 tool not found; build the project first")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--rates", nargs="+", default=["dstar", "ambe2450"])
    ap.add_argument("--only", nargs="*", default=[])
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--tool", type=Path, default=None)
    args = ap.parse_args()
    tool = args.tool or find_tool()

    files = sorted(CORPUS.glob("*.wav"))
    if args.only:
        files = [f for f in files if f.stem in args.only]
    if not files:
        sys.exit("no corpus files; run tools/corpus/prepare.py first")

    summary = {}
    t0 = time.time()
    for rate in args.rates:
        (GOLDEN_FRAMES / rate).mkdir(parents=True, exist_ok=True)
        (GOLDEN_PCM / rate).mkdir(parents=True, exist_ok=True)
        for wav in files:
            dvsi = GOLDEN_FRAMES / rate / f"{wav.stem}.dvsi"
            hw = GOLDEN_PCM / rate / f"{wav.stem}.hw.wav"
            if dvsi.exists() and hw.exists() and not args.force:
                continue
            print(f"[{rate}] {wav.name}", flush=True)
            r = subprocess.run([str(tool), "--port", args.port, "--rate", rate, "loopback",
                                str(wav), str(dvsi), str(hw)], capture_output=True, text=True)
            if r.returncode != 0:
                print(r.stderr, file=sys.stderr)
                return 1
            meta = json.loads((dvsi.with_suffix(".dvsi.json")).read_text())
            summary.setdefault(rate, {})[wav.stem] = meta["output_frames"]
    print(f"done in {(time.time() - t0) / 60:.1f} min")
    return 0


if __name__ == "__main__":
    sys.exit(main())
