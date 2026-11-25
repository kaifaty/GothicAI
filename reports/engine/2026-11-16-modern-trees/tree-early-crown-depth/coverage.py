"""Estimate tier coverage from the final winning pixels of selected spruce crowns."""

import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tree-cull-granularity"))

from box_test import Depth
from cut_replay import load_cut, replay_cut
from paired_capture import load_capture, load_membership
from projection import load_camera, load_objects
from recount import add_tests, rows, verify_inputs
from wind_bounds import load_wind_bounds


def crown_pixels(raw, owners, trees):
    """Decode 3D needle IDs, checking the join against the archived per-crown census."""
    position = {tuple(round(float(tree[k + "_cm"]) * 10) for k in "xyz"): int(tree["id"])
                for tree in trees}
    lookup = {}
    for owner in owners:
        if owner["crown"] != "1":
            continue
        tree_id = position[tuple(round(float(owner[k]) * 10) for k in "xyz")]
        for prefix in ("", "band_"):
            cluster = int(owner[prefix + "cluster"]) + 1
            instance = int(owner[prefix + "instance"]) + 1
            lookup[cluster] = tree_id
            lookup[(1 << 29) | instance] = tree_id
            lookup[(2 << 29) | instance] = tree_id
    unique, inverse = np.unique(raw, return_inverse=True)
    pixels = np.array([lookup.get(int(value), 0) for value in unique])[inverse].reshape(raw.shape)
    counts = np.bincount(pixels.ravel(), minlength=len(trees) + 1)
    if any(counts[int(tree["id"])] != int(tree["needle_3d_pixels"]) for tree in trees):
        raise ValueError("Decoded 3D needle pixels differ from the archived census")
    return pixels


def load_asset(census, fixture):
    folder = fixture / "meshes/trees_fir"
    base = folder / "OWN_TREE_FIR_A_4.glb"
    levels = sorted(folder.glob("OWN_TREE_FIR_A_4_L[1-4].glb"))
    asset = folder / "bough-s122b600g0/OWN_TREE_FIR_A_BOUGH_S122B600G0_H.json"
    verify_inputs(census, fixture, asset, base, levels)
    cut = load_cut(asset, [base, *levels])
    pad, _, _ = load_wind_bounds(asset, base, fixture / "materials/materials.json")
    return cut, pad


def replay_tiers(cut, pad, objects, membership, camera, height):
    view, projection, near, phases = camera
    focal = 0.5 * height * abs(projection[1, 1])
    if len(cut.roots) != 36:
        raise ValueError("Expected twelve tiers of three sectors")
    tier_starts = cut.roots[::3]
    lo = np.minimum.reduceat(cut.lo, tier_starts)
    hi = np.maximum.reduceat(cut.hi, tier_starts)
    tier_by_node = np.searchsorted(tier_starts, np.arange(len(cut.end)), side="right") - 1
    candidates = {}
    total_visits = 0
    for tree_id, (compact, triangle_count, node_count) in membership.items():
        obj = objects[tree_id]
        replay = replay_cut(cut, obj, view, focal, near)
        total_visits += replay.probed + replay.walked
        if replay.compact != compact or replay.triangles != triangle_count:
            raise ValueError(f"Archive cut does not match tree {tree_id}")
        if replay.compact:
            if replay.selected.size != node_count:
                raise ValueError(f"Archive node count does not match tree {tree_id}")
            band = tier_by_node[replay.selected]
            triangles = np.zeros(len(tier_starts), dtype=np.int64)
            np.add.at(triangles, band, cut.triangles[replay.selected])
            nodes = np.bincount(band, minlength=len(tier_starts))
            visits = np.add.reduceat(replay.walk_visits, tier_starts)
            candidates[tree_id] = (obj, phases, near, pad, lo, hi, triangles, nodes, visits)
    return candidates, total_visits


def score(plane, candidates, pinned=()):
    depth = Depth(plane)
    totals = dict.fromkeys(("boxes", "projected", "outside_boxes", "texels", "footprint",
                            "hidden_boxes", "triangles", "nodes", "visits", "outside_triangles"), 0)
    for tree_id, tier in candidates.items():
        if tree_id not in pinned:
            add_tests(totals, depth, *tier)
    return {field: totals[field] for field in ("hidden_boxes", "triangles", "visits", "texels")}


