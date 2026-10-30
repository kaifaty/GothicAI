import asyncio
from dataclasses import dataclass, field
from types import SimpleNamespace

import pytest

import gothic_ai.dialogue
from gothic_ai.amends import Lie as AmendsLie
from gothic_ai.budget import Budget
from gothic_ai.chronicle import Chronicle
from gothic_ai.concerns import Concerns
from gothic_ai.crime_law import CrimeLaw
from gothic_ai.crime_memory import memory_value
from gothic_ai.crime_react import CrimeReact
from gothic_ai.crimes import Crimes
from gothic_ai.deals import Deals, LieRef
from gothic_ai.dialogue import DialogueManager
from gothic_ai.fear import knows_violence
from gothic_ai.game import GameError
from gothic_ai.llm import Message, ToolCall
from gothic_ai.llm.fake import FakeStep
from gothic_ai.memory import MemoryStore
from gothic_ai.perception import PerceptionDesk
from gothic_ai.persona import PersonaProvider
from gothic_ai.protocol import NpcView
from gothic_ai.questbook import Goal, Progress, Quest, Stage, Talk
from gothic_ai.scenarios.common import ClaimScript
from gothic_ai.sightings import Sightings
from gothic_ai.social import Tie
from gothic_ai.storyguard import StoryGuard
from gothic_ai.trust import Trust
from gothic_ai.witnesses import ASK_TOOL, Candidate, Inquiry, Witnesses, _named
from gothic_ai.witnesses import Testimony as WitnessTestimony
from tests.fake_game import FakeGame, connected
from tests.test_crimes import deed
from tests.test_dialogue import _until
from tests.test_perception import HILDA, LOBART, MALETH, look
from tests.test_sightings import BOOK, record_theft

ANDRE, STRANGER = "MIL_311_ANDRE", "VLK_999_STRANGER"
DAY = 1440
AT_8 = DAY + 8 * 60 + 10  # the second day, 08:10
LIE = "Я всё утро был в городе, только что оттуда."


@dataclass
class Social:
    """Who knows whom: Lobart meets Hilda and Maleth on the farm, Andre in the evening (a dealing)."""

    ties: dict[str, dict[str, Tie]] = field(
        default_factory=lambda: {
            LOBART: {
                HILDA: Tie([(6 * 60, 22 * 60)]),
                MALETH: Tie([(6 * 60, 22 * 60)]),
                ANDRE: Tie([(18 * 60, 19 * 60)]),
            }
        }
    )
    names: dict[str, str] = field(
        default_factory=lambda: {HILDA: "Хильда", MALETH: "Малет", ANDRE: "Андре", LOBART: "Лобарт"}
    )
    guilds: dict[str, int] = field(default_factory=dict)


def hero_seen_by(npc: str, name: str, t: int, waypoint: str = "NW_FARM1_PATH_01") -> NpcView:
    """The npc's view at game minute `t`: the hero in sight (on the pasture, by default)."""
    return NpcView.model_validate(
        {"npc": npc, "name": name, "time": t, "hero": look("PLAYER", "Я", 8, seen=True, waypoint=waypoint)}
    )


def lobarts_view(state: str = "ZS_STAND") -> dict:
    return {
        "npc": LOBART,
        "name": "Лобарт",
        "waypoint": "NW_FARM1_OUT_01",
        "state": state,
        "time": DAY + 9 * 60 + 35,
        "hero": look("PLAYER", "Я", 3, seen=True, waypoint="NW_FARM1_OUT_01"),
        "sees": [look(HILDA, "Хильда", 9, state="ZS_COOK_CAULDRON", waypoint="NW_FARM1_OUT_02")],
    }


def hildas_view(state: str = "ZS_COOK_CAULDRON") -> dict:
    return {
        "npc": HILDA,
        "name": "Хильда",
        "waypoint": "NW_FARM1_OUT_02",
        "state": state,
        "time": DAY + 9 * 60 + 35,
    }


class World:
    """Lobart's farm at 09:35 of the second day: Hilda 9 m off, Maleth 40 m, Andre in the city."""

    def __init__(self, link, fake: FakeGame, llm, *, budget: Budget | None = None) -> None:
        fake.day, fake.hour, fake.minute = 1, 9, 35
        fake.npc_states = {
            HILDA: {"state": "alive", "name": "Хильда", "distance": 900},
            MALETH: {"state": "alive", "name": "Малет", "distance": 4000},
            ANDRE: {"state": "alive", "name": "Андре", "distance": 90000},
        }
        fake.views = {LOBART: lobarts_view(), HILDA: hildas_view()}
        self.store = MemoryStore(":memory:")
        self.dialogue = DialogueManager(link, llm, PersonaProvider(None), memory=self.store)
        self.chronicle = Chronicle(link, self.store)
        social = Social()
        self.desk = PerceptionDesk(link, social=lambda: social, places=BOOK)  # type: ignore[arg-type,return-value]
        self.seen = Sightings(link, self.store, self.desk, self.chronicle)
        self.held: list[tuple[str, str]] = []

        async def take(npc: str, why: str) -> bool:
            await link.attach(npc)
            self.held.append((npc, why))
            return True

        self.dialogue.take_body = take
        self.dialogue.free_body = lambda npc, why: None
        self.witnesses = Witnesses(
            link,
            self.dialogue,
            self.seen,
            self.desk,
            self.chronicle,
            StoryGuard(None),
            social=lambda: social,  # type: ignore[arg-type,return-value]
            budget=budget,
            hero_cm=lambda npc: 500.0,
        )
        self.trust = Trust(self.chronicle)
        self.dialogue.extensions += [self.desk, self.seen, self.witnesses, self.trust]
        self.dialogue.sight = self.desk.sight


