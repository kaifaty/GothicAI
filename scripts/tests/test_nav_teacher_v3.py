"""Asset-free checks for scenario isolation and teacher-v3 measurements."""
import gzip
import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace

import numpy as np
import pytest

SPEC = importlib.util.spec_from_file_location("nav_teacher_v3", Path(__file__).resolve().parents[1] / "nav_teacher_v3.py")
teacher = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(teacher)
demos = teacher.module("nav_demos")


def sources():
    places = [dict(id=f"CAVE{n}", kind="cave", anchor=f"ANCHOR{n}", center=[n * 1000, 0, 0],
                   entrances=[dict(out=f"DOOR{n}", pos=[n * 1000, 0, 100])]) for n in range(12)]
    training = [dict(region=f"area{r}", start=f"A{r}-{n}", goal=f"B{r}-{n}") for r in range(6) for n in range(10)]
    return places, training


def test_seeded_candidates_split_whole_places_and_moving_regions_and_exclude_both_directions():
    places, training = sources()
    excluded = {("B0-0", "A0-0"), ("DOOR0", "ANCHOR1")}
    selected = teacher.build_candidates(places, training, excluded, 92, moving_per_region=2)
    assert selected == teacher.build_candidates(list(reversed(places)), list(reversed(training)), excluded, 92, moving_per_region=2)
    assert selected != teacher.build_candidates(places, training, excluded, 93, moving_per_region=2)
    teacher.validate_splits(selected)
    assert all(teacher.isolated(scenario, excluded) for scenario in selected)
    assert len({p["id"] for p in selected}) == len(selected)
    assert all(p["goal"].startswith("cm:") for p in selected if p["goal_kind"] == "object")
    assert not any(p["start"] == "ANCHOR1" and "DOOR0" in p.get("goal_aliases", []) for p in selected)
    assert sum(p["goal_kind"] == "point" for p in selected) == 24
    assert sum(p["goal_kind"] == "follow" for p in selected) == 12
    assert sum(p["goal_kind"] == "approach" for p in selected) == 12
    assert all(len(set(p["route"])) >= 3 for p in selected if p["goal_kind"] == "follow")
    for kind in ("object", "follow", "approach"):
        assert {p["split"] for p in selected if p["goal_kind"] == kind} == set(teacher.SPLITS)
    for region in {p["region"] for p in selected if p["goal_kind"] == "follow"}:
        assert len({p["split"] for p in selected if p["region"] == region and p["goal_kind"] in ("follow", "approach")}) == 1


def test_actual_mill_is_target_and_field_mentioning_mill_is_not():
    places, _ = sources()
    places += [dict(id="FARM1_MILL", kind="yard", anchor="MILL", center=[0, 0, 0]),
               dict(id="BIGFARM_FIELD", en="field by the mill", kind="field", anchor="FIELD", center=[0, 0, 0])]
    selected = teacher.object_targets(places)
    assert any(p["goal"] == "MILL" for p in selected)
    assert not any(p["goal"] == "FIELD" for p in selected)


def test_missing_exclusion_file_and_cross_split_group_are_errors(tmp_path):
    holdout = tmp_path / "holdout.jsonl"
    holdout.write_text('{"start":"A","goal":"B"}\n')
    with pytest.raises(ValueError, match="Missing exclusion"):
        teacher.exclusion_pairs(holdout, tmp_path / "validation.jsonl")
    with pytest.raises(ValueError, match="Split overlap"):
        teacher.validate_splits([dict(goal_kind="object", place_id="CAVE", split=split) for split in ("train", "test")])
    with pytest.raises(ValueError, match="Split overlap"):
        teacher.validate_splits([dict(goal_kind=kind, region="R", split=split)
                                 for kind, split in (("follow", "train"), ("approach", "test"))])


def test_scenario_selection_is_frozen_and_runner_tokens_are_safe(tmp_path):
    path = tmp_path / "scenarios.jsonl"
    teacher.write_selection(path, [dict(id="A", seed=1)])
    teacher.write_selection(path, [dict(id="A", seed=1)])
    with pytest.raises(ValueError, match="Refusing to replace"):
        teacher.write_selection(path, [dict(id="B", seed=1)])
    with pytest.raises(ValueError, match="whitespace"):
        teacher.token("A\nnavdemos wait")


@pytest.mark.parametrize("visible,complete,direct,detour,blocked,expected", [
    (True, True, 100, 150, False, True), (True, True, 100, 149, False, False),
    (True, True, 100, 120, True, True), (False, True, 100, 200, True, False),
    (True, False, 100, 200, True, False), (True, True, 0, 200, True, False)])
