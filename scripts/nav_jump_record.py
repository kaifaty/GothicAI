#!/usr/bin/env python3
"""Record bounded batches of qualified CV teacher-v2 jump routes on Ryzen.

Run inside an admitted gq batch job (<=8 threads, <=6GiB). No training is run.
New attempts require per-physical-tick transition-frames.jsonl evidence.
Every Runner.scenario resets the saved world. All attempts, including failures,
remain in output/episodes/NNNN; --resume skips completed attempts, never retries
failed routes. A persisted pilot stop cannot be resumed automatically.

    python3 -I scripts/nav_jump_record.py --runner /absolute/gothic-body-runner \
        --assets /absolute/game --save /absolute/save.sav --cache /absolute/own-cache \
        --links /absolute/accepted-links-cv.txt --roads /absolute/roads.json \
        --routes /absolute/routes.jsonl --output /absolute/nav-demos-jumps --count 30

Counts and stop reasons are printed as JSON. The source routes and links are
read-only. Collection evidence is appended to each existing episode.json; raw
recordings never enter git. Use nav_demos.py check on the final output separately.
"""
from __future__ import annotations

import argparse
from collections import Counter
from copy import copy
import importlib.util
import json
import math
from pathlib import Path
import shutil
import sys
import time


REPO = Path(__file__).resolve().parent.parent
DEFAULT_OUTPUT = Path.home() / "gothic-ai/nav-demos-jumps"
TYPES = ("jump", "climb", "drop")
SUCCESS_CRITERION = "demo_goal_action_landing_side_v1"
LIMIT_MS = 240000
# Reserve above 2401 * 173838 uncompressed NAVDEMO bytes, with room for gzip
# overhead, trajectory/actions, teacher paths and runner diagnostics. The usual
# 17KB/decision compression estimate is too small for a storage safety margin.
RESERVE_BYTES = 512 * 1024**2
MAX_BYTES = 10 * 1024**3
MIN_FREE_BYTES = 20 * 1024**3
REQUIRED = ("episode.json", "samples.bin.gz", "actions.txt", "trajectory.txt", "teacher-paths.jsonl")


def module(name):
    spec = importlib.util.spec_from_file_location(name, REPO / "scripts" / f"{name}.py")
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def json_rows(path):
    with Path(path).open(encoding="utf-8") as stream:
        return [json.loads(line) for line in stream if line.strip()]


def write_metadata(path, value):
    temporary = path.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def tree_bytes(path):
    """Count actual present file lengths, not a decision-count estimate."""
    return sum(file.stat().st_size for file in Path(path).rglob("*") if file.is_file())


def check_storage(output, disk_usage=shutil.disk_usage):
    output = Path(output)
    existing = tree_bytes(output)
    if existing + RESERVE_BYTES > MAX_BYTES:
        raise RuntimeError(f"STOP storage: {existing} existing + {RESERVE_BYTES} reserved > {MAX_BYTES} bytes")
    free = disk_usage(output).free
    if free < MIN_FREE_BYTES:
        raise RuntimeError(f"STOP disk: {free} free < {MIN_FREE_BYTES} bytes (20GiB)")
    return dict(existing_bytes=existing, reserved_bytes=RESERVE_BYTES, free_bytes=free)


def storage_root(output, requested=None):
    output = Path(output).resolve()
    root = Path(requested).resolve() if requested is not None else (
        DEFAULT_OUTPUT.resolve() if output.is_relative_to(DEFAULT_OUTPUT.resolve()) else output)
    if not output.is_relative_to(root):
        raise ValueError("--storage-root must contain --output")
    return root


def parse_indices(value):
    if value is None:
        return None
    try:
        result = [int(item) for item in value.split(",")]
    except ValueError as exc:
        raise ValueError("--indices must be comma-separated nonnegative route indices") from exc
    if not result or any(index < 0 for index in result) or len(set(result)) != len(result):
        raise ValueError("--indices must contain distinct nonnegative route indices")
    return result


