#!/usr/bin/env python3
"""Merge packed props (pack_prop.py outputs) into one OPENGOTHIC_CONTENT root for the market set.

    python3 scripts/content/mesh3d/market_pack.py --output <content-staging>/market-set/content \
        <packed root> [<packed root> ...]

Each packed root holds materials/<name>/, meshes/<name>/ and their two index files.
Folders are copied as they are; the indexes are merged, and a key claimed twice with
different values is an error. Nothing is written into the working content folder.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil


def merge(output: Path, roots: list[Path]) -> tuple[dict, dict]:
    materials, meshes = {}, {}
    for root in roots:
        for kind, index in (("materials", materials), ("meshes", meshes)):
            entries = json.loads((root / kind / f"{kind}.json").read_text())
            for key, value in entries.items():
                if index.get(key, value) != value:
                    raise ValueError(f"{kind}.json: {key} differs between packed roots")
                index[key] = value
            for folder in (root / kind).iterdir():
                if folder.is_dir():
                    target = output / kind / folder.name
                    shutil.rmtree(target, ignore_errors=True)
                    shutil.copytree(folder, target)
    for kind, index in (("materials", materials), ("meshes", meshes)):
        (output / kind).mkdir(parents=True, exist_ok=True)
        (output / kind / f"{kind}.json").write_text(json.dumps(dict(sorted(index.items())), indent=2) + "\n")
    return materials, meshes


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("roots", type=Path, nargs="+")
    args = parser.parse_args()
    output = args.output.expanduser()
    if output.resolve() == (Path.home() / ".local/share/gothic-ai/content").resolve():
        raise SystemExit("refusing to write into the working content folder")
    materials, meshes = merge(output, [r.expanduser() for r in args.roots])
    print(f"{output}: {len(meshes)} meshes, {len(materials)} material keys")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
