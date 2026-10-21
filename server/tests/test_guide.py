import asyncio
from types import SimpleNamespace
from typing import Any

import pytest

import gothic_ai.guide
from gothic_ai.guide import LEAD, Guide, Lead, Way
from gothic_ai.journal import Journal
from gothic_ai.llm import ToolCall
from gothic_ai.llm.fake import FakeBackend, FakeStep
from gothic_ai.memory import MemoryStore
from gothic_ai.places import PlaceBook
from gothic_ai.protocol import ActionResult, RoutineEntry, RoutineInfo
from gothic_ai.storyguard import StoryGuard
from tests.fake_game import FakeGame
from tests.test_deals import GOLD, _setup, _shown
from tests.test_dialogue import XARDAS, _until

MALETH, GUARD = "BAU_954_MALETH", "MIL_315_GUARD"
BRAGO = "BDT_1014_BANDIT_L"


def _place(id: str, ru: str, where: str, center: list[int], kind: str, **more: Any) -> dict[str, Any]:
    return {"id": id, "ru": ru, "where_ru": where, "en": "", "where_en": "", "center": center, "kind": kind,
            **more}  # fmt: skip


BOOK = PlaceBook(
    {
        "version": 2,
        "world": "NEWWORLD.ZEN",
        "places": [
            _place("FARM1_PATH_SHEEP", "пастбище у дороги", "на пастбище у дороги", [0, 0, 0], "field"),
            _place("FARM1_MILL", "мельница фермы Лобарта", "у мельницы фермы Лобарта", [6000, 0, 0], "yard"),
            _place(
                "FARM1_INHOUSE",
                "дом Лобарта",
                "в доме Лобарта",
                [3000, 0, 3000],
                "house",
                inside=True,
                entrances=[
                    {
                        "in": "NW_FARM1_INHOUSE_01",
                        "out": "NW_FARM1_ENTRANCE_02",
                        "pos": [3000, 0, 2500],
                        "side": "FARM1_PATH_SHEEP",
                    }
                ],
            ),
            _place("FARM1_INSTABLE", "хлев фермы Лобарта", "в хлеву", [-3000, 0, 0], "house", inside=True),
            _place("CITY_MARKET", "рыночная площадь", "на рыночной площади", [90000, 0, 0], "street"),
        ],
        "points": {
            "NW_FARM1_PATH_CITY_SHEEP_09": 0,
            "NW_FARM1_MILL_01": 1,
            "NW_FARM1_INHOUSE_01": 2,
            "NW_FARM1_INSTABLE": 3,
            "NW_CITY_MARKET_01": 4,
        },
    }  # fmt: skip
)


@pytest.fixture(autouse=True)
def quick(monkeypatch):
    monkeypatch.setattr(gothic_ai.guide, "EVERY_S", 0.01)
    monkeypatch.setattr(gothic_ai.guide, "WAIT_S", 0.3)
    monkeypatch.setattr(gothic_ai.guide, "STUCK_S", 30.0)


def _snap(npc_pos, hero_pos, nearest="NW_FARM1_PATH_CITY_SHEEP_09"):
    distance = sum((a - b) ** 2 for a, b in zip(npc_pos, hero_pos, strict=True)) ** 0.5
    return SimpleNamespace(
        npc=SimpleNamespace(pos=tuple(npc_pos), nearest_waypoint=nearest),
        player=SimpleNamespace(pos=tuple(hero_pos), distance=distance),
        world=SimpleNamespace(day=2, hour=19, minute=5),
    )


class _Link:
    """The game as the guide sees it: the npc's snapshot, its routine, and its walk (the test moves it)."""

    def __init__(self) -> None:
        self.listeners: list[Any] = []
        self.snapshots: dict[str, Any] = {MALETH: _snap((0, 0, 0), (300, 0, 0))}
        self.routines: dict[str, list[RoutineEntry]] = {
            MALETH: [RoutineEntry(start="05:00", end="21:00", state="ZS_STAND_GUARDING",
                                  waypoint="NW_FARM1_PATH_CITY_SHEEP_09"),
                     RoutineEntry(start="21:00", end="05:00", state="ZS_SLEEP",
                                  waypoint="NW_FARM1_INSTABLE")],
        }  # fmt: skip
        self.acts: list[tuple[str, dict[str, Any]]] = []
        self.walk: asyncio.Future[str] | None = None

    async def routine(self, npc: str) -> RoutineInfo:
        return RoutineInfo(type="routine", npc=npc, entries=self.routines.get(npc, []))

    async def act(self, npc: str, action: str, /, *, timeout: float = 90, **args: Any) -> ActionResult:
        self.acts.append((action, args))
        if self.walk is not None and not self.walk.done():
            self.walk.set_result("cancelled")  # a new action takes the place of the walk
        status = "done"
        if action == "go_to_waypoint" and "points" in args:  # the way there (else a hop onto the waynet)
            self.walk = asyncio.get_running_loop().create_future()
            status = await self.walk
        return ActionResult(type="action_result", id="a", npc=npc, status=status)


