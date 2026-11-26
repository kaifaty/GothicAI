"""Offline coverage-LOD estimate from archived per-crown censuses.

Goal: price demoting the main-view cut of spruce crowns whose winning-pixel
contribution is low, without any game run. Inputs are local copies of the
Ryzen archives (see the note for paths):

  trees.csv, placements.csv, census-frame.txt
      fir-view1-pixels-20261122T184646Z (census, tick 11644, inside hold)
  {inside,walk}.csv, {inside,walk}-candidates.csv
      fir-bough-depth-20261122T201811Z/abba comparison counters + the
      granularity replay cohort tree-8ms-cull-granularity/pair/analysis/
      final-depth. The pair archive does not preserve the owner-to-tree
      join, so pair scenes enter as aggregate context only.

Prices are the RTX 3080 calibrations of tree-cull-granularity.md:
0.45 ns per removed card triangle (B1 GBuffer delta), 0.20 ns per new
triangle (F model with margin), Bough-cut P50 0.610 ms inside over
992 190 replay reads. Mechanism cost is bounded by the crown-level test
price of the same note (0.001-0.011 ms) plus win counting; 0.05-0.10 ms
is used as a conservative bracket.

Usage: python3 analyze.py <artifact-dir> [--json out.json]
"""

import argparse
import csv
import json
from pathlib import Path

REMOVED_NS = 0.45   # ms per million removed card triangles
NEW_NS = 0.20       # ms per million replacement triangles
BOUGH_CUT_MS = 0.610  # inside P50, census replay
MECH_LO, MECH_HI = 0.05, 0.10
TAUS = (1, 16, 100, 256, 1000)
KEPT = (0.0, 0.16, 0.4)  # impostor ceiling, two-level drop, one-level drop


def rows(path):
    with open(path, newline="") as fh:
        return list(csv.DictReader(fh))


def census_rows(folder):
    trees = rows(folder / "trees.csv")
    placements = rows(folder / "placements.csv")
    assert [t["key"] for t in trees] == [p["key"] for p in placements]
    total = int((folder / "census-frame.txt").read_text().split()[6])
    submitted = [t for t in trees if t["cut"] != "none"]
    assert sum(int(t["bough_triangles"]) for t in submitted) == total
    out = []
    for t in submitted:
        out.append(dict(key=t["key"], id=int(t["id"]), compact=t["cut"] == "compact",
                        tris=int(t["bough_triangles"]), units=int(t["bough_units"]),
                        wins=int(t["needle_3d_pixels"]), cut=t["cut"]))
    return out, total


def pair_context(folder, scene):
    """Aggregate counters only: the pair archive does not preserve the
    owner-to-tree join (see tree-cull-granularity.md), so per-crown wins
    cannot be decoded offline for these scenes."""
    counters = rows(folder / f"{scene}.csv")[0]
    candidates = {}
    for r in rows(folder / f"{scene}-candidates.csv"):
        # Same membership filter as the archived replay (paired_capture.py):
        # frustum sphere, impostor transition height, whole-crown early reject.
        if (float(r["frustum_distance"]) > -float(r["dynamic_radius"])
                and float(r["height_px"]) > 46 * 80 / 49
                and int(r["whole_reject"]) == 0):
            candidates[r["key"]] = r
    assert sum(int(c["a_tris"]) for c in candidates.values()) == int(counters["a_triangles"])
    return dict(scene=scene, tick=int(counters["tick"]), crowns=len(candidates),
                triangles=int(counters["a_triangles"]),
                whole_rejected=int(counters["occluded"]),
                compact=sum(1 for c in candidates.values() if c["compact"] == "1"))


def distribution(data, label):
    buckets = [("0", lambda w: w == 0), ("1-15", lambda w: 1 <= w < 16),
               ("16-99", lambda w: 16 <= w < 100), ("100-255", lambda w: 100 <= w < 256),
               ("256-999", lambda w: 256 <= w < 1000), (">=1000", lambda w: w >= 1000)]
    total_tris = sum(r["tris"] for r in data)
    total_wins = sum(r["wins"] for r in data)
    table = []
    for name, pred in buckets:
        group = [r for r in data if pred(r["wins"])]
        tris = sum(r["tris"] for r in group)
        wins = sum(r["wins"] for r in group)
        table.append(dict(bucket=name, crowns=len(group), triangles=tris,
                          triangle_pct=round(100 * tris / total_tris, 2),
                          wins=wins, win_pct=round(100 * wins / total_wins, 2) if total_wins else 0.0,
                          compact=sum(1 for r in group if r["compact"])))
    return dict(scene=label, crowns=len(data), triangles=total_tris, wins=total_wins, buckets=table)


def simulate(data, tau, kept):
    demoted = [r for r in data if r["wins"] < tau]
    kept_rows = [r for r in data if r["wins"] >= tau]
    dem_tris = sum(r["tris"] for r in demoted)
    dem_units = sum(r["units"] for r in demoted)
    all_units = sum(r["units"] for r in data)
    removed_m = dem_tris * (1 - kept) / 1e6
    new_m = dem_tris * kept / 1e6
    gpu = REMOVED_NS * removed_m - NEW_NS * new_m
    # Traversal bonus upper bound: reads scale with compact node count; a
    # coarser cut still walks its retained nodes, so scale by (1 - kept).
    bonus = BOUGH_CUT_MS * dem_units / all_units * (1 - kept) if all_units else 0.0
    return dict(tau=tau, kept=kept, demoted_crowns=len(demoted),
                kept_crowns=len(kept_rows),
                kept_triangles=sum(r["tris"] for r in kept_rows),
                removed_m=round(removed_m, 3), new_m=round(new_m, 3),
                gpu_ms=round(gpu, 3),
                net_lo_ms=round(gpu - MECH_HI, 3),
                net_hi_ms=round(gpu - MECH_LO + bonus, 3),
                walk_bonus_ms=round(bonus, 3))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("folder", type=Path)
    parser.add_argument("--json", type=Path, default=None)
    args = parser.parse_args()
    folder = args.folder

    census, census_total = census_rows(folder)

    result = {
        "inputs": dict(census_tick=11644, census_triangles=census_total),
        "distributions": [distribution(census, "census-inside")],
        "pair_context": [pair_context(folder, "inside"), pair_context(folder, "walk")],
        "simulation": {name: [simulate(data, tau, kept) for tau in TAUS for kept in KEPT]
                       for name, data in (("census-inside", census),)},
    }
    text = json.dumps(result, indent=2)
    print(text)
    if args.json:
        args.json.write_text(text)


if __name__ == "__main__":
    main()