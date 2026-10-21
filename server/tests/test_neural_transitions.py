"""Acceptance must reject stale native ownership after local cleanup."""

from types import SimpleNamespace

import pytest

from gothic_ai.bodypilot import BodyPilot
from gothic_ai.game import GameLink
from gothic_ai.llm import Message, Tool, ToolCall
from gothic_ai.protocol import BodyLease, RosterEntry
from gothic_ai.scenarios.companions import _neural_talk_bandit
from gothic_ai.scenarios.neural_knockout import _hero_release_state


@pytest.fixture
def released_episode():
    link = GameLink(None)
    pilot = BodyPilot(link, "unused")
    lease = BodyLease(
        npc="NPC", provider="neural-body", mode="realtime", session_epoch=1, world_epoch=1,
        entity_id=1, control_generation=7, goal_version=1, target_entity_id=2,
        acquired_observation_seq=1,
    )
    summary = SimpleNamespace(lease=lease, release_confirmed=True, watch_removed=True)
    return link, pilot, summary


@pytest.mark.parametrize("foreign_observer", [False, True])
def test_body_roster_blocks_release_even_after_local_fight_and_lease_are_gone(
    released_episode, foreign_observer,
):
    link, pilot, summary = released_episode
    pilot.people["NPC"] = RosterEntry(id="NPC", name="NPC", distance=100, activity="body")
    if foreign_observer:
        pilot.fights["NPC"] = SimpleNamespace(_target_kind=None, lease=None)
        pilot._watch_npcs.append("NPC")

    clean, detail = _hero_release_state(link, pilot, "NPC", summary)

    assert not clean
    assert "hero_body=1" in detail
    assert not link.body_leases and not link.body_latest


@pytest.mark.parametrize("activity", [None, "routine", "fight"])
def test_native_roster_or_out_of_range_passes_confirmed_release(released_episode, activity):
    link, pilot, summary = released_episode
    if activity is not None:
        pilot.people["NPC"] = RosterEntry(id="NPC", name="NPC", distance=100, activity=activity)

    clean, detail = _hero_release_state(link, pilot, "NPC", summary)

    assert clean
    assert "hero_body=0" in detail


def test_foreign_observer_and_another_npcs_body_do_not_fail_hero_release(released_episode):
    link, pilot, summary = released_episode
    pilot.people["NPC"] = RosterEntry(id="NPC", name="NPC", distance=100, activity="fight")
    pilot.people["OTHER"] = RosterEntry(id="OTHER", name="Other", distance=100, activity="body")
    pilot.fights["NPC"] = SimpleNamespace(_target_kind=None, lease=None)
    pilot._watch_npcs.append("NPC")

    clean, detail = _hero_release_state(link, pilot, "NPC", summary)

    assert clean
    assert "ignored_observers=1" in detail


@pytest.mark.parametrize("unconfirmed", ["release_confirmed", "watch_removed"])
def test_empty_local_state_requires_confirmed_release_and_unwatch(released_episode, unconfirmed):
    link, pilot, summary = released_episode
    setattr(summary, unconfirmed, False)

    assert not _hero_release_state(link, pilot, "NPC", summary)[0]


@pytest.mark.parametrize(("cue", "expected"), [
    ("Бандит, проверим твою шпагу.", "ultimatum"),
    ("[Срок вышел: герой не подчинился]", "set_intent"),
    ("Бандит, я вернулся. Поговорим?", None),
])
async def test_neural_talk_bandit_waits_for_ultimatum_before_fighting(cue, expected):
    attack = ToolCall("set_intent", {"goal": "fight", "target": "PLAYER"})
    model = _neural_talk_bandit(attack)
    events = [event async for event in model.stream(
        instructions="You are a bandit", messages=[Message("user", cue)],
        tools=[Tool("ultimatum", ""), Tool("set_intent", "")],
    )]
    calls = [event for event in events if isinstance(event, ToolCall)]

    assert [call.name for call in calls] == ([expected] if expected else [])
    if expected == "ultimatum":
        assert calls[0].arguments == {"demand": "Уходи отсюда", "seconds": 10}
    if expected == "set_intent":
        assert calls[0].arguments == attack.arguments
