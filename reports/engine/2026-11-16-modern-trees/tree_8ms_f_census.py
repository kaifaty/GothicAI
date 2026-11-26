#!/usr/bin/env python3
"""Reduce archived spruce submissions and test the F triangle budget."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import sys

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path[:0] = [str(HERE / "tree-cull-granularity"), str(HERE.parents[2] / "scripts")]
from cut_replay import glb_primitives  # noqa: E402
from paired_capture import load_membership  # noqa: E402
from perf_report import percentile, read_profile  # noqa: E402
from projection import load_camera, load_objects  # noqa: E402

CARD_MS_PER_MILLION = 0.45
NEW_MS_PER_MILLION = 0.20
OVERHEAD_MS = 0.30
MIN_REMOVED_TRIANGLES = 2_222_223


def rows(path):
    with path.open(newline="") as stream:
        return list(csv.DictReader(stream))


def profile(path, segments):
    attempts, _, _, _ = read_profile(path)
    if any(row["overflow"] for row in attempts):
        raise ValueError(f"{path}: truncated profile")
    for segment in segments:
        submitted = [row for row in attempts if row["segment"] == segment and row["outcome"] == 1]
        if not submitted:
            raise ValueError(f"{path}: missing {segment}")
        start = submitted[0]["begin_ns"] + 250_000_000
        totals = [row["tree_bough_triangles"] for row in submitted[:-1]
                  if row["begin_ns"] >= start and row.get("gpu_done", 1) and row["gpu_ns"] > 0]
        if not totals:
            raise ValueError(f"{path}: no complete GPU frames for {segment} after trim")
        p50 = percentile(totals, 0.5)
        upper = sum(total >= MIN_REMOVED_TRIANGLES for total in totals)
        print(f"{path.parent.name}/{segment}: {len(totals)} rows, all bough triangles "
              f"P50 {p50:,.0f}, range {min(totals):,}–{max(totals):,}, "
              f"{upper} rows with total >= {MIN_REMOVED_TRIANGLES:,}. Near count is unavailable.")


def snapshot(label, submitted, expected):
    if sum(triangles for _, _, triangles in submitted) != expected:
        raise ValueError(f"{label}: submission total differs from the archive")
    print(f"{label}: {len(submitted)} crowns, {expected:,} triangles")
    selections = {"full bough mode": [tri for full, _, tri in submitted if full]}
    for cutoff in (400, 300, 200):
        selections[f"projected >={cutoff} px"] = [tri for _, height, tri in submitted if height >= cutoff]
    for zone, triangles in selections.items():
        count, removed = len(triangles), sum(triangles)
        zero_geometry = CARD_MS_PER_MILLION * removed / 1_000_000 - OVERHEAD_MS
        nominal = zero_geometry - NEW_MS_PER_MILLION
        per_tree = str(1_000_000 // count) if count else "—"
        print(f"  {zone}: {count} crowns, {removed:,} triangles, {per_tree} new/tree, "
              f"{nominal:+.3f} ms at 1M new, {zero_geometry:+.3f} ms at zero new")


def owner_snapshot(root, census):
    fixture = root / "tree-8ms-cull-granularity/fixture"
    mesh = fixture / "meshes/trees_fir/OWN_TREE_FIR_A_4.glb"
    receipt = json.loads((census / "analysis/result.json").read_text())
    mesh_sha = "4e2ff42209be7ebc3359d2d841bb717a42fa132337112b0b788e47cf6b9e8eac"
    if hashlib.sha256(mesh.read_bytes()).hexdigest() != mesh_sha:
        raise ValueError(f"{mesh}: unexpected base mesh")
    objects = load_objects(census / "harness/placements.csv", fixture / "worlds/NEWWORLD/vobs.json")
    view, projection, _, _ = load_camera(census)
    camera = np.linalg.solve(view[:3, :3], -view[:3, 3])
    primitives = [positions for positions, _ in glb_primitives(mesh)]
    low = np.min([p["min"] for p in primitives], axis=0)
    high = np.max([p["max"] for p in primitives], axis=0)
    center = (low + high) * np.array([-50, 50, 50])
    focal = 0.5 * receipt["dimensions"][1] * abs(projection[1, 1])
    trees = rows(census / "analysis/trees.csv")
    if len(trees) != len(objects) or {int(row["id"]) for row in trees} != objects.keys():
        raise ValueError(f"{census}: owner census differs from verified placements")
    triangles = []
    for row in trees:
        if row["cut"] == "none":
            continue
        obj = objects[int(row["id"])]
        world_center = obj[:3, :3] @ center + obj[:3, 3]
        distance = max(1.0, np.linalg.norm(world_center - camera))
        height = (high[1] - low[1]) * 100 * np.linalg.norm(obj[:3, 1]) * focal / distance
        triangles.append((row["cut"] == "full", height, int(row["bough_triangles"])))
    snapshot("Owner census", triangles, receipt["ordinary_same_slot_triangles"])


def reconstructed_snapshots(root):
    cohorts = root / "tree-8ms-cull-granularity/pair/analysis/final-depth"
    comparison = root / "fir-bough-depth-20261122T201811Z/abba/comparison"
    for scene in ("inside", "walk"):
        membership = load_membership(cohorts, scene)
        frame, = rows(comparison / f"{scene}.csv")
        if len(membership) != int(frame["a_crowns"]):
            raise ValueError(f"{scene}: cohort size differs from archived GPU counts")
        heights = {row["key"]: float(row["height_px"]) for row in rows(cohorts / f"{scene}-candidates.csv")}
        submitted = [(not state["compact"], heights[key], state["a_tris"]) for key, state in membership.items()]
        snapshot(f"Reconstructed {scene}", submitted, int(frame["a_triangles"]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path, help="Archive root, or a local copy of the needed files")
    parser.add_argument("census", type=Path, help="fir-view1-pixels-20261122T184646Z")
    args = parser.parse_args()
    batch = args.root / "tree-8ms-batch1/result-20261125T135755Z"
    for arm in ("02-all", "03-all"):
        profile(batch / arm / "frameprofile.csv", ("inside", "hero", "under-low"))
    stage3 = args.root / "tree-nanite-step3/result-20261126T043449Z/01-A/frameprofile.csv"
    profile(stage3, [f"perf-tree-nanite/view-{scene}"
                     for scene in ("1-inside", "2-hero", "3-sun", "4-near", "5-far")])
    profile(args.root / "forest-frame-budget-20261124/evidence-final/frameprofile.csv", ("entry-repeat-1",))
    owner_snapshot(args.root, args.census)
    reconstructed_snapshots(args.root)


if __name__ == "__main__":
    main()