def lobart(*rules: tuple[str, FakeStep], hilda: list[tuple[str, FakeStep]] | None = None) -> ClaimScript:
    others = ClaimScript(hilda or [], [], "Ну.", only="Хильда")
    return ClaimScript(list(rules), [], "Ну?", only="Лобарт", others=others)


ASK_HILDA = FakeStep(
    "Хильда! Ты его утром не видала?",
    [ToolCall("ask_witness", {"who": "Хильду", "said": "всё утро был в городе"}, "a1")],
)
CATCH = FakeStep(
    "Так ты врёшь!", [ToolCall("catch_lie", {"said": "всё утро был в городе", "seen": "C1"}, "c1")]
)


async def test_ask_witness_reports_its_actual_crime_seen_and_catches_a_denial_by_that_testimony():
    fake = FakeGame(branch="A")
    denial = "я ничего не брал"
    llm = lobart(
        ("Ответ:", FakeStep("Хильда видела, а ты врёшь.", [
            ToolCall("catch_lie", {"said": denial, "seen": "С1"}, "catch"),
        ])),
        ("ничего", FakeStep("Хильда, ты видела, что он взял?", [
            ToolCall("ask_witness", {"who": "Хильду", "said": denial}, "ask"),
        ])),
        hilda=[("спрашивает тебя", FakeStep("Я своими глазами видела, как он взял вещь."))],
    )
    async with connected(fake) as link:
        w = World(link, fake, llm)
        crimes = Crimes(link, w.chronicle)
        w.seen.crimes = w.witnesses.crimes = crimes
        record_theft(w.chronicle)
        w.dialogue.open(LOBART, "Лобарт")
        await w.dialogue.say_as_player(LOBART, "Я ничего не брал.")
        await _until(lambda: bool(w.chronicle.facts("caught")), 10)

    testimony = w.chronicle.facts("testimony")[0]
    assert testimony["saw"] == [] and testimony["told"] == []
    assert testimony["crime_saw"] == testimony["crime_told"] == ["C1"]
    caught = w.chronicle.facts("caught")[0]
    assert (caught["crime"], caught["witness"], caught["testimony"]) == ("C1", HILDA, testimony["id"])
    assert caught["how"].startswith("узнал со слов: Хильда")
    reaction = next(c for c in llm.calls if "Ответ:" in str(c[1][-1].content))
    assert "С1: Хильда" in str(reaction[1][-2].content)
    assert "видел собственными глазами" in str(reaction[1][-2].content)
    assert "catch_lie" in [tool.name for tool in reaction[2]]
    assert w.dialogue.conversations[LOBART].intent.attitude == "wary"


async def test_crime_testimony_uses_only_what_witness_saw_before_the_claim_with_exact_evidence_rows():
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        w = World(link, fake, lobart())
        crimes = Crimes(link, w.chronicle)
        w.seen.crimes = w.witnesses.crimes = crimes
        record_theft(w.chronicle, id="before", hour=8)
        record_theft(w.chronicle, id="earlier", hour=7)
        record_theft(w.chronicle, id="after", hour=10)
        record_theft(w.chronicle, id="rumor", witness=MALETH, hour=6)
        w.chronicle.record({
            "fact": "rumor", "crime": "rumor", "kind": "theft", "from": MALETH, "to": HILDA,
            "hops": 1,
        })
        t = await w.witnesses._testify(None, LOBART, "Лобарт", HILDA, "Хильда", "ничего не брал", "far", 0)
        assert t is not None
        assert set(t.crime_told) == {"before", "earlier"}
        assert t.crime_told == t.crime_saw
        situation = w.witnesses.situation(LOBART) or ""
        assert "С1: Хильда" in situation and "С2: Хильда" in situation
        first, second = w.witnesses.told(LOBART, "С1"), w.witnesses.told(LOBART, "С2")
        assert first is not None and second is not None
        assert {first.crime, second.crime} == {"before", "earlier"}
        assert len(w.witnesses.concerns(LOBART)) == 1  # even without an ordinary hero sighting


