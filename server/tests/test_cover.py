"""Witness cover: the Chronicle keeps truth while an asker receives the witness's version."""

from typing import Any

import pytest

from gothic_ai.characters import Card
from gothic_ai.chronicle import Chronicle
from gothic_ai.crimes import Crimes
from gothic_ai.dialogue import DialogueManager
from gothic_ai.llm import Message, ToolCall
from gothic_ai.llm.fake import FakeStep
from gothic_ai.memory import MemoryStore
from gothic_ai.perception import PerceptionDesk
from gothic_ai.persona import PersonaProvider
from gothic_ai.sightings import Sighting, Sightings
from gothic_ai.storyguard import StoryGuard
from gothic_ai.witnesses import Witnesses
from tests.fake_game import FakeGame, connected
from tests.test_dialogue import _until
from tests.test_perception import HILDA, LOBART, MALETH
from tests.test_sightings import BOOK, record_theft
from tests.test_witnesses import (
    ANDRE,
    ASK_HILDA,
    AT_8,
    CATCH,
    LIE,
    Social,
    hero_seen_by,
    hildas_view,
    lobart,
    lobarts_view,
)

SAID = "всё утро был в городе"
VOUCH = "Он всё утро был со мной в поле."


class CoverWorld:
    def __init__(self, link, fake, llm=None, *, courage: int | None = None, cover: bool = True):
        fake.day, fake.hour, fake.minute = 1, 9, 35
        fake.npc_states = {
            HILDA: {"state": "alive", "name": "Хильда", "distance": 900},
            MALETH: {"state": "alive", "name": "Малет", "distance": 4000},
            ANDRE: {"state": "alive", "name": "Андре", "distance": 90000},
        }
        fake.views = {LOBART: lobarts_view(), HILDA: hildas_view()}
        self.store = MemoryStore(":memory:")
        self.dialogue = DialogueManager(link, llm or lobart(), PersonaProvider(None), memory=self.store)
        self.chronicle = Chronicle(link, self.store)
        social = Social()
        self.desk = PerceptionDesk(link, social=lambda: social, places=BOOK)  # type: ignore[arg-type,return-value]
        self.crimes = Crimes(link, self.chronicle, places=BOOK, cover=cover)
        self.seen = Sightings(link, self.store, self.desk, self.chronicle, crimes=self.crimes)

        async def take(npc: str, why: str) -> bool:
            await link.attach(npc)
            return True

        self.dialogue.take_body = take
        self.dialogue.free_body = lambda npc, why: None
        cards = {} if courage is None else {HILDA: Card(HILDA, "Хильда", "", "", 3, 3, courage, 3, 3, 3)}
        self.witnesses = Witnesses(
            link,
            self.dialogue,
            self.seen,
            self.desk,
            self.chronicle,
            StoryGuard(None),
            social=lambda: social,  # type: ignore[arg-type,return-value]
            hero_cm=lambda npc: 500.0,
            crimes=self.crimes,
            cards=cards,
            cover=cover,
        )
        self.dialogue.extensions += [self.desk, self.seen, self.witnesses]
        self.dialogue.sight = self.desk.sight

    def covered(self, *, npc: str = HILDA, deal: str = "D1", **fields: Any) -> None:
        self.chronicle.record({"fact": "covered", "npc": npc, "deal": deal, **fields})
        if fields.get("crime"):
            self.chronicle.record(
                {"fact": "silenced", "npc": npc, "deal": deal, "cover": deal, "crime": fields["crime"]}
            )

    def saw(
        self, npc: str = HILDA, name: str = "Хильда", minute: int = AT_8,
        waypoint: str = "NW_FARM1_PATH_01",
    ) -> None:
        self.seen.saw(hero_seen_by(npc, name, minute, waypoint))
        self.seen.saw(hero_seen_by(npc, name, minute + 5, waypoint))

    async def testify(self, *, witness: str = HILDA, name: str = "Хильда", said: str = SAID):
        result = await self.witnesses._testify(None, LOBART, "Лобарт", witness, name, said, "far", 0)
        assert result is not None
        return result

    def violence(self, *, degree: str = "seen", kind: str = "attack", forgiven: str = "") -> None:
        self.chronicle.record(
            {
                "fact": "crime",
                "id": "violence",
                "kind": kind,
                "victim": MALETH,
                "day": 1,
                "hour": 7,
                "minute": 0,
                "community": "farm",
                "victim_guild": 8,
            }
        )
        if degree == "seen":
            self.chronicle.record({"fact": "crime_seen", "npc": HILDA, "crime": "violence"})
        elif degree == "heard":
            self.chronicle.record(
                {
                    "fact": "rumor",
                    "to": HILDA,
                    "from": MALETH,
                    "crime": "violence",
                    "kind": kind,
                    "hops": 1,
                }
            )
        elif degree == "suspects":
            self.chronicle.record(
                {
                    "fact": "crime_suspected",
                    "npc": HILDA,
                    "crime": "violence",
                    "suspect": "PLAYER",
                }
            )
        if forgiven:
            self.chronicle.record({"fact": "forgiven", "npc": forgiven, "crime": "violence"})

    async def catch(self, sid: str = "В1", *, said: str = SAID):
        conv = self.dialogue.open(LOBART, "Лобарт")
        conv.trigger = Message("user", LIE)
        self.seen.situation(LOBART)
        self.witnesses.situation(LOBART)
        result = await self.seen.handle(
            self.dialogue, conv, ToolCall("catch_lie", {"said": said, "seen": sid}, "catch"), None
        )
        assert result is not None
        if result.status == "accepted":
            await _until(lambda: bool(self.chronicle.facts("caught")), 5)
        return result


