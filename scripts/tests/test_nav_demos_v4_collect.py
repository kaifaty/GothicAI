"""Asset-free selection, storage, eligibility and resume checks for demos-v4."""
from collections import Counter
from copy import deepcopy
from contextlib import nullcontext
import gzip
import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace

import pytest

SPEC = importlib.util.spec_from_file_location("nav_demos_v4_collect", Path(__file__).resolve().parents[1] / "nav_demos_v4_collect.py")
collect = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(collect)
v3 = collect.v3


def point(index=0, region="market", split="train"):
    return dict(id=f"point-{index}", goal_kind="point", split=split, region=region,
                start=f"S{index}", goal=f"G{index}", seed=collect.SEED)


def moving(index=0, region="market", split="train", kind="follow"):
    return dict(point(index, region, split), id=f"{kind}-{index}", goal_kind=kind,
                route=[f"G{index}", f"S{index}", f"T{index}"], target="TARGET")


def object_value(index=0, place="place", split="train"):
    return dict(point(index, split=split), id=f"object-{index}", goal_kind="object", place_id=place)


def probe(value, **changes):
    result = dict(label=value["id"], start=value["start"], goal=value["goal"],
                  visible=True, visible_centre=True, visible_edge=True, start_supported=True,
                  detour_complete=True, direct_cm=1000, detour_length_cm=2000)
    result.update(changes)
    return result


def test_selects_exactly_400_unique_training_pairs_evenly_and_is_deterministic():
    training = [dict(region=f"r{region}", start=f"S{region}-{index}", goal=f"G{region}-{index}", index=region*100+index)
                for region in range(6) for index in range(100)]
    excluded = {(value["goal"], value["start"]) for value in training[::11]}
    result = collect.select_points(training, excluded)
    assert len(result) == len({value["id"] for value in result}) == 400
    assert sorted(Counter(value["region"] for value in result).values()) == [66, 66, 67, 67, 67, 67]
    assert all(value["split"] == "train" and v3.isolated(value, excluded) for value in result)
    assert collect.select_points(list(reversed(training)), excluded) == result


def test_insufficient_balanced_region_cannot_hide_behind_large_other_regions():
    training = [dict(region=f"r{region}", start=f"S{region}-{index}", goal=f"G{region}-{index}")
                for region in range(6) for index in range(20 if region == 0 else 100)]
    with pytest.raises(ValueError, match="balanced"):
        collect.select_points(training, set())


def six_region_points():
    return [point(index * 6 + region, region=f"r{region}")
            for index in range(67) for region in range(6)][:400]


def test_point_cap_keeps_frozen_order_and_all_recorded_attempts_in_equal_quotas(tmp_path):
    values = six_region_points()
    frozen = tmp_path / "point.jsonl"
    collect.freeze_selection(frozen, values)
    before = frozen.read_bytes(), frozen.stat().st_mtime_ns, deepcopy(values)
    late = values[-1]
    done = {("point", late["id"]), ("point", values[0]["id"])}
    scheduled = collect.point_schedule(values, done, 12)
    assert len(scheduled) == 12
    assert Counter(value["region"] for value in scheduled) == Counter({f"r{index}": 2 for index in range(6)})
    assert late in scheduled and values[0] in scheduled
    assert scheduled == [value for value in values if value in scheduled]
    assert collect.point_schedule(values, done) == values
    assert (frozen.read_bytes(), frozen.stat().st_mtime_ns, values) == before


def test_point_cap_cannot_drop_an_already_attempted_region_even_if_other_regions_are_empty():
    values = six_region_points()
    done = {("point", value["id"]) for value in values if value["region"] == "r0"}
    with pytest.raises(ValueError, match="discard completed"):
        collect.point_schedule(values, done, 396)


@pytest.mark.parametrize("limit", [0, -6, 1, 7, 399, 402])
def test_reduced_point_cap_requires_equal_six_region_quotas(limit):
    with pytest.raises(ValueError, match="multiple of six"):
        collect.point_schedule(six_region_points(), set(), limit)


def test_point_cap_rejects_missing_regions_or_too_small_frozen_region_pool():
    with pytest.raises(ValueError, match="six regions"):
        collect.point_schedule([point()], set(), 6)
    with pytest.raises(ValueError, match="region pool"):
        collect.point_schedule(six_region_points()[:6], set(), 12)


def test_summary_uses_point_cap_without_changing_other_targets_or_frozen_pool(monkeypatch):
    selections = {name: [] for name in (*collect.FAMILIES, collect.FOLLOW_BELOW_GATE)}
    selections["point"] = six_region_points()
    selections["jump"] = [dict(point(index), dataset_family="jump") for index in range(168)]
    episodes = [dict(family="point", key_counts={}, decisions=1, training_eligible=False,
                     success=False, episode=f"/unused/{index}") for index in range(12)]
    monkeypatch.setattr(collect.campaign, "tree_bytes", lambda path: 0)
    normal = collect.summarize(episodes, selections, False, 0, True)
    capped = collect.summarize(episodes, selections, False, 0, True, point_limit=12)
    assert normal["point_limit"] == normal["by_family"]["point"]["target"] == 400
    assert normal["by_family"]["point"]["complete"] is False
    assert capped["point_limit"] == capped["by_family"]["point"]["target"] == 12
    assert capped["by_family"]["point"]["complete"] is True
    assert capped["by_family"]["point"]["available_candidates"] == 400
    assert {name: value["target"] for name, value in capped["by_family"].items() if name != "point"} == {
        "object": 100, "approach": 60, "follow": 60, "jump": 168, "follow_below_gate": 60}
    assert all(capped["by_family"][name] == normal["by_family"][name]
               for name in capped["by_family"] if name != "point")


def test_objects_require_train_split_matching_visible_qualified_probe_and_balance_places():
    values = [object_value(index, f"place-{index % 5}") for index in range(110)]
    probes = [probe(value) for value in values]
    bad = object_value(1000, "bad")
    hidden = object_value(1001, "hidden")
    heldout = object_value(1002, "heldout", "test")
    result = collect.select_objects(values + [bad, hidden, heldout],
        probes + [probe(bad, start_supported=False), probe(hidden, visible=False, visible_centre=False, visible_edge=False), probe(heldout)], set())
    assert len(result) == 110 and {value["id"] for value in result} == {value["id"] for value in values}
    assert len({value["place_id"] for value in result[:5]}) == 5
    changed = deepcopy(probes)
    changed[0]["start"] = "OTHER"
    with pytest.raises(ValueError, match="route differs"):
        collect.select_objects(values, changed, set())
    with pytest.raises(ValueError, match="at least 100"):
        collect.select_objects(values[:99], probes, set())


def test_object_place_split_overlap_rejected_before_recording():
    with pytest.raises(ValueError, match="Split overlap"):
        collect.select_objects([object_value(0), object_value(1, split="validation")], [], set())