@pytest.mark.parametrize("why", ["hostile", "quest"])
@pytest.mark.parametrize("asker,guild", [(LOBART, 8), (ANDRE, 2)])
async def test_testimony_records_askers_own_side_before_knowledge_and_guard_reporting(why, asker, guild):
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        w = World(link, fake, lobart())
        crimes = Crimes(link, w.chronicle)
        w.witnesses.crimes = crimes
        crimes.people.update({
            asker: {"guild": guild, "npctype": 0},
            HILDA: {"guild": 12, "npctype": 0},
            MALETH: {"guild": 3, "npctype": 0},
        })
        if why == "hostile":
            crimes.sides.hostile_guilds = {(guild, 3)}
        else:
            quest = Quest("DIR_Q1", asker, "Убить врага", Talk("Возьмусь", []), [
                Stage("Убить", [Goal(kind="kill", npcs=[MALETH])]),
            ], "Готово")
            crimes.sides.book = SimpleNamespace(
                all=lambda: {quest.id: quest}, progress=lambda _: Progress("given", 1),
            )
        case = crimes.record(deed(
            kind="murder", victim=MALETH, victim_guild=3, day=1, hour=8,
            witnesses=[HILDA], witnesses_metadata=[{"npc": HILDA, "guild": 12}],
        ))
        assert case is not None and crimes.side(HILDA, case["id"]) is None
        law = CrimeLaw(crimes)
        react = CrimeReact(link, crimes, law)
        observed = []
        w.chronicle.on_fact.append(lambda f: observed.append((
            f["fact"], crimes.side(asker, case["id"]), crimes.attitude(asker),
        )))
        t = await w.witnesses._testify(
            None, asker, asker, HILDA, "Хильда", "никого не убивал", "far", 0,
        )
        assert t is not None and t.crime_told == [case["id"]]
        assert [kind for kind, _, _ in observed] == ["crime_side", "testimony"]
        assert all(side is not None and side["why"] == why for _, side, _ in observed)
        assert all(attitude is None for _, _, attitude in observed)
        assert react.role(asker, case) == "ally"
        assert not knows_violence(asker, crimes)
        assert memory_value(crimes, asker) == (0, 0)
        assert law.deliveries() == []
        assert law.tick(DAY + 12 * 60) == []
        assert w.chronicle.facts("crime_known") == []
        assert w.chronicle.facts("crime_attitude") == []
        await react.close()


async def test_testimony_preserves_witness_justification_for_only_its_exact_crime():
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        w = World(link, fake, lobart())
        crimes = Crimes(link, w.chronicle)
        w.witnesses.crimes = crimes
        crimes.people.update({
            LOBART: {"guild": 8, "npctype": 0},
            HILDA: {"guild": 12, "npctype": 0},
            MALETH: {"guild": 3, "npctype": 0},
            STRANGER: {"guild": 9, "npctype": 0},
        })
        crimes.sides.note_fight(MALETH, HILDA)
        justified = crimes.record(deed(
            id="justified", kind="murder", victim=MALETH, victim_guild=3, day=1, hour=8,
            witnesses=[HILDA], witnesses_metadata=[{"npc": HILDA, "guild": 12}],
        ))
        ordinary = crimes.record(deed(
            id="ordinary", kind="murder", victim=STRANGER, victim_guild=9, day=1, hour=8,
            witnesses=[HILDA], witnesses_metadata=[{"npc": HILDA, "guild": 12}],
        ))
        assert justified is not None and ordinary is not None
        t = await w.witnesses._testify(
            None, LOBART, "Лобарт", HILDA, "Хильда", "никого не убивал", "far", 0,
        )
        assert t is not None and set(t.crime_told) == {"justified", "ordinary"}
        (fact,) = w.chronicle.facts("testimony")
        assert fact["crime_excuses"] == {"justified": {
            "excuse": "fought", "excuse_by": HILDA, "excuse_by_name": "Хильда",
        }}
        known = {k.crime["id"]: k for k in crimes.known(LOBART)}
        assert known["justified"].degree == "heard"
        assert known["justified"].excuse == "fought"
        assert known["justified"].excuse_by == HILDA
        assert not known["ordinary"].excuse
        assert crimes.side(LOBART, "justified") is None


