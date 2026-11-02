#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Structured listening (docs/encoder-quality.md): the material, the blind pairs, the key.

  python tools/quality/listening.py build --port COM3 [--files NAME...] [--rates ambe2450 dstar]
  python tools/quality/listening.py pairs  [--seed N]
  python tools/quality/listening.py reveal

build: for each file and rate, a folder build/listening/<rate>/<name> with the chains, all
level-matched to the AMBE-3000R's own encode + decode (2):
  1_input.wav                    the original speech
  2_chip_enc_chip_dec.wav        AMBE-3000R encoder and decoder
  3_ours_enc_chip_dec.wav        our encoder, AMBE-3000R decoder      (encoder comparison: 2 vs 3)
  4_ours_enc_ours_dec.wav        our encoder, our decoder             (end to end: 2 vs 4)
  5_chip_frames_ours_dec.wav     AMBE-3000R frames, our decoder       (decoder comparison: 2 vs 5)
  6_ber2_chip_dec.wav            AMBE-3000R frames at 2 % BER, AMBE-3000R decoder
  7_ber2_ours_dec.wav            the same damaged frames, our decoder (errors: 6 vs 7)
pairs: build/listening/pairs/pNN_A.wav and pNN_B.wav for every comparison, the assignment of
the AMBE-3000R's and our chain to A and B shuffled, the key in build/listening/pairs/key.json
(do not look), and a results sheet build/listening/pairs/results.md to fill in: for each pair
which is preferred (A, B or none) and what is heard, with the time in the file.
reveal: prints the key next to the filled-in sheet, and a summary per comparison.
"""
import argparse
import json
import random
import subprocess
import sys
from pathlib import Path

import numpy as np
from scipy.io import wavfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare import find_tool, read_wav  # noqa: E402
from hil_encoder import find_ambe3000  # noqa: E402
from hil_errors import damage, read_dvsi, write_dvsi  # noqa: E402

FS = 8000
DEFAULT_FILES = ["OSR_uk_000_0020", "OSR_us_000_0010", "arctic_slt_arctic_a0009", "arctic_bdl_arctic_a0003",
                 "libri_1089-134686-0001", "libri_672-122797-0001"]
COMPARISONS = [("decoder", "2_chip_enc_chip_dec.wav", "5_chip_frames_ours_dec.wav"),
               ("encoder", "2_chip_enc_chip_dec.wav", "3_ours_enc_chip_dec.wav"),
               ("end-to-end", "2_chip_enc_chip_dec.wav", "4_ours_enc_ours_dec.wav"),
               ("errors 2% BER", "6_ber2_chip_dec.wav", "7_ber2_ours_dec.wav")]


def write_wav(p: Path, x: np.ndarray):
    wavfile.write(p, FS, np.clip(np.round(x * 32767), -32768, 32767).astype(np.int16))


def match(x: np.ndarray, ref: np.ndarray) -> np.ndarray:
    return x * (np.sqrt(np.mean(ref ** 2)) / (np.sqrt(np.mean(x ** 2)) + 1e-9))


def chip(chip_tool, port, rate, cmd, src, dst):
    r = subprocess.run([str(chip_tool), "--port", port, "--rate", rate, cmd, str(src), str(dst)], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(r.stderr)


def build(args):
    tool, chip_tool = find_tool(), find_ambe3000()
    work = ROOT / "build/quality/listening"
    work.mkdir(parents=True, exist_ok=True)
    for rate in args.rates:
        for name in args.files:
            orig = ROOT / "testdata/corpus" / f"{name}.wav"
            golden = ROOT / "corpus/golden" / rate / f"{name}.dvsi"
            hw = ROOT / "testdata/golden" / rate / f"{name}.hw.wav"
            if not (orig.exists() and golden.exists() and hw.exists()):
                print(f"skip {name} ({rate}): missing corpus, golden or AMBE-3000R decode", file=sys.stderr)
                continue
            d = ROOT / "build/listening" / rate / name
            d.mkdir(parents=True, exist_ok=True)
            ref = read_wav(orig)
            cc = read_wav(hw)
            write_wav(d / "1_input.wav", match(ref, cc))
            write_wav(d / "2_chip_enc_chip_dec.wav", cc)
            # our encoder: frames decoded on the AMBE-3000R and by us
            dvsi = work / f"{name}.{rate}.ours.dvsi"
            subprocess.run([str(tool), "encode", "--rate", rate, str(orig), str(dvsi)], check=True, capture_output=True)
            oc_wav = work / f"{name}.{rate}.ours.chipdec.wav"
            chip(chip_tool, args.port, rate, "decode", dvsi, oc_wav)
            write_wav(d / "3_ours_enc_chip_dec.wav", match(read_wav(oc_wav), cc))
            oo_wav = work / f"{name}.{rate}.ours.oursdec.wav"
            subprocess.run([str(tool), "decode", "--rate", rate, str(dvsi), str(oo_wav)], check=True, capture_output=True)
            write_wav(d / "4_ours_enc_ours_dec.wav", match(read_wav(oo_wav), cc))
            # the AMBE-3000R's frames through our decoder
            co_wav = work / f"{name}.{rate}.chipframes.oursdec.wav"
            subprocess.run([str(tool), "decode", "--rate", rate, str(golden), str(co_wav)], check=True, capture_output=True)
            write_wav(d / "5_chip_frames_ours_dec.wav", match(read_wav(co_wav), cc))
            # 2 % BER on the AMBE-3000R's frames, both decoders
            damaged = work / f"{name}.{rate}.ber2.dvsi"
            write_dvsi(damaged, damage(read_dvsi(golden), "ber", 0.02, seed=7))
            bc_wav = work / f"{name}.{rate}.ber2.chipdec.wav"
            chip(chip_tool, args.port, rate, "decode", damaged, bc_wav)
            write_wav(d / "6_ber2_chip_dec.wav", match(read_wav(bc_wav), cc))
            bo_wav = work / f"{name}.{rate}.ber2.oursdec.wav"
            subprocess.run([str(tool), "decode", "--rate", rate, str(damaged), str(bo_wav)], check=True, capture_output=True)
            write_wav(d / "7_ber2_ours_dec.wav", match(read_wav(bo_wav), cc))
            print(f"{rate} {name}: 7 files in {d}", flush=True)


def pairs(args):
    rng = random.Random(args.seed)
    out = ROOT / "build/listening/pairs"
    out.mkdir(parents=True, exist_ok=True)
    for old in out.glob("p*_?.wav"):
        old.unlink()
    key = []
    n = 0
    sets = sorted(p for p in (ROOT / "build/listening").glob("*/*") if p.is_dir() and (p / "2_chip_enc_chip_dec.wav").exists())
    for d in sets:
        for label, chip_file, ours_file in COMPARISONS:
            if not (d / chip_file).exists() or not (d / ours_file).exists():
                continue
            n += 1
            chip_is_a = rng.random() < 0.5
            a, b = (chip_file, ours_file) if chip_is_a else (ours_file, chip_file)
            (out / f"p{n:02d}_A.wav").write_bytes((d / a).read_bytes())
            (out / f"p{n:02d}_B.wav").write_bytes((d / b).read_bytes())
            key.append({"pair": n, "rate": d.parent.name, "file": d.name, "comparison": label,
                        "A": "AMBE-3000R" if chip_is_a else "mbevocoder", "B": "mbevocoder" if chip_is_a else "AMBE-3000R"})
    (out / "key.json").write_text(json.dumps(key, indent=1))
    lines = ["# Listening results", "",
             "Blind pairs in build/listening/pairs (pNN_A.wav / pNN_B.wav). For each pair: which you prefer",
             "(A, B or - for no preference) and what you hear that differs, with the time in the file.",
             "Listen on headphones at one volume; play each pair at least twice; do not open key.json.", "",
             "| pair | prefer | what differs (time) |", "|---|---|---|"]
    lines += [f"| p{k['pair']:02d} |  |  |" for k in key]
    (out / "results.md").write_text("\n".join(lines) + "\n")
    print(f"{n} pairs in {out}; fill in {out / 'results.md'}, then: listening.py reveal")


def reveal(args):
    out = ROOT / "build/listening/pairs"
    key = {k["pair"]: k for k in json.loads((out / "key.json").read_text())}
    prefs = {}
    for ln in (out / "results.md").read_text().splitlines():
        if ln.startswith("| p"):
            cells = [c.strip() for c in ln.strip("|").split("|")]
            try:
                prefs[int(cells[0][1:])] = (cells[1], cells[2] if len(cells) > 2 else "")
            except ValueError:
                pass
    tally = {}
    print(f"{'pair':5s} {'rate':9s} {'file':26s} {'comparison':14s} {'A':11s} {'B':11s} {'prefer':7s} notes")
    for n, k in sorted(key.items()):
        pref, note = prefs.get(n, ("", ""))
        who = k["A"] if pref.upper() == "A" else (k["B"] if pref.upper() == "B" else "-")
        tally.setdefault(k["comparison"], {}).setdefault(who, 0)
        tally[k["comparison"]][who] += 1
        print(f"p{n:02d}  {k['rate']:9s} {k['file']:26s} {k['comparison']:14s} {k['A']:11s} {k['B']:11s} {who:7s} {note}")
    print("\npreferences per comparison:")
    for comp, t in tally.items():
        print(f"  {comp:14s} " + ", ".join(f"{w}: {c}" for w, c in sorted(t.items())))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["build", "pairs", "reveal"])
    ap.add_argument("--port", default="COM3")
    ap.add_argument("--rates", nargs="+", default=["ambe2450", "dstar"])
    ap.add_argument("--files", nargs="+", default=DEFAULT_FILES)
    ap.add_argument("--seed", type=int, default=20260918)
    args = ap.parse_args()
    {"build": build, "pairs": pairs, "reveal": reveal}[args.cmd](args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
