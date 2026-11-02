#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the mbevocoder test corpus from the raw downloads.

  python tools/corpus/prepare.py            # speech from corpus/raw -> testdata/corpus
  python tools/corpus/prepare.py --synthetic  # also generate synthetic test signals

Output: testdata/corpus/<name>.wav (8 kHz, 16-bit mono, -26 dBov active level) and
testdata/corpus/INDEX.json. Needs ffmpeg (with libsoxr) on PATH, numpy and scipy.
"""
import argparse
import glob
import io
import json
import os
import subprocess
import sys
import tarfile
from pathlib import Path

import numpy as np
from scipy.io import wavfile

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "corpus" / "manifest.json"
RAW = ROOT / "corpus" / "raw"
WORK = ROOT / "corpus" / "work"
OUT = ROOT / "testdata" / "corpus"
FS = 8000


def run_ffmpeg_to_8k(src: Path, max_seconds: float | None) -> np.ndarray:
    """Decode anything ffmpeg understands to 8 kHz mono float32 via soxr."""
    cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", str(src)]
    if max_seconds:
        cmd += ["-t", f"{max_seconds}"]
    cmd += ["-ac", "1", "-af", "aresample=resampler=soxr:precision=28:osr=8000",
            "-f", "f32le", "-acodec", "pcm_f32le", "-"]
    data = subprocess.run(cmd, check=True, capture_output=True).stdout
    return np.frombuffer(data, dtype=np.float32).astype(np.float64)


def active_level_dbov(x: np.ndarray) -> float:
    """P.56-style active speech level: RMS over 20 ms frames within 40 dB of the loudest."""
    n = len(x) // 160 * 160
    frames = x[:n].reshape(-1, 160)
    rms = np.sqrt(np.mean(frames ** 2, axis=1)) + 1e-12
    thr = rms.max() / 10 ** (40 / 20)
    active = frames[rms > thr]
    lvl = np.sqrt(np.mean(active ** 2)) if len(active) else rms.max()
    return 20 * np.log10(lvl + 1e-12)  # x is in units of full scale (1.0 = 0 dBov)


def normalise(x: np.ndarray, target_dbov: float) -> tuple[np.ndarray, dict]:
    x = x - np.mean(x)
    lvl = active_level_dbov(x)
    gain = 10 ** ((target_dbov - lvl) / 20)
    y = x * gain
    info = {"input_level_dbov": round(lvl, 2), "gain_db": round(20 * np.log10(gain), 2), "clipped": False}
    peak = np.max(np.abs(y))
    if peak > 0.999:
        y = y * (0.999 / peak)
        info["clipped"] = True
        info["gain_db"] = round(20 * np.log10(gain * 0.999 / peak), 2)
    return y, info


def write(name: str, y: np.ndarray, meta: dict, index: list) -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    pcm = np.clip(np.round(y * 32767), -32768, 32767).astype(np.int16)
    wavfile.write(OUT / f"{name}.wav", FS, pcm)
    meta = dict(meta)
    meta.update({"file": f"{name}.wav", "seconds": round(len(pcm) / FS, 2)})
    index.append(meta)
    print(f"  {name}.wav  {meta['seconds']:6.2f} s  {meta.get('sex','-')}")


def extract_tarball(tar: Path, members: list[str]) -> list[Path]:
    """Extract selected members (by exact name) into WORK, return their paths."""
    out = []
    with tarfile.open(tar) as t:
        for m in members:
            dst = WORK / m
            if not dst.exists():
                t.extract(m, WORK, filter="data")
            out.append(dst)
    return out


def tar_names(tar: Path) -> list[str]:
    cache = WORK / (tar.name + ".names")
    if cache.exists():
        return cache.read_text().splitlines()
    with tarfile.open(tar) as t:
        names = [m.name for m in t.getmembers() if m.isfile()]
    WORK.mkdir(parents=True, exist_ok=True)
    cache.write_text("\n".join(names))
    return names


def prepare_osr(src: dict, target: float, index: list) -> None:
    files = sorted(glob.glob(str(RAW / src["id"] / "*.wav")))
    sel = src["select"]
    # Take an evenly spread subset so both speakers/lists on the page are represented.
    step = max(1, len(files) // sel["files"])
    files = files[::step][: sel["files"]]
    for f in files:
        x = run_ffmpeg_to_8k(Path(f), sel.get("max_seconds"))
        y, info = normalise(x, target)
        name = Path(f).stem.replace("_8k", "")
        write(name, y, {"source": src["id"], "origin": Path(f).name, "sex": "unknown", **info}, index)


def prepare_arctic(src: dict, target: float, index: list) -> None:
    tar = RAW / src["id"] / os.path.basename(src["url"])
    sel = src["select"]
    import fnmatch
    names = sorted(n for n in tar_names(tar) if fnmatch.fnmatch(n, sel["glob"]))[: sel["files"]]
    sex = "male" if src["id"].endswith("bdl") else "female"
    for p in extract_tarball(tar, names):
        x = run_ffmpeg_to_8k(p, None)
        y, info = normalise(x, target)
        write(f"{src['id']}_{p.stem}", y, {"source": src["id"], "origin": p.name, "sex": sex, **info}, index)


def prepare_librispeech(src: dict, target: float, index: list) -> None:
    tar = RAW / src["id"] / os.path.basename(src["url"])
    sel = src["select"]
    names = tar_names(tar)
    spk_txt = [n for n in names if n.endswith("SPEAKERS.TXT")][0]
    (spk_path,) = extract_tarball(tar, [spk_txt])
    speakers = {}  # id -> sex, for test-clean
    for line in spk_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith(";"):
            continue
        parts = [p.strip() for p in line.split("|")]
        if len(parts) >= 3 and parts[2] == "test-clean":
            speakers[parts[0]] = "male" if parts[1] == "M" else "female"
    chosen = []
    for sex, count in sel["speakers"].items():
        ids = sorted(s for s, x in speakers.items() if x == sex)
        # spread across the list rather than taking the first N
        step = max(1, len(ids) // count)
        chosen += [(s, sex) for s in ids[::step][:count]]
    for spk, sex in chosen:
        flacs = sorted(n for n in names if f"/test-clean/{spk}/" in n and n.endswith(".flac"))
        flacs = flacs[: sel["utterances_per_speaker"]]
        for p in extract_tarball(tar, flacs):
            x = run_ffmpeg_to_8k(p, sel.get("max_seconds"))
            y, info = normalise(x, target)
            write(f"libri_{p.stem}", y, {"source": src["id"], "origin": p.name, "sex": sex, **info}, index)


def synthetic(target: float, index: list) -> None:
    rng = np.random.default_rng(1234)
    t = np.arange(0, 3.0, 1 / FS)

    def pink(n):
        spec = np.fft.rfft(rng.standard_normal(n))
        f = np.fft.rfftfreq(n, 1 / FS)
        spec[1:] /= np.sqrt(f[1:])
        spec[0] = 0
        return np.fft.irfft(spec, n)

    def harm(f0, n=30, amp_mod=None):
        s = np.zeros_like(t)
        for h in range(1, n + 1):
            if f0 * h >= FS / 2:
                break
            s += np.sin(2 * np.pi * f0 * h * t + rng.uniform(0, 2 * np.pi)) / h
        if amp_mod is not None:
            s *= amp_mod
        return s / np.max(np.abs(s))

    sigs = {
        "syn_silence": np.zeros(int(2.0 * FS)),
        "syn_dc_offset": np.full(int(2.0 * FS), 0.1),
        "syn_white_noise": rng.standard_normal(len(t)),
        "syn_pink_noise": pink(len(t)),
        "syn_tone_1khz": np.sin(2 * np.pi * 1000 * t),
        "syn_sweep_100_3800": np.sin(2 * np.pi * (100 * t + (3700 / 2 / 3.0) * t * t)),
        "syn_harm_f0_70": harm(70, amp_mod=0.5 + 0.5 * np.sin(2 * np.pi * 2.5 * t)),
        "syn_harm_f0_120": harm(120, amp_mod=0.5 + 0.5 * np.sin(2 * np.pi * 3 * t)),
        "syn_harm_f0_250": harm(250, amp_mod=0.5 + 0.5 * np.sin(2 * np.pi * 4 * t)),
        "syn_harm_f0_400": harm(400, amp_mod=0.5 + 0.5 * np.sin(2 * np.pi * 5 * t)),
        "syn_dtmf_1": np.sin(2 * np.pi * 697 * t) + np.sin(2 * np.pi * 1209 * t),
        "syn_dtmf_9": np.sin(2 * np.pi * 852 * t) + np.sin(2 * np.pi * 1477 * t),
        "syn_impulses": (np.arange(len(t)) % 800 == 0).astype(float),
    }
    for name, x in sigs.items():
        if name in ("syn_silence", "syn_dc_offset"):
            y, info = x, {"input_level_dbov": None, "gain_db": 0.0, "clipped": False}
        else:
            y, info = normalise(x, target)
        write(name, y, {"source": "synthetic", "origin": name, "sex": "-", **info}, index)

    # Clipped speech and noisy speech are derived from the first prepared speech file, if any.
    speech = sorted(OUT.glob("OSR_us_*.wav"))
    if speech:
        _, s = wavfile.read(speech[0])
        s = s.astype(np.float64) / 32767
        y, info = normalise(np.clip(s * 8, -0.5, 0.5), target)
        write("syn_clipped_speech", y, {"source": "derived", "origin": speech[0].name, "sex": "-", **info}, index)
        for snr in (20, 10, 5):
            n = rng.standard_normal(len(s))
            n *= np.sqrt(np.mean(s ** 2) / np.mean(n ** 2)) / 10 ** (snr / 20)
            y, info = normalise(s + n, target)
            write(f"syn_speech_snr{snr}", y, {"source": "derived", "origin": speech[0].name, "sex": "-", "snr_db": snr, **info}, index)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--synthetic", action="store_true", help="also generate synthetic signals")
    ap.add_argument("--only", nargs="*", default=[], help="restrict to these source ids")
    args = ap.parse_args()
    manifest = json.loads(MANIFEST.read_text())
    target = float(manifest["target_level_dbov"])
    index = []
    if not args.only or any(o != "synthetic" for o in args.only):
        for src in manifest["sources"]:
            if args.only and src["id"] not in args.only:
                continue
            print(f"[{src['id']}]")
            if src["id"].startswith("osr_"):
                prepare_osr(src, target, index)
            elif src["id"].startswith("arctic_"):
                prepare_arctic(src, target, index)
            elif src["id"].startswith("librispeech"):
                prepare_librispeech(src, target, index)
    if args.synthetic:
        print("[synthetic]")
        synthetic(target, index)
    total = sum(m["seconds"] for m in index)
    idx_path = OUT / "INDEX.json"
    old = json.loads(idx_path.read_text()).get("files", []) if idx_path.exists() else []
    merged = {m["file"]: m for m in old}
    merged.update({m["file"]: m for m in index})
    idx_path.write_text(json.dumps({"sample_rate": FS, "target_level_dbov": target,
                                    "files": sorted(merged.values(), key=lambda m: m["file"])}, indent=1))
    print(f"{len(index)} files, {total / 60:.1f} min written to {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