def selected_routes(routes, indices=None):
    if indices is None:
        return collection_order(routes)
    by_index = {route["index"]: route for route in routes}
    unknown = set(indices) - by_index.keys()
    if unknown:
        raise ValueError(f"Unknown original route indices: {sorted(unknown)}")
    return [by_index[index] for index in indices]


def load_routes(path, links_path):
    routes = json_rows(path)
    if not routes:
        raise ValueError("No qualified routes")
    probe_module = module("nav_jumps")
    links = {link["id"]: link for link in probe_module.links(links_path)}
    route_module = module("nav_jump_routes")
    owners, seen = {}, set()
    for route in routes:
        index = route.get("index")
        if type(index) is not int or index < 0 or index in seen:
            raise ValueError("Route indices must be distinct original nonnegative integers")
        seen.add(index)
        if route.get("type") not in TYPES or route.get("split") not in ("train", "validation", "test"):
            raise ValueError(f"Invalid route type/split: {index}")
        for key in ("label", "start", "goal", "region"):
            if not isinstance(route.get(key), str) or not route[key] or any(c.isspace() for c in route[key]):
                raise ValueError(f"Invalid scenario token {key}: {index}")
        site = route.get("site_id", route.get("cluster"))
        if not isinstance(site, str) or not site:
            raise ValueError(f"Missing spatial site for route {index}")
        route["site_id"] = site
        identifier = route.get("transition_id")
        link = links.get(identifier)
        if link is None or probe_module.TYPES[link["type"]] != route["type"]:
            raise ValueError(f"Route transition is not in supplied accepted CV links: {index}")
        if route_module.qualifying_plan(route, route.get("teacher_plan", {})) is not None:
            raise ValueError(f"Route was not qualified against teacher v2: {index}")
        for key, expected in (("transition_start_m", link["start"]), ("transition_end_m", link["end"])):
            value = route.get(key, expected)
            if len(value) != 3 or not all(math.isfinite(v) for v in value) or math.dist(value, expected) > .001:
                raise ValueError(f"Route endpoint differs from accepted link: {index}")
            route[key] = list(expected)
        for key in (("site", site), ("transition", identifier)):
            if key in owners and owners[key] != route["split"]:
                raise ValueError(f"Spatial site/transition crosses splits: {key}")
            owners[key] = route["split"]
    return routes


def collection_order(routes):
    """Interleave the first ten attempts per type before the remaining routes."""
    grouped = {kind: [route for route in routes if route["type"] == kind] for kind in TYPES}
    pilot = [grouped[kind][i] for i in range(10) for kind in TYPES if i < len(grouped[kind])]
    chosen = {route["index"] for route in pilot}
    return pilot + [route for route in routes if route["index"] not in chosen]


def counts(episodes):
    result = {}
    for kind in TYPES:
        group = [episode for episode in episodes if episode["type"] == kind]
        successes = sum(episode.get("transition_success") is True for episode in group)
        result[kind] = dict(attempts=len(group), successes=successes,
                            reach_rate=successes/len(group) if group else None,
                            target_70pct=successes/len(group) >= .70 if group else False,
                            failures=dict(Counter(episode.get("transition_failure", "unspecified")
                                                  for episode in group if not episode.get("transition_success"))))
    return result


def pilot_stop(episodes):
    for kind, metric in counts(episodes).items():
        if metric["attempts"] >= 10 and metric["reach_rate"] < .40:
            return f"STOP teacher {kind}: {metric['successes']}/{metric['attempts']} <40%; inspect approach/action/landing in retained recordings"
    return None


def _compact_samples(directory):
    demos = module("nav_demos")
    times, positions, flags, jump, climb = [], [], [], [], []
    for records in demos.iter_chunks(directory, 32):
        times.extend(records["time_ms"].tolist())
        positions.extend(records["position_cm"].tolist())
        flags.extend(records["body"]["flags"].tolist())
        jump.extend(records["keys"]["jump"].tolist())
        climb.extend(records["keys"]["climb"].tolist())
    if not times:
        raise ValueError(f"Empty recording: {directory}")
    times = [value - times[0] for value in times]
    return times, positions, flags, jump, climb