@pytest.mark.parametrize(
    "offset,place,hidden",
    [
        (-61, "FARM1_OUT", False),
        (-60, "FARM1_OUT", True),
        (60, "FARM1_OUT", True),
        (61, "FARM1_OUT", False),
        (0, "FARM1_PASTURE", False),
    ],
)
async def test_crime_cover_hides_the_deed_and_only_its_place_within_one_hour(offset, place, hidden):
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        w = CoverWorld(link, fake)
        fake.hour, fake.minute = 10, 0
        record_theft(w.chronicle)
        at = AT_8 + offset
        sighting = Sighting("PLAYER", "Герой", at, at, [place], place_ids=[place])
        w.seen.mind(HILDA).sightings.append(sighting)
        w.covered(crime="C1", vouch=VOUCH)
        t = await w.testify()

        assert t.saw == [sighting]
        assert t.told == ([] if hidden else [sighting])
        assert t.crime_saw == ["C1"] and t.crime_told == []
        fact = w.chronicle.facts("testimony")[0]
        assert fact["cover"] == "D1" and fact["vouch"] == VOUCH
        assert fact["crime_saw"] == ["C1"] and fact["crime_told"] == []
        assert fact["saw"] == [sighting.record()]


async def test_crime_cover_hides_a_later_trace_when_the_deed_just_left_the_question_window():
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        w = CoverWorld(link, fake)
        record_theft(w.chronicle)
        w.covered(crime="C1", vouch=VOUCH)
        at = AT_8 + 40
        sighting = Sighting("PLAYER", "Герой", at, at, place_ids=["FARM1_OUT"])
        w.seen.mind(HILDA).sightings.append(sighting)
        fake.day, fake.hour, fake.minute = 2, 8, 40

        t = await w.testify()

        assert t.at == AT_8 + 24 * 60 + 30
        assert t.saw == [sighting] and t.told == []
        assert t.crime_saw == t.crime_told == []  # the deed itself is over a day old
        fact = w.chronicle.facts("testimony")[0]
        assert fact["cover"] == "D1" and fact["vouch"] == VOUCH
        assert fact["saw"] == [sighting.record()] and fact["told"] == []


async def test_day_cover_keeps_other_days_and_other_deeds_truthful():
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        w = CoverWorld(link, fake)
        yesterday = Sighting("PLAYER", "Герой", 12 * 60, 12 * 60 + 5, place_ids=["FARM1_OUT"])
        today = Sighting("PLAYER", "Герой", AT_8, AT_8 + 5, place_ids=["FARM1_OUT"])
        w.seen.mind(HILDA).sightings.extend([yesterday, today])
        record_theft(w.chronicle)
        w.covered(day=1)
        t = await w.testify()
        assert t.saw == [today, yesterday] and t.told == [yesterday]
        assert t.crime_saw == t.crime_told == ["C1"]
        assert w.chronicle.facts("testimony")[0]["cover"] == "D1"


