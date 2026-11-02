#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""How does the AMBE-3000R's tone detector behave? Synthetic tones through the AMBE-3000R's encoder.

  python tools/quality/probe_tone_detect.py --port COM3 [--rate ambe2450|dstar] [--cached]
        [--tool mbe-cli] [--ours]

Builds one wav of sections (tone on, then silence): single tones on and between the tone-ID
frequencies, a level sweep at 1000 Hz, a duration sweep, DTMF at several twists, KNOX and
call-progress tones, tones in noise at several SNRs and a tone mixed with speech. The AMBE-3000R
encodes it with tone detection enabled (ECMODE 0x1000) and `mbe-cli parms` reads back, per
frame, whether a tone frame was sent, its id and amplitude. With --ours the library's
encoder (tone detection on) is run on the same wav and compared frame by frame.
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
from compare import find_tool  # noqa: E402
from hil_encoder import find_ambe3000  # noqa: E402

FS = 8000
DTMF = {"1": (697, 1209), "2": (697, 1336), "3": (697, 1477), "A": (697, 1633), "4": (770, 1209), "5": (770, 1336), "6": (770, 1477),
        "B": (770, 1633), "7": (852, 1209), "8": (852, 1336), "9": (852, 1477), "C": (852, 1633), "*": (941, 1209), "0": (941, 1336),
        "#": (941, 1477), "D": (941, 1633)}
KNOX = [(1162, 820), (1052, 606), (1162, 606), (1279, 606), (1052, 672), (1162, 672), (1279, 672), (1052, 743),
        (1162, 743), (1279, 743), (1430, 606), (1430, 672), (1430, 743), (1430, 820), (1052, 820), (1279, 820)]
KNOX_OFF = [(820, 1305)]   # 26 Hz off a KNOX pair: the AMBE-3000R still codes it (as 1279 + 820)
CALL = {"dial": (350, 440), "ring": (440, 480), "busy": (480, 620), "reorder": (350, 490)}


