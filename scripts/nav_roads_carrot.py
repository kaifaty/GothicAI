#!/usr/bin/env python3
"""Replay familiar-road guidance on recorded trajectories with two KnownRoads revisions.

Offline and light: no game, world or policy. Each episode directory needs
roads-events.jsonl (routine nodes, spawn, stranger), episode.json (goal kind,
target and decision count) and trajectory.txt (the physical position of every
decision; its extra final pose from navdemos finish() is not a decision). Follow,
object and approach goals bypass the roads and are skipped. The road graph is the
stand's roads-graph.json. scripts/nav_roads_carrot.cpp is compiled with
OpenGothic/common/game/knownroads.{h,cpp} of each revision (a git revision,
WORKTREE, or a directory holding knownroads.h and knownroads.cpp); every
revision replays the same positions.

A progress jump is one decision without a rebuild where road progress grows by
more than --jump-cm. An initial skip is a rebuild whose projection lies farther
along the new route than the actor stands from its first node plus --jump-cm.
A catch-up is a decision where a revision with a passage check took a later
stretch through a passage its memory saw free; catch-ups are counted apart
from jumps and initial skips. Live recordings also hold the carrot the game
used; the replay of the recorded revision must reproduce it. The first revision
is the reference: the carrots of every other are compared with it, separately on
episodes where the reference had neither a jump nor an initial skip. Creep is one
second of progress growth beyond the actor's own XZ travel plus --jump-cm, outside
rebuilds and catch-ups; lag is a decision where the actor is within 3 m of the
route at a point more than --jump-cm ahead of progress.

Passage: a revision with a passage check runs with none (`never`, the window rule)
or with the navmesh `oracle` as the walker's memory (--passage NAME=oracle): Free
where the straight segment stays on the walkable surface. Recordings carry no
memory, so the oracle stands in for a memory that has seen everything.
--recorded NAME reads instead the guidance the game itself wrote with its
memory (roads-guidance.txt of the closed-loop runs).

With --navmesh (the stand's OGNAV001 cache), the oracle of the measurement:
whether the straight segment from the actor to its progress point, and to the
first node of a rebuilt route, leaves the walkable surface (a wall, a ledge, a
drop) where the walkable way around is more than 1 m longer. An advance is a
jump, a catch-up beyond the window or an initial skip; a start event is a
rebuild whose first node differs from the previous rebuild's.

  python3 scripts/nav_roads_carrot.py --graph roads-graph.json --jump-cm 650 \\
      --navmesh newworld.navmesh --revision before=origin/main --revision after=WORKTREE \\
      --revision oracle=WORKTREE --passage oracle=oracle \\
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
    rows = [tuple(map(float, line.split()[1:4])) for line in trajectory.read_text().splitlines() if line.strip()]
    decisions = meta.get("decisions")
    if not isinstance(decisions, int) or decisions <= 0:
        return None
    # One row per decision, then at most the final pose written by finish().
    if len(rows) not in (decisions, decisions + 1):
        raise ValueError(f"{directory}: {len(rows)} trajectory rows for {decisions} decisions")
    positions = rows[:decisions]
    result = dict(directory=str(directory), stranger=bool(enabled["stranger"]),
                  routine=[r["node"] for r in enabled["routines"] if r["node"] is not None],
                  spawn=enabled["spawn_cm"], goal=meta["target_position"], positions=positions,
                  known=(enabled["known_nodes"], enabled["known_edges"]), success=bool(meta.get("success")),
                  elapsed_ms=meta.get("elapsed_ms"))
    recorded = directory / "live-decisions.jsonl"
    if recorded.is_file():
        result["recorded_carrot"] = [row["carrot_cm"] for row in read_jsonl(recorded)]
        if len(result["recorded_carrot"]) != decisions:
            raise ValueError(f"{directory}: {len(result['recorded_carrot'])} live decisions for {decisions}")
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


def compile_replay(revision, scratch, navmesh=None):
    """Build the replay against knownroads.{h,cpp} of one git revision or the worktree."""
    include = scratch / revision.strip("/").replace("/", "_")
    if (include / "replay").is_file():
        return include / "replay"
    (include / "game").mkdir(parents=True)
    for name in SOURCES:
        target = include / "game" / Path(name).name
        if revision == "WORKTREE":
            target.write_bytes((ROOT / name).read_bytes())
        elif Path(revision).is_dir():
            target.write_bytes((Path(revision) / Path(name).name).read_bytes())
        else:
            target.write_bytes(subprocess.run(["git", "-C", str(ROOT), "show", f"{revision}:{name}"],
                                              check=True, capture_output=True).stdout)
    binary = include / "replay"
    oracle = []
    if navmesh is not None:
        detour = ROOT / "OpenGothic/lib/recastnavigation/Detour"
        oracle = ["-DROADS_ORACLE", "-DDT_POLYREF64", "-Wno-deprecated-declarations", "-I", str(detour / "Include"),
                  *map(str, sorted((detour / "Source").glob("*.cpp")))]
    subprocess.run([*shlex.split(os.environ.get("CXX", "c++")), "-std=c++20", "-O2",
                    "-I", str(include), "-I", str(ROOT / "OpenGothic/lib/Tempest/Engine/include"),
                    str(ROOT / "scripts/nav_roads_carrot.cpp"), str(include / "game/knownroads.cpp"), *oracle,
                    "-o", str(binary)], check=True)
    return binary


def parse_guidance(lines, current):
    """Decision and polyline rows of the replay, or of the game's roads-guidance.txt."""
    for line in lines:
        parts = line.split()
        if parts[0] == "d":
            values = parts[1:]
            if len(values) == 11:  # the game's file: elapsed first, then the diagnosis
                values = [values[1], values[2], values[4], *values[7:11], values[3], values[5], values[6]]
            progress, rebuilt, beyond, cx, cy, cz, remaining, caught, skipped, fallback = values
            current["decisions"].append(dict(progress=float(progress), rebuilt=rebuilt == "1", beyond=beyond == "1",
                                             carrot=(float(cx), float(cy), float(cz)), remaining=float(remaining),
                                             caught=caught == "1", skipped=int(skipped), fallback=int(fallback)))
        elif parts[0] in ("polyline", "p"):
            values = list(map(float, parts[2:]))
            current["polylines"][len(current["decisions"]) - 1] = [values[i:i + 3] for i in range(0, len(values), 3)]


