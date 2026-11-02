#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""mbelib-neo's encoders (2.4 and later) against ours and the reference encoders on the corpus.

  python tools/quality/neo_encoders.py [--port COM3] [--rates ambe2450 dstar imbe7200] [--only NAME...]

For the 70 speech recordings: mbelib-neo's encoder (`neoenc` of tools/quality/dvsi_harness, built
against an mbelib-neo 2.4+ checkout, its frames packed by `mbe-cli frpack` bit for bit as it made
them), ours (`mbe-cli encode`) and the reference (the AMBE-3000R's encoder, corpus/golden; for IMBE
OpenBLIP25's, build/audio-quality-followup/imbe), each decoded by the AMBE-3000R and by our decoder
(AMBE rates) or by ours and JMBE (IMBE), scored against the input (PESQ-NB, ESTOI). The AMBE-3000R's
decodes of ours and of its own frames are the hardware-in-the-loop captures in testdata/golden
(`hil_encoder.py`); mbelib-neo's are made here. Prints the means and the paired differences
(± standard error) against ours; writes corpus/baselines/neo_encoders.json.
"""
import argparse
import json
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare import read_wav  # noqa: E402
from perceptual import score  # noqa: E402
from hil_encoder import find_ambe3000  # noqa: E402

CLI = (ROOT / "build/vs2026/tools/mbe-cli/Release/mbe-cli.exe").resolve()
NEOENC = (ROOT / "build/dvsi_harness/Release/neoenc.exe").resolve()
W = ROOT / "build/quality/neo_encoders"
REFERENCE = {"ambe2450": "chip", "dstar": "chip", "imbe7200": "openblip25"}
DECODERS = {"ambe2450": ("chip", "ours"), "dstar": ("chip", "ours"), "imbe7200": ("ours", "jmbe")}


def run(*a):
    subprocess.run([str(x) for x in a], check=True, capture_output=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="COM3")
    ap.add_argument("--rates", nargs="+", default=list(REFERENCE))
    ap.add_argument("--only", nargs="*", default=[])
    ap.add_argument("--out", type=Path, default=ROOT / "corpus/baselines/neo_encoders.json")
    args = ap.parse_args()
    if not NEOENC.exists():
        sys.exit(f"{NEOENC} not found (build tools/quality/dvsi_harness against mbelib-neo 2.4 or later)")
    names = sorted(p.stem for p in (ROOT / "testdata/corpus").glob("*.wav") if not p.stem.startswith("syn_"))
    if args.only:
        names = [n for n in names if n in args.only]
    chip = find_ambe3000() if any("chip" in DECODERS[r] for r in args.rates) else None
    jmbe = None
    if any("jmbe" in DECODERS[r] for r in args.rates):
        from jmbe_crosscheck import classpath, java_bin
        cp = classpath(ROOT.parent / "jmbe")
        jw = ROOT / "build/jmbe"
        if not (jw / "JmbeDecode.class").exists():
            jw.mkdir(parents=True, exist_ok=True)
            run(java_bin("javac"), "-cp", cp, "-d", jw, ROOT / "tools/quality/jmbe/JmbeDecode.java")
        jmbe = (java_bin("java"), f"{jw};{cp}")
    result = {"neo": "mbelib-neo encoder (neoenc)", "rates": {}}
    for rate in args.rates:
        d = W / rate
        d.mkdir(parents=True, exist_ok=True)
        ref = REFERENCE[rate]

        def bits(n, enc):
            if enc == "chip":
                return ROOT / "corpus/golden" / rate / f"{n}.dvsi"
            if enc == "openblip25":
                return ROOT / "build/audio-quality-followup/imbe" / f"{n}.reference.imbe"
            return d / f"{n}.{enc}.bits"

        def encode(n):
            src = ROOT / "testdata/corpus" / f"{n}.wav"
            if not bits(n, "neo").exists():
                txt = d / f"{n}.neo.fr.txt"
                run(NEOENC, rate, src, txt)
                run(CLI, "frpack", "--rate", rate, txt, bits(n, "neo"))
            if not bits(n, "ours").exists():
                run(CLI, "encode", "--rate", rate, src, bits(n, "ours"))

        with ThreadPoolExecutor(8) as ex:
            list(ex.map(encode, names))
        names_r = [n for n in names if bits(n, ref).exists()]

        def wav(n, enc, dec):
            if dec == "chip" and enc in ("chip", "ours"):
                return ROOT / "testdata/golden" / rate / (f"{n}.hw.wav" if enc == "chip" else f"{n}.ours.hw.wav")
            return d / f"{n}.{enc}.{dec}.wav"

        def decode_sw(job):
            n, enc, dec = job
            w = wav(n, enc, dec)
            if not w.exists():
                if dec == "ours":
                    run(CLI, "decode", "--rate", rate, bits(n, enc), w)
                else:
                    run(jmbe[0], "-cp", jmbe[1], "JmbeDecode", bits(n, enc), w)

        encs = ("neo", "ours", ref)
        with ThreadPoolExecutor(6) as ex:
            list(ex.map(decode_sw, [(n, e, dc) for n in names_r for e in encs for dc in DECODERS[rate] if dc != "chip"]))
        if "chip" in DECODERS[rate]:
            for n in names_r:   # the AMBE-3000R serially; only mbelib-neo's frames are new
                w = wav(n, "neo", "chip")
                if not w.exists():
                    run(chip, "--port", args.port, "--rate", rate, "decode", bits(n, "neo"), w)

        def scores(n):
            src = read_wav(ROOT / "testdata/corpus" / f"{n}.wav")
            out = {}
            for dc in DECODERS[rate]:
                for e in encs:
                    w = wav(n, e, dc)
                    if w.exists():
                        s = score(src, read_wav(w), None)
                        out[f"{e}.{dc}"] = (s["pesq"], s["estoi"])
            return n, out
        with ThreadPoolExecutor(6) as ex:
            files = dict(ex.map(scores, names_r))
        summary = {}
        print(f"{rate}: {len(files)} recordings, PESQ / ESTOI; difference against ours, paired")
        for dc in DECODERS[rate]:
            line = f"  decoded by {dc:5s}"
            for e in encs:
                k = f"{e}.{dc}"
                v = np.array([f[k] for f in files.values() if k in f and f"ours.{dc}" in f])
                o = np.array([f[f"ours.{dc}"] for f in files.values() if k in f and f"ours.{dc}" in f])
                if not len(v):
                    continue
                m, df = v.mean(0), v - o
                se = df.std(0, ddof=1) / np.sqrt(len(df)) if len(df) > 1 else np.zeros(2)
                summary[k] = {"files": len(v), "pesq": round(float(m[0]), 3), "estoi": round(float(m[1]), 4),
                              "pesq_vs_ours": round(float(df[:, 0].mean()), 3), "pesq_vs_ours_se": round(float(se[0]), 3),
                              "estoi_vs_ours": round(float(df[:, 1].mean()), 4), "estoi_vs_ours_se": round(float(se[1]), 4)}
                line += f"  {e} {m[0]:.3f} / {m[1]:.3f}" + ("" if e == "ours" else f" ({df[:, 0].mean():+.3f}±{se[0]:.3f} / {df[:, 1].mean():+.4f})")
            print(line, flush=True)
        result["rates"][rate] = {"reference": ref, "summary": summary, "files": files}
    args.out.write_text(json.dumps(result, indent=1) + "\n")
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
