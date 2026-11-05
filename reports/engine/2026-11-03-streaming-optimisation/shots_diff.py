#!/usr/bin/env python3
"""Pixel comparison of streaming-shots runs (RGBA, as in msl-cache.md).

    uv run --no-project --with pillow --with numpy python shots_diff.py \
        --csv pairs.csv --masks /tmp/masks LABEL_A=/run/a LABEL_B=/run/b [LABEL_C=/run/c ...]

Compares every adjacent pair of the given runs (A↔B, C↔D, ...) over the PNG shots both contain.
A row per shot: changed pixels (|Δ| > 0 in any channel), their share, max and mean |Δ| over all channels.
With --masks, writes <a>-<b>-<shot>.png: white where pixels differ.
"""
import argparse
import csv
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
    p.add_argument("--csv", type=Path, help="append rows here (header written when the file is new)")
    p.add_argument("--masks", type=Path, help="directory for difference masks")
    p.add_argument("--host", default="mac")
    p.add_argument("runs", nargs="+", help="LABEL=DIR, in pairs")
    args = p.parse_args()
    if len(args.runs) % 2:
        p.error("runs come in pairs")
    runs = [r.split("=", 1) for r in args.runs]
    rows = []
    for (la, da), (lb, db) in zip(runs[0::2], runs[1::2]):
        for shot in SHOTS:
            fa, fb = Path(da) / f"{shot}.png", Path(db) / f"{shot}.png"
            if not fa.exists() or not fb.exists():
                continue
            changed, n, total, mx, mean = compare(fa, fb)
            rows.append({"host": args.host, "spot": shot, "a": la, "b": lb, "changed_px": n,
                         "changed_pct": f"{100.0 * n / total:.2f}", "max_abs": mx, "mean_abs": f"{mean:.4f}"})
            if args.masks and n:
                args.masks.mkdir(parents=True, exist_ok=True)
                Image.fromarray((changed * 255).astype(np.uint8)).save(args.masks / f"{la}-{lb}-{shot}.png")
    w = csv.DictWriter(sys.stdout, fieldnames=list(rows[0].keys()) if rows else ["spot"])
    w.writeheader()
    w.writerows(rows)
    if args.csv and rows:
        new = not args.csv.exists()
        with args.csv.open("a", newline="") as f:
            cw = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
            if new:
                cw.writeheader()
            cw.writerows(rows)
    return 0


if __name__ == "__main__":
    sys.exit(main())
