#!/usr/bin/env python3
"""Forecast the next demo phase without changing frozen selections or recordings.

The forecast uses measured decisions per attempt and the larger of measured
payload size and 17,000 bytes per decision. Success-counted families use the
observed eligibility rate to estimate remaining attempts. One 10% allowance is
applied to future bytes, with another 256 MiB for phase metadata and log growth.
Only the road count may shrink; its six region quotas then remain equal.
"""
from __future__ import annotations

from collections import Counter
import json
import math
from pathlib import Path
import shutil


MAX_BYTES = 15_000_000_000
DECISION_BYTES_FLOOR = 17_000
FUTURE_BYTE_FACTOR = 1.10
HEADROOM_BYTES = 256 * 1024**2
DISK_FREE_FLOOR_BYTES = 20 * 1024**3
TARGETS = {"point": 400, "object": 100, "approach": 60, "jump": 168,
           "follow_below_gate": 60}
SUCCESS_FAMILIES = {"object", "approach", "follow_below_gate"}
DEFAULT_DECISIONS = {"point": 2400, "object": 2400, "approach": 2400,
                     "jump": 512, "follow_below_gate": 2400}


def family(value):
    return value.get("family", value.get("dataset_family", value.get("goal_kind")))


def rejection_rows(summary):
    values = summary.get("rejected_candidates", [])
    return list(values.values()) if isinstance(values, dict) else list(values)


def _point_selection(values, recorded, limit, rejected=()):
    regions = sorted({value["region"] for value in values})
    if len(regions) != 6:
        raise ValueError("Point budget requires the frozen six-region selection")
    counts = Counter(value["region"] for value in recorded)
    recorded_ids = {value["id"] for value in recorded}
    pool_ids = {value["id"] for value in values}
    if not recorded_ids <= pool_ids:
        raise ValueError("Recorded points are absent from the frozen selection")
    if len(recorded_ids) != len(recorded):
        raise ValueError("Duplicate completed point IDs")
    if limit == TARGETS["point"]:
        if len(values) != limit:
            raise ValueError("The original point selection must contain 400 pairs")
        quotas = Counter(value["region"] for value in values)
    else:
        if limit <= 0 or limit % 6:
            raise ValueError("Reduced point limit must be a positive multiple of six")
        quota = limit // 6
        if any(counts[region] > quota for region in regions):
            return None
        quotas = {region: quota for region in regions}
    remaining = Counter({region: quotas[region] - counts[region] for region in regions})
    selected = []
    for value in values:
        if value["id"] in recorded_ids:
            selected.append(value)
        elif value["id"] not in rejected and remaining[value["region"]] > 0:
            selected.append(value)
            remaining[value["region"]] -= 1
    return dict(ids=[value["id"] for value in selected], regions={region:
        dict(recorded=counts[region], quota=quotas[region]) for region in regions},
        exhausted_regions=[region for region in regions if remaining[region] > 0])


def _estimate(name, values, group, candidates, target, rejected=()):
    count = len(values) if values else int(group.get("episodes", 0))
    eligible = (sum(bool(value.get("training_eligible")) for value in values) if values
                else int(group.get("training_eligible", 0)))
    decisions = (sum(int(value.get("decisions", 0)) for value in values) if values
                 else int(group.get("decisions", 0)))
    payload = (sum(int(value.get("payload_bytes", 0)) for value in values) if values
               else int(group.get("dataset_bytes", 0)))
    mean_decisions = decisions / count if count else DEFAULT_DECISIONS[name]
    mean_payload = payload / count if count else 0
    bytes_per_attempt = max(mean_payload, mean_decisions * DECISION_BYTES_FLOOR)
    missing = max(0, target - (eligible if name in SUCCESS_FAMILIES else count))
    # A zero-success initial sample must not predict free future successes.
    rate = eligible / count if count and eligible else 1 / (count + 1) if count else 1.0
    attempts = math.ceil(missing / rate) if name in SUCCESS_FAMILIES else missing
    future_bytes = math.ceil(attempts * bytes_per_attempt * FUTURE_BYTE_FACTOR)
    recorded_ids = {value["id"] for value in values}
    candidate_ids = {value["id"] for value in candidates}
    rejected_count = len(set(rejected) & candidate_ids - recorded_ids)
    return dict(target=target, goal="eligible" if name in SUCCESS_FAMILIES else "episodes",
        recorded_episodes=count, training_eligible=eligible, recorded_decisions=decisions,
        recorded_payload_bytes=payload, eligible_rate=rate,
        mean_decisions_per_episode=mean_decisions, mean_payload_bytes_per_episode=mean_payload,
        forecast_bytes_per_episode=bytes_per_attempt, remaining_needed=missing,
        estimated_remaining_attempts=attempts, forecast_additional_bytes=future_bytes,
        rejected_candidates=rejected_count,
        candidates_remaining=max(0, len(candidates) - count - rejected_count), complete=missing == 0)


