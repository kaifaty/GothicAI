#!/usr/bin/env python3
"""Scale the archived B1 GPU passes by one offline snapshot's geometry and test work."""

import argparse
import csv
import json
from collections import defaultdict
from pathlib import Path
from statistics import median

MS_PER_TRIANGLE = 0.45e-6


def pyramid_times(prototype, scene):
    """Split the two identically labeled encoders using the archived reducer's frame window."""
    profile = prototype / "B1" / "frameprofile.csv"
    with profile.open(newline="") as file:
        frames = [
            (row["attempt"], int(row["begin_ns"]))
            for row in csv.DictReader(file)
            if row["segment"] == scene and row["outcome"] == "1"
        ]
    threshold = frames[0][1] + 250_000_000
    attempts = {attempt for attempt, timestamp in frames[:-1] if timestamp >= threshold}
    encoders = defaultdict(list)
    with profile.with_suffix(".csv.gpu.csv").open(newline="") as file:
        for row in csv.DictReader(file):
            attempt = row["attempt"]
            if attempt in attempts and row["valid"] == "1" and row["labels"] == "Bough-HiZ-pyramid":
                encoders[attempt].append((int(row["encoder"]), int(row["gpu_ns"])))
    if set(encoders) != attempts or any(len(pair) != 2 for pair in encoders.values()):
        raise ValueError("Every retained B1 frame must have two pyramid encoders")
    fresh_ns, final_ns = zip(*(tuple(ns for _, ns in sorted(pair)) for pair in encoders.values()))
    return {
        "frames": len(attempts),
        "fresh_ms": median(fresh_ns) / 1e6,
        "final_ms": median(final_ns) / 1e6,
        "both_ms": median(a + b for a, b in zip(fresh_ns, final_ns, strict=True)) / 1e6,
    }


def estimate(snapshot, scene, prototype, diagnostic):
    profile = json.loads((prototype / "partial-a1-b1.json").read_text())["scenes"][scene]
    diag = json.loads((diagnostic / "summary.json").read_text())["scenes"][scene]
    a1, b1 = profile["A1"], profile["B1"]
    walk_ms = a1["pass_ms"]["Bough-cut"]["p50"]
    test_ms = b1["pass_ms"]["Bough-cut"]["p50"] - walk_ms
    recheck_ms = b1["pass_ms"]["Bough-HiZ-recheck"]["p50"]
    pending_boxes = b1["counts"]["tree_bough_hiz_candidates"]["mean"]
    test_boxes = diag["per_frame"]["compact_nodes"]["mean"]
    pyramid = pyramid_times(prototype, scene)

    node = snapshot["boxes"]["node"]
    expected_texels = test_boxes * node["texels"] / node["boxes"]
    variants = {}
    for name, result in snapshot["boxes"].items():
        by_projection = test_ms * result["boxes"] / test_boxes
        by_texel = test_ms * result["texels"] / expected_texels
        recheck = recheck_ms * result["hidden_boxes"] / pending_boxes
        saved_walk = walk_ms * result["visits"] / snapshot["total_visits"]
        saved_draw = MS_PER_TRIANGLE * result["triangles"]
        no_fresh_depth = pyramid["final_ms"] - saved_walk - saved_draw
        fresh_depth = pyramid["both_ms"] + recheck - saved_walk - saved_draw
        variants[name] = {
            "test_projection_ms": by_projection,
            "test_texel_ms": by_texel,
            "recheck_ms": recheck,
            "saved_walk_ms": saved_walk,
            "saved_gbuffer_ms": saved_draw,
            "previous_only_delta_ms": [no_fresh_depth + value for value in sorted((by_projection, by_texel))],
            "two_phase_delta_ms": [fresh_depth + value for value in sorted((by_projection, by_texel))],
        }
    return {
        "scene": scene,
        "calibration": {
            "bough_cut_a1_ms": walk_ms,
            "bough_cut_b1_increment_ms": test_ms,
            "compact_boxes_per_frame": test_boxes,
            "modeled_b1_texels_per_frame": expected_texels,
            "recheck_ms": recheck_ms,
            "pending_boxes_per_frame": pending_boxes,
            "pyramid": pyramid,
        },
        "variants": variants,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scene", choices=("inside", "hero"), required=True)
    parser.add_argument("snapshot", type=Path, help="Spatial replay output JSON for one snapshot")
    parser.add_argument("prototype", type=Path, help="B1 prototype output-abba directory")
    parser.add_argument("diagnostic", type=Path, help="B1 diagnostic output-clean directory")
    args = parser.parse_args()
    result = estimate(json.loads(args.snapshot.read_text()), args.scene, args.prototype, args.diagnostic)
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