async def test_a_witness_called_over_tells_what_it_saw_and_the_lie_is_caught_by_its_word():
    fake = FakeGame(branch="A")
    llm = lobart(
        ("Ответ:", CATCH),
        ("город", ASK_HILDA),
        hilda=[("спрашивает тебя", FakeStep("Видела его утром на пастбище."))],
    )
    async with connected(fake) as link:
        w = World(link, fake, llm)
        for t in range(AT_8, AT_8 + 21, 5):  # this morning Hilda saw him on the pasture; Lobart did not
            w.seen.saw(hero_seen_by(HILDA, "Хильда", t))
        w.dialogue.open(LOBART, "Лобарт")
        await w.dialogue.say_as_player(LOBART, LIE)
        await _until(lambda: w.chronicle.facts("caught") != [], 10)

    first = llm.calls[0]
    assert "ask_witness" in [t.name for t in first[2]]
    situation = str(first[1][-2].content)
    assert (
        "Кого из знакомых можно расспросить о герое (ask_witness):\n- Хильда — рядом, в 9 м: окликнешь"
        in situation
    )
    assert "- Малет — в 40 м: дойдёшь и спросишь" in situation
    assert "- Андре — далеко: спросишь при встрече (сегодня около 18:00)" in situation

    t = w.chronicle.facts("testimony")[0]
    assert (t["npc"], t["witness"], t["said"], t["tier"]) == (LOBART, HILDA, "всё утро был в городе", "near")
    assert t["told"] and t["told"][0]["places"] == ["на пастбище у входа на ферму Лобарта"]
    assert t["told"][0]["place_ids"] == ["FARM1_PASTURE"]
    # Hilda was held, came up to Lobart and turned to him; then she answered aloud with what she saw
    acts = [
        (m["npc"], m["action"], m.get("args", {}).get("npc")) for m in fake.received if m["type"] == "action"
    ]
    assert (HILDA, "go_to_npc", LOBART) in acts and (LOBART, "play_ani", None) in acts
    hilda = llm.others.calls[0]  # type: ignore[union-attr]
    cue = str(hilda[1][-1].content)
    assert "Лобарт спрашивает тебя о герое: тот говорит «всё утро был в городе»" in cue
    assert "Что ты видел сам: сегодня 08:10–08:30" in cue and hilda[2] == []
    assert "Видела его утром на пастбище." in fake.said

    # Lobart's reaction: the answer is С1, catch_lie is offered for it
    reaction = next(c for c in llm.calls if "Ответ:" in str(c[1][-1].content))
    assert "catch_lie" in [x.name for x in reaction[2]]
    assert (
        "С1: Хильда, сегодня в 09:35, о его словах «всё утро был в городе»: героя видели сегодня 08:10–08:30"
        in str(reaction[1][-2].content)
    )
    c = w.chronicle.facts("caught")[0]
    assert (c["npc"], c["said"], c["witness"], c["witness_name"]) == (LOBART, True, HILDA, "Хильда")
    assert c["how"].startswith("узнал со слов: Хильда, сегодня в 09:35") and c["testimony"]
    assert "он уже лгал тебе: «всё утро был в городе» — узнал со слов: Хильда" in (
        w.trust.situation(LOBART) or ""
    )
    assert w.dialogue.conversations[LOBART].intent.attitude == "wary"
    # Hilda remembers she was asked and what the hero claims
    assert any(
        "тот говорит «всё утро был в городе»" in e.content
        for e in w.dialogue.conversations[HILDA].memory.entries
    )


async def test_a_witness_that_did_not_see_him_can_not_catch_him():
    fake = FakeGame(branch="A")
    llm = lobart(
        ("Ответ:", CATCH), ("город", ASK_HILDA), hilda=[("спрашивает тебя", FakeStep("Не видала я его."))]
    )
    async with connected(fake) as link:
        w = World(link, fake, llm)
        w.dialogue.open(LOBART, "Лобарт")
        await w.dialogue.say_as_player(LOBART, LIE)
        await _until(lambda: any("Ответ:" in str(c[1][-1].content) for c in llm.calls), 10)
        await _until(lambda: not w.dialogue.speaking(LOBART), 5)

    t = w.chronicle.facts("testimony")[0]
    assert t["witness"] == HILDA and t["told"] == []
    reaction = next(c for c in llm.calls if "Ответ:" in str(c[1][-1].content))
    assert "героя не видели за сутки до того" in str(reaction[1][-1].content)
    # it saw nothing: nothing to catch him with, the lie stands
    assert any(
        o.startswith("Отклонено: со слов: Хильда — героя не видели, уличить нечем.")
        and (
            " Ты уже сказал вслух: «Так ты врёшь!». "
            "Если отказ этого не меняет — ничего не говори. "
            "Если меняет — поправься одной короткой фразой, не повторяя сказанного."
        ) in o
        for o in llm.outputs
    )
    assert w.chronicle.facts("caught") == []


@pytest.mark.parametrize("answer", [
    "Подтверждаю: у героя есть поручение от меня.", "Отрицаю: герой мне солгал.",
])
async def test_gate_claim_answer_uses_code_truth_and_survives_the_testimony_fact(answer):
    fake = FakeGame(branch="A")
    llm = lobart(hilda=[("спрашивает тебя", FakeStep(answer))])
    heard = []

    async def gate_claim(witness, asker, said):
        heard.append((witness, asker, said))
        return answer

    async with connected(fake) as link:
        w = World(link, fake, llm)
        w.witnesses.gate_claim = gate_claim
        w.chronicle.record({
            "fact": "covered", "npc": HILDA, "deal": "cover-gate", "day": 1,
            "vouch": "Герой был со мной.",
        })
        said = "Хильда ждёт меня"
        t = await w.witnesses._testify(None, LOBART, "Лобарт", HILDA, "Хильда", said, "near", 0)
        assert t is not None
        assert heard == [(HILDA, LOBART, said)]
        assert t.gate_claim == answer and not t.cover and not t.vouch
        fact = w.chronicle.facts("testimony")[0]
        assert WitnessTestimony.of(fact).gate_claim == answer
        assert answer in (w.witnesses.situation(LOBART) or "")
        assert "героя не видели за сутки до того" not in w.witnesses._told(t)
        await w.witnesses._speak(w.dialogue.open(HILDA, "Хильда"), t)
        cue = str(llm.others.calls[-1][1][-1].content)
        assert answer in cue and "ложное заявление отрицай" in cue


