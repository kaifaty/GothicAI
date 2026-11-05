#!/usr/bin/env python3
"""Follow along the leader's trail: the teacher and a policy, trail versus line carrot.

Ryzen, one gq job each (scripts/nav-follow-remote.sh):

    python3 -I scripts/nav_follow_trail.py validation --model teacher|/abs/v5.navbc --guidance trail|line \\
        --output /abs/archive/validation/NAME --runner RUNNER --assets DIR --save SAVE --links LINKS \\
        --roads ROADS --cache /abs/task/navmesh --manifest MANIFEST --rejections DIR --parallel 12
    python3 scripts/nav_follow_trail.py live --guidance trail|line --suite SUITE --binary GAME \\
        --runtime /abs/task --output /abs/archive/live --weights /abs/v5.navbc
    python3 -I scripts/nav_follow_trail.py report --root /abs/archive

Validation: the frozen follow_below_gate routes, action seed 18102027, CV physics,
240 s. Live: the frozen 03-follow-hero place on its three seeds, the teacher and
the policy. `navdemos follow-guidance line` is the previous straight carrot and
teacher pace. Nothing is recorded as a demonstration and nothing is trained.

Every number comes from the stand's episode.json. After the first contact (<=5 m):
the share of time in the 2-5 m band, beyond 5 m and nearer than 2 m. Contacts are
rising edges of native CV blocked/steep contact between decisions, whole run.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from copy import copy, deepcopy
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import sys
import threading
import time
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

ACTION_SEED = 18102027
FAMILY = "follow_below_gate"
LIVE_PLACE = "03-follow-hero"
QUALIFICATION = .8
RUN = dict(run="on", own="off")  # teacher pace -> navdemos follow-run
# Decisions changed by the review fixes (loop standoff, unseen carrot) and the 2.5 m guard.
FIX_COUNTERS = ("loop_limited_decisions", "loop_line_decisions", "unseen_detour_decisions", "guarded_decisions")
SOURCES = ("hold", "direct", "cut", "trail", "line", "unseen", "heading", "aside", "last_seen")


def read_json(path):
    return json.loads(Path(path).read_text())


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(f"{path.suffix}.{os.getpid()}.{threading.get_ident()}.tmp")
    temporary.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n")
    temporary.replace(path)


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def episode_follow(directory):
    """Follow facts of one stand episode; walk share from the applied keys."""
    directory = Path(directory)
    episode = read_json(directory / "episode.json")
    if episode.get("goal_kind") != "follow":
        raise ValueError(f"Not a follow episode: {directory}")
    for key in ("follow_far_ms", "follow_near_ms", "follow_guidance", "follow_contact_entries"):
        if key not in episode:
            raise ValueError(f"Episode lacks {key}; built before the follow trail stand: {directory}")
    walk = decisions = 0
    for line in (directory / "actions.txt").read_text().splitlines()[:-1]:  # last row: terminal reset
        fields = line.split()
        decisions += 1
        walk += fields[4] != "0"
    observed = episode["follow_ms"]
    return dict(directory=str(directory), success=bool(episode["success"]), failure=episode.get("failure"),
                elapsed_ms=episode["elapsed_ms"], decisions=episode["decisions"],
                first_contact_ms=None if episode["first_contact_ms"] < 0 else episode["first_contact_ms"],
                follow_ms=observed, in_band_ms=episode["in_band_ms"], far_ms=episode["follow_far_ms"],
                near_ms=episode["follow_near_ms"],
                band=episode["in_band_ms"] / observed if observed else None,
                far=episode["follow_far_ms"] / observed if observed else None,
                contacts=episode["follow_contact_entries"], contact_decisions=episode["follow_contact_decisions"],
                visible_decisions=episode["follow_visible_decisions"], sources=episode["follow_carrot_sources"],
                **{key: episode.get(f"follow_{key}") for key in FIX_COUNTERS},
                guidance=episode["follow_guidance"], teacher_run=episode.get("follow_run"),
                walk_decisions=walk, action_rows=decisions,
                travel_cm=episode.get("travel_cm"), pace=episode.get("pace"),
                base_fingerprint=episode.get("base_fingerprint"))


def group(rows):
    """Pool time over episodes; per-episode qualification at the 80% band threshold."""
    observed = sum(row["follow_ms"] for row in rows)
    elapsed = sum(row["elapsed_ms"] for row in rows)
    decisions = sum(row["decisions"] for row in rows)
    sources = {name: sum(row["sources"].get(name, 0) for row in rows) for name in SOURCES}
    contacted = [row for row in rows if row["first_contact_ms"] is not None]
    return dict(episodes=len(rows), contacted=len(contacted),
                qualified=sum(row["band"] is not None and row["band"] >= QUALIFICATION for row in rows),
                band=sum(row["in_band_ms"] for row in rows) / observed if observed else None,
                far=sum(row["far_ms"] for row in rows) / observed if observed else None,
                near=sum(row["near_ms"] for row in rows) / observed if observed else None,
                follow_ms=observed, elapsed_ms=elapsed,
                contacts=sum(row["contacts"] for row in rows),
                contacts_per_minute=60000 * sum(row["contacts"] for row in rows) / elapsed if elapsed else None,
                visible=sum(row["visible_decisions"] for row in rows) / decisions if decisions else None,
                walk_decisions=sum(row["walk_decisions"] for row in rows), decisions=decisions,
                sources={name: value / decisions if decisions else None for name, value in sources.items()},
                source_decisions=sources,
                **{key: sum(row.get(key) or 0 for row in rows) for key in FIX_COUNTERS})


def paired(before, after, key):
    """Mean and SE of per-episode differences after - before over shared IDs."""
    left = {row["id"]: row for row in before}
    right = {row["id"]: row for row in after}
    shared = sorted(identifier for identifier in left.keys() & right.keys()
                    if left[identifier][key] is not None and right[identifier][key] is not None)
    values = [right[identifier][key] - left[identifier][key] for identifier in shared]
    if not values:
        return dict(n=0, delta=None, se=None)
    mean = sum(values) / len(values)
    variance = sum((value - mean) ** 2 for value in values) / (len(values) - 1) if len(values) > 1 else 0
    return dict(n=len(values), delta=mean, se=math.sqrt(variance / len(values)))


def validation_commands(args, value, teacher):
    import nav_rl as rl
    if args.limit_ms is not None:  # a short smoke only; the frozen routes keep their own horizon
        value = dict(value, limit_ms=args.limit_ms)
    worker = SimpleNamespace(npc=args.npc, roads=args.roads, weights=args.model, lean=True,
                             familiar_map=None, stranger=False, limit_ms=value.get("limit_ms", 240000))
    commands = rl.policy_commands(worker, value, ACTION_SEED)
    moving = [index for index, command in enumerate(commands) if command.startswith("navdemos drive-moving ")]
    if len(moving) != 1 or "navdemos record off" not in commands:
        raise ValueError(f"Unexpected follow validation commands for {value['id']}")
    if teacher:
        route = ",".join(value["route"])
        commands[moving[0]] = (f"navdemos moving-v3 3 {args.npc} {value['start']} {value['target']} {route} "
                               f"follow {value.get('limit_ms', 240000)} {value['seed']}")
    commands[moving[0]:moving[0]] = [f"navdemos follow-guidance {args.guidance}",
                                      f"navdemos follow-run {RUN[args.teacher_pace]}"]
    return commands


def validation(args):
    import nav_rl as rl
    teacher = args.model == "teacher"
    if not teacher:
        args.model = Path(args.model).resolve()
        if not args.model.is_file():
            raise ValueError("--model must be teacher or a weight file")
        # A companion always runs; v5 reads the pace as an input.
        os.environ["OPENGOTHIC_NAV_FORCE_PACE"] = "run"
    else:
        os.environ.pop("OPENGOTHIC_NAV_FORCE_PACE", None)
    values = rl.validation_scenarios(args.manifest, FAMILY)
    if args.first is not None:
        values = values[:args.first]
    if args.ids:
        wanted = args.ids.split(",")
        values = [value for value in values if value["id"] in wanted]
        if len(values) != len(wanted):
            raise ValueError("--ids names routes outside the frozen follow validation")
    workers = min(args.parallel, len(values))
    started = time.monotonic()
    args.output.mkdir(parents=True, exist_ok=True)
    identity = dict(model="teacher" if teacher else str(args.model),
                    model_sha256=None if teacher else sha256(args.model), guidance=args.guidance,
                    teacher_pace=args.teacher_pace,
                    runner_sha256=sha256(args.runner), manifest=str(args.manifest), action_seed=ACTION_SEED,
                    base_fingerprint=os.environ.get("OPENGOTHIC_BASE_FINGERPRINT"))
    if (args.output / "identity.json").is_file() and read_json(args.output / "identity.json") != identity:
        raise ValueError(f"Refusing different inputs at {args.output}")
    write_json(args.output / "identity.json", identity)

    def collect(index):
        worker = copy(args)
        worker.output = args.output / "workers" / str(index)
        worker.cache = args.cache / str(index)
        worker.cache.mkdir(parents=True, exist_ok=True)
        worker.seed = ACTION_SEED
        runner = rl.new_runner(worker, "follow")
        try:
            for value in values[index::workers]:
                directory = args.output / "episodes" / value["id"]
                if (args.rejections / f"{value['id']}.json").is_file() or (directory / "follow.json").is_file():
                    continue
                if directory.exists():
                    shutil.rmtree(directory)
                worker.seed = value["seed"]
                try:
                    rl.scenario(worker, runner, f"follow-{value['id']}", validation_commands(args, value, teacher),
                                directory)
                except RuntimeError as error:
                    if str(error) != "Runner rejected advance: navdemos moving route needs distinct connected points":
                        raise
                    write_json(args.rejections / f"{value['id']}.json",
                               dict(id=value["id"], family=FAMILY, validation_scenario=value, reason=str(error)))
                    runner.close()
                    runner = rl.new_runner(worker, "follow")
                    continue
                episode = read_json(directory / "episode.json")
                episode.update(id=value["id"], base_fingerprint=os.environ.get("OPENGOTHIC_BASE_FINGERPRINT"))
                write_json(directory / "episode.json", episode)
                if not args.keep_traces:
                    rl.clean_lean_diagnostics(directory)
                row = dict(episode_follow(directory), id=value["id"])
                if row["guidance"] != args.guidance or (row["walk_decisions"] and not teacher) or episode.get("record"):
                    raise ValueError(f"Episode guidance, pace or recording differs: {directory}")
                write_json(directory / "follow.json", row)
                print(json.dumps(dict(id=value["id"], band=row["band"], far=row["far"], contacts=row["contacts"])),
                      flush=True)
        finally:
            runner.close()
        shutil.rmtree(worker.output, ignore_errors=True)

    with ThreadPoolExecutor(max_workers=workers) as pool:
        for future in [pool.submit(collect, index) for index in range(workers)]:
            future.result()
    rows = [read_json(path) for path in sorted((args.output / "episodes").glob("*/follow.json"))]
    rejected = sorted(path.stem for path in args.rejections.glob("*.json"))
    if len(rows) + len([name for name in rejected if any(value["id"] == name for value in values)]) != len(values):
        raise ValueError(f"Incomplete follow validation: {len(rows)} episodes of {len(values)}")
    write_json(args.output / "summary.json", dict(identity, seconds=time.monotonic() - started, rejected=rejected,
                                                    all=group(rows), episodes=rows))
    print(json.dumps(dict(event="follow_validation_complete", **group(rows))), flush=True)


def live(args):
    import nav_live
    original = read_json(args.suite)
    cases = {case["id"]: case for case in nav_live.suite_cases(original)}
    case = deepcopy(cases[LIVE_PLACE])
    case.pop("evaluation_binary", None)
    case.pop("teacher_reference", None)
    case["pace"] = "run"
    case["prepare_commands"] = [*case.get("prepare_commands", []), f"navdemos follow-guidance {args.guidance}",
                                f"navdemos follow-run {RUN[args.teacher_pace]}"]
    inputs = dict(original["inputs"], cache=str(args.runtime / "live-navmesh"))
    for name in ("save", "roads", "familiar", "links"):
        if not Path(inputs[name]).is_file():
            raise ValueError(f"Missing frozen live input: {inputs[name]}")
    if not os.environ.get("OPENGOTHIC_BASE_FINGERPRINT"):
        raise ValueError("Live follow requires the current base fingerprint")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    state = SimpleNamespace(suite=args.suite, output=output, binary=args.binary.resolve(), inputs=inputs,
                            deadline=time.monotonic() + args.budget_seconds,
                            input_sha256={name: nav_live.sha256(inputs[name]) for name in ("save", "roads", "familiar", "links")})
    model = dict(name="v5", weights=str(args.weights), sha256=nav_live.sha256(args.weights), order=0,
                 force_place_pace=True, **nav_live.pace_input(args.weights))
    if model.get("pace_input") is not True:
        raise ValueError("Live follow compares the pace-input v5 policy")
    for seed in case["seeds"]:
        teacher = output / args.guidance / "teacher" / f"seed-{seed}"
        # The frozen place keeps its scenario seed; the run seed varies as in pace-v5.
        nav_live.run_episode(state, case, None, seed, teacher)
        nav_live.run_episode(state, case, model, seed, output / args.guidance / "v5" / f"seed-{seed}", teacher)
    rows = []
    for name in ("teacher", "v5"):
        for seed in case["seeds"]:
            directory = output / args.guidance / name / f"seed-{seed}"
            summary = read_json(directory / "summary.json")
            row = dict(episode_follow(directory), id=f"seed-{seed}", model=name, seed=seed,
                       obstacle_contacts=summary["obstacle_contacts"],
                       whole_band=summary["follow_band_fraction"], video=str(directory / "source.mp4"))
            if row["guidance"] != args.guidance or row["walk_decisions"] and (name == "v5" or args.teacher_pace == "run"):
                raise ValueError(f"Live follow guidance or pace differs: {directory}")
            rows.append(row)
    write_json(output / f"{args.guidance}-summary.json",
               dict(place=LIVE_PLACE, guidance=args.guidance, binary_sha256=nav_live.sha256(args.binary),
                    base_fingerprint=os.environ["OPENGOTHIC_BASE_FINGERPRINT"], weights_sha256=model["sha256"],
                    rows=rows))
    print(json.dumps(dict(event="follow_live_complete", guidance=args.guidance, runs=len(rows))), flush=True)


def report(args):
    root = args.root
    result = dict(validation={}, live={}, paired={})
    for name in ("teacher-line", "teacher-trail-attempt1", "teacher-trail",
                 "v5-line-premerge", "v5-trail-attempt1", "v5-trail-attempt2", "v5-line", "v5-trail"):
        path = root / "validation" / name / "summary.json"
        if path.is_file():
            result["validation"][name] = read_json(path)
    for guidance in ("line", "trail-attempt1", "trail"):
        path = root / "live" / f"{guidance}-summary.json"
        if path.is_file():
            value = read_json(path)
            result["live"][guidance] = {model: dict(group([row for row in value["rows"] if row["model"] == model]),
                                                    seeds=[row for row in value["rows"] if row["model"] == model])
                                        for model in ("teacher", "v5")}
    for model in ("teacher", "v5"):
        before, after = (result["validation"].get(f"{model}-{guidance}") for guidance in ("line", "trail"))
        if before and after:
            result["paired"][model] = {key: paired(before["episodes"], after["episodes"], key)
                                       for key in ("band", "far", "contacts")}
    write_json(root / "report.json", {**result, "validation": {name: {key: value for key, value in summary.items()
                                                                      if key != "episodes"}
                                                               for name, summary in result["validation"].items()}})
    print(markdown(result))


def markdown(result):
    """Report tables: pooled shares after first contact, contacts, carrot sources."""
    percent = lambda value: "—" if value is None else f"{100 * value:.1f}%".replace(".", ",")
    header = ("| Конфигурация | Эпизодов | Со сближением | ≥80% в полосе | Полоса 2–5 м | Дальше 5 м | Ближе 2 м "
              "| Контакты | Контактов в минуту | Виден |\n|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    def row(name, value):
        return (f"| {name} | {value['episodes']} | {value['contacted']} | {value['qualified']} | {percent(value['band'])} "
                f"| {percent(value['far'])} | {percent(value['near'])} | {value['contacts']} "
                f"| {value['contacts_per_minute']:.2f} | {percent(value['visible'])} |")
    lines = ["Validation follow:", header]
    lines += [row(name, summary["all"]) for name, summary in result["validation"].items()]
    for guidance, models in result["live"].items():
        lines += ["", f"03-follow-hero, {guidance}:", header]
        lines += [row(model, value) for model, value in models.items()]
        lines += ["", "| Модель | Seed | Полоса 2–5 м | Дальше 5 м | Контакты | Полоса за весь горизонт |", "|---|---:|---:|---:|---:|---:|"]
        for model, value in models.items():
            lines += [f"| {model} | {seed['seed']} | {percent(seed['band'])} | {percent(seed['far'])} | {seed['contacts']} "
                      f"| {percent(seed['whole_band'])} |" for seed in value["seeds"]]
    lines += ["", "Источники морковки (доля решений):", "| Конфигурация | " + " | ".join(SOURCES) + " |",
              "|---|" + "---:|" * len(SOURCES)]
    for name, summary in result["validation"].items():
        lines.append(f"| {name} | " + " | ".join(percent(summary["all"]["sources"][source]) for source in SOURCES) + " |")
    lines += ["", "Парные разности след − прямая по маршрутам (среднее ± SE):"]
    for model, metrics in result["paired"].items():
        lines.append(f"- {model}: " + "; ".join(f"{key} {value['delta']:+.4f} ± {value['se']:.4f} (n={value['n']})"
                                                 for key, value in metrics.items() if value["n"]))
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("validation", "live", "report"))
    parser.add_argument("--model", help="teacher or a .navbc weight file (validation)")
    parser.add_argument("--guidance", choices=("trail", "line"))
    parser.add_argument("--teacher-pace", choices=("run", "own"),
                        help="Follow teacher: always run, or its previous own pace; default run for trail, own for line")
    parser.add_argument("--npc", default="BAU_954_MALETH")
    parser.add_argument("--limit-ms", type=int, help="Override the route horizon (smoke only)")
    parser.add_argument("--first", type=int, help="Only the first N frozen routes (smoke only)")
    parser.add_argument("--ids", help="Only these frozen route IDs (diagnosis)")
    parser.add_argument("--keep-traces", action="store_true", help="Keep teacher paths and frames (diagnosis)")
    parser.add_argument("--parallel", type=int, default=12)
    parser.add_argument("--physics", default="cv")
    parser.add_argument("--watchdog-seconds", type=float, default=1800)
    parser.add_argument("--budget-seconds", type=float, default=6600)
    parser.add_argument("--ini", type=Path)
    for name in ("output", "runner", "assets", "save", "links", "roads", "cache", "manifest", "rejections",
                 "suite", "binary", "runtime", "weights", "root"):
        parser.add_argument("--" + name, type=Path)
    args = parser.parse_args(argv)
    if args.teacher_pace is None:
        args.teacher_pace = "run" if args.guidance == "trail" else "own"
    for key, value in vars(args).items():
        if isinstance(value, Path):
            setattr(args, key, value.expanduser().resolve())
    required = dict(validation=("model", "guidance", "output", "runner", "assets", "save", "links", "roads", "cache",
                                "manifest", "rejections"),
                    live=("guidance", "suite", "binary", "runtime", "output", "weights"), report=("root",))
    missing = [name for name in required[args.command] if getattr(args, name) is None]
    if missing:
        parser.error("missing --" + ", --".join(name.replace("_", "-") for name in missing))
    {"validation": validation, "live": live, "report": report}[args.command](args)


if __name__ == "__main__":
    main()