def test_moving_generation_preserves_all_frozen_membership_and_uses_original_region_splits(monkeypatch):
    old = [moving(0), moving(1, "onar", "validation"), moving(2, "lobart", "test")]
    generated = [moving(3, "onar", "train"), moving(4, "harbour", "test")]
    monkeypatch.setattr(v3, "build_candidates", lambda *args, **kwargs: generated)
    before = deepcopy(old)
    result = collect.select_moving([], [], set(), old)
    assert old == before and all(value in result for value in old)
    assert next(value for value in result if value["id"] == "follow-3")["split"] == "validation"
    assert next(value for value in result if value["id"] == "follow-4")["split"] == "train"
    old[0]["split"] = "validation"
    with pytest.raises(ValueError, match="Frozen moving split"):
        collect.select_moving([], [], set(), old)


def test_moving_route_id_collision_preserves_exact_frozen_seed_and_target(monkeypatch):
    old = [moving()]
    before = deepcopy(old)
    changed = dict(old[0], target="OTHER", seed=old[0]["seed"] + 1)
    monkeypatch.setattr(v3, "build_candidates", lambda *args, **kwargs: [changed])
    assert collect.select_moving([], [], set(), old) == before
    assert old == before


@pytest.mark.parametrize("changes", [dict(route=["G0", "S0", "OTHER"]), dict(start="OTHER"),
    dict(goal="OTHER"), dict(goal_kind="approach"), dict(region="harbour")])
def test_moving_generation_cannot_change_frozen_core_route_or_membership(monkeypatch, changes):
    old = [moving()]
    changed = dict(old[0], **changes)
    monkeypatch.setattr(v3, "build_candidates", lambda *args, **kwargs: [changed])
    with pytest.raises(ValueError, match="frozen moving membership"):
        collect.select_moving([], [], set(), old)


def gate(task, **changes):
    runner = task / "build/opengothic/gothic-body-runner"
    runner.parent.mkdir(parents=True, exist_ok=True)
    if not runner.exists():
        runner.write_bytes(b"current-runner")
    value = dict(complete=True, collection=dict(follow=True),
        settings=dict(runner_sha256=collect.campaign.binary_digest(runner)), by_kind=dict(follow=dict(v3=dict(
        episodes=10, contact_episodes=10, observed_episodes=10, full_horizon_episodes=10,
        nonterminal_episodes=10, follow_fraction=.8))))
    value["by_kind"]["follow"]["v3"].update(changes)
    collect.atomic_json(task / "validation/attempt-2/metrics.json", value)
    return value


def test_follow_auto_uses_complete_ten_episode_second_validation_and_can_explicitly_exclude(tmp_path):
    assert collect.follow_enabled(tmp_path, "no") is False
    gate(tmp_path)
    assert collect.follow_enabled(tmp_path, "auto") is True
    assert collect.follow_enabled(tmp_path, "yes") is True
    gate(tmp_path, episodes=3)
    assert collect.follow_enabled(tmp_path, "auto") is False
    with pytest.raises(ValueError, match="validation gate"):
        collect.follow_enabled(tmp_path, "yes")
    value = gate(tmp_path)
    value["collection"]["follow"] = False
    collect.atomic_json(tmp_path / "validation/attempt-2/metrics.json", value)
    assert collect.follow_enabled(tmp_path, "auto") is False


@pytest.mark.parametrize("field", ["full_horizon_episodes", "nonterminal_episodes"])
def test_follow_gate_requires_every_case_to_run_full_horizon_and_remain_alive(tmp_path, field):
    gate(tmp_path, **{field: 9})
    assert collect.follow_enabled(tmp_path, "auto") is False
    with pytest.raises(ValueError, match="validation gate"):
        collect.follow_enabled(tmp_path, "yes")


def test_follow_gate_rejects_stale_validation_for_another_runner(tmp_path):
    gate(tmp_path)
    (tmp_path / "build/opengothic/gothic-body-runner").write_bytes(b"changed-runner")
    assert collect.follow_enabled(tmp_path, "auto") is False
    with pytest.raises(ValueError, match="validation gate"):
        collect.follow_enabled(tmp_path, "yes")


def test_collection_identity_freezes_second_validation_result_hash(tmp_path, monkeypatch):
    result = gate(tmp_path)
    monkeypatch.setattr(collect.campaign, "binary_digest", lambda path: Path(path).read_bytes().hex()
                        if Path(path).is_file() else "other-input")
    options = SimpleNamespace(task=tmp_path, limit_ms=240000, max_bytes=collect.v3.MAX_BYTES)
    identity = collect.settings(options, True)
    result["collection"]["follow"] = False
    collect.atomic_json(tmp_path / "validation/attempt-2/metrics.json", result)
    assert collect.settings(options, True)["validation_attempt_2_sha256"] != identity["validation_attempt_2_sha256"]


def legacy_identity():
    return dict(schema_version=1, navigation_memory_version=3, demo_version=2, perception_mode="gaze", physics="cv",
        validation_attempt_2_sha256="validation-2-hash", input_sha256={name: f"hash-{name}" for name in collect.INPUTS},
        runner_sha256="runner-hash", contract_sha256="contract-hash", limit_ms=240000,
        include_follow=False, seed=collect.SEED, max_bytes=15_000_000_000)


def test_follow_override_is_additive_opt_in_and_preserves_original_recording_provenance():
    base = legacy_identity()
    before = deepcopy(base)
    assert collect.collection_settings(base, base) == base
    upgraded = collect.collection_settings(base, base, follow_below_gate=True, source_follow_sha256="source-follow-hash")
    assert base == before
    assert upgraded["schema_version"] == 2
    assert upgraded["base_recording_identity"] == before
    policy = upgraded["family_policies"]["follow_below_gate"]
    assert policy["follow_below_gate"] is True and policy["minimum_follow_fraction"] == .7
    assert policy["source_selection_sha256"] == "source-follow-hash"
    assert policy["source_validation_attempt_2_sha256"] == before["validation_attempt_2_sha256"]
    for value in (point(), object_value(), moving(kind="approach"), dict(point(), dataset_family="jump")):
        assert collect.recording_identity(upgraded, value) == before
    tagged = dict(moving(), dataset_family="follow_below_gate", follow_below_gate=True, minimum_follow_fraction=.7)
    new_identity = collect.recording_identity(upgraded, tagged)
    assert new_identity != before and new_identity["schema_version"] == 2
    assert new_identity["dataset_family"] == "follow_below_gate"
    assert new_identity["family_policy"] == policy
    assert new_identity["minimum_follow_fraction"] == .7 and new_identity["follow_below_gate"] is True
    assert collect.collection_settings(base, upgraded, follow_below_gate=True,
        source_follow_sha256="source-follow-hash") == upgraded
    with pytest.raises(ValueError):
        collect.collection_settings(base, upgraded)
    with pytest.raises(ValueError):
        collect.collection_settings(base, upgraded, follow_below_gate=True, source_follow_sha256="changed-follow-hash")