async def test_a_cover_outside_the_question_window_does_not_change_the_answer():
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        w = CoverWorld(link, fake)
        w.saw()
        w.covered(day=-1, vouch=VOUCH)
        t = await w.testify()
        assert t.told == t.saw and t.told
        assert not w.chronicle.facts("testimony")[0].get("cover")


@pytest.mark.parametrize("promise", ["silence", "crime_cover", "day_cover"])
@pytest.mark.parametrize("keeps", [False, True])
async def test_npc_breaking_a_promise_testifies_truthfully_and_betrayal_is_once(promise, keeps):
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        w = CoverWorld(link, fake)
        w.saw(waypoint="NW_FARM1_OUT_01")
        record_theft(w.chronicle)
        how = "silence" if promise == "silence" else "cover"
        terms = (
            {"silence": ["crime:C1"]} if promise == "silence" else
            {"cover": {"crime": "C1", "vouch": VOUCH}} if promise == "crime_cover" else
            {"cover": {"day": 1, "vouch": VOUCH}}
        )
        w.chronicle.record({
            "fact": "npc_intent", "npc": HILDA, "deal": "D1", "keeps": keeps, "how": [how],
        })
        w.chronicle.record({"fact": "deal", "npc": HILDA, "deal": "D1", **terms})
        t = await w.testify()
        assert t.saw and t.crime_saw == ["C1"]
        assert t.told == ([] if keeps and how == "cover" else t.saw)
        assert t.crime_told == ([] if keeps and promise != "day_cover" else ["C1"])
        assert t.cover == ("D1" if keeps and how == "cover" else "")
        await w.testify()
        assert w.chronicle.facts("betrayed") == ([] if keeps else [{
            "fact": "betrayed", "npc": HILDA, "deal": "D1", "how": "testified", "to": LOBART,
        }])


async def test_disabling_cheating_after_load_restores_cover_without_rewriting_facts():
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        w = CoverWorld(link, fake)
        w.saw(waypoint="NW_FARM1_OUT_01")
        record_theft(w.chronicle)
        w.chronicle.record({
            "fact": "npc_intent", "npc": HILDA, "deal": "D1", "keeps": False, "how": ["cover"],
        })
        w.chronicle.record({
            "fact": "deal", "npc": HILDA, "deal": "D1", "cover": {"crime": "C1", "vouch": VOUCH},
        })
        saved = w.store.head("A")
        await fake.send(type="session", branch="loaded", parent="A", parent_seq=saved, reason="load")
        await _until(lambda: w.chronicle.branch == "loaded", 5)
        w.chronicle.npc_cheat_enabled = False
        t = await w.testify()
        assert t.saw and not t.told and t.crime_saw == ["C1"] and not t.crime_told
        assert t.cover == "D1" and t.vouch == VOUCH
        assert w.chronicle.facts("covered") == w.chronicle.facts("silenced") == []
        assert w.chronicle.facts("npc_intent")[0]["keeps"] is False
        assert w.chronicle.facts("betrayed") == []


async def test_vouch_without_a_sighting_is_a_real_testimony_and_not_evidence():
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        w = CoverWorld(link, fake)
        w.covered(day=1, vouch=VOUCH)
        t = await w.testify()
        assert t.saw == t.told == [] and t.crime_saw == t.crime_told == []
        text = w.witnesses.situation(LOBART) or ""
        assert f"Хильда говорит: «{VOUCH}»" in text
        row = w.witnesses.told(LOBART, "С1")
        assert row is not None and not row.sightings and not row.crime
        out = await w.catch("С1")
        assert out.status == "rejected" and "уличить нечем" in out.output
        assert w.chronicle.facts("caught") == []


