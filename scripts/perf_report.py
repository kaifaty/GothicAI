#!/usr/bin/env python3
"""Summarize frame-profile CSVs and compare repeatable world performance runs."""

import argparse
import csv
import json
import math
from pathlib import Path
import statistics
import sys


METADATA_COLUMNS = {
    "attempt", "cmd_id", "dt", "segment", "frame", "begin_ns", "observer_ns",
    "end_ns", "outcome", "flags", "overflow", "gpu_ns", "gpu_done", "gpu_buffers",
    "drawable_width", "drawable_height", "width", "height",
}
TIMING = (
    "Frame intervals are differences between consecutive submitted attempt begin_ns "
    "timestamps in the same contiguous perf-PLACE/view-N or streaming-flythrough/PASS segment. Intervening "
    "FenceBusy attempts count toward elapsed time and zone costs, never as rendered "
    "frames. Partial time before the first and after the last submitted attempt "
    "of each segment is omitted. FPS is "
    "interval count / measured wall time; percentiles use linear interpolation. "
    "appcadence.csv is retained as a clock diagnostic, not used for these statistics."
)


def percentile(values, fraction):
    """Linear interpolation at (n-1)*fraction, including the endpoints."""
    ordered = sorted(values)
    if not ordered:
        return None
    position = (len(ordered) - 1) * fraction
    low = math.floor(position)
    high = math.ceil(position)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def describe(values):
    return {
        "mean": statistics.fmean(values),
        "p50": percentile(values, 0.50),
        "p95": percentile(values, 0.95),
        "p99": percentile(values, 0.99),
        "max": max(values),
    }


def read_profile(path):
    with Path(path).open(newline="") as stream:
        reader = csv.DictReader(stream)
        fields = reader.fieldnames or []
        required = {"begin_ns", "end_ns", "segment", "outcome"}
        if not required.issubset(fields):
            raise ValueError(f"{path}: missing columns: {', '.join(sorted(required - set(fields)))}")
        zones = [name[:-3] for name in fields if name.endswith("_ns")
                 and name not in METADATA_COLUMNS
                 and not name.endswith(("_first_ns", "_last_ns"))]
        zone_columns = {name + suffix for name in zones
                        for suffix in ("_ns", "_calls", "_first_ns", "_last_ns")}
        amounts = [name for name in fields if name not in METADATA_COLUMNS | zone_columns]
        rows = []
        previous = None
        for line, raw in enumerate(reader, 2):
            try:
                row = {name: (value if name == "segment" else int(value))
                       for name, value in raw.items()}
            except (TypeError, ValueError) as exc:
                raise ValueError(f"{path}:{line}: invalid numeric field") from exc
            if row["begin_ns"] < 0 or row["end_ns"] < row["begin_ns"]:
                raise ValueError(f"{path}:{line}: invalid attempt timestamps")
            if previous is not None and row["begin_ns"] < previous:
                raise ValueError(f"{path}:{line}: attempt timestamps went backwards")
            previous = row["begin_ns"]
            rows.append(row)
    return rows, zones, amounts, "gpu_ns" in fields


def segment_parts(segment):
    parts = segment.split("/")
    if len(parts) == 2 and parts[0] == "streaming-flythrough" and parts[1] in {"run", "fly"}:
        return tuple(parts)
    if len(parts) == 2 and parts[0].startswith("perf-") and parts[1] in {
            "view-1", "view-2", "view-3", "view-4"}:
        return parts[0][5:], parts[1]
    return None