def recorded(episodes):
    """The guidance the game wrote with its own memory."""
    results = []
    for ep in episodes:
        path = Path(ep["directory"]) / "roads-guidance.txt"
        current = dict(known=tuple(ep["known"]), decisions=[], polylines={})
        parse_guidance(path.read_text().splitlines(), current)
        if len(current["decisions"]) != len(ep["positions"]):
            raise ValueError(f"{path}: {len(current['decisions'])} guidance rows for {len(ep['positions'])} decisions")
        results.append(current)
    return results


def replay(binary, graph, episodes, navmesh=None, passage="never"):
    rows = [graph]
    for index, ep in enumerate(episodes):
        rows.append(f"episode {index} {int(ep['stranger'])} {len(ep['routine'])} "
                    + " ".join(map(str, ep["routine"])) + " "
                    + " ".join(repr(float(v)) for v in (*ep["spawn"], *ep["goal"]))
                    + f" {len(ep['positions'])}")
        rows += [f"{x!r} {y!r} {z!r}" for x, y, z in ep["positions"]]
    run = subprocess.run([str(binary), str(navmesh) if navmesh else "-", passage], input="\n".join(rows) + "\n",
                         capture_output=True, text=True, check=True)
    results, current, block = [], None, []
    for line in run.stdout.splitlines():
        if line.startswith("episode "):
            if current is not None:
                parse_guidance(block, current)
            parts = line.split()
            current, block = dict(known=(int(parts[2]), int(parts[3])), decisions=[], polylines={}), []
            results.append(current)
        else:
            block.append(line)
    if current is not None:
        parse_guidance(block, current)
    if len(results) != len(episodes):
        raise ValueError(f"replay returned {len(results)} episodes for {len(episodes)}")
    return results