def plan(summary, selections, used_bytes, max_bytes=MAX_BYTES,
         previous_point_limit=400, free_bytes=None):
    """Return an auditable bounded plan, preserving all completed recordings.

    ``selections`` contains the original frozen lists, not a reduced replacement.
    The returned point IDs are advisory evidence; collectors can apply the same
    deterministic per-region quota to pending candidates using ``point_limit``.
    """
    if not 0 < max_bytes <= MAX_BYTES or used_bytes < 0:
        raise ValueError("Invalid recording byte budget")
    if (previous_point_limit > 400 or previous_point_limit <= 0
            or previous_point_limit != 400 and previous_point_limit % 6):
        raise ValueError("Invalid previous point limit")
    episodes = summary.get("episodes", [])
    grouped = {name: [value for value in episodes if family(value) == name] for name in TARGETS}
    rejected = {name: {value["id"] for value in rejection_rows(summary) if family(value) == name}
                for name in TARGETS}
    groups = summary.get("by_family", {})
    points = grouped["point"]
    # Point records are required here because their region quotas cannot be
    # reconstructed from aggregate counts alone.
    if not points and groups.get("point", {}).get("episodes", 0):
        raise ValueError("Saved point episode details are required for region quotas")

    def estimates(limit):
        return {name: _estimate(name, grouped[name], groups.get(name, {}),
            selections.get(name, []), limit if name == "point" else target, rejected[name])
            for name, target in TARGETS.items()}

    def total(by_family):
        future = sum(value["forecast_additional_bytes"] for value in by_family.values())
        return int(used_bytes) + future + (HEADROOM_BYTES if future else 0)

    # Match the shared machine's admission floor; retain that free space even
    # when the corpus's own 15 GB ceiling would permit another phase.
    effective_limit = (min(max_bytes, used_bytes + max(0, free_bytes - DISK_FREE_FLOOR_BYTES))
                       if free_bytes is not None else max_bytes)
    current = _point_selection(selections["point"], points, previous_point_limit, rejected["point"])
    if current is None:
        raise ValueError("Previous point quota excludes completed recordings")
    chosen, by_family = previous_point_limit, estimates(previous_point_limit)
    unreduced = total(by_family)
    reason = "within_budget"
    if not any(not value["complete"] for value in by_family.values()):
        reason = "complete"
    elif unreduced > effective_limit:
        nonpoint = {name: value for name, value in by_family.items() if name != "point"}
        if total(nonpoint) > effective_limit:
            reason = "budget_blocked_nonpoint"
        else:
            found = None
            first = min(396, (previous_point_limit // 6) * 6)
            minimum = max(6, max((value["recorded"] for value in current["regions"].values()), default=0) * 6)
            for limit in range(first, minimum - 1, -6):
                selected = _point_selection(selections["point"], points, limit, rejected["point"])
                if selected is None or selected["exhausted_regions"]:
                    continue
                candidate_estimates = estimates(limit)
                if total(candidate_estimates) <= effective_limit:
                    found = (limit, selected, candidate_estimates)
                    break
            if found is None:
                reason = "budget_blocked_preserve_recorded_points"
            else:
                chosen, current, by_family = found
                reason = "point_reduced_for_budget"
    incomplete = [name for name, value in by_family.items() if not value["complete"]]
    exhausted = [name for name in incomplete if not by_family[name]["candidates_remaining"]]
    if current["exhausted_regions"] and "point" in incomplete and "point" not in exhausted:
        exhausted.append("point")
    if not incomplete:
        reason = "complete"
    elif exhausted:
        reason = "candidate_pool_exhausted"
    if used_bytes >= effective_limit and incomplete:
        reason = "disk_budget_exhausted"
    allowed = reason in ("within_budget", "point_reduced_for_budget") and bool(incomplete)
    return dict(schema_version=1, point_limit=chosen, previous_point_limit=previous_point_limit,
        original_point_limit=400, point_reduced=chosen < 400, point_ids=current["ids"],
        point_regions=current["regions"], targets=dict(TARGETS, point=chosen),
        by_family=by_family, used_bytes=int(used_bytes), max_bytes=int(max_bytes),
        free_bytes=free_bytes, effective_limit_bytes=int(effective_limit),
        forecast_bytes=total(by_family), unreduced_forecast_bytes=unreduced,
        can_collect=allowed, reason=reason, exhausted_families=exhausted,
        inputs=dict(decision_bytes_floor=DECISION_BYTES_FLOOR,
            future_byte_factor=FUTURE_BYTE_FACTOR, headroom_bytes=HEADROOM_BYTES,
            disk_free_floor_bytes=DISK_FREE_FLOOR_BYTES,
            formula="used + sum(ceil(remaining_attempts * max(mean_payload, mean_decisions * 17000) * 1.10)) + headroom",
            remaining_attempts="ceil(missing_eligible / observed_eligible_rate), or missing episode count"))


def tree_bytes(path):
    return sum(value.stat().st_size for value in Path(path).rglob("*") if value.is_file())


def task_plan(task, max_bytes=MAX_BYTES, previous_point_limit=None, free_bytes=None):
    """Load the current saved corpus and forecast its next phase, without writes."""
    task = Path(task)
    root = task / "recordings"
    summary = json.loads((root / "collection-metrics.json").read_text())
    state_path = root / "collection-state.json"
    if state_path.is_file():
        state = json.loads(state_path.read_text())
        rejected = {(family(value), value["id"]): value for value in
                    rejection_rows(summary) + rejection_rows(state)}
        summary["rejected_candidates"] = list(rejected.values())
    selections = {name: [json.loads(line) for line in
        (task / "recording-input" / f"{name}.jsonl").read_text().splitlines() if line.strip()]
        for name in TARGETS}
    prior = root / "collection-plan.json"
    if previous_point_limit is None:
        previous_point_limit = json.loads(prior.read_text()).get("point_limit", 400) if prior.is_file() else 400
    if free_bytes is None:
        free_bytes = shutil.disk_usage(root).free
    return plan(summary, selections, tree_bytes(root), max_bytes, previous_point_limit, free_bytes)
