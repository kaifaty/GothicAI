"""Bounded collection safety and physical transition evidence; no engine/assets."""
import gzip
import importlib.util
import json
from pathlib import Path
import sys
from types import SimpleNamespace

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import nav_jump_record as recorder
import nav_demos as demos


def route(index=0, kind="jump"):
    return dict(index=index, label=f"route-{index}", type=kind, transition_id=index+7,
                start="cm:-100,0,0", goal="cm:300,0,0", site_id=f"site-{index}",
                split="train", region="city", transition_start_m=[0, 0, 0], transition_end_m=[2, 0, 0])


def trace(value, **change):
    return [dict(elapsed_ms=0, active_transition_id=value["transition_id"], active_transition_type=value["type"],
                 on_transition=True, jump=value["type"] == "jump", climb=value["type"] == "climb",
                 transitions=[dict(id=value["transition_id"], type=value["type"])], **change)]


def samples(kind="jump"):
    flags = [[0]*5 for _ in range(4)]
    if kind == "climb":
        flags[1][3] = 1
    else:
        flags[1][0] = flags[1][1] = 1
    positions = [[0, 0, 0], [80, -50 if kind == "drop" else 70, 0], [200, 0, 0], [230, 0, 0]]
    return [0, 100, 200, 300], positions, flags, [kind == "jump", False, False, False], [kind == "climb", False, False, False]


@pytest.mark.parametrize("kind", recorder.TYPES)
def test_requires_planned_id_action_native_dynamics_and_landing(kind):
    value = route(kind=kind)
    evidence, failure = recorder.transition_evidence(value, trace(value), samples(kind))
    assert failure is None
    assert evidence["native_dynamics"] and evidence["landing"]
    assert evidence["event_ms"] == 100 and evidence["landing_ms"] == 200
    wrong = trace(value)
    wrong[0]["transitions"][0]["id"] += 100
    assert recorder.transition_evidence(value, wrong, samples(kind))[1] == "scheduled_transition_not_in_actual_path"
    static = samples(kind)
    static[2][:] = [[0]*5 for _ in range(4)]
    assert recorder.transition_evidence(value, trace(value), static)[1] == "native_action_not_witnessed"
    never_land = samples(kind)
    never_land[1][2:] = [[0, 0, 0], [0, 0, 0]]
    assert recorder.transition_evidence(value, trace(value), never_land)[1] == "landing_not_witnessed"


def test_goal_success_or_action_near_link_is_not_passage():
    value = route()
    wrong = trace(value)
    wrong[0]["active_transition_id"] = 99
    assert recorder.transition_evidence(value, wrong, samples())[1] == "approach_never_on_transition"
    wrong = trace(value)
    wrong[0]["jump"] = False
    assert recorder.transition_evidence(value, wrong, samples())[1] == "teacher_never_pressed_jump"


def test_storage_counts_tree_and_reserves_next_whole_episode(tmp_path):
    (tmp_path / "data").write_bytes(b"x"*128)
    assert recorder.tree_bytes(tmp_path) == 128
    free_disk = lambda _: SimpleNamespace(free=recorder.MIN_FREE_BYTES)
    metrics = recorder.check_storage(tmp_path, free_disk)
    assert metrics["existing_bytes"] == 128
    assert metrics["reserved_bytes"] > 2401*demos.RECORD_BYTES + 16*1024**2
    with pytest.raises(RuntimeError, match="STOP disk"):
        recorder.check_storage(tmp_path, lambda _: SimpleNamespace(free=recorder.MIN_FREE_BYTES - 1))
    # Sparse file tests the threshold without allocating gigabytes.
    with (tmp_path / "large").open("wb") as stream:
        stream.truncate(recorder.MAX_BYTES - recorder.RESERVE_BYTES)
    with pytest.raises(RuntimeError, match="STOP storage"):
        recorder.check_storage(tmp_path, free_disk)