class _Talk:
    def __init__(self) -> None:
        self.people: dict[str, Any] = {}
        self.held: dict[str, set[str]] = {}
        self.partner: Any = SimpleNamespace(npc=MALETH)

    async def hold(self, npc: str, why: str) -> bool:
        self.held.setdefault(npc, {"listen"}).add(why)
        return True

    def stop_listening(self, npc: str) -> None:
        self.held[npc].discard("listen")

    async def listen(self, npc: str) -> bool:
        self.held.setdefault(npc, set()).add("listen")
        return True

    def free(self, npc: str, why: str) -> None:
        self.held.get(npc, set()).discard(why)


class _Dialogue:
    def __init__(self) -> None:
        self.journal = Journal()
        self.cues: list[str] = []
        self.notes: list[str] = []
        self.conv = SimpleNamespace(actions=set(), voice=None, reply=None)
        self.conversations = {MALETH: self.conv}
        self.heard = False  # its line is being heard

    def audible(self, npc: str) -> bool:
        return self.heard

    def open(self, npc: str, name: str) -> Any:
        return self.conv

    def event_turn(self, npc: str, cue: str, **kw: Any) -> None:
        self.cues.append(cue)

    def note(self, npc: str, text: str) -> None:
        self.notes.append(text)


def _guide(link: _Link | None = None) -> tuple[Guide, _Link, _Talk, _Dialogue, list[dict[str, Any]]]:
    link = link or _Link()
    talk, dialogue, facts = _Talk(), _Dialogue(), []
    chronicle = SimpleNamespace(record=facts.append)
    guide = Guide(link, dialogue, talk, BOOK, StoryGuard(None), chronicle)  # type: ignore[arg-type]
    return guide, link, talk, dialogue, facts


async def test_where_it_may_lead_the_hero():
    guide, link, talk, _, _ = _guide()
    snap = link.snapshots[MALETH]
    mill, why = await guide.way(MALETH, "FARM1_MILL", snap)
    assert mill is not None and mill.points == ("NW_FARM1_MILL_01",) and not mill.door
    # another's house: up to its way in; its own (it sleeps in the stable): inside
    house, _ = await guide.way(MALETH, "FARM1_INHOUSE", snap)
    assert house is not None and house.door and house.points == ("NW_FARM1_ENTRANCE_02",)
    assert house.title() == "дом Лобарта, до входа" and house.where_ru() == "у входа: дом Лобарта"
    stable, _ = await guide.way(MALETH, "FARM1_INSTABLE", snap)
    assert stable is not None and not stable.door
    assert (await guide.way(MALETH, "CITY_MARKET", snap))[1].startswith("рыночная площадь — слишком далеко")
    assert (await guide.way(MALETH, "FARM1_PATH_SHEEP", snap))[1] == "вы и так здесь: на пастбище у дороги"
    assert (await guide.way(MALETH, "замок", snap))[1].startswith("ты не знаешь такого места: «замок»")

    # whose body must stay where it is
    plot = (await guide.way(XARDAS, "FARM1_MILL", snap))[1]
    assert plot == "тебе нельзя уходить отсюда (сюжетный: только слова)"
    assert (await guide.way("VLK_449_LARES", "FARM1_MILL", snap))[1] == plot  # he leads by the scripts
    talk.people[GUARD] = SimpleNamespace(state="ZS_GUARD_PASSAGE")
    assert (await guide.way(GUARD, "FARM1_MILL", snap))[1].endswith("(стоит на посту)")
    link.routines[MALETH][0] = RoutineEntry(start="05:00", end="21:00", state="ZS_GUIDE_PLAYER", waypoint="X")
    guide._routines.clear()  # read again: the scripts set it to lead the hero
    assert (await guide.way(MALETH, "FARM1_MILL", snap))[1].startswith("тебе нельзя уходить: по распорядку")
    guide.companion = lambda npc: True
    assert (await guide.way(MALETH, "FARM1_MILL", snap))[1].startswith("ты и так идёшь с героем")


async def test_it_is_told_it_may_lead_the_hero_unless_it_must_stay():
    guide, _, _, _, _ = _guide()
    await guide.prepare(MALETH)
    assert (guide.situation(MALETH) or "").startswith("Можешь отвести героя к месту, если захочешь")
    await guide.prepare(XARDAS)  # the plot's: it stays where it is
    assert guide.situation(XARDAS) is None


