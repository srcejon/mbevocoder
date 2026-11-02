#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""How much of the encoder's gap to the AMBE-3000R is in each part of the analysis?

  python tools/quality/frontend_oracle.py [--rate ambe2450|dstar] [--only NAME...]
        [--parts pitch voicing gain shape ...]

Re-encodes every corpus file with parts of the analysis replaced by the AMBE-3000R's own values for
the same frame (`mbe-cli encode --oracle-file`; the AMBE-3000R's decoded model comes from the
*.chip.parms dumps of encoder_compare.py, frame-aligned by b0 agreement) and scores the
result like encoder_compare.py: LSD of our encode + our decode against the original and
against the AMBE-3000R's encode + decode. The difference to the unforced run bounds what a better
estimate of that part could gain; the parts combine.

  pitch    analyse at the AMBE-3000R's fundamental (the tracker's history is untouched)
  voicing  the AMBE-3000R's voicing pattern (b1)
  gain     the AMBE-3000R's frame gain (decoded gamma)
  shape    the AMBE-3000R's harmonic amplitudes, mean removed (implies pitch)
  all      everything: the residual is the quantiser and the frames the AMBE-3000R coded differently

With --pesq the base and oracle frames are also decoded on the AMBE-3000R (--port) and scored
by PESQ / ESTOI against the original (docs/encoder-quality.md, "Front-end oracles"); the AMBE-3000R's own encode + decode is the ceiling.
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare import find_tool, metrics, read_wav  # noqa: E402
from encoder_compare import parse_parms, best_offset  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
PARTS = ["pitch", "voicing", "gain", "shape", "all"]