def test_pilot_counts_every_failure_and_stops_below_not_at_40_percent():
    episodes = [dict(type="jump", transition_success=i < 3, transition_failure="approach") for i in range(10)]
    assert recorder.pilot_stop(episodes[:9]) is None
    assert "3/10" in recorder.pilot_stop(episodes)
    episodes[3]["transition_success"] = True
    assert recorder.pilot_stop(episodes) is None
    metrics = recorder.counts(episodes)["jump"]
    assert metrics["attempts"] == 10 and metrics["successes"] == 4
    assert metrics["reach_rate"] == .4 and not metrics["target_70pct"]


def test_collection_order_checks_each_type_early_without_dropping_routes():
    values = [route(index=i, kind=kind) for i, kind in enumerate(["jump"]*15 + ["climb"]*15 + ["drop"]*15)]
    ordered = recorder.collection_order(values)
    assert [value["type"] for value in ordered[:6]] == ["jump", "climb", "drop"]*2
    assert {value["index"] for value in ordered} == set(range(45))
    assert len(ordered) == 45


def test_scenario_uses_unchanged_roads_teacher_v2_and_240s_limit():
    args = SimpleNamespace(cache=Path("/own/cache"), links=Path("/own/accepted-links-cv.txt"), roads=Path("/own/roads.json"), npc="NPC")
    assert recorder.scenario_commands(args) == ["navmesh build /own/cache /own/accepted-links-cv.txt"]
    commands = recorder.scenario_commands(args, route())
    assert commands == ["roads load /own/roads.json", "roads on NPC", "navdemos start-v2 NPC cm:-100,0,0 cm:300,0,0 240000", "navdemos wait"]


def test_resume_preserves_failed_attempt_and_cannot_pass_persisted_stop(tmp_path):
    value = route()
    directory = tmp_path / "episodes/0000"
    directory.mkdir(parents=True)
    for name in (*recorder.REQUIRED, "transition-frames.jsonl"):
        (directory / name).write_text("")
    episode = dict(value, route_index=0, transition_success=False, qualifying_success=False,
                   transition_failure="landing_not_witnessed", collection_stop="STOP teacher jump: 3/10",
                   success_criterion=recorder.SUCCESS_CRITERION)
    (directory / "episode.json").write_text(json.dumps(episode))
    with pytest.raises(RuntimeError, match="STOP teacher"):
        recorder.existing_episodes(tmp_path, [value])
    del episode["collection_stop"]
    (directory / "episode.json").write_text(json.dumps(episode))
    assert recorder.existing_episodes(tmp_path, [value])[0]["transition_success"] is False
    (directory / "samples.bin.gz").unlink()
    with pytest.raises(RuntimeError, match="no automatic retry"):
        recorder.existing_episodes(tmp_path, [value])


def native_frames(value, kind=None):
    kind = kind or value["type"]
    origin = [0., 100., 0.]
    # The link mesh y deliberately differs from actual onstart y.
    target = [coordinate*100 for coordinate in value["transition_end_m"]]
    middle = [80., 160. if kind != "drop" else 50., 0.]
    common = dict(transition_id=value["transition_id"], type=kind,
                  native_jump=False, native_climb=False, in_air=False, falling=False,
                  supported=False, landing_path_complete=False, landing_path_m=99., landing_snap_m=99., cv_active=True,
                  source_polygon=11, end_polygon=22, current_polygon=11,
                  source_component=1, end_component=2, current_component=1,
                  side_method="component", source_position_cm=[0., 0., 0.], end_position_cm=target)
    return [dict(common, elapsed_ms=0, position_cm=origin),
            dict(common, elapsed_ms=16, position_cm=middle,
                 native_jump=kind == "jump", native_climb=kind == "climb",
                 in_air=kind == "drop", falling=kind == "drop"),
            dict(common, elapsed_ms=32, position_cm=target, supported=True, current_polygon=22, current_component=2,
                 landing_path_complete=True, landing_path_m=.25, landing_snap_m=.10)]