def transition_evidence_legacy(route, trace, samples):
    """Require the scheduled id in the real teacher plan, a native action and landing.

    A successful goal alone, matching geometric proximity, or a button without
    airborne/climbing dynamics cannot count. Position/flags are outcome evidence;
    they do not enter the learner observability rule.
    """
    identifier, kind = route["transition_id"], route["type"]
    planned = [row for row in trace if any(link.get("id") == identifier and link.get("type") == kind
                                         for link in row.get("transitions", []))]
    active = [row for row in planned if row.get("active_transition_id") == identifier and
              row.get("active_transition_type") == kind and row.get("on_transition") is True]
    pressed = active if kind == "drop" else [row for row in active if row.get(kind) is True or row.get(kind) == 1]
    evidence = dict(planned=bool(planned), on_transition=bool(active), action=bool(pressed),
                    native_dynamics=False, landing=False, event_ms=None, landing_ms=None)
    if not planned:
        return evidence, "scheduled_transition_not_in_actual_path"
    if not active:
        return evidence, "approach_never_on_transition"
    if not pressed:
        return evidence, "teacher_never_pressed_" + kind
    times, positions, flags, jump, climb = samples
    start = [value*100 for value in route["transition_start_m"]]
    end = [value*100 for value in route["transition_end_m"]]
    dx, dz = end[0] - start[0], end[2] - start[2]
    horizontal_square = dx*dx + dz*dz
    for press in pressed:
        began = int(press["elapsed_ms"])
        witnessed = False
        for i, elapsed in enumerate(times):
            if elapsed < began or elapsed > began + 10000:
                continue
            airborne = flags[i][0] or flags[i][1]
            physical = ((airborne and positions[i][1] < start[1] - 20) if kind == "drop" else
                        bool(flags[i][3]) if kind == "climb" else bool(airborne))
            if physical and not witnessed:
                witnessed = True
                evidence["native_dynamics"] = True
                evidence["event_ms"] = elapsed
            along = ((positions[i][0] - start[0])*dx + (positions[i][2] - start[2])*dz) / horizontal_square if horizontal_square > 1 else 1.
            grounded = not (flags[i][0] or flags[i][1] or flags[i][3] or flags[i][4])
            if witnessed and grounded and along >= .80 and math.dist(positions[i], end) <= 150:
                evidence["landing"] = True
                evidence["landing_ms"] = elapsed
                return evidence, None
    return evidence, "landing_not_witnessed" if evidence["native_dynamics"] else "native_action_not_witnessed"


def _action_complete(frame):
    return not any(frame.get(name) for name in ("native_jump", "native_climb", "in_air", "falling"))


def _side_reference(frame):
    """Freeze scheduled-link ids; current ids in later frames remain global."""
    result = {name: frame.get(name, 0) for name in
              ("source_polygon", "end_polygon", "source_component", "end_component")}
    if result["source_component"] and result["end_component"] and result["source_component"] != result["end_component"]:
        result["method"] = "component"
    elif result["source_polygon"] and result["end_polygon"] and result["source_polygon"] != result["end_polygon"]:
        result["method"] = "polygon"
    else:
        result["method"] = "ambiguous"
    return result


def _frame_side(frame, reference):
    method = reference["method"]
    if method == "ambiguous":
        return "ambiguous"
    current = frame.get("current_" + method, 0)
    if not current:
        return "unknown"
    if current == reference["end_" + method]:
        return "landing"
    if current == reference["source_" + method]:
        return "takeoff"
    return "other"