def jumps(decisions, threshold, caught=False):
    """Single-decision progress growth beyond threshold on the same route; catch-ups apart."""
    found = []
    for index in range(1, len(decisions)):
        step = decisions[index]["progress"] - decisions[index - 1]["progress"]
        if (not decisions[index]["rebuilt"] and decisions[index].get("caught", False) == caught
                and step > threshold + EPSILON_CM):
            found.append(dict(decision=index, step_cm=round(step, 3)))
    return found


def initial_skips(result, positions, threshold, caught=False):
    """Rebuilds whose projection is farther along than the first node is from the actor, plus threshold."""
    found = []
    for index, line in result["polylines"].items():
        progress = result["decisions"][index]["progress"]
        offset = math.dist(positions[index], line[0])
        if result["decisions"][index].get("caught", False) == caught and progress > offset + threshold + EPSILON_CM:
            found.append(dict(decision=index, progress_cm=round(progress, 3), start_offset_cm=round(offset, 3)))
    return found


def creep(decisions, positions, threshold, span=10):
    """Decisions where one second of progress (no rebuild or catch-up) exceeds the actor's XZ travel plus threshold."""
    count = 0
    for index in range(span, len(decisions)):
        if any(decisions[i]["rebuilt"] or decisions[i].get("caught", False) for i in range(index - span + 1, index + 1)):
            continue
        travel = sum(math.dist((positions[i][0], positions[i][2]), (positions[i - 1][0], positions[i - 1][2]))
                     for i in range(index - span + 1, index + 1))
        if decisions[index]["progress"] - decisions[index - span]["progress"] > travel + threshold + EPSILON_CM:
            count += 1
    return count


def project(line, position):
    """Distance from position to the whole polyline and the arc length of the closest point."""
    best, along = None, 0.
    for a, b in zip(line, line[1:]):
        edge = [b[k] - a[k] for k in range(3)]
        squared = sum(value * value for value in edge)
        length = math.sqrt(squared)
        t = min(max(sum((position[k] - a[k]) * edge[k] for k in range(3)) / squared, 0.), 1.) if squared else 0.
        distance = math.dist([a[k] + edge[k] * t for k in range(3)], position)
        if best is None or distance < best[0]:
            best = (distance, along + t * length)
        along += length
    return best


def lag(result, positions, threshold, near_cm=300.):
    """Decisions where the actor is near a later stretch that progress has not taken."""
    count, line = 0, None
    for index, decision in enumerate(result["decisions"]):
        if decision["rebuilt"]:
            line = result["polylines"][index]
            continue
        if line is None or len(line) < 2 or decision["beyond"]:
            continue
        distance, along = project(line, positions[index])
        count += distance <= near_cm and along - decision["progress"] > threshold + EPSILON_CM
    return count


def point_at(line, distance):
    """KnownRoads pointAt: the point `distance` along the polyline."""
    for a, b in zip(line, line[1:]):
        length = math.dist(a, b)
        if length > 0 and distance < length:
            return [a[k] + (b[k] - a[k]) * distance / length for k in range(3)]
        distance -= length
    return list(line[-1])


def oracle_queries(result, positions, threshold):
    """Segments for the navmesh oracle: actor to progress point every decision, actor to a rebuilt start."""
    queries, line = [], None
    for index, decision in enumerate(result["decisions"]):
        if decision["rebuilt"]:
            line = result["polylines"][index]
            queries.append(("start", index, positions[index], line[0]))
        if line and not decision["beyond"]:
            queries.append(("progress", index, positions[index], point_at(line, decision["progress"])))
    return queries