def physical_trace(value):
    result = trace(value)
    result[0]["position_cm"] = [0., 100., 0.]
    return result


@pytest.mark.parametrize("kind", recorder.TYPES)
def test_physical_frames_witness_native_action_and_completed_landing_side(kind):
    value = route(kind=kind)
    evidence, failure = recorder.transition_evidence(value, physical_trace(value), frames=native_frames(value))
    assert failure is None
    assert evidence["source"] == "transition_frames" and not evidence["probe_equivalent"]
    assert evidence["success_criterion"] == recorder.SUCCESS_CRITERION
    assert evidence["takeoff_position_cm"] == [0., 100., 0.]
    assert evidence["event_ms"] == 16 and evidence["landing_ms"] == 32
    if kind == "drop":
        # y=50 is above mesh takeoff y=0, but below actual y=100.
        assert evidence["max_drop_cm"] == 100


def test_jump_requires_native_flag_even_when_100ms_flags_or_height_suggest_flight():
    value = route()
    frames = native_frames(value)
    # BS_JUMP absent: rise and inAir alone are not a native jump witness.
    frames[1].update(native_jump=False, in_air=True)
    assert recorder.transition_evidence(value, physical_trace(value), samples(), frames)[1] == "native_action_not_witnessed"
    # BS_CLIMB cannot be borrowed to explain a jump.
    frames[1].update(native_climb=True, in_air=False)
    assert recorder.transition_evidence(value, physical_trace(value), samples(), frames)[1] == "native_action_not_witnessed"
    # Native jump with >10cm rise can be real even if 100ms/inAir flags miss it.
    frames[1].update(native_jump=True, native_climb=False)
    assert recorder.transition_evidence(value, physical_trace(value), frames=frames)[1] is None


def test_demo_climb_witness_is_native_bs_climb_even_with_no_measured_rise():
    value = route(kind="climb")
    frames = native_frames(value)
    frames[1]["position_cm"][1] = 100 # Same actual height as the takeoff.
    evidence, failure = recorder.transition_evidence(value, physical_trace(value), frames=frames)
    assert failure is None
    assert evidence["native_dynamics"] and evidence["max_rise_cm"] == 0
    frames[1]["native_climb"] = False
    frames[1]["position_cm"][1] = 160 # Height alone still cannot prove climbing.
    assert recorder.transition_evidence(value, physical_trace(value), frames=frames)[1] == "native_action_not_witnessed"


@pytest.mark.parametrize("change", [
    dict(supported=False), dict(landing_path_complete=False),
    dict(landing_path_m=8.), dict(landing_snap_m=.42),
    dict(position_cm=[400., 0., 0.]), dict(transition_id=999, type="climb"),
])
def test_demo_side_accepts_probe_distance_snap_path_and_support_as_metrics(change):
    value = route()
    frames = native_frames(value)
    frames[-1].update(change)
    evidence, failure = recorder.transition_evidence(value, physical_trace(value), frames=frames)
    assert failure is None
    assert evidence["landing_metrics"]["final_pose"]["side"] == "landing"
    if "landing_snap_m" in change:
        assert evidence["landing_metrics"]["final_pose"]["landing_snap_m"] == .42
    if "position_cm" in change:
        assert evidence["landing_metrics"]["final_pose"]["distance_3d_cm"] == 200
    if change.get("supported") is False:
        assert evidence["landing_metrics"]["nearest_supported"] is None


def test_native_landing_metrics_retain_first_support_distance_and_cv():
    value = route()
    frames = native_frames(value)
    frames[-1].update(position_cm=[400., 10., 0.], landing_snap_m=.42)
    evidence, failure = recorder.transition_evidence(value, physical_trace(value), frames=frames)
    assert failure is None
    metric = evidence["landing_metrics"]["first_support_after_event"]
    assert metric["elapsed_ms"] == 32
    assert metric["landing_xz_distance_cm"] == 200
    assert metric["landing_y_offset_cm"] == 10
    assert metric["distance_3d_cm"] == pytest.approx(np.hypot(200, 10))
    assert metric["landing_snap_m"] == .42 and metric["cv_active"] is True


