"""Small CSV fixtures for frame cadence, dynamic zones and repeat comparisons."""
import csv
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

import pytest


SPEC = importlib.util.spec_from_file_location("perf_report", Path(__file__).parents[1] / "perf_report.py")
report = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(report)
sys.path.insert(0, str(Path(__file__).parents[1]))
import perf_content as content


@pytest.mark.parametrize("cache", (None, "/custom/base-cache"))
def test_remote_perf_uses_setup_cache_unless_caller_overrides(monkeypatch, cache):
    source = (Path(__file__).parents[1] / "perf.sh").read_text()
    collect = "GAME_ENV=()" + source.split("    GAME_ENV=()", 1)[1].split("    remote env", 1)[0]
    remote = "REMOTE_ENV=(" + source.split("    REMOTE_ENV=(", 1)[1].split("\n    if [[", 1)[0]
    monkeypatch.delenv("OPENGOTHIC_BASE_CACHE", raising=False)
    if cache is not None:
        monkeypatch.setenv("OPENGOTHIC_BASE_CACHE", cache)
    harness = r'''
set -euo pipefail
for name in PERF_VID_RES PERF_INI PERF_WINDOW PERF_REPEAT PERF_WAIT PERF_ARGS PERF_ENV PERF_COMMIT PERF_DIRTY; do
  printf -v "$name" '%s' fixture
done
TASK=/remote/task
''' + collect + remote + '\nprintf "%s\\0" "${REMOTE_ENV[@]}"\n'
    result = subprocess.run(["bash", "-c", harness], check=True, capture_output=True)
    environment = dict(value.decode().split("=", 1) for value in result.stdout.split(b"\0") if value)
    assert environment.get("OPENGOTHIC_BASE_CACHE") == cache


