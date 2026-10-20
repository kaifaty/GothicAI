import asyncio
import time
from collections.abc import AsyncIterator
from contextlib import asynccontextmanager
from dataclasses import dataclass
from pathlib import Path
from types import SimpleNamespace
from typing import Any

import pytest

from gothic_ai import dialogue as dialogue_module
from gothic_ai import gossip, social
from gothic_ai.bodies import Bodies
from gothic_ai.brain.planner import NpcRecord
from gothic_ai.budget import Budget
from gothic_ai.chronicle import Chronicle
from gothic_ai.crime_react import CrimeReact
from gothic_ai.crimes import Crimes
from gothic_ai.dialogue import DialogueManager, SceneLine
from gothic_ai.gossip import INSTRUCTIONS, GossipDesk
from gothic_ai.journal import Journal
from gothic_ai.llm import FakeBackend, FakeStep, ToolCall
from gothic_ai.memory import MemoryStore
from gothic_ai.perception import PerceptionDesk
from gothic_ai.persona import PersonaProvider
from gothic_ai.protocol import PROTOCOL_VERSION, NpcView, RosterEntry
from gothic_ai.scenarios.conversation import _talked_to
from gothic_ai.social import DAY, Rumor, Rumors, Social
from gothic_ai.storyguard import StoryGuard
from gothic_ai.talk import TalkDesk
from gothic_ai.trace import TraceWriter, read
from gothic_ai.trust import Trust
from tests.fake_game import FakeGame, connected
from tests.fakes import Clock, FakeDecider
from tests.test_dialogue import _until
from tests.test_director import HILDA, LOBART, QUEST, VINO
from tests.test_planner import make
from tests.test_social import PEOPLE, _h
from tests.test_talk import person

AT = DAY + _h(9) + 1  # day 1, 09:01: Lobart and Hilda's farm day goes on
TITLE = "Убрать поле репы"
KEY = f"caught:{LOBART}:{QUEST}"
NEWS = f"герой обманул человека по имени Лобарт — уверял, что сделал «{TITLE}», а это неправда"
THREE = [
    ("Лобарт", "Слыхала? Этот бродяга мне про репу наврал, глазом не моргнул.", "angry"),
    ("Хильда", "Да ну? А с виду смирный.", "neutral"),
    ("Лобарт", "Смирный… Глаз с него не спускай.", "neutral"),
]


def _lines(*lines: tuple[str, str, str], quiet: bool = True) -> FakeStep:
    """The model's answer: a call of `lines`."""
    said = [{"speaker": who, "text": text, "mood": mood} for who, text, mood in lines]
    return FakeStep(tool_calls=[ToolCall("lines", {"lines": said, "quiet": quiet, "reason": "он зол"})])


@dataclass
class G:
    fake: FakeGame
    llm: FakeBackend
    dialogue: DialogueManager
    chronicle: Chronicle
    talk: TalkDesk
    rumors: Rumors
    perception: PerceptionDesk
    budget: Budget
    guard: StoryGuard
    bodies: Bodies
    trace: TraceWriter
    desk: GossipDesk
    journal: list[dict[str, Any]]

    @property
    def rumor(self) -> Rumor:
        return self.rumors.rumors[KEY]


@asynccontextmanager
async def _gossip(
    tmp_path: Path,
    llm: FakeBackend | None = None,
    *,
    budget: Budget | None = None,
    protocol: int = PROTOCOL_VERSION,
) -> AsyncIterator[G]:
    """The parts as cli.py wires them, around the fake game (branch A). Lobart caught the hero lying; the gate
    opens only his pass to Hilda. The budget's clock stands: nothing refills."""
    fake, store = FakeGame(branch="A", protocol=protocol), MemoryStore(":memory:")
    fake.auto_idle = True
    llm = llm or FakeBackend(script=[_lines(*THREE)], delay=0)
    journal: list[dict[str, Any]] = []
    trace = TraceWriter(tmp_path)
    async with connected(fake) as link:
        dialogue = DialogueManager(
            link, llm, PersonaProvider(None), memory=store, journal=Journal(journal.append), trace=trace
        )
        dialogue.scene_pace = lambda text: 0.05
        chronicle = Chronicle(link, store)
        trust = Trust(chronicle)
        dialogue.extensions.append(trust)
        talk = TalkDesk(link, dialogue)
        rumors = Rumors(link, dialogue, chronicle)
        rumors.social = Social(PEOPLE)
        perception = PerceptionDesk(link, social=lambda: rumors.social)
        budget = budget if budget is not None else Budget(per_npc_per_min=2, clock=Clock())
        guard, bodies = StoryGuard(None), Bodies(link)
        desk = GossipDesk(
            link,
            dialogue,
            talk,
            rumors,
            perception,
            budget,
            chronicle,
            trust,
            guard,
            bodies,
            trace=trace,
            journal=dialogue.journal,
        )
        chronicle.record(
            {"fact": "caught", "quest": QUEST, "npc": LOBART, "name": "Лобарт", "title": TITLE, "day": 1}
        )
        rumors.gate = lambda r, a, b: (a, b) == (LOBART, HILDA)
        yield G(
            fake,
            llm,
            dialogue,
            chronicle,
            talk,
            rumors,
            perception,
            budget,
            guard,
            bodies,
            trace,
            desk,
            journal,
        )


@pytest.fixture(autouse=True)
def quick(monkeypatch):
    """The breath between two voices and the sight tier's chatter, shorter."""
    monkeypatch.setattr(dialogue_module, "SCENE_GAP_S", 0.05)
    monkeypatch.setattr(gossip, "SVM_GAP_S", 0.05)


def _look(npc: str, name: str, metres: float, **extra: Any) -> dict[str, Any]:
    return {"id": npc, "name": name, "distance": metres * 100, **extra}


def _view(
    npc: str, name: str, *sees: dict[str, Any], body: str = "stand", seen: bool = True
) -> dict[str, Any]:
    hero = {"id": "PLAYER", "name": "Я", "distance": 900, "seen": seen}
    return {"npc": npc, "name": name, "body": body, "hero": hero, "sees": list(sees)}


