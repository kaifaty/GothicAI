"""Hidden conditions follow only the facts and endings of their save branch."""

import asyncio
from contextlib import asynccontextmanager
from types import SimpleNamespace

import pytest

from gothic_ai.challenges import CLOSED, KINDS, Challenges, line_people
from gothic_ai.chronicle import Chronicle
from gothic_ai.memory import MemoryStore
from gothic_ai.questbook import Goal, Quest, Stage, Talk
from tests.fake_game import FakeGame, connected


@asynccontextmanager
async def world(kind):
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        chronicle = Chronicle(link, MemoryStore(":memory:"))

        def progress(quest):
            state = "open"
            for fact in chronicle.facts():
                for ending in CLOSED:
                    if fact.get("id") == quest and fact.get("fact") == f"story_quest_{ending}":
                        state = ending
            return SimpleNamespace(state=state)

        book = SimpleNamespace(chronicle=chronicle, progress=progress)
        observer = Challenges(chronicle, book)
        line = chronicle.record({"fact": "line", "quests": ["Q1", "Q2"]})
        challenge = chronicle.record({
            "fact": "challenge", "kind": kind, "line": line,
            "quests": ["Q1", "Q2"], "people": ["GIVER", "FRIEND"],
            "chapter": 1, "day": 1, "hour": 8, "minute": 0,
        })
        try:
            yield fake, chronicle, observer, book, challenge, line
        finally:
            observer.close()


def finish(chronicle, *, how="done", pay="none"):
    done = chronicle.record({"fact": "story_quest_done", "id": "Q1"})
    outcome = chronicle.record({
        "fact": "quest_outcome", "quest": "Q1", "npc": "GIVER", "how": how, "pay": pay,
    })
    closed = chronicle.record({"fact": "story_quest_closed", "id": "Q2"})
    dropped = chronicle.record({
        "fact": "quest_outcome", "quest": "Q2", "how": "dropped", "pay": "none",
    })
    return [done, outcome, closed, dropped]


@pytest.mark.parametrize("kind", KINDS)
async def test_met_only_after_all_quests_close_with_supporting_evidence(kind):
    async with world(kind) as (_, chronicle, _, _, challenge, line):
        support = []
        if kind == "secret_kept":
            support.append(chronicle.record({"fact": "lie", "quest": "Q1", "npc": "GIVER"}))
        elif kind == "mercy":
            support.append(chronicle.record({"fact": "surrender", "npc": "FRIEND", "status": "spared"}))
        # These facts do not concern the line or do not describe the hero's deed.
        for fact in [
            {"fact": "died", "npc": "OTHER", "killer": "PLAYER"},
            {"fact": "died", "npc": "FRIEND", "killer": "OTHER"},
            {"fact": "found_out", "quest": "OTHER"},
            {"fact": "caught", "quest": "OTHER"},
            {"fact": "quest_outcome", "quest": "OTHER", "how": "done", "pay": "full"},
        ]:
            chronicle.record(fact)
        await asyncio.sleep(0)
        assert not chronicle.facts("challenge_met")
        assert not chronicle.facts("challenge_failed")
        support.append(chronicle.record({"fact": "story_quest_done", "id": "Q1"}))
        support.append(chronicle.record({
            "fact": "quest_outcome", "quest": "Q1", "how": "done", "pay": "none",
        }))
        await asyncio.sleep(0)
        assert not chronicle.facts("challenge_met")
        assert not chronicle.facts("challenge_failed")
        support.append(chronicle.record({"fact": "story_quest_closed", "id": "Q2"}))
        support.append(chronicle.record({
            "fact": "quest_outcome", "quest": "Q2", "how": "dropped", "pay": "none",
        }))
        # A death after the closing fact cannot retroactively break no_blood.
        chronicle.record({"fact": "died", "npc": "FRIEND", "killer": "PLAYER"})
        await asyncio.sleep(0)
        assert chronicle.facts("challenge_met") == [{
            "fact": "challenge_met", "challenge": challenge, "kind": kind,
            "line": line, "people": ["GIVER", "FRIEND"], "evidence": sorted(support),
        }]
        assert not chronicle.facts("challenge_failed")
        chronicle.record({"fact": "surrender", "npc": "FRIEND", "status": "spared"})
        await asyncio.sleep(0)
        assert len(chronicle.facts("challenge_met")) == 1