def _landing_snapshot(frame, source, end, reference):
    position = frame["position_cm"]
    return dict(elapsed_ms=frame["elapsed_ms"], position_cm=list(position),
                landing_xz_distance_cm=math.hypot(position[0] - end[0], position[2] - end[2]),
                landing_y_offset_cm=position[1] - end[1], distance_3d_cm=math.dist(position, end),
                takeoff_xz_distance_cm=math.hypot(position[0] - source[0], position[2] - source[2]),
                takeoff_y_offset_cm=position[1] - source[1],
                supported=frame.get("supported"), cv_active=frame.get("cv_active"),
                landing_snap_m=frame.get("landing_snap_m"), landing_path_m=frame.get("landing_path_m"),
                landing_path_complete=frame.get("landing_path_complete"),
                current_polygon=frame.get("current_polygon", 0), current_component=frame.get("current_component", 0),
                side=_frame_side(frame, reference), action_complete=_action_complete(frame))


def _support_durations(frames, event_ms):
    """Integrate held frame flags until first support; support spans use samples.

    A continuous supported span is last-supported minus first-supported time,
    matching the probe's stable-since clock, without assuming support persisted
    into a later unsupported tick. Durations are observational metrics only.
    """
    supported = [frame for frame in frames if frame.get("supported")]
    first = supported[0]["elapsed_ms"] if supported else None
    end = first if first is not None else frames[-1]["elapsed_ms"] if frames else event_ms
    durations = dict(in_air_until_first_support_ms=0, falling_until_first_support_ms=0,
                     air_or_falling_until_first_support_ms=0, unsupported_until_first_support_ms=0)
    for current, following in zip(frames, frames[1:]):
        dt = max(0, min(following["elapsed_ms"], end) - max(current["elapsed_ms"], event_ms))
        for name, active in (("in_air_until_first_support_ms", current.get("in_air")),
                             ("falling_until_first_support_ms", current.get("falling")),
                             ("air_or_falling_until_first_support_ms", current.get("in_air") or current.get("falling")),
                             ("unsupported_until_first_support_ms", not current.get("supported"))):
            if active:
                durations[name] += dt
    longest, began = 0, None
    for frame in frames:
        if frame.get("supported"):
            if began is None:
                began = frame["elapsed_ms"]
            longest = max(longest, frame["elapsed_ms"] - began)
        else:
            began = None
    return dict(**durations, first_support_observed=first is not None,
                time_to_first_support_ms=first - event_ms if first is not None else None,
                support_duration_window_end_ms=end, max_continuous_support_ms=longest,
                supported_frames_after_event=len(supported),
                supported_cv_frames_after_event=sum(bool(frame.get("cv_active")) for frame in supported))


def _demo_landing_metrics(frames, event_ms, origin, source, end, reference):
    after = [frame for frame in frames if frame["elapsed_ms"] >= event_ms]
    snapshots = [_landing_snapshot(frame, source, end, reference) for frame in after]
    supported = [snapshot for snapshot in snapshots if snapshot["supported"]]
    complete = [snapshot for snapshot in snapshots if snapshot["action_complete"]]
    landings = [snapshot for snapshot in complete if snapshot["side"] == "landing"]
    first_landing = landings[0] if landings else None
    returned = [snapshot["elapsed_ms"] for snapshot in snapshots
                if first_landing and snapshot["elapsed_ms"] > first_landing["elapsed_ms"] and snapshot["side"] == "takeoff"]
    return dict(source_position_cm=list(source), end_position_cm=list(end),
                takeoff_position_cm=list(origin), side_reference=reference,
                first_support_after_event=supported[0] if supported else None,
                nearest_supported=min(supported, key=lambda row: row["distance_3d_cm"]) if supported else None,
                first_completed_after_event=complete[0] if complete else None,
                first_completed_landing=first_landing,
                final_pose=snapshots[-1] if snapshots else None,
                returned_to_takeoff=bool(returned), return_times_ms=returned,
                **_support_durations(after, event_ms))


