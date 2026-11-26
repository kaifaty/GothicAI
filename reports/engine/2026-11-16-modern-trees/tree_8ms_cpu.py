#!/usr/bin/env python3
"""Recompute the E CPU budget from one archived frame profile and segment."""

import argparse
from itertools import groupby, pairwise
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from perf_report import percentile, read_profile  # noqa: E402


ZONES = ("tick", "finish_step", "parallel_animation", "npc_full_tick",
         "npc_simplified_tick", "npc_pre_tick", "encode", "encode_main",
         "storage_pack", "rt_scene")
TARGET_MS = 7.8


def samples(path, segment):
    rows, _, _, _ = read_profile(path)
    if any(row["overflow"] for row in rows):
        raise ValueError(f"{path}: truncated frame profile")
    for name, window in groupby(rows, key=lambda row: row["segment"]):
        if name != segment:
            continue
        window = list(window)
        submitted = [i for i, row in enumerate(window) if row["outcome"] == 1]
        if len(submitted) < 2:
            continue
        start = window[submitted[0]]["begin_ns"] + 250_000_000
        for left, right in pairwise(submitted):
            row = window[left]
            if row["begin_ns"] < start or not row["gpu_done"] or row["gpu_ns"] <= 0:
                continue
            attempts = window[left:right]
            sample = {zone: sum(r[f"{zone}_ns"] for r in attempts) / 1e6 for zone in ZONES}
            sample["active"] = sum(r["end_ns"] - r["begin_ns"] - r["fence_poll_ns"] for r in attempts) / 1e6
            sample["frame"] = (window[right]["begin_ns"] - row["begin_ns"]) / 1e6
            sample["dt"] = row["dt"]
            sample["gpu"] = row["gpu_ns"] / 1e6
            sample["npc_simplified_ticked"] = row["npc_simplified_ticked"]
            sample["published"] = any(r["bindless_rebuild_calls"] or r["bindless_patch_calls"] or r["rt_tlas_builds"]
                                      for r in attempts)
            yield sample


def report(frames):
    def p50(values):
        return percentile(values, 0.50)

    def median_field(name):
        return p50([frame[name] for frame in frames])

    window_ms = sum(f["frame"] for f in frames)
    print(f"{len(frames)} frames, {1000 * len(frames) / window_ms:.2f} FPS; "
          f"CPU > GPU: {sum(f['active'] > f['gpu'] for f in frames)}; "
          f"descriptor/TLAS work: {sum(f['published'] for f in frames)}")
    for name in ("frame", "gpu", "active", *ZONES):
        print(f"{name}: {median_field(name):.3f} ms P50")
    steps = [f["dt"] for f in frames]
    print(f"simulation step: {p50(steps):.0f} ms P50 (range {min(steps)}–{max(steps)})")
    print(f"simulation ms / wall second: {1000 * sum(steps) / window_ms:.1f}")
    print(f"npc_simplified_ticked: {median_field('npc_simplified_ticked'):.0f} P50")
    # Sensitivity only. The archive does not isolate independent NPC or packet work.
    for label, npc_fraction, encode_gain, overhead in (("modest", 0.35, 0.40, 0.25),
                                                       ("favorable", 0.55, 0.80, 0.15)):
        cpu_after_npcs = p50([f["active"] - npc_fraction * f["npc_simplified_tick"] for f in frames])
        needed_encode = max(0.0, cpu_after_npcs + overhead - TARGET_MS)
        print(f"{label}: CPU {cpu_after_npcs - encode_gain + overhead:.3f} ms P50; "
              f"encode gain required at this NPC fraction: {needed_encode:.3f} ms")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", type=Path)
    parser.add_argument("segment")
    args = parser.parse_args()
    frames = list(samples(args.profile, args.segment))
    if not frames:
        parser.error(f"{args.profile}: no usable frames in {args.segment!r}")
    report(frames)


if __name__ == "__main__":
    main()
