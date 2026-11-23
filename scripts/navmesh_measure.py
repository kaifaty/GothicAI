#!/usr/bin/env python3
"""Repeat the Recast coverage and paired native-NPC experiment with body-runner.

Run `run` on Ryzen under ~/gothic-ai/body-simulation.lock. `summarize` is light
and uses only the standard library, including the two top-down PNG renderings.
All runner output paths are absolute; each trial resets the saved world and seed.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import json
import math
import os
from pathlib import Path
import re
import select
import shutil
import signal
import statistics
import struct
import subprocess
import sys
import time
import zlib


REGIONS = ("market", "harbour", "onar", "lobart", "port_streets", "upper_streets")
NPC = "BAU_954_MALETH"
REPO = Path(__file__).resolve().parent.parent


def rows(path):
    with Path(path).open(encoding="utf-8") as stream:
        for number, line in enumerate(stream, 1):
            if line.strip():
                try:
                    yield json.loads(line)
                except ValueError as exc:
                    raise RuntimeError(f"Invalid JSONL at {path}:{number}: {exc}") from exc


def write_json(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2)+"\n", encoding="utf-8")


def write_rows(path, values):
    with Path(path).open("w", encoding="utf-8") as stream:
        for value in values:
            stream.write(json.dumps(value, ensure_ascii=False, separators=(",", ":"))+"\n")


class Runner:
    def __init__(self, args, initial):
        self.navmesh_command = next((line for line in initial.read_text(encoding="utf-8").splitlines()
                                     if line.startswith("navmesh build ")), None)
        workdir = args.output / "run"
        workdir.mkdir(parents=True, exist_ok=True)
        if args.ini:
            shutil.copyfile(args.ini, workdir / "Gothic.ini")
        # Compare static authored collision meshes in both takes. With movable
        # vobs enabled the legacy controller queries kinematic copies of props.
        ini = workdir / "Gothic.ini"
        settings = ini.read_text(encoding="utf-8") if ini.exists() else ""
        settings = re.sub(r"(?im)^\s*movable_vobs\s*=.*$", "", settings)
        if re.search(r"(?im)^\[GAME\]\s*$", settings):
            settings = re.sub(r"(?im)^(\[GAME\])\s*$", r"\1\nmovable_vobs=0", settings, count=1)
        else:
            settings += "\n[GAME]\nmovable_vobs=0\n"
        ini.write_text(settings, encoding="utf-8")
        self.stderr = (args.output / "runner-stderr.log").open("ab")
        env = dict(os.environ, GOTHIC_AI="0", OPENGOTHIC_BODY_ANIMATION_RNG="isolated",
                   OPENGOTHIC_PHYSICS_THREADS="0",
                   OPENGOTHIC_STUCK_LOG=str(args.output / "stuck.jsonl"),
                   VK_DRIVER_FILES="/nonexistent", VK_ICD_FILENAMES="/nonexistent")
        env.pop("DISPLAY", None)
        env.pop("WAYLAND_DISPLAY", None)
        self.process = subprocess.Popen(
            [str(args.runner), "-g", str(args.assets), "-devmode",
             "--save", str(args.save), "--scenario", str(initial),
             "--output", str(args.output / "initial"), "--mode", "lockstep", "--stdio",
             "--fork-reset", "--serial-workers"],
            cwd=workdir, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.stderr,
            start_new_session=True)
        self.sequence = 0
        self.buffer = b""

    def request(self, command, timeout=180):
        self.sequence += 1
        request = dict(command, id=self.sequence)
        self.process.stdin.write((json.dumps(request)+"\n").encode())
        self.process.stdin.flush()
        deadline = time.monotonic()+timeout
        while b"\n" not in self.buffer:
            remaining = deadline-time.monotonic()
            if remaining <= 0:
                raise RuntimeError(f"Runner wall watchdog expired on {command['cmd']}")
            ready, _, _ = select.select([self.process.stdout], [], [], min(5, remaining))
            if not ready:
                if self.process.poll() is not None:
                    raise RuntimeError(f"Runner exited: {self.process.returncode}")
                continue
            chunk = os.read(self.process.stdout.fileno(), 65536)
            if not chunk:
                raise RuntimeError(f"Runner closed its response pipe: {self.process.poll()}")
            self.buffer += chunk
            if len(self.buffer) > 16*1024*1024:
                raise RuntimeError("Runner response exceeds 16 MiB")
        line, self.buffer = self.buffer.split(b"\n", 1)
        response = json.loads(line)
        if response.get("id") != request["id"] or response.get("schema_version") != 1:
            raise RuntimeError(f"Runner response ID/schema mismatch: {response}")
        if response.get("ok") is False:
            raise RuntimeError(f"Runner rejected {command['cmd']}: {response.get('error')}")
        return response

    def scenario(self, scenario, output, seed, timeout=180):
        output.mkdir(parents=True, exist_ok=True)
        # A fork inherits the pristine world, not the previous episode's navmesh.
        # Repeat the initial cache load before every scenario that does not build its own.
        commands = scenario.read_text(encoding="utf-8")
        if self.navmesh_command and not any(line.startswith("navmesh build ") for line in commands.splitlines()):
            scenario = output / "scenario.bench"
            scenario.write_text(self.navmesh_command + "\n" + commands, encoding="utf-8")
        response = self.request({"cmd": "reset", "scenario": str(scenario),
                                 "output_dir": str(output), "mode": "lockstep", "seed": seed}, timeout)
        for _ in range(240):
            if response.get("done"):
                break
            response = self.request({"cmd": "advance", "substeps": 600}, timeout)
        else:
            raise RuntimeError(f"Scenario exceeded 240 advance requests: {scenario}")
        log = output / "bench.log"
        if log.exists():
            text = log.read_text(encoding="utf-8", errors="replace")
            bad = re.search(r"unknown command|(?:navmesh|stuck|goto) .*failed|npcgo: no|freeze: no|navmesh:.*(?:error|expected)", text)
            if bad:
                raise RuntimeError(f"Bench preparation failed in {output}: {bad.group(0)}")
        return response

    def close(self):
        try:
            if self.process.poll() is None:
                self.request({"cmd": "close"}, 30)
                self.process.wait(timeout=30)
                if self.process.returncode:
                    raise RuntimeError(f"Runner failed on close: {self.process.returncode}")
        finally:
            try:
                # The template can exit before a busy fork; always signal the whole group.
                for sig in (signal.SIGTERM, signal.SIGKILL):
                    try:
                        os.killpg(self.process.pid, sig)
                    except ProcessLookupError:
                        pass
                    try:
                        self.process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        if sig == signal.SIGKILL:
                            raise
            finally:
                self.stderr.close()


def scenario_file(args, name, commands):
    path = args.output / "scenarios" / f"{name}.bench"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join([f"seed {args.seed}", "cheat god", "set time 10 0", *commands, "label done", ""]), encoding="utf-8")
    return path


def pair_fields(pair):
    return (pair.get("region", pair.get("place")),
            pair.get("start", pair.get("start_wp", pair.get("from"))),
            pair.get("goal", pair.get("target_fp", pair.get("to"))))


def run(args):
    if sys.platform != "linux":
        raise RuntimeError("Heavy runner measurements must run on Ryzen/Linux")
    args.output.mkdir(parents=True, exist_ok=True)
    args.cache.mkdir(parents=True, exist_ok=True)
    prepare = scenario_file(args, "prepare", [f"navmesh build {args.cache}",
                             "navmesh coverage", f"navmesh pairs {args.seed} {args.per_region}"])
    runner = Runner(args, prepare)
    try:
        if not args.measure_only:
            # Populate the file cache; each episode's fork loads it independently.
            runner.scenario(prepare, args.output / "prepare", args.seed, timeout=720)
            required = ("navmesh-stats.json", "navmesh-polygons.jsonl", "coverage.jsonl", "points.jsonl", "pairs.jsonl")
            for name in required:
                if not (args.output / "prepare" / name).is_file():
                    raise RuntimeError(f"Preparation did not write {name}")
            # A separate reset explicitly measures file-cache loading, including after process reuse.
            cached = scenario_file(args, "cache-load", [f"navmesh build {args.cache}"])
            runner.scenario(cached, args.output / "cache-load", args.seed, timeout=720)
        else:
            # Check that the file cache loads before starting the measurements.
            cached = scenario_file(args, "cache-load", [f"navmesh build {args.cache}"])
            runner.scenario(cached, args.output / "cache-load", args.seed, timeout=720)
        if args.prepare_only:
            return
        pairs = list(rows(args.output / "prepare" / "pairs.jsonl"))
        region_counts = Counter(pair_fields(pair)[0] for pair in pairs)
        if not args.smoke:
            if len(pairs) < 200 or any(region_counts[r] < args.per_region for r in REGIONS):
                raise RuntimeError(f"Expected >=200 pairs across six regions: {dict(region_counts)}")
        if args.smoke:
            pairs = pairs[:args.smoke]
        if args.control_only:
            pairs = []
        aggregate = args.output / "trials.jsonl"
        if aggregate.exists() and not args.resume:
            raise RuntimeError(f"Existing trials: {aggregate}; use --resume or a fresh output")
        completed = {r["sample"] for r in rows(aggregate)} if aggregate.exists() else set()
        for index, pair in enumerate(pairs):
            region, start, goal = pair_fields(pair)
            if not all(isinstance(value, str) and value for value in (region, start, goal)):
                raise RuntimeError(f"Invalid pair fields: {pair}")
            for mode in ("direct", "mesh"):
                sample = f"pair-{index:03d}-{mode}"
                if sample in completed:
                    continue
                scenario = scenario_file(args, sample, [f"goto {start}",
                    f"stuck begin {sample} 1000000",
                    f"navmesh start {mode} {args.npc} {start} {goal} {args.limit_ms}",
                    "navmesh finish", "stuck end"])
                output = args.output / "trials" / sample
                runner.scenario(scenario, output, args.seed)
                trial = json.loads((output / "trial.json").read_text())
                trial.update(sample=sample, pair=index, region=region, mode=mode,
                             start=start, goal=goal, npc=args.npc, seed=args.seed, limit_ms=args.limit_ms)
                with aggregate.open("a", encoding="utf-8") as stream:
                    stream.write(json.dumps(trial, ensure_ascii=False, separators=(",", ":"))+"\n")
                print(f"{sample}: success={trial.get('success')} elapsed_ms={trial.get('elapsed_ms')}", flush=True)
        if not args.no_control:
            for mode in ("direct", "mesh"):
                sample = f"intentional-{mode}"
                if sample in completed:
                    continue
                setup = ["goto NW_FARM1_ENTRANCE_03", f"put {NPC} 0 0 -1400",
                         "goto NW_FARM1_ENTRANCE_01", "wait 1000"]
                scenario = scenario_file(args, sample, [*setup,
                    f"stuck begin {sample} 1000000",
                    f"navmesh follow {mode} {NPC} FP_ROAM_FARM1_SHEEP_09 {args.limit_ms}",
                    "navmesh finish", "stuck end"])
                output = args.output / "trials" / sample
                runner.scenario(scenario, output, args.seed)
                trial = json.loads((output / "trial.json").read_text())
                trial.update(sample=sample, region="intentional", mode=mode, npc=NPC,
                             seed=args.seed, limit_ms=args.limit_ms)
                with aggregate.open("a", encoding="utf-8") as stream:
                    stream.write(json.dumps(trial, ensure_ascii=False, separators=(",", ":"))+"\n")
                print(f"{sample}: success={trial.get('success')} elapsed_ms={trial.get('elapsed_ms')}", flush=True)
    finally:
        if sys.exc_info()[0] is None:
            runner.close()
        else:
            try:
                runner.close()
            except Exception as exc:
                print(f"Runner cleanup after failure: {exc}", file=sys.stderr)


def edge_fields(row):
    return (row.get("from", row.get("a", row.get("start"))),
            row.get("to", row.get("b", row.get("goal"))),
            row.get("complete", row.get("path_complete", row.get("reachable", False))),
            row.get("edge_length_cm", row.get("direct_length_cm", row.get("distance_cm", 0))),
            row.get("path_length_cm", row.get("length_cm", 0)))


def position(value):
    if isinstance(value, dict):
        value = value.get("position", value.get("pos", value.get("vertices", value)))
    if isinstance(value, dict):
        return (value["x"], value["y"], value["z"])
    return value


def summarize(args):
    report = args.report or args.output / "summary"
    report.mkdir(parents=True, exist_ok=True)
    places = json.loads(args.places.read_text(encoding="utf-8"))

    def place(wp):
        index = places.get("points", {}).get(wp)
        return places["places"][index]["ru"] if isinstance(index, int) else "не сопоставлено"

    coverage = list(rows(args.output / "prepare" / "coverage.jsonl"))
    points = {row.get("name", row.get("waypoint")): position(row) for row in rows(args.output / "prepare" / "points.jsonl")}
    pairs = list(rows(args.output / "prepare" / "pairs.jsonl"))
    trials = list(rows(args.output / "trials.jsonl")) if (args.output / "trials.jsonl").exists() else []
    stuck = list(rows(args.output / "stuck.jsonl")) if (args.output / "stuck.jsonl").exists() else []
    stuck_counts = Counter((row.get("sample"), row.get("npc")) for row in stuck if row.get("type") == "npc_stuck")
    for row in trials:
        row["stuck_events"] = stuck_counts[row["sample"], row.get("npc", NPC)]
    complete = sum(bool(edge_fields(row)[2]) for row in coverage)
    long = sum(bool(ok and straight > 0 and length > 1.5*straight)
               for _, _, ok, straight, length in map(edge_fields, coverage))
    missing = sorted((row for row in coverage if not edge_fields(row)[2]),
                     key=lambda row: edge_fields(row)[3], reverse=True)
    top_missing = [{"from": edge_fields(row)[0], "to": edge_fields(row)[1],
                    "from_place": place(edge_fields(row)[0]), "to_place": place(edge_fields(row)[1]),
                    "edge_length_cm": edge_fields(row)[3], "reason": row.get("reason")} for row in missing[:20]]
    groups = defaultdict(list)
    for trial in trials:
        if trial.get("region") != "intentional":
            groups[trial["region"], trial["mode"]].append(trial)
            groups["all", trial["mode"]].append(trial)
    measurements = []
    for (region, mode), group in sorted(groups.items()):
        arrived = [row for row in group if row.get("success")]
        times = [row["elapsed_ms"] for row in group if isinstance(row.get("elapsed_ms"), (int, float))]
        arrival_times = [row["elapsed_ms"] for row in arrived if isinstance(row.get("elapsed_ms"), (int, float))]
        events = sum(row["stuck_events"] for row in group)
        measurements.append(dict(region=region, mode=mode, n=len(group), arrived=len(arrived),
            arrival_pct=100*len(arrived)/len(group), stuck_events=events,
            no_path=sum(not row.get("path_complete") for row in group) if mode == "mesh" else 0,
            stuck_per_100=100*events/len(group), median_elapsed_ms=statistics.median(times) if times else None,
            median_arrival_ms=statistics.median(arrival_times) if arrival_times else None))
    direct_places = defaultdict(lambda: {"walks": set(), "failures": set(), "stuck_events": 0})
    direct_trials = {trial["sample"]: trial for trial in trials
                     if trial["mode"] == "direct" and trial.get("region") != "intentional"}
    paired = defaultdict(dict)
    for trial in trials:
        if "pair" in trial:
            paired[trial["pair"]][trial["mode"]] = trial
    for episode in stuck:
        trial = direct_trials.get(episode.get("sample"))
        if episode.get("type") != "npc_stuck" or trial is None or episode.get("npc") != trial.get("npc", NPC):
            continue
        row = direct_places[place(episode.get("nearest_waypoint"))]
        row["walks"].add(trial["sample"])
        if not trial.get("success"):
            row["failures"].add(trial["sample"])
        row["stuck_events"] += 1
    top_stuck = [{"place": name, "walks": len(value["walks"]), "failures": len(value["failures"]),
                  "stuck_events": value["stuck_events"]} for name, value in direct_places.items()]
    top_stuck.sort(key=lambda row: (row["stuck_events"], row["failures"]), reverse=True)
    matched = [value for value in paired.values() if "direct" in value and "mesh" in value]
    common = [pair for pair in matched if pair["direct"].get("success") and pair["mesh"].get("success")]
    outcome = Counter((bool(pair["direct"].get("success")), bool(pair["mesh"].get("success"))) for pair in matched)
    stats = json.loads((args.output / "prepare" / "navmesh-stats.json").read_text())
    load_path = args.output / "cache-load" / "navmesh-stats.json"
    summary = dict(navmesh=stats, cache_load=json.loads(load_path.read_text()) if load_path.exists() else None,
        coverage=dict(edges=len(coverage), reachable=complete, reachable_pct=100*complete/len(coverage) if coverage else 0,
            longer_than_1_5=long, longer_than_1_5_pct=100*long/len(coverage) if coverage else 0,
            longer_than_1_5_reachable_pct=100*long/complete if complete else 0, top20_missing=top_missing),
        measurements=measurements, paired=dict(n=len(matched), both_arrive=outcome[True, True],
            only_mesh_arrives=outcome[False, True], only_direct_arrives=outcome[True, False], neither_arrives=outcome[False, False],
            common_arrival_direct_ms=statistics.median(pair["direct"]["elapsed_ms"] for pair in common) if common else None,
            common_arrival_mesh_ms=statistics.median(pair["mesh"]["elapsed_ms"] for pair in common) if common else None,
            common_arrival_delta_ms=statistics.median(pair["mesh"]["elapsed_ms"]-pair["direct"]["elapsed_ms"] for pair in common) if common else None),
        control=[row for row in trials if row.get("region") == "intentional"], top10_direct_stuck_places=top_stuck[:10],
        pursuit={"n": 0, "reason": "The existing come/approach commands only support NPC-to-hero or hero-to-NPC. There is no NPC-to-routine-NPC chase command or moving-target navmesh replanning fixture."})
    raw_dir = report / "raw"
    raw_dir.mkdir(exist_ok=True)
    omitted = []
    compact = args.output / "trials-with-stuck.jsonl"
    write_rows(compact, trials)
    for path in [compact, args.output / "prepare" / "coverage.jsonl", args.output / "prepare" / "pairs.jsonl", args.output / "stuck.jsonl"]:
        if path.exists():
            if path.stat().st_size < 1_000_000:
                shutil.copyfile(path, raw_dir / path.name)
            else:
                omitted.append({"name": path.name, "bytes": path.stat().st_size})
    summary["raw_omitted"] = omitted
    write_json(report / "summary.json", summary)
    lines = ["| Место | Вариант | Ходов | Дошли | Застреваний / 100 | Медиана, с | Медиана дошедших, с |",
             "|---|---|---:|---:|---:|---:|---:|"]
    sec = lambda value: f"{value/1000:.2f}" if value is not None else "—"
    for row in measurements:
        lines.append(f"| {row['region']} | {row['mode']} | {row['n']} | {row['arrived']} ({row['arrival_pct']:.1f}%) | {row['stuck_per_100']:.2f} | {sec(row['median_elapsed_ms'])} | {sec(row['median_arrival_ms'])} |")
    lines += ["", "| От | До | Место от | Место до | Длина ребра, м |", "|---|---|---|---|---:|"]
    for row in top_missing:
        lines.append(f"| {row['from']} | {row['to']} | {row['from_place']} | {row['to_place']} | {row['edge_length_cm']/100:.1f} |")
    (report / "tables.md").write_text("\n".join(lines)+"\n", encoding="utf-8")
    polygons = args.output / "prepare" / "navmesh-polygons.jsonl"
    if polygons.exists():
        for region in ("lobart", "market"):
            render_map(report / f"{region}.png", region, polygons, coverage, points, pairs)
    print(json.dumps({"report": str(report), "coverage": summary["coverage"], "paired": summary["paired"]}, ensure_ascii=False, indent=2))


FONT = {
    "A": "01110100011000111111100011000110001", "B": "11110100011000111110100011000111110",
    "C": "01111100001000010000100001000001111", "E": "11111100001000011110100001000011111",
    "H": "10001100011000111111100011000110001", "I": "11111001000010000100001000010011111",
    "K": "10001100101010011000101001001010001", "L": "10000100001000010000100001000011111",
    "M": "10001110111010110101100011000110001", "N": "10001110011010110011100011000110001",
    "O": "01110100011000110001100011000101110", "P": "11110100011000111110100001000010000",
    "R": "11110100011000111110101001001010001", "S": "01111100001000001110000010000111110",
    "T": "11111001000010000100001000010000100", "V": "10001100011000110001100010101000100",
    "W": "10001100011000110101101011101110001", "Y": "10001100010101000100001000010000100",
    "0": "01110100011001110101110011000101110", "1": "00100011000010000100001000010001110",
}


def render_map(path, region, polygons, coverage, points, pairs):
    width, height = 1200, 1000
    anchor_name = {"lobart": "NW_FARM1_ENTRANCE_01", "market": "NW_CITY_MERCHANT_PATH_20"}[region]
    anchor = points[anchor_name]
    half_x = 7000 if region == "lobart" else 5000
    half_z = half_x*(height-140)/(width-80)
    x0, x1 = anchor[0]-half_x, anchor[0]+half_x
    z0, z1 = anchor[2]-half_z, anchor[2]+half_z
    scale = min((width-80)/(x1-x0), (height-140)/(z1-z0))
    image = bytearray([249, 249, 245])*(width*height)

    def pixel(x, y, color):
        if 0 <= x < width and 0 <= y < height:
            image[(y*width+x)*3:(y*width+x)*3+3] = bytes(color)

    def project(p):
        return round(40+(p[0]-x0)*scale), round(height-40-(p[2]-z0)*scale)

    def line(a, b, color):
        dx, dy = b[0]-a[0], b[1]-a[1]
        length = max(abs(dx), abs(dy), 1)
        for i in range(length+1):
            pixel(round(a[0]+dx*i/length), round(a[1]+dy*i/length), color)

    def text(x, y, value, color=(40, 45, 50), zoom=2):
        for char in value:
            for i, bit in enumerate(FONT.get(char, "0"*35)):
                if bit == "1":
                    for ox in range(zoom):
                        for oy in range(zoom):
                            pixel(x+(i%5)*zoom+ox, y+(i//5)*zoom+oy, color)
            x += 6*zoom

    drawn = 0
    for row in rows(polygons):
        vertices = row.get("vertices", row.get("verts", row.get("points", [])))
        # NavMesh::writePolygons exports Recast meters; waypoint positions are game cm.
        vertices = [tuple(float(axis)*100 for axis in position(value)) for value in vertices]
        if len(vertices) < 3 or max(p[0] for p in vertices) < x0 or min(p[0] for p in vertices) > x1 or max(p[2] for p in vertices) < z0 or min(p[2] for p in vertices) > z1:
            continue
        poly = [project(p) for p in vertices]
        for y in range(max(100, min(p[1] for p in poly)), min(height-40, max(p[1] for p in poly))+1):
            crossings = []
            for a, b in zip(poly, poly[1:]+poly[:1]):
                if (a[1] <= y < b[1]) or (b[1] <= y < a[1]):
                    crossings.append(a[0]+(y-a[1])*(b[0]-a[0])/(b[1]-a[1]))
            crossings.sort()
            for a, b in zip(crossings[::2], crossings[1::2]):
                first, last = max(40, math.ceil(a)), min(width-40, math.floor(b))
                if last >= first:
                    offset = (y*width+first)*3
                    image[offset:offset+(last-first+1)*3] = bytes((174, 213, 183))*(last-first+1)
        for a, b in zip(poly, poly[1:]+poly[:1]):
            line(a, b, (140, 185, 153))
        drawn += 1
    for row in coverage:
        a, b, ok, _, _ = edge_fields(row)
        if a in points and b in points:
            pa, pb = points[a], points[b]
            if x0 <= pa[0] <= x1 and z0 <= pa[2] <= z1 and x0 <= pb[0] <= x1 and z0 <= pb[2] <= z1:
                line(project(pa), project(pb), (48, 64, 78) if ok else (205, 68, 45))
    for name, p in points.items():
        if name and not name.startswith("FP_") and x0 <= p[0] <= x1 and z0 <= p[2] <= z1:
            x, y = project(p)
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    pixel(x+dx, y+dy, (25, 42, 55))
    # Keep geometry outlines outside the plotting rectangle out of the legend.
    background = bytes((249, 249, 245))
    image[:100*width*3] = background*(100*width)
    image[(height-40)*width*3:] = background*(40*width)
    for y in range(100, height-40):
        image[y*width*3:(y*width+40)*3] = background*40
        image[(y*width+width-40)*3:(y+1)*width*3] = background*40
    ax, ay = project(anchor)
    line((ax-6, ay), (ax+6, ay), (40, 80, 165))
    line((ax, ay-6), (ax, ay+6), (40, 80, 165))
    text(ax+10, ay-7, "ANCHOR", (40, 80, 165), zoom=1)
    text(40, 20, region.upper())
    text(40, 48, "NAVMESH", (90, 150, 105))
    text(200, 48, "WAYNET", (48, 64, 78))
    text(350, 48, "NO PATH", (205, 68, 45))
    line((width-200, 40), (width-200+round(1000*scale), 40), (35, 40, 45))
    text(width-200, 50, "10 M")
    if not drawn:
        raise RuntimeError(f"Map {region} has no navmesh polygons in its bounds")
    raw = b"".join(b"\0"+image[y*width*3:(y+1)*width*3] for y in range(height))

    def chunk(kind, payload):
        return struct.pack("!I", len(payload))+kind+payload+struct.pack("!I", zlib.crc32(kind+payload)&0xffffffff)

    path.write_bytes(b"\x89PNG\r\n\x1a\n"+chunk(b"IHDR", struct.pack("!2I5B", width, height, 8, 2, 0, 0, 0))+
                     chunk(b"IDAT", zlib.compress(raw, 9))+chunk(b"IEND", b""))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    measure = sub.add_parser("run", help="fresh-world paired trials; use the shared Ryzen lock")
    for name in ("runner", "assets", "save", "output", "cache"):
        measure.add_argument("--"+name, type=Path, required=True)
    measure.add_argument("--ini", type=Path)
    measure.add_argument("--seed", type=int, default=7)
    measure.add_argument("--per-region", type=int, default=34)
    measure.add_argument("--limit-ms", type=int, default=90000)
    measure.add_argument("--npc", default=NPC)
    measure.add_argument("--prepare-only", action="store_true")
    measure.add_argument("--measure-only", action="store_true")
    measure.add_argument("--resume", action="store_true")
    measure.add_argument("--no-control", action="store_true")
    measure.add_argument("--control-only", action="store_true", help="only the original Maleth direct/mesh fixture")
    measure.add_argument("--smoke", type=int, metavar="PAIRS", help="measure only first N pairs (preparation check)")
    summary = sub.add_parser("summarize", help="summary JSON, Markdown tables, and top-down PNGs")
    summary.add_argument("--output", type=Path, required=True)
    summary.add_argument("--report", type=Path)
    summary.add_argument("--places", type=Path, default=REPO / "server/src/gothic_ai/gamedata/places.json")
    args = parser.parse_args()
    for key, value in vars(args).items():
        if isinstance(value, Path):
            setattr(args, key, value.expanduser().resolve())
    if args.command == "run":
        if args.prepare_only and args.measure_only:
            parser.error("--prepare-only and --measure-only are mutually exclusive")
        if args.control_only and args.no_control:
            parser.error("--control-only and --no-control are mutually exclusive")
        if args.limit_ms < 1000 or not 0 <= args.seed <= 0xffffffff:
            parser.error("limit must be >=1000ms; seed must be uint32")
        run(args)
    else:
        summarize(args)


if __name__ == "__main__":
    main()
