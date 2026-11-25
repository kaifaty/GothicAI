"""Compare conservative tree, tier, group and node tests on three archived frames."""

import argparse
import csv
import hashlib
import json
from pathlib import Path

import numpy as np

from box_test import Depth, project
from cut_replay import load_cut, replay_cut
from paired_capture import load_capture, load_membership
from projection import load_camera, load_objects
from wind_bounds import load_wind_bounds


def rows(path):
    with path.open() as stream:
        return list(csv.DictReader(stream))


def verify_inputs(census, fixture, asset, base, levels):
    receipt = census / "capture/inputs.sha256"
    hashes = {Path(path).name: digest for digest, path in
              (line.split(maxsplit=1) for line in receipt.read_text().splitlines())}
    hashes.update({
        asset.name: "3194176a52a09b49831df055a337ab7687f027212a4c330cd27835531581b08d",
        asset.with_suffix(".glb").name: "df2c6744c00f15f98ce92a1e5bf5ba5edd498254c7f064dc8d974ac8b8913833",
    })
    for path in (fixture / "worlds/NEWWORLD/vobs.json", asset, asset.with_suffix(".glb"), base, *levels):
        with path.open("rb") as stream:
            if hashlib.file_digest(stream, "sha256").hexdigest() != hashes[path.name]:
                raise ValueError(f"Archive fixture SHA-256 mismatch: {path}")


def mesh_groups(cut, selected):
    """A reproducible per-tree cohort, using preorder within each page and group size."""
    columns = ([], [], [], [])
    for page in np.unique(cut.primitive[selected]):
        for size in (2, 4, 8):
            nodes = selected[(cut.primitive[selected] == page) & (cut.mesh_class[selected] == size)]
            if not nodes.size:
                continue
            starts = np.arange(0, nodes.size, size)
            for column, values in zip(columns, (
                np.minimum.reduceat(cut.lo[nodes], starts),
                np.maximum.reduceat(cut.hi[nodes], starts),
                np.add.reduceat(cut.triangles[nodes], starts),
                np.minimum(size, nodes.size - starts),
            ), strict=True):
                column.append(values)
    return tuple(np.concatenate(column) for column in columns)


def add_tests(totals, depth, matrix, phases, near, pad, lo, hi, triangles, selected, visits):
    valid, outside, lower, upper, nearest = project(lo - pad, hi + pad, matrix, phases, near)
    hidden, texels, footprint = depth.test(lower, upper, nearest)
    rejected = np.flatnonzero(valid)[hidden]
    for field, value in (
        ("boxes", len(valid)), ("projected", valid.sum()), ("outside_boxes", outside.sum()),
        ("texels", texels.sum()), ("footprint", footprint.sum()), ("hidden_boxes", hidden.sum()),
        ("triangles", triangles[rejected].sum()), ("nodes", selected[rejected].sum()),
        ("visits", visits[rejected].sum()), ("outside_triangles", triangles[outside].sum()),
    ):
        totals[field] += int(value)
    return bool(hidden.any())