@pytest.mark.parametrize("field,changed", [
    ("runner_sha256", "other-runner"), ("contract_sha256", "other-contract"),
    ("validation_attempt_2_sha256", "other-validation"), ("input_sha256", {"training-pairs.jsonl": "changed"}),
    ("seed", 20), ("limit_ms", 239900), ("max_bytes", 15_000_000_001),
    ("navigation_memory_version", 2), ("demo_version", 1), ("perception_mode", "grid"),
    ("physics", "native"), ("include_follow", True),
])
def test_follow_override_cannot_migrate_changed_core_or_mutate_existing_identity(tmp_path, field, changed):
    previous = legacy_identity()
    state = tmp_path / "collection-state.json"
    collect.atomic_json(state, dict(settings=previous, status="bounded_phase_complete"))
    before = state.read_bytes(), state.stat().st_mtime_ns, deepcopy(previous)
    requested = deepcopy(previous)
    requested[field] = changed
    with pytest.raises(ValueError):
        collect.collection_settings(requested, previous, follow_below_gate=True, source_follow_sha256="source-follow-hash")
    assert (state.read_bytes(), state.stat().st_mtime_ns, previous) == before


def test_follow_override_rejects_policy_drift_on_subsequent_resume():
    base = legacy_identity()
    upgraded = collect.collection_settings(base, base, follow_below_gate=True, source_follow_sha256="source-follow-hash")
    tampered = deepcopy(upgraded)
    tampered["family_policies"]["follow_below_gate"]["minimum_follow_fraction"] = .69
    with pytest.raises(ValueError):
        collect.collection_settings(base, tampered, follow_below_gate=True, source_follow_sha256="source-follow-hash")


def test_follow_override_copies_only_frozen_train_selection_without_rewriting_existing_files(tmp_path, monkeypatch):
    source = tmp_path / "input"
    source.mkdir()
    for name in ("training-pairs.jsonl", "object-candidates.jsonl", "object-probes.jsonl",
                 "moving-scenarios.jsonl", "holdout-cv.jsonl", "ppo-validation.jsonl"):
        (source / name).write_text("")
    collect.atomic_json(source / "places.json", dict(places=[]))
    moving_values = [moving(), moving(1, "onar", "validation"), moving(2, "lobart", "test"),
                     moving(3, kind="approach")]
    monkeypatch.setattr(collect, "select_points", lambda *args: [point()])
    monkeypatch.setattr(collect, "select_objects", lambda *args: [object_value()])
    monkeypatch.setattr(collect, "select_moving", lambda *args: moving_values)
    routes = [dict(point(index, split="train" if index < 168 else "validation"), index=index)
              for index in range(267)]
    monkeypatch.setattr(collect.jumps, "load_routes", lambda *args: routes)
    monkeypatch.setattr(collect.jumps, "collection_order", lambda values: values)
    original = collect.prepare_selections(tmp_path)
    frozen = {path: (path.read_bytes(), path.stat().st_mtime_ns)
              for path in (tmp_path / "recording-input").iterdir()}
    upgraded = collect.prepare_selections(tmp_path, follow_below_gate=True)
    assert all((path.read_bytes(), path.stat().st_mtime_ns) == saved for path, saved in frozen.items())
    assert {key: upgraded[key] for key in original} == original
    assert upgraded["follow_below_gate"] == [dict(original["follow"][0], dataset_family="follow_below_gate",
        follow_below_gate=True, minimum_follow_fraction=.7)]
    value = upgraded["follow_below_gate"][0]
    assert value["goal_kind"] == "follow" and value["split"] == "train"
    assert collect.directory_for(tmp_path, value) == tmp_path / "recordings/follow_below_gate/train/follow-0"
    assert collect.directory_for(tmp_path, value) != collect.directory_for(tmp_path, original["follow"][0])
    again = {path: (path.read_bytes(), path.stat().st_mtime_ns)
             for path in (tmp_path / "recording-input").iterdir()}
    assert collect.prepare_selections(tmp_path, follow_below_gate=True) == upgraded
    assert all((path.read_bytes(), path.stat().st_mtime_ns) == saved for path, saved in again.items())


@pytest.mark.parametrize("split", ["validation", "test"])
def test_no_heldout_commands_or_recording_paths(tmp_path, split):
    value = point(split=split)
    options = SimpleNamespace(task=tmp_path, limit_ms=240000)
    with pytest.raises(ValueError, match="forbidden"):
        collect.directory_for(tmp_path, value)
    with pytest.raises(ValueError, match="forbidden"):
        collect.commands(collect.runner_args(options, 0), value)
    tagged = dict(moving(split=split), dataset_family="follow_below_gate", follow_below_gate=True, minimum_follow_fraction=.7)
    with pytest.raises(ValueError, match="forbidden"):
        collect.directory_for(tmp_path, tagged)
    with pytest.raises(ValueError, match="forbidden"):
        collect.commands(collect.runner_args(options, 0), tagged)


def test_live_suite_rejects_ids_and_renamed_routes_in_both_directions(tmp_path, monkeypatch):
    suite = tmp_path / "scenarios.json"
    suite.write_text(json.dumps(dict(training_eligible=False, scenarios=[
        dict(point(), id="live-point", training_eligible=False,
             variants=[dict(id="live-offset", start="OFFSET", goal="ROUTINE")]),
        dict(moving(2), id="live-follow", training_eligible=False)])))
    monkeypatch.setattr(collect, "LIVE_SUITE", suite)
    options = SimpleNamespace(task=tmp_path, limit_ms=240000)
    excluded = [dict(point(99), id="live-point"), dict(point(99), id="live-point-live-offset"),
                dict(point(99), start="G0", goal="S0"),
                dict(point(99), start="ROUTINE", goal="OFFSET"),
                dict(moving(99), route=["OTHER", "T2", "S2"])]
    for value in excluded:
        with pytest.raises(ValueError, match="Live navigation suite"):
            collect.directory_for(tmp_path, value)
        with pytest.raises(ValueError, match="Live navigation suite"):
            collect.commands(collect.runner_args(options, 0), value)
    assert collect.directory_for(tmp_path, point(99)).name == "point-99"


def test_commands_enable_recording_memory_input_and_correct_teacher(tmp_path):
    args = collect.runner_args(SimpleNamespace(task=tmp_path, limit_ms=240000), 0)
    commands = collect.commands(args, point())
    assert commands[2:5] == ["navdemos record on", "navdemos perception-mode gaze", "navdemos oracle off"]
    assert "start-v3" in commands[-2]
    jump = dict(point(), dataset_family="jump", index=50)
    assert "start-v2" in collect.commands(args, jump)[-2]
    assert collect.version(jump) == 2 and collect.version(point()) == 3


def native(value):
    return dict(start=value["start"], goal=value["goal"], goal_kind=value["goal_kind"], seed=value["seed"],
                teacher_version=collect.version(value), demo_version=2, navigation_memory_version=3,
                perception_mode="gaze", physics_mode="cv", oracle=False, record=True, demonstration_recorded=True,
                observation_source="detour_teacher_v2" if collect.version(value) == 2 else "learner_memory",
                success=True, elapsed_ms=240000, limit_ms=240000, decisions=2, failure="")


