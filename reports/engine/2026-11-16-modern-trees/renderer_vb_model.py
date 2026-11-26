#!/usr/bin/env python3
"""Fit visibility-buffer sensitivity to complete frames in the four forest archives."""

import argparse
from itertools import groupby, pairwise
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from perf_report import percentile, read_gpu_passes, read_profile  # noqa: E402


BATCH = "tree-8ms-batch1/result-20261125T135755Z"
CROWN = "tree-nanite-step3/result-20261126T043449Z"
WARM = "forest-frame-budget-20261124/evidence-final"
CENSUS = "fir-view1-pixels-20261122T184646Z/analysis/result.json"

BATCH_VIEWS = ("inside", "hero", "under-low")
CROWN_VIEWS = tuple(f"perf-tree-nanite/view-{i}-{name}" for i, name in enumerate(
    ("inside", "hero", "sun", "near", "far"), start=1
))
GI = ("GI-Alloc", "GI-Trace", "GI-HashMap", "GI-Lighting")
F_SNAPSHOT = {"inside": (111, 2_926_112), "hero": (84, 2_462_173)}
F_MAX_TRIANGLES = 9_009
MAX_MESHLET_TRIANGLES = 64
OLD_FOLIAGE_MS_PER_MILLION = 0.45
NEW_FOLIAGE_MS_PER_MILLION = 0.20
VB_MS_PER_MILLION = 0.12016


def median(values):
    return percentile(list(values), 0.5)


def frames(path, segments):
    rows, _, _, _ = read_profile(path)
    passes = read_gpu_passes(path)
    if not passes or any(row["overflow"] for row in rows):
        raise ValueError(f"{path}: missing GPU sidecar or profile overflow")
    result = {segment: [] for segment in segments}
    for segment, window in groupby(rows, key=lambda row: row["segment"]):
        if segment not in result:
            continue
        window = list(window)
        submitted = [i for i, row in enumerate(window) if row["outcome"] == 1]
        if not submitted:
            continue
        start = window[submitted[0]]["begin_ns"] + 250_000_000
        for left, right in pairwise(submitted):
            row = window[left]
            if row["begin_ns"] < start or not row["gpu_done"] or row["gpu_ns"] <= 0:
                continue
            labels = {name: ns / 1e6 for name, ns in passes[row["attempt"]].items()}
            for name in ("GBuffer", "VSM-rendering", *GI):
                if name not in labels:
                    raise ValueError(f"{path}: missing {name} on attempt {row['attempt']}")
            attempts = window[left:right]
            active = sum(a["end_ns"] - a["begin_ns"] - a["fence_poll_ns"] for a in attempts) / 1e6
            full = (window[right]["begin_ns"] - row["begin_ns"]) / 1e6
            gpu = row["gpu_ns"] / 1e6
            cpu_after_e = active - (
                0.55 * sum(a["npc_simplified_tick_ns"] for a in attempts) / 1e6
                + min(0.80, sum(a["encode_main_ns"] for a in attempts) / 1e6) - 0.15
            )
            result[segment].append({
                "frame": full, "gpu": gpu, "cpu": active, "cpu_e": cpu_after_e,
                "publication": sum(a[key] for a in attempts for key in
                                   ("rt_scene_ns", "bindless_rebuild_ns", "bindless_patch_ns")) / 1e6,
                "residual": full - max(gpu, active), "dt": row["dt"],
                "gbuffer": labels["GBuffer"], "bough": labels.get("Bough-cut", 0),
                "shadow": labels["VSM-rendering"], "gi": sum(labels[name] for name in GI),
                "cards": row["tree_bough_triangles"],
                "non_canopy_meshlets": max(0, row["main_meshlets"] - row["main_canopy_meshlets"]),
                "rejected_3b": row["hiz_history_rejected_meshlets"],
                "early_vb": labels.get("TreeNanite-VB-early", 0),
                "late_vb": labels.get("TreeNanite-VB-late", 0),
                "tree_vsm": labels.get("TreeNanite-VSM", 0),
                "tree_vb_triangles": row.get("tree_nanite_triangles_early", 0)
                + row.get("tree_nanite_triangles_late", 0),
            })
    if any(not group for group in result.values()):
        raise ValueError(f"{path}: missing usable frames for {segments}")
    return result


def world_gain(f, overdraw, rejected_fraction):
    old_foliage = OLD_FOLIAGE_MS_PER_MILLION * f["cards"] / 1e6
    # The residual is a mixed-pass timing proxy. Half is assigned to material for sensitivity.
    mixed = max(0, f["gbuffer"] - old_foliage)
    material = 0.5 * mixed * (1 - 1 / overdraw)
    candidate_mtri = f["non_canopy_meshlets"] * MAX_MESHLET_TRIANGLES / 1e6
    geometry = min(0.5 * mixed, VB_MS_PER_MILLION * candidate_mtri * rejected_fraction)
    return material + geometry - 0.10


def foliage_gain(f, snapshot):
    crowns, old_triangles = snapshot
    if not crowns:
        return 0
    removed = min(old_triangles, f["cards"])
    return (OLD_FOLIAGE_MS_PER_MILLION * removed
            - NEW_FOLIAGE_MS_PER_MILLION * F_MAX_TRIANGLES * crowns) / 1e6 - 0.30


def shadow_async(f, removed_fraction, cache_cost, overlap, queue_cost):
    removed = f["shadow"] * removed_fraction
    window = min(f["shadow"] - removed, f["gi"])
    return removed - cache_cost, window, max(0, overlap * window - queue_cost)