def test_native_event_must_belong_to_exact_scheduled_id_and_type():
    value = route()
    for field, replacement in (("transition_id", 999), ("type", "climb")):
        frames = native_frames(value)
        frames[1][field] = replacement
        assert recorder.transition_evidence(value, physical_trace(value), frames=frames)[1] == "native_action_not_witnessed"


def test_drop_cannot_borrow_jumping_or_climbing_between_onstart_and_event():
    value = route(kind="drop")
    for key in ("jump", "climb"):
        decision_trace = physical_trace(value)
        decision_trace.append(dict(elapsed_ms=8, **{key: True}))
        assert recorder.transition_evidence(value, decision_trace, frames=native_frames(value))[1] == "drop_contaminated_by_jump_or_climb"
    contaminated = samples("drop")
    contaminated[3][0] = True
    assert recorder.transition_evidence(value, physical_trace(value), contaminated, native_frames(value))[1] == "drop_contaminated_by_jump_or_climb"


def test_storage_root_defaults_to_common_tree_and_includes_prior_attempts(tmp_path, monkeypatch):
    common = tmp_path / "nav-demos-jumps"
    repair = common / "repair-1"
    final = common / "final"
    repair.mkdir(parents=True)
    final.mkdir()
    (common / "original.bin").write_bytes(b"old"*100)
    (repair / "pilot.bin").write_bytes(b"new"*100)
    monkeypatch.setattr(recorder, "DEFAULT_OUTPUT", common)
    scope = recorder.storage_root(final)
    assert scope == common
    assert recorder.check_storage(scope, lambda _: SimpleNamespace(free=recorder.MIN_FREE_BYTES))["existing_bytes"] == 600
    external = tmp_path / "external"
    assert recorder.storage_root(external) == external
    with pytest.raises(ValueError, match="must contain"):
        recorder.storage_root(repair, final)


def test_original_indices_are_selected_in_requested_order_without_renumbering():
    values = [route(3), route(80), route(105)]
    assert recorder.parse_indices("105,3") == [105, 3]
    selected = recorder.selected_routes(values, recorder.parse_indices("105,3"))
    assert [row["index"] for row in selected] == [105, 3]
    for value in ("1,1", "-1", "", "abc", "1,"):
        with pytest.raises(ValueError, match="indices"):
            recorder.parse_indices(value)
    with pytest.raises(ValueError, match="Unknown original"):
        recorder.selected_routes(values, [0])


def test_load_routes_preserves_sparse_original_indices(tmp_path, monkeypatch):
    values = [route(3), route(80)]
    for value in values:
        value["teacher_plan"] = {}
    source = tmp_path / "routes.jsonl"
    source.write_text("".join(json.dumps(value) + "\n" for value in values))
    links = [dict(id=value["transition_id"], type=1, start=value["transition_start_m"], end=value["transition_end_m"]) for value in values]
    monkeypatch.setattr(recorder, "module", lambda name: SimpleNamespace(links=lambda _: links, TYPES={1: "jump"})
                        if name == "nav_jumps" else SimpleNamespace(qualifying_plan=lambda *_: None))
    assert [row["index"] for row in recorder.load_routes(source, tmp_path / "links.txt")] == [3, 80]


def test_70_percent_gate_reports_all_attempts_and_fails_after_evaluation(capsys):
    episodes = [dict(type=kind, transition_success=i < 7, transition_failure="miss")
                for kind in recorder.TYPES for i in range(10)]
    assert recorder.finish_collection(episodes, 30, True, True)["target_70pct_passed"]
    episodes[-4]["transition_success"] = False
    with pytest.raises(RuntimeError, match=">=70%"):
        recorder.finish_collection(episodes, 30, True, True)
    payload = json.loads(capsys.readouterr().out.splitlines()[-1])
    assert payload["attempts"] == 30
    assert payload["by_type"]["drop"]["successes"] == 6
    assert not payload["target_70pct_passed"]


