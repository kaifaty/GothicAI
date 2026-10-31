"""Asset-free memory-v4 collection, safety and shared-storage contracts."""
import importlib.util
import json
from copy import deepcopy
from pathlib import Path
import shutil
from types import SimpleNamespace

import pytest


SPEC = importlib.util.spec_from_file_location("nav_demos_v5_collect", Path(__file__).resolve().parents[1] / "nav_demos_v4_collect.py")
collect = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(collect)


def scenario(index=0, **changes):
    return dict(dict(id=f"point-{index}", goal_kind="point", split="train", region="market",
        start="START", goal="GOAL", seed=collect.SEED), **changes)


def memberships():
    nodes = [["START", 0, 0, 0], ["FAR", 1000, 0, 0]]
    snapshots = [dict(kind="roads_snapshot", actor="OUTSIDER", stranger=False,
        known_node_ids=[1], routines=[dict(node=0)], spawn_cm=[0, 0, 0]),
        dict(kind="roads_snapshot", actor="RESIDENT", stranger=False,
        known_node_ids=[0, 1], routines=[dict(node=1)], spawn_cm=[1000, 0, 0])]
    return nodes, {"START": [0, 0, 0]}, snapshots


def test_residency_uses_native_membership_before_distance_and_preserves_a_stable_mix():
    values = [collect.resident_scenario(scenario(index), memberships()) for index in range(40)]
    assert {value["npc"] for value in values} == {"RESIDENT"}
    assert {value["local"] for value in values} == {False, True}
    assert all(value["knownroads_selection"]["nearest_start_node"] == 0 for value in values)
    assert [collect.resident_scenario(scenario(index), memberships()) for index in range(40)] == values
    assert scenario() == {key: values[0][key] for key in scenario()}


def test_start_unknown_to_every_native_actor_remains_a_stranger():
    nodes, positions, snapshots = memberships()
    snapshots[1]["known_node_ids"] = [1]
    result = collect.resident_scenario(scenario(), (nodes, positions, snapshots))
    assert result["npc"] == "OUTSIDER" and result["local"] is False


def test_moving_target_cannot_also_be_the_walking_actor():
    result = collect.resident_scenario(scenario(goal_kind="follow", target="RESIDENT"), memberships())
    assert result["npc"] == "OUTSIDER" and result["local"] is False


def test_jump_teacher2_uses_the_same_stable_native_resident_mix():
    values = [collect.resident_scenario(scenario(index, id=f"jump-{index:04d}", dataset_family="jump"), memberships())
              for index in range(40)]
    assert {value["local"] for value in values} == {False, True}
    assert {value["npc"] for value in values} == {"RESIDENT"}
    assert all("stranger_reason" not in value["knownroads_selection"] for value in values)
    assert values == [collect.resident_scenario(scenario(index, id=f"jump-{index:04d}", dataset_family="jump"), memberships())
                      for index in range(40)]


@pytest.mark.parametrize("local", [True, False])
@pytest.mark.parametrize("start,goal", [("START", "GOAL"), ("cm:0,0,0", "GOAL"),
    ("START", "cm:1000,0,0"), ("cm:0,0,0", "cm:1000,0,0")])
def test_v5_commands_install_familiar_memory_keep_gaze_cv_and_select_native_residency(tmp_path, local, start, goal):
    options = SimpleNamespace(task=tmp_path, limit_ms=240000, corpus_version=5)
    args = collect.runner_args(options, 0)
    value = dict(collect.resident_scenario(scenario(start=start, goal=goal), memberships()), local=local)
    commands = collect.commands(args, value)
    assert f"roads familiar-load {tmp_path}/input/familiar.bin" in commands
    assert f"roads {'on' if local else 'stranger'} RESIDENT" in commands
    assert not any("cm:" in command for command in commands if command.startswith("roads "))
    assert "navdemos perception-monitor on" in commands
    assert "navdemos record on" in commands and "navdemos oracle off" in commands
    assert "navdemos perception-mode gaze" in commands
    assert commands[-2] == f"navdemos start-v3 RESIDENT {start} {goal} 240000"
    jump = dict(value, dataset_family="jump", index=0)
    assert "start-v2 RESIDENT" in collect.commands(args, jump)[-2]


def native(value):
    return dict(start=value["start"], goal=value["goal"], goal_kind="point", seed=0,
        teacher_version=collect.version(value), demo_version=3, navigation_memory_version=4, journey_memory=True,
        familiar_map=value["local"], perception_mode="gaze", perception_monitor=True, physics_mode="cv",
        oracle=False, record=True, demonstration_recorded=True,
        observation_source="detour_teacher_v2" if collect.version(value) == 2 else "learner_memory",
        success=True, elapsed_ms=200, limit_ms=240000, decisions=2, failure="")