def profile(directory, rows, gpu=False, extra_zone=False, geometry=False):
    directory.mkdir(parents=True, exist_ok=True)
    fields = ["attempt", "segment", "begin_ns", "end_ns", "observer_ns", "outcome", "overflow",
              "encode_ns", "encode_calls", "encode_first_ns", "encode_last_ns", "palette_bytes"]
    if gpu:
        fields += ["gpu_ns", "gpu_done", "gpu_buffers"]
    if extra_zone:
        fields += ["p30_ns", "p30_calls", "p30_first_ns", "p30_last_ns"]
    if geometry:
        fields += ["triangles", "draw_calls", "geometry_done"]
    with (directory / "frameprofile.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fields)
        writer.writeheader()
        for index, row in enumerate(rows):
            values = dict.fromkeys(fields, 0)
            values.update(attempt=index, segment="perf-market/view-1", outcome=1,
                          begin_ns=index * 10_000_000, encode_ns=1_000_000,
                          encode_calls=1, palette_bytes=1024)
            if geometry:
                values.update(triangles=12000, draw_calls=40, geometry_done=1)
            values.update(row)
            values["end_ns"] = values["begin_ns"] + 2_000_000
            writer.writerow(values)
    (directory / "metadata.json").write_text(json.dumps({"git_commit": "abc123", "drawable": [1728, 1117],
                                                       "drawable_width": 1728, "drawable_height": 1117}))
    return directory


def cadence_rows(step_ms, zone_ms=1):
    return [{"begin_ns": index * step_ms * 1_000_000, "encode_ns": int(zone_ms * 1_000_000)}
            for index in range(5)]


def test_perf_content_arguments_and_abba(monkeypatch, tmp_path):
    args = content.parse_args(["none", "working-8"])
    assert args.places == ["harbour", "market"]
    assert (args.noise_pct, args.threshold_pct) == (2, 5)
    args = content.parse_args(["none", "working-8", "--noise-pct", "1", "--threshold-pct", "3"])
    assert (args.noise_pct, args.threshold_pct) == (1, 3)
    for invalid in (["../working-8", "none"], ["none", "working-8", "unknown"]):
        with pytest.raises(SystemExit):
            content.parse_args(invalid)
    calls = []
    monkeypatch.setattr(content.subprocess, "run", lambda command, **kwargs: calls.append((command, kwargs["env"])))
    reports = []
    monkeypatch.setattr(content, "write_report", lambda *values: reports.append(values))
    monkeypatch.setenv("PERF_ENV", "OPENGOTHIC_CONTENT=/wrong OPENGOTHIC_FOOTIK=0")
    content.run_attempts(tmp_path, tmp_path / "content", args)
    assert [command[-1] for command, _ in calls] == ["harbour"] * 4 + ["market"] * 4
    assert [env["OPENGOTHIC_CONTENT"] for _, env in calls] == ["", str(tmp_path / "content/working-8"),
                                                               str(tmp_path / "content/working-8"), ""] * 2
    assert all(env["PERF_REPEAT"] == "1" and env["NO_BUILD"] == "1" and "/wrong" not in env["PERF_ENV"]
               for _, env in calls)
    assert reports == [(tmp_path, 1, 3)]


@pytest.mark.parametrize("b_ms,verdict", [(10.1, "в шуме"), (10.5, "в пороге"),
                                         (10.6, "сверх порога"), (9, "в пороге")])
def test_perf_content_recorded_delta_threshold_and_counters(tmp_path, b_ms, verdict):
    (tmp_path / "metadata.json").write_text(json.dumps({"places": ["market"]}))
    for side, steps in (("A", (9, 11)), ("B", (b_ms, b_ms))):
        for repeat, step in enumerate(steps, 1):
            rows = [{"segment": f"perf-market/view-{view}",
                     "begin_ns": int((view * 1000 + frame * step) * 1e6)}
                    for view in range(1, 5) for frame in range(5)]
            # Incomplete geometry must not become a zero in the mean.
            rows[-1].update(geometry_done=0, triangles=0, draw_calls=0)
            profile(tmp_path / side / f"attempt-{repeat}", rows, geometry=True)
    data = content.comparison(tmp_path)["places"]["market"]
    assert data["p50"]["a"] == 10
    assert data["p50"]["b"] == pytest.approx(b_ms)
    assert data["p50"]["delta_pct"] == pytest.approx((b_ms / 10 - 1) * 100)
    # Pooled A p95 is 11 ms, so these B values all satisfy the p95 gate.
    assert data["within_threshold"] is True
    assert "within_noise" not in data["p50"]
    assert data["p50"]["spread_a_ms"] == 2
    assert data["counters"]["A"] == {"triangles": 12000, "draw_calls": 40,
                                       "completed_frames": 38, "submitted_frames": 40}
    # Constant recorded A isolates the exact +5% p95 boundary.
    for path in (tmp_path / "A").rglob("frameprofile.csv"):
        rows = [{"segment": f"perf-market/view-{view}", "begin_ns": (view * 1000 + frame * 10) * 1_000_000}
                for view in range(1, 5) for frame in range(5)]
        profile(path.parent, rows, geometry=True)
    result = content.comparison(tmp_path)
    assert result["noise_p95_pct"] == 2
    assert result["threshold_p95_pct"] == 5
    assert result["places"]["market"]["p95_verdict"] == verdict
    assert verdict in content.table(result)
    custom = content.comparison(tmp_path, noise_pct=6, threshold_pct=10)
    assert custom["places"]["market"]["p95_verdict"] == ("в пороге" if b_ms == 9 else "в шуме")
    assert (custom["noise_p95_pct"], custom["threshold_p95_pct"]) == (6, 10)
    assert [content.p95_verdict(value, 2, 5) for value in (-2, 2, -2.01, 2.01, 5, 5.01)] == [
        "в шуме", "в шуме", "в пороге", "в пороге", "в пороге", "сверх порога"]


def test_percentiles_dynamic_columns_segments_and_no_gpu(tmp_path):
    directory = profile(tmp_path / "market" / "repeat-1", [
        {"segment": "warmup", "begin_ns": 0},
        {"begin_ns": 100_000_000, "p30_ns": 3_000_000},
        {"begin_ns": 110_000_000, "p30_ns": 3_000_000},
        {"begin_ns": 130_000_000, "p30_ns": 3_000_000},
        {"begin_ns": 160_000_000},
        {"segment": "perf-market/view-2", "begin_ns": 200_000_000},
        {"segment": "perf-market/view-2", "begin_ns": 240_000_000},
        {"segment": "warmup", "begin_ns": 900_000_000},
    ], extra_zone=True)
    result = report.build_report(tmp_path)
    market = result["places"]["market"]
    first = market["views"]["view-1"]
    assert first["frames"] == 3
    assert first["fps"] == pytest.approx(50)
    assert first["frame_ms"] == {"mean": 20, "p50": 20, "p95": 29, "p99": pytest.approx(29.8), "max": 30}
    assert market["views"]["view-2"]["frames"] == 1
    assert first["gpu"] is None
    assert [zone["name"] for zone in first["zones"]] == ["p30", "encode"]
    assert first["zones"][0]["frame_pct"] == 15
    assert first["amounts"] == {"palette_bytes": {"total": 3072, "per_frame": 1024}}
    assert result["sources"][0]["metadata"]["drawable"] == [1728, 1117]
    assert report.percentile([1], .99) == 1
    assert report.percentile([], .95) is None
    assert report.main(["run", str(directory)]) == 0
    assert (directory / "report.md").exists()
    assert json.loads((directory / "report.json").read_text())["places"]["market"]["summary"]["frames"] == 4


    streaming = profile(tmp_path / "streaming", [
        {"segment": f"streaming-flythrough/{name}", "begin_ns": (offset + frame * step) * 1_000_000}
        for name, offset, step in (("run", 0, 10), ("fly", 100, 20)) for frame in range(3)])
    with (streaming / "streaming.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, ["segment", "frame", "update_ms", "work_ms", "near", "far", "outside",
                                         "near_transitions", "far_transitions", "publication_ms", "published", "released"])
        writer.writeheader()
        writer.writerows({"segment": f"streaming-flythrough/{name}", "frame": frame,
                          "update_ms": .001 * (frame + 1), "work_ms": .0001,
                          "near": 20 + frame, "far": 300, "outside": 3,
                          "near_transitions": frame, "far_transitions": frame + 1,
                          "publication_ms": ("", ".25;.5", ".75")[frame] if name == "run" else "",
                          "published": (0, 2, 1)[frame] if name == "run" else 0, "released": int(frame == 2)}
                         for name in ("run", "fly") for frame in range(3))
    (streaming / "events.jsonl").write_text("\n".join(
        json.dumps(event) for event in [
            {"kind": "streaming_ready", "far_publication_ms": [9], "far_max_per_frame": 1, "queue_max_ms": 10},
            {"kind": "streaming_ready", "far_publication_ms": [.3, .6], "far_max_per_frame": 1, "queue_max_ms": .8},
            {"kind": "perf_segment", "segment": "streaming-flythrough/run"},
            {"kind": "route_end", "speed_cm_s": 500, "pass": True},
            {"kind": "streaming_end", "segment": "streaming-flythrough/run", "gpu_mb": 320.5},
            {"kind": "streaming_ready", "far_publication_ms": [], "far_max_per_frame": 0, "queue_max_ms": .001},
            {"kind": "perf_segment", "segment": "streaming-flythrough/fly"},
            {"kind": "route_end", "speed_cm_s": 2000, "pass": True},
            {"kind": "streaming_end", "segment": "streaming-flythrough/fly", "gpu_mb": 330.5}]))
    data = report.build_report(streaming)["places"]["streaming-flythrough"]["views"]
    assert data["run"]["frame_ms"]["p50"] == 10
    assert data["fly"]["frame_ms"]["p99"] == data["fly"]["frame_ms"]["max"] == 20
    assert data["run"]["streaming"]["near_transitions"] == 3
    assert data["run"]["streaming"]["far_transitions"] == 6
    assert data["run"]["streaming"]["peak_near"] == 22
    assert data["run"]["streaming"]["update_ms"]["p99"] == pytest.approx(.00298)
    assert data["run"]["streaming"]["work_ms"]["max"] == .0001
    assert data["run"]["streaming"]["publication_ms"]["p99"] == pytest.approx(.745)
    assert data["run"]["streaming"]["publication_ms"]["max"] == .75
    assert data["run"]["streaming"]["published"] == 3
    assert data["run"]["streaming"]["released"] == 1
    assert data["fly"]["streaming"]["gpu_end_mb"] == 330.5
    assert data["fly"]["streaming"]["publication_ms"] is None
    assert data["fly"]["streaming"]["published"] == 0
    warmup = data["run"]["streaming"]
    assert warmup["warmup_far_count"] == 2
    assert warmup["warmup_far_publication_ms"]["p99"] == pytest.approx(.597)
    assert warmup["warmup_far_publication_ms"]["max"] == .6
    assert warmup["warmup_far_max_per_frame"] == 1
    assert warmup["warmup_queue_max_ms"] == .8
    assert data["fly"]["streaming"]["warmup_far_count"] == 0
    assert data["fly"]["streaming"]["warmup_far_publication_ms"] is None
    assert report.main(["run", str(streaming)]) == 0


