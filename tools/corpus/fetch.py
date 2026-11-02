#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Download the corpus sources listed in corpus/manifest.json into corpus/raw/.

Idempotent: files already present with a matching size are not re-downloaded.
Records sha256 of every downloaded file in corpus/raw/SHA256SUMS.
"""
import hashlib
import json
import os
import re
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "corpus" / "manifest.json"
RAW = ROOT / "corpus" / "raw"


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def download(url: str, dst: Path) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    req = urllib.request.Request(url, headers={"User-Agent": "curl/8.0", "Accept": "*/*"})
    with urllib.request.urlopen(req, timeout=60) as r:
        total = int(r.headers.get("Content-Length") or 0)
        if dst.exists() and total and dst.stat().st_size == total:
            print(f"  have {dst.name} ({total} bytes)")
            return
        tmp = dst.with_suffix(dst.suffix + ".part")
        done = 0
        with open(tmp, "wb") as f:
            while True:
                chunk = r.read(1 << 20)
                if not chunk:
                    break
                f.write(chunk)
                done += len(chunk)
                if total:
                    print(f"\r  {dst.name}: {done * 100 // total}%", end="", flush=True)
        print()
        tmp.replace(dst)


def main() -> int:
    manifest = json.loads(MANIFEST.read_text())
    only = set(sys.argv[1:])
    sums = {}
    for src in manifest["sources"]:
        if only and src["id"] not in only:
            continue
        print(f"[{src['id']}] {src['name']}")
        d = RAW / src["id"]
        if src["kind"] == "html-index":
            html = urllib.request.urlopen(urllib.request.Request(src["url"], headers={"User-Agent": "curl/8.0", "Accept": "*/*"}), timeout=60).read().decode("utf-8", "replace")
            links = re.findall(r'href="([^"]+\.wav)"', html)
            for rel in links:
                dst = d / os.path.basename(rel)
                download(src["base"] + rel, dst)
                sums[str(dst.relative_to(RAW)).replace(os.sep, "/")] = sha256(dst)
        elif src["kind"] == "tarball":
            dst = d / os.path.basename(src["url"])
            download(src["url"], dst)
            sums[str(dst.relative_to(RAW)).replace(os.sep, "/")] = sha256(dst)
        else:
            print(f"  unknown kind {src['kind']}", file=sys.stderr)
            return 1
    RAW.mkdir(parents=True, exist_ok=True)
    existing = {}
    p = RAW / "SHA256SUMS"
    if p.exists():
        for line in p.read_text().splitlines():
            h, _, name = line.partition("  ")
            existing[name] = h
    existing.update(sums)
    p.write_text("".join(f"{h}  {n}\n" for n, h in sorted(existing.items())))
    print(f"wrote {p}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