def test_cli_accepts_original_indices_storage_scope_and_gate(tmp_path, monkeypatch):
    source_paths = {}
    for name in ("runner", "assets", "save", "links", "roads", "routes"):
        source_paths[name] = tmp_path / name
        source_paths[name].touch()
    scope = tmp_path / "all-attempts"
    output = scope / "repair-1"
    arguments = ["nav_jump_record.py"]
    for name, path in source_paths.items():
        arguments.extend(["--" + name, str(path)])
    arguments.extend(["--cache", str(tmp_path / "cache"), "--output", str(output),
                      "--storage-root", str(scope), "--indices", "80,3", "--count", "30", "--require-70pct"])
    monkeypatch.setattr(sys, "argv", arguments)
    seen = []
    monkeypatch.setattr(recorder, "collect", lambda args: seen.append(args))
    recorder.main()
    assert seen[0].indices == [80, 3]
    assert seen[0].output == output and seen[0].storage_root == scope
    assert seen[0].require_70pct


def test_validation_collects_all_original_30_before_failing_70pct_gate(tmp_path, monkeypatch, capsys):
    values = [route(i, kind) for i, kind in enumerate(["jump"]*10 + ["climb"]*10 + ["drop"]*10)]
    seen = []
    class FakeRunner:
        def __init__(self, *args):
            pass
        def scenario(self, scenario, output, *args, **kwargs):
            output.mkdir(parents=True, exist_ok=True)
        def close(self):
            seen.append("closed")
    fake_collector = SimpleNamespace(scenario_file=lambda *args: tmp_path / "scenario.bench",
                                     runner_type=lambda: FakeRunner, check_log=lambda *_: None)
    monkeypatch.setattr(recorder, "module", lambda _: fake_collector)
    monkeypatch.setattr(recorder, "load_routes", lambda *_: values)
    monkeypatch.setattr(recorder, "check_storage", lambda *_: None)
    monkeypatch.setattr(sys, "platform", "linux")
    def assess(directory, value):
        seen.append(value["index"])
        return dict(value, route_index=value["index"], transition_success=value["type"] != "jump",
                    transition_failure="native_action_not_witnessed" if value["type"] == "jump" else "")
    monkeypatch.setattr(recorder, "assess_episode", assess)
    args = SimpleNamespace(routes=tmp_path / "routes", links=tmp_path / "links", output=tmp_path / "repair-1",
                           cache=tmp_path / "cache", roads=tmp_path / "roads", npc="NPC", storage_root=tmp_path, indices=list(range(30)),
                           count=30, require_70pct=True, resume=False, seed=19, watchdog_seconds=10)
    with pytest.raises(RuntimeError, match=">=70%"):
        recorder.collect(args)
    assert sorted(value for value in seen if isinstance(value, int)) == list(range(30))
    assert seen[-1] == "closed"
    assert len(list((args.output / "episodes").glob("*/episode.json"))) == 30
    assert not any(json.loads(path.read_text()).get("collection_stop")
                   for path in (args.output / "episodes").glob("*/episode.json"))
    report = json.loads(capsys.readouterr().out.splitlines()[-1])
    assert report["attempts"] == 30 and report["by_type"]["jump"]["successes"] == 0
    assert all(metric["attempts"] == 10 for metric in report["by_type"].values())