def transition_evidence(route, trace, samples=None, frames=None):
    """Witness a native action and completed arrival on the landing mesh side.

    Goal success is checked by assess_episode(). The link probe's support,
    radius, snap and local-path thresholds are recorded as metrics only. A new
    teacher hint cannot change the frozen source/end mesh ids for this action.
    """
    if frames is None:
        evidence, failure = transition_evidence_legacy(route, trace, samples)
        evidence.update(source="navdemo_100ms_diagnostic", probe_equivalent=False)
        return evidence, failure
    identifier, kind = route["transition_id"], route["type"]
    planned = [row for row in trace if any(link.get("id") == identifier and link.get("type") == kind
                                         for link in row.get("transitions", []))]
    active = [row for row in planned if row.get("active_transition_id") == identifier and
              row.get("active_transition_type") == kind and row.get("on_transition") is True]
    presses = active if kind == "drop" else [row for row in active if bool(row.get(kind))]
    evidence = dict(source="transition_frames", success_criterion=SUCCESS_CRITERION, probe_equivalent=False,
                    planned=bool(planned), on_transition=bool(active), action=bool(presses),
                    native_dynamics=False, landing=False, event_ms=None, landing_ms=None,
                    takeoff_position_cm=None, max_rise_cm=0., max_drop_cm=0., landing_metrics={})
    if not planned:
        return evidence, "scheduled_transition_not_in_actual_path"
    if not active:
        return evidence, "approach_never_on_transition"
    if not presses:
        return evidence, "teacher_never_pressed_" + kind
    ordered = sorted(frames, key=lambda row: row["elapsed_ms"])
    if not ordered:
        return evidence, "physical_transition_frames_empty"
    required = ("source_polygon", "end_polygon", "current_polygon", "source_component",
                "end_component", "current_component", "cv_active")
    if any(any(name not in frame for name in required) for frame in ordered):
        return evidence, "transition_topology_or_cv_fields_missing"
    # A later on_transition trace must not reset a drop baseline after a jump.
    if kind == "drop":
        presses = presses[:1]
    for press in presses:
        began = int(press["elapsed_ms"])
        relevant = [row for row in ordered if began <= row["elapsed_ms"] <= began + 10000]
        exact = [row for row in relevant if row.get("transition_id") == identifier and row.get("type") == kind]
        if not exact:
            continue
        origin = press.get("position_cm", exact[0]["position_cm"])
        if len(origin) != 3 or not all(math.isfinite(value) for value in origin):
            raise ValueError("Invalid physical takeoff position in teacher trace")
        evidence["takeoff_position_cm"] = list(origin)
        reference = _side_reference(exact[0])
        source = exact[0].get("source_position_cm", [value*100 for value in route["transition_start_m"]])
        end = exact[0].get("end_position_cm", [value*100 for value in route["transition_end_m"]])
        max_rise = max_drop = 0.
        saw_jump = saw_climb = saw_air = False
        for frame in relevant:
            elapsed = frame["elapsed_ms"]
            if kind == "drop":
                contaminated = any(began <= row["elapsed_ms"] <= elapsed and
                                   (row.get("jump") or row.get("climb")) for row in trace)
                if samples is not None:
                    contaminated |= any(began <= moment <= elapsed and (jump or climb)
                                        for moment, jump, climb in zip(samples[0], samples[3], samples[4]))
                if contaminated:
                    return evidence, "drop_contaminated_by_jump_or_climb"
            if frame.get("transition_id") != identifier or frame.get("type") != kind:
                continue
            position = frame["position_cm"]
            max_rise, max_drop = max(max_rise, position[1] - origin[1]), max(max_drop, origin[1] - position[1])
            saw_jump |= bool(frame.get("native_jump"))
            saw_climb |= bool(frame.get("native_climb"))
            saw_air |= bool(frame.get("in_air") or frame.get("falling"))
            evidence.update(max_rise_cm=max_rise, max_drop_cm=max_drop)
            physical = (saw_jump and (saw_air or max_rise > 10) if kind == "jump" else
                        saw_climb if kind == "climb" else
                        saw_air and max_drop > 20)
            if not physical:
                continue
            metrics = _demo_landing_metrics(ordered, elapsed, origin, source, end, reference)
            # Peak excursion until the first completed pose belongs to this action;
            # subsequent walking does not enlarge the reported jump/climb height.
            first_complete = metrics["first_completed_after_event"]
            until = first_complete["elapsed_ms"] if first_complete else ordered[-1]["elapsed_ms"]
            action_positions = [row["position_cm"] for row in ordered if began <= row["elapsed_ms"] <= until]
            metrics["max_rise_cm"] = max(max(0., point[1] - origin[1]) for point in action_positions)
            metrics["max_drop_cm"] = max(max(0., origin[1] - point[1]) for point in action_positions)
            first_landing, final = metrics["first_completed_landing"], metrics["final_pose"]
            evidence.update(native_dynamics=True, event_ms=elapsed, landing=bool(first_landing),
                            landing_ms=first_landing["elapsed_ms"] if first_landing else None,
                            max_rise_cm=metrics["max_rise_cm"], max_drop_cm=metrics["max_drop_cm"], landing_metrics=metrics)
            if reference["method"] == "ambiguous":
                return evidence, "landing_side_ambiguous"
            if not first_landing:
                return evidence, "completed_landing_side_not_witnessed"
            if metrics["returned_to_takeoff"]:
                return evidence, "returned_to_takeoff_after_landing"
            if final is None or not final["action_complete"]:
                return evidence, "final_action_not_complete"
            if final["side"] != "landing":
                return evidence, "final_pose_not_on_landing_side"
            return evidence, None
    return evidence, "native_action_not_witnessed"