def episode_files(directory):
    return {path.relative_to(directory): (path.read_bytes(), path.stat().st_mtime_ns)
            for path in directory.rglob("*") if path.is_file()}


def retained_record(tmp_path, monkeypatch, *, finalized=True, value=None):
    """A historical episode with real files and only native parsing mocked."""
    value = value or point()
    identity = legacy_identity()
    worker = collect.Worker(SimpleNamespace(task=tmp_path, limit_ms=240000), 0, None, identity)
    directory = collect.directory_for(tmp_path, value)
    directory.mkdir(parents=True)
    (directory / "samples.bin.gz").write_bytes(gzip.compress(b"retained NAVDEMO samples", mtime=0))
    for name in ("actions.txt", "trajectory.txt", "runner.log"):
        (directory / name).write_text("retained historical output\n")
    metadata = native(value)
    if finalized:
        measured = dict(id=value["id"], family=collect.family(value), goal_kind=value["goal_kind"],
            split="train", region=value["region"], success=False, failure="historical_failure",
            elapsed_ms=240000, decisions=2, teacher_version=collect.version(value), navigation_memory_version=3,
            episode=str(directory), key_counts=dict(back=1, strafe=1, jump=0, climb=0), payload_bytes=17,
            validated=True, training_eligible=False, retired_metric=17)
        metadata.update(value, recording_identity=identity, training_eligible=False,
            collection_metrics=measured,
            samples_sha256=collect.campaign.binary_digest(directory / "samples.bin.gz"))
    collect.atomic_json(directory / "episode.json", metadata)
    collect.atomic_json(directory / "contract.json", dict(records=2, mismatches=0,
        previous_mismatches=0, retired_contract_metric=7))
    witness = worker.args.output / "scenarios" / f"demos-v4-{value['id']}.bench"
    witness.parent.mkdir(parents=True)
    witness.write_text("\n".join([f"seed {value['seed']}", "cheat god", "set time 10 0", "navdemos physics cv",
        *collect.commands(worker.args, value), "label done", ""]))
    monkeypatch.setattr(worker, "scenario", lambda *args: pytest.fail("A retained episode must not run again"))
    monkeypatch.setattr(collect.demos, "summarize", lambda *args, **kwargs: dict(navigation_memory_versions=[3],
        format_versions=[2], key_counts=dict(back=1, strafe=1, jump=0, climb=0)))

    def replay(command, **kwargs):
        assert command == [str(worker.args.contract), str(directory / "samples.bin.gz")]
        assert kwargs == dict(capture_output=True, text=True, check=False)
        return SimpleNamespace(returncode=0, stdout=json.dumps(dict(records=2, mismatches=0,
            previous_mismatches=0, current_contract_metric=9)))

    monkeypatch.setattr(collect.subprocess, "run", replay)
    return worker, value, directory


@pytest.mark.parametrize("field,bad", [("navigation_memory_version", 2), ("teacher_version", 2),
    ("demo_version", 1), ("oracle", True), ("record", False), ("demonstration_recorded", False),
    ("perception_mode", "grid"), ("physics_mode", "native"), ("limit_ms", 100), ("start", "OTHER"),
    ("observation_source", "detour_raster"), ("drive", True)])
def test_wrong_native_identity_never_gets_relabelled_as_memory_v3(field, bad):
    value = point()
    metadata = native(value)
    metadata[field] = bad
    with pytest.raises(ValueError):
        collect.validate_native(metadata, value, dict(limit_ms=240000))


def test_new_point_accepts_native_seed_sentinel_without_changing_metadata():
    value = point()
    metadata = dict(native(value), seed=0)
    before = deepcopy(metadata)
    collect.validate_native(metadata, value, dict(limit_ms=240000))
    assert metadata == before


def test_new_moving_episode_still_requires_requested_target_seed():
    value = moving()
    with pytest.raises(ValueError, match="seed"):
        collect.validate_native(dict(native(value), seed=0), value, dict(limit_ms=240000))


def test_follow_eligibility_needs_contact_full_horizon_and_eighty_percent():
    value = moving()
    metadata = native(value)
    measured = dict(first_contact_ms=100, follow_observed_ms=239900, follow_fraction=.8)
    assert collect.eligibility(metadata, measured)
    for changed in (dict(first_contact_ms=None), dict(follow_observed_ms=0), dict(follow_fraction=.799)):
        assert not collect.eligibility(metadata, dict(measured, **changed))
    assert not collect.eligibility(dict(metadata, elapsed_ms=239900), measured)
    assert not collect.eligibility(dict(metadata, success=False), measured)
    assert not collect.eligibility(dict(metadata, dataset_family="jump", transition_success=False), measured)
    assert collect.eligibility(dict(metadata, dataset_family="jump", success=False, transition_success=True), measured)


def below_gate_metadata(**changes):
    result = dict(native(moving()), dataset_family="follow_below_gate", follow_below_gate=True,
        minimum_follow_fraction=.7, success=False, teacher_goal_success=False, final_dead=False, final_down=False,
        target_final_dead=False, target_final_down=False)
    result.update(changes)
    return result


@pytest.mark.parametrize("fraction,expected", [(.7, True), (.699, False), (.75, True)])
def test_follow_below_gate_uses_explicit_seventy_percent_without_changing_native_success(fraction, expected):
    metadata = below_gate_metadata()
    measured = dict(first_contact_ms=100, follow_observed_ms=239900, follow_fraction=fraction)
    assert collect.eligibility(metadata, measured) is expected
    assert metadata["success"] is False
    assert not collect.eligibility(dict(metadata, dataset_family="follow", follow_below_gate=False), measured)


@pytest.mark.parametrize("changes,measured_changes", [
    (dict(elapsed_ms=239900), {}),
    ({}, dict(first_contact_ms=None)),
    ({}, dict(follow_observed_ms=0)),
    (dict(final_dead=True), {}),
    (dict(final_down=True), {}),
    (dict(target_final_dead=True), {}),
    (dict(target_final_down=True), {}),
    (dict(final_dead=None), {}),
    (dict(target_final_down=None), {}),
    (dict(follow_below_gate=False), {}),
    (dict(minimum_follow_fraction=.69), {}),
])
def test_follow_below_gate_rejects_missing_contact_horizon_survival_or_policy(changes, measured_changes):
    measured = dict(first_contact_ms=100, follow_observed_ms=239900, follow_fraction=.75)
    assert not collect.eligibility(below_gate_metadata(**changes), dict(measured, **measured_changes))


def test_follow_below_gate_occupancy_uses_physical_time_after_first_contact(tmp_path):
    # Four of five observed trace rows are in band, but only 69.9% of the
    # elapsed post-contact time is: decision counts cannot supply the label.
    metadata = below_gate_metadata(id="follow-0", split="train", decisions=5)
    (tmp_path / "target-trajectory.txt").write_text("\n".join([
        "0 0 0 0 600 0", "100000 0 0 0 300 0", "100100 0 0 0 300 0",
        "100200 0 0 0 300 0", "197860 0 0 0 600 0", "240000 0 0 0 300 0", ""]))
    measured = collect.measured_episode(tmp_path, metadata, dict(key_counts={}))
    assert measured["first_contact_ms"] == 100000
    assert measured["follow_observed_ms"] == 140000
    assert measured["follow_fraction"] == pytest.approx(.699)
    assert measured["training_eligible"] is False
    assert measured["success"] is False