def test_v5_native_identity_requires_memory4_and_recording3_but_allows_point_seed_sentinel():
    value = collect.resident_scenario(scenario(), memberships())
    identity = dict(navigation_memory_version=4, demo_version=3, limit_ms=240000)
    collect.validate_native(native(value), value, identity)
    for changes in (dict(navigation_memory_version=3), dict(demo_version=2),
                    dict(journey_memory=False), dict(familiar_map=not value["local"]),
                    dict(perception_monitor=False)):
        with pytest.raises(ValueError):
            collect.validate_native(dict(native(value), **changes), value, identity)
    completed = dict(native(value), **{key: item for key, item in value.items() if key != "seed"},
                     command_seed=value["seed"], recording_identity=identity,
                     teacher_variant="v3b", teacher_familiar_hint=False)
    collect.validate_native(completed, value, identity)


@pytest.mark.parametrize("local", [True, False])
def test_v2_episode_identity_requires_memory4_and_navdemo3_for_both_residencies(local):
    value = dict(collect.resident_scenario(scenario(dataset_family="jump", index=0), memberships()), local=local,
                 residency="local" if local else "stranger")
    identity = dict(navigation_memory_version=4, demo_version=3, limit_ms=240000)
    metadata = native(value)
    collect.validate_native(metadata, value, identity)
    completed = dict(metadata, **{key: item for key, item in value.items() if key != "seed"},
                     command_seed=value["seed"], recording_identity=identity,
                     teacher_variant="v2", teacher_familiar_hint=False)
    collect.validate_native(completed, value, identity)
    with pytest.raises(ValueError, match="identity, memory or mode"):
        collect.validate_native(dict(metadata, demo_version=2, navigation_memory_version=3, journey_memory=False),
                                value, identity)


def test_stranger_keeps_major_roads_but_cannot_have_familiar_terrain(tmp_path):
    value = dict(npc="STRANGER", local=False)
    known = dict(kind="roads_snapshot", tag="recording", actor="STRANGER", stranger=True,
                 known_node_ids=[2, 9, 10], known_edge_ids=[14, 15])
    familiar = dict(kind="familiar_stats", actor="STRANGER", stranger=True,
                    known_tiles=0, known_2m_tiles_xz=0, eligible_2m_tiles=0, familiar_source_poses=0)
    (tmp_path / "roads-snapshots.jsonl").write_text(json.dumps(known) + "\n")
    events = tmp_path / "roads-events.jsonl"
    events.write_text(json.dumps(familiar) + "\n")
    assert collect.native_residency(tmp_path, value) == dict(knownroads=known, familiar=familiar)
    events.write_text(json.dumps(dict(familiar, known_tiles=1)) + "\n")
    with pytest.raises(ValueError, match="Stranger unexpectedly knows terrain"):
        collect.native_residency(tmp_path, value)


def monitor(directory, depths=(0, 20, 99, 0)):
    frames = [dict(elapsed_ms=index * 100, cv_active=True, swimming=False,
        water_depth_cm=depth, deep_water_threshold_cm=100) for index, depth in enumerate(depths)]
    (directory / "perception-safety-frames.jsonl").write_text("".join(json.dumps(frame) + "\n" for frame in frames))
    return dict(elapsed_ms=(len(frames) - 1) * 100)


def test_water_eligibility_uses_passive_depth_and_swim_evidence_not_goal_success_alone(tmp_path):
    metadata = monitor(tmp_path)
    measured = collect.water_metrics(tmp_path, metadata)
    assert measured["entered_water_frames"] == 2 and measured["water_entries"] == 1
    assert measured["deep_water_frames"] == 1 and measured["deep_water_entries"] == 1
    water = dict(dataset_family="water", goal_kind="point", success=True)
    assert not collect.eligibility(water, measured)
    dry = collect.water_metrics(tmp_path, monitor(tmp_path, (0, 10, 0)))
    assert collect.eligibility(water, dry)
    assert not collect.eligibility(water, dict(dry, swimming_frames=1))
    assert not collect.eligibility(dict(water, success=False), dry)


def test_incomplete_physical_water_trace_is_rejected(tmp_path):
    metadata = monitor(tmp_path)
    with pytest.raises(ValueError, match="incomplete physical"):
        collect.water_metrics(tmp_path, dict(metadata, elapsed_ms=1000))


