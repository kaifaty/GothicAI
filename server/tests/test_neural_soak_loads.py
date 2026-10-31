"""Death cleanup may race a load; scheduled combat coverage stays independent."""

from dataclasses import dataclass, replace
from types import SimpleNamespace

import pytest

from gothic_ai.protocol import BodyLease
from gothic_ai.scenarios.neural_soak import (
    _has_scheduled_combat_load,
    _load_cleanup_details,
    _load_summaries_ok,
    _scheduled_load_due,
)


def lease(entity_id):
    return BodyLease(
        npc=f"BANDIT_{entity_id}", provider="neural-body", mode="realtime", session_epoch=1,
        world_epoch=1, entity_id=entity_id, control_generation=1, goal_version=1,
        target_entity_id=10, acquired_observation_seq=1,
    )


@dataclass(frozen=True)
class Summary:
    lease: BodyLease
    reason: str = "loading"
    target_kind: str | None = "hero"
    release_confirmed: bool = True
    fallbacks: int = 0
    release_seconds: float | None = 0.1
    release_late_seconds: float | None = None
    native_release_reason: str | None = None


def test_hero_death_accepts_cleanup_published_between_snapshot_and_load():
    old = [lease(1), lease(2)]
    # The first old fight notices the dead hero before Loading reaches it;
    # the second still observes Loading. Both release the snapshotted leases.
    summaries = [Summary(old[0], reason="hero_down"), Summary(old[1])]

    assert _load_summaries_ok(old, summaries, cause="hero_death")
    assert not _load_summaries_ok(old, summaries, cause="scheduled_10m")


def test_companion_death_accepts_cleanup_published_between_snapshot_and_load():
    old = [lease(1), lease(2)]
    summaries = [Summary(old[0], reason="target_down", target_kind="companion"), Summary(old[1])]

    assert _load_summaries_ok(old, summaries, cause="companion_death")
    assert not _load_summaries_ok(old, summaries, cause="hero_death")
    assert not _load_summaries_ok(old, summaries, cause="scheduled_10m")


@pytest.mark.parametrize("cause,target_kind", [
    ("hero_death", "hero"), ("companion_death", "companion"),
])
def test_death_accepts_native_return_before_target_death_reaches_pilot(cause, target_kind):
    old = lease(1)
    summary = Summary(old, reason="native_restored", target_kind=target_kind,
                      native_release_reason="target_changed_or_lost", release_seconds=.034)
    assert _load_summaries_ok([old], [summary], cause=cause)
    assert not _load_summaries_ok([old], [summary], cause="scheduled_10m")
    other_death = "companion_death" if target_kind == "hero" else "hero_death"
    assert not _load_summaries_ok([old], [summary], cause=other_death)
    assert _load_summaries_ok([old], [summary], cause=other_death, dead_targets={target_kind})


@pytest.mark.parametrize("fields", [
    {"native_release_reason": None},
    {"native_release_reason": "native_script_takeover"},
    {"native_release_reason": "target_visibility_lost"},
    {"target_kind": None},
    {"target_kind": "other"},
    {"target_kind": "companion"},
    {"release_confirmed": False},
    {"release_seconds": None},
    {"release_seconds": 2.001},
    {"release_late_seconds": 2.75},
    {"fallbacks": 1},
])
def test_death_native_return_keeps_reason_target_and_release_guards(fields):
    old = lease(1)
    summary = Summary(old, reason="native_restored", native_release_reason="target_changed_or_lost")
    assert not _load_summaries_ok([old], [replace(summary, **fields)], cause="hero_death")


@pytest.mark.parametrize("cause", ["hero_death", "companion_death"])
def test_simultaneous_deaths_allow_only_known_dead_targets(cause):
    old = [lease(1), lease(2)]
    summaries = [Summary(old[0], reason="hero_down", target_kind="hero"),
                 Summary(old[1], reason="target_down", target_kind="companion")]
    assert _load_summaries_ok(old, summaries, cause=cause, dead_targets={"hero", "companion"})
    assert not _load_summaries_ok(old, summaries, cause=cause)
    assert not _load_summaries_ok(
        old, summaries, cause="scheduled_10m", dead_targets={"hero", "companion"},
    )


@pytest.mark.parametrize("target_kind", ["hero", "other", None])
def test_companion_death_does_not_exempt_another_targets_summary(target_kind):
    old = lease(1)
    summary = Summary(old, reason="target_down", target_kind=target_kind)
    assert not _load_summaries_ok([old], [summary], cause="companion_death")


@pytest.mark.parametrize("reason", ["target_down", "target_lost", "native_restored", "pilot_error"])
def test_hero_death_does_not_accept_unrelated_endings(reason):
    old = lease(1)
    assert not _load_summaries_ok([old], [Summary(old, reason=reason)], cause="hero_death")


@pytest.mark.parametrize("target_kind", ["companion", "other", None])
def test_hero_death_does_not_exempt_another_targets_summary(target_kind):
    old = lease(1)
    summary = Summary(old, reason="hero_down", target_kind=target_kind)
    assert not _load_summaries_ok([old], [summary], cause="hero_death")