async def test_gate_claim_is_checked_when_a_far_inquiry_reaches_the_named_witness():
    fake = FakeGame(branch="A")
    heard = []

    async def gate_claim(witness, asker, said):
        heard.append((witness, asker, said))
        return "Отрицаю: у героя нет связи со мной."

    async with connected(fake) as link:
        w = World(link, fake, lobart())
        w.witnesses.gate_claim = gate_claim
        q = Inquiry("gate-question", LOBART, "Лобарт", ANDRE, "Андре", "Андре ждёт меня", AT_8, DAY + 18 * 60)
        w.witnesses._inquire(q)
        await w.witnesses.tick()
        assert not heard
        fake.hour = 18
        await w.witnesses.tick()
        assert heard == [(ANDRE, LOBART, q.said)]
        assert w.chronicle.facts("testimony")[0]["gate_claim"].startswith("Отрицаю")


@pytest.mark.parametrize("error", [GameError, TimeoutError])
async def test_gate_claim_error_keeps_far_inquiry_until_an_answer(error):
    fake = FakeGame(branch="A")
    attempts = 0

    async def gate_claim(witness, asker, said):
        nonlocal attempts
        attempts += 1
        if attempts == 1:
            raise error("Game unavailable")
        return "Отрицаю: у героя нет связи со мной."

    async with connected(fake) as link:
        w = World(link, fake, lobart())
        w.witnesses.gate_claim = gate_claim
        q = Inquiry("gate-question", LOBART, "Лобарт", ANDRE, "Андре", "Андре ждёт меня", AT_8, DAY + 18 * 60)
        w.witnesses._inquire(q)
        fake.hour = 18
        await w.witnesses.tick()
        assert q.id in w.witnesses.inquiries
        assert not w.chronicle.facts("testimony")
        assert not w.chronicle.facts("inquiry_lapsed")

        await w.witnesses.tick()
        assert attempts == 2 and not w.witnesses.inquiries
        assert len(w.chronicle.facts("testimony")) == 1
        assert w.chronicle.facts("testimony")[0]["id"] == q.id


@pytest.mark.parametrize("error", [GameError, TimeoutError])
async def test_failed_far_inquiry_does_not_block_next_question_and_lapses_after_three_attempts(error):
    fake = FakeGame(branch="A")
    attempts = []

    async def gate_claim(witness, asker, said):
        attempts.append(said)
        if said == "Андре ждёт меня":
            raise error("Game unavailable")
        return "Подтверждаю: у героя есть связь со мной."

    async with connected(fake) as link:
        w = World(link, fake, lobart())
        w.witnesses.gate_claim = gate_claim
        failed = Inquiry(
            "failed-question", LOBART, "Лобарт", ANDRE, "Андре", "Андре ждёт меня", AT_8,
            DAY + 18 * 60,
        )
        next_question = Inquiry(
            "next-question", LOBART, "Лобарт", ANDRE, "Андре", "Андре меня послал", AT_8,
            DAY + 18 * 60 + 1,
        )
        w.witnesses._inquire(failed)
        w.witnesses._inquire(next_question)
        fake.hour, fake.minute = 18, 5

        await w.witnesses.tick()
        assert failed.id in w.witnesses.inquiries
        assert next_question.id not in w.witnesses.inquiries
        [testimony] = w.chronicle.facts("testimony")
        assert testimony["id"] == next_question.id
        assert testimony["gate_claim"].startswith("Подтверждаю")
        assert not w.chronicle.facts("inquiry_lapsed")

        await w.witnesses.tick()
        assert failed.id in w.witnesses.inquiries
        await w.witnesses.tick()
        assert not w.witnesses.inquiries
        assert w.chronicle.facts("inquiry_lapsed") == [
            {"fact": "inquiry_lapsed", "id": failed.id, "why": "error"}
        ]
        await w.witnesses.tick()
        assert attempts == [failed.said, next_question.said, failed.said, failed.said]
        assert len(w.chronicle.facts("testimony")) == 1


async def test_far_inquiry_with_no_answer_is_removed_when_branch_does_not_change():
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        w = World(link, fake, lobart())
        w.witnesses.npc_crimes_enabled = False
        q = Inquiry(
            "disabled-npc-question", LOBART, "Лобарт", ANDRE, "Андре", "Хильда взяла кошелёк", AT_8,
            DAY + 18 * 60, about=HILDA, about_name="Хильда",
        )
        w.witnesses._inquire(q)
        fake.hour = 18

        await w.witnesses.tick()
        assert not w.witnesses.inquiries
        assert not w.chronicle.facts("testimony")
        assert not w.chronicle.facts("inquiry_lapsed")


@pytest.mark.parametrize("error", [GameError, TimeoutError])
@pytest.mark.parametrize("tier", ["near", "walk"])
async def test_gate_claim_error_reports_unanswered_near_question(error, tier):
    fake = FakeGame(branch="A")

    async def gate_claim(witness, asker, said):
        raise error("Game unavailable")

    async with connected(fake) as link:
        w = World(link, fake, lobart())
        w.witnesses.gate_claim = gate_claim
        if tier == "walk":
            fake.npc_states[HILDA]["distance"] = 3000
        conv = w.dialogue.open(LOBART, "Лобарт")
        await w.witnesses.prepare(LOBART)
        c = next(c for c in w.witnesses._near[LOBART] if c.npc == HILDA)
        assert c.tier == tier

        text, success = await w.witnesses._ask(conv, c, "Хильда ждёт меня", AT_8)
        assert not success and "Хильда" in text and "пока не проверены" in text
        assert not w.chronicle.facts("testimony")
        if tier == "walk":
            assert any(
                m["type"] == "action" and m["npc"] == LOBART
                and m["action"] == "go_to_npc" and m.get("args", {}).get("npc") == "PLAYER"
                for m in fake.received
            )