def samples_from_profile(path):
    rows, zones, amounts, has_gpu = read_profile(path)
    samples = {}
    start = 0
    while start < len(rows):
        end = start + 1
        while end < len(rows) and rows[end]["segment"] == rows[start]["segment"]:
            end += 1
        parts = segment_parts(rows[start]["segment"])
        if parts:
            selected = [index for index in range(start, end) if rows[index]["outcome"] == 1]
            key = parts
            target = samples.setdefault(key, [])
            for left, right in zip(selected, selected[1:]):
                elapsed = rows[right]["begin_ns"] - rows[left]["begin_ns"]
                if elapsed <= 0:
                    raise ValueError(f"{path}: nonpositive submitted frame interval")
                attempts = rows[left:right]
                submitted = rows[left]
                gpu_valid = (has_gpu and submitted.get("gpu_done", 1) != 0
                             and submitted["gpu_ns"] > 0)
                target.append({
                    "frame_ms": elapsed / 1e6,
                    "cpu_ms": sum(row["end_ns"] - row["begin_ns"] for row in attempts) / 1e6,
                    "submitted_cpu_ms": (submitted["end_ns"] - submitted["begin_ns"]) / 1e6,
                    "attempts": len(attempts),
                    "zones": {zone: sum(row[zone + "_ns"] for row in attempts) / 1e6
                              for zone in zones},
                    "calls": {zone: sum(row.get(zone + "_calls", 0) for row in attempts)
                              for zone in zones},
                    "amounts": {name: sum(row[name] for row in attempts) for name in amounts},
                    "has_gpu": has_gpu,
                    "gpu_ms": submitted["gpu_ns"] / 1e6 if gpu_valid else None,
                    "gpu_buffers": submitted.get("gpu_buffers") if gpu_valid else None,
                })
        start = end
    warnings = []
    overflow = max((row.get("overflow", 0) for row in rows), default=0)
    if overflow:
        raise ValueError(f"{path}: profiler overflow {overflow}; recording is truncated")
    if any(row["outcome"] == 4 and segment_parts(row["segment"]) for row in rows):
        warnings.append(f"{path}: render errors occurred inside measurement segments")
    return samples, warnings


def summarize(samples):
    if not samples:
        raise ValueError("No complete submitted frame intervals in a measurement segment")
    frames = len(samples)
    intervals = [row["frame_ms"] for row in samples]
    elapsed_ms = sum(intervals)
    zones = sorted({zone for row in samples for zone in row["zones"]})
    amounts = sorted({name for row in samples for name in row["amounts"]})
    result = {
        "frames": frames,
        "attempts": sum(row["attempts"] for row in samples),
        "elapsed_s": elapsed_ms / 1000,
        "fps": frames * 1000 / elapsed_ms,
        "frame_ms": describe(intervals),
        "cpu_ms": describe([row["cpu_ms"] for row in samples]),
        "zones": [{"name": zone,
                   "mean_ms": sum(row["zones"].get(zone, 0) for row in samples) / frames,
                   "frame_pct": sum(row["zones"].get(zone, 0) for row in samples) * 100 / elapsed_ms,
                   "calls_per_frame": sum(row["calls"].get(zone, 0) for row in samples) / frames}
                  for zone in zones],
        "amounts": {name: {"total": sum(row["amounts"].get(name, 0) for row in samples),
                           "per_frame": sum(row["amounts"].get(name, 0) for row in samples) / frames}
                    for name in amounts},
        "gpu": None,
    }
    result["zones"].sort(key=lambda item: item["mean_ms"], reverse=True)
    if any(row["has_gpu"] for row in samples):
        completed = [row for row in samples if row["gpu_ms"] is not None]
        buffers = [row["gpu_buffers"] for row in completed if row["gpu_buffers"] is not None]
        result["gpu"] = {
            "completed_frames": len(completed),
            "completion_fraction": len(completed) / frames,
            "mean_ms": statistics.fmean(row["gpu_ms"] for row in completed) if completed else None,
            "p95_ms": percentile([row["gpu_ms"] for row in completed], 0.95),
            "longer_than_cpu_fraction": (sum(row["gpu_ms"] > row["submitted_cpu_ms"] for row in completed)
                                         / len(completed)) if completed else None,
            "buffers_per_completed_frame": statistics.fmean(buffers) if buffers else None,
        }
    return result


def read_metadata(directory):
    path = directory / "metadata.json"
    return json.loads(path.read_text()) if path.exists() else {}


