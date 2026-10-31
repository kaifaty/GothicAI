"""A witness gives its own evidence about a named NPC, without the hero's cover paths."""

import pytest

from gothic_ai.llm import Message, ToolCall
from gothic_ai.sightings import Sighting
from gothic_ai.storyguard import StoryGuard
from gothic_ai.witnesses import HERO, Inquiry, Witnesses
from gothic_ai.witnesses import Testimony as WitnessTestimony
from tests.fake_game import FakeGame, connected
from tests.test_cover import CoverWorld
from tests.test_perception import HILDA, LOBART, MALETH
from tests.test_witnesses import ANDRE, AT_8, DAY, World, lobart

SAID = "Это Малет взял кубок"


def npc_theft(world, *, id="N1", culprit=MALETH, minute=AT_8, seen_by=HILDA):
    world.chronicle.record({
        "fact": "npc_crime", "id": id, "kind": "theft", "culprit": culprit,
        "owner": LOBART, "victim": LOBART, "item": "ITMI_GOLD_CUP", "item_name": "кубок",
        "count": 1, "chest": "CHEST_LOBART", "waypoint": "NW_FARM1_PATH_01",
        "day": minute // DAY, "hour": minute % DAY // 60, "minute": minute % 60,
    })
    if seen_by:
        world.chronicle.record({
            "fact": "npc_crime_seen", "npc": seen_by, "crime": id,
            "culprit_name": "Малет", "victim_name": "Лобарт", "name": "Хильда",
        })


def sight(world, minute, *, who=MALETH, known=True):
    s = Sighting(who, "Малет", minute, minute + 5, ["у твоего сундука"],
                 place_ids=["FARM1_PASTURE"], known=known, notable=["weapon"] if not known else [])
    world.seen.mind(HILDA).sightings.append(s)
    return s


async def _testify(world, *, witness=HILDA, name="Хильда", at=0, about=MALETH):
    t = await world.witnesses._testify(
        None, LOBART, "Лобарт", witness, name, SAID, "far", at, about, "Малет",
    )
    assert t is not None
    return t


async def test_npc_deeds_are_truthful_even_when_the_witness_covers_or_fears_the_hero():
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        w = CoverWorld(link, fake, courage=1)
        w.violence()
        w.covered(day=1, vouch="Его тут не было")
        npc_theft(w)
        npc_theft(w, id="unseen", seen_by=None)
        npc_theft(w, id="other", culprit=ANDRE)
        t = await _testify(w)
        assert t.crime_saw == t.crime_told == ["N1"]
        assert not t.cover and not t.vouch
        fact = w.chronicle.facts("testimony")[0]
        assert (fact["about"], fact["about_name"], fact["at"]) == (MALETH, "Малет", w.witnesses.now)
        situation = w.witnesses.situation(LOBART) or ""
        assert "С1: Хильда" in situation and "Малет взял кубок" in situation and "дело N1" in situation
        assert "герой взял" not in situation
        assert w.witnesses.told(LOBART, "С1") is None
        assert not w.witnesses.any(LOBART)
        assert w.witnesses.concerns(LOBART) == []


@pytest.mark.parametrize("known_by", ["suspicion", "rumor", "hero_case"])
async def test_npc_sightings_use_the_known_cases_hour_window(known_by):
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        w = CoverWorld(link, fake)
        npc_theft(w)
        if known_by == "suspicion":
            w.chronicle.record({"fact": "npc_crime_suspected", "npc": LOBART,
                                "crime": "N1", "suspect": MALETH})
        elif known_by == "rumor":
            w.chronicle.record({
                "fact": "rumor", "kind": "npc_theft", "quest": "npc_crime_seen:N1",
                "subject": MALETH, "subject_name": "Малет", "to": LOBART,
                "from": HILDA, "from_name": "Хильда", "about": LOBART, "about_name": "Лобарт",
                "hops": 1, "day": 1,
            })
        else:
            w.chronicle.record({
                "fact": "crime", "id": "H1", "kind": "theft", "victim": LOBART,
                "item": "ITMI_GOLD_CUP", "day": 1, "hour": 8, "minute": 10,
            })
            w.chronicle.record({"fact": "crime_suspected", "npc": LOBART,
                                "crime": "H1", "suspect": "PLAYER"})
        too_early = sight(w, AT_8 - 70)
        before = sight(w, AT_8 - 55)
        after = sight(w, AT_8 + 55, known=False)
        too_late = sight(w, AT_8 + 65)
        other = sight(w, AT_8, who=HILDA)
        t = await _testify(w)
        assert t.saw == t.told == [after, before]
        assert all(s not in t.told for s in [too_early, too_late, other])
        assert t.crime_told == ["N1"]
        assert "Малет —" in (w.witnesses.situation(LOBART) or "")


async def test_without_a_known_case_the_witness_tells_the_previous_day_including_recent_sightings():
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        w = World(link, fake, lobart())
        at = DAY + 9 * 60 + 35
        sight(w, at - DAY - 10)
        yesterday = sight(w, at - DAY + 1)
        recent = sight(w, at - 1, known=False)
        sight(w, at + 1)
        t = await _testify(w, at=at)
        assert t.told == [recent, yesterday]
        assert not t.crime_told


