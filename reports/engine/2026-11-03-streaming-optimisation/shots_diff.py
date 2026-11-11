#!/usr/bin/env python3
"""Pixel comparison of streaming-shots runs (RGBA, as in msl-cache.md).

    uv run --no-project --with pillow --with numpy python shots_diff.py \
        --masks /tmp/masks LABEL_A=/run/a LABEL_B=/run/b [LABEL_C=/run/c ...]

Compares every adjacent pair of the given runs (A↔B, C↔D, ...) over all streaming-shots PNGs;
with --all-pairs, every pair among the given runs (five runs give ten pairs).
A shot missing on either side is an error (exit 1).
A row per shot: changed pixels (|Δ| > 0 in any channel), their share, max and mean |Δ| over all channels.
With --masks, writes <a>-<b>-<shot>.png: white where pixels differ.
"""
import argparse
import csv
import itertools
import sys
from pathlib import Path

import numpy as np
from PIL import Image

SHOTS = ["perf-market", "perf-harbour", "perf-forest", "perf-island", "perf-market-teleport"]


def compare(a: Path, b: Path):
    x = np.asarray(Image.open(a).convert("RGBA"), dtype=np.int16)
    y = np.asarray(Image.open(b).convert("RGBA"), dtype=np.int16)
    if x.shape != y.shape:
        raise ValueError(f"{a} {x.shape} vs {b} {y.shape}")
    d = np.abs(x - y)
    changed = d.max(axis=2) > 0
    return changed, int(changed.sum()), changed.size, int(d.max()), float(d.mean())


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--masks", type=Path, help="directory for difference masks")
    p.add_argument("--host", default="mac")
    p.add_argument("--all-pairs", action="store_true", help="compare every pair among the runs")
    p.add_argument("--shots", help="comma-separated shot names instead of the streaming-shots ones "
                                   "(scripts/bench/texture-owners-shots.bench: see its header)")
    p.add_argument("runs", nargs="+", help="LABEL=DIR, in pairs")
    args = p.parse_args()
    if not args.all_pairs and len(args.runs) % 2:
        p.error("runs come in pairs")
    runs = [r.split("=", 1) for r in args.runs]
    shots = [name for name in args.shots.split(",") if name] if args.shots else SHOTS
    pairs = itertools.combinations(runs, 2) if args.all_pairs else zip(runs[0::2], runs[1::2])
    rows = []
    for (la, da), (lb, db) in pairs:
        for shot in shots:
            fa, fb = Path(da) / f"{shot}.png", Path(db) / f"{shot}.png"
            if not fa.exists() or not fb.exists():
                missing = [str(f) for f in (fa, fb) if not f.exists()]
                print(f"missing shot: {', '.join(missing)}", file=sys.stderr)
                return 1
            changed, n, total, mx, mean = compare(fa, fb)
            rows.append({"host": args.host, "spot": shot, "a": la, "b": lb, "changed_px": n,
                         "changed_pct": f"{100.0 * n / total:.2f}", "max_abs": mx, "mean_abs": f"{mean:.4f}"})
            if args.masks and n:
                args.masks.mkdir(parents=True, exist_ok=True)
                Image.fromarray((changed * 255).astype(np.uint8)).save(args.masks / f"{la}-{lb}-{shot}.png")
    w = csv.DictWriter(sys.stdout, fieldnames=list(rows[0].keys()) if rows else ["spot"])
    w.writeheader()
    w.writerows(rows)
    return 0


if __name__ == "__main__":
    sys.exit(main())
