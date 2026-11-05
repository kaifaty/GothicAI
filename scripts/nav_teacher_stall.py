#!/usr/bin/env python3
"""The memory teacher's dead end without a start cell: measure it, and its body exit.

Ryzen, one gq job each (scripts/nav-teacher-stall-remote.sh):

    python3 -I scripts/nav_teacher_stall.py validation --model teacher|/abs/v5.navbc --families follow_below_gate \\
        --exit on|off --lost aside|last-seen [--pace own|walk|run] --output /abs/archive/NAME --runner RUNNER \\
        --assets DIR --save SAVE --links LINKS --roads ROADS --cache /abs/task/navmesh --manifest MANIFEST \\
        --rejections DIR [--detour /abs/detour-val.jsonl] --parallel 12
    python3 -I scripts/nav_teacher_stall.py stalls --root /abs/archive/diagnose/NAME
    python3 -I scripts/nav_teacher_stall.py report --root /abs/archive
    python3 -I scripts/nav_teacher_stall.py contacts --root /abs/archive

Validation: the frozen validation routes (follow_below_gate, point, object, approach)
and the detour cohort, action seed 18102027, CV physics, gaze, nothing recorded.
`--exit off` (`navdemos teacher-exit off`) is the teacher without its body exit,
`--lost last-seen` (`navdemos follow-lost last-seen`) the follow carrot that stands
at the last seen point: both are the behaviour of main before this change, on the
same runner. The teacher's pace off follow is forced with `navdemos teacher-pace`;
walk routes get the horizon x3.064158265, as in the pace-v5 comparison.

Every number comes from the stand's episode.json: success and elapsed time, the
teacher's decisions without a start cell, its rules over all decisions and those
standing (no forward or strafe key) far from the goal (follow: beyond 5 m,
otherwise beyond 2 m); for follow also the band, beyond 5 m and contacts after
nav_follow_trail.py.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from copy import copy
import json
import math
import os
from pathlib import Path
import random
import shutil
import statistics
import sys
import time
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import nav_follow_trail as follow  # noqa: E402

ACTION_SEED = follow.ACTION_SEED
FAMILIES = ("point", "object", "approach", "follow_below_gate", "detour")
WALK_FACTOR = 3.064158265
STALL_DECISIONS = 200  # an episode "stuck" without a start cell
# Rules of NavMemoryTeacher::decide grouped for the report.
REASONS = {
    "start_not_found": ("start_not_found",),
    "danger": ("escape_blocked", "recovery_blocked", "behind_blocked", "forward_blocked", "side_blocked"),
    "cautious": ("standoff",),
    "turn": ("turn",),
    "other": ("airborne", "at_goal", "no_aim", "escape", "body_exit", "recovery", "behind", "strafe", "forward"),
}


def read_json(path):
    return json.loads(Path(path).read_text())


def teacher_command(value, npc, limit):
    """The memory teacher on the frozen scenario (as nav_teacher_v3.start_command)."""
    kind = value["goal_kind"]
    if kind == "point":
        return f"navdemos start-v3 {npc} {value['start']} {value['goal']} {limit}"
    if kind == "object":
        return f"navdemos object-v3 3 {npc} {value['start']} {value['goal']} {limit}"
    route = ",".join(value["route"])
    return f"navdemos moving-v3 3 {npc} {value['start']} {value['target']} {route} {kind} {limit} {value['seed']}"


def commands(args, value, teacher):
    import nav_rl as rl
    limit = value.get("limit_ms", 240000)
    if args.pace == "walk":
        limit = math.ceil(240000 * WALK_FACTOR)
    if args.limit_ms is not None:
        limit = args.limit_ms
    worker = SimpleNamespace(npc=args.npc, roads=args.roads, weights=args.model, lean=True,
                             familiar_map=None, stranger=False, limit_ms=limit)
    result = rl.policy_commands(worker, dict(value, limit_ms=limit), ACTION_SEED)
    drive = [index for index, command in enumerate(result) if command.startswith("navdemos drive")]
    if len(drive) != 1 or "navdemos record off" not in result:
        raise ValueError(f"Unexpected validation commands for {value['id']}")
    if teacher:
        result[drive[0]] = teacher_command(value, args.npc, limit)
    switches = [f"navdemos teacher-exit {args.exit}", f"navdemos follow-lost {args.lost}"]
    if value["goal_kind"] == "follow":
        switches += ["navdemos follow-guidance trail", "navdemos follow-run on"]
    elif teacher:
        switches.append(f"navdemos teacher-pace {args.pace}")
    result[drive[0]:drive[0]] = switches
    return result


def scenarios(args):
    import nav_rl as rl
    families = args.families.split(",")
    if not families or set(families) - set(FAMILIES):
        raise ValueError(f"--families must name {', '.join(FAMILIES)}")
    values = []
    validation = [family for family in families if family != "detour"]
    if validation:
        values += rl.validation_scenarios(args.manifest, ",".join(validation))
    if "detour" in families:
        if args.detour is None:
            raise ValueError("--detour names the frozen detour cohort")
        values += rl.detour_scenarios(args.detour)
    if args.first is not None:
        values = values[:args.first]
    if args.ids:
        wanted = args.ids.split(",")
        values = [value for value in values if value["id"] in wanted]
        if len(values) != len(wanted):
            raise ValueError("--ids names routes outside the chosen families")
    return values


def episode_row(directory, value):
    """The facts of one stand episode used by the report."""
    episode = read_json(directory / "episode.json")
    row = dict(id=value["id"], family=value["family"], goal_kind=value["goal_kind"],
               success=bool(episode["success"]), failure=episode.get("failure"), elapsed_ms=episode["elapsed_ms"],
               limit_ms=episode["limit_ms"], decisions=episode["decisions"],
               start_not_found=episode.get("start_not_found_decisions"),
               body_exit=episode.get("teacher_body_exit"), teacher_pace=episode.get("teacher_pace"),
               branches=episode.get("teacher_branches"), far_idle=episode.get("teacher_far_idle"),
               far_decisions=episode.get("teacher_far_decisions"), pace=episode.get("pace"),
               base_fingerprint=os.environ.get("OPENGOTHIC_BASE_FINGERPRINT"))
    if value["goal_kind"] == "follow":
        row["follow"] = follow.episode_follow(directory)
        row["follow_lost"] = episode.get("follow_lost")
    return row


def validation(args):
    import nav_rl as rl
    teacher = args.model == "teacher"
    if not teacher:
        args.model = Path(args.model).resolve()
        if not args.model.is_file():
            raise ValueError("--model must be teacher or a weight file")
        if args.pace not in ("run", "walk"):
            raise ValueError("A pace-input policy needs --pace run|walk")
        os.environ["OPENGOTHIC_NAV_FORCE_PACE"] = args.pace
    else:
        os.environ.pop("OPENGOTHIC_NAV_FORCE_PACE", None)
    if args.trace:
        os.environ["OPENGOTHIC_NAV_STALL_TRACE"] = "1"
    else:
        os.environ.pop("OPENGOTHIC_NAV_STALL_TRACE", None)
    values = scenarios(args)
    workers = min(args.parallel, len(values))
    started = time.monotonic()
    args.output.mkdir(parents=True, exist_ok=True)
    identity = dict(model="teacher" if teacher else str(args.model),
                    model_sha256=None if teacher else follow.sha256(args.model), families=args.families,
                    exit=args.exit, lost=args.lost, pace=args.pace, limit_ms=args.limit_ms,
                    runner_sha256=follow.sha256(args.runner), manifest=str(args.manifest),
                    detour=str(args.detour) if args.detour else None, action_seed=ACTION_SEED,
                    base_fingerprint=os.environ.get("OPENGOTHIC_BASE_FINGERPRINT"))
    if (args.output / "identity.json").is_file() and read_json(args.output / "identity.json") != identity:
        raise ValueError(f"Refusing different inputs at {args.output}")
    follow.write_json(args.output / "identity.json", identity)

    def collect(index):
        worker = copy(args)
        worker.output = args.output / "workers" / str(index)
        worker.cache = args.cache / str(index)
        worker.cache.mkdir(parents=True, exist_ok=True)
        worker.seed = ACTION_SEED
        runner = rl.new_runner(worker, "stall")
        try:
            for value in values[index::workers]:
                directory = args.output / "episodes" / value["id"]
                if (args.rejections / f"{value['id']}.json").is_file() or (directory / "row.json").is_file():
                    continue
                if directory.exists():
                    shutil.rmtree(directory)
                worker.seed = value["seed"]
                try:
                    rl.scenario(worker, runner, f"stall-{value['id']}", commands(args, value, teacher), directory)
                except RuntimeError as error:
                    if str(error) != "Runner rejected advance: navdemos moving route needs distinct connected points":
                        raise
                    follow.write_json(args.rejections / f"{value['id']}.json",
                                      dict(id=value["id"], family=value["family"], validation_scenario=value,
                                           reason=str(error)))
                    runner.close()
                    runner = rl.new_runner(worker, "stall")
                    continue
                if not args.keep_traces:
                    rl.clean_lean_diagnostics(directory)
                row = episode_row(directory, value)
                if row["body_exit"] not in (None, args.exit) or (teacher and row["body_exit"] is None):
                    raise ValueError(f"Episode body exit differs: {directory}")
                if teacher and value["goal_kind"] != "follow" and row["teacher_pace"] != args.pace:
                    raise ValueError(f"Episode teacher pace differs: {directory}")
                if value["goal_kind"] == "follow" and row["follow_lost"] != args.lost.replace("-", "_"):
                    raise ValueError(f"Episode lost rule differs: {directory}")
                follow.write_json(directory / "row.json", row)
                print(json.dumps(dict(id=value["id"], success=row["success"], elapsed_ms=row["elapsed_ms"],
                                      start_not_found=row["start_not_found"])), flush=True)
        finally:
            runner.close()
        shutil.rmtree(worker.output, ignore_errors=True)

    with ThreadPoolExecutor(max_workers=workers) as pool:
        for future in [pool.submit(collect, index) for index in range(workers)]:
            future.result()
    rows = [read_json(path) for path in sorted((args.output / "episodes").glob("*/row.json"))]
    rejected = {path.stem for path in args.rejections.glob("*.json")}
    if len(rows) + sum(value["id"] in rejected for value in values) != len(values):
        raise ValueError(f"Incomplete validation: {len(rows)} episodes of {len(values)}")
    follow.write_json(args.output / "summary.json", dict(identity, seconds=time.monotonic() - started,
                                                         rejected=sorted(rejected), episodes=rows))
    print(json.dumps(dict(event="stall_validation_complete", episodes=len(rows),
                          stuck=sum((row["start_not_found"] or 0) > STALL_DECISIONS for row in rows))), flush=True)


def stalls(args):
    """Why there is no start cell: memory around the feet against the navmesh truth.

    Reads the stall_patch fields of teacher-paths.jsonl (OPENGOTHIC_NAV_STALL_TRACE=1).
    A cell is near the feet when its square lies within 50 cm of the body origin.
    """
    names = ("wall", "ledge", "drop", "water", "movable")
    patches, cells = 0, []
    for path in sorted(args.root.glob("episodes/*/teacher-paths.jsonl")):
        for line in path.read_text().splitlines():
            value = json.loads(line)
            patch = value.get("stall_patch")
            if patch is None:
                continue
            patches += 1
            for x, z, _, free, *channels, familiar, height, traversable, boundary, truth in patch["cells"]:
                dx = max(0., abs((x + .5) * 25) - 12.5)
                dz = max(0., abs((z + .5) * 25) - 12.5)
                if math.hypot(dx, dz) > 50 or familiar:
                    continue
                kinds = [name for name, byte in zip(names, channels) if byte]
                cells.append(dict(kinds=kinds, free=bool(free), traversable=bool(traversable),
                                  boundary=bool(boundary), height=height, truth=truth,
                                  under=math.hypot(dx, dz) <= 15, episode=path.parent.name))
    hazard = [cell for cell in cells if cell["kinds"]]
    result = dict(patches=patches, cells_near=len(cells), hazard_cells=len(hazard),
                  hazard_traversable=sum(cell["traversable"] for cell in hazard),
                  under_feet=sum(cell["under"] for cell in hazard),
                  under_feet_traversable=sum(cell["under"] and cell["traversable"] for cell in hazard),
                  by_kind={})
    for name in names:
        group = [cell for cell in hazard if name in cell["kinds"]]
        heights = [cell["height"] for cell in group if cell["height"] is not None]
        result["by_kind"][name] = dict(
            cells=len(group), traversable=sum(cell["traversable"] for cell in group),
            also_free=sum(cell["free"] for cell in group),
            with_ground=len(heights), ground_median_cm=statistics.median(heights) if heights else None,
            ground_within_step=sum(abs(height) <= 50 for height in heights))
    follow.write_json(args.root / "stalls.json", result)
    print(json.dumps(result, indent=2))


def contacts(args):
    """Where the follow teacher's extra contacts are: the run with the exit and `aside`
    against the one as on main, paired by route. The stand counts contacts per episode
    only, so episodes are grouped by the new code they reached (per-episode rule counts)."""
    root = args.root / "validation"
    on = {row["id"]: row for row in read_json(root / "follow-teacher-on-aside/summary.json")["episodes"]}
    off = {row["id"]: row for row in read_json(root / "follow-teacher-off-last-seen/summary.json")["episodes"]}
    count = lambda row: row["follow"]["contacts"]
    def group(ids):
        differences = [count(on[i]) - count(off[i]) for i in ids]
        mean = sum(differences) / len(differences) if differences else None
        se = (math.sqrt(sum((d - mean) ** 2 for d in differences) / (len(differences) - 1) / len(differences))
              if len(differences) > 1 else None)
        return dict(episodes=len(ids), off=sum(count(off[i]) for i in ids), on=sum(count(on[i]) for i in ids),
                    delta=mean, se=se)
    ids = sorted(on)
    exit_ = [i for i in ids if on[i]["branches"]["body_exit"] > 0]
    aside = [i for i in ids if i not in exit_ and on[i]["follow"]["sources"].get("aside", 0) > 0]
    escape = [i for i in ids if i not in exit_ and i not in aside
              and on[i]["branches"]["escape"] + on[i]["branches"]["escape_blocked"] > 0]
    result = dict(
        all=group(ids),
        without_start_not_found=group([i for i in ids if off[i]["start_not_found"] == 0 and on[i]["start_not_found"] == 0]),
        with_start_not_found=group([i for i in ids if off[i]["start_not_found"] > 0 or on[i]["start_not_found"] > 0]),
        body_exit=group(exit_), aside_only=group(aside), old_escape_only=group(escape),
        none=group([i for i in ids if i not in exit_ and i not in aside and i not in escape]),
        largest=[dict(id=i, off=count(off[i]), on=count(on[i]), body_exit=on[i]["branches"]["body_exit"],
                      start_not_found_off=off[i]["start_not_found"], start_not_found_on=on[i]["start_not_found"])
                 for i in sorted(ids, key=lambda i: count(off[i]) - count(on[i]))[:12]])
    follow.write_json(args.root / "contacts.json", result)
    print(json.dumps(result, indent=2))


def far_idle(rows):
    """Standing decisions far from the goal, by reason, over the teacher's decisions far from it."""
    total = sum(row["far_decisions"] or 0 for row in rows)
    counts = {reason: sum((row["far_idle"] or {}).get(name, 0) for row in rows for name in names)
              for reason, names in REASONS.items()}
    return dict(far_decisions=total, idle=sum(counts.values()), reasons=counts,
                episodes_stuck=sum((row["start_not_found"] or 0) > STALL_DECISIONS for row in rows),
                start_not_found=sum(row["start_not_found"] or 0 for row in rows))


