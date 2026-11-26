#!/usr/bin/env python3
"""Run or summarize the stage 3 forest stand. The run belongs in one gq quiet job."""

import argparse
import csv
from hashlib import file_digest
import json
from math import dist
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "scripts"))
import perf_gpu_log
import perf_report


PRICE = ("01-A", "02-B", "03-B", "04-A")
VERIFY = ("05-ordered", "06-single")
VIEWS = ("view-1-inside", "view-2-hero", "view-3-sun", "view-4-near", "view-5-far")
VERIFY_SEGMENTS = ("nanite-inside", "nanite-turn", "nanite-cut", "nanite-outside")
VERIFY_TAG = '{"event":"tree_nanite_verify"'
STATIC_CAMERAS = {
    0: [54140, 1760, -10780],
    2: [53990, 1760, -10780],
    3: [48765, 6679, -3156],
    4: [23216, 11160, -8618],
}
TRIM_NS = 250_000_000
WARMUP_MS = 52_000
RESULT_LIMIT_S = 520
INSIDE_GATE_MS = 1.0
Q = 0.267

SETTINGS = {
    "GOTHIC_AI": "0",
    "TEMPEST_BACKGROUND": "0",
    "TEMPEST_WINDOW_SIZE": "2560x1440",
    "OPENGOTHIC_BENCH_VISIBLE": "1",
    "OPENGOTHIC_UPSCALE": "taau",
    "OPENGOTHIC_RENDER_SCALE": "0.5",
    "OPENGOTHIC_MS": "1",
    "OPENGOTHIC_VSM": "1",
    "OPENGOTHIC_VSM_STATS": "0",
    "OPENGOTHIC_RT": "1",
    "OPENGOTHIC_GI": "1",
    "OPENGOTHIC_TREE_BOUGH_LOD": "1",
    "OPENGOTHIC_HIZ_HISTORY": "1",
    "OPENGOTHIC_TREE_WIND_FADE": "1",
    "OPENGOTHIC_TREE_WIND_CACHE": "0",
    "OPENGOTHIC_SHADOW_LEVELS": "1",
}
REQUIRED_LOG = (
    "[bench] done", "vsm = 1", "gi = 1", "tree HiZ history = 1", "tree bough LOD = 1",
    "tree shadow levels = 1", "tree wind fade = 1", "7175 source cards",
    "(taau) 1280x720 -> 2560x1440", "world check passed: NEWWORLD.ZEN",
    "applied=IMMEDIATE extent=2560x1440 result=0", "NVIDIA GeForce RTX 3080",
)
COUNTS = (
    "tree_nanite_triangles_candidate", "tree_nanite_triangles_early", "tree_nanite_triangles_late",
    "tree_nanite_card_triangles",
)


def write_json(path, result):
    path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")


def sha256(path):
    with path.open("rb") as stream:
        return file_digest(stream, "sha256").hexdigest()