async def _around(g: G, *people: dict[str, Any], views: tuple[dict[str, Any], ...] = ()) -> None:
    """Who is around the hero (npc_roster) and what they see (npc_views), once the server knows it."""
    await g.fake.roster(*people)
    want = {p["id"]: RosterEntry.model_validate({"activity": "routine", **p}) for p in people}
    if views:
        await g.fake.push_views(*views)
    await _until(
        lambda: (
            g.talk.people == want
            and all(g.perception.view(v["npc"]) == NpcView.model_validate(v) for v in views)
        )
    )


async def _together(
    g: G,
    lobart_m: float = 8,
    hilda_m: float = 10,
    gap_m: float = 4,
    *,
    seen: bool = True,
    lobart: dict[str, Any] | None = None,
    hilda: dict[str, Any] | None = None,
    hilda_body: str = "stand",
    lobart_body: str = "stand",
    others: tuple[dict[str, Any], ...] = (),
) -> None:
    """Lobart and Hilda near the hero, `gap_m` apart (Lobart sees her)."""
    await _around(
        g,
        person(LOBART, "Лобарт", lobart_m, seen, **{"state": "ZS_STAND_GUARDING", **(lobart or {})}),
        person(HILDA, "Хильда", hilda_m, seen, **{"state": "ZS_STAND_ARMSCROSSED", **(hilda or {})}),
        *others,
        views=(
            _view(LOBART, "Лобарт", _look(HILDA, "Хильда", gap_m, body=hilda_body), body=lobart_body),
            _view(HILDA, "Хильда", body=hilda_body),
        ),
    )


def _tick(g: G, at: int = AT) -> int:
    """A live tick of the rumors (a game minute): how many passes."""
    g.rumors.clock = at
    return g.rumors.spread(at - 1, at)


async def _over(g: G, state: str = "done", timeout: float = 5.0) -> gossip.Scene:
    """Until the scene ended (as `state`) and nobody is held for it any more."""
    await _until(
        lambda: g.desk.last is not None and g.desk.last.state == state and g.desk.last.released, timeout
    )
    await _until(lambda: not g.fake.attached and not g.talk.held, timeout)
    assert g.desk.last is not None
    return g.desk.last


def _rows(g: G, kind: str) -> list[dict[str, Any]]:
    """The trace's rows of a type."""
    path = g.trace.path()
    return [r for r in read(path) if r["type"] == kind] if path.exists() else []


def _history(g: G, npc: str) -> list[str]:
    conv = g.dialogue.conversations.get(npc)
    return [m.content for m in conv.history] if conv is not None else []


def _notes(g: G, npc: str) -> list[str]:
    """What the scene left in the npc's memory."""
    return [c for c in _history(g, npc) if "о герое" in c and "Слух" not in c]


def _actions(g: G) -> list[tuple[str, str, dict[str, Any]]]:
    return [(m["npc"], m["action"], m["args"]) for m in g.fake.received if m["type"] == "action"]


def _types(g: G, kind: str) -> list[str]:
    return [m["npc"] for m in g.fake.received if m["type"] == kind]


async def test_scene_instead_of_a_silent_pass(tmp_path):
    async with _gossip(tmp_path) as g:
        await _together(g)
        assert _tick(g) == 1
        # the fact first, at once: the words come second
        (fact,) = g.chronicle.facts("rumor")
        assert (fact["from"], fact["to"], fact["hops"], fact["how"]) == (LOBART, HILDA, 1, "talked")
        assert fact["seen_by"] == ["PLAYER"] and "where" in fact
        assert not g.fake.says and g.desk.scenes == 1 and g.desk.floor_busy()
        s = await _over(g)

    # both held, then free again
    assert _types(g, "attach") == [LOBART, HILDA] and sorted(_types(g, "detach")) == [LOBART, HILDA]
    # three lines in turn, each to the other
    says = [m for m, _ in g.fake.says]
    assert [(m["npc"], m["to"], m["text"]) for m in says] == [
        (LOBART, HILDA, THREE[0][1]),
        (HILDA, LOBART, THREE[1][1]),
        (LOBART, HILDA, THREE[2][1]),
    ]
    assert [m.get("mood") for m in says] == ["angry", "neutral", "neutral"]
    # no turn of ours before the first line (say.to turns the speaker); then the one spoken to turns
    first_say = next(i for i, m in enumerate(g.fake.received) if m["type"] == "say")
    first_act = next(i for i, m in enumerate(g.fake.received) if m["type"] == "action")
    assert first_act > first_say
    assert _actions(g) == [
        (HILDA, "turn_to", {"npc": LOBART}),
        (LOBART, "turn_to", {"npc": HILDA}),
        (HILDA, "turn_to", {"npc": LOBART}),
    ]
    # one call, with the two names for speakers
    ((instructions, request, tools),) = g.llm.calls
    assert instructions == INSTRUCTIONS and g.llm.cache_keys == ["gossip"]
    assert tools[0].parameters["properties"]["lines"]["items"]["properties"]["speaker"]["enum"] == [
        "Лобарт",
        "Хильда",
    ]
    brief = request[0].content
    assert "Рассказчик — Лобарт." in brief and "Слушатель — Хильда." in brief
    assert f"Новость (знает Лобарт, не знает Хильда): {NEWS}" in brief
    assert "Лобарт сам уличил героя сегодня." in brief
    assert "- он уже лгал тебе: «Убрать поле репы»" in brief  # what Lobart knows of his word
    assert "Герой стоит в 8 м от них. Лобарт видит его; Хильда видит его." in brief
    # both remember what was said, never as their own replies
    lines = [text for _, text, _ in THREE]
    assert _notes(g, LOBART) == [
        f"[Ты рассказал о герое человеку по имени Хильда (герой стоял в 8 м): Ты: «{lines[0]}» — "
        f"Хильда: «{lines[1]}» — Ты: «{lines[2]}»]"
    ]
    assert _notes(g, HILDA) == [
        f"[Лобарт рассказал тебе о герое: Лобарт: «{lines[0]}» — Ты: «{lines[1]}» — Лобарт: «{lines[2]}»]"
    ]
    assert not any(
        m.role == "assistant" for npc in (LOBART, HILDA) for m in g.dialogue.conversations[npc].history
    )
    # the trace, the journal
    (row,) = _rows(g, "scene")
    assert (row["status"], row["tier"], row["npcs"], row["rumor"], row["quiet"]) == (
        "done",
        "words",
        [LOBART, HILDA],
        KEY,
        True,
    )
    assert [x["text"] for x in row["said"]] == lines and row["brief"] == brief and row["reason"] == "он зол"
    assert [(e["npc"], e["to"]) for e in g.journal if e["kind"] == "npc"] == [
        (LOBART, HILDA),
        (HILDA, LOBART),
        (LOBART, HILDA),
    ]
    assert [e["status"] for e in g.journal if e["kind"] == "scene"] == ["start", "done"]
    assert s.state == "done" and s.why == "" and not g.desk.floor_busy()
    # one call for two: one token of each one's bucket
    assert g.budget.allow(LOBART) and not g.budget.allow(LOBART)
    assert g.budget.allow(HILDA) and not g.budget.allow(HILDA)


