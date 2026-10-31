"""Pure phase forecasts: retained recordings, equal roads, fixed other goals."""
from copy import deepcopy
import importlib.util
import json
from pathlib import Path

import pytest


SPEC = importlib.util.spec_from_file_location("nav_demos_v4_budget", Path(__file__).resolve().parents[1] / "nav_demos_v4_budget.py")
budget = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(budget)


def corpus(point_count=126, decisions=1200):
    selections = {name: [] for name in budget.TARGETS}
    selections["point"] = [dict(id=f"p-{index}", region=f"r{index % 6}") for index in range(400)]
    episodes = [dict(value, family="point", decisions=decisions, payload_bytes=decisions * 16000,
                     training_eligible=True) for value in selections["point"][:point_count]]
    for name, count, good, average in (("object", 120, 98, 675), ("approach", 81, 61, 1500),
                                     ("jump", 108, 90, 191), ("follow_below_gate", 48, 29, 2352)):
        candidates = 168 if name == "jump" else 410
        selections[name] = [dict(id=f"{name}-{index}") for index in range(candidates)]
        episodes.extend(dict(family=name, id=f"{name}-{index}", decisions=average,
            payload_bytes=average * 16000, training_eligible=index < good) for index in range(count))
    return dict(episodes=episodes), selections


def test_measured_cost_reduces_only_points_with_equal_regions_and_keeps_old_pairs():
    summary, selections = corpus()
    before = deepcopy((summary, selections))
    result = budget.plan(summary, selections, 6_556_700_000)
    assert result["can_collect"] and result["reason"] == "point_reduced_for_budget"
    assert 126 <= result["point_limit"] < 400 and result["point_limit"] % 6 == 0
    assert len({row["quota"] for row in result["point_regions"].values()}) == 1
    assert {row["id"] for row in summary["episodes"] if row["family"] == "point"} <= set(result["point_ids"])
    assert len(result["point_ids"]) == result["point_limit"]
    assert result["forecast_bytes"] <= 15_000_000_000 < result["unreduced_forecast_bytes"]
    assert result["targets"] == dict(budget.TARGETS, point=result["point_limit"])
    assert (summary, selections) == before


def test_no_reduction_when_original_400_fit_and_minimum_decision_cost_applies():
    summary, selections = corpus(decisions=200)
    result = budget.plan(summary, selections, 3_000_000_000)
    assert result["point_limit"] == 400 and result["reason"] == "within_budget"
    assert result["by_family"]["point"]["forecast_bytes_per_episode"] == 200 * 17000
    assert result["point_ids"] == [value["id"] for value in selections["point"]]


def test_previous_reduced_cap_never_grows_when_cost_becomes_lower():
    summary, selections = corpus(decisions=200)
    result = budget.plan(summary, selections, 3_000_000_000, previous_point_limit=300)
    assert result["point_limit"] == 300 and result["previous_point_limit"] == 300
    assert result["targets"]["follow_below_gate"] == 60


def test_reduction_to_already_recorded_balanced_points_marks_corpus_complete():
    summary, selections = corpus()
    for value in summary["episodes"]:
        if value["family"] in ("object", "follow_below_gate"):
            value["training_eligible"] = True
    for name, begin, end in (("jump", 108, 168), ("follow_below_gate", 48, 60)):
        summary["episodes"].extend(dict(family=name, id=f"{name}-{index}",
            decisions=200, payload_bytes=3_000_000, training_eligible=True)
            for index in range(begin, end))
    result = budget.plan(summary, selections, 14_700_000_000, previous_point_limit=132)
    assert result["point_limit"] == 126 and result["point_reduced"]
    assert all(value["complete"] for value in result["by_family"].values())
    assert result["reason"] == "complete" and not result["can_collect"]
    assert result["forecast_bytes"] == 14_700_000_000


def test_larger_cost_can_reduce_previous_cap_further():
    summary, selections = corpus(decisions=2200)
    result = budget.plan(summary, selections, 6_556_700_000, previous_point_limit=378)
    assert result["can_collect"] and result["point_limit"] < 378
    assert result["point_limit"] >= 126 and result["point_limit"] % 6 == 0


def test_nonpoint_forecast_that_cannot_fit_stops_without_reducing_other_goals():
    summary, selections = corpus()
    result = budget.plan(summary, selections, 14_500_000_000)
    assert not result["can_collect"] and result["reason"] == "budget_blocked_nonpoint"
    assert result["targets"] == budget.TARGETS


def test_recorded_region_imbalance_sets_minimum_quota_and_retains_nonprefix_ids():
    summary, selections = corpus(point_count=0, decisions=1200)
    old = [value for value in selections["point"] if value["region"] == "r0"][-50:]
    summary["episodes"].extend(dict(value, family="point", decisions=1200,
        payload_bytes=19_000_000, training_eligible=True) for value in old)
    result = budget.plan(summary, selections, 6_556_700_000)
    assert set(value["id"] for value in old) <= set(result["point_ids"])
    assert result["point_regions"]["r0"]["recorded"] == 50
    assert result["point_limit"] >= 300


def test_cannot_cut_below_already_recorded_region_quota():
    summary, selections = corpus(point_count=0)
    old = [value for value in selections["point"] if value["region"] == "r0"]
    summary["episodes"].extend(dict(value, family="point", decisions=2400,
        payload_bytes=38_000_000, training_eligible=True) for value in old)
    result = budget.plan(summary, selections, 6_556_700_000)
    assert not result["can_collect"] and result["reason"] == "budget_blocked_preserve_recorded_points"
    assert result["point_limit"] == 400


