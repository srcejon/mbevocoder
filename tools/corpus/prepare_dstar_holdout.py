#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prepare the fixed D-STAR validation set from the existing LibriSpeech archive.

Eight speakers absent from the original 70-file corpus, two utterances each.
Selection was fixed before candidate scoring; do not retune on these recordings
and continue calling them held out. Requires ffmpeg and the corpus dependencies.
"""
import argparse
import hashlib
import json
from pathlib import Path
import tarfile

import numpy as np
from scipy.io import wavfile
from prepare import normalise, run_ffmpeg_to_8k

ROOT = Path(__file__).resolve().parents[2]
SPEAKERS = ["1221", "1284", "1580", "1995", "1188", "1320", "2300", "260"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, default=ROOT / "corpus/raw/librispeech_test_clean/test-clean.tar.gz")
    parser.add_argument("--out", type=Path, default=ROOT / "build/dstar-pitch/heldout")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    index = []
    with tarfile.open(args.archive) as archive:
        files = sorted(m.name for m in archive.getmembers() if m.isfile() and m.name.endswith(".flac"))
        for speaker in SPEAKERS:
            selected = [f for f in files if f"/test-clean/{speaker}/" in f][:2]
            if len(selected) != 2:
                raise ValueError(f"Missing recordings for speaker {speaker}")
            for member in selected:
                raw = args.out / Path(member).name
                raw.write_bytes(archive.extractfile(member).read())
                samples, metadata = normalise(run_ffmpeg_to_8k(raw, 12), -26)
                name = "heldout_" + raw.stem
                wav = args.out / f"{name}.wav"
                wavfile.write(wav, 8000, np.round(samples * 32767).astype(np.int16))
                index.append({"name": name, "member": member, "speaker": speaker,
                              "sha256": hashlib.sha256(wav.read_bytes()).hexdigest(),
                              "seconds": len(samples) / 8000, **metadata})
    report = {"selection": "First four unused speaker IDs of each sex, first two utterances each, first 12 seconds. Selected before scoring on 2026-09-22.",
              "files": index}
    (args.out / "index.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"Prepared {len(index)} recordings in {args.out}")


if __name__ == "__main__":
    main()
