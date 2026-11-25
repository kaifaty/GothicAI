#!/usr/bin/env python3
"""Run the prepared Vulkan pilot within one gq job, or summarize its archived output."""

import argparse
import json
import math
from pathlib import Path
from statistics import median
import subprocess
import sys
import time

from perf_gpu_log import read_log


ARMS = (("01-A", "direct"), ("02-B", "vb"), ("03-B", "vb"), ("04-A", "direct"))
PROFILES = ("empty", "sparse", "dense", "dense-high")
METRICS = (
    "frame_ms", "gpu_ms", "prepare_ms", "clear_ms", "raster_ms", "barrier_ms", "resolve_ms",
    "attributes_ms", "gradients_ms", "material_ms",
)
COUNTS = ("submitted_primitives", "won_pixels")
TRIM_MS = 250
WARMUP_SECONDS = 50
REFERENCE_RASTER_MS_PER_MILLION = 0.45
REFERENCE_INSIDE_MILLIONS = 4.435
RESERVE_MS = 0.30


def read_jsonl(path):
    if not path.exists():
        return []
    with path.open() as stream:
        return [json.loads(line) for line in stream]


def stats(values):
    if not values:
        return None
    values = sorted(values)
    if not all(math.isfinite(value) for value in values):
        raise ValueError("Non-finite timing or counter")
    return {"p50": median(values), "p95": values[math.ceil(len(values) * 0.95) - 1]}


def count_stats(values):
    return {"min": min(values), "max": max(values)} if values else None


def constant_count(window, key):
    count = window[key]
    return count["min"] if count and count["min"] == count["max"] else None


def clock_stats(frames, clocks):
    if not frames or any("mono_ns" not in frame for frame in frames):
        return None
    begin = min(frame["mono_ns"] for frame in frames)
    end = max(frame["mono_ns"] for frame in frames)
    values = [row["sm_mhz"] for row in clocks if begin <= row["mono_ns"] <= end and row["sm_mhz"] is not None]
    return {"samples": len(values), "min": min(values), "max": max(values)} if values else None


def arm_stats(out, name, clocks):
    records = read_jsonl(out / f"{name}.jsonl")
    profiles = {}
    for profile in PROFILES:
        frames = [row for row in records if row.get("type") == "frame" and row["profile"] == profile
                  and row["elapsed_ms"] >= TRIM_MS]
        if frames:
            profiles[profile] = {
                "frames": len(frames),
                "sm_mhz": clock_stats(frames, clocks),
                **{key: stats([row[key] for row in frames if row.get(key) is not None]) for key in METRICS},
                **{key: count_stats([row[key] for row in frames if row.get(key) is not None]) for key in COUNTS},
            }
    return {"profiles": profiles, "metadata": [row for row in records if row.get("type") != "frame"]}


def calibration(direct, visibility, delta):
    if any(profile not in delta for profile in ("dense", "dense-high")) or "gpu_ms" not in delta["dense"]:
        return None
    slopes = []
    for arm in (direct, visibility):
        low, high = arm["dense"], arm["dense-high"]
        if low["raster_ms"] is None or high["raster_ms"] is None:
            return None
        low_count = constant_count(low, "submitted_primitives")
        high_count = constant_count(high, "submitted_primitives")
        if low_count is None or high_count is None:
            return None
        primitives = high_count - low_count
        if primitives <= 0 or not constant_count(low, "won_pixels") or low["won_pixels"] != high["won_pixels"]:
            return None
        slope = (high["raster_ms"]["p50"] - low["raster_ms"]["p50"]) / (primitives / 1e6)
        if slope <= 0:
            return None
        slopes.append(slope)
    dense_delta = delta["dense"]["gpu_ms"]["p50"]
    dense_primitives = constant_count(visibility["dense"], "submitted_primitives") / 1e6
    if dense_primitives <= 0:
        return None
    direct_slope, vb_slope = slopes
    q = vb_slope / REFERENCE_RASTER_MS_PER_MILLION
    h1 = dense_delta - (vb_slope - direct_slope) * dense_primitives
    # H3 has no measured HiZ/history/wind cost in this pilot. These are conditional ceilings.
    ceilings = []
    for case, h3 in (("pilot_remainder_only", h1), ("design_reserve", RESERVE_MS)):
        equivalent = REFERENCE_INSIDE_MILLIONS - (1 + h3) / REFERENCE_RASTER_MS_PER_MILLION
        submission_limit = max(0, equivalent / q)
        ceilings.append({"case": case, "assumed_h3_ms": h3, "max_submitted_millions": submission_limit,
                         "additional_cut_from_pilot_dense": max(0, 1 - submission_limit / dense_primitives),
                         "feasible": equivalent > 0})
    return {"q": q, "direct_raster_ms_per_million": direct_slope, "vb_raster_ms_per_million": vb_slope,
            "pilot_h1_ms": h1, "stage3_conditional_ceilings": ceilings}


