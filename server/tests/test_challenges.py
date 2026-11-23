"""Hidden conditions follow only the facts and endings of their save branch."""

import asyncio
from contextlib import asynccontextmanager
from types import SimpleNamespace

import pytest

from gothic_ai import challenges as challenges_module
from gothic_ai.challenges import CLOSED, KINDS, Challenges, line_people
from gothic_ai.chronicle import Chronicle
from gothic_ai.game import GameError
from gothic_ai.memory import MemoryStore
from gothic_ai.questbook import Goal, Quest, Stage, Talk
from gothic_ai.social import Social, _chance
from tests.fake_game import FakeGame, connected


@asynccontextmanager
async def world(kind, *, social=None, people=("GIVER", "FRIEND")):
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

        async def acquaintances():
            return social if social is not None else Social([])

        observer = Challenges(chronicle, book, social=acquaintances if social is not None else None)
        line = chronicle.record({"fact": "line", "giver": "GIVER", "quests": ["Q1", "Q2"]})
        challenge = chronicle.record({
            "fact": "challenge", "kind": kind, "line": line,
            "quests": ["Q1", "Q2"], "people": list(people),
            "chapter": 1, "day": 1, "hour": 8, "minute": 0,
        })
        await asyncio.sleep(0)
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


async def test_hint_share_is_deterministic_for_the_challenge_sequence():
    runs = []
    for _ in range(2):
        async with world("no_blood", social=Social([])) as (_, chronicle, observer, _, first, _):
            challenge = chronicle.facts("challenge")[0]
            seqs = [first, *(chronicle.record(challenge) for _ in range(99))]
            await asyncio.sleep(0)
            hinted = {f["challenge"] for f in chronicle.facts("challenge_hint")}
            assert hinted == {seq for seq in seqs if _chance("challenge_hint", str(seq)) < 0.5}
            assert 35 <= len(hinted) <= 65
            before = chronicle.facts("challenge_hint")
            for seq in seqs:
                observer._on_entry(seq, challenge)
            observer._check()
            await asyncio.sleep(0)
            assert chronicle.facts("challenge_hint") == before
            runs.append((seqs, hinted))
    assert runs[0] == runs[1]


@pytest.mark.parametrize(("people", "acquaintances", "expected"), [
    (["ZED", "PLAYER", "GIVER", "ANN", "BEA"], ["ZED", "BEA"], "BEA"),
    (["ZED", "PLAYER", "GIVER", "ANN"], [], "ANN"),
    (["GIVER", "PLAYER"], [], None),
    ([], [], None),
])
async def test_hint_source_and_branch_reload(monkeypatch, people, acquaintances, expected):
    monkeypatch.setattr(challenges_module, "_chance", lambda *parts: 0.0)
    social = Social([{"npc": npc, "name": f"Name {npc}"} for npc in ["GIVER", "ANN", "BEA", "ZED"]])
    # An empty Tie is not an acquaintance; Social.tie also creates ties for unrelated people.
    social.tie("GIVER", "ANN")
    for npc in acquaintances:
        social.tie("GIVER", npc).bonds.append("neighbors")
    async with world("no_blood", social=social, people=people) as (
        fake, chronicle, observer, _, seq, _,
    ):
        hints = chronicle.facts("challenge_hint")
        if expected is None:
            assert hints == []
        else:
            assert len(hints) == 1
            assert hints[0] == {
                "fact": "challenge_hint", "id": hints[0]["id"], "challenge": seq, "kind": "no_blood",
                "npc": "GIVER", "name": "Name GIVER", "witnesses": [expected],
                "witness_names": {expected: f"Name {expected}"},
                "chapter": 1, "day": 1, "hour": 8, "minute": 0,
            }
            assert hints[0]["id"]
        head = chronicle.memory.head("A")
        await fake.send(type="session", branch="loaded", parent="A", parent_seq=head, reason="load")
        await chronicle.link.story_state()
        observer._on_entry(seq, chronicle.facts("challenge")[0])
        observer._check()
        await asyncio.sleep(0)
        assert chronicle.facts("challenge_hint") == hints


async def test_hint_waits_for_social_refresh_and_is_cancelled_by_loading(monkeypatch):
    monkeypatch.setattr(challenges_module, "_chance", lambda *parts: 0.0)
    social = Social([{"npc": npc, "name": f"Name {npc}"} for npc in ["GIVER", "ANN", "ZED"]])
    social.tie("GIVER", "ZED").bonds.append("neighbors")
    async with world("no_blood", people=[]) as (fake, chronicle, observer, _, _, line):
        ready = asyncio.Event()

        async def refresh():
            await ready.wait()
            return social

        observer.social = refresh
        challenge = {**chronicle.facts("challenge")[0], "people": ["GIVER", "ANN", "ZED"]}
        seq = chronicle.record(challenge)
        await asyncio.sleep(0)
        assert not chronicle.facts("challenge_hint")
        ready.set()
        await asyncio.sleep(0)
        assert chronicle.facts("challenge_hint")[0]["witnesses"] == ["ZED"]
        assert chronicle.facts("challenge_hint")[0]["challenge"] == seq
        ready.clear()
        chronicle.record(challenge)
        await asyncio.sleep(0)
        await fake.send(type="session", branch="before", parent="A", parent_seq=line, reason="load")
        await chronicle.link.story_state()
        ready.set()
        await asyncio.sleep(0)
        assert not chronicle.facts("challenge_hint")


@pytest.mark.parametrize("error", [GameError, TimeoutError])
async def test_hint_survives_social_failure_with_an_alphabetical_source(monkeypatch, caplog, error):
    monkeypatch.setattr(challenges_module, "_chance", lambda *parts: 0.0)
    async with world("no_blood") as (_, chronicle, observer, _, _, _):
        async def unavailable():
            raise error("social unavailable")

        observer.social = unavailable
        with caplog.at_level("INFO", logger="gothic_ai.challenges"):
            seq = chronicle.record({
                **chronicle.facts("challenge")[0], "people": ["ZED", "PLAYER", "GIVER", "ANN"],
            })
            await asyncio.sleep(0)
        [hint] = chronicle.facts("challenge_hint")
        assert hint["challenge"] == seq and hint["witnesses"] == ["ANN"]
        assert hint["witness_names"] == {"ANN": "ANN"}
        assert "social unavailable" in caplog.text


async def test_challenges_without_social_write_no_hints_but_still_finish(monkeypatch):
    monkeypatch.setattr(challenges_module, "_chance", lambda *parts: 0.0)
    async with world("no_blood") as (_, chronicle, observer, _, seq, _):
        assert observer.social is None
        finish(chronicle)
        await asyncio.sleep(0)
        assert not chronicle.facts("challenge_hint")
        assert chronicle.facts("challenge_met")[0]["challenge"] == seq
