#!/usr/bin/env python3
"""Measure how the navigation teacher's walk key depends on the surroundings.

    python3 -I scripts/nav_pace_teacher.py --output /abs/teacher-pace.json --workers 8 \\
        --corpus v4 /abs/nav-demos-v4 /abs/nav-demos-v4/manifest.json \\
        --corpus v5 /abs/corpus-v5 /abs/v5-compact.json

Only the frozen training recordings of each manifest are read: the population a
pace-input learner trains on. For every decision the script takes the teacher's
walk key and three learner inputs of the same decision:

- eye: the nearest steep eye hit (hit and |normal_y| < 0.5), along the ray;
- wall: the nearest remembered wall cell (wall >= 0.5) of the 64x64 near map,
  25 cm cells, measured from the map centre;
- goal: the road goal distance.

The output holds decisions and walk decisions by corpus, family and distance
bin. Facts only: nothing is trained or changed.
"""
from __future__ import annotations

import argparse
import json
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import nav_demos
from nav_bc_data import load_training_manifest

OBSTACLE_EDGES_CM = (100, 200, 400)
OBSTACLE_BINS = ("<1 m", "1-2 m", "2-4 m", ">=4 m or none")
GOAL_EDGES_CM = (500, 2000, 5000)
GOAL_BINS = ("<5 m", "5-20 m", "20-50 m", ">=50 m")
MEASURES = {"eye": OBSTACLE_BINS, "wall": OBSTACLE_BINS, "goal": GOAL_BINS}
WALL = nav_demos.CHANNELS.index("wall")


def _wall_distance_grid(size: int = nav_demos.NEAR_SIZE, cell_cm: float = 25) -> np.ndarray:
    z, x = np.indices((size, size), dtype=np.float32)
    return np.hypot((x + .5 - size / 2) * cell_cm, (z + .5 - size / 2) * cell_cm)


def decision_bins(records: np.ndarray, grid: np.ndarray | None = None) -> dict[str, np.ndarray]:
    """Bin index per decision for every measure; inf (nothing seen) joins the last bin."""
    grid = _wall_distance_grid() if grid is None else grid
    inputs = nav_demos.learner_input(records)
    eye = inputs["eye"]
    steep = (eye["hit"] != 0) & (np.abs(eye["normal_y"]) < .5)
    eye_cm = np.where(steep, eye["distance_cm"], np.inf).min(axis=1)
    wall = inputs["memory_near"]["channels"][..., WALL] >= 128
    wall_cm = np.where(wall, grid, np.inf).reshape(len(records), -1).min(axis=1)
    goal_cm = inputs["roads"]["goal_distance_cm"]
    return dict(eye=np.searchsorted(OBSTACLE_EDGES_CM, eye_cm, side="right"),
                wall=np.searchsorted(OBSTACLE_EDGES_CM, wall_cm, side="right"),
                goal=np.searchsorted(GOAL_EDGES_CM, goal_cm, side="right"))


def episode_counts(samples: str) -> dict:
    """Decisions and walk decisions per measure bin, plus totals, for one episode."""
    grid = _wall_distance_grid()
    counts = {name: np.zeros((len(bins), 2), np.int64) for name, bins in MEASURES.items()}
    total = np.zeros(2, np.int64)
    for records in nav_demos.iter_chunks(samples, 64):
        walk = (records["keys"]["walk"] != 0).astype(np.int64)
        total += (len(walk), int(walk.sum()))
        for name, index in decision_bins(records, grid).items():
            np.add.at(counts[name], (index, 0), 1)
            np.add.at(counts[name], (index, 1), walk)
    return dict(total=total.tolist(), **{name: value.tolist() for name, value in counts.items()})


def _empty():
    return dict(total=[0, 0], **{name: [[0, 0] for _ in bins] for name, bins in MEASURES.items()})


def _add(target, value):
    target["total"] = [a + b for a, b in zip(target["total"], value["total"])]
    for name in MEASURES:
        target[name] = [[a + b for a, b in zip(left, right)] for left, right in zip(target[name], value[name])]


def measure(corpora, workers: int = 1) -> dict:
    result = dict(bins={name: list(bins) for name, bins in MEASURES.items()},
                  edges_cm=dict(eye=list(OBSTACLE_EDGES_CM), wall=list(OBSTACLE_EDGES_CM), goal=list(GOAL_EDGES_CM)),
                  definitions=dict(eye="nearest eye hit with |normal_y| < 0.5, distance along the ray",
                                   wall="nearest near-memory cell with wall >= 0.5 (128/255), 25 cm cells, from the map centre",
                                   goal="roads.goal_distance_cm", walk="teacher keys.walk on the same decision"),
                  corpora={})
    with ProcessPoolExecutor(max_workers=workers) as pool:
        for name, source, manifest in corpora:
            _, entries = load_training_manifest(Path(manifest), Path(source))
            paths = [str(nav_demos.samples_path(entry["directory"])) for entry in entries]
            corpus = dict(source=str(source), manifest=str(manifest), episodes=len(entries),
                          all=_empty(), families={})
            for entry, counts in zip(entries, pool.map(episode_counts, paths, chunksize=4)):
                if counts["total"][0] != entry["decisions"]:
                    raise ValueError(f"recorded decisions differ from the manifest: {entry['directory']}")
                family = corpus["families"].setdefault(entry["family"], dict(episodes=0, **_empty()))
                family["episodes"] += 1
                _add(family, counts)
                _add(corpus["all"], counts)
            result["corpora"][name] = corpus
            print(json.dumps(dict(corpus=name, episodes=len(entries), decisions=corpus["all"]["total"][0])), flush=True)
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--corpus", nargs=3, action="append", required=True, metavar=("NAME", "SOURCE", "MANIFEST"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--workers", type=int, default=1)
    args = parser.parse_args()
    if not args.output.is_absolute() or any(not Path(path).is_absolute() for _, *paths in args.corpus for path in paths):
        parser.error("--output and corpus paths must be absolute")
    if args.workers < 1:
        parser.error("--workers must be positive")
    result = measure(args.corpus, args.workers)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