@pytest.mark.parametrize("cause, reason, target_kind", [
    ("hero_death", "hero_down", "hero"),
    ("companion_death", "target_down", "companion"),
])
@pytest.mark.parametrize("fields", [
    {"release_confirmed": False},
    {"fallbacks": 1},
    {"release_seconds": None},
    {"release_seconds": 2.001},
    {"release_late_seconds": 2.75},
])
def test_death_race_keeps_release_and_fallback_checks(fields, cause, reason, target_kind):
    old = lease(1)
    summary = replace(Summary(old, reason=reason, target_kind=target_kind), **fields)
    assert not _load_summaries_ok([old], [summary], cause=cause)


def test_load_cleanup_requires_every_exact_old_lease_once():
    first, second = lease(1), lease(2)
    assert not _load_summaries_ok([first, second], [Summary(first)], cause="hero_death")
    assert not _load_summaries_ok(
        [first, second], [Summary(first), Summary(first)], cause="hero_death",
    )
    changed = second.model_copy(update={"control_generation": 2})
    assert not _load_summaries_ok(
        [first, second], [Summary(first), Summary(changed)], cause="hero_death",
    )
    assert _load_summaries_ok(
        [first, second], [Summary(first), Summary(second, release_seconds=2)], cause="scheduled_20m",
    )


def test_deaths_before_both_markers_cannot_spend_planned_loads_in_gaps():
    loads, scheduled = [], set()
    # Each wave ends just before its checkpoint; post-death reacquisition
    # succeeds, but a death load still does not satisfy scheduled combat.
    for elapsed in (600, 1200):
        loads.append({"cause": "hero_death", "mid_fight": True, "ok": True, "reacquired": True})
        marker = _scheduled_load_due(elapsed, 1800, scheduled, loads, combat=False)
        assert marker is None
        assert not _has_scheduled_combat_load(loads)

    # The overdue first marker remains available in the next actual fight.
    assert _scheduled_load_due(1201, 1800, scheduled, loads, combat=True) == 10
    loads.append({"cause": "scheduled_10m", "mid_fight": True, "ok": True, "reacquired": True})
    scheduled.add(10)
    assert _has_scheduled_combat_load(loads)
    assert _scheduled_load_due(1202, 1800, scheduled, loads, combat=False) == 20


def test_only_a_planned_combat_load_opens_the_peaceful_gap():
    scheduled = {10}
    loads = [{"cause": "scheduled_10m", "mid_fight": False}]
    assert _scheduled_load_due(1200, 1800, scheduled, loads, combat=False) is None
    loads[0]["mid_fight"] = True
    assert _scheduled_load_due(1199, 1800, scheduled, loads, combat=False) is None
    assert _scheduled_load_due(1200, 1800, scheduled, loads, combat=False) == 20
    scheduled.add(20)
    assert _scheduled_load_due(1500, 1800, scheduled, loads, combat=True) is None


def test_short_run_markers_keep_their_scaled_timing():
    assert _scheduled_load_due(59.9, 180, set(), [], combat=True) is None
    assert _scheduled_load_due(60, 180, set(), [], combat=True) == 10
    loads = [{"cause": "scheduled_10m", "mid_fight": True}]
    assert _scheduled_load_due(120, 180, {10}, loads, combat=False) == 20


def observation(*, world_epoch=2, control_generation=0, control_allowed=False):
    fields = dict(session_epoch=1, world_epoch=world_epoch, entity_id=1,
                  control_generation=control_generation, control_allowed=control_allowed)
    return SimpleNamespace(**fields, model_dump=lambda: fields.copy())


def observing_fight(npc="TALKER"):
    return SimpleNamespace(npc=npc, lease=None, _acquiring=False, _pending_acquire=None,
                           _reason=None, _rows=[])


@pytest.fixture
def loaded_world():
    link = SimpleNamespace(body_leases={}, body_latest={},
                           _body_acquisitions={}, _body_acquire_waiters={})
    pilot = SimpleNamespace(fights={}, people={}, _watch_npcs=[], _pending_unwatch=set(),
                            _releasing={})
    status = SimpleNamespace(actors=[])
    return link, pilot, status


def cleanup(loaded_world, old_fights=None):
    link, pilot, status = loaded_world
    return _load_cleanup_details(link, pilot, old_fights or {},
                                 old_worlds={(1, 1)}, new_world=(1, 2), status=status)


@pytest.mark.parametrize("old_owned", [False, True])
def test_load_allows_only_a_new_passive_observer_even_when_npc_symbol_is_reused(
    loaded_world, old_owned,
):
    link, pilot, _ = loaded_world
    old = observing_fight()
    if old_owned:
        old.lease = lease(1)
    pilot.fights["TALKER"] = observing_fight()
    pilot._watch_npcs = ["TALKER"]
    link.body_latest[1] = observation()
    result = cleanup(loaded_world, {"TALKER": old})
    assert all(result["checks"].values())
    assert result["observing_fights"] == ["TALKER"]
    assert not any(result["residuals"].values())


