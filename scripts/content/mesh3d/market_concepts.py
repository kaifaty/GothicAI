#!/usr/bin/env python3
"""Retain the market set's image_gen concepts unchanged and register them.

    uv run --no-project --with pillow python scripts/content/mesh3d/market_concepts.py \
        --target cart --prompt-file brief.md --utc <ISO> --view 34=/abs/a.png --view front=/abs/b.png [...]

The first view is made from text only; later views are edits of it and record
edit_of. Same checks and storage as concepts.py, under the mesh3d-market set and
the targets of data/content/mesh3d-market.json. Backs up sources afterwards.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import concepts  # noqa: E402

concepts.SOURCE_SET = "mesh3d-market"
TARGETS = Path(__file__).resolve().parents[3] / "data/content/mesh3d-market.json"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--target", required=True)
    parser.add_argument("--prompt-file", type=Path, required=True)
    parser.add_argument("--utc", required=True)
    parser.add_argument("--variant", type=int, default=1)
    parser.add_argument("--view", action="append", required=True, help="VIEW=PNG, the text-only view first")
    parser.add_argument("--registry", type=Path, default=concepts.registry_module.DEFAULT_REGISTRY)
    parser.add_argument("--source-root", type=Path, default=concepts.registry_module.DEFAULT_SOURCE_ROOT)
    parser.add_argument("--no-backup", action="store_true")
    args = parser.parse_args()
    target = next(t for t in json.loads(TARGETS.read_text()) if t["id"] == args.target)
    prompt = args.prompt_file.read_text(encoding="utf-8").strip()
    views = [v.split("=", 1) for v in args.view]
    first = None
    for view, png in views:
        record = concepts.import_concept(target, Path(png), args.variant, prompt, args.utc, args.registry,
                                         args.source_root, view, first)
        first = first or record["id"]
        print(f"Imported {record['id']}: {record['source_sha256']}")
    if not args.no_backup:
        subprocess.run([str(Path(__file__).resolve().parents[1] / "backup-sources.sh"), "--registry",
                        str(args.registry), "--source-root", str(args.source_root)], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