def oracle_lines(ours, chip, offset, parts, frames="all", shape_keeps_pitch=False):
    """One line per frame of ours: period b1 gamma L [w0 log2Ml...]; counts forced frames.
    frames: all | voiced (the AMBE-3000R voices every band) | mixed (some) | unvoiced (none)."""
    lines = []
    forced = 0
    want = set(PARTS[:-1]) if "all" in parts else set(parts)
    if "shape" in want and not shape_keeps_pitch:
        want.add("pitch")
    for i in range(len(ours)):
        j = i + offset
        period, b1, gamma, shape = 0.0, -1, "nan", ""
        if 0 <= j < len(chip):
            bc, w0c, Lc, gc, tc, mlc, vvc = chip[j]
            usable = not tc and w0c > 0 and bc[0] < 120
            if usable and frames != "all" and Lc > 0:
                vf = float(np.sum(vvc[:Lc])) / Lc
                usable = (frames == "voiced" and vf >= 0.999) or (frames == "unvoiced" and vf <= 0.001) or (frames == "mixed" and 0.001 < vf < 0.999)
            if usable:
                if "pitch" in want:
                    period = 2 * np.pi / w0c
                if "voicing" in want:
                    b1 = bc[1]
                if "gain" in want:
                    gamma = f"{gc:.4f}"
                if "shape" in want:
                    shape = f" {w0c:.6f} " + " ".join(f"{v:.3f}" for v in mlc[:Lc])
                if period or b1 >= 0 or gamma != "nan" or shape:
                    forced += 1
        lines.append(f"{period:.4f} {b1} {gamma} {Lc if shape else 0}{shape}")
    return lines, forced


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rate", default="ambe2450")
    ap.add_argument("--workdir", type=Path, default=ROOT / "build/quality")
    ap.add_argument("--only", nargs="*", default=[])
    ap.add_argument("--parts", nargs="+", default=["pitch"], choices=PARTS)
    ap.add_argument("--gain-adjust", type=float, default=0.0)
    ap.add_argument("--pesq", action="store_true", help="also decode base and oracle frames on the AMBE-3000R and score PESQ/ESTOI")
    ap.add_argument("--frames", default="all", choices=["all", "voiced", "mixed", "unvoiced"], help="force only frames the AMBE-3000R coded as this class")
    ap.add_argument("--shape-keeps-pitch", action="store_true", help="the shape oracle keeps our pitch (resampled onto our grid)")
    ap.add_argument("--port", default="COM3")
    args = ap.parse_args()
    tool = find_tool()
    tag = "+".join(args.parts)
    rows = []
    for ours_p in sorted(args.workdir.glob(f"*.{args.rate}.ours.parms")):
        name = ours_p.name.split(f".{args.rate}.")[0]
        if args.only and name not in args.only: continue
        if name.startswith("syn_"): continue
        chip_p = args.workdir / f"{name}.{args.rate}.chip.parms"
        wav = ROOT / "testdata/corpus" / f"{name}.wav"
        hw = ROOT / "testdata/golden" / args.rate / f"{name}.hw.wav"
        if not (chip_p.exists() and wav.exists()): continue
        ours, chip = parse_parms(ours_p), parse_parms(chip_p)
        offset, _ = best_offset(ours, chip)
        lines, forced = oracle_lines(ours, chip, offset, args.parts, args.frames, args.shape_keeps_pitch)
        oracle_file = args.workdir / f"{name}.{args.rate}.oracle"
        oracle_file.write_text("\n".join(lines) + "\n")
        res = {"name": name, "forced": forced, "frames": len(ours)}
        inp = read_wav(wav)
        for kind, extra in (("base", []), ("oracle", ["--oracle-file", str(oracle_file)])):
            dvsi = args.workdir / f"{name}.{args.rate}.{kind}.dvsi"
            out = args.workdir / f"{name}.{args.rate}.{kind}.wav"
            subprocess.run([str(tool), "encode", "--rate", args.rate, "--gain-adjust", str(args.gain_adjust), *extra, str(wav), str(dvsi)],
                           check=True, capture_output=True)
            subprocess.run([str(tool), "decode", "--rate", args.rate, str(dvsi), str(out)], check=True, capture_output=True)
            x = read_wav(out)
            res[f"{kind}_lsd_in"] = metrics(x, inp)["lsd_db"]
            if hw.exists():
                res[f"{kind}_lsd_chip"] = metrics(x, read_wav(hw))["lsd_db"]
            if args.pesq:
                from hil_encoder import find_ambe3000
                from perceptual import score
                hwdec = args.workdir / f"{name}.{args.rate}.{kind}.chipdec.wav"
                r = subprocess.run([str(find_ambe3000()), "--port", args.port, "--rate", args.rate, "decode", str(dvsi), str(hwdec)],
                                   capture_output=True, text=True)
                if r.returncode != 0:
                    sys.exit(r.stderr)
                sc = score(inp, read_wav(hwdec), None)
                res[f"{kind}_pesq"] = sc["pesq"]; res[f"{kind}_estoi"] = sc["estoi"]
                if kind == "base" and hw.exists():
                    sc = score(inp, read_wav(hw), None)
                    res["chip_pesq"] = sc["pesq"]; res["chip_estoi"] = sc["estoi"]
        rows.append(res)
        print(f"{name:24s} forced {forced:4d}/{len(ours):4d}  vs input {res['base_lsd_in']:.2f} -> {res['oracle_lsd_in']:.2f}"
              + (f"  vs chip {res['base_lsd_chip']:.2f} -> {res['oracle_lsd_chip']:.2f}" if 'base_lsd_chip' in res else "")
              + (f"  PESQ through the AMBE-3000R {res['base_pesq']:.2f} -> {res['oracle_pesq']:.2f} (its own {res.get('chip_pesq', float('nan')):.2f})" if 'base_pesq' in res else ""), flush=True)
    if not rows:
        return 1
    print(f"\n{len(rows)} files, parts {tag}: forced {np.mean([r['forced'] / r['frames'] for r in rows]) * 100:.0f}% of frames")
    for key in ("lsd_in", "lsd_chip"):
        base = [r[f"base_{key}"] for r in rows if f"base_{key}" in r]
        orc = [r[f"oracle_{key}"] for r in rows if f"oracle_{key}" in r]
        if base:
            print(f"  {key:8s}: {np.mean(base):.3f} dB -> {np.mean(orc):.3f} dB (mean of per-file LSD)")
    for key in ("pesq", "estoi"):
        base = [r[f"base_{key}"] for r in rows if f"base_{key}" in r]
        orc = [r[f"oracle_{key}"] for r in rows if f"oracle_{key}" in r]
        chipv = [r[f"chip_{key}"] for r in rows if f"chip_{key}" in r]
        if base:
            print(f"  {key:8s}: {np.mean(base):.3f} -> {np.mean(orc):.3f} through the AMBE-3000R's decoder (its own encoder {np.mean(chipv):.3f})")
    (args.workdir / f"frontend_oracle_{args.rate}_{tag}.json").write_text(json.dumps(rows, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