@pytest.mark.parametrize("retold", [False, True])
@pytest.mark.parametrize("why,reason", [
    ("fought", "Браго дрался с ним"),
    ("companion", "Браго напал на героя и его спутников"),
    ("hostile", "Браго был его врагом"),
    ("quest", "сам хотел, чтобы Браго умер"),
])
async def test_crime_scene_brief_keeps_the_excuse_and_asks_teller_to_voice_it(tmp_path, retold, why, reason):
    async with _gossip(tmp_path) as g:
        crimes = Crimes(g.desk.link, g.chronicle, names=lambda npc: "Браго" if npc == "BRAGO" else npc)
        g.rumors.crimes = crimes
        g.chronicle.record({"fact": "crime", "id": "c1", "kind": "murder", "victim": "BRAGO", "day": 1})
        source = HILDA if retold else LOBART
        g.chronicle.record({"fact": "crime_side", "npc": source, "crime": "c1", "why": why})
        if retold:
            g.chronicle.record({
                "fact": "rumor", "crime": "c1", "kind": "murder", "from": HILDA, "to": LOBART,
                "excuse": why, "excuse_by": HILDA, "excuse_by_name": HILDA,
            })
        rumor = Rumor("crime:c1", "murder", source, source, "герой убил человека: Браго", crime="c1")
        rumor.known[LOBART] = int(retold)
        scene = gossip.Scene("scene", LOBART, HILDA, "Лобарт", "Хильда", rumor.key, "words")
        scene.news = rumor.description()
        brief = g.desk._brief(scene, rumor)
        name = "Хильда" if retold else "Лобарт"
        assert f"Оправдание: {name} говорит: {reason}" in brief
        assert "Рассказчик должен озвучить эту причину вместе с новостью." in brief
        assert crimes.excuse(LOBART, "c1")["excuse_by_name"] == source


@pytest.mark.parametrize("degree,role,with_react,expected", [
    ("heard", "stranger", True, True),
    ("seen", "stranger", True, False),
    ("heard", "member", True, False),
    ("heard", "guard", True, False),
    ("heard", "stranger", False, False),
])
async def test_view_can_offer_the_justified_version_to_a_knowing_listener(
    tmp_path, monkeypatch, degree, role, with_react, expected,
):
    async with _gossip(tmp_path) as g:
        crimes = Crimes(g.desk.link, g.chronicle)
        reactions = CrimeReact(g.desk.link, crimes) if with_react else None
        crimes.people.update({
            LOBART: {"guild": 8, "npctype": 0},
            HILDA: {"guild": {"stranger": 8, "member": 3, "guard": 2}[role], "npctype": 0},
            "BRAGO": {"guild": 3, "npctype": 0},
        })
        g.rumors.crimes = crimes
        crime = {
            "fact": "crime", "id": "c1", "kind": "murder", "victim": "BRAGO",
            "community": "city", "day": 1,
        }
        g.chronicle.record(crime)
        g.chronicle.record({"fact": "crime_seen", "npc": LOBART, "crime": "c1"})
        g.chronicle.record({"fact": "crime_side", "npc": LOBART, "crime": "c1", "why": "fought"})
        g.chronicle.record({
            "fact": "rumor", "crime": "c1", "kind": "murder", "quest": "crime:c1",
            "about": LOBART, "from": VINO, "to": HILDA, "hops": 1, "day": 1,
        })
        if degree == "seen":
            g.chronicle.record({"fact": "crime_seen", "npc": HILDA, "crime": "c1"})
        [knowledge] = crimes.known(HILDA)
        assert knowledge.degree == degree and not knowledge.excuse
        if reactions is not None:
            assert reactions.base_role(HILDA, crime) == role
        rumor = next(r for r in g.rumors.rumors.values() if r.crime == "c1")
        assert rumor.known == {LOBART: 0, HILDA: int(degree == "heard")}
        g.rumors.rumors = {rumor.key: rumor}
        g.rumors.clock = AT
        offers = []

        async def offer(r, a, b, gap, name):
            offers.append((r, a, b))

        monkeypatch.setattr(g.desk, "_offer", offer)
        g.desk._on_view(NpcView.model_validate(_view(LOBART, "Лобарт", _look(HILDA, "Хильда", 4))))
        await _until(lambda: not g.desk._background)
        assert offers == ([(rumor, LOBART, HILDA)] if expected else [])
        assert g.desk.scene is None
        if reactions is not None:
            await reactions.close()


async def test_one_fact_per_pass_and_witnesses(tmp_path):
    async with _gossip(tmp_path) as g:
        vino = {"npc": VINO, "name": "Вино", "distance": 600, "guild": 8}
        g.fake.nearby_of = {
            LOBART: [vino, {"npc": HILDA, "name": "Хильда", "distance": 400, "guild": 8}],
            HILDA: [{"npc": LOBART, "name": "Лобарт", "distance": 400, "guild": 8}, vino],
        }
        await _together(g)
        _tick(g)
        await _over(g)
        await _until(lambda: not g.desk._background)  # both lookups of who stood by have answered

    lines = " — ".join(f"{who}: «{text}»" for who, text, _ in THREE)
    assert _history(g, VINO) == [f"[Ты слышал, как Лобарт и Хильда говорили о герое: {lines}]"]  # once
    assert [(f["from"], f["to"]) for f in g.chronicle.facts("rumor")] == [(LOBART, HILDA)]
    assert [m["radius"] for m in g.fake.received if m["type"] == "story_nearby"] == [1000, 1000]


