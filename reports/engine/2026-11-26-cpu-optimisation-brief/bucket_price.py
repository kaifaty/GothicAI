#!/usr/bin/env python3
"""Compare direct BucketPublish metrics on A/B/B/A texture-publication frames."""
import argparse
import json
from pathlib import Path
import sys

ARM_NAMES = ("a1", "b1", "b2", "a2")
EXPECTED_SELECTIVE = {"a1": False, "b1": True, "b2": True, "a2": False}
METRICS = ("frame_ms", "active_cpu_ms", "bucket_publish", "bucket_publish_residual", "encode_record")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("runs", nargs=4, type=Path, help="A1 B1 B2 A2 run roots with market/forest subdirectories")
    parser.add_argument("--scripts", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    sys.path.insert(0, str(args.scripts))
    import perf_report as perf

    def zone(row, name):
        return row["zones"].get(name, 0.0)

    def resource_frame(row):
        activity = row.get("texture_activity", {})
        return (any(activity.get(kind, False) for kind in ("read", "upload", "complete", "ready", "publish"))
                or row["amounts"].get("rt_tlas_builds", 0) != 0
                or row["amounts"].get("storage_growths", 0) != 0 or zone(row, "bindless_rebuild") != 0)

    def summarize(rows):
        result = {"frames": len(rows)}
        if not rows:
            return result
        for name in METRICS:
            if name == "active_cpu_ms":
                values = [row["cpu_ms"] - sum(zone(row, z) for z in ("fence_poll", "limiter", "upload_wait"))
                          for row in rows]
            elif name == "bucket_publish_residual":
                values = [zone(row, "encode_main") - sum(zone(row, z) for z in (
                    "world_pre_frame", "storage_commit", "clusters_commit", "rt_scene",
                    "bindless_rebuild", "bindless_patch", "lighting_prep")) for row in rows]
            elif name == "frame_ms":
                values = [row[name] for row in rows]
            else:
                values = [zone(row, name) for row in rows]
            result[name] = perf.describe(values)
        for name in ("buckets_total", "buckets_refreshed"):
            result[name] = perf.describe([row["amounts"].get(name, 0) for row in rows])
        return result

    report = {"arms": {}, "errors": []}
    metadata = {}
    for name, run in zip(ARM_NAMES, args.runs):
        steady, publish = [], []
        run = run.resolve()
        metadata[name] = []
        for place in ("market", "forest"):
            path = run / place / "repeat-1"
            meta = json.loads((path / "metadata.json").read_text())
            metadata[name].append(meta)
            groups, warnings = perf.samples_from_profile(path / "frameprofile.csv")
            if warnings:
                report["errors"].append(f"{name}/{place}: {warnings}")
            for (_, view), rows in groups.items():
                publish.extend(row for row in rows if row.get("texture_activity", {}).get("publish", False))
                rows = [row for row in rows if row["begin_ns"] >= rows[0]["begin_ns"] + 250_000_000]
                steady.extend(row for row in rows if not resource_frame(row))
        report["arms"][name] = {"steady": summarize(steady), "publish": summarize(publish)}
        if not publish:
            report["errors"].append(f"{name}: no texture-publication frames")
        expected = "1" if EXPECTED_SELECTIVE[name] else "0"
        for meta in metadata[name]:
            actual = meta.get("game_env", {}).get("OPENGOTHIC_BUCKET_SELECTIVE_REFRESH")
            if actual != expected:
                report["errors"].append(f"{name}: switch is {actual!r}, expected {expected!r}")

    for name in ARM_NAMES:
        for meta in metadata[name]:
            other = metadata["a1"][0]
            if meta.get("binary_sha256") != other.get("binary_sha256"):
                report["errors"].append(f"{name}: binary hash differs")
            if meta.get("source_fingerprint") != other.get("source_fingerprint"):
                report["errors"].append(f"{name}: source fingerprint differs")

    report["comparisons"] = {}
    for cohort in ("publish", "steady"):
        comparison = {}
        for metric in METRICS:
            a1 = report["arms"]["a1"][cohort].get(metric, {}).get("p50")
            b1 = report["arms"]["b1"][cohort].get(metric, {}).get("p50")
            b2 = report["arms"]["b2"][cohort].get(metric, {}).get("p50")
            a2 = report["arms"]["a2"][cohort].get(metric, {}).get("p50")
            if None not in (a1, b1, b2, a2):
                comparison[metric] = {"a1_minus_b1": a1 - b1, "a2_minus_b2": a2 - b2}
        report["comparisons"][cohort] = comparison

    gate = {}
    for metric in ("bucket_publish", "bucket_publish_residual", "frame_ms"):
        values = report["comparisons"]["publish"].get(metric, {})
        gate[metric] = bool(values) and values["a1_minus_b1"] >= 0.5 and values["a2_minus_b2"] >= 0.5
    report["gate"] = {"metric": "publication P50, full minus selective, ms", "minimum_ms": 0.5,
                      "passed": all(gate.values()), "comparisons": gate}
    report["passed"] = not report["errors"] and report["gate"]["passed"]
    if args.output:
        args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n")
    for name in ARM_NAMES:
        for cohort in ("steady", "publish"):
            summary = report["arms"][name][cohort]
            if not summary["frames"]:
                continue
            print(name, cohort, "frames", summary["frames"],
                  "frame", round(summary["frame_ms"]["p50"], 3),
                  "bucket", round(summary["bucket_publish"]["p50"], 3),
                  "residual", round(summary["bucket_publish_residual"]["p50"], 3),
                  "total", round(summary["buckets_total"]["p50"], 1),
                  "refreshed", round(summary["buckets_refreshed"]["p50"], 1))
    print(json.dumps({"errors": report["errors"], "comparisons": report["comparisons"],
                      "gate": report["gate"], "passed": report["passed"]}, indent=2, ensure_ascii=False))
    if args.output:
        print(f"Wrote {args.output}")
    sys.exit(0 if report["passed"] else 1)


if __name__ == "__main__":
    main()
