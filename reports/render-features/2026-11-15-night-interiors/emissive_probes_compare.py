#!/usr/bin/env python3
"""Compare two runs of emissive-probes.bench shot by shot.

    emissive_probes_compare.py <run A> <run B> [--sheet out.png]

Prints, per shot, how many pixels differ and where B is warmer and brighter than A (the glow of embers:
red rises more than blue). With --sheet writes "A | B | difference x8" crops around that area.
"""
import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image

SHOTS = ("side-close", "front-close", "side-1m", "front-1m")


def load(run: Path, shot: str) -> np.ndarray:
    files = sorted(run.rglob(f"{shot}.png"))
    if not files:
        sys.exit(f"no {shot}.png under {run}")
    return np.asarray(Image.open(files[0]).convert("RGB"), dtype=np.int16)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("a", type=Path)
    parser.add_argument("b", type=Path)
    parser.add_argument("--sheet", type=Path)
    args = parser.parse_args()

    rows = []
    print(f"{'shot':12} {'differ %':>9} {'mean |d|':>9} {'glow px':>8} {'glow dR':>8} {'box'}")
    for shot in SHOTS:
        a, b = load(args.a, shot), load(args.b, shot)
        if a.shape != b.shape:
            sys.exit(f"{shot}: sizes differ {a.shape} {b.shape}")
        d = b - a
        differ = (np.abs(d).max(axis=2) > 2).mean() * 100
        # embers: red up by more than 6 levels and by more than blue
        glow = (d[..., 0] > 6) & (d[..., 0] > d[..., 2] + 3)
        count = int(glow.sum())
        box = "-"
        mean_red = 0.0
        if count:
            ys, xs = np.nonzero(glow)
            box = f"x {xs.min()}..{xs.max()} y {ys.min()}..{ys.max()}"
            mean_red = float(d[..., 0][glow].mean())
        print(f"{shot:12} {differ:9.3f} {np.abs(d).mean():9.3f} {count:8d} {mean_red:8.1f} {box}")
        if args.sheet and count:
            ys, xs = np.nonzero(glow)
            cy, cx = int(np.median(ys)), int(np.median(xs))
            half = 90
            y0 = min(max(cy - half, 0), a.shape[0] - 2 * half)
            x0 = min(max(cx - half, 0), a.shape[1] - 2 * half)
            crop = (slice(y0, y0 + 2 * half), slice(x0, x0 + 2 * half))
            parts = [a[crop], b[crop], np.clip(np.abs(d[crop]) * 8, 0, 255)]
            row = np.concatenate([np.clip(p, 0, 255).astype(np.uint8) for p in parts], axis=1)
            rows.append(np.asarray(Image.fromarray(row).resize((row.shape[1] * 2, row.shape[0] * 2), Image.NEAREST)))
    if args.sheet and rows:
        Image.fromarray(np.concatenate(rows, axis=0)).save(args.sheet)
        print(f"sheet: {args.sheet}")


if __name__ == "__main__":
    main()