def test_archived_sample_hash_pins_and_metrics_survive_moving_without_symlinks(tmp_path):
    task, archive = tmp_path / "active", tmp_path / "shared"
    value = scenario()
    local = collect.directory_for(task, value)
    local.mkdir(parents=True)
    (local / "samples.bin.gz").write_bytes(b"unchanged sample payload")
    digest = collect.campaign.binary_digest(local / "samples.bin.gz")
    state_path = task / "recordings/collection-state.json"
    collect.atomic_json(task / "recordings/collection-metrics.json", dict(
        episodes=[dict(episode=str(local), samples_sha256=digest)]))
    state = collect.CollectionState(state_path, {}, archive_root=archive)
    state.pin_samples(local, digest)
    archived = collect.directory_for(archive, value)
    archived.parent.mkdir(parents=True)
    local.rename(archived)
    options = SimpleNamespace(task=task, archive_root=archive)
    assert collect.resolve_directory(options, value) == archived
    assert state.key(archived) == "point/train/point-0"
    assert state.saved_metrics(archived)["samples_sha256"] == digest
    assert state.verify_samples(archived, {}) == digest
    (archived / "samples.bin.gz").write_bytes(b"changed samples")
    with pytest.raises(ValueError, match="hash differs"):
        state.verify_samples(archived, {})


def test_duplicate_active_and_archived_attempts_are_not_silently_selected(tmp_path):
    options = SimpleNamespace(task=tmp_path / "active", archive_root=tmp_path / "shared")
    for task in (options.task, options.archive_root):
        collect.directory_for(task, scenario()).mkdir(parents=True)
    with pytest.raises(ValueError, match="both active and archived"):
        collect.resolve_directory(options, scenario())


def test_v5_budget_counts_shared_failures_and_retains_20gib_locally(tmp_path, monkeypatch):
    archive, local = tmp_path / "shared", tmp_path / "active"
    archive.mkdir()
    (archive / "failed-attempt").write_bytes(b"x" * 20)
    budget = collect.StorageBudget(local, max_bytes=100, archive_root=archive,
                                   min_free_bytes=collect.V5_MIN_FREE_BYTES)
    monkeypatch.setattr(collect.shutil, "disk_usage", lambda root: SimpleNamespace(free=collect.V5_MIN_FREE_BYTES + 100))
    assert budget.used() == 20
    budget.require_room(80)
    with pytest.raises(collect.StorageLimit, match="used=20"):
        budget.require_room(81)
    monkeypatch.setattr(collect.shutil, "disk_usage", lambda root: SimpleNamespace(free=collect.V5_MIN_FREE_BYTES))
    with pytest.raises(collect.StorageLimit):
        budget.require_room(1)


def frozen_v5_base():
    return dict(schema_version=1, corpus_version=5, navigation_memory_version=4, demo_version=3,
        perception_mode="gaze", physics="cv", validation_attempt_2_sha256=None,
        input_sha256={"save.sav": "save-hash"}, selection_sha256={"jump": "jump-selection"},
        knownroads_sha256={"roads-snapshots.jsonl": "memberships-hash"}, v5_input_sha256={"familiar.bin": "map-hash"},
        runner_sha256="old-runner", contract_sha256="unchanged-contract", include_follow=False,
        limit_ms=240000, max_bytes=collect.V5_MAX_BYTES, seed=collect.SEED,
        student_input="hd", teacher_familiar_hint=False)


def upgraded_v5_settings():
    previous = collect.collection_settings(frozen_v5_base(), follow_below_gate=True,
                                           source_follow_sha256="frozen-follow")
    current = dict(frozen_v5_base(), runner_sha256="memory4-v2-runner")
    upgraded = collect.collection_settings(current, previous, follow_below_gate=True,
        source_follow_sha256="frozen-follow", upgrade_v2_memory=True)
    return previous, current, upgraded


def test_v2_memory_upgrade_is_explicit_once_and_keeps_the_exact_previous_base():
    previous, current, upgraded = upgraded_v5_settings()
    before = deepcopy(previous)
    assert upgraded["v2_memory4_previous_base"] == frozen_v5_base()
    assert upgraded["base_recording_identity"] == current
    assert upgraded["family_policies"] == previous["family_policies"]
    with pytest.raises(ValueError, match="another binary"):
        collect.collection_settings(current, previous, follow_below_gate=True, source_follow_sha256="frozen-follow")
    assert collect.collection_settings(current, upgraded, follow_below_gate=True,
                                      source_follow_sha256="frozen-follow") == upgraded
    assert collect.collection_settings(current, upgraded, follow_below_gate=True,
        source_follow_sha256="frozen-follow", upgrade_v2_memory=True) == upgraded
    with pytest.raises(ValueError, match="another binary"):
        collect.collection_settings(dict(current, runner_sha256="third-runner"), upgraded, follow_below_gate=True,
            source_follow_sha256="frozen-follow", upgrade_v2_memory=True)
    assert previous == before