def test_object_qualification_requires_visibility_reachability_and_detour(visible, complete, direct, detour, blocked, expected):
    assert teacher.object_qualified(dict(visible=visible, detour_complete=complete, direct_cm=direct,
                                        detour_length_cm=detour, straight_blocked=blocked, start_supported=True)) is expected


def test_object_qualification_rejects_unsupported_start():
    assert not teacher.object_qualified(dict(visible=True, detour_complete=True, direct_cm=100,
                                            detour_length_cm=200, straight_blocked=True, start_supported=False))


def write_episode(path, kind="point", version=3):
    path.mkdir(parents=True)
    records = np.zeros(5, dtype=demos.RECORD_V2_DTYPE)
    records["time_ms"] = 500 + np.arange(5) * 100
    records["position_cm"][:, 0] = [0, 100, 100, 100, 100]
    records["roads"]["beyond_known"] = [1, 0, 0, 0, 0]
    records["eye"]["distance_cm"] = 1000
    records["keys"]["walk"] = 1
    records["keys"]["forward"] = [1, -1, 0, 1, 1]
    records["keys"]["strafe"] = [0, 1, -1, 0, 0]
    records["previous_keys"][1:] = records["keys"][:-1]
    metadata = dict(id="test", success=True, decisions=5, elapsed_ms=450, goal_kind=kind,
                    teacher_version=version, demo_version=2, detour_length_cm=50,
                    blocked_spans=2, blocked_ms=100, max_blocked_ms=100,
                    eye_layout=[[0, 0, 1000]] * demos.RAY_COUNT)
    teacher.write_json(path / "episode.json", metadata)
    with gzip.open(path / "samples.bin.gz", "wb") as stream:
        stream.write(demos.HEADER.pack(demos.MAGIC, 2, demos.RECORD_BYTES, demos.RAY_COUNT,
                                      demos.NEAR_SIZE, demos.FAR_SIZE, demos.PERIOD_MS))
        stream.write(records.tobytes())
    actions = np.zeros((6, 9))
    actions[:, 0] = [0, 100, 200, 300, 400, 450]
    for column, field in enumerate(("forward", "strafe", "turn", "walk", "jump", "climb", "head_yaw_deg", "head_pitch_deg"), 1):
        actions[:-1, column] = records["keys"][field]
    actions[-1, 4] = 1
    np.savetxt(path / "actions.txt", actions)
    trajectory = np.zeros((6, 5))
    trajectory[:, 0] = actions[:, 0]
    trajectory[:, 1] = [0, 100, 100, 100, 100, 120]
    np.savetxt(path / "trajectory.txt", trajectory)
    teacher.write_json(path / "contract.json", dict(records=5, mismatches=0, previous_mismatches=0))
    return records


def test_beyond_known_qualification_uses_distance_including_final_interval(tmp_path):
    path = tmp_path / "v2"
    write_episode(path, version=2)
    # Only one of five decisions is beyond-known, but it covers 100/120 cm.
    assert teacher.beyond_path_fraction(path) == pytest.approx(100 / 120)


def test_beyond_known_path_keeps_label_at_chunk_boundary():
    records = np.zeros(66, dtype=demos.RECORD_V2_DTYPE)
    records["position_cm"][64, 0] = 100
    records["position_cm"][65, 0] = 100
    records["roads"]["beyond_known"][63] = 1
    reader = SimpleNamespace(episode_version=lambda _: 2,
                             iter_chunks=lambda _: iter((records[:64], records[64:])))
    assert teacher.beyond_path_fraction("/does-not-exist", reader) == 1


def test_episode_metrics_validate_navdemo_and_count_actual_actions(tmp_path):
    path = tmp_path / "episode"
    write_episode(path)
    result = teacher.episode_metrics(path)
    assert result["backward"] == 1
    assert result["strafe"] == 2
    assert result["strafe_left"] == result["strafe_right"] == 1
    assert result["travelled_cm"] == 120
    assert result["length_over_detour"] == pytest.approx(2.4)
    assert result["contract_records"] == 5
    teacher.write_json(path / "contract.json", dict(records=4, mismatches=0, previous_mismatches=0))
    with pytest.raises(ValueError, match="incomplete contract"):
        teacher.episode_metrics(path)


def test_follow_occupancy_counts_lost_contact_and_partial_last_interval():
    result = teacher.follow_metrics([0, 100, 200, 300, 400], [900, 400, 100, 600, 300], 450)
    assert result["first_contact_ms"] == 100
    assert result["follow_observed_ms"] == 350
    assert result["follow_in_band_ms"] == 150
    assert result["follow_fraction"] == pytest.approx(150 / 350)
    assert teacher.follow_metrics([0, 100], [600, 700], 200)["follow_fraction"] is None
    with pytest.raises(ValueError, match="times"):
        teacher.follow_metrics([0, 100, 100], [100, 100, 100], 200)