def test_load_rejects_surviving_old_lease_less_observer(loaded_world):
    link, pilot, _ = loaded_world
    old = observing_fight()
    pilot.fights["TALKER"] = old
    pilot._watch_npcs = ["TALKER"]
    link.body_latest[1] = observation()
    result = cleanup(loaded_world, {"TALKER": old})
    assert not result["checks"]["old_fights"]
    assert not result["checks"]["unsafe_watch"]
    assert result["residuals"]["old_fights"] == ["TALKER"]


@pytest.mark.parametrize("reason", ["loading", "session_changed", "world_changed", "disconnected"])
def test_load_rejects_old_observer_created_after_the_initial_snapshot(loaded_world, reason):
    _, pilot, _ = loaded_world
    fight = observing_fight()
    fight._reason = reason
    pilot.fights["TALKER"] = fight
    result = cleanup(loaded_world)
    assert not result["checks"]["stopped_fights"]
    assert result["residuals"]["stopped_fights"] == [{"npc": "TALKER", "reason": reason}]
    assert not result["observing_fights"]


@pytest.mark.parametrize("fields", [
    {"world_epoch": 1}, {"world_epoch": 3},
    {"control_generation": 1}, {"control_allowed": True},
])
@pytest.mark.parametrize("source", ["cached_rows", "queued_rows"])
def test_load_rejects_old_or_owned_rows_in_cache_and_fight_queue(loaded_world, fields, source):
    link, pilot, _ = loaded_world
    row = observation(**fields)
    if source == "cached_rows":
        link.body_latest[1] = row
    else:
        fight = observing_fight()
        fight._rows.append((row.model_dump(), 1.0))
        pilot.fights["TALKER"] = fight
    result = cleanup(loaded_world)
    assert not result["checks"][source]
    assert result["residuals"][source][0]["world_epoch"] == row.world_epoch


@pytest.mark.parametrize("world_epoch", [1, 2])
@pytest.mark.parametrize("source", ["leases", "fight_leases"])
def test_load_rejects_any_old_or_fresh_owned_lease(loaded_world, world_epoch, source):
    link, pilot, _ = loaded_world
    owned = lease(1).model_copy(update={"world_epoch": world_epoch})
    if source == "leases":
        link.body_leases[1] = owned
    else:
        fight = observing_fight()
        fight.lease = owned
        pilot.fights["TALKER"] = fight
    result = cleanup(loaded_world)
    assert not result["checks"][source]
    assert result["residuals"][source][0]["world_epoch"] == world_epoch


@pytest.mark.parametrize("source", ["pending_fights", "_body_acquisitions", "_body_acquire_waiters"])
@pytest.mark.parametrize("world_epoch", [1, 2])
def test_load_rejects_pending_old_or_fresh_acquisition(loaded_world, source, world_epoch):
    link, pilot, _ = loaded_world
    pending = observation(world_epoch=world_epoch)
    if source == "pending_fights":
        fight = observing_fight()
        fight._pending_acquire = pending
        pilot.fights["TALKER"] = fight
    else:
        getattr(link, source)[1] = SimpleNamespace(npc="TALKER", cmd=pending)
    check = source if source == "pending_fights" else "pending_link"
    result = cleanup(loaded_world)
    assert not result["checks"][check]
    assert result["residuals"][check][0]["world_epoch"] == world_epoch


@pytest.mark.parametrize("source", ["pending_unwatch", "unsafe_watch", "body_without_lease",
                                    "pending_releases", "pending_fights"])
def test_load_rejects_unfinished_cleanup_without_a_registered_lease(loaded_world, source):
    _, pilot, _ = loaded_world
    if source == "pending_unwatch":
        pilot._pending_unwatch.add("TALKER")
    elif source == "unsafe_watch":
        pilot._watch_npcs = ["TALKER"]
    elif source == "body_without_lease":
        pilot.people["TALKER"] = SimpleNamespace(activity="body")
    elif source == "pending_releases":
        pilot._releasing["TALKER"] = SimpleNamespace(lease=lease(1))
    else:
        fight = observing_fight()
        fight._acquiring = True
        pilot.fights["TALKER"] = fight
    result = cleanup(loaded_world)
    assert not result["checks"][source]
    assert result["residuals"][source]


@pytest.mark.parametrize("fields", [
    {"owner": "scripted"}, {"release_pending": True},
    {"acquire_pending": True}, {"acquire_request_id": 7},
])
def test_load_checks_authoritative_status_for_untracked_ownership(loaded_world, fields):
    _, _, status = loaded_world
    actor = dict(session_epoch=1, world_epoch=2, entity_id=1, control_generation=0,
                 owner="native", release_pending=False, acquire_pending=False, acquire_request_id=0)
    status.actors = [SimpleNamespace(**(actor | fields))]
    result = cleanup(loaded_world)
    assert not result["checks"]["owned_status"]
    assert result["residuals"]["owned_status"][0]["entity_id"] == 1