@pytest.mark.parametrize("field, changed", [
    ("contract_sha256", "changed-contract"), ("input_sha256", {"save.sav": "changed-save"}),
    ("selection_sha256", {"jump": "changed-selection"}),
    ("knownroads_sha256", {"roads-snapshots.jsonl": "changed-memberships"}),
    ("v5_input_sha256", {"familiar.bin": "changed-map"}), ("navigation_memory_version", 3),
    ("demo_version", 2), ("limit_ms", 100), ("student_input", "other"),
])
def test_v2_memory_upgrade_rejects_changed_frozen_inputs(field, changed):
    previous, current, _ = upgraded_v5_settings()
    with pytest.raises(ValueError):
        collect.collection_settings(dict(current, **{field: changed}), previous, follow_below_gate=True,
            source_follow_sha256="frozen-follow", upgrade_v2_memory=True)


def test_v2_memory_upgrade_rejects_changed_family_policy_or_old_provenance():
    previous, current, upgraded = upgraded_v5_settings()
    with pytest.raises(ValueError, match="authorization policy"):
        collect.collection_settings(current, previous, follow_below_gate=True,
            source_follow_sha256="changed-follow", upgrade_v2_memory=True)
    tampered = deepcopy(upgraded)
    tampered["v2_memory4_previous_base"]["input_sha256"]["save.sav"] = "changed-save"
    with pytest.raises(ValueError, match="upgrade provenance"):
        collect.collection_settings(current, tampered, follow_below_gate=True, source_follow_sha256="frozen-follow")


@pytest.mark.parametrize("tagged_follow", [False, True])
def test_resumed_identity_accepts_only_current_or_exact_previous_binary(tagged_follow):
    previous, _, upgraded = upgraded_v5_settings()
    value = scenario()
    if tagged_follow:
        value.update(goal_kind="follow", dataset_family="follow_below_gate", follow_below_gate=True,
                     minimum_follow_fraction=.7)
    for identity in (collect.recording_identity(previous, value), collect.recording_identity(upgraded, value)):
        metadata = dict(id=value["id"], recording_identity=identity)
        assert collect.resumed_recording_identity(upgraded, value, metadata) == identity
    forged = dict(collect.recording_identity(upgraded, value), runner_sha256="forged-runner")
    with pytest.raises(ValueError, match="frozen binary"):
        collect.resumed_recording_identity(upgraded, value, dict(id=value["id"], recording_identity=forged))


def test_resuming_previous_memory4_episode_preserves_episode_and_samples(tmp_path, monkeypatch):
    previous, _, upgraded = upgraded_v5_settings()
    value = collect.resident_scenario(scenario(), memberships())
    directory = collect.directory_for(tmp_path, value)
    directory.mkdir(parents=True)
    (directory / "samples.bin.gz").write_bytes(b"original memory4 episode")
    for name in ("actions.txt", "trajectory.txt"):
        (directory / name).write_text("original output\n")
    digest = collect.campaign.binary_digest(directory / "samples.bin.gz")
    measured = dict(id=value["id"], success=True, training_eligible=True, decisions=2)
    metadata = dict(native(value), **{key: item for key, item in value.items() if key != "seed"},
        command_seed=value["seed"], recording_identity=collect.recording_identity(previous, value),
        teacher_variant="v3b", teacher_familiar_hint=False, collection_metrics=measured, samples_sha256=digest)
    collect.atomic_json(directory / "episode.json", metadata)
    before = {path.name: (path.read_bytes(), path.stat().st_mtime_ns) for path in directory.iterdir()}
    monkeypatch.setattr(collect, "residency_inputs", lambda task: memberships())
    options = SimpleNamespace(task=tmp_path, corpus_version=5, limit_ms=240000)
    worker = collect.Worker(options, 0, None, upgraded)
    monkeypatch.setattr(collect.demos, "summarize", lambda *args, **kwargs: dict(
        navigation_memory_versions=[4], format_versions=[3]))
    monkeypatch.setattr(collect, "measured_episode", lambda *args: dict(measured))
    monkeypatch.setattr(worker, "scenario", lambda *args: pytest.fail("A completed recording cannot run again"))
    assert worker.trial(value)["samples_sha256"] == digest
    assert {path.name: (path.read_bytes(), path.stat().st_mtime_ns) for path in directory.iterdir()} == before