def seconds(row):
    """Time to arrive; a failure counts as the whole horizon, also when it ended early
    (actor_down, body_blocked)."""
    return (row["elapsed_ms"] if row["success"] else row["limit_ms"]) / 1000


def static_group(rows):
    times = [seconds(row) for row in rows]
    return dict(episodes=len(rows), arrived=sum(row["success"] for row in rows),
                share=sum(row["success"] for row in rows) / len(rows) if rows else None,
                median_s=statistics.median(times) if times else None, **far_idle(rows))


def paired_static(before, after):
    """Arrival share and median time, after - before, paired by route.

    SE of the arrival difference from per-route differences; SE of the median
    difference from 5000 paired bootstrap resamples of routes (seed 18102027).
    A failure counts as the horizon in the medians and in every resample.
    """
    left = {row["id"]: row for row in before}
    right = {row["id"]: row for row in after}
    shared = sorted(left.keys() & right.keys())
    if len(shared) < 2:
        return dict(n=len(shared))
    arrived = [int(right[i]["success"]) - int(left[i]["success"]) for i in shared]
    mean = sum(arrived) / len(arrived)
    se = math.sqrt(sum((value - mean) ** 2 for value in arrived) / (len(arrived) - 1) / len(arrived))
    a = [seconds(left[i]) for i in shared]
    b = [seconds(right[i]) for i in shared]
    delta = statistics.median(b) - statistics.median(a)
    rng = random.Random(ACTION_SEED)
    samples = []
    for _ in range(5000):
        pick = [rng.randrange(len(shared)) for _ in shared]
        samples.append(statistics.median(b[k] for k in pick) - statistics.median(a[k] for k in pick))
    mean_sample = sum(samples) / len(samples)
    median_se = math.sqrt(sum((value - mean_sample) ** 2 for value in samples) / (len(samples) - 1))
    changed = [i for i in shared if left[i]["success"] != right[i]["success"]
               or abs(left[i]["elapsed_ms"] - right[i]["elapsed_ms"]) > 0]
    stalled = [i for i in shared if (left[i]["start_not_found"] or 0) > 0]
    return dict(n=len(shared), arrived_delta=mean, arrived_se=se, median_delta_s=delta, median_se_s=median_se,
                arrived_ok=mean >= -2 * se, median_ok=delta <= 2 * median_se,
                routes_changed=len(changed), routes_with_start_not_found_before=len(stalled),
                routes_stuck_before=sum((left[i]["start_not_found"] or 0) > STALL_DECISIONS for i in shared),
                routes_stuck_after=sum((right[i]["start_not_found"] or 0) > STALL_DECISIONS for i in shared),
                gained=sum(right[i]["success"] and not left[i]["success"] for i in shared),
                lost=sum(left[i]["success"] and not right[i]["success"] for i in shared))