async def test_no_evidence_names_the_subject_and_hero_testimony_stays_separate():
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        w = World(link, fake, lobart())
        t = await _testify(w)
        assert t.told == t.saw == [] and t.crime_told == []
        assert "Малет тогда не видел" in (w.witnesses.situation(LOBART) or "")
        assert w.witnesses._asked(LOBART, HILDA, SAID, MALETH).id == t.id
        assert w.witnesses._asked(LOBART, HILDA, SAID) is None


@pytest.mark.parametrize("about", ["PLAYER", "героя", "незнакомого", ""])
async def test_about_rejects_the_hero_or_an_unknown_subject(about):
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        w = World(link, fake, lobart())
        conv = w.dialogue.open(LOBART, "Лобарт")
        conv.trigger = Message("user", SAID)
        await w.witnesses.prepare(LOBART)
        out = await w.witnesses.handle(w.dialogue, conv, ToolCall(
            "ask_witness", {"who": "Хильду", "said": SAID, "about": about}, "ask"), None)
        assert out is not None and out.status == "rejected"
        assert not w.chronicle.facts("testimony")


async def test_far_npc_question_and_answer_survive_reload_without_repeating_the_fact():
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        w = World(link, fake, lobart())
        npc_theft(w, seen_by=ANDRE)
        conv = w.dialogue.open(LOBART, "Лобарт")
        conv.trigger = Message("user", SAID)
        await w.witnesses.prepare(LOBART)
        out = await w.witnesses.handle(w.dialogue, conv, ToolCall(
            "ask_witness", {"who": "Андре", "said": SAID, "about": "Малета"}, "ask"), None)
        assert out is not None and out.status == "accepted"
        again = Witnesses(link, w.dialogue, w.seen, w.desk, w.chronicle, StoryGuard(None))
        question = next(iter(again.inquiries.values()))
        assert (question.about, question.about_name) == (MALETH, "Малет")
        fake.hour, fake.minute = 18, 5
        await again.tick()
        again._load()
        await again.tick()
        assert not again.inquiries
        facts = w.chronicle.facts("testimony")
        assert len(facts) == 1 and facts[0]["id"] == question.id
        t = again.testimonies(LOBART)[0]
        assert (t.about, t.at, t.crime_told) == (MALETH, question.minute, ["N1"])


@pytest.mark.parametrize("about", ["Малет", None])
async def test_disabled_npc_crimes_removes_about_and_refuses_crafted_calls_and_old_npc_questions(about):
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        w = World(link, fake, lobart())
        w.witnesses._inquire(Inquiry("Q1", LOBART, "Лобарт", ANDRE, "Андре", SAID,
                                    DAY + 9 * 60, DAY + 18 * 60, MALETH, "Малет"))
        disabled = Witnesses(link, w.dialogue, w.seen, w.desk, w.chronicle,
                             StoryGuard(None), npc_crimes=False)
        assert not disabled.inquiries
        disabled._near[LOBART] = w.witnesses._near[LOBART] = []
        assert "about" not in disabled.all_tools[0].parameters["properties"]
        conv = w.dialogue.open(LOBART, "Лобарт")
        conv.trigger = Message("user", SAID)
        out = await disabled.handle(w.dialogue, conv, ToolCall(
            "ask_witness", {"who": "Хильду", "said": SAID, "about": about}, "ask"), None)
        assert out is not None and out.status == "rejected" and "выключены" in out.output
        assert await disabled._testify(None, LOBART, "Лобарт", HILDA, "Хильда", SAID,
                                       "far", 0, MALETH, "Малет") is None
        assert not w.chronicle.facts("testimony")


def test_legacy_testimony_timestamp_and_new_subject_are_read_without_confusion():
    old = WitnessTestimony.of({"fact": "testimony", "about": AT_8, "minute": AT_8 + 60})
    new = WitnessTestimony.of({"fact": "testimony", "about": MALETH, "about_name": "Малет",
                        "at": AT_8, "minute": AT_8 + 60})
    assert (old.about, old.at) == (HERO, AT_8)
    assert (new.about, new.at) == (MALETH, AT_8)


async def test_nullable_about_uses_the_hero_route_for_strict_model_tool_calls():
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        w = World(link, fake, lobart())
        conv = w.dialogue.open(LOBART, "Лобарт")
        conv.trigger = Message("user", "Я всё утро был в городе")
        await w.witnesses.prepare(LOBART)
        tool = w.witnesses.tools(LOBART)[0]
        assert set(tool.parameters["required"]) == set(tool.parameters["properties"])
        assert "null" in tool.parameters["properties"]["about"]["type"]
        out = await w.witnesses.handle(w.dialogue, conv, ToolCall(
            "ask_witness", {"who": "Андре", "said": "всё утро был в городе", "about": None}, "ask"), None)
        assert out is not None and out.status == "accepted"
        question = next(iter(w.witnesses.inquiries.values()))
        assert question.about == HERO and question.about_name == ""
        fact = w.chronicle.facts("inquiry")[0]
        assert "about" not in fact and "about_name" not in fact
