import asyncio
from types import SimpleNamespace
from typing import Any

import pytest

from gothic_ai.bodies import BodyOutcome
from gothic_ai.guide import Guide, Way
from gothic_ai.llm import ToolCall
from gothic_ai.llm.fake import FakeBackend, FakeStep
from gothic_ai.meetings import DAY, Meetings, MeetTerm
from gothic_ai.memory import MemoryStore
from gothic_ai.protocol import RosterEntry
from gothic_ai.storyguard import StoryGuard
from gothic_ai.trust import Trust
from tests.fake_game import FakeGame
from tests.test_deals import _setup, _shown
from tests.test_dialogue import XARDAS, _until
from tests.test_guide import BOOK, MALETH, _Dialogue, _Link, _snap, _Talk


class _Chronicle:
    def __init__(self, facts: list[dict[str, Any]] | None = None) -> None:
        self.all = list(facts or [])

    def record(self, fact: dict[str, Any]) -> None:
        self.all.append(fact)

    def facts(self, kind: str | None = None) -> list[dict[str, Any]]:
        return [f for f in self.all if kind is None or f.get("fact") == kind]


class _Bodies:
    """The npc's walk to the meeting: the test says how it ends."""

    def __init__(self) -> None:
        self.acts: list[tuple[str, dict[str, Any]]] = []
        self.walk: asyncio.Future[str] | None = None

    async def act(self, npc: str, action: str, /, **args: Any) -> BodyOutcome:
        self.acts.append((action, args))
        self.walk = asyncio.get_running_loop().create_future()
        return BodyOutcome("n1", npc, await self.walk)  # type: ignore[arg-type]


class _MeetTalk(_Talk):
    def __init__(self) -> None:
        super().__init__()
        self.partner = None
        self.begun: list[str] = []

    def begin(self, npc: str, name: str, posture: str | None = None) -> bool:
        self.begun.append(npc)
        self.partner = SimpleNamespace(npc=npc)
        return True


class _MeetDialogue(_Dialogue):
    def __init__(self) -> None:
        super().__init__()
        self.memory: list[str] = []

    def remember(self, npc: str, name: str, text: str) -> None:
        self.memory.append(text)


def _desk(facts: list[dict[str, Any]] | None = None):
    link, talk, dialogue, bodies = _Link(), _MeetTalk(), _MeetDialogue(), _Bodies()
    link.snapshots[MALETH] = _snap((0, 0, 0), (300, 0, 0))  # day 2 (1 from 0), 19:05
    effects: list[dict[str, Any]] = []

    async def story_effect(npc: str, fx: list[dict[str, Any]], *a: Any, **kw: Any) -> None:
        effects.extend(fx)

    link.story_effect = story_effect  # type: ignore[attr-defined]
    chronicle = _Chronicle(facts)
    guide = Guide(link, dialogue, talk, BOOK, StoryGuard(None), chronicle)  # type: ignore[arg-type]
    meetings = Meetings(link, dialogue, talk, guide, chronicle, bodies)  # type: ignore[arg-type]
    return meetings, link, talk, dialogue, bodies, chronicle, effects


NOW = 2 * DAY + 19 * 60 + 5  # _snap: day 2, 19:05
DAWN = {"where": "FARM1_MILL", "day": 4, "hour": 5, "minute": 0, "hours": 2}  # tomorrow, 05:00–07:00


async def test_where_and_when_a_meeting_may_be():
    meetings, link, *_ = _desk()
    snap = link.snapshots[MALETH]
    term, why = await meetings.term(MALETH, DAWN, snap)
    assert term is not None and why == ""
    assert term.text() == "у мельницы фермы Лобарта, день 4, 05:00–07:00"
    assert (term.start, term.end) == (3 * DAY + 5 * 60, 3 * DAY + 7 * 60)
    assert await meetings.term(MALETH, {**DAWN, "where": ""}, snap) == (None, "")  # no meeting
    past = (await meetings.term(MALETH, {**DAWN, "day": 3, "hour": 18}, snap))[1]
    assert past == "это время уже прошло или вот-вот наступит: сейчас день 3, 19:05"
    assert (await meetings.term(MALETH, {**DAWN, "day": 9}, snap))[1].startswith("так далеко вперёд")
    assert (await meetings.term(MALETH, {**DAWN, "hours": 6}, snap))[1] == "ждать можно от 1 до 4 часов"
    plot = (await meetings.term("NONE_100_XARDAS", DAWN, snap))[1]
    assert plot == "тебе нельзя уходить отсюда (сюжетный: только слова)"
    assert term is not None
    meetings.add(MALETH, "Малет", term, "D1")
    taken = (await meetings.term(MALETH, {**DAWN, "hour": 6}, snap))[1]
    assert taken == "в это время у тебя уже встреча: у мельницы фермы Лобарта, день 4, 05:00–07:00"