def test_aggregate_storage_reserves_across_families_and_releases_after_failure(tmp_path, monkeypatch):
    budget = collect.StorageBudget(tmp_path, max_bytes=2 * 20480)
    monkeypatch.setattr(collect.shutil, "disk_usage", lambda root: SimpleNamespace(free=10**12))
    retained = tmp_path / "point/train/failed/samples.bin.gz"
    retained.parent.mkdir(parents=True)
    retained.write_bytes(b"x" * 10)
    new_family = tmp_path / "follow_below_gate/train/follow-0/samples.bin.gz"
    new_family.parent.mkdir(parents=True)
    with budget.reserve(100):
        new_family.write_bytes(b"x" * 10)
        with pytest.raises(ValueError, match="Disk budget"):
            with budget.reserve(100):
                pytest.fail("Concurrent family exceeded aggregate budget")
        assert budget.reserved == 20480
    assert budget.reserved == 0
    with pytest.raises(RuntimeError):
        with budget.reserve(100):
            raise RuntimeError("retained partial attempt")
    assert budget.reserved == 0 and retained.read_bytes() == new_family.read_bytes() == b"x" * 10


def test_storage_preflight_accounts_for_whole_parallel_batch_and_active_reservations(tmp_path, monkeypatch):
    budget = collect.StorageBudget(tmp_path, max_bytes=3 * 20480)
    monkeypatch.setattr(collect.shutil, "disk_usage", lambda root: SimpleNamespace(free=10**12))
    (tmp_path / "retained").write_bytes(b"x")
    assert budget.can_reserve_batch(100, 2)
    assert not budget.can_reserve_batch(100, 3)
    with budget.reserve(100):
        assert budget.can_reserve_batch(100, 1)
        assert not budget.can_reserve_batch(100, 2)
    assert budget.reserved == 0
    monkeypatch.setattr(collect.shutil, "disk_usage", lambda root: SimpleNamespace(free=256 * 1024**2))
    assert not budget.can_reserve_batch(100, 1)
    with pytest.raises(collect.StorageLimit, match="Disk budget"):
        with budget.reserve(100):
            pytest.fail("A full disk must not start a new episode")


def forbid_existing_recalculation(monkeypatch):
    def forbidden(*args, **kwargs):
        pytest.fail("A finalized recording must not replay or recalculate historical metrics")

    monkeypatch.setattr(collect, "validate_native", forbidden)
    monkeypatch.setattr(collect, "replay_contract", forbidden)
    monkeypatch.setattr(collect, "measured_episode", forbidden)
    monkeypatch.setattr(collect.jumps, "assess_episode", forbidden)


def test_raw_completed_native_point_keeps_engine_seed_and_files_when_adopted(tmp_path, monkeypatch):
    worker, value, directory = retained_record(tmp_path, monkeypatch, finalized=False)
    metadata = v3.read_json(directory / "episode.json")
    metadata["seed"] = 0
    collect.atomic_json(directory / "episode.json", metadata)
    for witness in worker.args.output.glob("scenarios/*.bench"):
        witness.unlink()
    before = episode_files(directory)
    measured = worker.trial(value)
    assert measured["id"] == value["id"] and measured["training_eligible"] is True
    assert measured["seed"] == 0
    assert episode_files(directory) == before
    assert "id" not in v3.read_json(directory / "episode.json")


@pytest.mark.parametrize("old_metrics", [
    dict(training_eligible=False, retired_metric=17),
    dict(training_eligible=False, payload_bytes=1, validated=True, key_counts={}),
    dict(success=False, failure="historical_failure", elapsed_ms=12345),
])
def test_resume_carries_saved_metrics_without_recalculation_or_recording_writes(tmp_path, monkeypatch, old_metrics):
    worker, value, directory = retained_record(tmp_path, monkeypatch)
    metadata = v3.read_json(directory / "episode.json")
    metadata["collection_metrics"].update(old_metrics)
    saved = deepcopy(metadata["collection_metrics"])
    collect.atomic_json(directory / "episode.json", metadata)
    before = episode_files(directory)
    forbid_existing_recalculation(monkeypatch)
    measured = worker.trial(value)
    assert all(measured[key] == expected for key, expected in saved.items())
    assert measured["samples_sha256"] == metadata["samples_sha256"]
    assert measured["seed"] == metadata["seed"]
    assert episode_files(directory) == before
    assert worker.trial(value) == measured
    assert episode_files(directory) == before


@pytest.mark.parametrize("changes", [
    dict(seed=collect.SEED + 1), dict(start="OTHER_START"), dict(goal="OTHER_GOAL"),
    dict(region="OTHER_REGION"), dict(seed=0, start="OTHER_START", goal="OTHER_GOAL"),
])
def test_existing_episode_parameters_are_authoritative_over_current_candidate(tmp_path, monkeypatch, changes):
    worker, value, directory = retained_record(tmp_path, monkeypatch)
    metadata = v3.read_json(directory / "episode.json")
    saved = deepcopy(metadata["collection_metrics"])
    before = episode_files(directory)
    forbid_existing_recalculation(monkeypatch)
    candidate = dict(value, **changes)
    measured = worker.trial(candidate)
    assert all(measured[key] == expected for key, expected in saved.items())
    assert measured["seed"] == metadata["seed"]
    assert measured["scenario"]["start"] == metadata["start"]
    assert measured["scenario"]["goal"] == metadata["goal"]
    assert episode_files(directory) == before


def test_existing_failed_episode_remains_completed_and_never_reruns(tmp_path, monkeypatch):
    worker, value, directory = retained_record(tmp_path, monkeypatch)
    forbid_existing_recalculation(monkeypatch)
    before = episode_files(directory)
    first = worker.trial(value)
    assert first["training_eligible"] is False and first["success"] is False
    assert worker.trial(dict(value, seed=0)) == first
    assert episode_files(directory) == before