def oracle_answers(binary, navmesh, queries):
    if not queries:
        return []
    rows = [f"segments {len(queries)}"] + [" ".join(repr(float(v)) for v in (*a, *b)) for _, _, a, b in queries]
    run = subprocess.run([str(binary), str(navmesh), "never"], input="\n".join(rows) + "\n",
                         capture_output=True, text=True, check=True)
    answers = [int(line.split()[1]) for line in run.stdout.splitlines()]
    if len(answers) != len(queries):
        raise ValueError(f"oracle answered {len(answers)} of {len(queries)} segments")
    return answers


def oracle_metrics(result, positions, threshold, answers_by_query):
    """Advances, every decision's progress and rebuilt starts against the navmesh boundary."""
    wall = {(kind, index): answer for (kind, index), answer in answers_by_query}
    advances = {j["decision"] for j in jumps(result["decisions"], threshold)}
    advances |= {j["decision"] for j in jumps(result["decisions"], threshold, caught=True)}
    advances |= {s["decision"] for s in initial_skips(result, positions, threshold)}
    advances |= {s["decision"] for s in initial_skips(result, positions, threshold, caught=True)}
    through = sorted(index for index in advances if wall.get(("progress", index)) == 1)
    starts = [index for index, d in enumerate(result["decisions"]) if d["rebuilt"] and ("start", index) in wall]
    # A new start: the first rebuild, or one whose first node differs from the previous rebuild's.
    events, previous = [], None
    for index in starts:
        first = tuple(result["polylines"][index][0]) if result["polylines"][index] else None
        if first != previous:
            events.append(index)
        previous = first
    return dict(advances=len(advances), advances_through_wall=len(through), advances_through_wall_at=through,
                start_events=len(events), start_events_through_wall=sum(wall[("start", i)] == 1 for i in events),
                start_events_through_wall_at=[i for i in events if wall[("start", i)] == 1],
                progress_behind_wall=sum(1 for (kind, _), answer in wall.items() if kind == "progress" and answer == 1),
                progress_off_mesh=sum(1 for (kind, _), answer in wall.items() if kind == "progress" and answer < 0),
                starts=len(starts), starts_through_wall=sum(wall[("start", i)] == 1 for i in starts),
                starts_through_wall_at=[i for i in starts if wall[("start", i)] == 1],
                starts_off_mesh=sum(wall[("start", i)] < 0 for i in starts))