async def test_it_is_there_by_the_hour_and_speaks_first_when_he_comes():
    meetings, link, talk, dialogue, bodies, chronicle, effects = _desk()
    term, _ = await meetings.term(MALETH, DAWN, link.snapshots[MALETH])
    assert term is not None
    m = meetings.add(MALETH, "Малет", term, "D1")
    assert chronicle.facts("meet")[0]["place"] == "FARM1_MILL" and m.lead_min == 45  # 60 m: 30 min + 15
    await _until(lambda: effects != [])
    assert effects[0] == {"do": "journal", "topic": "Встреча: Малет, день 4",
                          "text": "Малет будет ждать меня у мельницы фермы Лобарта, день 4, 05:00–07:00.",
                          "status": "LOG_RUNNING"}  # fmt: skip
    assert meetings.situation(MALETH) == (
        "Ты договорился с героем о встрече: у мельницы фермы Лобарта, день 4, 05:00–07:00."
    )

    meetings.tick(term.start - 50)  # not yet
    assert m.state == "agreed" and bodies.acts == []
    meetings.tick(term.start - 45)  # it sets off
    await _until(lambda: bodies.walk is not None)
    action, args = bodies.acts[0]
    assert action == "start_activity" and args["state"] == "ZS_STAND_ARMSCROSSED"
    assert args["waypoint"] == "NW_FARM1_MILL_01" and args["minutes"] == 165 and args["arrive"]
    assert meetings.allow(SimpleNamespace(id=MALETH), "plan_2") == "на встрече с героем"  # type: ignore[arg-type]
    assert bodies.walk is not None
    bodies.walk.set_result("arrived")
    await _until(lambda: m.state == "there")
    assert "Ты ждёшь героя на встрече: у мельницы фермы Лобарта, до 07:00." == meetings.situation(MALETH)

    talk.people[MALETH] = RosterEntry(id=MALETH, name="Малет", distance=600)
    meetings.tick(term.start + 40)  # 05:40, he comes
    assert m.state == "kept" and chronicle.facts("meet_kept")[0]["meet"] == m.id
    await _until(lambda: talk.begun == [MALETH])
    assert dialogue.cues[-1] == (
        "[Герой пришёл на встречу, как вы договорились: у мельницы фермы Лобарта, день 4, 05:00–07:00. "
        "Заговори с ним первым.]"
    )
    await _until(lambda: len(effects) == 2)
    assert effects[1]["status"] == "LOG_SUCCESS"


async def test_he_does_not_come_his_word_is_broken_and_it_tells_him_so_once():
    meetings, link, talk, dialogue, bodies, chronicle, effects = _desk()
    term, _ = await meetings.term(MALETH, DAWN, link.snapshots[MALETH])
    assert term is not None
    m = meetings.add(MALETH, "Малет", term, "D1")
    meetings.tick(term.start - 45)
    await _until(lambda: bodies.walk is not None)
    assert bodies.walk is not None
    bodies.walk.set_result("arrived")
    await _until(lambda: m.state == "there")
    meetings.tick(term.end)  # 07:00, nobody came
    assert m.state == "missed"
    broken = chronicle.facts("broken")[0]
    assert broken["promise"] == "meet" and broken["meet"] == m.id and broken["day"] == 3
    assert dialogue.memory[-1].startswith("Герой не пришёл на встречу, о которой вы договорились")
    assert Trust(chronicle).of(MALETH).broke == [  # type: ignore[arg-type]
        "обещал прийти на встречу (у мельницы фермы Лобарта, день 4, 05:00–07:00), а не пришёл"
    ]
    assert [c.key for c in meetings.concerns(MALETH)] == [f"meet_missed:{m.id}"]
    assert "не пришёл на встречу" in (meetings.situation(MALETH) or "")  # nobody talks to it: not told yet
    talk.partner = SimpleNamespace(npc=MALETH)
    assert "Он не сдержал слово" in (meetings.situation(MALETH) or "")  # told now
    assert meetings.situation(MALETH) is None and meetings.concerns(MALETH) == []
    assert chronicle.facts("meet_raised") == [{"fact": "meet_raised", "meet": m.id, "npc": MALETH}]