def census_sweep(census, vobs, cut, pad):
    snapshot = census / "capture/snapshot"
    _, _, _, tick, width, height, triangles = map(int, (snapshot / "frame.txt").read_text().split())
    trees = rows(census / "analysis/trees.csv")
    raw = np.fromfile(snapshot / "pixels-depth.u32", dtype="<u4").reshape(2, height, width)
    needle = crown_pixels(raw[0], rows(snapshot / "owners.csv"), trees)
    final = raw[1].view("<f4")
    membership = {int(tree["id"]): (tree["cut"] == "compact", int(tree["bough_triangles"]), int(tree["bough_units"]))
                  for tree in trees if tree["cut"] != "none"}
    objects = load_objects(census / "harness/placements.csv", vobs)
    candidates, total_visits = replay_tiers(cut, pad, objects, membership, load_camera(census), height)
    if sum(int(tree["bough_triangles"]) for tree in trees) != triangles:
        raise ValueError("Archive has inconsistent triangle totals")

    winner_counts = np.bincount(needle.ravel(), minlength=len(trees) + 1)
    sampled_counts = np.bincount(needle[2::4, 2::4].ravel(), minlength=len(trees) + 1)
    winners = [tree for tree in trees if tree["cut"] != "none" and sampled_counts[int(tree["id"])] >= 16]
    winners.sort(key=lambda tree: (-sampled_counts[int(tree["id"])], int(tree["id"])))
    cases = {}
    for name, group in [("top4", winners[:4]), ("sampled_winners", winners),
                        ("all_winners", [tree for tree in trees if winner_counts[int(tree["id"])] > 0])]:
        ids = {int(tree["id"]) for tree in group}
        selected = np.isin(needle, sorted(ids))
        cases[name] = {
            "seed_crowns": len(ids),
            "seed_triangles": sum(int(tree["bough_triangles"]) for tree in group),
            "seed_pixels": int(selected.sum()),
            "needle_only": score(np.where(selected, final, 1), candidates, ids),
            "with_other_final": score(np.where(selected | (needle == 0), final, 1), candidates, ids),
            "full_final_pinned": score(final, candidates, ids),
        }
    return {"tick": tick, "total_triangles": triangles, "total_visits": total_visits,
            "full_final": score(final, candidates), "cases": cases}


def pair_sweep(pair, cohort, vobs, cut, pad):
    placements = pair / "harness/placements.csv"
    objects = load_objects(placements, vobs)
    key_ids = {row["key"]: i for i, row in enumerate(rows(placements), 1)}
    comparison = pair / "comparison"
    scenes = {}
    for scene in ("inside", "walk"):
        counters, final, early = load_capture(comparison, scene)
        width, height = counters["width"], counters["height"]
        membership = {key_ids[key]: (row["compact"], row["a_tris"], row["a_nodes"])
                      for key, row in load_membership(cohort, scene).items()}
        if sum(record[1] for record in membership.values()) != counters["a_triangles"]:
            raise ValueError(f"Archive has inconsistent triangle totals: {scene}")
        candidates, total_visits = replay_tiers(cut, pad, objects, membership, load_camera(comparison, scene), height)
        raw = np.memmap(comparison / f"{scene}-A.u32", dtype="<u4", mode="r", shape=(height, width))
        tag = raw >> 29
        needle = (tag == 1) | (tag == 2)
        owners = defaultdict(set)
        for row in rows(comparison / f"{scene}-owners.csv"):
            if row["crown"] == "1":
                for key in ("instance", "band_instance"):
                    instance = int(row[key])
                    if instance != 0xffffffff:
                        owners[int(row["object"])].update(((1 << 29) | (instance + 1), (2 << 29) | (instance + 1)))
        if not set(np.unique(raw[needle])).issubset(set().union(*owners.values())):
            raise ValueError(f"Unmapped needle owner: {scene}")
        samples = dict(zip(*np.unique(raw[2::4, 2::4], return_counts=True), strict=True))
        sampled_ids = [value for ids in owners.values() if sum(samples.get(value, 0) for value in ids) >= 16
                       for value in ids]
        cases = {"early": score(early, candidates), "final": score(final, candidates)}
        for name, mask in (("sampled_winners", np.isin(raw, sampled_ids)), ("all_winners", needle),
                           ("full_only", tag == 1), ("compact_only", tag == 2)):
            cases[name] = score(np.minimum(early, np.where(mask, final, 1)), candidates)
        scenes[scene] = {"tick": counters["tick"], "total_visits": total_visits, "cases": cases}
    return scenes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path, help="Directory containing the capture archives and the granularity replay")
    args = parser.parse_args()
    archive = args.archive
    census = archive / "fir-view1-pixels-20261122T184646Z"
    granularity = archive / "tree-8ms-cull-granularity"
    fixture = granularity / "fixture"
    vobs = fixture / "worlds/NEWWORLD/vobs.json"
    cut, pad = load_asset(census, fixture)
    result = {
        "census": census_sweep(census, vobs, cut, pad),
        "pair": pair_sweep(archive / "fir-bough-depth-20261122T201811Z/abba",
                           granularity / "pair/analysis/final-depth", vobs, cut, pad),
    }
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