def test_fence_attempts_cost_time_but_do_not_inflate_rendered_fps(tmp_path):
    profile(tmp_path, [
        {"begin_ns": 0},
        {"begin_ns": 10_000_000, "outcome": 2, "encode_ns": 0, "encode_calls": 0},
        {"begin_ns": 30_000_000},
        {"begin_ns": 50_000_000},
    ])
    stats = report.build_report(tmp_path)["places"]["market"]["summary"]
    assert stats["frames"] == 2
    assert stats["attempts"] == 3
    assert stats["fps"] == 40
    assert stats["frame_ms"]["p95"] == pytest.approx(29.5)
    assert stats["cpu_ms"]["mean"] == 3
    assert stats["zones"][0]["mean_ms"] == 1
    assert stats["amounts"]["palette_bytes"]["per_frame"] == 1536


def test_gpu_completion_and_matching_cpu_duration(tmp_path):
    profile(tmp_path, [
        {"gpu_ns": 4_000_000, "gpu_done": 1, "gpu_buffers": 2},
        {"gpu_ns": 50_000_000, "gpu_done": 0, "gpu_buffers": 1},
        {"gpu_ns": 1_000_000, "gpu_done": 1, "gpu_buffers": 1},
        {"gpu_ns": 100_000_000, "gpu_done": 1, "gpu_buffers": 1},
    ], gpu=True)
    gpu = report.build_report(tmp_path)["places"]["market"]["summary"]["gpu"]
    assert gpu["completed_frames"] == 2
    assert gpu["completion_fraction"] == pytest.approx(2 / 3)
    assert gpu["mean_ms"] == 2.5
    assert gpu["p95_ms"] == pytest.approx(3.85)
    assert gpu["longer_than_cpu_fraction"] == .5
    assert gpu["buffers_per_completed_frame"] == 1.5


