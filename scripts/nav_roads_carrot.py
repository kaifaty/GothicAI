#!/usr/bin/env python3
"""Replay familiar-road guidance on recorded trajectories with two KnownRoads revisions.

Offline and light: no game, world or policy. Each episode directory needs
roads-events.jsonl (routine nodes, spawn, stranger), episode.json (goal kind and
target) and trajectory.txt (the physical position of every decision). Follow,
object and approach goals bypass the roads and are skipped. The road graph is the
stand's roads-graph.json. scripts/nav_roads_carrot.cpp is compiled with
OpenGothic/common/game/knownroads.{h,cpp} of each revision (a git revision or
WORKTREE); every revision replays the same positions.

A progress jump is one decision without a rebuild where road progress grows by
more than --jump-cm. An initial skip is a rebuild whose projection lies farther
along the new route than the actor stands from its first node plus --jump-cm.
Live recordings also hold the carrot the game used; the replay of the recorded
revision must reproduce it. The first revision is the reference: the carrots of
the second are compared with it, separately on episodes where the reference had
neither a jump nor an initial skip.

  python3 scripts/nav_roads_carrot.py --graph roads-graph.json --jump-cm 650 \\
      --revision before=origin/main --revision after=WORKTREE \\
      --set point-v5-run=results/v5-run/validation/point --set live-v5=live/pace-v5 \\
      --output roads-carrot.json
"""
from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ("OpenGothic/common/game/knownroads.h", "OpenGothic/common/game/knownroads.cpp")
EPSILON_CM = .01  # a step of exactly the window is not a jump


def read_json(path):
    return json.loads(Path(path).read_text())


def read_jsonl(path):
    return [json.loads(line) for line in Path(path).read_text().splitlines() if line.strip()]


def graph_text(path):
    graph = read_json(path)
    rows = [f"graph {len(graph['nodes'])} {len(graph['edges'])}"]
    rows += [f"{x!r} {y!r} {z!r}" for _, x, y, z in graph["nodes"]]
    rows += [f"{a} {b} {length!r} {int(bool(major))}" for a, b, length, major in graph["edges"]]
    return "\n".join(rows) + "\n", graph


def episode(directory):
    """One road-guided point episode, or None when the carrot does not follow roads."""
    directory = Path(directory)
    events = directory / "roads-events.jsonl"
    info = directory / "episode.json"
    trajectory = directory / "trajectory.txt"
    if not (events.is_file() and info.is_file() and trajectory.is_file()):
        return None
    meta = read_json(info)
    if meta.get("goal_kind") != "point":
        return None
    enabled = [row for row in read_jsonl(events) if row.get("kind") == "roads_enabled"]
    if len(enabled) != 1:
        return None
    enabled = enabled[0]
    positions = [tuple(map(float, line.split()[1:4])) for line in trajectory.read_text().splitlines() if line.strip()]
    if not positions:
        return None
    result = dict(directory=str(directory), stranger=bool(enabled["stranger"]),
                  routine=[r["node"] for r in enabled["routines"] if r["node"] is not None],
                  spawn=enabled["spawn_cm"], goal=meta["target_position"], positions=positions,
                  known=(enabled["known_nodes"], enabled["known_edges"]), success=bool(meta.get("success")),
                  elapsed_ms=meta.get("elapsed_ms"))
    decisions = directory / "live-decisions.jsonl"
    if decisions.is_file():
        result["recorded_carrot"] = [row["carrot_cm"] for row in read_jsonl(decisions)]
    return result


def collect(root):
    root = Path(root)
    found = []
    for events in sorted(root.rglob("roads-events.jsonl")):
        ep = episode(events.parent)
        if ep is not None:
            ep["id"] = events.parent.relative_to(root).as_posix() or events.parent.name
            found.append(ep)
    return found


