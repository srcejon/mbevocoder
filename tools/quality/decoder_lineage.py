#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The three decoders side by side on the AMBE-3000R's own frames: mbelib 1.x, mbelib-neo, mbevocoder.

  python tools/quality/decoder_lineage.py [--rates ambe2450 dstar] [--mbelib1 path/to/mbelib1-decode]
        [--neodec build/dvsi_harness/Release/neodec.exe] [--only NAME...]

For every corpus file the AMBE-3000R encoded (corpus/golden/<rate>/*.dvsi), decodes the frames with
mbelib 1.x (`tools/mbelib1-decode`, built from its sources with MBELIB1_SOURCE_DIR, fed
mbelib's ambe_fr[4][24] via `mbe-cli frbits`),
with mbelib-neo (its latest release, 2.6.5: `neodec` of tools/quality/dvsi_harness built against
an mbelib-neo checkout, fed the same frame text; its float output is scored unrounded, as it puts
out 25-35 dB less than mbe-cli's 16-bit scale) and with mbevocoder (the default build), and scores each against
the AMBE-3000R's own decode of the same frames (LSD, level) and against the original speech (PESQ,
ESTOI), the AMBE-3000R's decoder scored the same way as the reference. Writes
corpus/baselines/decoder_lineage.json and prints the table for the README.
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

import numpy as np
from scipy.io import wavfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare import find_tool, metrics, read_wav  # noqa: E402
from perceptual import score  # noqa: E402

FS = 8000
GAIN = 0.125   # mbe-cli decode's default: mbelib's float scale to 16-bit


def decode_mbelib1(exe: Path, tool: Path, dvsi: Path, rate: str, out: Path):
    fr_txt = out.with_suffix(".fr.txt")
    subprocess.run([str(tool), "frbits", "--rate", rate, str(dvsi), str(fr_txt)], check=True, capture_output=True)
    raw = out.with_suffix(".f32")
    subprocess.run([str(exe), rate, str(fr_txt), str(raw)], check=True, capture_output=True)
    x = np.fromfile(raw, dtype=np.float32) * GAIN
    wavfile.write(out, FS, np.clip(np.round(x), -32768, 32767).astype(np.int16))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rates", nargs="+", default=["ambe2450", "dstar"])
    ap.add_argument("--only", nargs="*", default=[])
    ap.add_argument("--mbelib1", type=Path, default=ROOT / "build/vs2026/tools/mbelib1-decode/Release/mbelib1-decode.exe")
    ap.add_argument("--neodec", type=Path, default=ROOT / "build/dvsi_harness/Release/neodec.exe")
    ap.add_argument("--out", type=Path, default=ROOT / "corpus/baselines/decoder_lineage.json")
    ap.add_argument("--workdir", type=Path, default=ROOT / "build/quality/lineage")
    args = ap.parse_args()
    tool = find_tool()
    args.workdir.mkdir(parents=True, exist_ok=True)
    dll = args.mbelib1 if args.mbelib1.exists() else None
    if dll is None:
        print(f"mbelib1-decode not found at {args.mbelib1} (configure with MBELIB1_SOURCE_DIR); skipping mbelib 1.x", file=sys.stderr)
    neo = args.neodec if args.neodec.exists() else None
    if neo is None:
        print(f"neodec not found at {args.neodec} (tools/quality/dvsi_harness); skipping mbelib-neo", file=sys.stderr)
    decoders = [("mbelib 1.x", "mbelib1"), ("mbelib-neo", "neo"), ("mbevocoder", "ours")]
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
            chip = read_wav(hw)
            ref = read_wav(orig)
            row = {"chip": score(ref, chip, None)}
            outs = {}
            if dll:
                p = args.workdir / f"{name}.{rate}.mbelib1.wav"
                decode_mbelib1(dll, tool, dvsi, rate, p)
                outs["mbelib1"] = p
            if neo:
                txt, raw = args.workdir / f"{name}.{rate}.neo.fr.txt", args.workdir / f"{name}.{rate}.neo.f32"
                subprocess.run([str(tool), "frbits", "--rate", rate, str(dvsi), str(txt)], check=True, capture_output=True)
                subprocess.run([str(neo), rate, str(txt), str(raw)], check=True, capture_output=True)
                outs["neo"] = np.fromfile(raw, dtype=np.float32).astype(np.float64) * GAIN / 32768.0
            p = args.workdir / f"{name}.{rate}.ours.wav"
            subprocess.run([str(tool), "decode", "--rate", rate, str(dvsi), str(p)], check=True, capture_output=True)
            outs["ours"] = p
            for key, path in outs.items():
                x = path if isinstance(path, np.ndarray) else read_wav(path)
                m = metrics(x, chip)
                s = score(ref, x, None)
                row[key] = {"lsd_db": m["lsd_db"], "level_db": m["level_db"], "pesq": s["pesq"], "estoi": s["estoi"]}
            rows[name] = row
            print(f"{rate} {name:26s} " + "  ".join(f"{k} {row[k]['pesq']:.2f}" + (f"/{row[k]['lsd_db']:.1f}dB" if k != "chip" else "") for k in row), flush=True)
        agg = {}
        for key in ["chip"] + [k for _, k in decoders]:
            vals = [r[key] for r in rows.values() if key in r]
            if not vals:
                continue
            agg[key] = {m: round(float(np.mean([v[m] for v in vals])), 3) for m in vals[0] if m != "lag" and vals[0][m] is not None}
        result["rates"][rate] = {"aggregate": agg, "files": rows}
        print(f"\n{rate}: {len(rows)} files")
        print(f"  {'decoder':12s} {'LSD vs chip':>12s} {'level':>7s} {'PESQ':>6s} {'ESTOI':>6s}")
        for label, key in [("AMBE-3000R", "chip")] + decoders:
            if key in agg:
                a = agg[key]
                print(f"  {label:12s} {a.get('lsd_db', float('nan')):12.2f} {a.get('level_db', float('nan')):7.1f} {a['pesq']:6.2f} {a['estoi']:6.3f}")
    args.out.write_text(json.dumps(result, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