async def test_it_does_not_get_there_itself_no_word_of_his_is_broken():
    meetings, link, _, dialogue, bodies, chronicle, _ = _desk()
    term, _ = await meetings.term(MALETH, DAWN, link.snapshots[MALETH])
    assert term is not None
    m = meetings.add(MALETH, "Малет", term, "D1")
    for i in range(3):  # cut off three times: a fight, a dialog...
        meetings.tick(term.start - 45 + i * 10)
        await _until(lambda: m.state == "going" and bodies.walk is not None and not bodies.walk.done())
        assert bodies.walk is not None
        bodies.walk.set_result("gone")
        await _until(lambda: m.state == "agreed")
    meetings.tick(term.start + 60)
    assert m.state == "agreed" and len(bodies.acts) == 3  # it gave up
    meetings.tick(term.end)
    assert m.state == "failed" and chronicle.facts("broken") == []
    assert chronicle.facts("meet_failed")[0]["meet"] == m.id


async def test_he_sleeps_till_the_hour_it_is_there_at_once():
    meetings, link, _, _, bodies, _, _ = _desk()
    term, _ = await meetings.term(MALETH, DAWN, link.snapshots[MALETH])
    assert term is not None
    m = meetings.add(MALETH, "Малет", term, "D1")  # the clock from the snapshot: 19:05
    t0 = asyncio.get_running_loop().time()
    meetings.read(NOW + 2, t0 + 10)  # the clock as it goes: no leap
    assert m.state == "agreed" and not m.place
    meetings.read(term.start - 20, t0 + 15)  # he slept till 04:40: the engine put it at its routine's spot
    await _until(lambda: bodies.walk is not None)
    assert bodies.acts[0][1]["place"] is True and not m.place  # there at once
    assert bodies.walk is not None
    bodies.walk.set_result("rejected")  # still held for the talk: it did not go at all
    await _until(lambda: m.state == "agreed")
    meetings.read(term.start - 14, t0 + 40)  # five minutes on: again, still there at once
    await _until(lambda: len(bodies.acts) == 2)
    assert bodies.acts[1][1]["place"] is True
    bodies.walk.set_result("arrived")
    await _until(lambda: m.state == "there")
    meetings.read(term.start + 60, t0 + 45)  # he slept again: off its spot again, put back
    await _until(lambda: len(bodies.acts) == 3)
    assert bodies.acts[2][1]["place"] is True and bodies.acts[2][1]["minutes"] == 60


async def test_he_sleeps_through_it_his_word_is_broken():
    meetings, link, _, _, bodies, chronicle, _ = _desk()
    term, _ = await meetings.term(MALETH, DAWN, link.snapshots[MALETH])
    assert term is not None
    m = meetings.add(MALETH, "Малет", term, "D1")
    # he slept through it: it never set off before his eyes, but the world went on and it waited there
    meetings.read(term.end + 60, asyncio.get_running_loop().time())
    assert m.state == "missed" and chronicle.facts("broken")[0]["meet"] == m.id and bodies.acts == []


async def test_a_load_brings_the_meetings_back():
    meetings, link, *_ = _desk()
    term, _ = await meetings.term(MALETH, DAWN, link.snapshots[MALETH])
    assert term is not None
    m = meetings.add(MALETH, "Малет", term, "D1")
    facts = [*meetings.chronicle.facts(), {"fact": "broken", "promise": "meet", "meet": m.id, "npc": MALETH}]
    again, *_ = _desk(facts)
    back = again.meetings[m.id]
    assert back.state == "missed" and back.term == term and back.lead_min == m.lead_min