def assess_episode(directory, route, require_frames=True):
    required = (*REQUIRED, "transition-frames.jsonl") if require_frames else REQUIRED
    missing = [name for name in required if not (directory / name).is_file()]
    if missing:
        raise RuntimeError(f"Incomplete attempt {directory}: missing {', '.join(missing)}; retained, no retry")
    episode = json.loads((directory / "episode.json").read_text(encoding="utf-8"))
    recorded_format = (episode.get("demo_version") == 2 or
        (episode.get("demo_version") == 3 and episode.get("navigation_memory_version") == 4
         and episode.get("journey_memory") is True))
    if (episode.get("physics_mode") != "cv" or episode.get("frames", 0) <= 0 or
            episode.get("cv_frames", 0) <= 0 or not recorded_format or episode.get("teacher_version") != 2):
        # Native airborne/climbing states temporarily disable actual CV inside
        # MoveAlgo even with the episode's CV forcing active.
        raise RuntimeError(f"Attempt lacks teacher v2/CV evidence: {directory}")
    samples = _compact_samples(directory)
    if len(samples[0]) != episode.get("decisions"):
        raise RuntimeError(f"Record count differs from episode metadata: {directory}")
    frames_path = directory / "transition-frames.jsonl"
    frames = json_rows(frames_path) if frames_path.is_file() else None
    evidence, failure = transition_evidence(route, json_rows(directory / "teacher-paths.jsonl"), samples, frames)
    if evidence.get("success_criterion") != SUCCESS_CRITERION and failure is None:
        failure = "diagnostic_only_missing_transition_frames"
    if not failure and episode.get("success") is not True:
        failure = "goal_not_reached:" + episode.get("failure", "unspecified")
    initial_hp, final_hp = episode.get("initial_state", {}).get("hp"), episode.get("final_hp")
    episode.update({key: route[key] for key in ("index", "label", "start", "goal", "split", "site_id", "transition_id", "type", "region")})
    episode.update(route_index=route["index"], episode=str(directory), transition_evidence=evidence,
                   transition_success=failure is None, qualifying_success=failure is None, demo_success=failure is None,
                   success_criterion=SUCCESS_CRITERION, transition_failure=failure or "",
                   landing_metrics=evidence.get("landing_metrics", {}), health_metrics=dict(initial_hp=initial_hp, final_hp=final_hp),
                   planned_transition_ids=route.get("planned_transition_ids", [route["transition_id"]]),
                   requested_physics="cv", collection_limit_ms=LIMIT_MS,
                   action_decisions=dict(jump=sum(bool(value) for value in samples[3]), climb=sum(bool(value) for value in samples[4])))
    return episode