async def test_hero_cuts_both(tmp_path, monkeypatch):
    monkeypatch.setattr(gossip, "GRACE_S", 0.5)
    async with _gossip(tmp_path) as g:
        g.dialogue.scene_pace = lambda text: 0.5
        await _together(g)
        _tick(g)
        await _until(lambda: len(g.fake.says) == 2)
        s = g.desk.scene
        assert s is not None and s.play is not None
        second = s.play.lines[1]
        await _until(lambda: g.dialogue.started_at(second.line) is not None)
        await g.fake.send(type="ptt_start")  # the talk key: both stop at once
        await _until(lambda: s.state == "cut" and len(g.fake.hushed) == 2)
        cut_at = time.monotonic()
        assert g.desk.addressable() == [HILDA, LOBART]  # she spoke last
        assert g.desk.floor_busy()  # the floor is his: nobody comes up to speak first over him
        await asyncio.sleep(0.8)  # past the grace from the key, but it is still down
        assert g.fake.attached == {LOBART, HILDA}
        await g.fake.send(type="ptt_stop")
        stopped = time.monotonic()
        await asyncio.sleep(0.3)
        assert g.fake.attached == {LOBART, HILDA}  # the grace counts from the end of his voice
        assert g.desk.floor_busy()  # and the floor stays his through it: his words may be for them
        await _until(lambda: not g.fake.attached, 2)
        freed = time.monotonic()

    assert freed - stopped >= 0.45 and freed - cut_at >= 1.2
    assert g.fake.hushed == [
        {"type": "hush", "npc": HILDA, "line": second.line, "said": second.text},  # keeps her subtitle
        {"type": "hush", "npc": LOBART},
    ]
    assert len(g.fake.says) == 2 and (s.why, s.play.sent) == ("hero", 2)  # the third is never sent
    lines = [text for _, text, _ in THREE]
    (lobart,) = _notes(g, LOBART)
    assert (
        lobart.endswith(f"Хильда: «{lines[1]}». Вы замолчали: заговорил герой.]") and lines[2] not in lobart
    )
    (hilda,) = _notes(g, HILDA)
    hero = "Вы замолчали: заговорил герой."
    assert hilda == f"[Лобарт рассказал тебе о герое: Лобарт: «{lines[0]}» — Ты: «{lines[1]}». {hero}]"
    assert [e["why"] for e in g.journal if e["kind"] == "scene_cut"] == ["hero"]
    assert not any(e["kind"] == "interrupt" for e in g.journal)  # nobody was the hero's partner
    assert g.desk.addressable() == [] and not g.desk.floor_busy()  # let go: the usual rules again


async def test_hero_addresses_one_after_the_cut(tmp_path):
    answer = "Не твоё дело. Хотя… Лобарт говорит, ты ему наврал."
    llm = FakeBackend(script=[_lines(*THREE), FakeStep(answer)], delay=0)
    async with _gossip(tmp_path, llm) as g:
        g.dialogue.scene_pace = lambda text: 0.5
        await _together(g)
        _tick(g)
        await _until(lambda: len(g.fake.says) == 2)
        await g.fake.send(type="player_text", text="Хильда, о чём вы шептались?")
        await _until(lambda: answer in g.fake.said)
        assert g.talk.held[HILDA] == {"scene", "listen"} and g.talk.held[LOBART] == {"scene"}

    to, _ = g.talk.last
    assert to.id == HILDA and g.talk.partner.npc == HILDA and g.talk.partner.posture == "listen"
    assert (HILDA, "turn_to", {"npc": "PLAYER"}) in _actions(g)  # she turns to him
    say = next(m for m, _ in g.fake.says if m["text"] == answer)
    assert "overlay" not in say and "to" not in say  # she stands and answers him
    request = [m.content for m in llm.calls[1][1]]
    assert any(c.startswith("[Лобарт рассказал тебе о герое: Лобарт: «") for c in request)
    assert any("ты слышал, что он обманул других: Лобарт" in c for c in request)
    assert g.desk.last.why == "hero"


async def test_his_voice_keeps_the_floor(tmp_path):
    # while the hero holds the talk key no scene starts and nobody comes up to speak first
    async with _gossip(tmp_path) as g:
        planner, _, _, _ = make()
        g.desk.follow(planner)
        r, desk = g.rumor, g.desk
        await _together(g)
        await g.fake.send(type="ptt_start")
        await _until(desk.floor_busy)
        assert desk.stage(r, LOBART, HILDA).tier == "wait"
        assert planner.allow(NpcRecord(VINO, "Вино"), "approach_talk") == "герой говорит"
        await g.fake.send(type="ptt_cancel")
        await _until(lambda: not desk.floor_busy())
        assert desk.stage(r, LOBART, HILDA).tier == "words"
        assert planner.allow(NpcRecord(VINO, "Вино"), "approach_talk") is None


async def test_budget_for_two(tmp_path):
    # Hilda has no call left: they are only seen talking
    budget = Budget(per_npc_per_min=1, clock=Clock())
    assert budget.allow(HILDA)
    async with _gossip(tmp_path, budget=budget) as g:
        await _together(g)
        assert g.desk.stage(g.rumor, LOBART, HILDA).tier == "sight"
        _tick(g)
        await _until(lambda: len(g.fake.acts) == 2)
    assert not g.llm.calls and [f["how"] for f in g.chronicle.facts("rumor")] == ["met"]
    assert budget.allow(LOBART)  # nothing taken

    # a retry is a call too
    llm = FakeBackend(script=[_lines(*THREE, *THREE[1:]), _lines(*THREE)], delay=0)
    budget = Budget(per_npc_per_min=2, clock=Clock())
    async with _gossip(tmp_path, llm, budget=budget) as g:
        await _together(g)
        _tick(g)
        await _over(g)
    assert len(llm.calls) == 2 and len(g.fake.says) == 3
    assert not budget.can(LOBART) and not budget.can(HILDA)

    # a hold that fails takes nothing
    budget = Budget(per_npc_per_min=2, clock=Clock())
    async with _gossip(tmp_path, budget=budget) as g:
        g.fake.busy_attaches = 1
        await _together(g)
        _tick(g)
        s = await _over(g, "dropped")
    assert s.why == "hold" and not g.llm.calls and not g.fake.says
    assert budget.allow_all((LOBART, HILDA)) and budget.allow_all((LOBART, HILDA))


async def test_far_without_the_model(tmp_path):
    async with _gossip(tmp_path) as g:
        await _together(g, 40, 42, seen=False)
        assert _tick(g) == 1
        await asyncio.sleep(0.2)

    assert [(f["to"], f["how"]) for f in g.chronicle.facts("rumor")] == [(HILDA, "met")]
    assert "where" not in g.chronicle.facts("rumor")[0]
    assert not g.llm.calls and not g.fake.says and not g.fake.acts and not _types(g, "attach")
    assert g.desk.scenes == 0 and g.desk.last is None