def summarize_price(arm, gpu_log):
    profile = arm / "frameprofile.csv"
    segments, warnings = perf_report.samples_from_profile(profile)
    if warnings:
        raise ValueError("; ".join(warnings))
    expected = {("tree-nanite", view) for view in VIEWS}
    if set(segments) != expected:
        raise ValueError(f"{arm}: unexpected or missing timing windows: {set(segments) ^ expected}")

    result = {}
    for view in VIEWS:
        rows = segments["tree-nanite", view]
        if not rows:
            raise ValueError(f"{arm}: empty timing window {view}")
        start = rows[0]["begin_ns"] + TRIM_NS
        rows = [row for row in rows if row["begin_ns"] >= start]
        if len(rows) < 2:
            raise ValueError(f"{arm}: too few submitted frames for {view}")
        clocks = [row for row in gpu_log if rows[0]["begin_ns"] <= row["mono_ns"] <= rows[-1]["next_ns"]]
        if not any(row["sm_mhz"] is not None for row in clocks):
            raise ValueError(f"{arm}: missing SM clock samples in {view}")

        zone_names = [name for name in rows[0]["zones"]
                      if name in ("encode", "encode_main") or name.startswith("tree_nanite")]
        count_names = [name for name in rows[0]["amounts"]
                       if name.startswith(("tree_nanite", "tree_bough_triangles", "main_canopy_meshlets"))]
        active = [row["cpu_ms"] - row["zones"].get("fence_poll", 0) for row in rows]
        gpu_times = [row["gpu_ms"] for row in rows if row["gpu_ms"] is not None]
        pass_names = sorted({name for row in rows for name in row.get("gpu_passes", {})})
        view_stats = result[view] = {
            "frames": len(rows),
            "frame_ms": perf_report.describe([row["frame_ms"] for row in rows]),
            "cpu_active_ms": perf_report.describe(active),
            "gpu_ms": perf_report.describe(gpu_times) if gpu_times else None,
            "gpu_frames": len(gpu_times),
            "gpu_clock": perf_report.gpu_clock(clocks),
            "cpu_zones_ms": {name: perf_report.describe([row["zones"][name] for row in rows])
                             for name in zone_names},
            "counts": {name: perf_report.describe([row["amounts"][name] for row in rows])
                       for name in count_names},
            "gpu_passes_ms": {
                name: perf_report.describe([row["gpu_passes"][name] for row in rows
                                            if name in row.get("gpu_passes", {})])
                for name in pass_names
            },
        }
        if arm.name.endswith("-B"):
            missing = set(COUNTS[:3]) - set(count_names)
            if missing:
                raise ValueError(f"{arm}: missing triangle counts: {sorted(missing)}")
            ratios, submitted = [], []
            for row in rows:
                counts = row["amounts"]
                candidates, early, late = (counts[name] for name in COUNTS[:3])
                if early + late > candidates:
                    raise ValueError(f"{arm}: rasterized triangles exceed candidates in {view}")
                if candidates:
                    ratios.append((candidates - early - late) / candidates)
                submitted.append(early + late)
            view_stats["culled_triangle_fraction"] = perf_report.describe(ratios) if ratios else None
            view_stats["submitted_triangles"] = perf_report.describe(submitted)
            if view == VIEWS[0] and (not ratios or not any(submitted)):
                raise ValueError(f"{arm}: new path submitted no geometry inside the forest")

    return result


def verification_row(line):
    if VERIFY_TAG in line:
        return json.loads(line[line.index(VERIFY_TAG):])
    return None


def verification(arm):
    with (arm / "game.log").open(errors="replace") as log:
        pairs = [pair for line in log if line.endswith("\n") and (pair := verification_row(line))]
    columns = ("pairs", "tested_pixels", "lost_pixels", "depth_mismatches", "id_mismatches",
               "flags_mismatches", "tie_mismatches", "gained_pixels")
    result = {key: sum(row[key] for row in pairs) for key in columns}
    failed = [row for row in pairs if not row["passed"] or row["tested_pixels"] != 1280 * 720
              or any(row[key] for key in columns[2:])]
    events = read_events(arm, partial=True) if (arm / "events.jsonl").exists() else []
    per_view = dict.fromkeys(VERIFY_SEGMENTS, 0)
    first_failure = dict(min(failed, key=lambda row: row["tick"])) if failed else None
    for pair in pairs:
        segment = pair["segment"]
        per_view[segment] = per_view.get(segment, 0) + pair["pairs"]
    if first_failure:
        if first_failure["first_bad_pixel"]:
            y, x = divmod(first_failure["first_bad_pixel"] - 1, 1280)
            first_failure["pixel_xy"] = [x, y]
        mark_name = "verify-" + first_failure["segment"].removeprefix("nanite-")
        first_failure["route_mark"] = next(
            (row for row in events if row["kind"] == "mark" and row["text"] == mark_name), None)
    result["passed"] = not failed and per_view == dict.fromkeys(VERIFY_SEGMENTS, 8) and result["pairs"] == 32
    result["pairs_per_view"] = per_view
    result["reference_mode"] = VERIFY.index(arm.name) + 1
    result["first_failure"] = first_failure
    return result


def publication(arm):
    with (arm / "frameprofile.csv").open(newline="") as stream:
        frames = []
        for row in csv.DictReader(stream):
            calls = int(row.get("tree_nanite_publish_calls", 0))
            if calls:
                frames.append({"attempt": int(row["attempt"]), "segment": row["segment"], "calls": calls,
                               "cpu_ms": int(row["tree_nanite_publish_ns"]) / 1e6,
                               "bytes": int(row["tree_nanite_publish_bytes"])})
    dirty = [row for row in frames if row["bytes"]]
    return {
        "calls": sum(row["calls"] for row in frames),
        "dirty_frames": dirty,
        "cpu_ms": perf_report.describe([row["cpu_ms"] for row in frames]) if frames else None,
    }


