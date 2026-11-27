#!/usr/bin/env python3
"""Separate steady and resource-publication frames in archived frame profiles.

Read-only P1a analysis. The selected arms use serial recording, so the reported
zones are main-thread wall time. Parent zones are never added to their children.
"""
import argparse
import json
from pathlib import Path
import sys

ARMS = {
    "current-a1": "coverage-pipeline/price-a1",
    "current-a2": "coverage-pipeline/price-a2",
    "n2-d2": "instance-management/d2",
    "n2-d3": "instance-management/d3",
}
NATIVE_ARMS = {
    "native-market": "perf-stand/runs/perf-20261127T073344Z-2964/market/repeat-1",
    "native-forest": "perf-stand/runs/perf-20261127T073344Z-2964/forest/repeat-1",
}
EXPECTED = {"perf-10k/far", "perf-10k/near"}
ZONES = (
    "frame_ms", "active_cpu_ms", "world_pre_frame", "visuals_prepare",
    "trees_prepare", "levels_prepare", "wind_prepare", "morph_prepare", "strands_prepare",
    "pfx_prepare", "lights_tick", "lighting_prep", "lights_total", "visibility_cpu",
    "storage_commit", "storage_pack", "storage_plan_scan", "storage_plan_copy", "storage_plan_history",
    "storage_dispatch", "storage_join", "clusters_commit", "prepare_uniforms",
    "houses_tick", "grass_update", "commit_ubo", "bindless_rebuild", "bindless_patch", "rt_scene",
    "encode_main", "encode_record", "world_other", "visuals_other", "tree_level_loops",
)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path("/mnt/shared/gothic-ai"))
    parser.add_argument("--scripts", type=Path, required=True, help="Directory containing perf_report.py")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    sys.path.insert(0, str(args.scripts))
    import perf_report as perf

    original_parts = perf.segment_parts
    perf.segment_parts = lambda segment: tuple(segment.split("/")) if segment in EXPECTED else original_parts(segment)

    def zone(row, name):
        return row["zones"].get(name, 0.0)

    def derived(row, name):
        if name == "active_cpu_ms":
            return row["cpu_ms"] - sum(zone(row, z) for z in ("fence_poll", "limiter", "upload_wait"))
        if name == "lights_total":
            return zone(row, "lights_tick") + zone(row, "lighting_prep")
        if name == "tree_level_loops":
            return zone(row, "trees_prepare") + zone(row, "levels_prepare")
        if name == "world_other":
            return zone(row, "world_pre_frame") - sum(zone(row, z) for z in (
                "visuals_prepare", "houses_tick", "lights_tick", "grass_update", "commit_ubo", "pfx_prepare"))
        if name == "visuals_other":
            return zone(row, "visuals_prepare") - sum(zone(row, z) for z in (
                "wind_prepare", "morph_prepare", "strands_prepare", "trees_prepare", "levels_prepare"))
        if name == "frame_ms":
            return row[name]
        return zone(row, name)

    def resource_frame(row):
        activity = row.get("texture_activity", {})
        publication = any(activity.get(kind, False) for kind in ("read", "upload", "complete", "ready", "publish"))
        return (publication or row["amounts"].get("rt_tlas_builds", 0) != 0
                or row["amounts"].get("storage_growths", 0) != 0 or zone(row, "bindless_rebuild") != 0)

    def summarize(rows):
        result = {"frames": len(rows)}
        if not rows:
            return result
        for name in ZONES:
            values = [derived(row, name) for row in rows]
            result[name] = perf.describe(values)
        result["resource_counters"] = {
            name: sum(row["amounts"].get(name, 0) for row in rows)
            for name in ("rt_tlas_builds", "storage_growths", "grow_bytes", "upload_execution_bytes")
        }
        result["object_counts"] = {
            name: perf.describe([row["amounts"].get(name, 0) for row in rows])
            for name in ("objects_levels_n", "objects_tree_n", "levels_computed", "levels_skipped",
                         "tree_fade_computed", "tree_fade_skipped")
        }
        return result

    all_rows = {"far": [], "near": []}
    report = {"arms": {}, "classification": (
        "resource: any texture read/upload/complete/ready/publish, TLAS build, storage growth or bindless rebuild; "
        "all other submitted-frame intervals are steady. The first 250 ms of each window is trimmed.")}
    for name, relative in ARMS.items():
        path = args.root / relative / "frameprofile.csv"
        groups, warnings = perf.samples_from_profile(path)
        if warnings:
            raise RuntimeError(f"{path}: {warnings}")
        arm = {}
        for view in ("far", "near"):
            rows = groups.get(("perf-10k", view), [])
            if not rows:
                raise RuntimeError(f"{path}: no {view} frames")
            rows = [row for row in rows if row["begin_ns"] >= rows[0]["begin_ns"] + 250_000_000]
            all_rows[view].extend(rows)
            resource = [row for row in rows if resource_frame(row)]
            steady = [row for row in rows if not resource_frame(row)]
            arm[view] = {"all": summarize(rows), "steady": summarize(steady), "resource": summarize(resource)}
        report["arms"][name] = arm
    report["combined"] = {}
    for view, rows in all_rows.items():
        resource = [row for row in rows if resource_frame(row)]
        steady = [row for row in rows if not resource_frame(row)]
        report["combined"][view] = {"all": summarize(rows), "steady": summarize(steady), "resource": summarize(resource)}

    report["native"] = {}
    for name, relative in NATIVE_ARMS.items():
        path = args.root / relative / "frameprofile.csv"
        groups, warnings = perf.samples_from_profile(path)
        if warnings:
            raise RuntimeError(f"{path}: {warnings}")
        views = {}
        for (_, view), rows in groups.items():
            rows = [row for row in rows if row["begin_ns"] >= rows[0]["begin_ns"] + 250_000_000]
            resource = [row for row in rows if resource_frame(row)]
            steady = [row for row in rows if not resource_frame(row)]
            views[view] = {"all": summarize(rows), "steady": summarize(steady), "resource": summarize(resource)}
        report["native"][name] = views

    if args.output:
        args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n")
    columns = ("frame_ms", "active_cpu_ms", "world_pre_frame", "tree_level_loops", "pfx_prepare",
               "lights_total", "visibility_cpu", "storage_commit", "clusters_commit", "encode_record")
    print("arm view class frames " + " ".join(columns))
    for name, arm in report["arms"].items():
        for view in ("far", "near"):
            for kind in ("steady", "resource"):
                summary = arm[view][kind]
                if not summary["frames"]:
                    continue
                values = [f"{summary[column]['p50']:.3f}" for column in columns]
                print(name, view, kind, summary["frames"], *values)
    for view in ("far", "near"):
        for kind in ("steady", "resource"):
            summary = report["combined"][view][kind]
            if not summary["frames"]:
                continue
            values = [f"{summary[column]['p50']:.3f}" for column in columns]
            print("combined", view, kind, summary["frames"], *values)
    for name, views in report["native"].items():
        for view in sorted(views):
            for kind in ("steady", "resource"):
                summary = views[view][kind]
                if not summary["frames"]:
                    continue
                values = [f"{summary[column]['p50']:.3f}" for column in columns]
                print(name, view, kind, summary["frames"], *values)
    if args.output:
        print(f"Wrote {args.output}")


if __name__ == "__main__":
    main()
