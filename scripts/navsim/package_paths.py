#!/usr/bin/env python3
"""Compare package path lengths with the engine, without loading the game.

Usage: python scripts/navsim/package_paths.py PACKAGE BUILD_DIRECTORY OUTPUT_JSON
Run on Ryzen through gq. The helper compiles navmesh.cpp and Detour from this
checkout, loads the exported cache, and queries every exported PPO pair.
"""
from __future__ import annotations

import json
import math
import os
from pathlib import Path
import shlex
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from navsim.formats import read_package

ROOT = Path(__file__).resolve().parents[2]


def compare(directory: Path, build: Path, output: Path) -> dict:
    package = read_package(directory)
    checks = package.path_checks
    complete_pairs = {(row["start"], row["goal"]) for row in checks if row["complete"]}
    if len(complete_pairs) < 200:
        raise ValueError(f"Need 200 complete distinct package pairs, found {len(complete_pairs)}")
    build.mkdir(parents=True, exist_ok=True)
    native = ROOT / "OpenGothic"
    recast = native / "lib/recastnavigation"
    binary = build / "package-paths"
    subprocess.run([
        *shlex.split(os.environ.get("CXX", "c++")), "-std=c++20", "-O2", "-DDT_POLYREF64",
        "-Wno-deprecated-declarations", "-I", str(native / "common"),
        "-I", str(recast / "Recast/Include"), "-I", str(recast / "Detour/Include"),
        str(ROOT / "scripts/navsim/package_paths.cpp"), str(native / "common/physics/navmesh.cpp"),
        *map(str, sorted((recast / "Recast/Source").glob("*.cpp"))),
        *map(str, sorted((recast / "Detour/Source").glob("*.cpp"))), "-o", str(binary),
    ], check=True)
    queries = "".join(" ".join(map(str, [*row["start_m"], *row["end_m"], int(row["allow_transitions"])])) + "\n"
                      for row in checks)
    params = package.navmesh_params
    process = subprocess.run([str(binary), str(directory / "navmesh.navmesh"),
                              str(params["nearest_horizontal_m"]), str(params["nearest_vertical_m"])],
                             input=queries, text=True, capture_output=True, check=True)
    rows = process.stdout.splitlines()
    if len(rows) != len(checks):
        raise ValueError(f"Path query count differs: {len(rows)} != {len(checks)}")
    mismatches = []
    max_error = 0.
    for index, (expected, text) in enumerate(zip(checks, rows)):
        complete, length, reason = text.split()
        complete, length = bool(int(complete)), float(length)
        error = abs(length - expected["length_m"])
        max_error = max(max_error, error)
        if (complete != expected["complete"] or reason != expected["reason"]
                or not math.isclose(length, expected["length_m"], rel_tol=0, abs_tol=1.e-4)):
            mismatches.append(dict(index=index, split=expected["split"], pair_index=expected["pair_index"],
                                   expected=dict(complete=expected["complete"], length_m=expected["length_m"],
                                                 reason=expected["reason"]),
                                   actual=dict(complete=complete, length_m=length, reason=reason)))
    result = dict(package=str(directory), engine_revision=package.manifest["engine_revision"],
                  queries=len(checks), complete_distinct_pairs=len(complete_pairs),
                  mismatches=len(mismatches), max_absolute_error_m=max_error, tolerance_m=1.e-4,
                  examples=mismatches[:5])
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit("Usage: package_paths.py PACKAGE BUILD_DIRECTORY OUTPUT_JSON")
    result = compare(*(Path(value).resolve() for value in sys.argv[1:]))
    print(json.dumps(result))
    sys.exit(bool(result["mismatches"]))