def read_streaming(directory):
    events = [json.loads(line) for line in (directory / "events.jsonl").read_text().splitlines()]
    completed = [event for event in events if event.get("kind") == "route_end" and event.get("pass")]
    if sorted(event.get("speed_cm_s") for event in completed) != [500, 2000]:
        raise ValueError(f"{directory}: both route passes must reach their endpoints")
    path = directory / "streaming.csv"
    if not path.exists():
        if any(event.get("kind") == "perf_segment" and event.get("residency_manager") is True for event in events):
            raise ValueError(f"{path}: residency manager was enabled but samples are missing")
        return None
    with path.open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    grouped = {}
    for row in rows:
        parts = segment_parts(row["segment"])
        if parts and parts[0] == "streaming-flythrough":
            sample = {key: float(value) for key, value in row.items() if key not in {"segment", "publication_ms"}}
            publications = [float(value) for value in row.get("publication_ms", "").split(";") if value]
            if not all(math.isfinite(value) and value >= 0 for value in [*sample.values(), *publications]):
                raise ValueError(f"{path}: invalid residency metric")
            sample["publication_ms"] = publications
            grouped.setdefault(parts[1], []).append(sample)
    if set(grouped) != {"run", "fly"} or any(len(rows) < 2 for rows in grouped.values()):
        raise ValueError(f"{path}: both run and fly require complete residency samples")
    warmup = None
    for event in events:
        if event.get("kind") == "streaming_ready":
            warmup = event
            continue
        parts = segment_parts(event.get("segment", ""))
        if not parts or parts[0] != "streaming-flythrough":
            continue
        if event.get("kind") == "perf_segment" and warmup is not None and "far_publication_ms" in warmup:
            publications = warmup.get("far_publication_ms", [])
            maximum = warmup.get("far_max_per_frame", 0)
            queue_ms = warmup.get("queue_max_ms", 0)
            if not all(math.isfinite(value) and value >= 0 for value in [*publications, maximum, queue_ms]):
                raise ValueError(f"{directory}: invalid warmup publication metric")
            grouped[parts[1]][0].update(warmup_far_publication_ms=publications,
                                      warmup_far_max_per_frame=maximum, warmup_queue_max_ms=queue_ms)
            warmup = None
        elif event.get("kind") == "streaming_end":
            gpu_mb = event["gpu_mb"]
            if not math.isfinite(gpu_mb) or gpu_mb < 0:
                raise ValueError(f"{directory}: invalid GPU allocation metric")
            grouped[parts[1]][-1]["gpu_mb"] = gpu_mb
    return grouped


def streaming_stats(rows):
    publications = [value for row in rows for value in row["publication_ms"]]
    warmup = [value for row in rows for value in row.get("warmup_far_publication_ms", [])]
    return {
        "updates": len(rows),
        "near_transitions": int(sum(row["near_transitions"] for row in rows)),
        "far_transitions": int(sum(row["far_transitions"] for row in rows)),
        "peak_near": int(max(row["near"] for row in rows)),
        "update_ms": describe([row["update_ms"] for row in rows]),
        "work_ms": describe([row["work_ms"] for row in rows]),
        # Each timing is one chunk, including synchronous Near publications.
        "publication_ms": describe(publications) if publications else None,
        "warmup_far_publication_ms": describe(warmup) if warmup else None,
        "warmup_far_count": len(warmup) if any("warmup_far_publication_ms" in row for row in rows) else None,
        "warmup_far_max_per_frame": max((row["warmup_far_max_per_frame"] for row in rows
                                         if "warmup_far_max_per_frame" in row), default=None),
        "warmup_queue_max_ms": max((row["warmup_queue_max_ms"] for row in rows
                                    if "warmup_queue_max_ms" in row), default=None),
        "published": int(sum(row.get("published", 0) for row in rows)),
        "released": int(sum(row.get("released", 0) for row in rows)),
        "gpu_end_mb": next((row["gpu_mb"] for row in reversed(rows) if "gpu_mb" in row), None),
    }