async def test_sight_turns_and_chatter(tmp_path):
    for state, chatter in (("ZS_STAND_ARMSCROSSED", True), ("ZS_SMALLTALK", False)):
        async with _gossip(tmp_path) as g:
            # in view at 18 m, together: seen, not heard
            await _together(g, 18, 19, lobart={"state": state}, hilda={"state": state})
            _tick(g)
            await asyncio.sleep(0.3)
            assert not g.desk.busy(LOBART) and not g.desk.busy(HILDA)

        (fact,) = g.chronicle.facts("rumor")
        assert (fact["how"], fact["seen_by"]) == ("met", ["PLAYER"])
        assert not g.llm.calls and not g.fake.says and not _types(g, "attach")
        acts = [(m["npc"], m["action"], m["args"]["npc"]) for m in g.fake.acts]
        if chatter:  # each turns to the other with one of the game's smalltalk lines
            assert acts == [(LOBART, "say_svm", HILDA), (HILDA, "say_svm", LOBART)]
            assert all(m["args"]["svm"].startswith("SMALLTALK") for m in g.fake.acts)
        else:  # the game's own chatter is the scene
            assert acts == []


async def test_sight_waits_while_one_of_them_talks(tmp_path):
    async with _gossip(tmp_path) as g:
        r, desk = g.rumor, g.desk
        await _together(g, 18, 19)  # seen, not heard
        assert desk.stage(r, LOBART, HILDA).tier == "sight"
        # the hero talks to Lobart across the yard: no chatter turns him away from it
        assert g.talk.begin(LOBART, "Лобарт")
        assert desk.stage(r, LOBART, HILDA).tier == "wait"
        g.talk.end()
        # she answers someone
        g.dialogue.speaking = lambda npc: npc == HILDA
        assert desk.stage(r, LOBART, HILDA).tier == "wait"
        del g.dialogue.speaking
        # he talks to another: only the words wait for that
        assert g.talk.begin(VINO, "Вино")
        assert desk.stage(r, LOBART, HILDA).tier == "sight"
        g.talk.end()
        assert g.talk.begin(LOBART, "Лобарт")
        assert _tick(g) == 0
        await asyncio.sleep(0.2)
    assert not g.fake.acts and g.chronicle.facts("rumor") == []


async def test_busy_waits_and_sleep_is_fact(tmp_path):
    async with _gossip(tmp_path) as g:
        r, desk = g.rumor, g.desk
        await _together(g)
        assert desk.stage(r, LOBART, HILDA).tier == "words"
        # the hero talks to someone
        assert g.talk.begin(VINO, "Вино")
        assert desk.stage(r, LOBART, HILDA).tier == "wait"
        g.talk.end()
        # one of them answers him
        g.dialogue.speaking = lambda npc: npc == HILDA
        assert desk.stage(r, LOBART, HILDA).tier == "wait"
        del g.dialogue.speaking
        # held by another; a dialog of the scripts around
        await _together(g, hilda={"activity": "attached"})
        assert desk.stage(r, LOBART, HILDA).tier == "wait"
        await _together(g, hilda={"activity": "body"})
        assert desk.stage(r, LOBART, HILDA).tier == "wait"
        await _together(g, others=(person(VINO, "Вино", 12, activity="dialog"),))
        assert desk.stage(r, LOBART, HILDA).tier == "wait"
        await _together(g)
        # on his way to the hero (initiative); a soft act of ours under way
        desk.initiative = SimpleNamespace(active=LOBART)
        assert desk.stage(r, LOBART, HILDA).tier == "wait"
        desk.initiative = None
        g.fake.auto_idle = False
        walk = asyncio.create_task(g.bodies.act(HILDA, "look_at", ms=500))
        await _until(lambda: g.bodies.busy(HILDA))
        assert desk.stage(r, LOBART, HILDA).tier == "wait"
        walk.cancel()
        await asyncio.gather(walk, return_exceptions=True)
        assert desk.stage(r, LOBART, HILDA).tier == "words"
        # they don't see each other: how far apart is asked, the next tick knows
        await _around(
            g,
            person(LOBART, "Лобарт", 8, True),
            person(HILDA, "Хильда", 10, True),
            views=(_view(LOBART, "Лобарт"), _view(HILDA, "Хильда")),
        )
        g.fake.nearby_of = {LOBART: [{"npc": HILDA, "name": "Хильда", "distance": 450, "guild": 8}]}
        assert desk.stage(r, LOBART, HILDA).tier == "wait"
        await _until(lambda: bool(desk._gaps))
        assert desk.stage(r, LOBART, HILDA).tier == "words"
        assert [m["radius"] for m in g.fake.received if m["type"] == "story_nearby"] == [1500]
        desk._gaps.clear()
        g.fake.nearby_of = {LOBART: []}  # not within WALK_CM: apart
        assert desk.stage(r, LOBART, HILDA).tier == "wait"
        await _until(lambda: bool(desk._gaps))
        assert desk.stage(r, LOBART, HILDA).tier == "fact"
        # asleep: every sleep of the routines is ZS_GOTOBED
        await _together(g, hilda={"state": "ZS_GOTOBED"})
        assert desk.stage(r, LOBART, HILDA).tier == "fact"

    # an older bridge can't address a line to an npc: as before, and nothing waits
    async with _gossip(tmp_path, protocol=14) as g:
        planner, decider, _, _ = make(FakeDecider("tell", 0.9))
        g.desk.follow(planner)
        await _together(g)
        assert g.desk.stage(g.rumor, LOBART, HILDA).tier == "fact"
        # nor the second occasion: two who see each other near him are not asked, and nothing passes
        g.rumors.clock = AT
        await g.fake.push_views(_view(LOBART, "Лобарт", _look(HILDA, "Хильда", 5)))
        await _until(lambda: g.perception.view(LOBART).sees[0].distance == 500)
        await asyncio.sleep(0.1)
        assert not decider.calls and g.chronicle.facts("rumor") == []
        assert _tick(g) == 1 and g.chronicle.facts("rumor")[0]["how"] == "met"