def report(label, rows, snapshot, target):
    def get(key):
        return median(f[key] for f in rows)

    # Both rows are hypotheses. No unvalidated cache or optical proxy is included in the first.
    no_cache = [shadow_async(f, 0, 0, 0.60, 0.10) for f in rows]
    strong_cache = [shadow_async(f, 0.702, 0.32, 0.60, 0.10) for f in rows]
    world_low = [world_gain(f, 1.25, 0.10) for f in rows]
    world_high = [world_gain(f, 2.00, 0.25) for f in rows]
    foliage = [foliage_gain(f, snapshot) for f in rows]

    def predict(cache):
        gpu = [f["gpu"] - w - leaves - c - d for f, w, leaves, (c, _, d)
               in zip(rows, world_high, foliage, cache, strict=True)]
        wall = median(max(g, f["cpu_e"]) + f["residual"] for g, f in zip(gpu, rows, strict=True))
        return median(gpu), wall

    gpu_nc, wall_nc = predict(no_cache)
    gpu_c, wall_c = predict(strong_cache)

    # Needed net main-view gain with CPU below the target and strong C/D: the GPU branch only.
    need_main = median(
        f["gpu"] + f["residual"] - c - d - target
        for f, (c, _, d) in zip(rows, strong_cache, strict=True)
    )
    window_threshold = median(min(max(0, f["shadow"] - 0.82), f["gi"]) for f in rows)

    print(f"{label}, N={len(rows)}, dt={get('dt'):.0f} ms")
    print(f"  frame/GPU/CPU/E: {get('frame'):.3f} / {get('gpu'):.3f} / "
          f"{get('cpu'):.3f} / {get('cpu_e'):.3f} ms")
    print(f"  GBuffer/Bough/VSM raster: {get('gbuffer'):.3f} / {get('bough'):.3f} / {get('shadow'):.3f} ms; "
          f"bough triangles {get('cards') / 1e6:.3f} M")
    print(f"  Non-canopy / 3b rejected meshlets: {get('non_canopy_meshlets'):.0f} / {get('rejected_3b'):.0f}; "
          f"world net {median(world_low):+.3f}…{median(world_high):+.3f} ms")
    print(f"  F net: {median(foliage):+.3f} ms; overlap window none/threshold/strong: "
          f"{median(v[1] for v in no_cache):.3f} / {window_threshold:.3f} / "
          f"{median(v[1] for v in strong_cache):.3f} ms")
    print(f"  D range after strong C: "
          f"{median(max(0, 0.30*v[1]-0.15) for v in strong_cache):.3f}…"
          f"{median(v[2] for v in strong_cache):.3f} ms")
    print(f"  Hypothetical no cache GPU/frame: {gpu_nc:.3f} / {wall_nc:.3f}; "
          f"strong C GPU/frame: {gpu_c:.3f} / {wall_c:.3f}; gain {get('frame') - wall_c:.3f} ms")
    print(f"  Required net main gain at {target:.3f} ms: {need_main:.3f} ms")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path, help="Published archive root or an equivalent local copy")
    args = parser.parse_args()
    root = args.root

    census = json.loads((root / CENSUS).read_text())
    pixels = census["dimensions"][0] * census["dimensions"][1]
    classes = census["winner_pixel_classes"]
    if sum(classes.values()) != pixels:
        raise ValueError("Pixel owner census does not cover the framebuffer")
    trees = census["visible"]["pixels"]
    other = classes["other_scene"]
    print(f"Census: tree owners {trees}/{pixels}, other scene {other}/{pixels} ({100*other/pixels:.2f}%), "
          f"clear depth {census['clear_depth_pixels']}; no fragment invocation counts")

    batch = [frames(root / BATCH / arm / "frameprofile.csv", BATCH_VIEWS)
             for arm in ("02-all", "03-all")]
    for view in BATCH_VIEWS:
        report(f"Batch 1 / {view}", batch[0][view] + batch[1][view], F_SNAPSHOT.get(view, (0, 0)), 7.751)
    print("Batch 1 / near, far: no timing windows")
    batch_inside = batch[0]["inside"] + batch[1]["inside"]
    free_gpu = median(f["cpu"] - 1.60 + f["residual"] for f in batch_inside)
    free_publication_cpu = median(f["cpu"] - 1.60 - f["publication"] for f in batch_inside)
    free_publication_frame = median(f["cpu"] - 1.60 - f["publication"] + f["residual"]
                                    for f in batch_inside)
    print(f"Batch 1 / inside with free GPU and CPU -1.60: {free_gpu:.3f} ms frame; "
          f"also removing all publication: {free_publication_cpu:.3f} ms CPU, "
          f"{free_publication_frame:.3f} ms frame")
    crown = frames(root / CROWN / "01-A/frameprofile.csv", CROWN_VIEWS)
    for name, view in zip(("inside", "hero", "sun", "near", "far"), CROWN_VIEWS, strict=True):
        report(f"Crown-fix A1 / {name}", crown[view], F_SNAPSHOT.get(name, (0, 0)), 8.00)
    warm = frames(root / WARM / "frameprofile.csv", ("entry-repeat-1",))
    report("24 Nov warm calibration only", warm["entry-repeat-1"], F_SNAPSHOT["inside"], 8.00)
    b1 = frames(root / CROWN / "02-B/frameprofile.csv", (CROWN_VIEWS[0], CROWN_VIEWS[-1]))
    far = [f for f in b1[CROWN_VIEWS[-1]] if not f["tree_vb_triangles"] and f["early_vb"] and f["late_vb"]]
    print(f"Stage 3 B1: far VB P50 {median(f['early_vb'] + f['late_vb'] for f in far):.3f} ms, "
          f"inside VSM raster P50 {median(f['tree_vsm'] for f in b1[CROWN_VIEWS[0]]):.3f} ms")


if __name__ == "__main__":
    main()