def comparison(control, candidate):
    result = {}
    for view in VIEWS:
        a, b = control[view], candidate[view]
        frame_delta = {key: a["frame_ms"][key] - b["frame_ms"][key] for key in ("p50", "p95")}
        gpu_gain = a["gpu_ms"]["p50"] - b["gpu_ms"]["p50"] if a["gpu_ms"] and b["gpu_ms"] else None
        # The stage 1 calibration uses the archived card canopy submission counter.
        old = a["counts"].get(COUNTS[3])
        basis = COUNTS[3]
        if old is None:
            basis = "tree_bough_triangles"
            old = a["counts"].get(basis)
        front_gain = None
        if old is not None and old["p50"] > 0:
            new = b["submitted_triangles"]["p50"]
            front_gain = 0.45 * (old["p50"] - Q * new) / 1e6
        result[view] = {
            "frame_gain_ms": frame_delta,
            "gpu_gain_p50_ms": gpu_gain,
            "model_card_triangle_counter": basis if front_gain is not None else None,
            "model_raster_gain_p50_ms": front_gain,
            "h3_gpu_residual_ms": front_gain - gpu_gain if front_gain is not None and gpu_gain is not None else None,
            "h3_frame_residual_ms": front_gain - frame_delta["p50"] if front_gain is not None else None,
        }
    return result


def summarize(output):
    gpu_log = perf_gpu_log.read_log(output / "gpu.csv")
    prices = {name: summarize_price(output / name, gpu_log) for name in PRICE
              if (output / name / "frameprofile.csv").exists()}
    checks = {name: verification(output / name) for name in VERIFY if (output / name / "game.log").exists()}
    pairs = {}
    for control, candidate in ((PRICE[0], PRICE[1]), (PRICE[3], PRICE[2])):
        if control in prices and candidate in prices:
            pairs[f"{control}/{candidate}"] = comparison(prices[control], prices[candidate])
    status = "incomplete"
    regressions = []
    if any(not check["passed"] for check in checks.values()):
        status = "verification-failed"
    elif any(pair[VIEWS[0]]["frame_gain_ms"]["p50"] < INSIDE_GATE_MS for pair in pairs.values()):
        status = "price-gate-failed"
    elif len(pairs) == 2:
        for view in VIEWS[1:]:
            for key in ("p50", "p95"):
                noise = abs(prices[PRICE[0]][view]["frame_ms"][key] - prices[PRICE[3]][view]["frame_ms"][key])
                for name, pair in pairs.items():
                    regression = -pair[view]["frame_gain_ms"][key]
                    if regression > noise:
                        regressions.append({"pair": name, "view": view, "stat": key,
                                            "regression_ms": regression, "control_spread_ms": noise})
        if len(checks) == 2:
            status = "regression-gate-failed" if regressions else "numeric-gates-passed"
    return {"status": status, "prices": prices, "checks": checks, "pairs": pairs,
            "publication": {name: publication(output / name) for name in PRICE if name in prices},
            "outside_regressions": regressions}


def read_events(arm, partial=False):
    with (arm / "events.jsonl").open() as stream:
        return [json.loads(line) for line in stream if not partial or line.endswith("\n")]


def run_verification_game(command, work, env, path, timeout):
    deadline = time.monotonic() + timeout
    with path.open("w") as output, path.open(errors="replace") as reader, subprocess.Popen(
            command, cwd=work, env=env, stdout=output, stderr=subprocess.STDOUT) as game:
        try:
            while game.poll() is None:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise subprocess.TimeoutExpired(command, timeout)
                offset = reader.tell()
                line = reader.readline()
                if line.endswith("\n"):
                    pair = verification_row(line)
                    if pair and pair["passed"] is False:
                        game.terminate()
                        try:
                            return game.wait(timeout=2), True
                        except subprocess.TimeoutExpired:
                            game.kill()
                            return game.wait(), True
                    continue
                # The writer may be partway through the next record.
                reader.seek(offset)
                time.sleep(min(0.05, remaining))
            return game.returncode, False
        finally:
            if game.poll() is None:
                game.kill()


