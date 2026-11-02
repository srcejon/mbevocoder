#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""IMBE cross-check: our IMBE 7200x4400 frames decoded by JMBE (sdrtrunk's Java decoder).

  python tools/quality/jmbe_crosscheck.py [--jmbe C:/path/to/jmbe] [--files NAME...] [--all] [--parms]

There is no IMBE hardware; a second, independently written decoder (JMBE, from the same
standard, not from mbelib's code) catches a systematic error in our encoder's frames that
mbelib-neo parity cannot. Needs a JDK on the PATH or at JAVA_HOME, and JMBE built with its
Gradle wrapper (`gradlew :codec:jar`, whose dependency jars land in ~/.gradle). Each file is
encoded with `mbe-cli encode --rate imbe7200`, decoded by JMBE (tools/quality/jmbe/
JmbeDecode.java) and by mbe-cli, and scored (PESQ, ESTOI, LSD) against the original and
against each other. Writes corpus/baselines/encoder_imbe_jmbe.json. With --parms both
decoders' models are dumped as well (JmbeDecode's third argument, `mbe-cli parms --rate
imbe7200`) and compared frame by frame: fundamental, harmonic count, voicing and the rms
difference of the log2 amplitudes, which is how the amplitude-prediction difference in
docs/decoder-quality.md was found.
"""
import argparse
import glob
import json
import os
import subprocess
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare import find_tool, metrics, read_wav  # noqa: E402
from perceptual import score  # noqa: E402

DEFAULT_FILES = ["OSR_uk_000_0020", "OSR_us_000_0010", "arctic_slt_arctic_a0009", "arctic_bdl_arctic_a0003",
                 "libri_1089-134686-0001", "libri_672-122797-0001", "OSR_uk_000_0027", "arctic_slt_arctic_a0001",
                 "libri_121-121726-0001", "arctic_bdl_arctic_a0009"]


def java_bin(name):
    """A JDK (javac present): JAVA_HOME if it is one, else the newest under Program Files."""
    home = os.environ.get("JAVA_HOME")
    cands = ([home] if home else []) + sorted(glob.glob("C:/Program Files/Java/jdk-*"), reverse=True)
    for cand in cands:
        if (Path(cand) / "bin/javac.exe").exists() and (Path(cand) / "bin" / f"{name}.exe").exists():
            return str(Path(cand) / "bin" / f"{name}.exe")
    return name


def classpath(jmbe: Path):
    jars = [jmbe / "codec/build/libs/jmbe-1.0.9.jar", jmbe / "api/build/libs/jmbe-api-1.0.9.jar"]
    cache = Path.home() / ".gradle/caches/modules-2/files-2.1"
    for dep in ("JTransforms-3.1.jar", "JLargeArrays-1.6.jar", "commons-math3-3.5.jar", "slf4j-api-1.7.25.jar", "slf4j-simple-1.7.25.jar"):
        found = glob.glob(str(cache / "**" / dep), recursive=True)
        if not found:
            sys.exit(f"{dep} not found under {cache}; build JMBE with its Gradle wrapper first")
        jars.append(Path(found[0]))
    for j in jars[:2]:
        if not j.exists():
            sys.exit(f"{j} not found; build JMBE with `gradlew :codec:jar`")
    return ";".join(str(j).replace("\\", "/") for j in jars)


def compare_parms(jmbe_path: Path, ours_path: Path) -> str:
    """Frame-by-frame agreement of the two decoders' models (JMBE's amplitudes are 2x ours)."""
    def load(p):
        out = []
        for line in p.read_text().splitlines():
            v = line.split()
            if len(v) < 4:
                continue
            out.append((float(v[1]), int(v[2]), v[3], np.array([float(x) for x in v[4:]])))
        return out
    j, o = load(jmbe_path), load(ours_path)
    n = min(len(j), len(o))
    dw = dl = dv = 0
    shape = []
    for a, b in zip(j[:n], o[:n]):
        if abs(a[0] - b[0]) > 1e-5:
            dw += 1
        if a[1] != b[1]:
            dl += 1
            continue
        if a[2] != b[2]:
            dv += 1
        r = np.log2(np.maximum(a[3], 1e-9) / np.maximum(2.0 * b[3], 1e-9))
        shape.append(np.std(r) * 6.02)
    return f"{n} frames, fundamental differs {dw}, harmonic count {dl}, voicing {dv}; amplitude shape {np.mean(shape):.2f} dB rms"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--jmbe", type=Path, default=ROOT.parent / "jmbe")
    ap.add_argument("--files", nargs="+", default=DEFAULT_FILES)
    ap.add_argument("--all", action="store_true", help="every speech file of the corpus")
    ap.add_argument("--workdir", type=Path, default=ROOT / "build/jmbe")
    ap.add_argument("--out", type=Path, default=ROOT / "corpus/baselines/encoder_imbe_jmbe.json")
    ap.add_argument("--parms", action="store_true", help="also compare the decoded models frame by frame")
    args = ap.parse_args()
    tool = find_tool()
    cp = classpath(args.jmbe)
    args.workdir.mkdir(parents=True, exist_ok=True)
    src = ROOT / "tools/quality/jmbe/JmbeDecode.java"
    if not (args.workdir / "JmbeDecode.class").exists() or (args.workdir / "JmbeDecode.class").stat().st_mtime < src.stat().st_mtime:
        subprocess.run([java_bin("javac"), "-cp", cp, "-d", str(args.workdir), str(src)], check=True)
    names = [p.stem for p in sorted((ROOT / "testdata/corpus").glob("*.wav")) if not p.stem.startswith("syn_")] if args.all else args.files
    files = {}
    for name in names:
        wav = ROOT / "testdata/corpus" / f"{name}.wav"
        if not wav.exists():
            continue
        frames = args.workdir / f"{name}.imbe"
        jw = args.workdir / f"{name}.jmbe.wav"
        ow = args.workdir / f"{name}.ours.wav"
        subprocess.run([str(tool), "encode", "--rate", "imbe7200", str(wav), str(frames)], check=True, capture_output=True)
        jp, op = args.workdir / f"{name}.jmbe.parms", args.workdir / f"{name}.ours.parms"
        subprocess.run([java_bin("java"), "-cp", f"{args.workdir};{cp}", "JmbeDecode", str(frames), str(jw)] + ([str(jp)] if args.parms else []),
                       check=True, capture_output=True)
        subprocess.run([str(tool), "decode", "--rate", "imbe7200", str(frames), str(ow)], check=True, capture_output=True)
        if args.parms:
            subprocess.run([str(tool), "parms", "--rate", "imbe7200", str(frames), str(op)], check=True, capture_output=True)
            print(f"{name:26s} models: " + compare_parms(jp, op))
        ref, j, o = read_wav(wav), read_wav(jw), read_wav(ow)
        sj, so = score(ref, j, None), score(ref, o, None)
        rec = {"jmbe_vs_input": {"pesq": sj["pesq"], "estoi": sj["estoi"], "lsd_db": metrics(j, ref)["lsd_db"]},
               "ours_vs_input": {"pesq": so["pesq"], "estoi": so["estoi"], "lsd_db": metrics(o, ref)["lsd_db"]},
               "jmbe_vs_ours": {"lsd_db": metrics(j, o)["lsd_db"], "pesq": score(o, j, None)["pesq"]}}
        files[name] = rec
        print(f"{name:26s} JMBE decode: PESQ {sj['pesq']:.2f} LSD {rec['jmbe_vs_input']['lsd_db']:.2f} | ours: PESQ {so['pesq']:.2f} LSD {rec['ours_vs_input']['lsd_db']:.2f} | JMBE vs ours LSD {rec['jmbe_vs_ours']['lsd_db']:.2f} PESQ {rec['jmbe_vs_ours']['pesq']:.2f}", flush=True)
    agg = {}
    for key in ("jmbe_vs_input", "ours_vs_input", "jmbe_vs_ours"):
        for k in files[next(iter(files))][key]:
            v = [f[key][k] for f in files.values()]
            agg[f"{key}.{k}"] = {"mean": round(float(np.mean(v)), 3), "median": round(float(np.median(v)), 3), "min": round(float(np.min(v)), 3)}
    print(f"\n{len(files)} files")
    for k, a in agg.items():
        print(f"  {k:28s} mean {a['mean']:7.3f}  median {a['median']:7.3f}  min {a['min']:7.3f}")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps({"aggregate": agg, "files": files}, indent=1))
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