def summarize(episodes, replays, threshold, oracle=None):
    names = list(replays)
    per_episode = []
    totals = {name: dict(decisions=0, jumps=0, episodes_with_jump=0, initial_skips=0, episodes_with_initial_skip=0,
                         catch_ups=0, catch_ups_beyond_window=0, episodes_with_catch_up=0, starts_moved=0,
                         start_fallbacks=0, start_departures=0, rebuilds=0, creep_decisions=0, lag_decisions=0, max_jump_cm=0.,
                         recorded_carrot_max_error_cm=None)
              for name in names}
    if oracle is not None:
        for total in totals.values():
            total.update(advances=0, advances_through_wall=0, progress_behind_wall=0, progress_off_mesh=0,
                         starts=0, starts_through_wall=0, starts_off_mesh=0, start_events=0,
                         start_events_through_wall=0)
        answers = {}
        for name in names:
            queries = [(index, query) for index, ep in enumerate(episodes)
                       for query in oracle_queries(replays[name][index], ep["positions"], threshold)]
            values = oracle_answers(*oracle, [query for _, query in queries])
            for (index, (kind, decision, _, _)), value in zip(queries, values, strict=True):
                answers.setdefault((name, index), []).append(((kind, decision), value))
    same = {name: dict(decisions=0, carrot_within_1cm=0, clean_episodes=0, clean_decisions=0, clean_within_1cm=0,
                       clean_differences=[], differs_before_memory=[])
            for name in names[1:]}
    for index, ep in enumerate(episodes):
        row = dict(id=ep["id"], success=ep["success"], elapsed_ms=ep["elapsed_ms"], decisions=len(ep["positions"]))
        for name in names:
            result = replays[name][index]
            if result["known"] != tuple(ep["known"]):
                raise ValueError(f"{ep['id']}: rebuilt knowledge {result['known']} differs from recorded {ep['known']}")
            decisions = result["decisions"]
            caught = jumps(decisions, threshold, caught=True)
            row[name] = dict(jumps=jumps(decisions, threshold),
                             initial_skips=initial_skips(result, ep["positions"], threshold),
                             catch_ups=[i for i, d in enumerate(decisions) if d.get("caught")],
                             catch_ups_beyond_window=caught + initial_skips(result, ep["positions"], threshold, True),
                             starts_moved=[i for i, d in enumerate(decisions) if d["rebuilt"] and d.get("skipped")],
                             start_fallbacks=[i for i, d in enumerate(decisions) if d["rebuilt"] and d.get("fallback")],
                             start_departures=[i for i, d in enumerate(decisions) if d["rebuilt"] and d.get("fallback") == 2],
                             rebuilds=sum(d["rebuilt"] for d in decisions),
                             creep_decisions=creep(decisions, ep["positions"], threshold),
                             lag_decisions=lag(result, ep["positions"], threshold))
            if oracle is not None:
                row[name]["oracle"] = oracle_metrics(result, ep["positions"], threshold, answers.get((name, index), []))
                for key, value in row[name]["oracle"].items():
                    if not key.endswith("_at"):
                        totals[name][key] += value
            total = totals[name]
            total["catch_ups"] += len(row[name]["catch_ups"])
            total["catch_ups_beyond_window"] += len(row[name]["catch_ups_beyond_window"])
            total["episodes_with_catch_up"] += bool(row[name]["catch_ups"])
            total["starts_moved"] += len(row[name]["starts_moved"])
            total["start_fallbacks"] += len(row[name]["start_fallbacks"])
            total["start_departures"] += len(row[name]["start_departures"])
            total["decisions"] += len(result["decisions"])
            total["jumps"] += len(row[name]["jumps"])
            total["episodes_with_jump"] += bool(row[name]["jumps"])
            total["initial_skips"] += len(row[name]["initial_skips"])
            total["episodes_with_initial_skip"] += bool(row[name]["initial_skips"])
            total["rebuilds"] += row[name]["rebuilds"]
            total["creep_decisions"] += row[name]["creep_decisions"]
            total["lag_decisions"] += row[name]["lag_decisions"]
            total["max_jump_cm"] = max(total["max_jump_cm"], max((j["step_cm"] for j in row[name]["jumps"]), default=0.))
            if "recorded_carrot" in ep:
                error = max(math.dist(a["carrot"], b)
                            for a, b in zip(result["decisions"], ep["recorded_carrot"], strict=True))
                row[name]["recorded_carrot_max_error_cm"] = error
                total["recorded_carrot_max_error_cm"] = max(total["recorded_carrot_max_error_cm"] or 0., error)
        first = replays[names[0]][index]["decisions"]
        for name in names[1:]:
            second = replays[name][index]["decisions"]
            close = [math.dist(a["carrot"], b["carrot"]) <= 1. for a, b in zip(first, second, strict=True)]
            different = close.index(False) if not all(close) else None
            row[name].update(carrot_within_1cm=sum(close), first_different_decision=different)
            # Before its memory first acts, a revision with a passage check is the window rule.
            memory = next((i for i, d in enumerate(second) if d.get("caught") or d.get("skipped") or d.get("fallback")),
                          None)
            row[name]["first_memory_decision"] = memory
            value = same[name]
            if different is not None and (memory is None or different < memory):
                value["differs_before_memory"].append(dict(id=ep["id"], first=different, memory=memory))
            value["decisions"] += len(close)
            value["carrot_within_1cm"] += sum(close)
            if not row[names[0]]["jumps"] and not row[names[0]]["initial_skips"]:
                value["clean_episodes"] += 1
                value["clean_decisions"] += len(close)
                value["clean_within_1cm"] += sum(close)
                if not all(close):
                    value["clean_differences"].append(dict(id=ep["id"], decisions=len(close) - sum(close),
                                                           first=different))
        per_episode.append(row)
    for total in totals.values():
        total["jump_ratio"] = total["jumps"] / total["decisions"] if total["decisions"] else 0.
    return dict(episodes=len(episodes), totals=totals, same_carrot=same, per_episode=per_episode)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--graph", required=True, type=Path)
    parser.add_argument("--revision", action="append", default=[], metavar="NAME=GIT_REVISION|WORKTREE|DIRECTORY")
    parser.add_argument("--passage", action="append", default=[], metavar="NAME=never|oracle",
                        help="the walker's memory for a revision with a passage check (default never)")
    parser.add_argument("--recorded", metavar="NAME", help="also read the game's own roads-guidance.txt as NAME")
    parser.add_argument("--navmesh", type=Path, help="the stand's OGNAV001 cache: oracle metrics and passage")
    parser.add_argument("--set", action="append", required=True, metavar="NAME=ROOT")
    parser.add_argument("--jump-cm", type=float, required=True, help="progress growth per decision counted as a jump")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    graph, data = graph_text(args.graph)
    revisions = dict(item.split("=", 1) for item in args.revision)
    passages = dict(item.split("=", 1) for item in args.passage)
    if set(passages) - set(revisions) or set(passages.values()) - {"never", "oracle"}:
        raise ValueError("--passage names a revision and never|oracle")
    if "oracle" in passages.values() and args.navmesh is None:
        raise ValueError("--passage oracle needs --navmesh")
    if not revisions and not args.recorded:
        raise ValueError("give at least one --revision or --recorded")
    sets = dict(item.split("=", 1) for item in args.set)
    report = dict(graph=dict(nodes=len(data["nodes"]), edges=len(data["edges"])), jump_cm=args.jump_cm,
                  revisions={name: revision if revision == "WORKTREE" or Path(revision).is_dir() else subprocess.run(
                      ["git", "-C", str(ROOT), "rev-parse", revision], check=True, capture_output=True,
                      text=True).stdout.strip() for name, revision in revisions.items()},
                  passages={name: passages.get(name, "never") for name in revisions}, recorded=args.recorded,
                  navmesh=str(args.navmesh) if args.navmesh else None, sets={})
    with tempfile.TemporaryDirectory() as scratch:
        binaries = {name: compile_replay(revision, Path(scratch), args.navmesh) for name, revision in revisions.items()}
        oracle = (compile_replay("WORKTREE", Path(scratch), args.navmesh), args.navmesh) if args.navmesh else None
        for label, root in sets.items():
            episodes = collect(root)
            replays = {name: replay(binary, graph, episodes, args.navmesh, passages.get(name, "never"))
                       for name, binary in binaries.items()}
            if args.recorded:
                replays[args.recorded] = recorded(episodes)
            report["sets"][label] = dict(root=str(root), **summarize(episodes, replays, args.jump_cm, oracle))
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    for label, value in report["sets"].items():
        print(json.dumps(dict(set=label, episodes=value["episodes"], totals=value["totals"],
                              same_carrot={name: {k: v for k, v in same.items()
                                                  if k not in ("clean_differences", "differs_before_memory")}
                                           | dict(clean_differences=len(same["clean_differences"]),
                                                  differs_before_memory=len(same["differs_before_memory"]))
                                           for name, same in value["same_carrot"].items()})))


if __name__ == "__main__":
    sys.exit(main())
