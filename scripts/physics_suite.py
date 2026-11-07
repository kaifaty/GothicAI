#!/usr/bin/env python3
"""Run stage 1 physics numeric scenarios with the headless game runner."""

import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import re
from collections import Counter


SCENARIOS = ("smoke", "ik-moves", "ik-stairs", "ik-sweep", "lean", "aim", "manual-aim", "crossbow", "quad-wolf", "quad-zoo", "flinch")


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--save", type=Path, required=True)
    parser.add_argument("--scenarios", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--backend", required=True)
    parser.add_argument("--queries", type=Path)
    parser.add_argument("--only", nargs="+", default=["queries", *SCENARIOS])
    parser.add_argument("--timeout", type=int, default=900)
    parser.add_argument("--resume", action="store_true", help="continue this runner/save's incomplete suite, preserving completed evidence")
    args = parser.parse_args()
    for name in ("runner", "assets", "save", "scenarios", "output", "queries"):
        value = getattr(args, name)
        if value is not None:
            setattr(args, name, value.resolve())
    args.output.mkdir(parents=True, exist_ok=args.resume)
    manifest = {"schema_version": 1, "backend": args.backend, "runner": str(args.runner), "runner_sha256": sha(args.runner), "save": str(args.save), "save_sha256": sha(args.save), "assets": str(args.assets), "mode": "headless lockstep, 30fps, no render or models", "scenarios": {}}
    if args.resume:
        prior = json.loads((args.output / "manifest.json").read_text())
        if any(prior[field] != manifest[field] for field in ("backend", "runner_sha256", "save_sha256", "assets", "mode")):
            raise SystemExit("resume provenance differs from original suite")
        manifest = prior
    env = dict(os.environ, GOTHIC_AI="0", DISPLAY="", WAYLAND_DISPLAY="", OPENGOTHIC_TEXTURES="", OPENGOTHIC_BODY_ANIMATION_RNG="isolated", VK_DRIVER_FILES="/nonexistent/physics-no-vulkan.json", VK_ICD_FILENAMES="/nonexistent/physics-no-vulkan.json", ALSOFT_DRIVERS="null")
    failures = []
    for name in args.only:
        if args.resume and manifest["scenarios"].get(name, {}).get("pass"):
            continue
        out = args.output / name
        if args.resume and out.exists():
            out.rename(args.output / (name + "-incomplete-" + str(time.time_ns())))
        out.mkdir()
        run = out / "run"
        run.mkdir()
        (run / "Gothic.ini").write_text("[INTERNAL]\nvidResIndex=2\n[SOUND]\nsoundVolume=0\nmusicVolume=0\n")
        scenario = out / "scenario.bench"
        if name == "queries":
            replay = " " + str(args.queries) if args.queries else ""
            scenario.write_text("physics dump 100000 73013" + replay + "\n")
        else:
            source = args.scenarios / (name + ".bench")
            lines = []
            for line in source.read_text().splitlines():
                command, marker, comment = line.partition("#")
                if command.strip().startswith("rec ") and not command.strip().endswith(" novideo"):
                    line = command.rstrip() + " novideo" + (" #" + comment if marker else "")
                lines.append(line)
            scenario.write_text("seed 73013\n" + "\n".join(lines) + "\n")
        argv = [str(args.runner), "-g", str(args.assets), "--save", str(args.save), "--scenario", str(scenario), "--output", str(out), "--mode", "lockstep"]
        start = time.monotonic()
        scenario_env = dict(env, OPENGOTHIC_PHYSICS_TRACE=str(out / "physics-time.csv"))
        with (out / "launcher.log").open("w") as log:
            try:
                completed = subprocess.run(argv, cwd=run, env=scenario_env, stdout=log, stderr=subprocess.STDOUT, timeout=args.timeout)
                rc = completed.returncode
            except subprocess.TimeoutExpired:
                rc = 124
        bench = (out / "bench.log").read_text() if (out / "bench.log").exists() else ""
        errors = [line for line in bench.splitlines() if any(word in line for word in ("unknown ", " failed", "can not", "timeout", "no waypoint", ": no npc", ": no item", ": no spot", ": no edge"))]
        expected_foreach = []
        expected_records = Counter()
        indices = [None]
        for line in scenario.read_text().splitlines():
            words = line.split("#", 1)[0].split()
            if not words:
                continue
            if words[0] == "foreach":
                index = {"slope": 4, "stairs": 2, "wp": 3}[words[1]]
                count = int(words[index])
                expected_foreach.append((words[1], count))
                indices = list(range(count))
            elif words[0] == "end":
                indices = [None]
            elif words[0] == "rec":
                for index in indices:
                    label = words[1].replace("$I", str(index)) if index is not None else words[1]
                    expected_records[label] += 1
        actual_foreach = [(kind, int(count)) for kind, count in re.findall(r"^foreach (\w+): (\d+) spots$", bench, re.MULTILINE)]
        actual_records = Counter(label for label in re.findall(r"^rec (\S+)", bench, re.MULTILINE) if label != "stop")
        if actual_foreach != expected_foreach:
            errors.append("foreach coverage differs: " + repr((expected_foreach, actual_foreach)))
        if actual_records != expected_records:
            errors.append("record coverage differs: " + repr((expected_records, actual_records)))
        finished = "done" in bench.splitlines()
        rows = 0
        if (out / "metrics.csv").exists():
            with (out / "metrics.csv").open() as file:
                rows = sum(1 for _ in csv.DictReader(file))
        result = {"returncode": rc, "done": finished, "errors": errors, "wall_seconds": time.monotonic() - start, "metric_rows": rows, "scenario_sha256": sha(scenario), "argv": argv}
        timing_path = out / "physics-time.csv"
        if timing_path.exists() and name != "queries":
            samples = [list(map(int, line.split(","))) for line in timing_path.read_text().splitlines()]
            values = sorted(row[1] for row in samples[32:])
            if values:
                result["physics_frame_timing"] = {"frames": len(values), "warmup_frames": 32, "p50_ns": values[int(0.5 * (len(values) - 1))], "p95_ns": values[int(0.95 * (len(values) - 1))], "scope": "DynamicWorld tick and public query/movement work between ticks, nested calls counted once; temporary identical instrumentation"}
        result["pass"] = rc == 0 and finished and not errors and (rows > 0 or name == "queries")
        manifest["scenarios"][name] = result
        (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        print(f"{args.backend} {name}: {'PASS' if result['pass'] else 'FAIL'}, rows={rows}, wall={result['wall_seconds']:.1f}s", flush=True)
        if not result["pass"]:
            failures.append(name)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