async def test_brago_may_lead_to_dexters_real_hut_but_not_meet_or_enter_other_houses():
    guide, link, talk, _, _ = _guide()
    snap = link.snapshots[MALETH]
    guide.book = PlaceBook.load()
    assert guide.book is not None
    hut = guide.book.get("CASTLEMINE_HUT")
    assert hut is not None and hut.kind == "house" and not hut.inside and not hut.entrances
    way, why = await guide.way(BRAGO, hut.id, snap)
    assert why == "" and way is not None and not way.door
    assert way.points == hut.waynet and way.pos == hut.center
    assert (await guide.way(MALETH, hut.id, snap))[1].endswith("где в него вход, ты не знаешь")
    assert (await guide.way(BRAGO, hut.id, snap, meet=True))[1].endswith("где в него вход, ты не знаешь")
    far = _snap((-100000, 0, 0), (-100000, 0, 100))
    assert "слишком далеко" in (await guide.way(BRAGO, hut.id, far))[1]
    talk.people[BRAGO] = SimpleNamespace(state="ZS_GUIDE_PLAYER")
    assert "по скриптам" in (await guide.way(BRAGO, hut.id, snap))[1]
    talk.people.clear()
    guide.book = BOOK
    assert (await guide.way(BRAGO, "FARM1_INSTABLE", snap))[1].endswith("где в него вход, ты не знаешь")

    guide.leads[BRAGO] = Lead(BRAGO, "Браго", way, "D1", state="going")
    assert guide.situation(BRAGO) == "Ты ведёшь героя к Декстеру за награду; он идёт за тобой."
    guide.leads[BRAGO].state = "waiting"
    assert guide.situation(BRAGO) == "Ты ведёшь героя к Декстеру за награду; он отстал: ты ждёшь его."


async def test_it_leads_the_hero_waits_for_him_and_says_they_are_there():
    guide, link, talk, dialogue, facts = _guide()
    way, _ = await guide.way(MALETH, "FARM1_MILL", link.snapshots[MALETH])
    assert way is not None
    guide.start(MALETH, "Малет", way, "D1")
    assert dialogue.cues[0].startswith("[Герой согласился, сделка состоялась: ты ведёшь его — мельница")
    await _until(lambda: link.walk is not None)
    assert talk.held[MALETH] == {LEAD}  # it walks ahead: it answers him on the move
    assert guide.situation(MALETH) == "Ты ведёшь героя — мельница фермы Лобарта; он идёт за тобой."
    assert dialogue.conv.actions  # no gesture cuts the way off
    go = link.acts[-1][1]
    assert go["points"] == ["NW_FARM1_MILL_01"] and go["pos"] == [6000, 0, 0]

    # the hero falls behind: it stops, turns to him and calls him
    link.snapshots[MALETH] = _snap((2000, 0, 0), (0, 0, 0))
    await _until(lambda: guide.leads[MALETH].state == "waiting")
    assert link.acts[-1] == ("turn_to", {"npc": "PLAYER"})
    assert dialogue.cues[-1] == "[Герой отстал: он в 20 м позади. Позови его — пусть идёт за тобой.]"
    assert "отстал: ты ждёшь его" in (guide.situation(MALETH) or "")
    # he comes up: it goes on, and gets there
    link.snapshots[MALETH] = _snap((2000, 0, 0), (1700, 0, 0))
    await _until(lambda: link.acts[-1][0] == "go_to_waypoint" and not link.walk.done())
    link.snapshots[MALETH] = _snap((6000, 0, 0), (5600, 0, 0), "NW_FARM1_MILL_01")
    link.walk.set_result("done")
    await _until(lambda: MALETH not in guide.leads)
    assert dialogue.cues[-1] == "[Вы пришли: у мельницы фермы Лобарта. Скажи герою, что пришли.]"
    assert facts == [{"fact": "led", "npc": MALETH, "name": "Малет", "place": "FARM1_MILL", "door": False,
                      "deal": "D1", "how": "arrived", "day": 2, "hour": 19, "minute": 5}]  # fmt: skip
    assert talk.held[MALETH] == {"listen"}  # it stands by him while they talk


async def test_he_does_not_come_and_it_goes_back_to_its_business():
    guide, link, talk, dialogue, facts = _guide()
    way, _ = await guide.way(MALETH, "FARM1_MILL", link.snapshots[MALETH])
    assert way is not None
    talk.partner = None
    guide.start(MALETH, "Малет", way, "D1")
    await _until(lambda: link.walk is not None)
    link.snapshots[MALETH] = _snap((2000, 0, 0), (-1000, 0, 0))
    await _until(lambda: MALETH not in guide.leads)
    assert [f["how"] for f in facts] == ["lost"] and LEAD not in talk.held[MALETH]
    assert dialogue.notes[-1].endswith("но он отстал и не пришёл; ты вернулся к своим делам")


