#!/usr/bin/env python3
"""Compare memory-v3 and v2 at physically qualified cliffs, water and ledges.

Run only inside the caller's admitted Ryzen gq batch. Historical CV links,
qualified routes and jump recordings are read-only. Fixture preparation is
bounded and precedes every actual attempt; episodes run ordinary start-v3/v2 with
passive physical-tick monitoring and never create NAVDEMO recordings.

The runner's automatic ``cheat god`` affects the player HERO, not the observed
Maleth actor. Health is monitored from tick zero; absence of damage is evidence
from an ordinary NPC, rather than an attempt to toggle a global invulnerability.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import sys
from types import SimpleNamespace


REPO = Path(__file__).resolve().parent.parent
KINDS = ("cliff", "water", "ledge")
TEACHERS = ("memory", "v2")
BRIDGE_LINK = 1230208767
SHORES = (("water-bigfarm-lake", (51441, 1861, -3979)),
          ("water-lake", (49232, 1619, -7571)),
          ("water-harbour", (41, -125, 780)),
          ("water-ship-shore", (-4180, -594, -19312)),
          ("water-shipwreck-shore", (-319, -455, 22600)),
          ("water-mountain-lake", (42515, 2918, -25361)),
          ("water-troll-lake", (61494, 6527, 33526)),
          ("water-bigfarm-hut", (51937, 1680, -10504)))
CV_STEP_CM = 60
CRITERIA_VERSION = 2
LANDING_DAMAGE_WINDOW_MS = 500
WATER_DEPTH_EPSILON_CM = 1


def module(name):
    spec = importlib.util.spec_from_file_location(name, REPO / "scripts" / f"{name}.py")
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def rows(path):
    return [json.loads(line) for line in Path(path).read_text().splitlines() if line.strip()]


def write_json(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n")


def digest(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def point(value):
    if not value.startswith("cm:"):
        raise ValueError("Historical hazard route must use explicit cm coordinates")
    result = [float(field) for field in value[3:].split(",")]
    if len(result) != 3 or not all(math.isfinite(field) for field in result):
        raise ValueError("Invalid historical hazard route coordinate")
    return result


def token(value):
    return "cm:" + ",".join(f"{field:.9g}" for field in value)


def fixture_row(label, kind, start, goal, hazard, identifier):
    return " ".join([label, kind, *(f"{v:.9g}" for p in (start, goal, hazard) for v in p), str(identifier)])


def height_candidates(routes, links, records):
    """Use recorded sites before looking at new-episode outcomes."""
    accepted = {value["id"]: value for value in module("nav_jumps").links(links)}
    historical = rows(routes)
    result, xardas_links = [], set()
    for route in historical:
        identifier = route["transition_id"]
        if identifier not in accepted:
            raise ValueError(f"Historical route link absent from accepted CV links: {identifier}")
        link = accepted[identifier]
        for key, expected in (("transition_start_m", link["start"]), ("transition_end_m", link["end"])):
            if math.dist(route[key], expected) > .001:
                raise ValueError(f"Historical route differs from accepted link: {identifier}")
        if identifier != BRIDGE_LINK and not (route["region"] == "xardas" and
                abs(link["end"][1] - link["start"][1]) >= .60 and identifier not in xardas_links):
            continue
        recorded = Path(records) / "episodes" / f"{route['index']:04d}" / "episode.json"
        if not recorded.is_file():
            raise ValueError(f"Missing historical hazard recording: {recorded}")
        metadata = json.loads(recorded.read_text())
        if metadata.get("transition_id") != identifier:
            raise ValueError(f"Historical hazard recording belongs to another link: {recorded}")
        if identifier == BRIDGE_LINK:
            start, goal = point(route["start"]), point(route["goal"])
            hazard = [v * 100 for v in link["end"]]
            dx, dz = goal[0] - start[0], goal[2] - start[2]
            length = math.hypot(dx, dz)
            for back in (0, 100, 200):
                shifted = [start[0] - back * dx / length, start[1], start[2] - back * dz / length]
                result.append(fixture_row(f"cliff-bridge-{route['index']}-back{back}", "cliff", shifted, goal, hazard, identifier))
        else:
            xardas_links.add(identifier)
            high, low = sorted((link["start"], link["end"]), key=lambda p: p[1], reverse=True)
            start, hazard = ([v * 100 for v in p] for p in (high, low))
            dx, dz = hazard[0] - start[0], hazard[2] - start[2]
            length = math.hypot(dx, dz)
            goal = [hazard[0] + 100 * dx / length, hazard[1], hazard[2] + 100 * dz / length]
            result.append(fixture_row(f"ledge-xardas-link{identifier}", "ledge", start, goal, hazard, identifier))
    if len(xardas_links) < 3 or not any(row.startswith("cliff-") for row in result):
        raise ValueError("Need historical bridge drop and at least three distinct Xardas height links")
    return result


def select_fixtures(fixtures, per_kind=3):
    selected = []
    for kind in KINDS:
        group = [row for row in fixtures if row["kind"] == kind and row.get("qualified") is True]
        unique = []
        for row in group:
            signature = tuple(tuple(row[key]) for key in ("projected_start_cm", "goal_cm", "hazard_cm"))
            if any(signature == tuple(tuple(other[key]) for key in ("projected_start_cm", "goal_cm", "hazard_cm"))
                   for other in unique):
                continue
            # Different seeds can rediscover the same shore. They do not count
            # as additional independent water boundary controls.
            if kind == "water" and any(math.hypot(row["hazard_cm"][0] - other["hazard_cm"][0],
                                                 row["hazard_cm"][2] - other["hazard_cm"][2]) < 200
                                       for other in unique):
                continue
            unique.append(row)
        group = unique
        if len(group) < per_kind:
            reasons = Counter(row["reason"] for row in fixtures if row["kind"] == kind)
            raise ValueError(f"Safety preparation {kind}: qualified {len(group)}/{per_kind}, reasons={dict(reasons)}")
        selected.extend(group[:per_kind])
    if len({row["label"] for row in selected}) != len(selected):
        raise ValueError("Safety fixture labels must be unique")
    return selected


def baseline_water_fixtures(path):
    """Keep all three original water controls, regardless of their outcomes."""
    fixtures = json.loads(Path(path).read_text())
    water = [row for row in fixtures if row.get("kind") == "water"]
    if len(water) != 3 or any(row.get("qualified") is not True for row in water):
        raise ValueError("Water baseline must contain exactly three qualified original water fixtures")
    for row in water:
        for key in ("projected_start_cm", "goal_cm", "hazard_cm"):
            values = row.get(key, [])
            if len(values) != 3 or not all(isinstance(v, (int, float)) and math.isfinite(v) for v in values):
                raise ValueError(f"Invalid original water fixture coordinate: {key}")
    if len({row["label"] for row in water}) != 3:
        raise ValueError("Original water fixture labels must be unique")
    return water


def select_water_followup(original, candidates, new_shores=3):
    """Freeze independent shores before attempting any teacher episode."""
    selected = list(original)
    for row in candidates:
        if row.get("kind") != "water" or row.get("qualified") is not True:
            continue
        if row["label"] in {other["label"] for other in selected}:
            continue
        if any(math.hypot(row["hazard_cm"][0] - other["hazard_cm"][0],
                          row["hazard_cm"][2] - other["hazard_cm"][2]) < 500 for other in selected):
            continue
        selected.append(row)
        if len(selected) == len(original) + new_shores:
            return selected
    raise ValueError(f"Water preparation: qualified {len(selected) - len(original)}/{new_shores} new shores "
                     "at least 500cm from every original/new hazard; no episodes attempted")


def fall_intervals(frames):
    """Physical airborne/landing intervals, without joining a grounded slope."""
    result, active = [], None
    for previous, frame in zip(frames, frames[1:]):
        unsupported = frame["in_air"] or frame["falling"] or not frame["supported"]
        if active is None and unsupported:
            active = dict(start_ms=previous["elapsed_ms"], end_ms=frame["elapsed_ms"],
                          peak_cm=previous["position_cm"][1], descent_cm=0.)
        if active is not None:
            active["end_ms"] = frame["elapsed_ms"]
            active["peak_cm"] = max(active["peak_cm"], frame["position_cm"][1])
            active["descent_cm"] = max(active["descent_cm"], active["peak_cm"] - frame["position_cm"][1])
            if active["descent_cm"] > CV_STEP_CM:
                active.setdefault("descent_witnessed_ms", frame["elapsed_ms"])
            if not unsupported:
                result.append(active)
                active = None
        elif previous["position_cm"][1] - frame["position_cm"][1] > CV_STEP_CM:
            # A complete fall inside one observed tick still has a measurable
            # drop; this is diagnostic unless health also decreases nearby.
            result.append(dict(start_ms=previous["elapsed_ms"], end_ms=frame["elapsed_ms"],
                               peak_cm=previous["position_cm"][1],
                               descent_witnessed_ms=frame["elapsed_ms"],
                               descent_cm=previous["position_cm"][1] - frame["position_cm"][1]))
    if active is not None:
        result.append(active)
    return result


def assess(fixture, metadata, frames):
    if len(frames) < 2 or any(b["elapsed_ms"] <= a["elapsed_ms"] for a, b in zip(frames, frames[1:])):
        raise ValueError("Missing/nonmonotonic physical safety monitor")
    if frames[0]["elapsed_ms"] != 0:
        raise ValueError("Safety monitor must include the initial elapsed_ms=0 frame")
    required = ("cv_active", "supported", "in_air", "falling", "native_jump", "native_climb", "swimming")
    for frame in frames:
        if any(type(frame.get(key)) is not bool for key in required):
            raise ValueError("Incomplete passive safety monitor fields")
        if len(frame["position_cm"]) != 3 or not all(math.isfinite(v) for v in frame["position_cm"]):
            raise ValueError("Nonfinite physical safety monitor position")
        depth = frame.get("water_depth_cm")
        if not isinstance(depth, (float, int)) or not math.isfinite(depth) or depth < 0:
            raise ValueError("Missing/nonfinite passive feet water depth")
        threshold = frame.get("deep_water_threshold_cm")
        if not isinstance(threshold, (float, int)) or not math.isfinite(threshold) or threshold <= 0:
            raise ValueError("Missing/nonfinite actual guild deep water threshold")
        health, maximum = frame.get("hitpoints"), frame.get("hitpoints_max")
        if (type(health) is not int or type(maximum) is not int or maximum <= 0
                or not 0 <= health <= maximum):
            raise ValueError("Missing/invalid passive NPC hitpoints")
    initial = frames[0]
    if (not initial["supported"] or initial["in_air"] or initial["falling"]
            or initial["swimming"] or initial["hitpoints"] <= 0
            or initial["water_depth_cm"] + WATER_DEPTH_EPSILON_CM >= initial["deep_water_threshold_cm"]):
        raise ValueError("Safety control initial NPC must be alive, dry and physically supported")
    if len({frame["hitpoints_max"] for frame in frames}) != 1:
        raise ValueError("NPC maximum health changed during safety control")
    cv_frames = sum(frame["cv_active"] for frame in frames)
    supported_cv = sum(frame["cv_active"] and frame["supported"] for frame in frames)
    if cv_frames == 0:
        raise ValueError(f"Safety control lacks CV evidence: {cv_frames}/{len(frames)} physical ticks")
    # CV can activate on the first simulation tick. A real fall immediately
    # after the independently supported t=0 frame remains an actual attempt.
    if not any(frame["cv_active"] and frame["elapsed_ms"] <= 100 for frame in frames):
        raise ValueError("Safety control lacks CV evidence within its initial 100ms")
    intervals = fall_intervals(frames)
    max_descent = max((interval["descent_cm"] for interval in intervals), default=0.)
    damages = []
    for previous, frame in zip(frames, frames[1:]):
        loss = previous["hitpoints"] - frame["hitpoints"]
        if loss <= 0:
            continue
        matching = [interval for interval in intervals if interval["descent_cm"] > CV_STEP_CM
                    and interval["descent_witnessed_ms"] <= frame["elapsed_ms"]
                    <= interval["end_ms"] + LANDING_DAMAGE_WINDOW_MS]
        damages.append(dict(elapsed_ms=frame["elapsed_ms"], hitpoints_lost=loss,
                            associated_with_fall=bool(matching),
                            descent_cm=max((interval["descent_cm"] for interval in matching), default=0.)))
    swim = sum(frame["swimming"] for frame in frames)
    wet = sum(frame["water_depth_cm"] > 10 for frame in frames)
    deep = sum(frame["water_depth_cm"] + WATER_DEPTH_EPSILON_CM >= frame["deep_water_threshold_cm"]
               for frame in frames)
    hazard = fixture["hazard_cm"]
    closest = min(math.hypot(frame["position_cm"][0] - hazard[0], frame["position_cm"][2] - hazard[2]) for frame in frames)
    start = frames[0]["position_cm"]
    movement = max(math.hypot(frame["position_cm"][0] - start[0], frame["position_cm"][2] - start[2]) for frame in frames)
    # A goal may be deliberately unreachable. Stopping before the witnessed
    # boundary is safe. Harmless drops and shallow wading are diagnostic; the
    # requested failures are damaging falls, deep water, or swimming. Health
    # loss without a physical fall witness is inconclusive rather than silently
    # attributed to an edge (e.g. another NPC may have attacked).
    fell = any(damage["associated_with_fall"] for damage in damages)
    unexplained = sum(damage["hitpoints_lost"] for damage in damages if not damage["associated_with_fall"])
    safe = (not fell and swim == 0 and deep == 0 and unexplained == 0
            and not metadata.get("final_dead") and not metadata.get("final_down") and frames[-1]["hitpoints"] > 0)
    return dict(label=fixture["label"], kind=fixture["kind"], link_id=fixture["link_id"],
                safe=safe, harmful_fall=fell, unsafe_descent=fell,
                harmless_descent_over_step=max_descent > CV_STEP_CM and not fell,
                max_unsupported_descent_cm=max_descent, fall_intervals=intervals,
                damage_events=damages, unattributed_damage=unexplained, inconclusive=unexplained > 0,
                initial_hitpoints=initial["hitpoints"], final_hitpoints=frames[-1]["hitpoints"],
                hitpoints_max=initial["hitpoints_max"],
                swimming_frames=swim, entered_water_frames=wet, deep_water_frames=deep,
                max_water_depth_cm=max(frame["water_depth_cm"] for frame in frames),
                deep_water_threshold_cm=initial["deep_water_threshold_cm"],
                physical_frames=len(frames), cv_frames=cv_frames, supported_cv_frames=supported_cv,
                closest_hazard_cm=closest, max_displacement_cm=movement,
                initial_hazard_distance_cm=fixture["start_to_hazard_cm"],
                goal_success=metadata["success"], elapsed_ms=metadata["elapsed_ms"], failure=metadata.get("failure"))


def run(options):
    if sys.platform != "linux":
        raise ValueError("Safety episodes require Ryzen/Linux inside caller-owned gq batch")
    v3 = module("nav_teacher_v3")
    baseline = getattr(options, "water_baseline", None)
    original_water = baseline_water_fixtures(baseline) if baseline else []
    new_shores = getattr(options, "new_shores", 3)
    memory_version = getattr(options, "memory_version", 2)
    kinds = ("water",) if baseline else KINDS
    output = options.output or options.task / ("water-check-1" if baseline else "safety-check-1")
    if baseline and (baseline == output or output in baseline.parents):
        raise ValueError("Water follow-up output must be separate from original fixtures")
    output.mkdir(parents=True, exist_ok=True)
    args = SimpleNamespace(output=output, runner=options.runner, assets=options.assets, save=options.save,
        cache=options.cache, links=options.links, roads=options.roads, npc=options.npc, ini=None,
        seed=19102026, physics="cv", watchdog_seconds=900, limit_ms=options.limit_ms)
    v3.checked_paths(args)
    heights = [] if baseline else height_candidates(options.routes, options.links, options.records)
    height_file = output / "height-fixtures.txt"
    height_text = "\n".join(heights) + "\n"
    shores_file = output / "shore-seeds.txt"
    custom_seeds = getattr(options, "shore_seeds", None)
    shore_text = (custom_seeds.read_text() if custom_seeds else
                  "".join(label + " " + " ".join(map(str, center)) + "\n" for label, center in SHORES))
    source_identity = dict(input_sha256={name: digest(getattr(options, name))
                           for name in ("runner", "routes", "links", "save", "roads")},
        historical_records_sha256={str(path.relative_to(options.records)): digest(path)
            for path in ([] if baseline else sorted((options.records / "episodes").glob("*/episode.json")))},
        assets=str(options.assets), records=str(options.records), npc=args.npc, physics="cv", seed=args.seed,
        mode=options.mode, limit_ms=options.limit_ms, per_kind=options.per_kind, teachers=list(TEACHERS),
        navigation_memory_version=memory_version, safety_script_sha256=digest(Path(__file__)), criteria_version=CRITERIA_VERSION,
        criteria=dict(harmful_fall="HP loss during witnessed >60cm unsupported descent or within 500ms of landing",
                      deep_water="swimming or feet water depth +1cm >= actual guild water_depth_chest",
                      visible_water_followup="no entry deeper than 10cm on the six water fixtures" if baseline else None,
                      unrelated_damage="inconclusive; cannot count as a clean safety pass",
                      harmless_descent="diagnostic only", landing_damage_window_ms=LANDING_DAMAGE_WINDOW_MS),
        height_candidates_sha256=hashlib.sha256(height_text.encode()).hexdigest(),
        shore_seeds_sha256=hashlib.sha256(shore_text.encode()).hexdigest(),
        kinds=list(kinds), water_baseline_sha256=digest(baseline) if baseline else None,
        water_baseline_source=str(baseline) if baseline else None,
        new_shores=new_shores if baseline else 0, new_shore_min_separation_cm=500 if baseline else None,
        recordings_created=False, player_godmode="runner cheat god affects HERO only; controlled actor is Maleth")
    identity_file = output / "identity.json"
    if identity_file.exists():
        if json.loads(identity_file.read_text()) != source_identity:
            raise ValueError("Refusing to resume safety controls with different binary, inputs or criteria")
    else:
        if any(output.iterdir()):
            raise ValueError(f"Safety output contains unidentified prior work: {output}")
        write_json(identity_file, source_identity)
    if not baseline:
        height_file.write_text(height_text)
    shores_file.write_text(shore_text)
    runner = v3.new_runner(args)
    try:
        fixtures_file = output / "fixtures.json"
        if fixtures_file.exists():
            saved = json.loads(fixtures_file.read_text())
            fixtures = (select_water_followup(original_water, saved, new_shores) if baseline else
                        select_fixtures(saved, options.per_kind))
            if saved != fixtures:
                raise ValueError("Saved safety fixtures differ from the frozen pre-episode selection")
        else:
            preparation = [("shore", "perception-shores", shores_file)]
            if not baseline:
                preparation.insert(0, ("height", "perception-hazards", height_file))
            for name, command, source in preparation:
                v3.runner_scenario(args, runner, f"safety-{name}-prepare", [f"navdemos {command} {source}"], output / name, args.seed)
            water = rows(output / "shore/perception-hazards.jsonl")
            fixtures = (select_water_followup(original_water, water, new_shores) if baseline else
                        select_fixtures(rows(output / "height/perception-hazards.jsonl") + water, options.per_kind))
            write_json(fixtures_file, fixtures)
        results = []
        for teacher in TEACHERS:
            version = 2 if teacher == "v2" else 3
            for fixture in fixtures:
                directory = output / "episodes" / teacher / fixture["label"]
                identity = dict(source_identity, teacher=teacher, teacher_version=version, fixture_sha256=hashlib.sha256(
                    json.dumps(fixture, sort_keys=True, separators=(",", ":")).encode()).hexdigest())
                resumed = (directory / "episode.json").is_file()
                if not resumed:
                    if directory.exists() and any(directory.iterdir()):
                        raise ValueError(f"Refusing to overwrite partial safety episode: {directory}")
                    commands = command_sequence(args, fixture, teacher, options.mode)
                    v3.runner_scenario(args, runner, f"safety-{teacher}-{fixture['label']}", commands, directory, args.seed)
                metadata = json.loads((directory / "episode.json").read_text())
                validate_episode(directory, metadata, fixture, teacher, options.mode, args.limit_ms, memory_version)
                if resumed and metadata.get("perception_safety_identity") != identity:
                    raise ValueError(f"Safety episode belongs to different inputs or criteria: {directory}")
                if not resumed:
                    metadata["perception_safety_identity"] = identity
                    write_json(directory / "episode.json", metadata)
                measured = dict(assess(fixture, metadata, rows(directory / "perception-safety-frames.jsonl")),
                                teacher=teacher, teacher_version=version)
                if baseline:
                    measured["fixture_cohort"] = "original" if fixture in original_water else "new"
                results.append(measured)
                print(json.dumps(measured), flush=True)
        result = dict(episodes=results, by_kind={kind: {teacher: group_metrics(
            [row for row in results if row["kind"] == kind and row["teacher"] == teacher])
            for teacher in TEACHERS} for kind in kinds},
            identity=source_identity, fixtures=fixtures, recordings_created=False, complete=True,
            v3_passed=all(row["safe"] for row in results if row["teacher"] == "memory"),
            all_teachers_passed=all(row["safe"] for row in results))
        if baseline:
            result["v3_water_avoidance_passed"] = all(row["safe"] and row["entered_water_frames"] == 0
                for row in results if row["teacher"] == "memory")
            result["water_by_cohort"] = {cohort: {teacher: group_metrics(
                [row for row in results if row["fixture_cohort"] == cohort and row["teacher"] == teacher])
                for teacher in TEACHERS} for cohort in ("original", "new")}
        write_json(output / "metrics.json", result)
        if not result["v3_passed"] or (baseline and not result["v3_water_avoidance_passed"]):
            raise RuntimeError(f"Memory-v3 safety control failed or inconclusive: {result['by_kind']}")
        return result
    finally:
        runner.close()


def command_sequence(args, fixture, teacher, mode):
    if teacher not in TEACHERS:
        raise ValueError(f"Unknown safety teacher: {teacher}")
    version = 2 if teacher == "v2" else 3
    return [f"roads load {args.roads}", f"roads on {args.npc}",
            "navdemos record off", "navdemos oracle off", f"navdemos perception-mode {mode}",
            "navdemos perception-monitor on",
            f"navdemos start-v{version} {args.npc} {token(fixture['projected_start_cm'])} {token(fixture['goal_cm'])} {args.limit_ms}",
            "navdemos wait"]


def validate_episode(directory, metadata, fixture, teacher, mode, limit_ms, memory_version=2):
    version = 2 if teacher == "v2" else 3
    expected_source = "detour_teacher_v2" if teacher == "v2" else "learner_memory"
    if ((directory / "samples.bin.gz").exists() or metadata.get("record") is not False
            or metadata.get("demonstration_recorded") is not False):
        raise ValueError("Safety evaluation unexpectedly recorded demonstration samples")
    if (metadata.get("start") != token(fixture["projected_start_cm"])
            or metadata.get("goal") != token(fixture["goal_cm"]) or metadata.get("teacher_version") != version
            or metadata.get("oracle") is not False or metadata.get("navigation_memory_version") != memory_version
            or metadata.get("demo_version") != 2 or metadata.get("observation_source") != expected_source
            or metadata.get("limit_ms") != limit_ms or metadata.get("perception_mode") != mode):
        raise ValueError(f"Safety episode identity/memory version mismatch: {directory}")


def group_metrics(results):
    return dict(attempts=len(results), safe=sum(row["safe"] for row in results),
                inconclusive=sum(row["inconclusive"] for row in results),
                swimming_episodes=sum(row["swimming_frames"] > 0 for row in results),
                entered_water_episodes=sum(row["entered_water_frames"] > 0 for row in results),
                deep_water_episodes=sum(row["deep_water_frames"] > 0 for row in results),
                harmful_fall_episodes=sum(row["harmful_fall"] for row in results),
                harmless_descent_episodes=sum(row["harmless_descent_over_step"] for row in results))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("task", "runner", "assets", "save", "cache", "links", "roads", "routes", "records"):
        parser.add_argument(f"--{name}", type=Path, required=True)
    parser.add_argument("--output", type=Path, help="Absolute output path; default TASK/safety-check-1 (preserves old safety preparation)")
    parser.add_argument("--water-baseline", type=Path,
                        help="Original fixtures.json: repeat its exact three water controls, skip heights")
    parser.add_argument("--new-shores", type=int, choices=(3,), default=3,
                        help="With --water-baseline, add three independently qualified shores before episodes")
    parser.add_argument("--shore-seeds", type=Path,
                        help="Optional absolute physical shore search seed file (LABEL X Y Z in cm)")
    parser.add_argument("--memory-version", type=int, choices=(2, 3), default=2,
                        help="Required native navigation memory representation (default: historical 2)")
    parser.add_argument("--npc", default="BAU_954_MALETH")
    parser.add_argument("--mode", choices=("gaze", "rows"), default="gaze")
    parser.add_argument("--limit-ms", type=int, default=30000)
    parser.add_argument("--per-kind", type=int, choices=range(3, 6), default=3)
    options = parser.parse_args()
    if any(not getattr(options, name).is_absolute() for name in ("task", "runner", "assets", "save", "cache", "links", "roads", "routes", "records")):
        parser.error("All input/output paths must be absolute")
    for name in ("output", "water_baseline", "shore_seeds"):
        value = getattr(options, name)
        if value is not None and not value.is_absolute():
            parser.error(f"Safety {name} path must be absolute")
    if options.npc != "BAU_954_MALETH":
        parser.error("Safety controls require the ordinary BAU_954_MALETH NPC (runner HERO alone has godmode)")
    if not 1000 <= options.limit_ms <= 60000:
        parser.error("Safety episode limit must be 1000..60000 ms")
    run(options)


if __name__ == "__main__":
    main()
