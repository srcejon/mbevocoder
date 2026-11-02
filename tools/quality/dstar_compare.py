#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare two D-STAR encoder builds through a common decoder, optionally hardware.

Example (run from the repository root):
  python tools/quality/dstar_compare.py --baseline path/to/base/mbe-cli.exe \
      --candidate path/to/new/mbe-cli.exe --out build/dstar-comparison

Use --input-dir and --only for held-out recordings. Input must be 8 kHz mono WAV.
Both encoders use default options. Hardware access is serial, on the explicit port.
Reports include executable, input, and encoded-frame hashes; existing golden files
are never replaced. PESQ/ESTOI are diagnostics, not a substitute for listening.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

os.environ.setdefault("OPENBLAS_NUM_THREADS", "1")
os.environ.setdefault("OMP_NUM_THREADS", "1")

from compare import read_wav
from perceptual import score

ROOT = Path(__file__).resolve().parents[2]


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(*args):
    result = subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=120)
    if result.returncode:
        raise RuntimeError(f"{args[0]} failed: {result.stderr}")
    return result.stdout


def summarise(rows, chain):
    result = {}
    for metric in ("pesq", "estoi"):
        pairs = [(row["baseline"][chain][metric], row["candidate"][chain][metric]) for row in rows.values()]
        differences = [b - a for a, b in pairs]
        result[metric] = {
            "baseline": sum(a for a, _ in pairs) / len(pairs),
            "candidate": sum(b for _, b in pairs) / len(pairs),
            "mean_delta": sum(differences) / len(pairs),
            "worst_delta": min(differences),
            "best_delta": max(differences),
            "wins": sum(d > 0 for d in differences),
            "losses": sum(d < 0 for d in differences),
            "ties": sum(d == 0 for d in differences),
        }
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--candidate", required=True, type=Path)
    parser.add_argument("--decoder", type=Path, help="Common decoder; defaults to baseline")
    parser.add_argument("--input-dir", type=Path, default=ROOT / "testdata/corpus")
    parser.add_argument("--only", nargs="+", help="Exact WAV stems")
    parser.add_argument("--manifest", type=Path, help="Prepared holdout index.json (files[].name)")
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--hardware-tool", type=Path)
    parser.add_argument("--port", help="Explicit serial port, e.g. COM3")
    args = parser.parse_args()
    if args.manifest and args.only:
        parser.error("Choose --manifest or --only")
    if args.manifest:
        args.only = [row["name"] for row in json.loads(args.manifest.read_text(encoding="utf-8"))["files"]]
        if not args.only:
            parser.error("Empty input manifest")
    if bool(args.hardware_tool) != bool(args.port):
        parser.error("--hardware-tool and --port must be supplied together")
    tools = {"baseline": args.baseline.resolve(), "candidate": args.candidate.resolve(),
             "decoder": (args.decoder or args.baseline).resolve()}
    if args.hardware_tool:
        tools["hardware"] = args.hardware_tool.resolve()
    for path in tools.values():
        if not path.is_file():
            parser.error(f"Executable not found: {path}")
    inputs = ([args.input_dir / f"{name}.wav" for name in args.only] if args.only else
              [p for p in sorted(args.input_dir.glob("*.wav")) if not p.stem.startswith("syn_")])
    if not inputs or any(not p.is_file() for p in inputs):
        parser.error("No inputs, or a requested input is missing")
    args.out.mkdir(parents=True, exist_ok=True)
    report = {"tools": {key: {"path": str(path), "sha256": sha256(path)} for key, path in tools.items()},
              "files": {}}
    if args.port:
        report["hardware_identify"] = run(tools["hardware"], "--port", args.port, "identify")
    for wav in inputs:
        reference = read_wav(wav)
        row = {"source": str(wav.resolve()), "source_sha256": sha256(wav), "seconds": len(reference) / 8000}
        for label in ("baseline", "candidate"):
            frames = args.out / f"{wav.stem}.{label}.dvsi"
            audio = args.out / f"{wav.stem}.{label}.software.wav"
            run(tools[label], "encode", "--rate", "dstar", wav, frames)
            run(tools["decoder"], "decode", "--rate", "dstar", frames, audio)
            row[label] = {"frames_sha256": sha256(frames), "software": score(reference, read_wav(audio), None)}
            if args.port:
                audio = args.out / f"{wav.stem}.{label}.hardware.wav"
                run(tools["hardware"], "--port", args.port, "--rate", "dstar", "decode", frames, audio)
                row[label]["hardware"] = score(reference, read_wav(audio), None)
                row[label]["hardware_audio_sha256"] = sha256(audio)
        report["files"][wav.stem] = row
        for chain in ("software", "hardware") if args.port else ("software",):
            report.setdefault("summary", {})[chain] = summarise(report["files"], chain)
        (args.out / "scores.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        delta = row["candidate"]["software"]["pesq"] - row["baseline"]["software"]["pesq"]
        print(f"{wav.stem}: software PESQ {delta:+.3f}", flush=True)
    print(json.dumps(report["summary"], indent=2))


if __name__ == "__main__":
    main()