def compile_replay(revision, scratch):
    """Build the replay against knownroads.{h,cpp} of one git revision or the worktree."""
    include = scratch / revision.replace("/", "_")
    (include / "game").mkdir(parents=True)
    for name in SOURCES:
        target = include / "game" / Path(name).name
        if revision == "WORKTREE":
            target.write_bytes((ROOT / name).read_bytes())
        else:
            target.write_bytes(subprocess.run(["git", "-C", str(ROOT), "show", f"{revision}:{name}"],
                                              check=True, capture_output=True).stdout)
    binary = include / "replay"
    subprocess.run([*shlex.split(os.environ.get("CXX", "c++")), "-std=c++20", "-O2",
                    "-I", str(include), "-I", str(ROOT / "OpenGothic/lib/Tempest/Engine/include"),
                    str(ROOT / "scripts/nav_roads_carrot.cpp"), str(include / "game/knownroads.cpp"),
                    "-o", str(binary)], check=True)
    return binary


def replay(binary, graph, episodes):
    rows = [graph]
    for index, ep in enumerate(episodes):
        rows.append(f"episode {index} {int(ep['stranger'])} {len(ep['routine'])} "
                    + " ".join(map(str, ep["routine"])) + " "
                    + " ".join(repr(float(v)) for v in (*ep["spawn"], *ep["goal"]))
                    + f" {len(ep['positions'])}")
        rows += [f"{x!r} {y!r} {z!r}" for x, y, z in ep["positions"]]
    run = subprocess.run([str(binary)], input="\n".join(rows) + "\n", capture_output=True, text=True, check=True)
    results, current = [], None
    for line in run.stdout.splitlines():
        parts = line.split()
        if parts[0] == "episode":
            current = dict(known=(int(parts[2]), int(parts[3])), decisions=[], polylines={})
            results.append(current)
        elif parts[0] == "d":
            progress, rebuilt, beyond, cx, cy, cz, remaining = parts[1:]
            current["decisions"].append(dict(progress=float(progress), rebuilt=rebuilt == "1", beyond=beyond == "1",
                                             carrot=(float(cx), float(cy), float(cz)), remaining=float(remaining)))
        elif parts[0] == "polyline":
            values = list(map(float, parts[2:]))
            current["polylines"][len(current["decisions"]) - 1] = [values[i:i + 3] for i in range(0, len(values), 3)]
    if len(results) != len(episodes):
        raise ValueError(f"replay returned {len(results)} episodes for {len(episodes)}")
    return results


def jumps(decisions, threshold):
    """Single-decision progress growth beyond threshold on the same route."""
    found = []
    for index in range(1, len(decisions)):
        step = decisions[index]["progress"] - decisions[index - 1]["progress"]
        if not decisions[index]["rebuilt"] and step > threshold + EPSILON_CM:
            found.append(dict(decision=index, step_cm=round(step, 3)))
    return found


def initial_skips(result, positions, threshold):
    """Rebuilds whose projection is farther along than the first node is from the actor, plus threshold."""
    found = []
    for index, line in result["polylines"].items():
        progress = result["decisions"][index]["progress"]
        offset = math.dist(positions[index], line[0])
        if progress > offset + threshold + EPSILON_CM:
            found.append(dict(decision=index, progress_cm=round(progress, 3), start_offset_cm=round(offset, 3)))
    return found


def creep(decisions, positions, threshold, span=10):
    """Decisions where one second of progress (no rebuild) exceeds the actor's XZ travel plus threshold."""
    count = 0
    for index in range(span, len(decisions)):
        if any(decisions[i]["rebuilt"] for i in range(index - span + 1, index + 1)):
            continue
        travel = sum(math.dist((positions[i][0], positions[i][2]), (positions[i - 1][0], positions[i - 1][2]))
                     for i in range(index - span + 1, index + 1))
        if decisions[index]["progress"] - decisions[index - span]["progress"] > travel + threshold + EPSILON_CM:
            count += 1
    return count