def follow_group(rows):
    value = follow.group([row["follow"] for row in rows])
    value.update(far_idle(rows))
    return value


def report(args):
    root = args.root
    result = dict(follow={}, static={}, paired_static={}, paired_follow={})
    for path in sorted(root.glob("validation/*/summary.json")):
        summary = read_json(path)
        rows = summary["episodes"]
        name = path.parent.name
        if summary["families"] == "follow_below_gate":
            result["follow"][name] = dict(follow_group(rows), seconds=summary["seconds"],
                                          runner_sha256=summary["runner_sha256"])
        else:
            result["static"][name] = dict(seconds=summary["seconds"], runner_sha256=summary["runner_sha256"],
                                          families={family: static_group([row for row in rows
                                                                          if row["family"] == family])
                                                    for family in sorted({row["family"] for row in rows})})
    rows = {path.parent.name: read_json(path)["episodes"] for path in root.glob("validation/*/summary.json")}
    for pace in ("run", "walk"):
        for family in ("point", "object", "approach", "detour"):
            before, after = rows.get(f"teacher-{pace}-off-{family}"), rows.get(f"teacher-{pace}-on-{family}")
            if before and after:
                result["paired_static"][f"{pace}/{family}"] = paired_static(before, after)
    for name, before, after in (("teacher", "follow-teacher-off-last-seen", "follow-teacher-on-aside"),
                                ("v5", "follow-v5-last-seen", "follow-v5-aside")):
        if before in rows and after in rows:
            left = [dict(row["follow"], id=row["id"]) for row in rows[before]]
            right = [dict(row["follow"], id=row["id"]) for row in rows[after]]
            result["paired_follow"][name] = {key: follow.paired(left, right, key) for key in ("band", "far", "contacts")}
    path = root / "live" / "trail-summary.json"
    if path.is_file():
        value = read_json(path)
        for row in value["rows"]:
            episode = read_json(Path(row["directory"]) / "episode.json")
            row.update(start_not_found=episode.get("start_not_found_decisions"),
                       far_idle=episode.get("teacher_far_idle"), far_decisions=episode.get("teacher_far_decisions"))
        result["live"] = {model: dict(follow.group([row for row in value["rows"] if row["model"] == model]),
                                      seeds=[row for row in value["rows"] if row["model"] == model])
                          for model in ("teacher", "v5")}
        result["live"]["teacher"].update(far_idle([row for row in value["rows"] if row["model"] == "teacher"]))
    follow.write_json(root / "report.json", result)
    print(markdown(result))