@pytest.mark.parametrize("tier", ["near", "walk", "far"])
async def test_cover_applies_to_all_three_question_tiers(tier):
    fake = FakeGame(branch="A")
    llm = lobart(
        ("Ответ:", CATCH),
        ("город", ASK_HILDA),
        hilda=[("спрашивает тебя", FakeStep("Он был со мной в поле."))],
    )
    async with connected(fake) as link:
        w = CoverWorld(link, fake, llm)
        who, name = (ANDRE, "Андре") if tier == "far" else (HILDA, "Хильда")
        w.saw(who, name)
        w.covered(npc=who, day=1, vouch=VOUCH)
        if tier == "walk":
            fake.npc_states[HILDA]["distance"] = 3000
        if tier == "far":
            conv = w.dialogue.open(LOBART, "Лобарт")
            conv.trigger = Message("user", LIE)
            await w.witnesses.prepare(LOBART)
            out = await w.witnesses.handle(
                w.dialogue, conv, ToolCall("ask_witness", {"who": name, "said": SAID}, "ask"), None
            )
            assert out is not None and out.status == "accepted"
            assert w.chronicle.facts("testimony") == []
            fake.hour, fake.minute = 18, 5
            await w.witnesses.tick()
        else:
            w.dialogue.open(LOBART, "Лобарт")
            await w.dialogue.say_as_player(LOBART, LIE)
            await _until(lambda: bool(w.chronicle.facts("testimony")), 10)
            await _until(lambda: any("уличить нечем" in s for s in llm.outputs), 10)
            cue = str(llm.others.calls[0][1][-1].content)
            assert "ты обещал прикрыть героя" in cue and VOUCH in cue
        (t,) = w.chronicle.facts("testimony")
        assert t["tier"] == tier and t["saw"] and t["told"] == []
        assert t["cover"] == "D1" and t["vouch"] == VOUCH
        assert VOUCH in (w.witnesses.situation(LOBART) or "")
        assert (await w.catch("С1")).status == "rejected"
        assert w.chronicle.facts("caught") == []


@pytest.mark.parametrize("degree,kind", [("seen", "attack"), ("heard", "attack"), ("heard", "murder")])
async def test_a_timorous_witness_with_unforgiven_violence_lies_about_everything(degree, kind):
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        w = CoverWorld(link, fake, courage=2)
        w.saw()
        w.violence(degree=degree, kind=kind)
        record_theft(w.chronicle)
        t = await w.testify()
        assert t.saw and t.crime_saw and not t.told and not t.crime_told
        assert w.chronicle.facts("testimony")[0]["cover"] == "fear"
        assert "говорит, что не видел" in (w.witnesses.situation(LOBART) or "")
        assert (await w.catch("С1")).status == "rejected"
        await w.witnesses._speak(w.dialogue.open(HILDA, "Хильда"), t)
        cue = str(w.dialogue.llm.others.calls[0][1][-1].content)
        assert "ты боишься героя: скажи, что ничего не видел" in cue


@pytest.mark.parametrize(
    "courage,degree,kind,forgiven",
    [
        (3, "seen", "attack", ""),
        (2, "none", "attack", ""),
        (None, "seen", "attack", ""),
        (2, "suspects", "attack", ""),
        (2, "heard", "theft", ""),
        (2, "heard", "murder", HILDA),
    ],
)
async def test_without_fear_ground_or_after_its_own_forgiveness_a_witness_tells_truth(
    courage, degree, kind, forgiven
):
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        w = CoverWorld(link, fake, courage=courage)
        w.saw()
        w.violence(degree=degree, kind=kind, forgiven=forgiven)
        t = await w.testify()
        assert t.saw and t.told == t.saw and t.crime_told == t.crime_saw
        assert not w.chronicle.facts("testimony")[0].get("cover")


async def test_another_persons_forgiveness_does_not_remove_the_witnesss_fear():
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        w = CoverWorld(link, fake, courage=2)
        w.saw()
        w.violence(degree="heard", kind="murder", forgiven=MALETH)
        await w.testify()
        assert w.chronicle.facts("testimony")[0]["cover"] == "fear"


async def test_a_deal_cover_takes_priority_over_fear_and_keeps_its_words():
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        w = CoverWorld(link, fake, courage=2)
        w.saw()
        w.violence(degree="heard", kind="murder")
        w.covered(day=1, vouch=VOUCH)
        await w.testify()
        fact = w.chronicle.facts("testimony")[0]
        assert fact["cover"] == "D1" and fact["vouch"] == VOUCH
        assert VOUCH in (w.witnesses.situation(LOBART) or "")