def tone(f1, f2, seconds, level_db, twist_db=0.0, noise_db=None):
    t = np.arange(int(seconds * FS)) / FS
    a = 10 ** (level_db / 20) * 32767
    if f2 and abs(f2 - f1) > 1:
        a2 = a * 10 ** (twist_db / 20)
        x = 0.5 * a * np.sin(2 * np.pi * f1 * t) + 0.5 * a2 * np.sin(2 * np.pi * f2 * t)
    else:
        x = a * np.sin(2 * np.pi * f1 * t)
    if noise_db is not None:
        x = x + np.random.default_rng(1).normal(0, 10 ** (noise_db / 20) * 32767 / np.sqrt(2), len(t))
    return x


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--rate", default="ambe2450", choices=["ambe2450", "dstar"])
    ap.add_argument("--cached", action="store_true")
    ap.add_argument("--tool", type=Path, default=None)
    ap.add_argument("--ours", action="store_true", help="also run our encoder with tone detection and compare")
    args = ap.parse_args()
    tool, chip = args.tool or find_tool(), find_ambe3000()
    work = ROOT / "build/probe"
    work.mkdir(parents=True, exist_ok=True)
    sections = []   # (label, expected id or None, samples)
    gap = np.zeros(int(0.3 * FS))
    ids = [5, 6] + list(range(7, 123, 3))
    for i in ids:
        f = 156.25 if i == 5 else (187.5 if i == 6 else 31.25 * i)
        sections.append((f"single {f:.2f} Hz id {i}", i, tone(f, 0, 0.6, -20)))
    for f in (500 + 15.6, 1000 + 10, 2000 + 15.6, 3000 + 8):   # between ids
        sections.append((f"offgrid {f:.1f} Hz", None, tone(f, 0, 0.6, -20)))
    for lvl in (-45, -40, -35, -30, -25, -20, -15, -10, -6, -3):
        sections.append((f"level {lvl} dBFS 1000 Hz", 32, tone(1000, 0, 0.6, lvl)))
    for ms in (40, 60, 80, 100, 140, 200):
        sections.append((f"duration {ms} ms 1000 Hz", 32, tone(1000, 0, ms / 1000, -20)))
    for k, (r, c) in DTMF.items():
        sections.append((f"dtmf {k}", k, tone(r, c, 0.6, -20)))
    for tw in (-10, -6, 6, 10):
        r, c = DTMF["5"]
        sections.append((f"dtmf 5 twist {tw:+d} dB", "5", tone(r, c, 0.6, -20, tw)))
    for j, (a, b) in enumerate(KNOX):
        sections.append((f"knox {j}", 144 + j, tone(a, b, 0.6, -20)))
    for a, b in KNOX_OFF:
        sections.append((f"knox off {a}+{b}", None, tone(a, b, 0.6, -20)))
    for name, (a, b) in CALL.items():
        sections.append((f"call {name}", name, tone(a, b, 0.6, -20)))
    for snr in (30, 20, 15, 10, 5, 0):
        sections.append((f"snr {snr} dB 1000 Hz", 32, tone(1000, 0, 0.6, -20, noise_db=-20 - snr)))
    speech = ROOT / "testdata/corpus/OSR_uk_000_0020.wav"
    if speech.exists():
        sp = wavfile.read(speech)[1].astype(float)[:int(1.2 * FS)]
        sections.append(("speech only", None, sp))
        sections.append(("speech + 1000 Hz -20 dBFS", 32, sp + tone(1000, 0, 1.2, -20)))
    x = np.concatenate([np.concatenate([s[2], gap]) for s in sections])
    wav = work / f"probe_tone_detect_{args.rate}.wav"
    wavfile.write(wav, FS, np.clip(x, -32768, 32767).astype(np.int16))
    dvsi = work / f"probe_tone_detect_{args.rate}.dvsi"
    if not (args.cached and dvsi.exists()):
        r = subprocess.run([str(chip), "--port", args.port, "--rate", args.rate, "--ecmode", "0x1000", "encode", str(wav), str(dvsi)],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stderr, file=sys.stderr); return 1
    parms = work / f"probe_tone_detect_{args.rate}.parms"
    subprocess.run([str(tool), "parms", "--rate", args.rate, str(dvsi), str(parms)], check=True, capture_output=True)
    frames = [ln.split() for ln in parms.read_text().splitlines() if ln.strip()]
    tones = [(int(f[-2]), int(f[-1])) for f in frames]   # (id, amp), -1 for voice
    ours = None
    if args.ours:
        odvsi = work / f"probe_tone_detect_{args.rate}.ours.dvsi"
        subprocess.run([str(tool), "encode", "--rate", args.rate, "--tone-detect", str(wav), str(odvsi)], check=True, capture_output=True)
        oparms = work / f"probe_tone_detect_{args.rate}.ours.parms"
        subprocess.run([str(tool), "parms", "--rate", args.rate, str(odvsi), str(oparms)], check=True, capture_output=True)
        ours = [(int(f[-2]), int(f[-1])) for f in (ln.split() for ln in oparms.read_text().splitlines() if ln.strip())]
    # walk the sections: the AMBE-3000R's frames are delayed; align by the section start
    delay = 2   # frames of encoder delay (the AMBE-3000R's 34 ms and the section gap absorb the rest)
    pos = 0
    report = []
    agree = tot = 0
    for label, expect, samples in sections:
        f0 = pos // 160 + delay
        f1 = (pos + len(samples)) // 160 + delay
        seg = tones[f0:f1]
        tone_frames = [t for t in seg if t[0] >= 0]
        ids_seen = sorted(set(t[0] for t in tone_frames))
        amps = sorted(set(t[1] for t in tone_frames))
        first = next((i for i, t in enumerate(seg) if t[0] >= 0), None)
        last = next((len(seg) - 1 - i for i, t in enumerate(reversed(seg)) if t[0] >= 0), None)
        line = f"{label:34s} chip: {len(tone_frames):2d}/{len(seg):2d} tone frames, ids {ids_seen}, amps {amps[:3]}{'...' if len(amps) > 3 else ''}, first {first} last {last}"
        if ours is not None:
            oseg = ours[f0:f1]
            oframes = [t for t in oseg if t[0] >= 0]
            oids = sorted(set(t[0] for t in oframes)); oamps = sorted(set(t[1] for t in oframes))
            same = sum(1 for a, b in zip(seg, oseg) if (a[0] >= 0) == (b[0] >= 0) and (a[0] < 0 or a[0] == b[0]))
            agree += same; tot += len(seg)
            line += f" | ours: {len(oframes):2d} frames, ids {oids}, amps {oamps[:3]}, agree {same}/{len(seg)}"
        report.append(line)
        pos += len(samples) + len(gap)
    print("\n".join(report))
    if ours is not None:
        print(f"\nframe agreement (tone/voice and id): {agree}/{tot} = {100 * agree / tot:.1f}%")
    (work / f"probe_tone_detect_{args.rate}.txt").write_text("\n".join(report) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