def build_report(directory):
    directory = Path(directory).resolve()
    profiles = sorted(directory.rglob("frameprofile.csv"))
    if not profiles:
        raise ValueError(f"{directory}: no frameprofile.csv files")
    grouped = {}
    streaming = {}
    warnings = []
    sources = []
    for path in profiles:
        segments, issues = samples_from_profile(path)
        warnings.extend(issues)
        relative = str(path.parent.relative_to(directory))
        metadata = read_metadata(path.parent)
        if any(place == "streaming-flythrough" for place, _ in segments):
            streaming[relative] = read_streaming(path.parent)
        sources.append({"directory": relative, "metadata": metadata})
        for (place, view), samples in segments.items():
            if not samples:
                raise ValueError(f"{path}: {place}/{view} has fewer than two submitted frames")
            grouped.setdefault(place, {}).setdefault(relative, {}).setdefault(view, []).extend(samples)
    if not grouped:
        raise ValueError(f"{directory}: no performance measurement segments")
    places = {}
    for place, repeats in grouped.items():
        view_samples = {}
        repeat_reports = []
        for repeat, views in repeats.items():
            if place != "streaming-flythrough" and set(views) != {"view-1", "view-2", "view-3", "view-4"}:
                warnings.append(f"{place}/{repeat}: expected four views, found {', '.join(sorted(views))}")
            repeat_reports.append({"directory": repeat,
                                   "summary": summarize([sample for rows in views.values() for sample in rows]),
                                   "views": {view: summarize(rows) for view, rows in sorted(views.items())}})
            if place == "streaming-flythrough":
                if set(views) != {"run", "fly"}:
                    raise ValueError(f"{directory}/{repeat}: both run and fly frame segments are required")
                if streaming[repeat] is not None:
                    for view, stats in repeat_reports[-1]["views"].items():
                        stats["streaming"] = streaming_stats(streaming[repeat][view])
                    repeat_reports[-1]["summary"]["streaming"] = streaming_stats(
                        [row for rows in streaming[repeat].values() for row in rows])
            for view, rows in views.items():
                view_samples.setdefault(view, []).extend(rows)
        places[place] = {
            "summary": summarize([sample for rows in view_samples.values() for sample in rows]),
            "views": {view: summarize(rows) for view, rows in sorted(view_samples.items())},
            "repeats": repeat_reports,
        }
        measured = [repeat for repeat in repeats if streaming.get(repeat) is not None]
        if place == "streaming-flythrough" and measured:
            for view, stats in places[place]["views"].items():
                stats["streaming"] = streaming_stats(
                    [row for repeat in measured for row in streaming[repeat][view]])
            places[place]["summary"]["streaming"] = streaming_stats(
                [row for repeat in measured for rows in streaming[repeat].values() for row in rows])
    return {"schema_version": 1, "directory": str(directory), "metadata": read_metadata(directory),
            "timing": TIMING, "warnings": warnings, "sources": sources, "places": places}


def number(value, digits=3):
    return f"{value:.{digits}f}" if value is not None else "n/a"


def summary_table(report):
    lines = ["| Place / view | Frames | FPS | Mean ms | p50 ms | p95 ms | p99 ms | Max ms |",
             "|---|---:|---:|---:|---:|---:|---:|---:|"]
    for place, data in report["places"].items():
        for name, stats in [(place, data["summary"])] + [
                (place + "/" + view, stats) for view, stats in data["views"].items()]:
            frame = stats["frame_ms"]
            lines.append(f"| {name} | {stats['frames']} | {stats['fps']:.2f} | "
                         + " | ".join(number(frame[key]) for key in ("mean", "p50", "p95", "p99", "max")) + " |")
    streaming = report["places"].get("streaming-flythrough")
    if streaming and "streaming" in streaming["summary"]:
        lines.extend(["", "| Pass | Near transitions | Far transitions | Peak Near | Update p99 ms | Work p99 / max ms | Publish p99 / max ms | Published / released | GPU end MiB |",
                      "|---|---:|---:|---:|---:|---:|---:|---:|---:|"])
        for view, stats in streaming["views"].items():
            data = stats["streaming"]
            publication = data["publication_ms"] or {"p99": None, "max": None}
            lines.append(f"| {view} | {data['near_transitions']} | {data['far_transitions']} | {data['peak_near']} | "
                         f"{data['update_ms']['p99']:.6f} | {data['work_ms']['p99']:.6f} / {data['work_ms']['max']:.6f} | "
                         f"{number(publication['p99'], 6)} / {number(publication['max'], 6)} | "
                         f"{data['published']} / {data['released']} | {number(data['gpu_end_mb'], 1)} |")
        lines.extend(["", "| Warmup before pass | Far published | Publish p99 / max ms | Max Far per frame | Queue max ms |",
                      "|---|---:|---:|---:|---:|"])
        for view, stats in streaming["views"].items():
            data = stats["streaming"]
            publication = data["warmup_far_publication_ms"] or {"p99": None, "max": None}
            lines.append(f"| {view} | {number(data['warmup_far_count'], 0)} | "
                         f"{number(publication['p99'], 6)} / {number(publication['max'], 6)} | "
                         f"{number(data['warmup_far_max_per_frame'], 0)} | {number(data['warmup_queue_max_ms'], 6)} |")
    return "\n".join(lines)