@pytest.mark.parametrize("has_room", [True, False])
def test_collection_skips_completed_failed_candidate_when_selecting_new_work(tmp_path, monkeypatch, has_room):
    worker, value, directory = retained_record(tmp_path, monkeypatch)
    old = v3.read_json(directory / "episode.json")["collection_metrics"]
    next_value = point(1)
    selections = {name: [] for name in collect.FAMILIES}
    selections["point"] = [dict(value, seed=0), next_value]
    source = tmp_path / "recording-input"
    source.mkdir()
    for name, values in selections.items():
        (source / f"{name}.jsonl").write_text("".join(json.dumps(item) + "\n" for item in values))
    (source / "moving-all-splits.jsonl").write_text("")
    collect.atomic_json(tmp_path / "recordings/collection-state.json", dict(settings=legacy_identity()))
    calls = []

    def trial(self, candidate):
        calls.append(candidate["id"])
        if candidate["id"] == value["id"]:
            return old
        assert candidate["id"] == next_value["id"]
        return dict(old, id=candidate["id"], episode=str(collect.directory_for(tmp_path, candidate)))

    monkeypatch.setattr(collect.Worker, "trial", trial)
    monkeypatch.setattr(collect, "settings", lambda *args: legacy_identity())
    monkeypatch.setattr(collect, "prepare_selections", lambda *args, **kwargs: pytest.fail("Existing selections must not regenerate"))
    monkeypatch.setattr(collect.sys, "platform", "linux")
    monkeypatch.setattr(collect.StorageBudget, "can_reserve_batch", lambda *args, **kwargs: has_room)
    options = SimpleNamespace(task=tmp_path, include_follow="no", follow_below_gate=False,
        parallel=1, limit_ms=240000, max_bytes=15_000_000_000, wall_minutes=100,
        max_new=1, round_size=12, verify_existing_only=False, expect_existing=None)
    result = collect.run(options)
    assert calls == ([value["id"], next_value["id"]] if has_room else [value["id"]])
    assert result["resume_verified_episodes"] == 1
    assert result["by_family"]["point"]["episodes"] == (2 if has_room else 1)
    assert result["by_family"]["point"]["training_eligible"] == 0
    if not has_room:
        assert result["stop_reason"] == "storage_limit"
        assert result["complete"] is False
        assert v3.read_json(tmp_path / "recordings/collection-metrics.json")["stop_reason"] == "storage_limit"


def test_existing_episode_ignores_command_witness_and_current_recording_identity(tmp_path, monkeypatch):
    worker, value, directory = retained_record(tmp_path, monkeypatch)
    for witness in worker.args.output.glob("scenarios/*.bench"):
        witness.unlink()
    worker.identity = dict(worker.identity, seed=0, limit_ms=100, runner_sha256="different-current-runner")
    before = episode_files(directory)
    forbid_existing_recalculation(monkeypatch)
    measured = worker.trial(value)
    assert measured["training_eligible"] is False
    assert episode_files(directory) == before


def test_auxiliary_log_growth_does_not_recompute_authoritative_payload_metric(tmp_path, monkeypatch):
    worker, value, directory = retained_record(tmp_path, monkeypatch)
    forbid_existing_recalculation(monkeypatch)
    measured = worker.trial(value)
    with (directory / "runner.log").open("ab") as stream:
        stream.write(b"\n")
    before = episode_files(directory)
    updated = worker.trial(value)
    assert updated == measured
    assert updated["payload_bytes"] == 17
    assert episode_files(directory) == before


@pytest.mark.parametrize("hash_source", ["state_pin", "saved_summary"])
def test_legacy_finalized_episode_uses_recorded_summary_or_state_samples_hash(tmp_path, monkeypatch, hash_source):
    worker, value, directory = retained_record(tmp_path, monkeypatch)
    metadata = v3.read_json(directory / "episode.json")
    digest = metadata.pop("samples_sha256")
    collect.atomic_json(directory / "episode.json", metadata)
    if hash_source == "state_pin":
        worker.state.data["integrity"]["episodes"][worker.state.key(directory)] = dict(
            samples_sha256=digest, scenario=dict(seed=0, start="OLD_VALUE"))
    else:
        worker.state.remember_metrics(directory, dict(metadata["collection_metrics"], samples_sha256=digest))
    before = episode_files(directory)
    forbid_existing_recalculation(monkeypatch)
    measured = worker.trial(dict(value, seed=0, start="CURRENT_VALUE"))
    assert measured["samples_sha256"] == digest
    assert measured["training_eligible"] is False
    assert episode_files(directory) == before


def test_finalized_episode_without_any_recorded_samples_hash_is_not_silently_repinned(tmp_path, monkeypatch):
    worker, value, directory = retained_record(tmp_path, monkeypatch)
    metadata = v3.read_json(directory / "episode.json")
    del metadata["samples_sha256"]
    collect.atomic_json(directory / "episode.json", metadata)
    before = episode_files(directory)
    with pytest.raises(ValueError, match="(?i)(hash|sample)"):
        worker.trial(value)
    assert episode_files(directory) == before


@pytest.mark.parametrize("pin_first", [False, True])
def test_saved_samples_hash_change_rejected_before_parsing_or_new_recording(tmp_path, monkeypatch, pin_first):
    worker, value, directory = retained_record(tmp_path, monkeypatch)
    if pin_first:
        worker.trial(value)
    state = tmp_path / "recordings/collection-state.json"
    state_before = (state.read_bytes(), state.stat().st_mtime_ns) if state.exists() else None
    (directory / "samples.bin.gz").write_bytes(gzip.compress(b"different NAVDEMO samples", mtime=0))
    before = episode_files(directory)
    monkeypatch.setattr(collect.demos, "summarize", lambda *args, **kwargs: pytest.fail("Changed samples must fail before NAVDEMO check"))
    monkeypatch.setattr(collect.subprocess, "run", lambda *args, **kwargs: pytest.fail("Existing record must never replay"))
    with pytest.raises(ValueError, match="(?i)(hash|fingerprint|integrity|sample)"):
        worker.trial(value)
    assert episode_files(directory) == before
    if state_before is not None:
        assert (state.read_bytes(), state.stat().st_mtime_ns) == state_before


def test_existing_record_still_runs_navdemo_check(tmp_path, monkeypatch):
    worker, value, directory = retained_record(tmp_path, monkeypatch)
    before = episode_files(directory)
    calls = []

    def invalid(path, *, validate):
        assert Path(path) == directory and validate is True
        calls.append(path)
        raise ValueError("NAVDEMO sample CRC differs")

    monkeypatch.setattr(collect.demos, "summarize", invalid)
    forbid_existing_recalculation(monkeypatch)
    with pytest.raises(ValueError, match="CRC"):
        worker.trial(value)
    assert len(calls) == 1
    assert episode_files(directory) == before