def recount(depth_plane, camera, objects, membership, cut, wind, expected, needle_pixels=None):
    view, projection, near, phases = camera
    pad, crown_lo, crown_hi = wind
    height, width = depth_plane.shape
    depth = Depth(depth_plane)
    focal = 0.5 * height * abs(projection[1, 1])
    counts = dict.fromkeys((
        "boxes", "projected", "outside_boxes", "texels", "footprint", "hidden_boxes",
        "triangles", "nodes", "visits", "outside_triangles",
    ), 0)
    results = {variant: counts.copy() for variant in ("tree", "full_tier", "tier", "group", "node")}
    total_triangles = total_nodes = total_visits = total_probe = mode_mismatches = 0
    tree_pixel_conflicts = mismatched_trees = checked_crowns = 0
    tier_lo = np.minimum.reduceat(cut.lo, cut.roots)
    tier_hi = np.maximum.reduceat(cut.hi, cut.roots)
    tier_by_node = np.searchsorted(cut.roots, np.arange(len(cut.end)), side="right") - 1
    # The verified source asset has twelve height bands, each with all three
    # azimuth sectors, in the exporter's sorted (height, azimuth) order.
    if len(cut.roots) != 36:
        raise ValueError("Expected twelve tiers of three sectors in this fixture")
    band_starts = np.arange(0, len(cut.roots), 3)
    band_lo = np.minimum.reduceat(tier_lo, band_starts)
    band_hi = np.maximum.reduceat(tier_hi, band_starts)

    for tree_id, (is_compact, triangles, selected_units) in membership.items():
        obj = objects[tree_id]
        replay = replay_cut(cut, obj, view, focal, near)
        selected = replay.selected
        mode_mismatches += replay.compact != is_compact
        mismatched_trees += replay.triangles != triangles or (is_compact and selected_units != selected.size)
        total_triangles += replay.triangles
        total_visits += replay.probed + replay.walked
        total_probe += replay.probed
        compact_units = selected.size if is_compact else 0
        total_nodes += compact_units

        def test(variant, lo, hi, triangles, nodes, visits):
            return add_tests(results[variant], depth, obj, phases, near, pad, lo, hi, triangles, nodes, visits)

        tree_hidden = test("tree", crown_lo[None], crown_hi[None], np.array([replay.triangles]),
                           np.array([compact_units]), np.array([replay.probed + replay.walked]))
        if tree_hidden and needle_pixels is not None:
            tree_pixel_conflicts += int(needle_pixels[tree_id])
            checked_crowns += 1
        if not is_compact:
            continue

        tiers = tier_by_node[selected]
        tier_triangles = np.zeros(cut.roots.size, dtype=np.int64)
        np.add.at(tier_triangles, tiers, cut.triangles[selected])
        tier_nodes = np.bincount(tiers, minlength=cut.roots.size)
        tier_visits = np.add.reduceat(replay.walk_visits, cut.roots)
        test("full_tier", band_lo, band_hi, np.add.reduceat(tier_triangles, band_starts),
             np.add.reduceat(tier_nodes, band_starts), np.add.reduceat(tier_visits, band_starts))
        test("tier", tier_lo, tier_hi, tier_triangles, tier_nodes, tier_visits)

        lo, hi, group_triangles, group_nodes = mesh_groups(cut, selected)
        test("group", lo, hi, group_triangles, group_nodes, np.zeros(group_nodes.size, dtype=int))
        test("node", cut.lo[selected], cut.hi[selected], cut.triangles[selected],
             np.ones(selected.size, dtype=int), np.zeros(selected.size, dtype=int))

    if (mode_mismatches or mismatched_trees or total_triangles != expected or tree_pixel_conflicts):
        raise ValueError(f"Archive parity failed: {mode_mismatches} modes, {mismatched_trees} cuts, "
                         f"{total_triangles}/{expected} triangles, {tree_pixel_conflicts} winning pixels on rejected crowns")
    return dict(width=width, height=height, tested_crowns=len(membership), total_triangles=total_triangles,
                total_compact_nodes=total_nodes, total_visits=total_visits, probe_visits=total_probe,
                jitter_phases=len(phases), rejected_crowns_checked_for_owners=checked_crowns,
                winning_pixels_on_rejected_crowns=tree_pixel_conflicts if needle_pixels is not None else None,
                cull_padding_cm=float(pad), boxes=results)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("census", type=Path, help="fir-view1-pixels archive")
    parser.add_argument("pair", type=Path, help="fir-bough-depth archive")
    parser.add_argument("membership", type=Path, help="Archived analysis/final-depth cohort CSVs")
    parser.add_argument("fixture", type=Path, help="Verified content root containing meshes/, materials/ and worlds/")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    vobs = args.fixture / "worlds/NEWWORLD/vobs.json"
    mesh_dir = args.fixture / "meshes/trees_fir"
    base = mesh_dir / "OWN_TREE_FIR_A_4.glb"
    levels = sorted(mesh_dir.glob("OWN_TREE_FIR_A_4_L[1-4].glb"))
    if len(levels) != 4:
        raise ValueError("Expected four authored levels of the archive fixture")
    asset = mesh_dir / "bough-s122b600g0/OWN_TREE_FIR_A_BOUGH_S122B600G0_H.json"
    verify_inputs(args.census, args.fixture, asset, base, levels)
    cut = load_cut(asset, [base, *levels])
    wind = load_wind_bounds(asset, base, args.fixture / "materials/materials.json")
    args.output.mkdir(parents=True, exist_ok=True)

    def write(scene, depth, camera, objects, membership, expected, needle_pixels=None):
        report = recount(depth, camera, objects, membership, cut, wind, expected, needle_pixels)
        path = args.output / f"{scene}.json"
        path.write_text(json.dumps(report, indent=2) + "\n")
        print(path, flush=True)

    archive = args.census
    tree_rows = rows(archive / "analysis/trees.csv")
    snapshot = archive / "capture/snapshot"
    _, _, _, _, width, height, stock = map(int, (snapshot / "frame.txt").read_text().split())
    raw = np.fromfile(snapshot / "pixels-depth.u32", dtype="<u4").reshape(2, height, width)
    depth = raw[1].view("<f4")
    membership = {int(row["id"]): (row["cut"] == "compact", int(row["bough_triangles"]), int(row["bough_units"]))
                  for row in tree_rows if row["cut"] != "none"}
    pixels = {int(row["id"]): int(row["needle_3d_pixels"]) for row in tree_rows}
    objects = load_objects(archive / "harness/placements.csv", vobs)
    write("census", depth, load_camera(archive), objects, membership, stock, pixels)

    comparison = args.pair / "abba/comparison"
    placements = args.pair / "abba/harness/placements.csv"
    objects = load_objects(placements, vobs)
    key_ids = {row["key"]: i for i, row in enumerate(rows(placements), 1)}
    for scene in ("inside", "walk"):
        counters, depth, early = load_capture(comparison, scene)
        membership = {key_ids[key]: (state["compact"], state["a_tris"], state["a_nodes"])
                      for key, state in load_membership(args.membership, scene).items()}
        if sum(record[2] for record in membership.values()) != counters["a_nodes"]:
            raise ValueError(f"Selected unit totals differ from archived GPU counts: {scene}")
        if len(membership) != counters["a_crowns"]:
            raise ValueError(f"Crown totals differ from archived GPU counts: {scene}")
        camera = load_camera(comparison, scene)
        write(scene, depth, camera, objects, membership, counters["a_triangles"])
        write(f"{scene}-early", early, camera, objects, membership, counters["a_triangles"])


if __name__ == "__main__":
    main()