def existing_episodes(output, routes, enforce_pilot=True):
    episodes = []
    root = output / "episodes"
    if not root.exists():
        return episodes
    by_index = {route["index"]: route for route in routes}
    for directory in sorted(root.iterdir()):
        if not directory.is_dir() or not directory.name.isdecimal():
            raise RuntimeError(f"Unexpected artifact in episodes directory: {directory}")
        index = int(directory.name)
        if index not in by_index:
            raise RuntimeError(f"Existing episode is outside selected routes: {directory}")
        missing = [name for name in (*REQUIRED, "transition-frames.jsonl") if not (directory / name).is_file()]
        if missing:
            raise RuntimeError(f"Incomplete previous attempt {directory}; retained for diagnosis, no automatic retry")
        episode = json.loads((directory / "episode.json").read_text(encoding="utf-8"))
        for key in ("route_index", "label", "start", "goal", "split", "site_id", "transition_id", "type", "region"):
            expected = index if key == "route_index" else by_index[index][key]
            if episode.get(key) != expected:
                raise RuntimeError(f"Changed/unchecked previous attempt {directory}: {key}")
        if episode.get("success_criterion") != SUCCESS_CRITERION:
            raise RuntimeError(f"Previous attempt uses another success criterion: {directory}")
        if episode.get("collection_stop"):
            raise RuntimeError(episode["collection_stop"])
        if type(episode.get("transition_success")) is not bool:
            raise RuntimeError(f"Previous attempt lacks passage evidence: {directory}")
        episodes.append(episode)
    stop = pilot_stop(episodes) if enforce_pilot else None
    if stop:
        raise RuntimeError(stop)
    return episodes


def scenario_commands(args, route=None):
    if route is None:
        return [f"navmesh build {args.cache} {args.links}"]
    return [f"roads load {args.roads}", f"roads on {args.npc}",
            f"navdemos start-v2 {args.npc} {route['start']} {route['goal']} {LIMIT_MS}", "navdemos wait"]


def finish_collection(episodes, total_routes, complete, require_70pct=False):
    by_type = counts(episodes)
    passed = all(value["target_70pct"] for value in by_type.values())
    result = dict(complete=complete, attempts=len(episodes), routes=total_routes, success_criterion=SUCCESS_CRITERION,
                  target_70pct_passed=passed, by_type=by_type)
    print(json.dumps(result, ensure_ascii=False), flush=True)
    if require_70pct and not passed:
        raise RuntimeError("Teacher validation did not reach >=70% in every transition type")
    return result