@pytest.mark.parametrize(("kind", "violation"), [
    ("no_blood", {"fact": "died", "npc": "FRIEND", "killer": "PLAYER"}),
    ("no_reward", {"fact": "quest_outcome", "quest": "Q1", "how": "done", "pay": "full"}),
    ("secret_kept", {"fact": "found_out", "quest": "Q1", "npc": "GIVER"}),
    ("secret_kept", {"fact": "caught", "quest": "Q2", "npc": "FRIEND"}),
])
async def test_first_violation_fails_once_before_line_closes(kind, violation):
    async with world(kind) as (_, chronicle, _, _, challenge, _):
        first = chronicle.record(violation)
        chronicle.record(violation)
        await asyncio.sleep(0)
        assert chronicle.facts("challenge_failed") == [{
            "fact": "challenge_failed", "challenge": challenge, "kind": kind,
            "why": violation["fact"], "evidence": [first],
        }]
        finish(chronicle)
        await asyncio.sleep(0)
        assert len(chronicle.facts("challenge_failed")) == 1
        assert not chronicle.facts("challenge_met")


@pytest.mark.parametrize(("kind", "how", "why"), [
    ("no_reward", "dropped", "no_outcome"),
    ("secret_kept", "done", "no_lie"),
    ("mercy", "done", "no_mercy"),
])
async def test_closed_line_without_required_deed_fails(kind, how, why):
    async with world(kind) as (_, chronicle, _, _, challenge, _):
        chronicle.record({"fact": "lie", "quest": "OTHER", "npc": "GIVER"})
        chronicle.record({"fact": "surrender", "npc": "OTHER", "status": "spared"})
        chronicle.record({"fact": "surrender", "npc": "FRIEND", "status": "offered"})
        evidence = finish(chronicle, how=how)
        await asyncio.sleep(0)
        assert chronicle.facts("challenge_failed") == [{
            "fact": "challenge_failed", "challenge": challenge, "kind": kind,
            "why": why, "evidence": evidence,
        }]
        assert not chronicle.facts("challenge_met")


async def test_no_reward_needs_an_executed_outcome_even_when_line_is_closed():
    async with world("no_reward") as (_, chronicle, _, _, _, _):
        chronicle.record({"fact": "story_quest_failed", "id": "Q1"})
        chronicle.record({"fact": "story_quest_obsolete", "id": "Q2"})
        await asyncio.sleep(0)
        assert chronicle.facts("challenge_failed")[0]["why"] == "no_outcome"
        assert not chronicle.facts("challenge_met")


async def test_people_snapshot_includes_givers_characters_roles_and_all_goal_people():
    async with world("no_blood") as (_, chronicle, _, book, _, _):
        quest = Quest(
            "Q1", "QUEST_GIVER", "Help", Talk("Help?", []),
            [Stage("Help them.", [Goal("talk", npc="TALKER"), Goal("kill", npcs=["A", "B"])])],
            "Helped.",
        )
        book.all = lambda: {"Q1": quest}
        chronicle.record({"fact": "chain", "quests": ["Q1"], "roles": {"friend": ["ROLE"]}})
        chronicle.record({"fact": "chain", "quests": ["OTHER"], "roles": {"friend": ["UNRELATED"]}})
        assert line_people(book, ["Q1"], "LINE_GIVER", ["CHARACTER"]) == [
            "A", "B", "CHARACTER", "LINE_GIVER", "QUEST_GIVER", "ROLE", "TALKER",
        ]


async def test_loading_completed_branch_does_not_repeat_and_old_branch_can_still_fail():
    async with world("no_blood") as (fake, chronicle, _, _, challenge, _):
        finish(chronicle)
        await asyncio.sleep(0)
        met = chronicle.facts("challenge_met")
        head = chronicle.memory.head("A")
        await fake.send(type="session", branch="after", parent="A", parent_seq=head, reason="load")
        await chronicle.link.story_state()
        await asyncio.sleep(0)
        assert chronicle.facts("challenge_met") == met
        assert len(met) == 1
        await fake.send(type="session", branch="before", parent="A", parent_seq=challenge, reason="load")
        await chronicle.link.story_state()
        death = chronicle.record({"fact": "died", "npc": "FRIEND", "killer": "PLAYER"})
        await asyncio.sleep(0)
        assert not chronicle.facts("challenge_met")
        assert chronicle.facts("challenge_failed")[0]["evidence"] == [death]
