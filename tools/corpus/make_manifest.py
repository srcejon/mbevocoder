#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write testdata/MANIFEST.sha256 and pack the release bundle mbevocoder-testdata.tar.gz.

  python tools/corpus/make_manifest.py [--bundle out.tar.gz]

The manifest lists every file under testdata/ (corpus audio, hardware-decoded PCM). CMake
verifies present files against it (cmake/FetchTestData.cmake). The bundle is what gets
attached to the `testdata-vN` GitHub release; publishing it is a manual step.
"""
import argparse
import hashlib
import sys
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TESTDATA = ROOT / "testdata"


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bundle", type=Path, default=None)
    args = ap.parse_args()
    files = sorted(p for p in TESTDATA.rglob("*") if p.is_file() and p.name != "MANIFEST.sha256")
    lines = []
    total = 0
    for p in files:
        rel = p.relative_to(TESTDATA).as_posix()
        lines.append(f"{sha256(p)}  {rel}\n")
        total += p.stat().st_size
    (TESTDATA / "MANIFEST.sha256").write_text("".join(lines))
    print(f"{len(files)} files, {total / 1e6:.1f} MB, manifest written")
    if args.bundle:
        with tarfile.open(args.bundle, "w:gz") as t:
            t.add(TESTDATA / "MANIFEST.sha256", arcname="MANIFEST.sha256")
            for p in files:
                t.add(p, arcname=p.relative_to(TESTDATA).as_posix())
        print(f"bundle {args.bundle} ({args.bundle.stat().st_size / 1e6:.1f} MB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