def test_moving_metrics_use_recorded_target_distance(tmp_path):
    path = tmp_path / "follow"
    write_episode(path, kind="follow")
    trace = np.zeros((5, 6))
    trace[:, 0] = [0, 100, 200, 300, 400]
    trace[:, 4] = [900, 400, 100, 600, 300]
    np.savetxt(path / "target-trajectory.txt", trace)
    result = teacher.episode_metrics(path)
    assert result["follow_fraction"] == pytest.approx(150 / 350)
    assert result["approach_success"] is True


def test_comparison_conditions_on_successful_v2_not_all_trials():
    episodes = [dict(id=key, teacher_version=version, success=success, elapsed_ms=elapsed,
                     travelled_cm=length)
                for key, version, success, elapsed, length in (
                    ("A", 2, True, 100, 20), ("A", 3, True, 200, 30),
                    ("B", 2, True, 100, 20), ("B", 3, False, 500, 50),
                    ("C", 2, False, 100, 20), ("C", 3, False, 500, 50),
                    ("unpaired", 2, True, 100, 20))]
    result = teacher.comparison_metrics(episodes)
    assert result["paired"] == 3
    assert result["conditional_success_fraction"] == .5
    assert result["v2_successes"] == 2
    assert result["mean_elapsed_ratio_on_joint_success"] == 2
    assert result["mean_length_ratio_on_joint_success"] == 1.5


def test_disk_guard_reserves_next_episode_and_enforces_15gib(tmp_path, monkeypatch):
    (tmp_path / "data").write_bytes(b"12345")
    monkeypatch.setattr(teacher.shutil, "disk_usage", lambda _: SimpleNamespace(free=100 * 1024**3))
    assert teacher.disk_guard(tmp_path, 100, 5 + teacher.ESTIMATED_DECISION_BYTES) == 5
    with pytest.raises(ValueError, match="Disk budget"):
        teacher.disk_guard(tmp_path, 100, 5 + teacher.ESTIMATED_DECISION_BYTES - 1)
    with pytest.raises(ValueError, match="15 GiB"):
        teacher.disk_guard(tmp_path, 100, teacher.MAX_BYTES + 1)


def test_commands_keep_teacher_comparison_and_moving_seeds_explicit():
    point = dict(goal_kind="point", start="A", goal="B")
    assert teacher.start_command(point, 3, "NPC", 1000) == "navdemos start-v3 NPC A B 1000"
    assert teacher.start_command(point, 2, "NPC", 1000) == "navdemos start-v2 NPC A B 1000"
    point["goal_kind"] = "object"
    assert teacher.start_command(point, 2, "NPC", 1000) == "navdemos object-v3 2 NPC A B 1000"
    moving = dict(goal_kind="follow", start="A", goal="B", target="HERO", route=["B", "C", "D"], seed=42)
    assert teacher.start_command(moving, 3, "NPC", 1000) == "navdemos moving-v3 3 NPC A HERO B,C,D follow 1000 42"


def test_contract_rejects_unreplayed_previous_keys(tmp_path, monkeypatch):
    monkeypatch.setattr(teacher.subprocess, "run", lambda *a, **kw: SimpleNamespace(
        returncode=0, stdout=json.dumps(dict(records=10, mismatches=0, previous_mismatches=1)), stderr=""))
    with pytest.raises(ValueError, match="contract mismatch"):
        teacher.replay_contract("/contract", tmp_path)


def test_new_runner_supplies_default_ini_to_reused_runner(tmp_path, monkeypatch):
    args = SimpleNamespace(output=tmp_path, cache=tmp_path / "cache", links=tmp_path / "links", seed=42)
    prepare = tmp_path / "prepare.bench"
    calls = []

    class Runner:
        def __init__(self, worker, initial):
            # navmesh_measure.Runner reads this attribute before launching.
            assert worker.ini is None
            assert worker.output == tmp_path / "runner-work"
            assert worker.output.is_dir()
            assert initial == prepare

        def scenario(self, scenario, output, seed, timeout):
            calls.append((scenario, output, seed, timeout))

    collector = SimpleNamespace(scenario_file=lambda *a: prepare, runner_type=lambda: Runner,
                                check_log=lambda path: calls.append(path))
    monkeypatch.setattr(teacher, "module", lambda name: collector)
    runner = teacher.new_runner(args)
    assert isinstance(runner, Runner)
    assert not hasattr(args, "ini")
    assert calls == [(prepare, tmp_path / "runner-work/prepare", 42, 720), tmp_path / "runner-work/prepare"]