async def test_posts_and_escorts_are_never_held(tmp_path):
    async with _gossip(tmp_path) as g:
        r, desk = g.rumor, g.desk
        for who in ({"lobart": {"state": "ZS_GUARD_PASSAGE"}}, {"hilda": {"state": "ZS_FOLLOW_PLAYER"}}):
            await _together(g, **who)
            assert desk.stage(r, LOBART, HILDA).tier == "sight"
            await _together(g, seen=False, **who)
            assert desk.stage(r, LOBART, HILDA).tier == "fact"
        await _together(g, lobart={"state": "ZS_GUARD_PASSAGE"})
        _tick(g)
        await _until(lambda: len(g.fake.acts) == 2)
        assert not _types(g, "attach") and not g.fake.says

    # a plot npc may be held and speak (it never walks)
    async with _gossip(tmp_path) as g:
        g.guard.plot = lambda npc: npc == LOBART
        await _together(g)
        _tick(g)
        await _over(g)
    assert _types(g, "attach") == [LOBART, HILDA] and [m["npc"] for m, _ in g.fake.says] == [
        LOBART,
        HILDA,
        LOBART,
    ]
    assert not [a for a in _actions(g) if a[1] == "go_to_npc"]


async def test_bad_lines_retried_then_dropped(tmp_path):
    five = [*THREE, *THREE[1:]]
    twice = [THREE[0], THREE[2]]  # the same one twice running
    llm = FakeBackend(script=[_lines(*five), _lines(*twice)], delay=0)
    async with _gossip(tmp_path, llm) as g:
        await _together(g)
        _tick(g)
        s = await _over(g, "dropped")

    assert s.why == "model" and len(llm.calls) == 2 and not g.fake.says
    retry = [m.content for m in llm.calls[1][1]]
    assert retry[0] == llm.calls[0][1][0].content  # the same brief
    assert "Отклонено:\n- lines: от 2 до 4 реплик, а не 5" in retry[1]
    assert s.errors == [
        "lines: от 2 до 4 реплик, а не 5",
        "реплика 2: говорят по очереди, а не один дважды подряд",
    ]
    # the pass stands (fact first): Lobart remembers telling her, in the code's words
    assert _notes(g, LOBART) == [f"[Ты рассказал человеку по имени Хильда то, что знаешь о герое: {NEWS}]"]
    assert _notes(g, HILDA) == [] and [f["how"] for f in g.chronicle.facts("rumor")] == ["talked"]
    assert [r["status"] for r in _rows(g, "scene")] == ["dropped"]


async def test_no_retry_once_out_of_earshot(tmp_path):
    five = [*THREE, *THREE[1:]]
    llm = FakeBackend(script=[_lines(*five), _lines(*THREE)], delay=0, first=0.5)
    async with _gossip(tmp_path, llm) as g:
        await _together(g)
        _tick(g)
        await _until(lambda: bool(llm.calls))  # writing
        # he steps back meanwhile: not gone (no cut), but they are not heard any more
        await _around(g, person(LOBART, "Лобарт", 16, True), person(HILDA, "Хильда", 18, True))
        s = await _over(g, "dropped")

    assert s.why == "moved" and len(llm.calls) == 1 and not g.fake.says
    # the one call took one token of each bucket, the retry none
    assert g.budget.allow_all((LOBART, HILDA)) and not g.budget.allow_all((LOBART, HILDA))


def test_lines_are_checked_and_cleaned():
    labels = ("Лобарт", "Хильда")
    args = {
        "lines": [
            {"speaker": "Лобарт", "text": "Лобарт: [зло] Наврал он мне.", "mood": "angry"},
            {"speaker": "Хильда", "text": "**Да ну?**", "mood": "sly"},
        ],
        "quiet": True,
    }
    assert gossip.check(args, labels) == (
        [(0, "Наврал он мне.", "angry"), (1, "Да ну?", "neutral")],
        True,
        [],
    )
    assert gossip.check({}, labels)[2] == ["ответ — вызов lines"]
    wrong = {
        "lines": [
            {"speaker": "Хильда", "text": "Да?", "mood": "neutral"},
            {"speaker": "Вино", "text": "x" * 200},
        ]
    }
    assert gossip.check(wrong, labels)[2] == [
        "реплика 1: первая — рассказчика («Лобарт»)",
        "реплика 2: speaker — «Лобарт» или «Хильда»",
    ]
    long = {"lines": [{"speaker": "Лобарт", "text": "x" * 161}, {"speaker": "Хильда", "text": "[кивает]"}]}
    assert gossip.check(long, labels)[2] == ["реплика 1: длиннее 160 знаков", "реплика 2: пустая"]
    assert gossip.labels_of("Стражник", "Стражник") == ("первый", "второй")


async def test_a_line_that_never_starts(tmp_path, monkeypatch):
    monkeypatch.setattr(dialogue_module, "SCENE_START_S", 0.3)
    async with _gossip(tmp_path) as g:
        g.fake.drop_says = {HILDA}  # she can't speak now: the bridge drops her line without a word
        await _together(g)
        _tick(g)
        s = await _over(g)

    assert s.why == "not_started" and [m["npc"] for m, _ in g.fake.says] == [LOBART, HILDA]
    assert g.fake.hushed == [{"type": "hush", "npc": HILDA}]
    first = THREE[0][1]
    assert _notes(g, LOBART) == [
        f"[Ты рассказал о герое человеку по имени Хильда (герой стоял в 8 м): Ты: «{first}»]"
    ]
    assert _notes(g, HILDA) == [f"[Лобарт рассказал тебе о герое: Лобарт: «{first}»]"]


async def test_cut_during_the_attach_frees_it(tmp_path):
    async with _gossip(tmp_path) as g:
        g.fake.attach_delay = 0.5
        await _together(g)
        _tick(g)
        await _until(lambda: _types(g, "attach") == [LOBART])
        await g.fake.send(type="ptt_start")  # while the game is still attaching him
        s = await _over(g, "cut")

    assert s.why == "hero" and s.held == {LOBART}
    assert _types(g, "attach") == [LOBART] and _types(g, "detach") == [LOBART]  # held after the cut, freed
    assert not g.llm.calls and not g.fake.says
    assert _notes(g, LOBART) == [f"[Ты рассказал человеку по имени Хильда то, что знаешь о герое: {NEWS}]"]