@pytest.mark.parametrize("source", ["own", "other"])
@pytest.mark.parametrize("cover", ["deal", "fear"])
@pytest.mark.parametrize("said", [SAID, "был в городе"])
async def test_catching_the_same_words_exposes_a_covering_witness(source, cover, said):
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        w = CoverWorld(link, fake, courage=2 if cover == "fear" else None)
        w.saw()
        if cover == "fear":
            w.violence(degree="heard", kind="murder")
        else:
            w.covered(day=1, vouch=VOUCH)
        false = await w.testify()
        if source == "own":
            w.saw(LOBART, "Лобарт")
            sid = "В1"
        else:
            fake.minute += 1  # the second witness's answer is first in the situation
            w.saw(MALETH, "Малет")
            await w.testify(witness=MALETH, name="Малет")
            sid = "С1"
        assert (await w.catch(sid, said=said)).status == "accepted"
        (caught,) = w.chronicle.facts("caught")
        (exposed,) = w.chronicle.facts("exposed")
        assert (exposed["npc"], exposed["liar"], exposed["testimony"]) == (LOBART, HILDA, false.id)
        assert exposed["by"] == caught["quest"]
        assert exposed["cover"] == ("fear" if cover == "fear" else "D1")
        notes = " ".join(e.content for e in w.dialogue.conversations[LOBART].memory.entries)
        assert "Хильда" in notes
        assert ("побоял" if cover == "fear" else "покрывая героя") in notes


@pytest.mark.parametrize("vouch,exposed", [("", False), (VOUCH, True)])
async def test_vouch_can_be_a_lie_without_sightings_but_honest_ignorance_cannot(vouch, exposed):
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        w = CoverWorld(link, fake)
        w.covered(day=1, vouch=vouch)
        await w.testify()
        w.saw(LOBART, "Лобарт")
        assert (await w.catch()).status == "accepted"
        assert bool(w.chronicle.facts("exposed")) is exposed


async def test_cover_and_exposure_survive_load_and_the_same_pair_is_not_exposed_twice():
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        w = CoverWorld(link, fake)
        w.saw()
        w.covered(day=1, vouch=VOUCH)
        false = await w.testify()
        w.saw(LOBART, "Лобарт")
        assert (await w.catch()).status == "accepted"
        (exposed,) = w.chronicle.facts("exposed")
        (caught,) = w.chronicle.facts("caught")
        saved = w.store.head("A")
        await fake.send(type="session", branch="loaded", parent="A", parent_seq=saved, reason="load")
        await _until(lambda: w.chronicle.branch == "loaded", 5)
        assert w.chronicle.facts("covered")[0]["deal"] == "D1"
        assert w.chronicle.facts("exposed") == [exposed]
        fake.minute += 2
        restored = await w.testify(said="с утра сидел в таверне")
        assert restored.saw and not restored.told
        assert w.chronicle.facts("testimony")[-1]["cover"] == "D1"
        w.chronicle.record(caught)
        assert w.chronicle.facts("exposed") == [exposed]
        assert exposed["testimony"] == false.id


async def test_a_catch_for_different_words_does_not_expose_the_witness():
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        w = CoverWorld(link, fake)
        w.saw()
        w.covered(day=1, vouch=VOUCH)
        await w.testify()
        w.chronicle.record(
            {
                "fact": "caught",
                "quest": "other",
                "npc": LOBART,
                "title": "я ничего не брал",
                "said": True,
                "how": "сам видел",
                "day": 1,
            }
        )
        assert w.chronicle.facts("exposed") == []


async def test_no_cover_ignores_deals_fear_and_old_covered_testimonies():
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        w = CoverWorld(link, fake, courage=2, cover=False)
        w.saw()
        w.violence(degree="heard", kind="murder")
        record_theft(w.chronicle)
        record_theft(w.chronicle, id="ordinary-silence", hour=7)
        w.covered(crime="C1", day=1, vouch=VOUCH)
        w.chronicle.record({"fact": "silenced", "npc": HILDA, "crime": "ordinary-silence"})
        t = await w.testify()
        assert t.told == t.saw and t.saw
        assert "C1" in t.crime_told and "ordinary-silence" not in t.crime_told
        fact = w.chronicle.facts("testimony")[0]
        assert not fact.get("cover") and not fact.get("vouch")
        w.chronicle.record(
            {
                **fact,
                "id": "old-cover",
                "cover": "D1",
                "vouch": VOUCH,
                "told": [],
                "crime_told": [],
            }
        )
        old = next(t for t in w.witnesses.testimonies(LOBART) if t.id == "old-cover")
        assert old.told == old.saw and "C1" in old.crime_told
        assert "ordinary-silence" not in old.crime_told
        assert VOUCH not in (w.witnesses.situation(LOBART) or "")
        w.chronicle.record(
            {
                "fact": "caught",
                "quest": "proof",
                "npc": LOBART,
                "title": SAID,
                "said": True,
                "how": "сам видел",
                "day": 1,
            }
        )
        assert w.chronicle.facts("exposed") == []