@pytest.mark.parametrize("format_version", [2, 3])
def test_new_episode_requires_frames_and_ignores_incomplete_100ms_native_flags(tmp_path, format_version):
    value = route()
    records = np.zeros(4, dtype=demos.RECORD_DTYPES[format_version])
    records["time_ms"] = [1000, 1100, 1200, 1300]
    # Deliberately no airborne/climbing flags at the 100ms sample cadence.
    records["keys"]["jump"][0] = 1
    with gzip.open(tmp_path / "samples.bin.gz", "wb") as stream:
        stream.write(demos.HEADER.pack(demos.MAGIC, format_version, demos.RECORD_BYTES_BY_VERSION[format_version], demos.RAY_COUNT,
                                      demos.NEAR_SIZE, demos.FAR_SIZE, demos.PERIOD_MS))
        stream.write(records.tobytes())
    for name in ("actions.txt", "trajectory.txt"):
        (tmp_path / name).write_text("")
    (tmp_path / "teacher-paths.jsonl").write_text("".join(json.dumps(row) + "\n" for row in physical_trace(value)))
    metadata = dict(physics_mode="cv", frames=60, cv_frames=10, demo_version=format_version, teacher_version=2,
                    navigation_memory_version=4 if format_version == 3 else 3, journey_memory=format_version == 3,
                    decisions=4, success=True, initial_state=dict(hp=100), final_hp=100)
    (tmp_path / "episode.json").write_text(json.dumps(metadata))
    with pytest.raises(RuntimeError, match="transition-frames"):
        recorder.assess_episode(tmp_path, value)
    (tmp_path / "transition-frames.jsonl").write_text("".join(json.dumps(row) + "\n" for row in native_frames(value)))
    episode = recorder.assess_episode(tmp_path, value)
    assert episode["success"] and episode["qualifying_success"] and episode["transition_success"]
    assert episode["transition_evidence"]["source"] == "transition_frames"
    assert episode["success_criterion"] == "demo_goal_action_landing_side_v1"
    assert episode["demo_success"]
    metadata["success"] = False
    (tmp_path / "episode.json").write_text(json.dumps(metadata))
    assert recorder.assess_episode(tmp_path, value)["transition_failure"].startswith("goal_not_reached:")
    metadata.update(success=True, final_hp=1)
    (tmp_path / "episode.json").write_text(json.dumps(metadata))
    assert recorder.assess_episode(tmp_path, value)["transition_success"]
    if format_version == 3:
        for invalid in (dict(navigation_memory_version=3), dict(journey_memory=False)):
            (tmp_path / "episode.json").write_text(json.dumps(dict(metadata, **invalid)))
            with pytest.raises(RuntimeError, match="teacher v2/CV evidence"):
                recorder.assess_episode(tmp_path, value)


@pytest.mark.parametrize("component,polygon", [(1, 11), (99, 99), (0, 0)])
def test_wrong_bank_other_component_and_unknown_ground_cannot_prove_landing(component, polygon):
    value = route()
    frames = native_frames(value)
    frames[-1].update(current_component=component, current_polygon=polygon)
    assert recorder.transition_evidence(value, physical_trace(value), frames=frames)[1] == "completed_landing_side_not_witnessed"


def test_return_to_takeoff_after_completed_landing_is_rejected_even_if_goal_side_later():
    value = route()
    frames = native_frames(value)
    frames.append(dict(frames[-1], elapsed_ms=48, current_component=1, current_polygon=11,
                       in_air=True, position_cm=[0., 100., 0.]))
    frames.append(dict(frames[2], elapsed_ms=64))
    evidence, failure = recorder.transition_evidence(value, physical_trace(value), frames=frames)
    assert failure == "returned_to_takeoff_after_landing"
    assert evidence["landing_metrics"]["returned_to_takeoff"]
    assert evidence["landing_metrics"]["return_times_ms"] == [48]


@pytest.mark.parametrize("flag", ["native_jump", "native_climb", "in_air", "falling"])
def test_body_still_in_native_action_does_not_count_as_completed_landing(flag):
    value = route()
    frames = native_frames(value)
    frames[-1][flag] = True
    assert recorder.transition_evidence(value, physical_trace(value), frames=frames)[1] == "completed_landing_side_not_witnessed"