async def test_whom_the_hero_names_to_ask_is_pointed_out_with_the_call_it_takes():
    # 2026-10-29 live: on «спроси Ватраса» Lobart said «Встречу Ватраса — спрошу» and called nothing
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        w = World(link, fake, lobart())
        conv = w.dialogue.open(LOBART, "Лобарт")
        await w.witnesses.prepare(LOBART)

        def offered(phrase: str, role: str = "user") -> str:
            conv.trigger = Message(role, phrase)
            situation = w.witnesses.situation(LOBART) or ""
            line = next((x for x in situation.splitlines() if x.startswith("Герой предлагает")), "")
            return line

        assert offered("Не веришь — спроси Андре, он подтвердит.") == (
            "Герой предлагает расспросить: Андре. Решишь расспросить — вызови ask_witness: сказать «спрошу» "
            "мало, без вызова ты не спросишь."
        )
        assert offered("Спроси Малета или Хильду.").startswith("Герой предлагает расспросить: Хильда, Малет.")
        vino = Candidate("BAU_952_VINO", "Вино", "near", 900.0, True)
        assert _named([vino], "Налей мне вино, хозяин.") == []  # the drink, not Вино
        assert _named([vino], "Спроси у Вино.") == [vino]
        assert offered("[Андре спрашивает тебя о герое]", "developer") == ""  # not the hero's words
        w.witnesses._inquire(
            Inquiry("q", LOBART, "Лобарт", ANDRE, "Андре", "всё утро был в городе", AT_8, DAY + 18 * 60)
        )
        assert offered("Спроси Андре.") == ""  # already going to


async def test_who_may_be_asked_and_when_not():
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        w = World(link, fake, lobart())
        conv = w.dialogue.open(LOBART, "Лобарт")
        conv.trigger = Message("user", LIE)
        await w.witnesses.prepare(LOBART)
        assert [c.name for c in w.witnesses._near[LOBART]] == ["Хильда", "Малет", "Андре"]

        async def ask(who: str, said: str = "всё утро был в городе") -> tuple[str, str]:
            out = await w.witnesses.handle(
                w.dialogue, conv, ToolCall("ask_witness", {"who": who, "said": said}, "x"), None
            )
            assert out is not None
            return out.status, out.output

        status, text = await ask("Бенгар")
        assert status == "rejected" and "Хильда, Малет, Андре" in text
        status, text = await ask("Хильда", "убил волка")
        assert status == "rejected" and "он этого не говорил" in text
        fake.views[HILDA] = hildas_view("ZS_SLEEP")
        status, text = await ask("Хильда")
        assert (status, text) == ("rejected", "Отклонено: Хильда спит.")
        # the same question to her within the hour: the old answer, no new one
        w.chronicle.record(
            {"fact": "testimony", "id": "T1", "npc": LOBART, "name": "Лобарт", "witness": HILDA,
             "witness_name": "Хильда", "said": "всё утро был в городе", "tier": "near", "saw": [], "told": [],
             "minute": DAY + 9 * 60 + 20}
        )  # fmt: skip
        status, text = await ask("Хильду")
        assert status == "read" and "Ты уже спрашивал: Хильда" in text
        # nobody it knows: no tool
        fake.npc_states = {}
        w.desk._views.clear()
        await w.witnesses.prepare(LOBART)
        assert w.witnesses.tools(LOBART) == []
    params = ASK_TOOL.parameters
    assert params["additionalProperties"] is False
    assert set(params["required"]) == set(params["properties"]) == {"who", "said", "about"}
    assert params["properties"]["about"]["type"] == ["string", "null"]


async def test_a_far_witness_is_asked_at_their_meeting_and_the_answer_is_on_its_mind():
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        w = World(link, fake, lobart())
        concerns = Concerns(link, w.chronicle, Deals(link, w.dialogue, w.chronicle))
        concerns.extra.append(w.witnesses.concerns)
        for t in (AT_8, AT_8 + 15):  # Andre saw the hero on the pasture this morning too
            w.seen.saw(hero_seen_by(ANDRE, "Андре", t))
        conv = w.dialogue.open(LOBART, "Лобарт")
        conv.trigger = Message("user", LIE)
        await w.witnesses.prepare(LOBART)
        out = await w.witnesses.handle(
            w.dialogue,
            conv,
            ToolCall("ask_witness", {"who": "Андре", "said": "всё утро был в городе"}, "x"),
            None,
        )
        assert (
            out is not None and out.status == "accepted" and "при встрече (сегодня около 18:00)" in out.output
        )
        assert "Ты собирался расспросить при встрече:\n- Андре" in (w.witnesses.situation(LOBART) or "")
        # a load keeps the question
        again = Witnesses(link, w.dialogue, w.seen, w.desk, w.chronicle, StoryGuard(None))
        assert [q.witness for q in again.inquiries.values()] == [ANDRE]
        w.seen.hearsay = w.witnesses

        fake.hour = 17
        await w.witnesses.tick()
        assert w.chronicle.facts("testimony") == []
        fake.hour, fake.minute = 18, 5
        await w.witnesses.tick()
        t = w.chronicle.facts("testimony")[0]
        assert (t["witness"], t["tier"], t["id"]) == (ANDRE, "far", w.chronicle.facts("inquiry")[0]["id"])
        assert t["told"] and not w.witnesses.inquiries
        notes = [e.content for e in w.dialogue.conversations[LOBART].memory.entries]
        assert any("Ты расспросил при встрече: Андре" in n for n in notes)
        # on its mind: it may come up to the hero about it, until it has spoken of it
        top = concerns.top(LOBART)
        assert (
            top is not None
            and top.key == f"witness:{t['id']}"
            and "а он говорил тебе «всё утро был в городе»" in top.ru
        )
        w.chronicle.record(
            {"fact": "initiative", "npc": LOBART, "occasion": top.key, "day": 1, "hour": 18, "minute": 20}
        )
        assert concerns.top(LOBART) is None
        assert w.seen.tools(LOBART) != []  # catch_lie: a witness saw him