async def test_stuck_on_its_way_it_gets_back_onto_the_waynet_and_at_last_gives_up(monkeypatch):
    monkeypatch.setattr(gothic_ai.guide, "STUCK_S", 0.1)
    guide, link, _, dialogue, facts = _guide()
    link.snapshots[MALETH] = _snap((1000, 0, 0), (800, 0, 0))  # he is close behind, it does not move
    guide.start(MALETH, "Малет", Way(BOOK.places[1], ("NW_FARM1_MILL_01",), (6000.0, 0.0, 0.0)), "D1")
    await _until(lambda: MALETH not in guide.leads)
    hops = [args for action, args in link.acts if action == "go_to_waypoint" and "points" not in args]
    assert len(hops) == 3 and hops[0] == {"waypoint": "", "pos": [1000, 0, 0], "radius": 200}
    assert [f["how"] for f in facts] == ["failed"]
    assert dialogue.cues[-1] == "[Ты не смог довести героя: дорогу туда не пройти. Скажи ему.]"


async def test_standing_while_it_says_its_line_is_not_stuck(monkeypatch):
    monkeypatch.setattr(gothic_ai.guide, "STUCK_S", 0.1)
    guide, link, _, dialogue, _ = _guide()
    dialogue.heard = True  # a live «за мной» takes seconds, and the game starts the walk after it
    link.snapshots[MALETH] = _snap((1000, 0, 0), (800, 0, 0))
    guide.start(MALETH, "Малет", Way(BOOK.places[1], ("NW_FARM1_MILL_01",), (6000.0, 0.0, 0.0)), "D1")
    await _until(lambda: link.walk is not None)
    await asyncio.sleep(0.3)
    assert not [args for action, args in link.acts if action == "go_to_waypoint" and "points" not in args]
    dialogue.heard = False  # said: now standing is stuck
    await _until(lambda: any(a == "go_to_waypoint" and "points" not in args for a, args in link.acts))


async def test_something_else_takes_its_body_the_way_is_over():
    guide, link, _, dialogue, facts = _guide()
    way = Way(BOOK.places[1], ("NW_FARM1_MILL_01",), (6000.0, 0.0, 0.0))
    guide.start(MALETH, "Малет", way, "D1")
    await _until(lambda: link.walk is not None)
    link.walk.set_result("cancelled")  # a fight, an order, the console
    await _until(lambda: MALETH not in guide.leads)
    assert [f["how"] for f in facts] == ["taken"] and len(dialogue.cues) == 1


async def test_the_way_is_a_term_of_a_deal_and_it_sets_off_once_he_says_yes():
    fake = FakeGame(branch="A")
    lead = {"npc_gives": [], "hero_gives": [{"item": GOLD, "count": 5}], "lead": "мельница"}
    offer = FakeStep("Пять монет — и отведу.", [ToolCall("propose_deal", lead, "c1")])
    llm = FakeBackend(script=[offer], delay=0)
    link_cm, dialogue, chronicle, deals = await _setup(fake, llm, MemoryStore(":memory:"))
    mill = BOOK.get("FARM1_MILL")
    assert mill is not None
    way = Way(mill, mill.points, (6000.0, 0.0, 0.0))

    async def way_of(npc: str, what: str, snap: Any) -> tuple[Way | None, str]:
        return (way, "") if what == "мельница" else (None, "ты не знаешь такого места")

    started: list[tuple[Any, ...]] = []
    deals.guide = SimpleNamespace(way=way_of, start=lambda *a: started.append(a))  # type: ignore[assignment]
    try:
        out = await deals.propose(XARDAS, "Ксардас", {"lead": "замок"}, None)
        assert out.output == "Отклонено: ты не знаешь такого места"
        await dialogue.say_as_player(XARDAS, "Отведи меня к мельнице.")
        await _until(lambda: len(_shown(fake)) == 1)
        shown = _shown(fake)[0]
        assert shown["npc_gives"] == [{"item": "", "count": 0, "text": "Проводит: мельница фермы Лобарта"}]
        terms = deals.open[XARDAS].terms()
        assert terms == "ты ведёшь его — мельница фермы Лобарта; герой отдаёт — 5 золотых"

        fake.hero_items = {GOLD: 10}
        await fake.send(type="deal_accept", deal=shown["deal"], version=1)
        await _until(lambda: started != [])
        assert started == [(XARDAS, "Ксардас", way, shown["deal"])]
        assert chronicle.facts("deal")[0]["lead"] == "FARM1_MILL"
        await asyncio.sleep(0.1)
        assert len(llm.calls) == 1  # the guide's turn tells him to come along, not the deal's
    finally:
        await link_cm.__aexit__(None, None, None)
