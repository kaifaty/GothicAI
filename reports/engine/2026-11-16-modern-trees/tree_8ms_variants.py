#!/usr/bin/env python3
"""Recompute inside-forest calibration and conditional savings from archived profiles."""

import argparse
from dataclasses import dataclass
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from perf_report import percentile, read_gpu_passes, read_profile  # noqa: E402


GI_FORK = ("GI-Alloc", "GI-Trace", "GI-HashMap", "GI-Lighting")
GI_ALL = (*GI_FORK, "GI-Stats", "Irradiance")
VSM_ALL = ("VSM-pages", "VSM-pages-epipolar", "VSM-pages-alloc", "VSM-visibility", "VSM-rendering")


@dataclass(frozen=True)
class Frame:
    full: float
    gpu: float
    cpu: float
    passes: dict[str, float]

    def total(self, labels):
        return sum(self.passes.get(label, 0.0) for label in labels)


def read_frames(path, segments):
    rows, _, _, _ = read_profile(path)
    passes = read_gpu_passes(path)
    if not passes or any(row.get("overflow", 0) for row in rows):
        raise ValueError(f"{path}: missing GPU sidecar or truncated profile")
    rows = [row for row in rows if row["segment"] in segments]
    submitted = [i for i, row in enumerate(rows) if row["outcome"] == 1]
    if len(submitted) < 2:
        raise ValueError(f"{path}: inside segment not found")
    start = rows[submitted[0]]["begin_ns"] + 250_000_000
    frames = []
    for left, right in zip(submitted, submitted[1:]):
        row = rows[left]
        begin = row["begin_ns"]
        if begin < start or not row.get("gpu_done", 1) or row["gpu_ns"] <= 0:
            continue
        labels = passes.get(row["attempt"])
        if not labels:
            continue
        full = (rows[right]["begin_ns"] - begin) / 1e6
        gpu = row["gpu_ns"] / 1e6
        cpu = sum(attempt["end_ns"] - attempt["begin_ns"] - attempt["fence_poll_ns"]
                  for attempt in rows[left:right]) / 1e6
        frames.append(Frame(full, gpu, cpu, {name: ns / 1e6 for name, ns in labels.items()}))
    if not frames:
        raise ValueError(f"{path}: no usable frames after warm-up trim")
    return frames


@dataclass(frozen=True)
class Scenario:
    name: str
    shadow_removed: float
    shadow_cost: float
    main_view_gain: float
    gi_gain: float
    cpu_gain: float
    overlap: float = 0.0
    submit_cost: float = 0.0

    def gpu_savings(self, frame):
        shadow = frame.passes.get("VSM-rendering", 0.0)
        gi = frame.total(GI_FORK)
        # Only the surviving raster and probe work is available to overlap.
        overlap_window = min(shadow * (1 - self.shadow_removed), max(0.0, gi - self.gi_gain))
        async_gain = max(0.0, self.overlap * overlap_window - self.submit_cost)
        shadow_gain = shadow * self.shadow_removed - self.shadow_cost
        view_gain = min(self.main_view_gain, frame.passes.get("GBuffer", 0.0))
        return shadow_gain + view_gain + self.gi_gain + async_gain

    def predict(self, frame):
        gpu = frame.gpu - self.gpu_savings(frame)
        cpu = max(0.0, frame.cpu - self.cpu_gain)
        residual = frame.full - max(frame.gpu, frame.cpu)
        return max(gpu, cpu) + residual


SCENARIOS = (
    Scenario("D alone, modest", 0, 0, 0, 0, 0, 0.30, 0.15),
    Scenario("D alone, favorable", 0, 0, 0, 0, 0, 0.60, 0.10),
    Scenario("C+F+K+E", 0.90 * 0.78, 0.20, 0.77, 0.12, 1.60),
    Scenario("C+F+K+D+E", 0.90 * 0.78, 0.20, 0.77, 0.12, 1.60, 0.60, 0.10),
    Scenario("C+G+K+D+E", 0.90 * 0.78, 0.20, 0.76, 0.12, 1.60, 0.60, 0.10),
    Scenario("C+F+G+K+E, disjoint", 0.90 * 0.78, 0.20, 0.77 + 0.76, 0.12, 1.60),
    Scenario("C+F+G+D+E, disjoint", 0.90 * 0.78, 0.20, 0.77 + 0.76, 0, 1.60, 0.60, 0.10),
    Scenario("C+F+G+K+D+E, disjoint", 0.90 * 0.78, 0.20, 0.77 + 0.76, 0.12, 1.60, 0.60, 0.10),
    Scenario("Threshold, more GBuffer savings", 0.95 * 0.94, 0.20, 2.00, 0.12, 1.60, 0.60, 0.10),
    Scenario("Threshold, more GI savings", 0.95 * 0.94, 0.20, 1.25, 0.87, 1.60, 0.60, 0.10),
)


def report(title, frames, scenarios=SCENARIOS):
    def p50(values):
        return percentile(values, 0.50)

    full = p50([frame.full for frame in frames])
    p95 = percentile([frame.full for frame in frames], 0.95)
    print(f"\n{title}, N={len(frames)}: frame {full:.3f}/{p95:.3f} ms P50/P95")
    timing = [("GPU", lambda f: f.gpu), ("active CPU", lambda f: f.cpu),
              ("VSM", lambda f: f.total(VSM_ALL)), ("GI", lambda f: f.total(GI_ALL)),
              ("D upper bound", lambda f: min(f.total(GI_FORK), f.total(("VSM-rendering",))))]
    for label, duration in timing:
        print(f"  {label}: {p50([duration(frame) for frame in frames]):.3f} ms")
    for label in ("VSM-rendering", "GBuffer", "GI-Trace", "GI-Lighting"):
        print(f"  {label}: {p50([frame.passes.get(label, 0.0) for frame in frames]):.3f} ms")
    for scenario in scenarios:
        # This is a model on archived frames, with the observed residual held fixed.
        predicted = p50([scenario.predict(frame) for frame in frames])
        gpu_gain = p50([scenario.gpu_savings(frame) for frame in frames])
        print(f"  {scenario.name}: {predicted:.3f} ms ({full - predicted:+.3f} ms full, "
              f"{gpu_gain:+.3f} ms GPU savings)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("batch", type=Path, help="tree-8ms-batch1/result-20261125T135755Z")
    parser.add_argument("stage3", type=Path, help="tree-nanite-step3/result-20261126T043449Z")
    parser.add_argument("warm", type=Path, help="forest-frame-budget-20261124/evidence-final")
    args = parser.parse_args()

    batch = []
    for arm in ("02-all", "03-all"):
        frames = read_frames(args.batch / arm / "frameprofile.csv", {"inside"})
        report(arm, frames)
        batch.extend(frames)
    report("Batch 1 pooled", batch)
    report("Warm control", read_frames(args.warm / "frameprofile.csv", {"entry-repeat-1"}))
    report("Stage 3 01-A", read_frames(args.stage3 / "01-A/frameprofile.csv",
                                      {"perf-tree-nanite/view-1-inside"}))
    report("Stage 3 02-B", read_frames(args.stage3 / "02-B/frameprofile.csv",
                                      {"perf-tree-nanite/view-1-inside"}), scenarios=())


if __name__ == "__main__":
    main()