async def test_without_budget_the_witness_is_silent_and_the_answer_is_a_remark():
    fake = FakeGame(branch="A")
    llm = lobart(("город", ASK_HILDA), hilda=[("спрашивает тебя", FakeStep("Видела."))])
    async with connected(fake) as link:
        w = World(link, fake, llm, budget=Budget(per_npc_per_min=0, total_per_min=0))
        w.seen.saw(hero_seen_by(HILDA, "Хильда", AT_8))
        w.seen.saw(hero_seen_by(HILDA, "Хильда", AT_8 + 5))
        w.dialogue.open(LOBART, "Лобарт")
        await w.dialogue.say_as_player(LOBART, LIE)
        await _until(lambda: w.chronicle.facts("testimony") != [], 10)
        await _until(
            lambda: any("Ответ:" in e.content for e in w.dialogue.conversations[LOBART].memory.entries), 5
        )

    assert llm.others.calls == []  # type: ignore[union-attr]
    assert not any("Ответ:" in str(c[1][-1].content) for c in llm.calls)  # a remark, not a turn
    assert w.chronicle.facts("testimony")[0]["told"]


def test_a_lie_told_by_a_witness_is_retold_so():
    lie = AmendsLie(
        "SAID_X_1", "caught", LOBART, "Лобарт", "всё утро был в городе", said=True, witness_name="Хильда"
    )
    assert (
        "сказал ему «всё утро был в городе», а тот узнал от человека по имени Хильда, что это неправда"
        in lie.words()
    )
    ref = LieRef(f"caught:{LOBART}:SAID_X_1", "всё утро был в городе", LOBART, "Лобарт", said=True)
    assert ref.words(LOBART) == "обманул тебя: сказал «всё утро был в городе», а это неправда"
    assert ref.words(HILDA) == (
        "обманул человека по имени Лобарт: сказал «всё утро был в городе», а это неправда"
    )


async def test_the_asker_walks_to_a_witness_and_back():
    fake = FakeGame(branch="A")
    llm = lobart(("Ответ:", CATCH), ("город", ASK_HILDA), hilda=[("спрашивает тебя", FakeStep("Видела."))])
    async with connected(fake) as link:
        w = World(link, fake, llm)
        fake.npc_states[HILDA]["distance"] = 3000  # 30 m: a walk away
        for t in range(AT_8, AT_8 + 11, 5):
            w.seen.saw(hero_seen_by(HILDA, "Хильда", t))
        w.dialogue.open(LOBART, "Лобарт")
        await w.dialogue.say_as_player(LOBART, LIE)
        await _until(lambda: w.chronicle.facts("caught") != [], 10)

    acts = [
        (m["npc"], m["action"], m.get("args", {}).get("npc")) for m in fake.received if m["type"] == "action"
    ]
    there, back = acts.index((LOBART, "go_to_npc", HILDA)), acts.index((LOBART, "go_to_npc", "PLAYER"))
    assert there < back and not any(a[0] == HILDA and a[1] == "go_to_npc" for a in acts)  # she stays put
    assert w.chronicle.facts("testimony")[0]["tier"] == "walk"
    assert (LOBART, "witness") in w.held


async def test_a_witness_cut_off_by_the_hero_still_gets_its_answer_to_the_asker():
    fake = FakeGame(branch="A")
    slow = FakeStep("Видела его утром на пастбище, у самых ворот, с утра пораньше, а потом ещё раз у реки.")
    llm = lobart(("Ответ:", CATCH), ("город", ASK_HILDA), hilda=[("спрашивает тебя", slow)])
    async with connected(fake) as link:
        w = World(link, fake, llm)
        for t in range(AT_8, AT_8 + 11, 5):
            w.seen.saw(hero_seen_by(HILDA, "Хильда", t))
        w.dialogue.open(LOBART, "Лобарт")
        await w.dialogue.say_as_player(LOBART, LIE)
        await _until(
            lambda: (c := w.dialogue.conversations.get(HILDA)) is not None and c.reply is not None, 10
        )
        w.dialogue.conversations[HILDA].reply.cancel()  # the hero spoke to her meanwhile
        await _until(lambda: w.chronicle.facts("caught") != [], 10)
    assert w.chronicle.facts("caught")[0]["witness"] == HILDA  # Lobart still heard the answer and reacted