def test_expansion_keeps_initial_point_pool_and_fixed_group_splits():
    places, _ = sources()
    training = [dict(region=f"area{region}", start=f"A{region}-{index}", goal=f"B{region}-{index}")
                for region in range(6) for index in range(400)]
    original = teacher.build_candidates(places, training, set(), 92, starts_per_object=1)
    expanded = teacher.build_candidates(places, training, set(), 92, starts_per_object=2,
                                        point_count=2400, object_places=999)
    points = [value for value in expanded if value["goal_kind"] == "point"]
    assert len(points) == 2400
    assert teacher.Counter(value["region"] for value in points) == {f"area{region}": 400 for region in range(6)}
    assert {p["id"] for p in original if p["goal_kind"] == "point"} <= {p["id"] for p in points}
    old_splits = {p["place_id"]: p["split"] for p in original if p["goal_kind"] == "object"}
    assert all(p["split"] == old_splits[p["place_id"]] for p in expanded if p["goal_kind"] == "object")


def test_object_candidates_prefer_nearby_discovered_named_starts():
    places, training = sources()
    selected = teacher.build_candidates(places, training, set(), 92, starts_per_object=1,
                                        start_positions={"SURVEY_CLOSE": [600, 0, 100], "SURVEY_FAR": [15000, 0, 100]})
    cave = next(p for p in selected if p.get("place_id") == "CAVE0")
    assert cave["start"] == "SURVEY_CLOSE"
    assert cave["entrance_in"] is None
    assert cave["entrance_out"] == "DOOR0"


def test_start_position_loader_and_opening_orientation_probe_rows(tmp_path):
    points = tmp_path / "points.jsonl"
    points.write_text('{"name":"A","position":[1,2,3]}\n{"waypoint":"B","position_cm":[4,5,6]}\n')
    assert teacher.load_start_positions(points) == {"A": [1, 2, 3], "B": [4, 5, 6]}
    assert teacher.probe_row(dict(id="X", start="A", goal="cm:1,2,3", entrance_in="IN", entrance_out="OUT")) == "X A cm:1,2,3 IN OUT"
    assert teacher.probe_row(dict(id="X", start="A", goal="B")) == "X A B"


def test_qualification_uses_only_v2_success_and_reports_geometry_and_visibility_rejections(tmp_path, monkeypatch):
    pool = [dict(id=name, goal_kind=kind, start=f"{name}-START", goal=f"{name}-GOAL", region="area0",
                 split="train", **(dict(place_id=name) if kind == "object" else {}))
            for name, kind in (("ok", "point"), ("low", "point"), ("failed", "point"), ("missing", "point"),
                               ("edge", "object"), ("centre-failed", "object"), ("hidden", "object"))]
    refs = {}
    originals = {}
    for value in pool:
        if value["id"] in ("missing", "hidden"):
            continue
        directory = tmp_path / value["id"]
        metadata = dict(value, teacher_version=2, demo_version=2, success=value["id"] not in ("failed", "centre-failed"),
                        beyond_path_fraction=.3 if value["id"] == "low" else .31)
        teacher.write_json(directory / "episode.json", metadata)
        originals[directory / "episode.json"] = (directory / "episode.json").read_bytes()
        # Exercise both supported reference lookup forms.
        refs[value["id"] if value["goal_kind"] == "object" else (value["start"], value["goal"])] = directory
    probes = {value["id"]: dict(value, visible=value["id"] != "hidden", visible_centre=value["id"] == "centre-failed",
                                visible_edge=value["id"] == "edge", start_supported=True, detour_complete=True,
                                direct_cm=100, detour_length_cm=160)
              for value in pool if value["goal_kind"] == "object"}
    monkeypatch.setattr(teacher, "beyond_path_fraction", lambda _: pytest.fail("A finite cached fraction must be reused"))
    pool_before = json.dumps(pool, sort_keys=True)
    result, counts = teacher.qualified_candidates(pool, probes, refs)
    assert [value["id"] for value in result] == ["ok", "edge"]
    assert all(value["teacher_v2_success"] for value in result)
    assert counts["qualified"] == dict(point=1, object=1)
    assert counts["rejections"]["point"] == dict(beyond_at_most_30pct=1, v2_failed=1, missing_v2=1)
    assert counts["rejections"]["object"] == dict(v2_failed=1, not_visible=1)
    assert counts["v2_failures"] == dict(point=1, object=1)
    assert counts["object_visibility"] == dict(visible_centre=1, visible_edge_only=1, not_visible=1)
    assert counts["qualified_object_visibility"] == dict(visible_edge_only=1)
    assert counts["object_places"] == {"edge": 1}
    assert counts["point_regions"] == {"area0": 1}
    assert json.dumps(pool, sort_keys=True) == pool_before
    assert all(path.read_bytes() == content for path, content in originals.items())


