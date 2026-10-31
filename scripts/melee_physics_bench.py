#!/usr/bin/env python3
"""Melee physics matrix and paired flags, normal 30 s cadence without video."""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import subprocess


ROOT = Path(__file__).resolve().parent.parent


def percentile(values: list[float]) -> float:
    return sorted(values)[math.ceil(len(values)*.95)-1]


def summarize(output: Path, *, shadow: bool = False) -> dict:
    with (output/"appcadence.csv").open() as stream:
        frames = list(csv.DictReader(stream))
    seconds = sum(int(row["interval_ns"]) for row in frames)/1e9
    if seconds < 30 or not frames:
        raise RuntimeError(f"incomplete 30 s cadence: {output}")
    with (output/"melee-physics.csv").open() as stream:
        samples = [{key: float(value) for key, value in row.items()} for row in csv.DictReader(stream)]
    active = [row for row in samples if row["updates"] > 0]
    if not samples or (not active and not shadow) or any(row["finite"] != 1 for row in samples):
        raise RuntimeError(f"missing physics steps or nonfinite rig: {output}")
    measured = active or samples
    result = {"frames": len(frames), "seconds": seconds, "fps": len(frames)/seconds,
              "samples": len(samples), "active_samples": len(active),
              "physics_mean_ms": statistics.mean(row["physics_ms"] for row in samples),
              "physics_p95_ms": percentile([row["physics_ms"] for row in samples]),
              "active_physics_mean_ms": statistics.mean(row["physics_ms"] for row in measured),
              "active_physics_p95_ms": percentile([row["physics_ms"] for row in measured]),
              "solve_mean_ms": statistics.mean(row["solve_ms"] for row in measured),
              "solve_p95_ms": percentile([row["solve_ms"] for row in measured]),
              "cast_mean_ms": statistics.mean(row["cast_ms"] for row in measured),
              "cast_p95_ms": percentile([row["cast_ms"] for row in measured])}
    events = [json.loads(line) for line in (output/"events.jsonl").read_text().splitlines()]
    begin = next(row for row in events if row["kind"] == "melee_physics_measure_before")
    end = next(row for row in events if row["kind"] == "melee_physics_measure_after")
    result.update(game_seconds=(end["t"]-begin["t"])/1000,
                  steady_mix=begin.get("steady_mix", False), brawl=begin.get("brawl", False),
                  attack_starts=end.get("attack_starts", 0), knockdowns=0, posture_hits=0)
    posture = output/"run/melee-posture.jsonl"
    rows = []
    if posture.is_file():
        rows = [json.loads(line) for line in posture.read_text().splitlines()]
        result["knockdowns"] = sum(row["event"] == "knockdown" for row in rows)
        result["posture_hits"] = sum(row["event"] == "hit" for row in rows)
    starts = [row for row in rows if row["event"] == "getup_start" and "wait_ms" in row]
    durations = [row["getup_duration_ms"] for row in rows if row["event"] == "recover" and "getup_duration_ms" in row]
    forced = sum(row["forced"] for row in starts)
    result.update(getup_starts=len(starts), getup_completed=len(durations), forced_getup_starts=forced,
                  forced_getup_fraction=forced/result["knockdowns"] if result["knockdowns"] else None,
                  forced_getup_start_fraction=forced/len(starts) if starts else None)
    for name, values in (("getup_wait_ms", [row["wait_ms"] for row in starts]), ("getup_duration_ms", durations)):
        result.update({name+"_median": statistics.median(values) if values else None,
                       name+"_min": min(values) if values else None, name+"_max": max(values) if values else None})
    with (output/"melee-physics.csv.steps.csv").open() as stream:
        steps = list(csv.DictReader(stream))
    result["fixed_steps"] = len(steps)
    result["step_mean_ms"] = statistics.mean(float(row["physics_ms"]) for row in steps)
    result["step_p95_ms"] = percentile([float(row["physics_ms"]) for row in steps])
    result["step_cast_mean_ms"] = statistics.mean(float(row["cast_ms"]) for row in steps)
    result["step_cast_p95_ms"] = percentile([float(row["cast_ms"]) for row in steps])
    for key in ("updates", "casts", "body_hits", "world_hits", "floor_samples", "floor_misses"):
        result[key] = int(sum(row[key] for row in samples))
    for key in ("dynamic_parts", "blades", "max_linear_cm_s", "max_angular_rad_s", "pose_rms_cm",
                "pose_max_cm", "max_foot_drop_cm", "max_foot_penetration_cm"):
        result[key] = max(row[key] for row in samples)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--case", action="append", help="one matrix row: count:mode:pelvis:substeps")
    parser.add_argument("--shadow-pair", action="store_true", help="stage 1: ten native animated fighters, shadow OFF then ON")
    parser.add_argument("--hits-pair", action="store_true", help="stage 2: ten animated fighters, physics hits OFF then ON; shadow stays ON")
    parser.add_argument("--posture-pair", action="store_true", help="stage 3: six native fighters, posture and body reactions OFF then ON; physics hits stay ON")
    args = parser.parse_args()
    if sum((args.hits_pair, args.shadow_pair, args.posture_pair)) > 1:
        parser.error("choose one of --hits-pair, --shadow-pair or --posture-pair")
    paired = args.shadow_pair or args.hits_pair or args.posture_pair
    if paired and args.case:
        parser.error("paired measurements and --case are mutually exclusive")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    binary = ROOT/"OpenGothic/build/opengothic/Gothic2Notr"
    binary_hash = hashlib.sha256(binary.read_bytes()).hexdigest()
    cases = args.case or [f"{count}:{mode}:{pelvis}:{steps}"
                         for count in (1, 2, 4, 10) for mode in ("full", "chain")
                         for pelvis in ("kinematic", "dynamic") for steps in (1, 2)]
    if args.case is None and not paired:
        cases = [f"{count}:base:kinematic:{steps}" for count in (0, 1, 2, 4, 10) for steps in (1, 2)] + cases
        cases += [f"10:blade:kinematic:{steps}" for steps in (1, 2)]
    if paired:
        # the posture pair keeps the fighters' ordinary living bodies: the blows push them
        cases = ["6:game:kinematic:2" if args.posture_pair else "10:base:kinematic:2"]*2
    template = (ROOT/"scripts/bench/melee-physics.bench").read_text()
    results = []
    for index, case in enumerate(cases):
        subprocess.run(["/bin/sh", "-c", "until ! pgrep -x Gothic2Notr >/dev/null; do sleep 5; done"], check=True)
        count, mode, pelvis, steps = case.split(":")
        flag = "posture" if args.posture_pair else "hits" if args.hits_pair else "shadow"
        label = f"{flag}-{'on' if index else 'off'}" if paired else case.replace(":", "-")
        take = output/label
        take.mkdir()
        scenario = take/"melee-physics.bench"
        scenario_text = template.replace("setup 10 full kinematic 2", f"setup {count} {mode} {pelvis} {steps}")
        if args.hits_pair:
            scenario_text = scenario_text.replace("cadence normal", "melee-physics steady\ncadence normal")
        if args.posture_pair:
            scenario_text = scenario_text.replace("cadence normal", "melee-physics brawl\ncadence normal")
        scenario.write_text(scenario_text)
        env = {**os.environ, "BENCH_HOST": "local", "NO_BUILD": "1", "WORLD": "NEWWORLD.ZEN",
               "OPENGOTHIC_MELEE_BENCH": "fixture", "GOTHIC_AI": "0", "TIMEOUT": "180"}
        if paired:
            env["MELEE_BLADE_SHADOW"] = str(index) if args.shadow_pair else "1"
            env["MELEE_PHYSICS_HITS"] = str(index) if args.hits_pair else "0" if args.shadow_pair else "1"
        if args.posture_pair:
            # stage 3 as a whole: knockdown logic and the body's reaction to blows
            env["MELEE_POSTURE"] = str(index)
            env["MELEE_REACTIONS"] = str(index)
        print(f"Running {case}", flush=True)
        with (take/"launcher.log").open("w") as stream:
            subprocess.run([str(ROOT/"scripts/bench.sh"), str(scenario), str(take)],
                           cwd=ROOT, env=env, stdout=stream, stderr=subprocess.STDOUT, check=True)
        result = {"case": label, "binary_sha256": binary_hash, **summarize(take, shadow=paired)}
        results.append(result)
        if mode == "blade" and (result["blades"] != int(count) or result["casts"] == 0):
            raise RuntimeError(f"missing blade bodies or casts: {case}")
        with (output/"summary.csv").open("a") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(result))
            if stream.tell() == 0:
                writer.writeheader()
            writer.writerow(result)
        print(f"{case}: step p95 {result['step_p95_ms']:.3f} ms, mean {result['step_mean_ms']:.3f} ms, "
              f"{result['fps']:.2f} FPS, pose max {result['pose_max_cm']:.2f} cm", flush=True)
    if paired:
        baseline, candidate = results
        loss = 100*(1-candidate["fps"]/baseline["fps"])
        comparison = {"physics_p95_ms": candidate["physics_p95_ms"], "step_p95_ms": candidate["step_p95_ms"],
                      "fps_off": baseline["fps"], "fps_on": candidate["fps"], "fps_loss_percent": loss,
                      "physics_budget_pass": max(candidate["physics_p95_ms"], candidate["step_p95_ms"]) <= 2,
                      "fps_budget_pass": loss <= 5}
        (output/"comparison.json").write_text(json.dumps(comparison, indent=2)+"\n")
        print(f"{flag} ON: physics step p95 {candidate['step_p95_ms']:.3f} ms; FPS loss {loss:.2f}%", flush=True)
        return int(not comparison["physics_budget_pass"] or not comparison["fps_budget_pass"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
