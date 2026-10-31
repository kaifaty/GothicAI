"""Contracts for frozen, eval-only memory comparisons and resident selection."""
import importlib.util
import json
from pathlib import Path

import pytest

SPEC = importlib.util.spec_from_file_location("nav_memory_v4", Path(__file__).resolve().parents[1] / "nav_memory_v4.py")
measurement = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(measurement)


def test_resident_selection_uses_start_routines_and_ignores_goal():
    actors = [dict(instance="LOCAL_A", routines=[dict(node=0)]),
              dict(instance="LOCAL_B", routines=[dict(node=1)])]
    nodes = [["A", 0, 0, 0], ["B", 10000, 0, 0]]
    case = dict(start="cm:100,0,0", goal="cm:10000,0,0")
    first = measurement.local_actor(case, actors, nodes, {})
    assert first["npc"] == "LOCAL_A"
    assert first["nearest_routine_to_start_cm"] == 100
    assert measurement.local_actor(dict(case, goal="secret-goal"), actors, nodes, {}) == first
    assert measurement.local_actor(dict(case, start="START"), actors, nodes, {"START": [100, 0, 0]}) == first


def test_same_actor_and_teacher_are_used_without_recording_or_oracle(tmp_path):
    case = dict(goal_kind="point", start="START", goal="GOAL")
    for mode in measurement.MODES:
        commands = measurement.command_sequence(tmp_path, case, "REGIONAL_LOCAL", mode)
        assert "navdemos record off" in commands and "navdemos oracle off" in commands
        assert commands[-2:] == ["navdemos start-v3 REGIONAL_LOCAL START GOAL 240000", "navdemos wait"]
        assert (f"roads {'stranger' if mode.endswith('-stranger') else 'on'} REGIONAL_LOCAL") in commands
        assert any("roads familiar-load" in command for command in commands) == mode.startswith("familiar-")


@pytest.mark.parametrize("local,stranger,journey,familiar", [
    ("baseline", "baseline-stranger", "0", "0"),
    ("journey", "journey-stranger", "1", "0"),
    ("familiar-local", "familiar-stranger", "1", "1"),
])
def test_residency_is_independent_of_memory_mode(monkeypatch, local, stranger, journey, familiar):
    monkeypatch.setenv("OPENGOTHIC_NAV_JOURNEY_MEMORY", "old")
    monkeypatch.setenv("OPENGOTHIC_NAV_FAMILIAR_MAP", "old")
    for mode in (local, stranger):
        measurement.mode_environment(mode)
        assert measurement.os.environ["OPENGOTHIC_NAV_JOURNEY_MEMORY"] == journey
        assert measurement.os.environ["OPENGOTHIC_NAV_FAMILIAR_MAP"] == familiar


@pytest.mark.parametrize("mode,memory_version,demo_version,journey", [
    ("baseline", 3, 2, False),
    ("baseline-stranger", 3, 2, False),
    ("journey", 4, 3, True),
    ("journey-stranger", 4, 3, True),
    ("familiar-local", 4, 3, True),
    ("familiar-stranger", 4, 3, True),
])
def test_representation_validation_uses_memory_mode_for_both_residencies(tmp_path, mode, memory_version, demo_version, journey):
    case = dict(start="START", goal="GOAL", goal_kind="point")
    metadata = dict(case, teacher_version=3, demo_version=demo_version, navigation_memory_version=memory_version,
        journey_memory=journey, limit_ms=240000, perception_mode="gaze", oracle=False,
        observation_source="learner_memory", record=False, demonstration_recorded=False)
    measurement.validate_raw(tmp_path, metadata, case, mode)


@pytest.mark.parametrize("mode", measurement.MODES)
def test_training_recordings_and_wrong_representation_are_rejected(tmp_path, mode):
    case = dict(start="START", goal="GOAL", goal_kind="point")
    journey = not mode.startswith("baseline")
    metadata = dict(case, teacher_version=3, demo_version=3 if journey else 2, navigation_memory_version=4 if journey else 3,
        journey_memory=journey, limit_ms=240000, perception_mode="gaze", oracle=False,
        observation_source="learner_memory", record=False, demonstration_recorded=False)
    measurement.validate_raw(tmp_path, metadata, case, mode)
    for changes in (dict(record=True), dict(demonstration_recorded=True), dict(oracle=True), dict(drive=True),
                    dict(demo_version=2 if journey else 3), dict(navigation_memory_version=3 if journey else 4),
                    dict(journey_memory=not journey)):
        with pytest.raises(ValueError, match="identity or no-recording"):
            measurement.validate_raw(tmp_path, dict(metadata, **changes), case, mode)
    (tmp_path / "samples.bin.gz").write_bytes(b"forbidden")
    with pytest.raises(ValueError, match="identity or no-recording"):
        measurement.validate_raw(tmp_path, metadata, case, mode)