def markdown(result):
    """The report tables: Russian decimal commas, shares in percent."""
    def number(value, digits=1, percent=False):
        if value is None:
            return "—"
        return f"{100 * value if percent else value:.{digits}f}".replace(".", ",")
    lines = ["Follow validation (после первого сближения):",
             "| Конфигурация | Эпизодов | Со сближением | ≥80% | Полоса 2–5 м | Дальше 5 м | Ближе 2 м | Контакты "
             "| Застряли (>200 snf) | Решений snf | Источники aside / last_seen |",
             "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for name, value in result["follow"].items():
        sources = value["source_decisions"]
        # Under a policy the teacher only labels: its start cells say nothing about the body.
        teacher = "teacher" in name
        stuck = value["episodes_stuck"] if teacher else "—"
        stalls = value["start_not_found"] if teacher else "—"
        lines.append(f"| {name} | {value['episodes']} | {value['contacted']} | {value['qualified']} "
                     f"| {number(value['band'], percent=True)}% | {number(value['far'], percent=True)}% "
                     f"| {number(value['near'], percent=True)}% | {value['contacts']} | {stuck} "
                     f"| {stalls} | {sources.get('aside', 0)} / {sources.get('last_seen', 0)} |")
    lines += ["", "Парные разности по маршрутам (после − до, среднее ± SE, n):"]
    for name, metrics in result["paired_follow"].items():
        lines.append(f"- {name}: " + "; ".join(
            f"{key} {number(value['delta'] * (1 if key == 'contacts' else 100), 2)} ± "
            f"{number(value['se'] * (1 if key == 'contacts' else 100), 2)} ({value['n']})"
            for key, value in metrics.items() if value["n"]))
    lines += ["", "Стоит вдали от цели (решения без шага / решения дальше 5 м для follow и 2 м для остальных), по причинам:",
              "| Конфигурация | Семейство | Решений вдали | Стоит | start_not_found | опасность | осторожная | поворот | другое "
              "| Застряли |", "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
    def idle_row(name, family, value):
        far = value["far_decisions"] or 0
        share = lambda count: f"{count} ({number(count / far if far else None, percent=True)}%)"
        reasons = value["reasons"]
        return (f"| {name} | {family} | {far} | {share(value['idle'])} | {share(reasons['start_not_found'])} "
                f"| {share(reasons['danger'])} | {share(reasons['cautious'])} | {share(reasons['turn'])} "
                f"| {share(reasons['other'])} | {value['episodes_stuck']} |")
    for name, value in result["follow"].items():
        if "teacher" in name:
            lines.append(idle_row(name, "follow", value))
    for name, value in result["static"].items():
        for family, group in value["families"].items():
            lines.append(idle_row(name, family, group))
    lines += ["", "Остальные семейства, учитель с выходом (on) против без него (off):",
              "| Темп / семейство | n | Приезды off → on | Медиана, с, off → on | Δ приездов, п.п. ± SE | Δ медианы, с ± SE "
              "| Не хуже | Маршрутов застряло off → on | Маршрутов со snf до | Получили / потеряли приезд |",
              "|---|---:|---:|---:|---:|---:|---|---:|---:|---:|"]
    for key, value in result["paired_static"].items():
        pace, family = key.split("/")
        before = result["static"][f"teacher-{pace}-off-{family}"]["families"][family]
        after = result["static"][f"teacher-{pace}-on-{family}"]["families"][family]
        lines.append(f"| {key} | {value['n']} | {before['arrived']} → {after['arrived']} "
                     f"| {number(before['median_s'])} → {number(after['median_s'])} "
                     f"| {number(100 * value['arrived_delta'], 2)} ± {number(100 * value['arrived_se'], 2)} "
                     f"| {number(value['median_delta_s'], 2)} ± {number(value['median_se_s'], 2)} "
                     f"| {'да' if value['arrived_ok'] and value['median_ok'] else 'нет'} "
                     f"| {value['routes_stuck_before']} → {value['routes_stuck_after']} "
                     f"| {value['routes_with_start_not_found_before']} | {value['gained']} / {value['lost']} |")
    if "live" in result:
        lines += ["", "03-follow-hero:", "| Модель | Полоса 2–5 м | Дальше 5 м | Ближе 2 м | Контакты | Решений snf |",
                  "|---|---:|---:|---:|---:|---:|"]
        for model, value in result["live"].items():
            lines.append(f"| {model} | {number(value['band'], percent=True)}% | {number(value['far'], percent=True)}% "
                         f"| {number(value['near'], percent=True)}% | {value['contacts']} "
                         f"| {value.get('start_not_found', '—')} |")
            for seed in value["seeds"]:
                lines.append(f"| {model} seed {seed['seed']} | {number(seed['band'], percent=True)}% "
                             f"| {number(seed['far'], percent=True)}% "
                             f"| {number(seed['near_ms'] / seed['follow_ms'] if seed['follow_ms'] else None, percent=True)}% "
                             f"| {seed['contacts']} | {seed.get('start_not_found', '—')} |")
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("validation", "stalls", "report", "contacts"))
    parser.add_argument("--model", help="teacher or a .navbc weight file (validation)")
    parser.add_argument("--families", default="follow_below_gate")
    parser.add_argument("--exit", choices=("on", "off"), default="on")
    parser.add_argument("--lost", choices=("aside", "last-seen"), default="aside")
    parser.add_argument("--pace", choices=("own", "walk", "run"), default="own",
                        help="The teacher's forced pace off follow; a policy needs run or walk")
    parser.add_argument("--npc", default="BAU_954_MALETH")
    parser.add_argument("--limit-ms", type=int, help="Override the route horizon (smoke only)")
    parser.add_argument("--first", type=int, help="Only the first N frozen routes (smoke only)")
    parser.add_argument("--ids", help="Only these frozen route IDs (diagnosis)")
    parser.add_argument("--keep-traces", action="store_true", help="Keep teacher paths and frames (diagnosis)")
    parser.add_argument("--trace", action="store_true", help="Dump memory and navmesh truth at stalls (diagnosis)")
    parser.add_argument("--parallel", type=int, default=12)
    parser.add_argument("--physics", default="cv")
    parser.add_argument("--watchdog-seconds", type=float, default=1800)
    parser.add_argument("--ini", type=Path)
    for name in ("output", "runner", "assets", "save", "links", "roads", "cache", "manifest", "rejections",
                 "detour", "root"):
        parser.add_argument("--" + name, type=Path)
    args = parser.parse_args(argv)
    for key, value in vars(args).items():
        if isinstance(value, Path):
            setattr(args, key, value.expanduser().resolve())
    required = dict(validation=("model", "output", "runner", "assets", "save", "links", "roads", "cache",
                                "manifest", "rejections"), stalls=("root",), report=("root",),
                    contacts=("root",))
    missing = [name for name in required[args.command] if getattr(args, name) is None]
    if missing:
        parser.error("missing --" + ", --".join(missing))
    {"validation": validation, "stalls": stalls, "report": report, "contacts": contacts}[args.command](args)


if __name__ == "__main__":
    main()