@pytest.mark.parametrize("asleep,hero_present", [(False, False), (False, True), (True, False)])
async def test_dishonest_npc_never_sets_off_and_its_absence_does_not_break_the_heros_word(
    asleep: bool, hero_present: bool
):
    intent = {
        "fact": "npc_intent",
        "npc": MALETH,
        "deal": "D1",
        "keeps": False,
        "how": ["meet"],
        "why": "dishonesty",
    }
    meetings, link, talk, _, bodies, chronicle, _ = _desk([intent])
    term, _ = await meetings.term(MALETH, DAWN, link.snapshots[MALETH])
    assert term is not None
    meeting = meetings.add(MALETH, "Малет", term, "D1")
    meetings.tick(term.start - meeting.lead_min)
    assert meeting.state == "agreed" and bodies.acts == [] and chronicle.facts("npc_broken") == []
    meetings.tick(term.start - 1)
    assert chronicle.facts("npc_broken") == []
    if hero_present:
        talk.people[MALETH] = RosterEntry(id=MALETH, name="Малет", distance=300)
    if asleep:
        meetings.read(term.end + 60, asyncio.get_running_loop().time())
    else:
        meetings.tick(term.start)
    assert meeting.state == "no_show" and bodies.acts == []
    broken = chronicle.facts("npc_broken")[0]
    assert (broken["npc"], broken["npc_name"], broken["deal"], broken["how"], broken["meet"]) == (
        MALETH,
        "Малет",
        "D1",
        "no_show",
        meeting.id,
    )
    assert not broken.get("witnesses")
    assert chronicle.facts("broken") == [] and chronicle.facts("meet_failed") == []
    assert meetings.concerns(MALETH) == []
    meetings.tick(term.end + 120)
    assert len(chronicle.facts("npc_broken")) == 1

    again, _, _, _, again_bodies, again_chronicle, _ = _desk(chronicle.facts())
    assert again.meetings[meeting.id].state == "no_show"
    again.tick(term.end + 180)
    assert again_bodies.acts == [] and len(again_chronicle.facts("npc_broken")) == 1
    assert again_chronicle.facts("broken") == [] and again_chronicle.facts("meet_failed") == []


async def test_combined_deals_unpaid_reward_does_not_hide_the_later_no_show_on_reload():
    intent = {
        "fact": "npc_intent",
        "npc": MALETH,
        "deal": "D1",
        "keeps": False,
        "how": ["contract", "meet"],
        "why": "dishonesty",
    }
    meetings, link, _, _, bodies, chronicle, _ = _desk([intent])
    term, _ = await meetings.term(MALETH, DAWN, link.snapshots[MALETH])
    assert term is not None
    meeting = meetings.add(MALETH, "Малет", term, "D1")
    unpaid = {
        "fact": "npc_broken",
        "npc": MALETH,
        "deal": "D1",
        "how": "unpaid",
        "contract": "C1",
        "witnesses": ["HEARER"],
    }
    chronicle.record(unpaid)

    meetings.tick(term.start)
    meetings.tick(term.end)
    assert meeting.state == "no_show" and bodies.acts == []
    breaches = chronicle.facts("npc_broken")
    assert breaches[0] == unpaid
    assert [(f["deal"], f["how"]) for f in breaches] == [("D1", "unpaid"), ("D1", "no_show")]
    assert breaches[1]["meet"] == meeting.id and not breaches[1].get("witnesses")

    again, _, _, _, again_bodies, again_chronicle, _ = _desk(chronicle.facts())
    assert again.meetings[meeting.id].state == "no_show"
    again.tick(term.end + 60)
    again.tick(term.end + 120)
    assert again_chronicle.facts("npc_broken") == breaches and again_bodies.acts == []
    assert again_chronicle.facts("broken") == [] and again_chronicle.facts("meet_failed") == []


async def test_a_meeting_is_a_term_of_a_deal_in_its_window():
    fake = FakeGame(branch="A")
    offer = FakeStep("Встретимся на рассвете.", [ToolCall("propose_deal", {"meet": DAWN}, "c1")])
    link_cm, dialogue, chronicle, deals = await _setup(
        fake, FakeBackend(script=[offer], delay=0), MemoryStore(":memory:")
    )
    mill = BOOK.get("FARM1_MILL")
    assert mill is not None
    term = MeetTerm(Way(mill, mill.waynet, (6000.0, 0.0, 0.0)), 3 * DAY + 300, 3 * DAY + 420)

    async def term_of(npc: str, raw: Any, snap: Any) -> tuple[MeetTerm | None, str]:
        return term, ""

    added: list[tuple[Any, ...]] = []
    deals.meetings = SimpleNamespace(term=term_of, add=lambda *a: added.append(a))  # type: ignore[assignment]
    try:
        await dialogue.say_as_player(XARDAS, "Давай встретимся.")
        await _until(lambda: len(_shown(fake)) == 1)
        shown = _shown(fake)[0]
        assert shown["hero_gives"] == [
            {"item": "", "count": 0, "text": "Встреча: у мельницы фермы Лобарта, день 4, 05:00–07:00"}
        ]
        terms = deals.open[XARDAS].terms()
        assert terms.endswith(
            "герой обещает прийти на встречу — у мельницы фермы Лобарта, день 4, 05:00–07:00"
        )
        await fake.send(type="deal_accept", deal=shown["deal"], version=1)
        await _until(lambda: added != [])
        assert added == [(XARDAS, "Ксардас", term, shown["deal"])]
    finally:
        await link_cm.__aexit__(None, None, None)