@pytest.mark.parametrize("witness_state", ["current", "stale_worker", "stale_teacher", "tampered", "missing"])
def test_new_recording_checks_own_witness_then_resume_preserves_all_episode_files(tmp_path, monkeypatch, witness_state):
    worker, value, directory = retained_record(tmp_path, monkeypatch, finalized=False)
    witness = worker.args.output / "scenarios" / f"demos-v4-{value['id']}.bench"
    original_commands = witness.read_text()
    other_mode = "0/jumps" if witness_state == "stale_teacher" else "1/v3"
    other = tmp_path / "recording-workers" / other_mode / "scenarios" / witness.name
    other.parent.mkdir(parents=True)
    other.write_text(original_commands if witness_state in ("tampered", "missing") else "seed 0\n")
    other_before = other.read_bytes(), other.stat().st_mtime_ns
    originals = {path.name: path.read_bytes() for path in directory.iterdir() if path.name != "contract.json"}
    for path in directory.iterdir():
        path.unlink()
    directory.rmdir()
    calls = []

    def scenario(name, commands, destination, seed):
        assert destination == directory and seed == value["seed"]
        calls.append(name)
        destination.mkdir(parents=True)
        for name, payload in originals.items():
            (destination / name).write_bytes(payload)
        witness.write_text(original_commands)
        if witness_state == "tampered":
            witness.write_text(original_commands.replace(f"seed {seed}", "seed 0", 1))
        elif witness_state == "missing":
            witness.unlink()

    monkeypatch.setattr(worker, "scenario", scenario)
    worker.budget = SimpleNamespace(reserve=lambda *args, **kwargs: nullcontext())
    if witness_state in ("tampered", "missing"):
        with pytest.raises(ValueError, match="Missing or changed seeded command witness"):
            worker.trial(value)
        assert "id" not in v3.read_json(directory / "episode.json")
        assert (other.read_bytes(), other.stat().st_mtime_ns) == other_before
        return
    result = worker.trial(value)
    assert (other.read_bytes(), other.stat().st_mtime_ns) == other_before
    metadata = v3.read_json(directory / "episode.json")
    assert calls == [f"demos-v4-{value['id']}"]
    assert metadata["recording_identity"] == legacy_identity()
    assert metadata["id"] == value["id"] and metadata["training_eligible"] is True
    assert (directory / "contract.json").is_file()
    before = episode_files(directory)
    assert worker.trial(value) == result
    assert calls == [f"demos-v4-{value['id']}"]
    assert episode_files(directory) == before


def test_partial_attempt_is_retained_and_never_retried(tmp_path, monkeypatch):
    value = point()
    directory = collect.directory_for(tmp_path, value)
    directory.mkdir(parents=True)
    partial = directory / "samples.bin.gz"
    partial.write_bytes(b"unfinished")
    worker = collect.Worker(SimpleNamespace(task=tmp_path, limit_ms=240000), 0, None, dict(limit_ms=240000))
    monkeypatch.setattr(worker, "scenario", lambda *args: pytest.fail("Partial recording must not run again"))
    with pytest.raises(ValueError, match="Partial recording retained"):
        worker.trial(value)
    assert partial.read_bytes() == b"unfinished"


def test_contract_replay_does_not_rewrite_existing_contract(tmp_path, monkeypatch):
    result = dict(records=42, mismatches=0, previous_mismatches=0)
    monkeypatch.setattr(collect.subprocess, "run", lambda *args, **kwargs: SimpleNamespace(returncode=0, stdout=json.dumps(result)))
    path = tmp_path / "contract.json"
    assert collect.replay_contract(Path("/contract"), tmp_path, 42, write=True) == result
    before = path.read_bytes(), path.stat().st_mtime_ns
    result["new_derived_metric"] = 100
    assert collect.replay_contract(Path("/contract"), tmp_path, 42) == result
    assert (path.read_bytes(), path.stat().st_mtime_ns) == before
    with pytest.raises(ValueError, match="mismatch"):
        collect.replay_contract(Path("/contract"), tmp_path, 43)


@pytest.mark.parametrize("changes", [dict(records=41), dict(mismatches=1), dict(previous_mismatches=1)])
def test_contract_replay_still_rejects_current_teacher_mismatch_or_decision_count(tmp_path, monkeypatch, changes):
    saved = dict(records=42, mismatches=0, previous_mismatches=0, retired_metric=7)
    collect.atomic_json(tmp_path / "contract.json", saved)
    before = episode_files(tmp_path)
    result = dict(saved, **changes)
    monkeypatch.setattr(collect.subprocess, "run", lambda *args, **kwargs: SimpleNamespace(returncode=0, stdout=json.dumps(result)))
    with pytest.raises(ValueError, match="mismatch"):
        collect.replay_contract(Path("/contract"), tmp_path, 42)
    assert episode_files(tmp_path) == before


SETUP_REFUSAL = "Runner rejected advance: navdemos moving route needs distinct connected points"


def setup_refusal_collection(tmp_path, monkeypatch):
    """Exercise the real worker and scheduler without starting a game."""
    values = [moving(index) for index in range(3)]
    selections = {name: [] for name in collect.FAMILIES}
    selections["follow"] = values
    source = tmp_path / "recording-input"
    source.mkdir()
    for name, candidates in selections.items():
        (source / f"{name}.jsonl").write_text("".join(json.dumps(value) + "\n" for value in candidates))
    (source / "moving-all-splits.jsonl").write_text("")
    collect.atomic_json(tmp_path / "recordings/collection-state.json", dict(settings=legacy_identity()))
    monkeypatch.setattr(collect, "settings", lambda *args: legacy_identity())
    monkeypatch.setattr(collect, "follow_enabled", lambda *args: True)
    monkeypatch.setattr(collect.sys, "platform", "linux")
    monkeypatch.setattr(collect.shutil, "disk_usage", lambda root: SimpleNamespace(free=10**12))
    checks = []

    def check(directory, *, validate):
        assert validate is True
        checks.append(Path(directory).name)
        return dict(navigation_memory_versions=[3], format_versions=[2],
                    key_counts=dict(back=0, strafe=0, jump=0, climb=0))

    monkeypatch.setattr(collect.demos, "summarize", check)
    monkeypatch.setattr(collect.subprocess, "run", lambda *args, **kwargs: SimpleNamespace(
        returncode=0, stdout=json.dumps(dict(records=2, mismatches=0, previous_mismatches=0))))
    calls = []

    def scenario(self, name, commands, directory, seed):
        value = next(value for value in values if value["id"] == directory.name)
        calls.append(value["id"])
        self.runner = SimpleNamespace(process=SimpleNamespace(poll=lambda: None), close=lambda: None)
        directory.mkdir(parents=True)
        (directory / "runner.log").write_text("scenario setup attempted\n")
        if value is values[0]:
            raise RuntimeError(SETUP_REFUSAL)
        collect.atomic_json(directory / "episode.json", native(value))
        (directory / "samples.bin.gz").write_bytes(gzip.compress(b"fake complete samples", mtime=0))
        for file_name in ("actions.txt", "trajectory.txt"):
            (directory / file_name).write_text("fake complete trajectory\n")
        (directory / "target-trajectory.txt").write_text(
            "0 0 0 0 300 0\n120000 0 0 0 300 0\n240000 0 0 0 300 0\n")
        witness = self.args.output / "scenarios" / f"{name}.bench"
        witness.parent.mkdir(parents=True, exist_ok=True)
        witness.write_text("\n".join([f"seed {seed}", "cheat god", "set time 10 0", "navdemos physics cv",
                                      *commands, "label done", ""]))

    monkeypatch.setattr(collect.Worker, "scenario", scenario)
    options = SimpleNamespace(task=tmp_path, include_follow="yes", follow_below_gate=False,
        parallel=2, limit_ms=240000, max_bytes=15_000_000_000, wall_minutes=100,
        max_new=100, round_size=12, verify_existing_only=False, expect_existing=None)
    return options, values, calls, checks