def summarize(episodes, replays, threshold):
    names = list(replays)
    per_episode = []
    totals = {name: dict(decisions=0, jumps=0, episodes_with_jump=0, initial_skips=0, episodes_with_initial_skip=0,
                         rebuilds=0, creep_decisions=0, max_jump_cm=0., recorded_carrot_max_error_cm=None)
              for name in names}
    same = dict(decisions=0, carrot_within_1cm=0, clean_episodes=0, clean_decisions=0, clean_within_1cm=0,
                clean_differences=[])
    for index, ep in enumerate(episodes):
        row = dict(id=ep["id"], success=ep["success"], elapsed_ms=ep["elapsed_ms"], decisions=len(ep["positions"]))
        for name in names:
            result = replays[name][index]
            if result["known"] != tuple(ep["known"]):
                raise ValueError(f"{ep['id']}: rebuilt knowledge {result['known']} differs from recorded {ep['known']}")
            row[name] = dict(jumps=jumps(result["decisions"], threshold),
                             initial_skips=initial_skips(result, ep["positions"], threshold),
                             rebuilds=sum(d["rebuilt"] for d in result["decisions"]),
                             creep_decisions=creep(result["decisions"], ep["positions"], threshold))
            total = totals[name]
            total["decisions"] += len(result["decisions"])
            total["jumps"] += len(row[name]["jumps"])
            total["episodes_with_jump"] += bool(row[name]["jumps"])
            total["initial_skips"] += len(row[name]["initial_skips"])
            total["episodes_with_initial_skip"] += bool(row[name]["initial_skips"])
            total["rebuilds"] += row[name]["rebuilds"]
            total["creep_decisions"] += row[name]["creep_decisions"]
            total["max_jump_cm"] = max(total["max_jump_cm"], max((j["step_cm"] for j in row[name]["jumps"]), default=0.))
            if "recorded_carrot" in ep:
                error = max(math.dist(a["carrot"], b) for a, b in zip(result["decisions"], ep["recorded_carrot"]))
                row[name]["recorded_carrot_max_error_cm"] = error
                total["recorded_carrot_max_error_cm"] = max(total["recorded_carrot_max_error_cm"] or 0., error)
        if len(names) == 2:
            first, second = (replays[name][index]["decisions"] for name in names)
            close = [math.dist(a["carrot"], b["carrot"]) <= 1. for a, b in zip(first, second)]
            row["carrot_within_1cm"] = sum(close)
            row["first_different_decision"] = close.index(False) if not all(close) else None
            same["decisions"] += len(close)
            same["carrot_within_1cm"] += sum(close)
            if not row[names[0]]["jumps"] and not row[names[0]]["initial_skips"]:
                same["clean_episodes"] += 1
                same["clean_decisions"] += len(close)
                same["clean_within_1cm"] += sum(close)
                if not all(close):
                    same["clean_differences"].append(dict(id=ep["id"], decisions=len(close) - sum(close),
                                                          first=row["first_different_decision"]))
        per_episode.append(row)
    for total in totals.values():
        total["jump_ratio"] = total["jumps"] / total["decisions"] if total["decisions"] else 0.
    return dict(episodes=len(episodes), totals=totals, same_carrot=same, per_episode=per_episode)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--graph", required=True, type=Path)
    parser.add_argument("--revision", action="append", required=True, metavar="NAME=GIT_REVISION|WORKTREE")
    parser.add_argument("--set", action="append", required=True, metavar="NAME=ROOT")
    parser.add_argument("--jump-cm", type=float, required=True, help="progress growth per decision counted as a jump")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    graph, data = graph_text(args.graph)
    revisions = dict(item.split("=", 1) for item in args.revision)
    sets = dict(item.split("=", 1) for item in args.set)
    report = dict(graph=dict(nodes=len(data["nodes"]), edges=len(data["edges"])), jump_cm=args.jump_cm,
                  revisions={name: revision if revision == "WORKTREE" else subprocess.run(
                      ["git", "-C", str(ROOT), "rev-parse", revision], check=True, capture_output=True,
                      text=True).stdout.strip() for name, revision in revisions.items()}, sets={})
    with tempfile.TemporaryDirectory() as scratch:
        binaries = {name: compile_replay(revision, Path(scratch)) for name, revision in revisions.items()}
        for label, root in sets.items():
            episodes = collect(root)
            replays = {name: replay(binary, graph, episodes) for name, binary in binaries.items()}
            report["sets"][label] = dict(root=str(root), **summarize(episodes, replays, args.jump_cm))
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    for label, value in report["sets"].items():
        print(json.dumps(dict(set=label, episodes=value["episodes"], totals=value["totals"],
                              same_carrot={k: v for k, v in value["same_carrot"].items() if k != "clean_differences"},
                              clean_differences=value["same_carrot"]["clean_differences"])))


if __name__ == "__main__":
    sys.exit(main())