def report_markdown(report):
    lines = ["# World performance", "", f"Run: `{report['directory']}`", "",
             "Metadata: `" + json.dumps(report["metadata"], ensure_ascii=False, sort_keys=True) + "`", "",
             report["timing"], "", summary_table(report), "",
             "Zone times include all attempts between submitted frames. Scopes can overlap or "
             "contain child scopes; worker elapsed sums can exceed wall time. Percentages must "
             "not be added. GPU/CPU comparisons use the matched submitted attempt's end_ns-begin_ns; "
             "they indicate overlap pressure, not exclusive bottleneck attribution.", ""]
    for warning in report["warnings"]:
        lines.append("Warning: " + warning + "\n")
    for source in report["sources"]:
        lines.extend([f"Source `{source['directory']}`: `"
                      + json.dumps(source["metadata"], ensure_ascii=False, sort_keys=True) + "`", ""])
    for place, data in report["places"].items():
        for name, stats in [(place, data["summary"])] + [
                (place + "/" + view, stats) for view, stats in data["views"].items()]:
            lines.extend([f"## {name}", "",
                          f"{stats['frames']} frame intervals, {stats['attempts']} attempts, "
                          f"{stats['elapsed_s']:.3f} s; CPU attempt work mean {stats['cpu_ms']['mean']:.3f} ms/frame.", ""])
            gpu = stats["gpu"]
            if gpu is None:
                lines.extend(["GPU timestamps unavailable.", ""])
            else:
                lines.extend([f"GPU mean/p95 {number(gpu['mean_ms'])}/{number(gpu['p95_ms'])} ms; "
                              f"completed {gpu['completed_frames']}/{stats['frames']} frames "
                              f"({100 * gpu['completion_fraction']:.1f}%); GPU longer than CPU "
                              f"{number(None if gpu['longer_than_cpu_fraction'] is None else 100 * gpu['longer_than_cpu_fraction'], 1)}%; "
                              f"buffers/completed frame {number(gpu['buffers_per_completed_frame'])}.", ""])
            lines.extend(["| Zone | Mean ms/frame | % frame | Calls/frame |",
                          "|---|---:|---:|---:|"])
            lines.extend(f"| {zone['name']} | {zone['mean_ms']:.3f} | {zone['frame_pct']:.2f} | {zone['calls_per_frame']:.2f} |"
                         for zone in stats["zones"])
            lines.extend(["", "| Amount | Total | Per frame |", "|---|---:|---:|"])
            lines.extend(f"| {amount} | {values['total']} | {values['per_frame']:.3f} |"
                         for amount, values in stats["amounts"].items())
            lines.append("")
        lines.extend([f"### {place} repeats", "", "| Repeat | FPS | p95 ms |",
                      "|---|---:|---:|"])
        lines.extend(f"| {repeat['directory']} | {repeat['summary']['fps']:.2f} | {repeat['summary']['frame_ms']['p95']:.3f} |"
                     for repeat in data["repeats"])
        lines.append("")
    return "\n".join(lines)


def zone_value(stats, name):
    return next((zone["mean_ms"] for zone in stats["zones"] if zone["name"] == name), 0)


def delta(a, b, repeats_a, repeats_b, getter):
    spreads = [max(values) - min(values) for repeats in (repeats_a, repeats_b)
               if len(repeats) > 1 for values in [[getter(repeat["summary"]) for repeat in repeats]]]
    noise = max(spreads) if spreads else None
    change = b - a
    return {"a": a, "b": b, "delta": change,
            "delta_pct": change * 100 / a if a else (0.0 if b == 0 else None),
            "noise": noise, "within_noise": abs(change) <= noise if noise is not None else None}