async def test_the_answer_waits_for_the_askers_turn_under_way_and_keeps_catch_lie(monkeypatch):
    # 2026-10-29 live: Lobart was in another turn when Hilda's answer came; the reaction cue went to `pending`
    # of that turn (set_intent only) and the turn with catch_lie never came
    monkeypatch.setattr(gothic_ai.dialogue, "line_duration", lambda text: 0.2)

    class Thinks(ClaimScript):
        """Lobart thinks a second over Maleth's shout: his turn is under way when Hilda answers."""

        async def stream(self, **kw):
            if "по имени Лобарт" in kw["instructions"] and "Малет кричит" in str(kw["messages"][-1].content):
                await asyncio.sleep(1.0)
            async for event in super().stream(**kw):
                yield event

    fake = FakeGame(branch="A")
    saw = FakeStep("Видела его утром на пастбище.")
    hilda = ClaimScript([("спрашивает тебя", saw)], [], "Ну.", only="Хильда")
    llm = Thinks(
        [("Ответ:", CATCH), ("Малет кричит", FakeStep("Погоди, Малет.")), ("город", FakeStep("Хм."))],
        [], "Ну?", only="Лобарт", others=hilda,
    )
    async with connected(fake) as link:
        w = World(link, fake, llm)
        for t in range(AT_8, AT_8 + 21, 5):
            w.seen.saw(hero_seen_by(HILDA, "Хильда", t))
        turns: list[dict] = []
        event_turn = w.dialogue.event_turn

        def recorded(npc, cue, *args, **kw):
            turns.append({"npc": npc, **kw})
            return event_turn(npc, cue, *args, **kw)

        w.dialogue.event_turn = recorded  # type: ignore[method-assign]
        conv = w.dialogue.open(LOBART, "Лобарт")
        await w.dialogue.say_as_player(LOBART, LIE)
        await _until(lambda: conv.reply is None or conv.reply.done(), 5)
        await w.witnesses.prepare(LOBART)
        c = next(c for c in w.witnesses._near[LOBART] if c.npc == HILDA)
        assert w.dialogue.event_turn(LOBART, "[Малет кричит тебе с поля.]") is not None
        text, success = await w.witnesses._ask(conv, c, "всё утро был в городе", AT_8 + 90)
        await _until(lambda: w.chronicle.facts("caught") != [], 10)

    assert success and not text
    cues = [str(call[1][-1].content) for call in llm.calls]
    reaction = next(i for i, cue in enumerate(cues) if "Ответ:" in cue)
    assert reaction > next(i for i, cue in enumerate(cues) if "Малет кричит" in cue)
    assert "catch_lie" in [x.name for x in llm.calls[reaction][2]]
    assert "вызовом catch_lie" in cues[reaction]
    # it must decide by a call: catch_lie, or set_intent if it believes him
    assert {"npc": LOBART, "tools": ("catch_lie", "set_intent"), "kind": "witness", "act": True} in turns
    assert not w.witnesses.asking(LOBART)  # the answer is in: group.py may give him answer turns again
    assert w.chronicle.facts("caught")[0]["witness"] == HILDA


async def test_a_meeting_going_on_now_is_no_meeting_to_ask_at():
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        w = World(link, fake, lobart())
        fake.hour, fake.minute = 18, 30  # Lobart and Andre meet 18:00–19:00: they are far apart all the same
        await w.witnesses.prepare(LOBART)
        andre = next(c for c in w.witnesses._near[LOBART] if c.npc == ANDRE)
        assert andre.tier == "far" and andre.meets == 2 * DAY + 18 * 60  # tomorrow's


async def test_a_far_answer_leaves_the_mind_with_time_or_once_he_is_caught_for_it():
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        w = World(link, fake, lobart())
        concerns = Concerns(link, w.chronicle, Deals(link, w.dialogue, w.chronicle))
        concerns.extra.append(w.witnesses.concerns)
        w.witnesses.on_change.append(concerns.changed)
        w.seen.saw(hero_seen_by(ANDRE, "Андре", AT_8))
        w.seen.saw(hero_seen_by(ANDRE, "Андре", AT_8 + 5))
        for i, said in enumerate(("всё утро был в городе", "с утра сидел в таверне")):
            w.witnesses._inquire(
                Inquiry(f"Q{i}", LOBART, "Лобарт", ANDRE, "Андре", said, DAY + 9 * 60 + 30, DAY + 18 * 60)
            )
        fake.hour, fake.minute = 18, 5
        await w.witnesses.tick()

        def witness() -> list[str]:
            return [c.key for c in concerns.of(LOBART) if c.key.startswith("witness:")]

        assert witness() == ["witness:Q0", "witness:Q1"]
        # caught by С1 in other words: that answer is settled
        w.chronicle.record(
            {"fact": "caught", "npc": LOBART, "title": "был в городе", "said": True, "testimony": "Q1"}
        )
        assert witness() == ["witness:Q0"]  # (and a caught lie of its own now: concerns.lied)
        # half a day later the other one is off its mind too
        fake.day, fake.hour = 2, 7
        await w.witnesses.tick()
        assert witness() == []