def test_setup_refusal_rejects_candidate_preserves_parallel_peer_and_collects_next(tmp_path, monkeypatch):
    options, values, calls, checks = setup_refusal_collection(tmp_path, monkeypatch)
    result = collect.run(options)
    assert Counter(calls) == Counter(value["id"] for value in values)
    assert result["by_family"]["follow"]["episodes"] == 2
    assert result["by_family"]["follow"]["training_eligible"] == 2
    assert result["by_family"]["follow"]["rejected_candidates"] == 1
    assert result["by_family"]["follow"]["candidate_pool_exhausted"] is True
    assert {value["id"] for value in result["episodes"]} == {values[1]["id"], values[2]["id"]}
    rejected, = result["rejected_candidates"]
    assert rejected["id"] == values[0]["id"] and rejected["reason"] == SETUP_REFUSAL
    assert rejected["training_eligible"] is False and rejected["recorded"] is False
    assert rejected["scenario"] == values[0]
    directory = collect.directory_for(tmp_path, values[0])
    assert not (directory / "episode.json").exists() and not (directory / "samples.bin.gz").exists()
    assert directory.joinpath("runner.log").read_text() == "scenario setup attempted\n"
    assert Counter(checks) == Counter({values[1]["id"]: 1, values[2]["id"]: 1})
    assert v3.read_json(tmp_path / "recordings/collection-metrics.json")["rejected_candidates"] == [rejected]

    before = {value["id"]: episode_files(collect.directory_for(tmp_path, value)) for value in values}
    again = collect.run(options)
    assert Counter(calls) == Counter(value["id"] for value in values)
    assert again["rejected_candidates"] == [rejected]
    assert again["by_family"]["follow"]["episodes"] == 2
    assert again["by_family"]["follow"]["training_eligible"] == 2
    assert again["resume_verified_episodes"] == 2
    assert {value["id"]: episode_files(collect.directory_for(tmp_path, value)) for value in values} == before


@pytest.mark.parametrize("failure", [
    RuntimeError("runner exited before response: 1"),
    RuntimeError("runner closed protocol output: 1"),
    RuntimeError("runner watchdog expired"),
    RuntimeError("Runner rejected advance: unknown internal failure"),
    RuntimeError("unexpected collector defect"),
    TimeoutError("runner watchdog expired"),
    BrokenPipeError("runner input pipe closed"),
    OSError("No space left on device"),
])
def test_runner_or_disk_failures_are_fatal_instead_of_candidate_rejection(tmp_path, monkeypatch, failure):
    worker = collect.Worker(SimpleNamespace(task=tmp_path, limit_ms=240000), 0,
                           SimpleNamespace(reserve=lambda *args, **kwargs: nullcontext()), legacy_identity())
    worker.runner = SimpleNamespace(process=SimpleNamespace(poll=lambda: None), close=lambda: None)

    def scenario(*args):
        raise failure

    monkeypatch.setattr(worker, "scenario", scenario)
    with pytest.raises(type(failure), match=str(failure)):
        worker.trial(moving())
    assert worker.state.rejections() == []


def test_known_setup_refusal_from_dead_runner_remains_fatal(tmp_path, monkeypatch):
    worker = collect.Worker(SimpleNamespace(task=tmp_path, limit_ms=240000), 0,
                           SimpleNamespace(reserve=lambda *args, **kwargs: nullcontext()), legacy_identity())
    worker.runner = SimpleNamespace(process=SimpleNamespace(poll=lambda: 1), close=lambda: None)
    monkeypatch.setattr(worker, "scenario", lambda *args: (_ for _ in ()).throw(RuntimeError(SETUP_REFUSAL)))
    with pytest.raises(RuntimeError, match="distinct connected points"):
        worker.trial(moving())
    assert worker.state.rejections() == []


@pytest.mark.parametrize("started_file", ["episode.json", "samples.bin.gz", "samples.bin"])
def test_setup_refusal_after_recording_started_is_fatal_and_retains_files(tmp_path, monkeypatch, started_file):
    value = moving()
    worker = collect.Worker(SimpleNamespace(task=tmp_path, limit_ms=240000), 0,
                           SimpleNamespace(reserve=lambda *args, **kwargs: nullcontext()), legacy_identity())
    worker.runner = SimpleNamespace(process=SimpleNamespace(poll=lambda: None), close=lambda: None)

    def scenario(name, commands, directory, seed):
        directory.mkdir(parents=True)
        (directory / started_file).write_bytes(b"recording already started")
        raise RuntimeError(SETUP_REFUSAL)

    monkeypatch.setattr(worker, "scenario", scenario)
    with pytest.raises(RuntimeError, match="distinct connected points"):
        worker.trial(value)
    assert worker.state.rejections() == []
    assert (collect.directory_for(tmp_path, value) / started_file).read_bytes() == b"recording already started"


def test_navdemo_failure_stays_fatal_after_a_valid_scenario(tmp_path, monkeypatch):
    options, values, calls, checks = setup_refusal_collection(tmp_path, monkeypatch)
    value = values[1]
    worker = collect.Worker(options, 0, SimpleNamespace(reserve=lambda *args, **kwargs: nullcontext()), legacy_identity())
    monkeypatch.setattr(collect.demos, "summarize", lambda *args, **kwargs:
                        (_ for _ in ()).throw(ValueError("NAVDEMO sample CRC differs")))
    with pytest.raises(ValueError, match="NAVDEMO sample CRC differs"):
        worker.trial(value)
    assert worker.state.rejections() == []
    assert calls == [value["id"]]
    directory = collect.directory_for(tmp_path, value)
    assert v3.complete(directory)
    assert (directory / "samples.bin.gz").is_file()
    assert "training_eligible" not in v3.read_json(directory / "episode.json")


def test_fatal_runner_failure_drains_parallel_peer_into_summary_before_stopping(tmp_path, monkeypatch):
    options, values, calls, checks = setup_refusal_collection(tmp_path, monkeypatch)
    original_scenario = collect.Worker.scenario

    def scenario(self, name, commands, directory, seed):
        if directory.name == values[0]["id"]:
            calls.append(values[0]["id"])
            self.runner = SimpleNamespace(process=SimpleNamespace(poll=lambda: 1), close=lambda: None)
            raise RuntimeError("runner exited before response: 1")
        return original_scenario(self, name, commands, directory, seed)

    monkeypatch.setattr(collect.Worker, "scenario", scenario)
    with pytest.raises(RuntimeError, match="runner exited before response: 1"):
        collect.run(options)
    assert Counter(calls) == Counter({values[0]["id"]: 1, values[1]["id"]: 1})
    result = v3.read_json(tmp_path / "recordings/collection-metrics.json")
    assert [value["id"] for value in result["episodes"]] == [values[1]["id"]]
    assert result["by_family"]["follow"]["episodes"] == 1
    assert result["by_family"]["follow"]["training_eligible"] == 1
    assert result["rejected_candidates"] == []