def test_compare_repeat_noise_sorted_regressions_and_leaf_inputs(tmp_path):
    a, b = tmp_path / "a", tmp_path / "b"
    for root, values in [(a, [(10, 2), (12, 4)]), (b, [(14, 3.5), (16, 4.5)])]:
        for repeat, (step, cost) in enumerate(values, 1):
            profile(root / "market" / f"repeat-{repeat}", [
                {**row, "p30_ns": 1_000_000 if root == a else 6_000_000}
                for row in cadence_rows(step, cost)], extra_zone=True)
    comparison = report.compare_reports(report.build_report(a), report.build_report(b))
    market = comparison["places"]["market"]
    assert market["fps"]["delta"] < 0
    assert market["p95_ms"]["delta"] == 4
    assert market["p95_ms"]["within_noise"] is False
    assert [zone["name"] for zone in market["zones"]] == ["p30", "encode"]
    assert market["zones"][0]["delta"] == 5
    assert market["zones"][0]["delta_pct"] == 500
    assert market["zones"][1]["delta"] == 1
    assert market["zones"][1]["within_noise"] is True
    assert "в пределах шума" in report.compare_markdown(comparison)
    left, right = a / "market/repeat-1", a / "market/repeat-2"
    assert report.main(["compare", str(left), str(right)]) == 0
    leaf = json.loads((right / "compare.json").read_text())
    assert leaf["places"]["market"]["zones"][0]["within_noise"] is None
    assert (right / "compare.md").exists()