def compare_reports(a, b):
    places = {}
    for place in sorted(set(a["places"]) & set(b["places"])):
        left, right = a["places"][place], b["places"][place]
        def compare(getter):
            return delta(getter(left["summary"]), getter(right["summary"]),
                         left["repeats"], right["repeats"], getter)
        zones = {zone["name"] for data in (left, right) for zone in data["summary"]["zones"]}
        changes = [{"name": name, **compare(lambda stats, name=name: zone_value(stats, name))}
                   for name in zones]
        changes.sort(key=lambda item: item["delta"], reverse=True)
        places[place] = {"fps": compare(lambda stats: stats["fps"]),
                         "p95_ms": compare(lambda stats: stats["frame_ms"]["p95"]), "zones": changes}
    if not places:
        raise ValueError("Runs have no places in common")
    return {"schema_version": 1, "a": a["directory"], "b": b["directory"],
            "metadata_a": a["metadata"], "metadata_b": b["metadata"],
            "only_a": sorted(set(a["places"]) - set(b["places"])),
            "only_b": sorted(set(b["places"]) - set(a["places"])),
            "noise_method": "Within noise when absolute delta <= the larger max-min repeat spread "
                            "of A or B for that metric; unavailable without at least two repeats.",
            "places": places}


def noise_label(change):
    if change["within_noise"] is None:
        return "noise unknown (single repeat)"
    return "в пределах шума" if change["within_noise"] else "exceeds repeat spread"


def compare_markdown(report):
    lines = ["# Performance comparison", "", f"A: `{report['a']}`  \nB: `{report['b']}`", "",
             "A metadata: `" + json.dumps(report["metadata_a"], ensure_ascii=False, sort_keys=True) + "`", "",
             "B metadata: `" + json.dumps(report["metadata_b"], ensure_ascii=False, sort_keys=True) + "`", "",
             report["noise_method"], "", "| Place | FPS A → B | p95 ms A → B | FPS / p95 noise |",
             "|---|---:|---:|---|"]
    for place, data in report["places"].items():
        fps, p95 = data["fps"], data["p95_ms"]
        lines.append(f"| {place} | {fps['a']:.2f} → {fps['b']:.2f} | {p95['a']:.3f} → {p95['b']:.3f} | "
                     f"{noise_label(fps)} / {noise_label(p95)} |")
    if report["only_a"] or report["only_b"]:
        lines.extend(["", f"Only A: {', '.join(report['only_a']) or 'none'}; "
                      f"only B: {', '.join(report['only_b']) or 'none'}."])
    for place, data in report["places"].items():
        lines.extend(["", f"## {place}: zones, largest regression first", "",
                      "| Zone | A ms | B ms | Δ ms | Δ % | Repeat spread ms | Noise |",
                      "|---|---:|---:|---:|---:|---:|---|"])
        for zone in data["zones"]:
            lines.append(f"| {zone['name']} | {zone['a']:.3f} | {zone['b']:.3f} | {zone['delta']:+.3f} | "
                         f"{number(zone['delta_pct'], 2)} | {number(zone['noise'])} | {noise_label(zone)} |")
    return "\n".join(lines) + "\n"


def write_outputs(directory, stem, report, markdown):
    (directory / (stem + ".json")).write_text(json.dumps(report, indent=2, ensure_ascii=False, allow_nan=False) + "\n")
    (directory / (stem + ".md")).write_text(markdown)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    subcommands = parser.add_subparsers(dest="command", required=True)
    run_parser = subcommands.add_parser("run", help="write report.md and report.json in a run folder")
    run_parser.add_argument("directory", type=Path)
    compare_parser = subcommands.add_parser("compare", help="write compare.md and compare.json in B")
    compare_parser.add_argument("a", type=Path)
    compare_parser.add_argument("b", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == "run":
            report = build_report(args.directory)
            write_outputs(args.directory, "report", report, report_markdown(report))
            print(summary_table(report))
            for warning in report["warnings"]:
                print("Warning: " + warning, file=sys.stderr)
        else:
            report = compare_reports(build_report(args.a), build_report(args.b))
            markdown = compare_markdown(report)
            write_outputs(args.b, "compare", report, markdown)
            print(markdown.split("\n## ", 1)[0])
    except (OSError, ValueError) as exc:
        print(f"perf_report: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