def launch(args, name, deadline):
    arm = args.output / name
    arm.mkdir()
    timed = name in PRICE
    source = ROOT / "scripts" / "bench" / ("tree-nanite-price.bench" if timed else "tree-nanite-motion.bench")
    route = arm / source.name
    text = source.read_text()
    if name == PRICE[0]:
        text = text.replace("# warmup insertion point",
                            f"wait real {WARMUP_MS}\nset time 12 3", 1)
    route.write_text(text)

    task = args.output.parent
    work = task / "game" / args.output.name / name
    work.mkdir(parents=True)
    (work / "Gothic.ini").write_text("[ENGINE]\nzMaxFps=0\n[INTERNAL]\nvidResIndex=0\n")
    cache = work / "cache"
    pipeline = cache / "derived/pipelines/vulkan-000010de00002206.bin"
    pipeline.parent.mkdir(parents=True)
    shutil.copyfile(args.inputs / "vulkan-pipeline.bin", pipeline)

    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("OPENGOTHIC_", "TEMPEST_"))}
    env.update(SETTINGS)
    env.update(
        LD_PRELOAD=str(args.inputs / "present.so"),
        OPENGOTHIC_BASE=str(args.inputs / "base"),
        OPENGOTHIC_BASE_CACHE=str(cache),
        OPENGOTHIC_BASE_STORE=str(task / "base-store"),
        OPENGOTHIC_CONTENT=str(args.inputs / "content"),
        OPENGOTHIC_BENCH=str(route),
        OPENGOTHIC_BENCH_OUT=str(arm),
        OPENGOTHIC_TREE_NANITE="0" if name.endswith("-A") else "1",
        OPENGOTHIC_TREE_NANITE_VERIFY=str(VERIFY.index(name) + 1) if not timed else "0",
    )
    clip_info = None
    if timed:
        env.update(OPENGOTHIC_FRAME_PROFILE=str(arm / "frameprofile.csv"), OPENGOTHIC_GPU_PASSES="1")
    else:
        env.update(OPENGOTHIC_BENCH_ENCODER="-c:v h264_nvenc -preset p4 -b:v 20M")
    args_for_game = [str(args.binary), "-g", str(args.inputs / "game-settings"), "-window",
                     "-devmode", "-nomenu", "-nointro", "-w", "NEWWORLD.ZEN", "-ms"]
    started = time.monotonic()
    cap = 120 if name == PRICE[0] else 80 if timed else 97
    if started >= deadline:
        raise TimeoutError("Timing job reached its budget before " + name)
    print(f"{name}: started", flush=True)
    timeout = min(cap, deadline - started)
    log_path = arm / "game.log"
    stopped_on_pixel = False
    if timed:
        with log_path.open("w") as log:
            game = subprocess.run(args_for_game, cwd=work, env=env, stdout=log,
                                  stderr=subprocess.STDOUT, timeout=timeout)
        returncode = game.returncode
    else:
        returncode, stopped_on_pixel = run_verification_game(args_for_game, work, env, log_path, timeout)
    duration = time.monotonic() - started
    log = log_path.read_text(errors="replace")
    missing = [mark for mark in REQUIRED_LOG if mark not in log and not (stopped_on_pixel and mark == "[bench] done")]
    if "Tree Nanite VSM pair capacity exceeded:" in log:
        raise ValueError(f"{arm}: the geometric shadow caster exceeded its pair capacity")
    if (returncode and not stopped_on_pixel) or missing or "[bench] error" in log:
        raise ValueError(f"{arm}: game exit={returncode}, missing={missing}\n{log[-3500:]}")
    expected_geometry = not name.endswith("-A")
    active = "Tree Nanite Vulkan path active" in log
    if active != expected_geometry:
        raise ValueError(f"{arm}: active Nanite path={active}, expected={expected_geometry}")
    events = read_events(arm, partial=stopped_on_pixel)
    if timed:
        segments = [row for row in events if row["kind"] == "perf_segment"]
        if tuple(row["segment"] for row in segments) != tuple("perf-tree-nanite/" + view for view in VIEWS):
            raise ValueError(f"{arm}: wrong timing route")
        if any(not row["residency_manager"] or row["actors"] for row in segments):
            raise ValueError(f"{arm}: wrong world residency or actor setup")
        if any(segments[index]["camera_cm"] != camera for index, camera in STATIC_CAMERAS.items()):
            raise ValueError(f"{arm}: wrong static cameras")
        positions = [row["position_cm"] for row in events if row["kind"] == "position"]
        if len(positions) != 6 or not 800 < dist(positions[1], positions[2]) < 2000:
            raise ValueError(f"{arm}: hero did not traverse the reference path: {positions}")
    else:
        result = verification(arm)
        if result["passed"]:
            clip = arm / "motion.mp4"
            info = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                                   "stream=width,height,nb_frames", "-of", "json", str(clip)],
                                  check=True, text=True, capture_output=True, timeout=6)
            video, = json.loads(info.stdout)["streams"]
            if (video["width"], video["height"]) != (2560, 1440) or int(video["nb_frames"]) < 430:
                raise ValueError(f"{arm}: incomplete motion clip: {video}")
            clip_info = {**video, "sha256": sha256(clip),
                         "marks": [row for row in events if row["kind"] == "mark" and row["seg"] == "motion"]}
    write_json(arm / "launch.json", {"seconds": duration, "returncode": returncode,
                                     "warmup_ms": WARMUP_MS if name == PRICE[0] else 0,
                                     "stopped_on_pixel": stopped_on_pixel,
                                     "clip": clip_info})
    action = "stopped on a pixel mismatch" if stopped_on_pixel else "completed"
    print(f"{name}: {action} in {duration:.2f} s", flush=True)


