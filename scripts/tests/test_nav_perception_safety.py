"""Safety controls require physical CV evidence, independent of arrival."""
import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace

import pytest


SPEC = importlib.util.spec_from_file_location("nav_perception_safety", Path(__file__).resolve().parents[1] / "nav_perception_safety.py")
safety = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(safety)


def frame(time, height=100, **updates):
    value = dict(elapsed_ms=time, position_cm=[0, height, 0], cv_active=True, supported=True,
                 in_air=False, falling=False, native_jump=False, native_climb=False, swimming=False,
                 water_depth_cm=0, deep_water_threshold_cm=140, hitpoints=200, hitpoints_max=200)
    value.update(updates)
    return value


def assess(frames, **metadata):
    fixture = dict(label="cliff-bridge", kind="cliff", link_id=123, hazard_cm=[0, 0, 200], start_to_hazard_cm=200)
    result = dict(success=False, elapsed_ms=1000, failure="timeout")
    result.update(metadata)
    return safety.assess(fixture, result, frames)


def test_stop_before_hazard_is_safe_without_arrival():
    result = assess([frame(0), frame(1000)])
    assert result["safe"] and not result["goal_success"]


def test_successful_arrival_does_not_excuse_fall_with_damage():
    result = assess([frame(0), frame(100, 80, supported=False, falling=True, cv_active=False),
                     frame(200, 10, hitpoints=175)], success=True)
    assert not result["safe"]
    assert result["max_unsupported_descent_cm"] == 90
    assert result["harmful_fall"] and not result["inconclusive"]


def test_harmless_drop_over_step_is_diagnostic_not_failure():
    result = assess([frame(0), frame(100, 80, supported=False, falling=True), frame(200, 10)])
    assert result["safe"]
    assert result["harmless_descent_over_step"]
    assert not result["harmful_fall"]


def test_delayed_landing_damage_within_bounded_window_is_harmful():
    result = assess([frame(0), frame(100, 80, supported=False, falling=True), frame(200, 10),
                     frame(500, 10, hitpoints=175)])
    assert not result["safe"] and result["harmful_fall"]


def test_later_unrelated_damage_is_inconclusive_not_labeled_as_fall():
    result = assess([frame(0), frame(100, 80, supported=False, falling=True), frame(200, 10),
                     frame(900, 10, hitpoints=175)])
    assert not result["safe"] and result["inconclusive"]
    assert not result["harmful_fall"]
    assert result["unattributed_damage"] == 25


def test_damage_without_a_fall_cannot_claim_clean_safety_pass():
    result = assess([frame(0), frame(100, hitpoints=175)])
    assert not result["safe"] and result["inconclusive"]
    assert not result["harmful_fall"]


def test_gradual_grounded_downhill_does_not_accumulate_as_cliff():
    result = assess([frame(i * 100, 1000 - i * 20) for i in range(40)])
    assert result["safe"]
    assert result["max_unsupported_descent_cm"] == 0


def test_shallow_water_is_diagnostic_and_does_not_fail():
    result = assess([frame(0), frame(100, water_depth_cm=30)])
    assert result["safe"]
    assert result["entered_water_frames"] == 1
    assert result["swimming_frames"] == 0
    assert result["deep_water_frames"] == 0


@pytest.mark.parametrize("updates", [dict(water_depth_cm=140), dict(water_depth_cm=139),
                                      dict(water_depth_cm=130, deep_water_threshold_cm=130),
                                      dict(swimming=True, water_depth_cm=30)])
def test_actual_chest_depth_or_swimming_fails(updates):
    result = assess([frame(0), frame(100, **updates)])
    assert not result["safe"]


def test_initial_cv_activation_on_first_tick_is_ordinary_attempt():
    result = assess([frame(0, cv_active=False), frame(16), frame(100)])
    assert result["safe"]


def test_immediate_fall_after_supported_t0_is_result_not_preparation_failure():
    result = assess([frame(0, cv_active=False), frame(16, 80, supported=False, falling=True),
                     frame(100, 0, supported=False, falling=True, hitpoints=175)])
    assert not result["safe"] and result["harmful_fall"]


@pytest.mark.parametrize("frames,match", [([frame(0)], "Missing"),
    ([frame(100), frame(100)], "nonmonotonic"),
    ([frame(0, cv_active=False), frame(100, cv_active=False)], "lacks CV"),
    ([frame(10), frame(100)], "elapsed_ms=0"),
    ([frame(0, supported=False), frame(100)], "initial NPC"),
    ([frame(0), frame(100, water_depth_cm=float("nan"))], "water depth"),
    ([frame(0), frame(100, deep_water_threshold_cm=0)], "threshold"),
    ([frame(0), frame(100, hitpoints=None)], "hitpoints"),
    ([frame(0), frame(100, hitpoints_max=300)], "maximum health")])
def test_incomplete_evidence_cannot_pass(frames, match):
    with pytest.raises(ValueError, match=match):
        assess(frames)


def test_each_hazard_kind_needs_three_qualified_fixtures():
    fixtures = [dict(label=f"{kind}-{i}", kind=kind, qualified=True, reason="ok",
                     projected_start_cm=[300*i, 100, 0], goal_cm=[300*i, 0, 500], hazard_cm=[300*i, 0, 200])
                for kind in safety.KINDS for i in range(3)]
    assert len(safety.select_fixtures(fixtures)) == 9
    fixtures[-1]["qualified"] = False
    fixtures[-1]["reason"] = "no_real_height_drop"
    with pytest.raises(ValueError, match="ledge: qualified 2/3"):
        safety.select_fixtures(fixtures)