def test_final_pose_must_be_completed_even_after_an_earlier_completed_landing():
    value = route()
    frames = native_frames(value)
    frames.append(dict(frames[-1], elapsed_ms=48, in_air=True))
    assert recorder.transition_evidence(value, physical_trace(value), frames=frames)[1] == "final_action_not_complete"


def test_same_component_uses_strict_end_polygon_and_rejects_shared_polygon_ambiguity():
    value = route()
    frames = native_frames(value)
    for frame in frames:
        frame.update(source_component=1, end_component=1, current_component=1)
    assert recorder.transition_evidence(value, physical_trace(value), frames=frames)[1] is None
    frames[-1]["current_polygon"] = 33
    assert recorder.transition_evidence(value, physical_trace(value), frames=frames)[1] == "completed_landing_side_not_witnessed"
    for frame in frames:
        frame.update(source_polygon=11, end_polygon=11, current_polygon=11)
    assert recorder.transition_evidence(value, physical_trace(value), frames=frames)[1] == "landing_side_ambiguous"


def test_global_current_ids_keep_original_side_after_hint_changes():
    value = route()
    frames = native_frames(value)
    frames[-1].update(transition_id=123456, type="drop", source_polygon=333, end_polygon=444,
                      source_component=333, end_component=444, side_method="component")
    evidence, failure = recorder.transition_evidence(value, physical_trace(value), frames=frames)
    assert failure is None
    assert evidence["landing_metrics"]["side_reference"]["end_component"] == 2
    assert evidence["landing_metrics"]["final_pose"]["current_component"] == 2


def test_new_criterion_requires_topology_and_cv_fields():
    value = route()
    for field in ("current_polygon", "source_component", "cv_active"):
        frames = native_frames(value)
        del frames[-1][field]
        assert recorder.transition_evidence(value, physical_trace(value), frames=frames)[1] == "transition_topology_or_cv_fields_missing"


def test_resume_refuses_previous_success_criterion(tmp_path):
    value = route()
    directory = tmp_path / "episodes/0000"
    directory.mkdir(parents=True)
    for name in (*recorder.REQUIRED, "transition-frames.jsonl"):
        (directory / name).write_text("")
    episode = dict(value, route_index=0, transition_success=True,
                   success_criterion="native_probe_60cm_legacy")
    (directory / "episode.json").write_text(json.dumps(episode))
    with pytest.raises(RuntimeError, match="another success criterion"):
        recorder.existing_episodes(tmp_path, [value])


def test_landing_metrics_measure_air_support_durations_and_cv_frame_counts():
    value = route()
    frames = native_frames(value)
    end_pose = frames[-1]
    frames = frames[:2] + [
        dict(end_pose, elapsed_ms=40, supported=False, in_air=True, falling=True),
        dict(end_pose, elapsed_ms=88, supported=False, falling=True),
        dict(end_pose, elapsed_ms=100),
        dict(end_pose, elapsed_ms=180, cv_active=False),
        dict(end_pose, elapsed_ms=420),
        dict(end_pose, elapsed_ms=450, supported=False),
        dict(end_pose, elapsed_ms=500),
        dict(end_pose, elapsed_ms=540),
    ]
    frames[1].update(in_air=True, falling=False)
    evidence, failure = recorder.transition_evidence(value, physical_trace(value), frames=frames)
    assert failure is None
    metrics = evidence["landing_metrics"]
    assert metrics["time_to_first_support_ms"] == 84
    assert metrics["in_air_until_first_support_ms"] == 72
    assert metrics["falling_until_first_support_ms"] == 60
    assert metrics["air_or_falling_until_first_support_ms"] == 84
    assert metrics["unsupported_until_first_support_ms"] == 84
    assert metrics["max_continuous_support_ms"] == 320
    assert metrics["supported_frames_after_event"] == 5
    assert metrics["supported_cv_frames_after_event"] == 4
    assert metrics["first_support_observed"]