def run(args):
    if not os.environ.get("GQ_HOME"):
        raise ValueError("The six-arm stand must run inside a gq quiet job")
    for path in (args.binary, args.inputs, args.output):
        if not path.is_absolute():
            raise ValueError(f"Pass an absolute path: {path}")
    for path in (args.binary, *(args.inputs / name for name in (
            "base/base.json", "content/meshes/meshes.json", "game-settings/system/Gothic.ini",
            "present.so", "vulkan-pipeline.bin"))):
        if not path.is_file():
            raise ValueError(f"Missing prepared stand input: {path}")
    started = time.monotonic()
    content = args.inputs / "content"
    mesh_index = content / "meshes/meshes.json"
    world_vobs = content / "worlds/NEWWORLD/vobs.json"
    trees = json.loads(world_vobs.read_text())["vobs"]
    if sum(vob.get("visual_name") == "OWN_TREE_FIR_A_4.3DS" for vob in trees) != 1024:
        raise ValueError("The stand requires the 1,024-tree crown-fix forest")
    asset = json.loads(mesh_index.read_text())["OWN_TREE_FIR_A_4"]["treeNanite"]
    asset_hashes = {name: sha256(content / "meshes" / name)
                    for name in (asset["dag"], asset["contours"], *asset["levels"])}
    args.output.mkdir()
    write_json(args.output / "metadata.json", {
        "binary_sha256": sha256(args.binary),
        "content_manifest_sha256": sha256(mesh_index),
        "placements_sha256": sha256(world_vobs),
        "base_manifest_sha256": sha256(args.inputs / "base/base.json"),
        "asset_sha256": asset_hashes,
        "settings": SETTINGS,
        "route": [*PRICE, *VERIFY],
        "warmup_ms": WARMUP_MS,
        "trim_ms": TRIM_NS / 1e6,
    })
    failure = None
    status = "incomplete"
    with (args.output / "gpu.log").open("w") as log:
        monitor = subprocess.Popen([sys.executable, str(ROOT / "scripts/perf_gpu_log.py"),
                                    str(args.output / "gpu.csv")], stdout=log, stderr=log)
        try:
            for name in (*PRICE, *VERIFY):
                launch(args, name, started + RESULT_LIMIT_S)
                if name in (PRICE[1], PRICE[3], *VERIFY):
                    result = summarize(args.output)
                    write_json(args.output / "summary.json", result)
                    status = result["status"]
                    print(f"Gate after {name}: {status}", flush=True)
                    if status in ("verification-failed", "price-gate-failed"):
                        break
        except (KeyError, ValueError, OSError, subprocess.SubprocessError) as error:
            failure = error
        finally:
            if monitor.poll() is None:
                monitor.terminate()
                monitor.wait(timeout=6)
    write_json(args.output / "job.json", {"elapsed_seconds": time.monotonic() - started,
                                        "status": status, "error": str(failure) if failure else None})
    if failure:
        raise failure
    return 0 if status == "numeric-gates-passed" else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    job = commands.add_parser("run", help="run the budgeted game stand")
    job.add_argument("binary", type=Path)
    job.add_argument("inputs", type=Path)
    job.add_argument("output", type=Path)
    report = commands.add_parser("report", help="regenerate the summary from saved profiles and pixel checks")
    report.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "run":
            return run(args)
        write_json(args.output / "summary.json", summarize(args.output))
        return 0
    except (ValueError, OSError, subprocess.SubprocessError, TimeoutError, KeyError) as error:
        print(error, file=sys.stderr)
        return 76


if __name__ == "__main__":
    sys.exit(main())