async def test_cut_before_it_begins(tmp_path):
    # the hero's voice in the same moment the pass was staged: nobody is held
    async with _gossip(tmp_path) as g:
        await _together(g)
        _tick(g)
        assert g.desk.cut("hero")  # its task has not run yet
        s = await _over(g, "cut")
    assert s.why == "hero" and not s.held and not _types(g, "attach")
    assert not g.llm.calls and not g.fake.says

    # mob users (no holds, no walk): no model call, and nothing taken from the budget
    async with _gossip(tmp_path) as g:
        await _together(g, lobart_body="mob", hilda_body="mob")
        assert g.desk.stage(g.rumor, LOBART, HILDA).tier == "words"
        _tick(g)
        assert g.desk.cut("hero")
        s = await _over(g, "cut")
        await asyncio.sleep(0.1)  # a write begun anyway would have called the model by now
    assert s.why == "hero" and not _types(g, "attach") and not g.llm.calls
    assert g.budget.allow_all((LOBART, HILDA)) and g.budget.allow_all((LOBART, HILDA))


async def test_loading_drops_the_scene(tmp_path):
    async with _gossip(tmp_path) as g:
        g.dialogue.scene_pace = lambda text: 0.5
        await _together(g)
        _tick(g)
        await _until(lambda: len(g.fake.says) == 1)
        await g.fake.send(type="loading")
        await _until(lambda: g.desk.last is not None)
        await asyncio.sleep(0.8)

    s = g.desk.last
    assert (s.state, s.why) == ("cut", "loading") and s.released
    assert len(g.fake.says) == 1 and not g.fake.hushed and not _types(g, "detach")  # the world is gone
    assert _notes(g, LOBART) == [] and _notes(g, HILDA) == []
    assert not g.desk.floor_busy() and g.desk.addressable() == []


async def test_a_hold_that_lands_after_a_load_is_freed(tmp_path):
    async with _gossip(tmp_path) as g:
        g.fake.attach_delay = 0.5
        await _together(g)
        _tick(g)
        await _until(lambda: _types(g, "attach") == [LOBART])
        await g.fake.send(type="loading")  # while the game is still attaching him: his hold comes after
        await _until(lambda: g.desk.last is not None and g.desk.last.released, 2)
        await _until(lambda: not g.fake.attached and not g.talk.held, 2)

    s = g.desk.last
    assert (s.state, s.why, s.held) == ("cut", "loading", {LOBART})
    assert _types(g, "attach") == [LOBART] and _types(g, "detach") == [LOBART]
    assert not g.llm.calls and not g.fake.says


async def test_threat_cuts_the_scene(tmp_path):
    async with _gossip(tmp_path) as g:
        g.dialogue.scene_pace = lambda text: 0.5
        await _together(g)
        _tick(g)
        await _until(lambda: len(g.fake.says) == 1)
        await g.fake.perception(LOBART, "ASSESSDAMAGE", "WOLF")  # a held speaker is hit
        s = await _over(g, "cut", 2)  # freed at once
        assert s.why == "threat" and len(g.fake.says) == 1

    # Hilda at the stove speaks free (not held); a wolf goes for her
    async with _gossip(tmp_path) as g:
        g.dialogue.scene_pace = lambda text: 0.5
        await _together(g, hilda_body="mob")
        _tick(g)
        await _until(lambda: len(g.fake.says) == 1)
        assert _types(g, "attach") == [LOBART]
        await g.fake.send(type="npc_event", npc=HILDA, kind="monster_attack", other="WOLF", detail="you")
        s = await _over(g, "cut", 2)
        assert s.why == "threat" and s.held == {LOBART}


async def test_the_hero_walks_away_and_a_dialog_opens(tmp_path):
    async with _gossip(tmp_path) as g:
        g.dialogue.scene_pace = lambda text: 0.5
        await _together(g)
        _tick(g)
        await _until(lambda: len(g.fake.says) == 1)
        await _around(g, person(LOBART, "Лобарт", 21), person(HILDA, "Хильда", 23))
        s = await _over(g, "cut", 2)
        await asyncio.sleep(0.6)
        assert s.why == "hero_left" and len(g.fake.says) == 1

    async with _gossip(tmp_path) as g:
        g.dialogue.scene_pace = lambda text: 0.5
        await _together(g)
        _tick(g)
        await _until(lambda: len(g.fake.says) == 1)
        await _together(g, others=(person(VINO, "Вино", 5, activity="dialog"),))
        s = await _over(g, "cut", 2)
        assert s.why == "dialog" and len(g.fake.says) == 1


async def test_view_trigger_with_shadow_talk_to(tmp_path, monkeypatch):
    monkeypatch.setattr(social, "P_MEET", 1.0)
    async with _gossip(tmp_path) as g:
        planner, decider, _, _ = make(FakeDecider("tell", 0.9))
        g.desk.follow(planner)
        g.rumors.gate = lambda r, a, b: False  # held closed
        g.rumors.clock = AT
        await g.fake.push_views(_view(LOBART, "Лобарт", _look(HILDA, "Хильда", 4)))
        await _until(lambda: bool(_rows(g, "brain")))
        assert g.chronicle.facts("rumor") == []
        # the same pair is looked at again only a game hour later; then the lot decides
        g.rumors.gate = None
        await g.fake.push_views(_view(LOBART, "Лобарт", _look(HILDA, "Хильда", 4)))
        await asyncio.sleep(0.2)
        assert len(decider.calls) == 1
        g.rumors.clock = AT + gossip.PAIR_GAP_MIN
        await g.fake.push_views(_view(LOBART, "Лобарт", _look(HILDA, "Хильда", 4)))
        await _until(lambda: g.chronicle.facts("rumor") != [])

    held, told = _rows(g, "brain")
    assert (held["question"], held["applied"], held["why"], held["code"]) == (
        "talk_to",
        False,
        "shadow",
        "held",
    )
    assert (held["npc"], held["other"], held["choice"], held["lane"]) == (LOBART, HILDA, "tell", "background")
    assert held["options"] == {"tell": "tell Хильда now", "keep": "keep it to yourself for now"}
    assert "News you know and Хильда does not: the hero lied to you: he claimed" in held["state"]
    assert told["code"] == "told"
    call = decider.calls[0]
    assert (call["lane"], call["priority"], call["deadline_ms"]) == ("background", 0.2, gossip.SHADOW_MS)
    # nobody around the hero: the pass is a fact as any other
    assert [(f["to"], f["how"]) for f in g.chronicle.facts("rumor")] == [(HILDA, "met")]