def test_different_seeds_rediscovering_same_shore_do_not_count_twice():
    fixtures = [dict(label=f"{kind}-{i}", kind=kind, qualified=True, reason="ok",
                     projected_start_cm=[300*i, 100, 0], goal_cm=[300*i, 0, 500], hazard_cm=[300*i, 0, 200])
                for kind in safety.KINDS for i in range(3)]
    water = [row for row in fixtures if row["kind"] == "water"]
    water[2]["hazard_cm"] = water[1]["hazard_cm"]
    with pytest.raises(ValueError, match="water: qualified 2/3"):
        safety.select_fixtures(fixtures)


def water_fixture(label, x):
    return dict(label=label, kind="water", qualified=True, reason="ok", link_id=0,
                projected_start_cm=[x, 100, 0], goal_cm=[x, 0, 800], hazard_cm=[x, 0, 500])


def test_water_followup_keeps_exact_original_three_and_freezes_independent_new_shores(tmp_path):
    original = [water_fixture(f"original-{i}", i * 1000) for i in range(3)]
    path = tmp_path / "old/fixtures.json"
    path.parent.mkdir()
    path.write_text(json.dumps([*original, dict(kind="cliff", qualified=True)]))
    before = path.read_bytes()
    loaded = safety.baseline_water_fixtures(path)
    candidates = [water_fixture("rediscovered-original", 300), water_fixture("new-a", 3000),
                  water_fixture("too-close-to-new-a", 3400),
                  dict(water_fixture("not-qualified", 5000), qualified=False),
                  water_fixture("new-b", 3500), water_fixture("new-c", 4000),
                  water_fixture("not-selected-after-three", 5000)]
    selected = safety.select_water_followup(loaded, candidates)
    assert selected[:3] == original
    assert [row["label"] for row in selected[3:]] == ["new-a", "new-b", "new-c"]
    assert safety.select_water_followup(loaded, selected) == selected
    assert path.read_bytes() == before


def test_water_followup_cannot_replace_a_failing_original_or_count_nearby_shore(tmp_path):
    original = [water_fixture(f"original-{i}", i * 1000) for i in range(3)]
    path = tmp_path / "fixtures.json"
    path.write_text(json.dumps(original[:2]))
    with pytest.raises(ValueError, match="exactly three qualified"):
        safety.baseline_water_fixtures(path)
    candidates = [water_fixture("new-a", 3000), water_fixture("new-b", 4000),
                  water_fixture("same-as-new-b", 4001)]
    with pytest.raises(ValueError, match="qualified 2/3 new shores.*no episodes attempted"):
        safety.select_water_followup(original, candidates)


@pytest.mark.parametrize("teacher", safety.TEACHERS)
def test_both_teachers_share_fixture_without_recording_or_oracle(teacher):
    args = SimpleNamespace(roads=Path("/own/roads"), npc="BAU_954_MALETH", limit_ms=30000)
    fixture = dict(projected_start_cm=[1, 2, 3], goal_cm=[4, 5, 6])
    commands = safety.command_sequence(args, fixture, teacher, "gaze")
    assert "navdemos record off" in commands and "navdemos oracle off" in commands
    assert "navdemos perception-monitor on" in commands
    assert commands[-2] == f"navdemos start-v{2 if teacher == 'v2' else 3} BAU_954_MALETH cm:1,2,3 cm:4,5,6 30000"
    assert commands[-1] == "navdemos wait"


@pytest.mark.parametrize("teacher", safety.TEACHERS)
def test_episode_validation_binds_teacher_representation_and_disables_recording(tmp_path, teacher):
    fixture = dict(projected_start_cm=[1, 2, 3], goal_cm=[4, 5, 6])
    metadata = dict(start="cm:1,2,3", goal="cm:4,5,6", demo_version=2,
                    teacher_version=2 if teacher == "v2" else 3, limit_ms=30000,
                    observation_source="detour_teacher_v2" if teacher == "v2" else "learner_memory",
                    oracle=False, navigation_memory_version=2, perception_mode="gaze",
                    record=False, demonstration_recorded=False)
    safety.validate_episode(tmp_path, metadata, fixture, teacher, "gaze", 30000)
    for field, value in (("teacher_version", 4), ("navigation_memory_version", 1), ("oracle", True),
                         ("perception_mode", "rows"), ("limit_ms", 1000)):
        with pytest.raises(ValueError, match="identity/memory version"):
            safety.validate_episode(tmp_path, dict(metadata, **{field: value}), fixture, teacher, "gaze", 30000)
    (tmp_path / "samples.bin.gz").write_bytes(b"bad")
    with pytest.raises(ValueError, match="unexpectedly recorded"):
        safety.validate_episode(tmp_path, metadata, fixture, teacher, "gaze", 30000)


@pytest.mark.parametrize("teacher", safety.TEACHERS)
def test_water_patch_representation_three_cannot_mix_with_historical_two(tmp_path, teacher):
    fixture = dict(projected_start_cm=[1, 2, 3], goal_cm=[4, 5, 6])
    metadata = dict(start="cm:1,2,3", goal="cm:4,5,6", demo_version=2,
                    teacher_version=2 if teacher == "v2" else 3, limit_ms=30000,
                    observation_source="detour_teacher_v2" if teacher == "v2" else "learner_memory",
                    oracle=False, navigation_memory_version=3, perception_mode="gaze",
                    record=False, demonstration_recorded=False)
    safety.validate_episode(tmp_path, metadata, fixture, teacher, "gaze", 30000, memory_version=3)
    with pytest.raises(ValueError, match="identity/memory version"):
        safety.validate_episode(tmp_path, metadata, fixture, teacher, "gaze", 30000)
    with pytest.raises(ValueError, match="identity/memory version"):
        safety.validate_episode(tmp_path, dict(metadata, navigation_memory_version=2), fixture,
                                teacher, "gaze", 30000, memory_version=3)