def test_exhausted_success_candidate_pool_stops():
    summary, selections = corpus()
    selections["object"] = selections["object"][:120]
    result = budget.plan(summary, selections, 3_000_000_000)
    assert not result["can_collect"] and result["reason"] == "candidate_pool_exhausted"
    assert result["exhausted_families"] == ["object"]


def test_rejections_consume_candidates_without_counting_as_episodes_or_lowering_quota():
    summary, selections = corpus(decisions=200)
    baseline = budget.plan(summary, selections, 3_000_000_000)
    summary["rejected_candidates"] = [dict(id="follow_below_gate-48", family="follow_below_gate",
        reason="moving route needs distinct connected points", training_eligible=False,
        decisions=2400, payload_bytes=40_000_000)]
    result = budget.plan(summary, selections, 3_000_000_000)
    before = baseline["by_family"]["follow_below_gate"]
    after = result["by_family"]["follow_below_gate"]
    assert result["can_collect"] and result["targets"] == baseline["targets"]
    assert after["rejected_candidates"] == 1
    assert after["candidates_remaining"] == before["candidates_remaining"] - 1
    assert {key: value for key, value in after.items() if key not in ("candidates_remaining", "rejected_candidates")} == {
        key: value for key, value in before.items() if key not in ("candidates_remaining", "rejected_candidates")}
    assert after["training_eligible"] == 29 and after["remaining_needed"] == 31


def test_rejected_last_candidate_exhausts_pool_without_satisfying_success_target():
    summary, selections = corpus(decisions=200)
    selections["follow_below_gate"] = selections["follow_below_gate"][:49]
    summary["rejected_candidates"] = [dict(id="follow_below_gate-48", family="follow_below_gate")]
    result = budget.plan(summary, selections, 3_000_000_000)
    assert not result["can_collect"] and result["reason"] == "candidate_pool_exhausted"
    assert result["exhausted_families"] == ["follow_below_gate"]
    assert not result["by_family"]["follow_below_gate"]["complete"]


def test_reduced_point_quotas_skip_rejected_candidates_and_select_replacements_in_same_region():
    summary, selections = corpus(decisions=200)
    summary["rejected_candidates"] = [dict(id=f"p-{index}", family="point") for index in (126, 132)]
    result = budget.plan(summary, selections, 3_000_000_000, previous_point_limit=372)
    assert result["can_collect"] and result["point_limit"] == 372
    assert len(result["point_ids"]) == 372
    assert {"p-126", "p-132"}.isdisjoint(result["point_ids"])
    assert {"p-372", "p-378"} <= set(result["point_ids"])
    assert {row["quota"] for row in result["point_regions"].values()} == {62}
    assert result["by_family"]["point"]["recorded_episodes"] == 126


def test_point_region_pool_exhaustion_does_not_count_rejections_toward_recording_quota():
    summary, selections = corpus(decisions=200)
    summary["rejected_candidates"] = [dict(id="p-126", family="point")]
    result = budget.plan(summary, selections, 3_000_000_000)
    assert result["point_limit"] == 400
    assert not result["can_collect"] and result["reason"] == "candidate_pool_exhausted"
    assert result["exhausted_families"] == ["point"]
    assert result["by_family"]["point"]["remaining_needed"] == 274


def test_task_plan_merges_new_state_rejections_with_saved_summary_without_double_counting(tmp_path):
    summary, selections = corpus(decisions=200)
    rejected = dict(id="follow_below_gate-48", family="follow_below_gate")
    summary["rejected_candidates"] = [rejected]
    recordings = tmp_path / "recordings"
    recordings.mkdir()
    (recordings / "collection-metrics.json").write_text(json.dumps(summary))
    (recordings / "collection-state.json").write_text(json.dumps(dict(rejected_candidates={
        "follow_below_gate/train/48": rejected,
        "follow_below_gate/train/49": dict(id="follow_below_gate-49", family="follow_below_gate")})))
    inputs = tmp_path / "recording-input"
    inputs.mkdir()
    for name, values in selections.items():
        (inputs / f"{name}.jsonl").write_text("".join(json.dumps(value) + "\n" for value in values))
    result = budget.task_plan(tmp_path, free_bytes=100_000_000_000)
    assert result["by_family"]["follow_below_gate"]["rejected_candidates"] == 2
    assert result["by_family"]["follow_below_gate"]["candidates_remaining"] == 410 - 48 - 2
    assert json.loads((recordings / "collection-metrics.json").read_text()) == summary


def test_empty_success_history_is_conservative_and_complete_families_cost_zero():
    summary, selections = corpus()
    for value in summary["episodes"]:
        if value["family"] == "follow_below_gate":
            value["training_eligible"] = False
    result = budget.plan(summary, selections, 3_000_000_000)
    assert result["by_family"]["follow_below_gate"]["eligible_rate"] == 1 / 49
    assert result["by_family"]["approach"]["estimated_remaining_attempts"] == 0
    assert not result["can_collect"]


def test_physical_free_space_also_limits_future_recording_budget():
    summary, selections = corpus()
    result = budget.plan(summary, selections, 6_556_700_000, free_bytes=100_000_000)
    assert not result["can_collect"]
    assert result["effective_limit_bytes"] == 6_556_700_000


@pytest.mark.parametrize("limit", [0, 399, 401])
def test_invalid_previous_cap_is_rejected(limit):
    summary, selections = corpus()
    with pytest.raises(ValueError, match="previous point limit"):
        budget.plan(summary, selections, 3_000_000_000, previous_point_limit=limit)