def collect(args):
    if sys.platform != "linux":
        raise RuntimeError("Run collection on Ryzen inside gq batch")
    routes = load_routes(args.routes, args.links)
    args.output.mkdir(parents=True, exist_ok=True)
    args.cache.mkdir(parents=True, exist_ok=True)
    previous = existing_episodes(args.output, routes, enforce_pilot=not args.require_70pct)
    if previous and not args.resume:
        raise RuntimeError("Previous attempts exist; --resume preserves and counts them without retry")
    done = {episode["route_index"] for episode in previous}
    requested = selected_routes(routes, args.indices)
    selected = [route for route in requested if route["index"] not in done]
    if args.indices is None:
        selected = selected[:args.count]
    desired = {route["index"] for route in requested} if args.indices is not None else None
    if not selected:
        assessed = [episode for episode in previous if desired is None or episode["route_index"] in desired]
        return finish_collection(assessed, len(requested), True, args.require_70pct)
    check_storage(args.storage_root)
    collector = module("collect_nav_demos")
    worker = copy(args)
    worker.output = args.output / "runner-work"
    worker.output.mkdir(parents=True, exist_ok=True)
    prepare = collector.scenario_file(args, "jumps-record-prepare", scenario_commands(args))
    runner = collector.runner_type()(worker, prepare)
    try:
        runner.scenario(prepare, worker.output / "prepare", args.seed, timeout=720)
        collector.check_log(worker.output / "prepare")
        for route in selected:
            check_storage(args.storage_root)
            directory = args.output / "episodes" / f"{route['index']:04d}"
            if directory.exists():
                raise RuntimeError(f"Refusing to replace attempt {directory}")
            task = collector.scenario_file(args, f"jumps-record-{route['index']:04d}", scenario_commands(args, route))
            began = time.monotonic()
            # Runner.scenario issues reset before every episode, restoring the save.
            runner.scenario(task, directory, args.seed, timeout=args.watchdog_seconds)
            collector.check_log(directory)
            episode = assess_episode(directory, route)
            episode["collector_wall_s"] = time.monotonic() - began
            previous.append(episode)
            # Explicit repair validation must finish all selected routes, so
            # every type has its full denominator before the 70% gate. A normal
            # full collection keeps the early <40% pilot stop.
            stop = pilot_stop(previous) if not args.require_70pct else None
            if stop:
                episode["collection_stop"] = stop
            write_metadata(directory / "episode.json", episode)
            print(json.dumps(dict(route=route["index"], type=route["type"],
                                  transition_success=episode["transition_success"], failure=episode["transition_failure"],
                                  by_type=counts(previous), stop=stop), ensure_ascii=False), flush=True)
            if stop:
                raise RuntimeError(stop)
    finally:
        runner.close()
    assessed = [episode for episode in previous if desired is None or episode["route_index"] in desired]
    complete = desired <= {episode["route_index"] for episode in previous} if desired is not None else len(previous) == len(routes)
    return finish_collection(assessed, len(requested), complete, args.require_70pct)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("runner", "assets", "save", "cache", "links", "roads", "routes"):
        parser.add_argument(f"--{name}", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--storage-root", type=Path, help="10GiB scope; defaults to common nav-demos-jumps tree when output is inside it")
    parser.add_argument("--indices", help="comma-separated original route indices; overrides --count, never renumbers")
    parser.add_argument("--require-70pct", action="store_true", help="validation: finish all selected attempts (no early pilot stop), then fail unless every type reached 70%")
    parser.add_argument("--ini", type=Path)
    parser.add_argument("--npc", default="BAU_954_MALETH")
    parser.add_argument("--seed", type=int, default=19)
    parser.add_argument("--count", type=int, default=30, help="bounded new attempts in this gq request")
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--watchdog-seconds", type=int, default=900)
    args = parser.parse_args()
    args.physics = "cv"
    try:
        args.indices = parse_indices(args.indices)
        if args.storage_root is not None and not args.storage_root.is_absolute():
            parser.error("--storage-root must be absolute")
        args.storage_root = storage_root(args.output, args.storage_root)
    except ValueError as exc:
        parser.error(str(exc))
    if args.count < 1 or args.count > 300 or args.watchdog_seconds <= 0:
        parser.error("--count must be 1..300 and watchdog positive")
    for name, value in vars(args).items():
        if isinstance(value, Path) and not value.is_absolute():
            parser.error(f"--{name} must be absolute")
    for name in ("runner", "assets", "save", "links", "roads", "routes"):
        if not getattr(args, name).exists():
            parser.error(f"--{name} does not exist")
    forbidden = (Path.home() / "gothic-ai/nav-ppo").resolve()
    for value in (args.output, args.cache, args.storage_root):
        if value.resolve() == forbidden or forbidden in value.resolve().parents:
            parser.error("Use private output/cache outside nav-ppo")
    # A cache cannot alias raw output or mutate a source tree containing inputs.
    if args.cache.resolve() == args.output.resolve():
        parser.error("cache and output must differ")
    if any(char.isspace() for char in args.npc):
        parser.error("--npc must be a single scenario token")
    collect(args)


if __name__ == "__main__":
    main()