def compare(direct, visibility):
    direct, visibility = direct["profiles"], visibility["profiles"]
    delta, matching_clocks, matching_counts = {}, {}, {}
    for profile in direct.keys() & visibility.keys():
        a, b = direct[profile], visibility[profile]
        counts = all(constant_count(a, key) is not None and a[key] == b[key] for key in COUNTS)
        matching_counts[profile] = counts
        clocks = a["sm_mhz"], b["sm_mhz"]
        matching_clocks[profile] = (clocks[0]["min"] == clocks[0]["max"] == clocks[1]["min"] == clocks[1]["max"]
                                    if all(clocks) else None)
        if counts:
            delta[profile] = {key: {p: b[key][p] - a[key][p] for p in ("p50", "p95")}
                              for key in METRICS if a[key] is not None and b[key] is not None}
    dense_gpu = delta.get("dense", {}).get("gpu_ms")
    return {"vb_minus_direct_ms": delta, "matching_counts": matching_counts,
            "matching_fixed_sm_clock": matching_clocks, "calibration": calibration(direct, visibility, delta),
            "dense_consumes_reserve": None if dense_gpu is None else dense_gpu["p50"] >= RESERVE_MS}


def summary(out):
    clocks = read_log(out / "gpu.csv")
    arms = {name: arm_stats(out, name, clocks) for name, _ in ARMS}
    return {"trim_ms": TRIM_MS, "reserve_ms": RESERVE_MS, "verification": read_jsonl(out / "verify.jsonl"),
            "arms": arms, "pairs": {"02-B vs 01-A": compare(arms["01-A"], arms["02-B"]),
                                     "03-B vs 04-A": compare(arms["04-A"], arms["03-B"])}}


def run_arm(args, name, mode):
    out = args.output
    result_file = out / f"{name}.jsonl"
    timed = mode != "verify"
    command = [str(args.binary), str(args.shaders), str(args.asset), mode, str(result_file)]
    if timed:
        command.extend((str(WARMUP_SECONDS), str(args.samples)))
    print(f"{name}: {mode}", flush=True)
    start = time.monotonic()
    with (out / f"{name}.log").open("w") as log:
        result = subprocess.run(command, stdout=log, stderr=log, timeout=80 if timed else 100)
    print(f"{name}: {time.monotonic() - start:.2f}s", flush=True)
    if not timed:
        checks = [row for row in read_jsonl(result_file) if row.get("type") == "verification"]
        if not checks:
            raise ValueError(f"No verification results (exit {result.returncode}). See {out / 'verify.log'}")
        lost = sum(row["lost_pixels"] for row in checks)
        replaced = sum(row["replaced_pixels"] for row in checks)
        if lost or replaced:
            raise ValueError(f"Winning pixels lost: {lost}; replaced: {replaced}. Stopping before timing.")
        if result.returncode == 3 or not all(row["passed"] for row in checks):
            raise ValueError("Material or raster verification failed. Stopping before timing.")
    result.check_returncode()
    if timed:
        profiles = arm_stats(out, name, [])["profiles"]
        for profile in PROFILES:
            if profile not in profiles or any(profiles[profile][key] is None for key in (*COUNTS, "gpu_ms")):
                raise ValueError(f"{name} / {profile}: missing samples or counters after the initial {TRIM_MS}ms")


def run(args):
    for path in (args.binary, args.shaders, args.asset, args.output):
        if not path.is_absolute():
            raise ValueError(f"Pass an absolute path: {path}")
    if args.samples <= 0:
        raise ValueError("Sample count must be positive")
    if not args.binary.is_file() or not args.shaders.is_dir() or not args.asset.is_file():
        raise ValueError("The pilot executable, shader directory and exported asset must exist")
    args.output.mkdir(parents=True, exist_ok=False)
    try:
        run_arm(args, "verify", "verify")
        with (args.output / "gpu.log").open("w") as log:
            monitor = subprocess.Popen([sys.executable, str(Path(__file__).with_name("perf_gpu_log.py")),
                                        str(args.output / "gpu.csv")], stdout=log, stderr=log)
            try:
                for name, mode in ARMS:
                    run_arm(args, name, mode)
            finally:
                if monitor.poll() is None:
                    monitor.terminate()
                    monitor.wait(timeout=6)
    finally:
        (args.output / "summary.json").write_text(json.dumps(summary(args.output), indent=2, sort_keys=True) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subcommands = parser.add_subparsers(dest="command", required=True)
    launcher = subcommands.add_parser("run", help="run verification and the four timed arms on Ryzen")
    launcher.add_argument("binary", type=Path)
    launcher.add_argument("shaders", type=Path)
    launcher.add_argument("asset", type=Path)
    launcher.add_argument("output", type=Path)
    launcher.add_argument("--samples", type=int, default=256)
    report = subcommands.add_parser("report", help="recompute the summary from an archived output directory")
    report.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "run":
            run(args)
        else:
            print(json.dumps(summary(args.output), indent=2, sort_keys=True))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"tree-nanite-step1: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
