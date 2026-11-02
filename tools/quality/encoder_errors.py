#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The encoders' frames under channel errors (docs/encoder-quality.md, "Under channel errors").

  python tools/quality/encoder_errors.py [--rates ambe2450 dstar imbe7200] [--only N] [--port COM3]
        [--out corpus/baselines/encoder_errors.json]

hil_errors.py damages the AMBE-3000R's own frames; this asks whether our encoders' frames
survive the same channel as well as a reference encoder's. Per development recording, two
encoders' frames are damaged identically (the same flips at the same positions: uniform BER,
2-8 bit bursts in 5 % of frames, 5 % of frames replaced by random bits) and decoded by two
decoders, and every decode is scored (PESQ, ESTOI) against the original speech:

  AMBE+2, D-STAR: our encoder and the AMBE-3000R's (corpus/golden), decoded by the AMBE-3000R
                  and by ours (72 on-air bits per frame)
  IMBE 7200:      our encoder and OpenBLIP25's (build/audio-quality-followup/imbe), decoded by
                  ours and by JMBE (144 bits per frame, P25 Phase 1 layout)

What matters is the loss from each encoder's clean score, paired per recording.
"""
import argparse
import json
import subprocess
import sys
import threading
import zlib
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare import read_wav  # noqa: E402
from perceptual import score  # noqa: E402

CONDITIONS = [("clean", None, 0.0), ("ber1", "ber", 0.01), ("ber3", "ber", 0.03), ("ber5", "ber", 0.05),
              ("burst5", "burst", 0.05), ("lost5", "lost", 0.05)]
RATES = {  # bits per frame, reference encoder, decoders
    "ambe2450": (72, "chip", ("chip", "ours")),
    "dstar": (72, "chip", ("chip", "ours")),
    "imbe7200": (144, "openblip25", ("ours", "jmbe")),
}
MBE_CLI = ROOT / "build/vs2026/tools/mbe-cli/Release/mbe-cli.exe"


def damage(frames: np.ndarray, nbits: int, kind: str, p: float, seed: int) -> np.ndarray:
    """frames: (n, bytes) uint8; the same seed gives the same flips whatever the frames hold."""
    if kind is None:
        return frames.copy()
    rng = np.random.default_rng(seed)
    bits = np.unpackbits(frames, axis=1)[:, :nbits]
    n = len(bits)
    if kind == "ber":
        bits = bits ^ (rng.random(bits.shape) < p).astype(np.uint8)
    elif kind == "burst":
        for i in np.nonzero(rng.random(n) < p)[0]:
            length = int(rng.integers(2, 9))
            start = int(rng.integers(0, nbits - length))
            bits[i, start:start + length] ^= 1
    elif kind == "lost":
        for i in np.nonzero(rng.random(n) < p)[0]:
            bits[i] = rng.integers(0, 2, nbits, dtype=np.uint8)
    return np.packbits(bits, axis=1)


def read_frames(p: Path, nbytes: int) -> np.ndarray:
    raw = np.frombuffer(p.read_bytes(), dtype=np.uint8)
    return raw[:len(raw) // nbytes * nbytes].reshape(-1, nbytes).copy()


def run(*a):
    r = subprocess.run([str(x) for x in a], capture_output=True, text=True, timeout=900)
    if r.returncode:
        raise RuntimeError(f"{a}: {r.stderr[-400:]}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rates", nargs="+", default=list(RATES), choices=list(RATES))
    ap.add_argument("--only", type=int, default=0, help="every n-th recording, this many")
    ap.add_argument("--port", default="COM3")
    ap.add_argument("--mbe-cli", type=Path, default=MBE_CLI, help="the build whose encoder and decoder are 'ours'")
    ap.add_argument("--decoders", nargs="+", help="only these decoders (e.g. ours jmbe)")
    ap.add_argument("--workdir", type=Path, default=ROOT / "build/quality/encoder_errors")
    ap.add_argument("--out", type=Path, default=ROOT / "corpus/baselines/encoder_errors.json")
    args = ap.parse_args()
    cli = args.mbe_cli.resolve()
    args.workdir.mkdir(parents=True, exist_ok=True)
    names = sorted(p.stem for p in (ROOT / "testdata/corpus").glob("*.wav") if not p.stem.startswith("syn_"))   # speech only
    if args.only:
        names = names[::max(1, len(names) // args.only)][:args.only]

    chip = jmbe_cp = None
    if any("chip" in RATES[r][2] and (not args.decoders or "chip" in args.decoders) for r in args.rates):
        from hil_encoder import find_ambe3000
        chip = find_ambe3000()
    if "imbe7200" in args.rates:
        from jmbe_crosscheck import classpath, java_bin
        cp = classpath(ROOT.parent / "jmbe")
        jw = ROOT / "build/jmbe"
        if not (jw / "JmbeDecode.class").exists():
            run(java_bin("javac"), "-cp", cp, "-d", jw, ROOT / "tools/quality/jmbe/JmbeDecode.java")
        jmbe_cp = (java_bin("java"), f"{jw};{cp}")

    result = {"conditions": [c[0] for c in CONDITIONS], "rates": {}}
    for rate in args.rates:
        nbits, refname, decoders = RATES[rate]
        decoders = tuple(d for d in decoders if not args.decoders or d in args.decoders)
        nbytes = nbits // 8
        W = args.workdir / rate
        W.mkdir(exist_ok=True)
        jobs = []   # (name, encoder, cond, damaged frames path)
        for n in names:
            src = ROOT / "testdata/corpus" / f"{n}.wav"
            ref = (ROOT / "corpus/golden" / rate / f"{n}.dvsi") if refname == "chip" else \
                (ROOT / "build/audio-quality-followup/imbe" / f"{n}.reference.imbe")
            if not ref.exists():
                continue
            ours = W / f"{n}.ours.frames"
            if not ours.exists():
                run(cli, "encode", "--rate", rate, src, ours)
            for enc, fp in (("ours", ours), (refname, ref)):
                fr = read_frames(fp, nbytes)
                for cond, kind, p in CONDITIONS:
                    d = W / f"{n}.{enc}.{cond}.frames"
                    if not d.exists():
                        d.write_bytes(damage(fr, nbits, kind, p, zlib.crc32(f"{n}.{cond}".encode()) & 0xffffffff).tobytes())
                    jobs.append((n, enc, cond, d))

        def wav_of(d, dec):
            return d.with_suffix(f".{dec}.wav")

        def decode_sw(job, dec):
            d = job[3]
            w = wav_of(d, dec)
            if w.exists():
                return
            if dec == "ours":
                run(cli, "decode", "--rate", rate, d, w)
            elif dec == "jmbe":
                run(jmbe_cp[0], "-cp", jmbe_cp[1], "JmbeDecode", d, w)

        # the AMBE-3000R serially on its own thread, the software decoders alongside
        chip_thread = None
        if "chip" in decoders:
            def chip_all():
                for j in jobs:
                    w = wav_of(j[3], "chip")
                    if not w.exists():
                        run(chip, "--port", args.port, "--rate", rate, "decode", j[3], w)
            chip_thread = threading.Thread(target=chip_all)
            chip_thread.start()
        with ThreadPoolExecutor(6) as ex:
            list(ex.map(lambda jd: decode_sw(*jd), [(j, dec) for j in jobs for dec in decoders if dec != "chip"]))
        if chip_thread:
            chip_thread.join()

        def score_job(j):
            n, enc, cond, d = j
            src = read_wav(ROOT / "testdata/corpus" / f"{n}.wav")
            return j, {dec: score(src, read_wav(wav_of(d, dec)), None) for dec in decoders}

        S = {}
        with ThreadPoolExecutor(6) as ex:
            for (n, enc, cond, _), s in ex.map(score_job, jobs):
                S.setdefault(n, {}).setdefault(enc, {})[cond] = {dec: {"pesq": v["pesq"], "estoi": v["estoi"]} for dec, v in s.items()}

        # the loss from each encoder's clean score, and ours against the reference's, paired
        summary = {}
        print(f"\n{rate}: {len(S)} recordings; loss from clean (PESQ / ESTOI), ours against {refname}'s, paired")
        for dec in decoders:
            print(f"  decoded by {dec}")
            for cond, _, _ in CONDITIONS:
                row = {}
                for enc in ("ours", refname):
                    for m in ("pesq", "estoi"):
                        row[f"{enc}.{m}"] = float(np.mean([S[n][enc][cond][dec][m] for n in S]))
                        row[f"{enc}.{m}_loss"] = float(np.mean([S[n][enc][cond][dec][m] - S[n][enc]["clean"][dec][m] for n in S]))
                for m in ("pesq", "estoi"):
                    x = np.array([(S[n]["ours"][cond][dec][m] - S[n]["ours"]["clean"][dec][m])
                                  - (S[n][refname][cond][dec][m] - S[n][refname]["clean"][dec][m]) for n in S])
                    row[f"loss_diff.{m}"] = float(x.mean())
                    row[f"loss_diff.{m}_se"] = float(x.std(ddof=1) / np.sqrt(len(x))) if len(x) > 1 else 0.0
                summary[f"{dec}.{cond}"] = row
                print(f"    {cond:7s} ours {row['ours.pesq']:.3f} / {row['ours.estoi']:.3f}  {refname} {row[refname + '.pesq']:.3f} / {row[refname + '.estoi']:.3f}"
                      + ("" if cond == "clean" else
                         f"  loss ours {row['ours.pesq_loss']:+.3f} / {row['ours.estoi_loss']:+.4f}  {refname} {row[refname + '.pesq_loss']:+.3f} / {row[refname + '.estoi_loss']:+.4f}"
                         f"  difference {row['loss_diff.pesq']:+.3f}±{row['loss_diff.pesq_se']:.3f} / {row['loss_diff.estoi']:+.4f}±{row['loss_diff.estoi_se']:.4f}"),
                      flush=True)
        result["rates"][rate] = {"reference": refname, "decoders": list(decoders), "summary": summary, "files": S}
    args.out.write_text(json.dumps(result, indent=1))
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