def test_qualification_calculates_nonfinite_cache_and_checks_reference_identity(tmp_path, monkeypatch):
    value = dict(id="point", goal_kind="point", start="A", goal="B", region="area0", split="train")
    teacher.write_json(tmp_path / "episode.json", dict(value, teacher_version=2, success=True, beyond_path_fraction=float("nan")))
    calls = []
    monkeypatch.setattr(teacher, "beyond_path_fraction", lambda path: calls.append(path) or .31)
    result, _ = teacher.qualified_candidates([value], {}, {"point": tmp_path})
    assert result[0]["beyond_path_fraction"] == .31
    assert calls == [tmp_path]
    teacher.write_json(tmp_path / "episode.json", dict(value, goal="DIFFERENT", teacher_version=2, success=True))
    with pytest.raises(ValueError, match="Mismatched v2 reference"):
        teacher.qualified_candidates([value], {}, {"point": tmp_path})


def test_qualification_gate_requires_both_sample_sizes_and_all_regions():
    points = [dict(goal_kind="point", region=f"area{region}") for region in range(6) for _ in range(5)]
    objects = [dict(goal_kind="object", place_id=f"place{index}") for index in range(30)]
    required = [f"area{region}" for region in range(6)]
    assert teacher.qualification_gate(points + objects, required_point_regions=required,
                                      minimum_points_per_region=5, minimum_object_places=30)["object_places"] == 30
    with pytest.raises(ValueError, match="points=29/30"):
        teacher.qualification_gate(points[:-1] + objects, required_point_regions=required)
    with pytest.raises(ValueError, match="objects=29/30"):
        teacher.qualification_gate(points + objects[:-1], required_point_regions=required)
    missing_region = [dict(goal_kind="point", region="area0") for _ in range(30)]
    with pytest.raises(ValueError, match="underfilled_regions=.*area5"):
        teacher.qualification_gate(missing_region + objects, required_point_regions=required)
    with pytest.raises(ValueError, match="object_places=1/30"):
        teacher.qualification_gate(points + [dict(goal_kind="object", place_id="same")] * 30,
                                   minimum_object_places=30)


def test_resume_reuses_baseline_after_qualification_without_rewriting_metadata(tmp_path, monkeypatch):
    original = dict(id="point", goal_kind="point", split="train", region="area0", start="A", goal="B", seed=92)
    enriched = dict(original, teacher_v2_episode="/reference", teacher_v2_success=True, beyond_path_fraction=.8)
    directory = teacher.episode_directory(tmp_path, original, 2)
    write_episode(directory, version=2)
    metadata = dict(teacher.read_json(directory / "episode.json"), **original)
    teacher.write_json(directory / "episode.json", metadata)
    before = (directory / "episode.json").read_bytes()
    calls = []
    monkeypatch.setattr(teacher, "chosen_scenarios", lambda _: [enriched])
    monkeypatch.setattr(teacher, "checked_paths", lambda _: None)
    monkeypatch.setattr(teacher, "new_runner", lambda _: SimpleNamespace(close=lambda: calls.append("close")))
    monkeypatch.setattr(teacher, "module", lambda _: SimpleNamespace(summarize=lambda path, validate: calls.append((path, validate))))
    teacher.run(SimpleNamespace(output=tmp_path, teacher_version=2, resume=True))
    assert calls == [(directory, True), "close"]
    assert (directory / "episode.json").read_bytes() == before
    assert teacher.same_scenario(metadata, enriched, 2)
    assert not teacher.same_scenario(metadata, dict(enriched, seed=93), 2)
    assert not teacher.same_scenario(metadata, dict(enriched, start="OTHER"), 2)


def test_reference_index_preserves_distinct_ids_for_the_same_named_pair(tmp_path):
    for kind in ("point", "object"):
        directory = tmp_path / kind
        teacher.write_json(directory / "episode.json", dict(id=kind, start="A", goal="B", teacher_version=2, goal_kind=kind))
        (directory / "samples.bin.gz").touch()
    refs = teacher.references(tmp_path)
    assert refs["point"] == tmp_path / "point"
    assert refs["object"] == tmp_path / "object"