def old_jump(task, index):
    value = dict(scenario(index, id=f"jump-{index:04d}", dataset_family="jump"), index=index)
    directory = collect.directory_for(task, value)
    directory.mkdir(parents=True)
    metadata = dict(native(value | {"local": False}), demo_version=2, navigation_memory_version=3,
                    journey_memory=False, familiar_map=False)
    collect.atomic_json(directory / "episode.json", metadata)
    for name in ("samples.bin.gz", "actions.txt", "trajectory.txt", "bench.log"):
        (directory / name).write_bytes(f"unchanged old jump {index} {name}\n".encode())
    return value, directory


@pytest.mark.parametrize("interrupt_delete", [False, True])
def test_obsolete_three_raw_jumps_archive_byte_exactly_and_resume_idempotently(tmp_path, monkeypatch, interrupt_delete):
    task, archive = tmp_path / "active", tmp_path / "shared"
    archive.mkdir()
    _, _, upgraded = upgraded_v5_settings()
    attempts = [old_jump(task, index) for index in range(3)]
    before = {value["id"]: collect.v2_memory3_file_hashes(directory) for value, directory in attempts}
    state = collect.CollectionState(task / "recordings/collection-state.json", upgraded, archive_root=archive)
    options = SimpleNamespace(task=task, archive_root=archive, max_bytes=collect.V5_MAX_BYTES)
    monkeypatch.setattr(collect.demos, "summarize", lambda *args, **kwargs: dict(
        navigation_memory_versions=[3], format_versions=[2]))
    transfers = []
    def copy(command, *, check):
        assert command[:4] == ["rsync", "-a", "--no-links", "--"] and check
        transfers.append(command)
        shutil.copytree(command[-2], command[-1], dirs_exist_ok=True)
    monkeypatch.setattr(collect.subprocess, "run", copy)
    original_delete = collect.shutil.rmtree
    if interrupt_delete:
        monkeypatch.setattr(collect.shutil, "rmtree", lambda path: (_ for _ in ()).throw(OSError("interrupted delete")))
        with pytest.raises(OSError, match="interrupted delete"):
            collect.archive_v2_memory3(options, {"jump": [value for value, _ in attempts]}, state)
        monkeypatch.setattr(collect.shutil, "rmtree", original_delete)
    collect.archive_v2_memory3(options, {"jump": [value for value, _ in attempts]}, state)
    for value, directory in attempts:
        target = archive / "superseded-v2-memory3/jump/train" / value["id"]
        assert not directory.exists()
        assert collect.v2_memory3_file_hashes(target) == before[value["id"]]
        assert collect.resolve_directory(options, value) == directory
        assert not collect.directory_for(archive, value).exists()
    assert len(state.data["superseded_v2_memory3"]) == 3
    assert not state.data["integrity"]["episodes"] and not state.saved
    transfer_count = len(transfers)
    collect.archive_v2_memory3(options, {"jump": [value for value, _ in attempts]}, state)
    assert len(transfers) == transfer_count


@pytest.mark.parametrize("collision", ["summary", "pin", "metadata"])
def test_obsolete_jump_cannot_discard_finalized_state(tmp_path, monkeypatch, collision):
    _, _, upgraded = upgraded_v5_settings()
    value, directory = old_jump(tmp_path, 0)
    state = collect.CollectionState(tmp_path / "recordings/collection-state.json", upgraded,
                                    archive_root=tmp_path / "shared")
    if collision == "summary":
        state.remember_metrics(directory, {"id": value["id"]})
    elif collision == "pin":
        state.pin_samples(directory, "already-pinned")
    else:
        metadata = collect.v3.read_json(directory / "episode.json")
        collect.atomic_json(directory / "episode.json", dict(metadata, id=value["id"]))
    before = collect.v2_memory3_file_hashes(directory)
    monkeypatch.setattr(collect.subprocess, "run", lambda *args, **kwargs: pytest.fail("Must not archive a finalized episode"))
    with pytest.raises(ValueError, match="collides with finalized"):
        collect.archive_v2_memory3(SimpleNamespace(task=tmp_path, archive_root=tmp_path / "shared"), {"jump": [value]}, state)
    assert collect.v2_memory3_file_hashes(directory) == before
