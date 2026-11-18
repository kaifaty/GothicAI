#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy", "pillow", "scipy"]
# ///
"""Compare exact terrain-coverage PNGs from matched cluster LOD bench recordings."""

import argparse
import csv
import json
from pathlib import Path

import numpy as np
from PIL import Image
from scipy.ndimage import binary_erosion, binary_fill_holes


def read_recording(directory):
    events = [json.loads(line) for line in (directory.parent / "events.jsonl").read_text().splitlines()]
    segment = [event for event in events if event["seg"] == directory.name]
    poses = [event for event in segment if event["kind"] == "cluster_lod_mask"]
    ends = [event["frames"] for event in segment if event["kind"] == "rec_stop"]
    if not poses or ends != [len(poses)] or [event["frame"] for event in poses] != list(range(len(poses))):
        raise ValueError(f"{directory}: missing, duplicate or incomplete mask frames")
    if len(list(directory.glob("*.png"))) != len(poses):
        raise ValueError(f"{directory}: PNG count disagrees with events")
    return poses


def read_coverage(path):
    with Image.open(path) as image:
        rgb = np.asarray(image.convert("RGB"))
    gray = rgb[:, :, 1]
    if not (np.all(rgb[:, :, ::2] == 255) and np.all((gray == 0) | (gray == 255))):
        raise ValueError(f"{path}: the coverage frame must contain only white and magenta pixels")
    return gray == 255


def compare(reference, candidate, edge, output):
    before = read_recording(reference)
    after = read_recording(candidate)
    if len(before) != len(after):
        raise ValueError(f"recordings have different lengths: {len(before)} and {len(after)}")
    output.parent.mkdir(parents=True, exist_ok=True)
    regressed = 0
    worst = 0
    max_base = max_cut = 0
    with output.open("w", newline="") as stream:
        fields = ["frame", "interior_px", "base_crack_px", "cut_crack_px", "delta_px", "new_crack_x", "new_crack_y"]
        writer = csv.writer(stream)
        writer.writerow(fields)
        for index, (a, b) in enumerate(zip(before, after)):
            if a["view_projection"] != b["view_projection"]:
                raise ValueError(f"frame {index}: camera projections differ ({a['camera_cm']} / {b['camera_cm']})")
            if a.get("residency") != b.get("residency"):
                raise ValueError(f"frame {index}: Near/Far landscape residency differs")
            if a.get("pending", 0) or b.get("pending", 0):
                raise ValueError(f"frame {index}: landscape cells still loading ({a['pending']} / {b['pending']})")
            base = read_coverage(reference / f"{index}.png")
            cut = read_coverage(candidate / f"{index}.png")
            if base.shape != cut.shape:
                raise ValueError(f"frame {index}: mask dimensions differ ({base.shape} / {cut.shape})")
            # Fill enclosed gaps while keeping the reference's exterior contour, then discard its edge band.
            # The reference interior is shared so disappearing regions still count if a seam opens to the sky.
            silhouette = binary_fill_holes(base)
            if edge:
                silhouette = binary_erosion(silhouette, iterations=edge, border_value=1)
            interior = int(silhouette.sum())
            if not interior:
                raise ValueError(f"frame {index}: reference has an empty terrain silhouette")
            base_gaps = silhouette & ~base
            cut_gaps = silhouette & ~cut
            base_count = int(base_gaps.sum())
            cut_count = int(cut_gaps.sum())
            delta = cut_count - base_count
            max_base = max(max_base, base_count)
            max_cut = max(max_cut, cut_count)
            x = y = ""
            if delta > 0:
                regressed += 1
                new_gaps = cut_gaps & base
                y, x = np.unravel_index(np.argmax(new_gaps), new_gaps.shape)
                if delta > worst:
                    worst = delta
                    worst_frame = index
                    proof = np.where(new_gaps[:, :, None], (255, 0, 255), np.where(cut[:, :, None], 255, 0))
                    Image.fromarray(proof.astype(np.uint8)).save(output.with_suffix(".png"))
            writer.writerow([index, interior, base_count, cut_count, delta, x, y])
    print(f"{len(before)} frames, edge band {edge} px, max gaps {max_base} / {max_cut} px, {regressed} frames regressed")
    if regressed:
        print(f"Largest increase: +{worst} px in frame {worst_frame}, marked in {output.with_suffix('.png')}")
    return int(regressed > 0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path, help="reference mask recording folder")
    parser.add_argument("candidate", type=Path, help="candidate mask recording folder")
    parser.add_argument("output", type=Path, help="per-frame CSV output path")
    parser.add_argument("--edge-px", type=int, default=1, help="ignore this band along the L0 outer contour")
    args = parser.parse_args()
    if args.edge_px < 0:
        parser.error("--edge-px must be nonnegative")
    try:
        return compare(args.reference, args.candidate, args.edge_px, args.output)
    except (ValueError, OSError, KeyError) as error:
        parser.exit(2, f"{error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
