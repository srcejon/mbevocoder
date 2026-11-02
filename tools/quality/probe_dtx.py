#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""How does the AMBE-3000R's VAD/DTX behave? Speech with pauses in noise through the chip.

  python tools/quality/probe_dtx.py --port COM3 [--rate ambe2450|dstar] [--cached] [--ours] [--speech NAME]

Builds one wav: a speech file with 0.6 s pauses inserted, repeated at several background
noise levels (none, -50, -40, -30, -20 dBFS white), then noise-only and digital-silence
sections. The AMBE-3000R encodes it with DTX enabled (ECMODE 0x0800); per frame the ECMODE_OUT
word (PKT_CHANFMT bit 0 makes the AMBE-3000R send it) gives VOICE_ACTIVE and `mbe-cli parms` gives the frame bits. Reports, per section, how
many frames are silence frames, how the decision follows the level and what the silence
frames contain, then decodes the AMBE-3000R's frames on the AMBE-3000R to hear its comfort noise. With
--ours the library's encoder (dtx on) is compared frame by frame.
"""
import argparse
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


def speech_with_pauses(x, pause_s=0.6, every_s=1.0):
    out = []
    n = int(every_s * FS)
    for i in range(0, len(x), n):
        out.append(x[i:i + n])
        out.append(np.zeros(int(pause_s * FS)))
    return np.concatenate(out)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--rate", default="ambe2450", choices=["ambe2450", "dstar"])
    ap.add_argument("--cached", action="store_true")
    ap.add_argument("--ours", action="store_true")
    ap.add_argument("--speech", default="OSR_uk_000_0020", help="corpus file for the speech sections")
    args = ap.parse_args()
    tool, chip = find_tool(), find_ambe3000()
    work = ROOT / "build/probe"
    work.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(7)
    sp = wavfile.read(ROOT / "testdata/corpus" / f"{args.speech}.wav")[1].astype(float)[:int(4.0 * FS)]
    base = speech_with_pauses(sp)
    sections = []
    gap = np.zeros(int(0.5 * FS))
    for lvl in (None, -50, -40, -30, -20):
        x = base.copy()
        if lvl is not None:
            x = x + rng.normal(0, 10 ** (lvl / 20) * 32767, len(x))
        sections.append((f"speech+pauses noise {lvl}", x))
    for lvl in (-60, -50, -45, -40, -35, -30, -25, -20):
        sections.append((f"noise only {lvl} dBFS", rng.normal(0, 10 ** (lvl / 20) * 32767, int(1.0 * FS))))
    sections.append(("digital silence", np.zeros(int(1.0 * FS))))
    x = np.concatenate([np.concatenate([s[1], gap]) for s in sections])
    tag = f"{args.rate}" if args.speech == "OSR_uk_000_0020" else f"{args.rate}_{args.speech}"
    wav = work / f"probe_dtx_{tag}.wav"
    wavfile.write(wav, FS, np.clip(x, -32768, 32767).astype(np.int16))
    dvsi = work / f"probe_dtx_{tag}.dvsi"
    if not (args.cached and dvsi.exists()):
        r = subprocess.run([str(chip), "--port", args.port, "--rate", args.rate, "--ecmode", "0x0800", "--ctl", "150001", "encode", str(wav), str(dvsi)],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stderr, file=sys.stderr); return 1
        r = subprocess.run([str(chip), "--port", args.port, "--rate", args.rate, "decode", str(dvsi), str(work / f"probe_dtx_{tag}.chip.wav")],
                           capture_output=True, text=True)
    cmode_path = Path(str(dvsi) + ".cmode")
    cmodes = [int(v) for v in cmode_path.read_text().split()] if cmode_path.exists() else []
    parms = work / f"probe_dtx_{tag}.parms"
    subprocess.run([str(tool), "parms", "--rate", args.rate, str(dvsi), str(parms)], check=True, capture_output=True)
    lines = [ln.split() for ln in parms.read_text().splitlines() if ln.strip()]
    raw = np.fromfile(dvsi, dtype=np.uint8).reshape(-1, 9 if args.rate == "dstar" else 9)
    ours = None
    if args.ours:
        odvsi = work / f"probe_dtx_{tag}.ours.dvsi"
        subprocess.run([str(tool), "encode", "--rate", args.rate, "--dtx", str(wav), str(odvsi), "--flags", str(work / f"probe_dtx_{tag}.ours.flags")],
                       check=True, capture_output=True)
        ours = [int(v) for v in (work / f"probe_dtx_{tag}.ours.flags").read_text().split()]
    delay = 2
    pos = 0
    agree = tot = 0
    for label, samples in sections:
        f0 = pos // 160 + delay
        f1 = (pos + len(samples)) // 160 + delay
        cm = cmodes[f0:f1]
        active = [(c >> 1) & 1 if c >= 0 else -1 for c in cm]
        runs = []
        for i, a in enumerate(active):
            if i == 0 or a != active[i - 1]:
                runs.append([a, 1])
            else:
                runs[-1][1] += 1
        runstr = " ".join(f"{'V' if a == 1 else ('s' if a == 0 else '?')}{n}" for a, n in runs)
        # what the silence frames look like: distinct raw byte patterns
        sil = [tuple(raw[i]) for i in range(f0, min(f1, len(raw))) if i - f0 < len(active) and active[i - f0] == 0]
        distinct = len(set(sil))
        line = f"{label:30s} frames {len(cm):3d} active {sum(1 for a in active if a == 1):3d} silence {sum(1 for a in active if a == 0):3d} ({distinct} distinct) | {runstr}"
        if ours is not None:
            o = ours[f0:f1]
            oa = [(v >> 1) & 1 for v in o]
            same = sum(1 for a, b in zip(active, oa) if a == b)
            agree += same; tot += len(active)
            oruns = []
            for i, a in enumerate(oa):
                if i == 0 or a != oa[i - 1]:
                    oruns.append([a, 1])
                else:
                    oruns[-1][1] += 1
            line += f"\n{'':30s} ours   active {sum(oa):3d} silence {len(oa) - sum(oa):3d} agree {same}/{len(active)} | " + " ".join(f"{'V' if a else 's'}{n}" for a, n in oruns)
        print(line)
        pos += len(samples) + len(gap)
    if ours is not None:
        print(f"\nframe agreement (voice/silence): {agree}/{tot} = {100 * agree / tot:.1f}%")
    # dump a few silence frames' parameter lines and bytes
    print("\nexample silence frames (raw bytes | parms line):")
    shown = 0
    for i, c in enumerate(cmodes):
        if c >= 0 and not (c >> 1) & 1 and i < len(lines):
            print(" ".join(f"{b:02x}" for b in raw[i]), "|", " ".join(lines[i][:8]), "| cmode", hex(c))
            shown += 1
            if shown >= 12:
                break
    return 0


if __name__ == "__main__":
    sys.exit(main())