async def test_a_waiting_view_pass_is_asked_once(tmp_path):
    async with _gossip(tmp_path) as g:
        planner, decider, _, _ = make(FakeDecider("tell", 0.9))
        g.desk.follow(planner)
        await _together(g, 18, 19)  # seen, not heard
        g.desk.initiative = SimpleNamespace(active=LOBART)  # on his way to the hero: the pass waits
        g.rumors.clock = AT
        view = _view(LOBART, "Лобарт", _look(HILDA, "Хильда", 4))
        await g.fake.push_views(view)
        await _until(lambda: bool(_rows(g, "brain")))
        for _ in range(3):  # the views come about once a second while it waits
            await g.fake.push_views(view)
            await asyncio.sleep(0.05)
        g.desk.initiative = None
        await g.fake.push_views(view)
        await asyncio.sleep(0.1)
        assert g.chronicle.facts("rumor") == []  # offered again only a game minute later
        g.rumors.clock = AT + gossip.RETRY_MIN
        await g.fake.push_views(view)
        await _until(lambda: len(g.fake.acts) == 2)  # they are seen talking

    assert len(decider.calls) == 1  # the shadow question once, not on every view while it waited
    assert [row["code"] for row in _rows(g, "brain")] == ["wait"]
    assert [(f["to"], f["how"]) for f in g.chronicle.facts("rumor")] == [(HILDA, "met")]


async def test_planner_veto_while_busy(tmp_path):
    async with _gossip(tmp_path, FakeBackend(script=[_lines(*THREE)], delay=0, first=1.0)) as g:
        planner, _, _, _ = make()
        planner.allow = lambda rec, choice: "before" if rec.id == VINO else None  # the initiative's
        g.desk.follow(planner)
        await _together(g)
        _tick(g)
        await _until(lambda: bool(g.llm.calls))  # writing
        assert planner.allow(NpcRecord(LOBART, "Лобарт"), "look_at") == "болтает"
        assert planner.allow(NpcRecord(LOBART, "Лобарт"), "fight") is None  # its body is the threat's
        assert planner.allow(NpcRecord(VINO, "Вино"), "approach_talk") == "рядом болтают"
        assert planner.allow(NpcRecord(VINO, "Вино"), "look_at") == "before"
        await _over(g)
        assert planner.allow(NpcRecord(LOBART, "Лобарт"), "look_at") is None


async def test_walker_walks_up(tmp_path):
    async with _gossip(tmp_path) as g:
        written_before: list[bool] = []
        handle = g.fake.handle_action

        async def slow(msg: dict[str, Any]) -> None:
            if msg["action"] == "go_to_npc":
                await asyncio.sleep(0.3)  # on his way
                written_before.append(bool(g.llm.calls))
            await handle(msg)

        g.fake.handle_action = slow
        await _together(g, gap_m=12)
        staging = g.desk.stage(g.rumor, LOBART, HILDA)
        assert (staging.tier, staging.walker) == ("words", LOBART)
        _tick(g)
        await _over(g)

    assert written_before == [False] and len(g.llm.calls) == 1  # the write only once he is there
    walk = [a for a in _actions(g) if a[1] == "go_to_npc"]
    assert walk == [(LOBART, "go_to_npc", {"npc": HILDA, "radius": 250})]

    async with _gossip(tmp_path) as g:
        r, desk = g.rumor, g.desk
        await _together(g, gap_m=12)
        g.guard.plot = lambda npc: npc == LOBART  # a plot teller stays: she comes to him
        staging = desk.stage(r, LOBART, HILDA)
        assert (staging.tier, staging.walker) == ("words", HILDA)
        g.guard.plot = lambda npc: True  # neither may leave its spot
        assert desk.stage(r, LOBART, HILDA).tier == "sight"
        g.guard.plot = lambda npc: npc == LOBART
        await _together(g, gap_m=12, hilda_body="mob")  # nor may she leave her stove
        assert desk.stage(r, LOBART, HILDA).tier == "sight"


async def test_a_natural_pass_reaches_words(tmp_path, monkeypatch):
    monkeypatch.setattr(social, "P_MEET", 1.0)
    async with _gossip(tmp_path) as g:
        g.rumors.gate = None
        await _together(g)
        assert g.talk.begin(LOBART, "Лобарт")  # he caught the hero, they still talk
        assert _tick(g) == 0 and g.rumors.waiting == {(KEY, LOBART, HILDA): AT}
        assert g.chronicle.facts("rumor") == []
        g.talk.end()  # the hero walks off: the next tick, Lobart turns to his wife
        assert _tick(g, AT + 1) == 1 and not g.rumors.waiting
        await _over(g)

    assert [(f["to"], f["how"]) for f in g.chronicle.facts("rumor")] == [(HILDA, "talked")]
    assert len(g.fake.says) == 3


def test_e2e_look_waits_out_the_line_before():
    # the e2e's [look]: a later line counts only once the look of the line before has run out
    first = SceneLine("l1", LOBART, "Лобарт", HILDA, THREE[0][1], "angry", audio_s=2.0)
    second = SceneLine("l2", HILDA, "Хильда", LOBART, THREE[1][1], "neutral", audio_s=2.0)
    said, starts = [first, second], {"l1": (0.0, True), "l2": (2.3, True)}

    def sample(t: float, lobart_to: str | None) -> tuple[float, dict[str, NpcView]]:
        """What the two see at t: Lobart sees her talking to him; she sees him talking to `lobart_to`."""
        to = {"talking_to": lobart_to} if lobart_to else {}
        views = (
            _view(LOBART, "Лобарт", _look(HILDA, "Хильда", 4, talking_to=LOBART)),
            _view(HILDA, "Хильда", _look(LOBART, "Лобарт", 4, **to)),
        )
        return t, {v["npc"]: NpcView.model_validate(v) for v in views}

    times = (0.5, 1.5, 2.5, 2.9, 3.5, 4.0)
    # her line went to nobody: his look at her is only what is left of his own line (until 3 s)
    leftover = [sample(t, HILDA if t < 3.0 else None) for t in times]
    assert _talked_to(leftover, said, starts, 0) and not _talked_to(leftover, said, starts, 1)
    # said to him: he talks to her while it plays
    assert _talked_to([sample(t, HILDA) for t in times], said, starts, 1)