@pytest.mark.parametrize("mode", measurement.MODES[2:])
def test_completed_v4_native_episode_resumes_without_running_or_recreating_payload(tmp_path, monkeypatch, mode):
    case = dict(id="completed-native", cohort="beyond-roads", start="START", goal="GOAL", goal_kind="point", seed=42)
    directory = tmp_path / "evaluation" / mode / case["cohort"] / case["id"]
    directory.mkdir(parents=True)
    raw = dict(start=case["start"], goal=case["goal"], goal_kind=case["goal_kind"], teacher_version=3,
        demo_version=3, navigation_memory_version=4, journey_memory=True, limit_ms=240000,
        perception_mode="gaze", oracle=False, observation_source="learner_memory", record=False,
        demonstration_recorded=False, decisions=1, elapsed_ms=100, success=True, detour_length_cm=100)
    measurement.v3.write_json(directory / "episode.json", raw)
    payload = {"actions.txt": b"0 1 0 0 0 0 0 0 0\n100 0 0 0 1 0 0 0 0\n",
               "trajectory.txt": b"0 0 0 0 0\n100 100 0 0 0\n", "teacher-paths.jsonl": b""}
    for name, content in payload.items():
        (directory / name).write_bytes(content)
    stats = dict(kind="familiar_stats", actor="LOCAL", stranger=mode.endswith("-stranger"))
    (directory / "roads-events.jsonl").write_text(json.dumps(stats) + "\n" if mode.startswith("familiar-") else "")
    worker = measurement.Worker(measurement.args_for(tmp_path, tmp_path / "worker"), 3, None)

    def forbidden(*args, **kwargs):
        pytest.fail("A completed native evaluation must not start a runner or replay its scenario")

    monkeypatch.setattr(worker, "ready", forbidden)
    monkeypatch.setattr(worker, "scenario", forbidden)
    selection = dict(npc="LOCAL", nearest_routine_to_start_cm=0, selection="frozen selection")
    current = dict(mode=mode, runner_sha256="frozen-binary")
    first = worker.trial(tmp_path, case, mode, selection, current)
    enriched = (directory / "episode.json").read_bytes()
    assert worker.trial(tmp_path, case, mode, selection, current) == first
    assert (directory / "episode.json").read_bytes() == enriched
    assert first["success"] is True and first["length_over_detour"] == 1
    saved = json.loads(enriched)
    assert all(saved[key] == value for key, value in raw.items())
    assert saved["memory_v4_comparison"] == current and saved["resident_selection"] == selection
    assert {name: (directory / name).read_bytes() for name in payload} == payload
    assert not (directory / "samples.bin.gz").exists()


@pytest.fixture
def npc_stats_stand(tmp_path, monkeypatch):
    actors = [dict(instance="LOCAL_A", routines=[dict(node=0)]),
              dict(instance="LOCAL_B", routines=[dict(node=1)])]
    nodes = [["A", 0, 0, 0], ["B", 10000, 0, 0]]
    monkeypatch.setattr(measurement, "discovery", lambda task: (actors, nodes, {}))
    monkeypatch.setattr(measurement, "frozen_cases", lambda task: [dict(start="cm:100,0,0"), dict(start="cm:200,0,0")])
    monkeypatch.setattr(measurement, "frozen_safety", lambda task: [dict(projected_start_cm=[10000, 0, 0])])
    stats = [dict(kind="familiar_stats", actor=npc, stranger=stranger, npc_count=300, max_bytes=50_000_000,
                  total_bytes_for_npcs=49_000_000, known_tiles=0 if stranger else 2,
                  known_2m_tiles_xz=0 if stranger else 2, eligible_2m_tiles=0 if stranger else 4,
                  familiar_source_poses=0 if stranger else 1, coverage_fraction=0 if stranger else .5)
             for npc in ("LOCAL_A", "LOCAL_B") for stranger in (False, True)]
    captured = []

    def stand(task, directory, commands):
        assert task == tmp_path and directory == tmp_path / "bake-npcs" and directory.is_absolute()
        captured.extend(commands)
        directory.mkdir()
        (directory / "roads-events.jsonl").write_text("".join(json.dumps(row) + "\n" for row in stats))

    monkeypatch.setattr(measurement, "stand", stand)
    return tmp_path, captured, stats


def test_bake_measures_unique_npcs_from_route_and_safety_starts(npc_stats_stand):
    task, commands, stats = npc_stats_stand
    measurement.bake_npc_stats(task)
    assert commands == [f"roads load {task / 'input/places.json'}", f"roads familiar-load {task / 'input/familiar.bin'}",
                        "roads on LOCAL_A", "roads familiar-stats LOCAL_A 300",
                        "roads stranger LOCAL_A", "roads familiar-stats LOCAL_A 300",
                        "roads on LOCAL_B", "roads familiar-stats LOCAL_B 300",
                        "roads stranger LOCAL_B", "roads familiar-stats LOCAL_B 300"]
    assert json.loads((task / "bake-npcs/memory-cost.json").read_text()) == dict(
        scenario_starts=3, npc_count=300, unique_npc_count=2, total_bytes_for_300_npcs=49_000_000, measurements=stats)


@pytest.mark.parametrize("problem", ("missing", "duplicate", "budget", "stranger", "coverage"))
def test_bake_rejects_missing_stats_over_budget_or_stranger_knowledge(npc_stats_stand, problem):
    task, _, stats = npc_stats_stand
    if problem == "missing":
        stats.pop()
    elif problem == "duplicate":
        stats[-1] = stats[0]
    elif problem == "budget":
        stats[0]["total_bytes_for_npcs"] = 50_000_001
    elif problem == "stranger":
        stats[1]["known_tiles"] = 1
    else:
        stats[0]["eligible_2m_tiles"] = 1
    with pytest.raises(ValueError):
        measurement.bake_npc_stats(task)
    assert not (task / "bake-npcs/memory-cost.json").exists()
