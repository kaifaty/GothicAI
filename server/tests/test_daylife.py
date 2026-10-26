import asyncio
from collections.abc import AsyncIterator
from contextlib import asynccontextmanager
from dataclasses import dataclass, field, replace
from pathlib import Path
from typing import Any

import pytest

from gothic_ai import daylife, gossip
from gothic_ai import dialogue as dialogue_module
from gothic_ai.bodies import Bodies
from gothic_ai.brain.apply import Decision, GameApplier
from gothic_ai.brain.day import DayMenu, DayOption
from gothic_ai.brain.planner import NpcPlanner, NpcRecord
from gothic_ai.budget import Budget
from gothic_ai.chronicle import Chronicle, Fact
from gothic_ai.daylife import DayDesk, chat_text, routine_entries, trip_options
from gothic_ai.dialogue import DialogueManager
from gothic_ai.gossip import GossipDesk
from gothic_ai.journal import Journal
from gothic_ai.llm import FakeBackend, FakeStep, ToolCall
from gothic_ai.memory import MemoryStore
from gothic_ai.perception import PerceptionDesk
from gothic_ai.persona import PersonaProvider, habits_of
from gothic_ai.places import PlaceBook
from gothic_ai.protocol import PROTOCOL_VERSION, RoutineEntry, RoutineInfo
from gothic_ai.social import DAY, Rumor, Rumors, Social
from gothic_ai.storyguard import StoryGuard
from gothic_ai.talk import TalkDesk
from gothic_ai.trace import TraceWriter
from tests.fake_game import FakeGame, connected
from tests.fakes import Clock, FakeDecider
from tests.test_dialogue import _until
from tests.test_director import LOBART, QUEST, VINO, XARDAS
from tests.test_memory import close_memory_stores as close_memory_stores
from tests.test_planner import make, step
from tests.test_talk import person

MALETH, HILDA = "BAU_954_MALETH", "BAU_951_HILDA"
VATRAS, BARTOK = "VLK_439_VATRAS", "VLK_440_BARTOK"
T1, T2 = "VLK_901_TOWNSMAN", "VLK_902_TOWNSWOMAN"  # two townsmen 50 m from the temple and the tavern
P, Q = "VLK_903_PAUL", "VLK_904_QUINT"  # a pair that meets at 11:00
GUARD = "MIL_905_GUARD"
SHEEP_09 = "NW_FARM1_PATH_CITY_SHEEP_09"
TEMPLE = "NW_CITY_MERCHANT_TEMPLE_IN"
TAVERN = "NW_CITY_MERCHANT_TAVERN01_FRONT"
TITLE = "Убрать поле репы"
KEY = f"caught:{LOBART}:{QUEST}"
AT = DAY + 10 * 60  # day 1, 10:00
BOOK = PlaceBook.load()
# the scene near the hero (scenarios' DAY_LINES): Vino tells, Malet answers
LINES = [
    ("Вино", "Слыхал? Чужак Лобарту про репу наврал, а тот его и поймал.", "neutral"),
    ("Малет", "Вот как? Пригляжу за ним у стада.", "neutral"),
    ("Вино", "Пригляди. Мне-то что, лишь бы выпивку ставил.", "neutral"),
]


def _person(npc: str, name: str, guild: int, *day: tuple[int, int, str, str, list[int]]) -> dict[str, Any]:
    routine = [{"start": s, "end": e, "state": st, "waypoint": wp, "pos": pos} for s, e, st, wp, pos in day]
    return {"npc": npc, "name": name, "guild": guild, "routine": routine}


BED = [14753, 1844, -16778]  # the stable's bed: Vino and Malet sleep there, they meet only at night
PEOPLE_DAY = [
    # the farm, from the routines' dump (SCENARIO=places)
    _person(
        LOBART,
        "Лобарт",
        12,
        (450, 1380, "ZS_STAND_GUARDING", "NW_FARM1_LOBART", [13608, 1753, -15600]),
        (1380, 450, "ZS_GOTOBED", "NW_FARM1_INHOUSE_BED_01", [13952, 1647, -13802]),
    ),
    _person(
        HILDA,
        "Хильда",
        12,
        (480, 1320, "ZS_COOK_STOVE", "NW_FARM1_INHOUSE_02", [14678, 1642, -13417]),
        (1320, 480, "ZS_GOTOBED", "NW_FARM1_INHOUSE_BED_02", [14221, 1647, -13778]),
    ),
    _person(
        VINO,
        "Вино",
        12,
        (480, 720, "ZS_RAKE_FP", "NW_FARM1_FIELD_04", [9703, 1343, -13534]),
        (720, 900, "ZS_RAKE_FP", "NW_FARM1_FIELD_05", [11036, 1409, -11107]),
        (900, 1080, "ZS_RAKE_FP", "NW_FARM1_FIELD_04", [9703, 1343, -13534]),
        (1080, 1320, "ZS_RAKE_FP", "NW_FARM1_FIELD_05", [11036, 1409, -11107]),
        (1320, 480, "ZS_GOTOBED", "NW_FARM1_INSTABLE_BED", BED),
    ),
    _person(
        MALETH,
        "Малет",
        12,
        (480, 1320, "ZS_STAND_GUARDING", SHEEP_09, [12007, 1648, -18670]),
        (1320, 480, "ZS_GOTOBED", "NW_FARM1_INSTABLE_BED", BED),
    ),
    _person(
        XARDAS,
        "Ксардас",
        0,
        (480, 1380, "ZS_READ_BOOKSTAND", "NW_XARDAS_TOWER_IN1_28", [29467, 5198, -15396]),
        (1380, 480, "ZS_READ_BOOKSTAND", "NW_XARDAS_TOWER_IN1_28", [29467, 5198, -15396]),
    ),
    # the city's prayer and tavern spots, from the dump: Vatras prays in the temple 20:10-05:05, Bartok
    # drinks in front of the tavern 21:45-00:45
    _person(VATRAS, "Ватрас", 3, (1210, 305, "ZS_PRAY_INNOS_FP", TEMPLE, [9796, 320, -716])),
    _person(BARTOK, "Барток", 3, (1305, 45, "ZS_STAND_DRINKING", TAVERN, [7510, 178, 836])),
    # no farm spot reaches a temple or a tavern: two townsmen 50 m from both, who know each other
    _person(
        T1,
        "Горожанин",
        3,
        (360, 1290, "ZS_STAND_ARMSCROSSED", "T1_SPOT", [11352, 320, 4035]),
        (1290, 360, "ZS_GOTOBED", "T1_BED", [11352, 320, 9035]),
    ),
    _person(
        T2,
        "Горожанка",
        3,
        (360, 1290, "ZS_SIT_BENCH", "T2_SPOT", [11652, 320, 4035]),
        (1290, 360, "ZS_GOTOBED", "T2_BED", [16652, 320, 9035]),
    ),
    # 40 m apart 09:00-11:00 (no meeting), 10 m apart 11:00-12:00 (a meeting)
    _person(
        P,
        "Пауль",
        3,
        (540, 660, "ZS_STAND_ARMSCROSSED", "P_A", [60000, 0, 0]),
        (660, 720, "ZS_STAND_ARMSCROSSED", "P_C", [60000, 0, 10000]),
        (720, 540, "ZS_GOTOBED", "P_BED", [60000, 0, 30000]),
    ),
    _person(
        Q,
        "Квинт",
        3,
        (540, 660, "ZS_STAND_ARMSCROSSED", "Q_B", [64000, 0, 0]),
        (660, 720, "ZS_STAND_ARMSCROSSED", "Q_D", [61000, 0, 10000]),
        (720, 540, "ZS_GOTOBED", "Q_BED", [70000, 0, 30000]),
    ),
    _person(
        GUARD,
        "Стражник",
        2,
        (360, 1320, "ZS_GUARD_PASSAGE", "G_POST", [90000, 0, 0]),
        (1320, 360, "ZS_GOTOBED", "G_BED", [90000, 0, 20000]),
    ),
]
PEOPLE_BY = {p["npc"]: p for p in PEOPLE_DAY}


@dataclass
class D:
    fake: FakeGame
    llm: FakeBackend
    chronicle: Chronicle
    talk: TalkDesk
    rumors: Rumors
    perception: PerceptionDesk
    guard: StoryGuard
    gossip: GossipDesk
    desk: DayDesk
    planner: NpcPlanner
    decider: FakeDecider
    clock: Clock
    journal: list[dict[str, Any]]
    open: set[tuple[str, str]] = field(default_factory=set)  # the pairs the rumors' gate lets pass

    @property
    def rumor(self) -> Rumor:
        return self.rumors.rumors[KEY]

    def facts(self, kind: str, **match: Any) -> list[Fact]:
        return [f for f in self.chronicle.facts(kind) if all(f.get(k) == v for k, v in match.items())]


@asynccontextmanager
async def _day(
    tmp_path: Path, *, protocol: int = PROTOCOL_VERSION, apply_day: bool = True
) -> AsyncIterator[D]:
    """The parts as cli.py wires them, around the fake game at 10:00 of day 1: the rumors with the people's
    days, the gossip desk, the day desk and a planner that asks the routines from the game and applies the
    do menu. Lobart caught the hero lying, Vino heard it from him today; the gate lets Vino tell Malet."""
    fake, store = FakeGame(branch="A", protocol=protocol), await MemoryStore.open(":memory:")
    fake.auto_idle = True
    fake.hour, fake.minute = 10, 0
    for p in PEOPLE_DAY:
        fake.routines[p["npc"]] = {"entries": [e.model_dump() for e in routine_entries(p)]}
    llm = FakeBackend(script=[_lines(*LINES)], delay=0)
    journal: list[dict[str, Any]] = []
    trace = TraceWriter(tmp_path)
    async with connected(fake) as link:
        dialogue = DialogueManager(
            link, llm, PersonaProvider(None), memory=store, journal=Journal(journal.append), trace=trace
        )
        await dialogue.initialize()
        dialogue.scene_pace = lambda text: 0.05
        chronicle = Chronicle(link, store)
        await chronicle.initialize()
        talk = TalkDesk(link, dialogue)
        rumors = Rumors(link, dialogue, chronicle)
        rumors.social, rumors.clock = Social(PEOPLE_DAY), AT
        perception = PerceptionDesk(link, social=lambda: rumors.social, places=BOOK)
        guard, bodies = StoryGuard(None), Bodies(link)
        scenes = GossipDesk(
            link,
            dialogue,
            talk,
            rumors,
            perception,
            Budget(per_npc_per_min=2, clock=Clock()),
            chronicle,
            None,
            guard,
            bodies,
            trace=trace,
            journal=dialogue.journal,
        )
        desk = DayDesk(
            link, rumors, chronicle, bodies, guard, perception, talk, BOOK, journal=dialogue.journal
        )
        planner, decider, _, clock = make(FakeDecider("chat", 0.9), apply_day=apply_day)
        planner.routine_of = link.routine
        link.listeners.append(planner.on_message)
        scenes.follow(planner)
        desk.follow(planner, scenes)
        await chronicle.record(
            {"fact": "caught", "quest": QUEST, "npc": LOBART, "name": "Лобарт", "title": TITLE, "day": 1}
        )
        await _heard(chronicle, VINO)
        d = D(
            fake,
            llm,
            chronicle,
            talk,
            rumors,
            perception,
            guard,
            scenes,
            desk,
            planner,
            decider,
            clock,
            journal,
        )
        d.open.add((VINO, MALETH))
        rumors.gate = lambda r, a, b: (a, b) in d.open
        try:
            yield d
        finally:  # a walk still under way waits no more
            for task in list(desk._background):
                task.cancel()
            await asyncio.gather(*desk._background, return_exceptions=True)


@pytest.fixture(autouse=True)
def quick(monkeypatch):
    """The breath between two voices, the sight tier's chatter and the news' retries, shorter."""
    monkeypatch.setattr(dialogue_module, "SCENE_GAP_S", 0.05)
    monkeypatch.setattr(gossip, "SVM_GAP_S", 0.05)
    monkeypatch.setattr(daylife, "OFFER_GAP_S", 0.05)


def _lines(*lines: tuple[str, str, str]) -> FakeStep:
    said = [{"speaker": who, "text": text, "mood": mood} for who, text, mood in lines]
    return FakeStep(tool_calls=[ToolCall("lines", {"lines": said, "quiet": False, "reason": "новость"})])


async def _heard(chronicle: Chronicle, npc: str) -> None:
    """`npc` heard Lobart's news from him today (one retelling away)."""
    rumor = {"fact": "rumor", "quest": QUEST, "kind": "caught", "about": LOBART, "about_name": "Лобарт"}
    told = {"title": TITLE, "from": LOBART, "from_name": "Лобарт", "to": npc, "hops": 1, "how": "met"}
    await chronicle.record({**rumor, **told, "day": 1})


def _minute(at: str) -> int:
    h, m = at.split(":")
    return int(h) * 60 + int(m)


def _rec(d: D, npc: str, at: str = "10:00", state: str | None = None, guild: int | None = None) -> NpcRecord:
    """The npc as the planner knows it: in the state its routine has it in at that time."""
    assert d.rumors.social is not None
    p, spot = PEOPLE_BY[npc], d.rumors.social.spot(npc, _minute(at))
    now = spot.state if spot is not None else ""
    return NpcRecord(
        npc, p["name"], p["guild"] if guild is None else guild, state=now if state is None else state
    )


def _menu(d: D, npc: str, at: str = "10:00", state: str | None = None, guild: int | None = None) -> DayMenu:
    """What the desk offers the npc for its next hour, as the planner asks it."""
    info = RoutineInfo(type="routine", npc=npc, entries=routine_entries(PEOPLE_BY[npc]))
    rec = _rec(d, npc, at, state, guild)
    return d.desk.menu(rec, info, *divmod(_minute(at), 60), habits_of(npc))


def _take(d: D, npc: str = VINO, key: str = "chat", at: str = "10:00") -> DayOption:
    """The npc's trip `key` chosen, as the planner hands it to the desk."""
    option = _menu(d, npc, at).do[key]
    d.desk.take(Decision(npc, "plan", key, 0.9, {}, args=option.args), _rec(d, npc, at), option)
    return option


async def _over(d: D, timeout: float = 5.0) -> tuple[str, DayOption, str]:
    """Until the trip ended (its fact written, or not), and nobody walks."""
    await _until(lambda: d.desk.last is not None and not d.desk.walking, timeout)
    assert d.desk.last is not None
    return d.desk.last


async def _plan(d: D, *people: dict[str, Any]) -> None:
    """Who is around the hero (the planner learns the game time from it); then each is asked of its next
    hour, once its routine came from the game."""
    await d.fake.roster(*people)
    await _until(lambda: all(p["id"] in d.planner.npcs for p in people))
    await step(d.planner, d.clock, 281)
    await _until(lambda: len(d.planner.routines) == len(people))
    await _until(lambda: not any(s.fetching for s in d.planner.routines.values()))
    await step(d.planner, d.clock, 1)


def _view(npc: str, name: str, *sees: dict[str, Any], waypoint: str | None = None) -> dict[str, Any]:
    hero = {"id": "PLAYER", "name": "Я", "distance": 900, "seen": True}
    return {"npc": npc, "name": name, "waypoint": waypoint, "time": AT, "hero": hero, "sees": list(sees)}


def _look(npc: str, name: str, metres: float, **extra: Any) -> dict[str, Any]:
    return {"id": npc, "name": name, "distance": metres * 100, **extra}


def test_routine_entries_and_the_chat_text() -> None:
    entries = routine_entries(PEOPLE_BY[VINO])
    assert entries[0] == RoutineEntry(
        start="08:00", end="12:00", state="ZS_RAKE_FP", waypoint="NW_FARM1_FIELD_04"
    )
    assert (entries[-1].start, entries[-1].end, entries[-1].state) == ("22:00", "08:00", "ZS_GOTOBED")
    # the probe's control: the same walk without its reason
    assert chat_text("Малет", None, None, 61) == "go to Малет (about an hour)"
    assert chat_text("Малет", "by the sheep", "the hero lied to Лобарт", 30) == (
        "go to Малет by the sheep to tell the news: the hero lied to Лобарт (about half an hour)"
    )


async def test_fresh_news_offers_a_walk_to_tell_it(tmp_path, monkeypatch):
    async with _day(tmp_path) as d:
        menu = _menu(d, VINO)
        # his other entries are the same rake elsewhere, or his bed: only the walk to Malet
        assert list(menu.do) == ["keep_routine", "chat"] and menu.stay is None
        assert menu.do["chat"] == DayOption(
            "chat",
            f"chat:{MALETH}",
            "go to Малет on the road from Lobart's farm to Khorinis to tell the news: the hero lied to Лобарт"
            " (about an hour)",
            {"state": "ZS_STAND_WP", "waypoint": SHEEP_09, "minutes": 61},  # 56.4 m: 16 min there, 45 to stay
            "chat",
            f"news:{KEY}",
            MALETH,
            trip=True,
        )
        assert d.desk.menus[VINO] is menu and list(menu.plan[0]) == ["keep_routine"]
        assert _menu(d, VINO, "20:30").do["chat"].args == {
            "state": "ZS_STAND_WP",
            "waypoint": SHEEP_09,
            "minutes": 66,  # from the other field, 76 m
        }

        def offered(at: str = "10:00") -> bool:
            return "chat" in _menu(d, VINO, at).do

        r = d.rumor
        r.known[MALETH] = 2  # Malet knows it already
        assert not offered()
        del r.known[MALETH]
        d.open.clear()  # the lot says no today
        assert not offered()
        d.open.add((VINO, MALETH))
        d.rumors.clock = 3 * DAY + 600  # two days on: no longer news
        assert not offered()
        d.rumors.clock = AT
        r.silent.add(VINO)  # he promised the hero to keep it to himself
        assert not offered()
        r.silent.clear()
        d.rumors.dead.add(MALETH)
        assert not offered()
        d.rumors.dead.clear()
        assert not offered("21:30")  # trips by day only
        assert _menu(d, VINO, "23:00").stay == "спит"
        world = d.desk.world(VINO, 23, 0)
        assert world is not None and trip_options(VINO, 12, "ZS_RAKE_FP", world) == []
        world = d.desk.world(VINO, 10, 0)
        assert world is not None and trip_options(VINO, 12, "ZS_RAKE_FP", replace(world, trip_cm=5000)) == []
        with monkeypatch.context() as m:
            m.setattr(d.guard, "plot", lambda npc: npc == VINO)  # a plot npc stays where it stands
            plot = _menu(d, VINO)
            assert plot.stay is None and list(plot.do) == ["keep_routine"]
        assert offered()

        # Paul and Quint meet at 11:00: at 10:30 the schedule passes it anyway, at 09:30 he walks over
        await _heard(d.chronicle, P)
        d.open.add((P, Q))
        assert "chat" not in _menu(d, P, "10:30").do
        chat = _menu(d, P, "09:30").do["chat"]
        assert (chat.partner, chat.args) == (Q, {"state": "ZS_STAND_WP", "waypoint": "Q_B", "minutes": 56})
        assert chat.text == "go to Квинт to tell the news: the hero lied to Лобарт (about an hour)"


async def test_far_from_the_hero_the_news_passes_as_a_visit(tmp_path):
    async with _day(tmp_path) as d:
        d.fake.nearby_of[VINO] = [{"npc": MALETH, "name": "Малет", "distance": 300}]
        await _plan(d, person(VINO, "Вино", 50, state="ZS_RAKE_FP", guild=12))
        npc, option, outcome = await _over(d)

    (call,) = d.decider.calls
    assert list(call["questions"]) == ["do"] and "chat" in call["questions"]["do"]["criteria"]
    assert (npc, option.id, outcome) == (VINO, f"chat:{MALETH}", "told") and d.desk.trips == 1
    (act,) = d.fake.acts
    assert (act["npc"], act["action"]) == (VINO, "start_activity")
    assert act["args"] == {
        "state": "ZS_STAND_WP",
        "waypoint": SHEEP_09,
        "minutes": 61,
        "arrive": True,
        "radius": 500,
    }
    (told,) = d.facts("rumor", to=MALETH)
    assert (told["from"], told["hops"], told["how"]) == (VINO, 2, "visited")
    (fact,) = d.facts("day")
    assert fact == {
        "fact": "day",
        "npc": VINO,
        "name": "Вино",
        "what": "chat",
        "to": MALETH,
        "to_name": "Малет",
        "concern": f"news:{KEY}",
        "where": "FARM1_PATH_CITY",
        "minutes": 61,
        "outcome": "told",
        "seen_by": [],  # 50 m from the hero, off his screen
        "day": 1,
        "hour": 10,
        "minute": 0,
    }
    assert not d.llm.calls and not d.fake.says and not d.desk.walking


async def test_a_closed_lot_asks_nothing_until_opened(tmp_path):
    """The day e2e's order: while the gate holds the pair closed his do menu is the routine alone and he
    is asked nothing; opened and looked at now, the first question he gets has the walk."""
    async with _day(tmp_path) as d:
        d.open.clear()
        d.fake.nearby_of[VINO] = [{"npc": MALETH, "name": "Малет", "distance": 300}]
        await _plan(d, person(VINO, "Вино", 50, state="ZS_RAKE_FP", guild=12))
        rec = d.planner.npcs[VINO]
        assert list(d.desk.menus[VINO].do) == ["keep_routine"]  # he was looked at: a menu without the walk
        assert not d.decider.calls and rec.flight is None and not d.fake.acts
        d.open.add((VINO, MALETH))
        rec.next_check = d.clock.t
        await step(d.planner, d.clock)
        _, option, outcome = await _over(d)

    (call,) = d.decider.calls
    assert list(call["questions"]) == ["do"] and "chat" in call["questions"]["do"]["criteria"]
    assert (option.id, outcome) == (f"chat:{MALETH}", "told") and len(d.fake.acts) == 1


async def test_near_the_hero_the_two_say_it(tmp_path):
    async with _day(tmp_path) as d:
        d.fake.nearby_of[VINO] = [{"npc": MALETH, "name": "Малет", "distance": 300}]
        # Malet sees Vino 3 m off once he is there (his view gives the gap; he knows nothing to tell)
        await d.fake.push_views(_view(MALETH, "Малет", _look(VINO, "Вино", 3)))
        await _plan(
            d,
            person(VINO, "Вино", 8, True, state="ZS_RAKE_FP", guild=12),
            person(MALETH, "Малет", 10, state="ZS_STAND_GUARDING", guild=12),
        )
        _, _, outcome = await _over(d)
        await _until(lambda: d.gossip.last is not None and d.gossip.last.released, 5)
        scene = d.gossip.last

    assert outcome == "told" and len(d.llm.calls) == 1
    assert scene is not None and (scene.a, scene.b, scene.state) == (VINO, MALETH, "done")
    assert [(m["npc"], m["to"]) for m, _ in d.fake.says] == [(VINO, MALETH), (MALETH, VINO), (VINO, MALETH)]
    (told,) = d.facts("rumor", to=MALETH)
    assert (told["from"], told["hops"], told["how"]) == (VINO, 2, "talked")
    (fact,) = d.facts("day")
    assert fact["outcome"] == "told" and fact["seen_by"] == ["PLAYER", MALETH]
    # only Vino was asked: Malet has nothing to choose
    assert [c["rid"].split("#")[0] for c in d.decider.calls] == [VINO]


async def test_a_walking_body_is_kept_from_other_acts(tmp_path):
    async with _day(tmp_path) as d:
        d.fake.idle_delay = 0.5  # the walk takes a while
        d.fake.nearby_of[VINO] = [{"npc": MALETH, "name": "Малет", "distance": 300}]
        _take(d)
        rec = _rec(d, VINO)
        assert d.desk.walking[VINO].id == f"chat:{MALETH}"
        assert d.planner.allow is not None
        assert d.planner.allow(rec, "look_at") == "идёт по делу"  # a newer act would drop its arrival
        assert d.planner.allow(rec, "fight") is None  # a threat still takes the body
        assert d.desk.reserved[SHEEP_09] == {VINO: AT + 61}
        _, _, outcome = await _over(d)
        assert outcome == "told" and d.planner.allow(rec, "look_at") is None


async def test_the_one_to_be_told_is_not_there(tmp_path):
    async with _day(tmp_path) as d:
        d.fake.nearby_of[VINO] = []  # nobody near him once there
        _take(d)
        assert (await _over(d))[2] == "not there"
        d.desk.last = None
        d.fake.nearby_error.add(VINO)  # the bridge does not know him (not in the world)
        _take(d)
        assert (await _over(d))[2] == "not there"
        facts = d.facts("day")

    assert [f["outcome"] for f in facts] == ["not there", "not there"]
    assert not d.facts("rumor", to=MALETH)


async def test_busy_bodies_wait_then_the_news_passes(tmp_path, monkeypatch):
    async with _day(tmp_path) as d:
        # both on the hero's screen but out of earshot; how far apart they are is not known yet
        await d.fake.roster(
            person(VINO, "Вино", 17, True, state="ZS_RAKE_FP", guild=12),
            person(MALETH, "Малет", 18, True, state="ZS_STAND_GUARDING", guild=12),
        )
        await _until(lambda: len(d.talk.people) == 2)
        d.fake.nearby_of[VINO] = [{"npc": MALETH, "name": "Малет", "distance": 300}]
        statuses: list[str] = []
        offer = d.rumors.offer_status

        async def recorded(r: Rumor, a: str, b: str, how: str = "met") -> str:
            statuses.append(await offer(r, a, b, how))
            return statuses[-1]

        monkeypatch.setattr(d.rumors, "offer_status", recorded)
        _take(d)
        _, _, outcome = await _over(d)

    assert statuses[0] == "wait" and statuses[-1] == "told" and outcome == "told"
    (told,) = d.facts("rumor", to=MALETH)
    assert told["how"] == "visited" and told["seen_by"] == ["PLAYER"]  # seen, not heard: the sight tier
    assert not d.llm.calls


async def test_the_view_trigger_got_there_first(tmp_path):
    async with _day(tmp_path) as d:
        d.fake.idle_delay = 0.3
        d.fake.nearby_of[VINO] = [{"npc": MALETH, "name": "Малет", "distance": 300}]
        await d.fake.roster(person(VINO, "Вино", 50, state="ZS_RAKE_FP", guild=12))
        await _until(lambda: VINO in d.talk.people)
        _take(d)
        # on his way he sees Malet: two acquaintances, the lot of a meeting (gossip.py)
        await d.fake.push_views(_view(VINO, "Вино", _look(MALETH, "Малет", 3)))
        await _until(lambda: d.facts("rumor", to=MALETH))
        _, _, outcome = await _over(d)

    assert outcome == "told"
    (told,) = d.facts("rumor", to=MALETH)  # not twice
    assert told["how"] == "met"


async def test_a_death_seen_is_prayed_for(tmp_path):
    async with _day(tmp_path) as d:
        body = _look(MALETH, "Малет", 4, kind="body", guild=12)
        seen = _view(VINO, "Вино", body, waypoint="NW_FARM1_FIELD_04")
        await d.fake.push_views(seen)
        await d.fake.push_views(seen)  # once per one dead
        # a creature's body, and a person's he does not know: nothing
        wolf = _look("WOLF", "Волк", 6, kind="body", guild=34)
        await d.fake.push_views(_view(VINO, "Вино", wolf, _look(GUARD, "Стражник", 8, kind="body", guild=2)))
        await d.fake.push_views(_view(T1, "Горожанин", _look(T2, "Горожанка", 2, kind="body", guild=3)))
        await _until(lambda: len(d.facts("death_seen")) == 2)
        vino, t1 = d.facts("death_seen")
        assert vino == {
            "fact": "death_seen",
            "npc": VINO,
            "name": "Вино",
            "dead": MALETH,
            "dead_name": "Малет",
            "where": "FARM1_FIELD",
            "day": 1,
            "hour": 10,
            "minute": 0,
        }
        assert (t1["npc"], t1["dead"]) == (T1, T2)

        menu = _menu(d, T1)
        assert list(menu.do) == ["keep_routine", "pray"]
        assert menu.do["pray"] == DayOption(
            "pray",
            f"pray:{T2}",
            "pray to Innos for Горожанка in the temple by the market square (about an hour)",
            {"state": "ZS_PRAY_INNOS_FP", "waypoint": TEMPLE, "minutes": 59},  # 50 m: 14 min there
            "pray",
            f"death:{T2}",
            trip=True,
        )
        assert "pray" not in _menu(d, T1, "20:30").do  # Vatras prays there then: the temple holds one
        assert "pray" not in _menu(d, T1, state="ZS_PRAY_INNOS_FP").do  # it prays already
        assert "pray" in _menu(d, T1).do
        await d.chronicle.record(
            {"fact": "day", "npc": T1, "what": "pray", "concern": f"death:{T2}", "day": 1}
        )
        assert "pray" not in _menu(d, T1).do  # prayed for


async def test_a_loss_is_drunk_away_in_a_tavern(tmp_path):
    async with _day(tmp_path) as d:
        d.fake.auto_idle = False  # his walk goes on
        await d.chronicle.record(
            {"fact": "found_out", "quest": "Q1", "npc": T1, "name": "Горожанин", "day": 1}
        )
        await d.chronicle.record({"fact": "paid", "quest": "Q1", "item": "ITMI_GOLD", "count": 50})
        await d.chronicle.record({"fact": "broken", "debt": "D1", "npc": T2, "name": "Горожанка", "day": 1})
        menu = _menu(d, T1)
        assert list(menu.do) == ["keep_routine", "tavern"]
        assert menu.do["tavern"] == DayOption(
            "tavern",
            "tavern:found_out:Q1",
            "drink away the reward the hero cheated you of in the tavern by the market square"
            " (about an hour)",
            {"state": "ZS_STAND_DRINKING", "waypoint": TAVERN, "minutes": 59},
            "tavern",
            "loss:found_out:Q1",
            trip=True,
        )
        owed = _menu(d, T2).do["tavern"]
        assert (owed.concern, owed.args) == ("loss:broken:D1", {**menu.do["tavern"].args, "minutes": 60})
        assert owed.text.startswith("drink away the money the hero did not pay back in the tavern")
        assert "tavern" not in _menu(d, T1, guild=9).do  # an outlaw
        assert "tavern" not in _menu(d, T1, state="ZS_STAND_DRINKING").do  # it drinks already
        _take(d, T1, "tavern")
        assert "tavern" not in _menu(d, T2).do  # the spot in front of the tavern holds one: his now
        await _until(lambda: d.fake.acts)
        assert d.fake.acts[-1]["args"]["state"] == "ZS_STAND_DRINKING"


async def test_a_spot_with_room_for_two_takes_two_trips(tmp_path):
    """A spot the routines fill with two at once (a second drinker beside Bartok at night) holds two trips:
    the menu offers it while one trip holds it, the backstop agrees, and each trip holds and lets go of its
    own place."""
    drinker = _person(
        "VLK_441_DRINKER", "Пьяница", 3, (1320, 1380, "ZS_STAND_DRINKING", TAVERN, [7510, 178, 836])
    )
    async with _day(tmp_path) as d:
        d.rumors.social = Social([*PEOPLE_DAY, drinker])
        assert d.rumors.social.capacity(TAVERN) == 2
        d.fake.auto_idle = False  # their walks go on
        await d.chronicle.record(
            {"fact": "found_out", "quest": "Q1", "npc": T1, "name": "Горожанин", "day": 1}
        )
        await d.chronicle.record({"fact": "broken", "debt": "D1", "npc": T2, "name": "Горожанка", "day": 1})
        _take(d, T1, "tavern")
        assert "tavern" in _menu(d, T2).do  # one place of two is held
        rec = _rec(d, T2)
        assert d.planner.allow(rec, "tavern") is None  # the backstop counts as the menu does
        d.desk.reserved[TAVERN][LOBART] = AT + 60  # a third trip: no room any more
        assert d.planner.allow(rec, "tavern") == "место занято"
        del d.desk.reserved[TAVERN][LOBART]
        _take(d, T2, "tavern")
        assert d.desk.reserved[TAVERN] == {T1: AT + 59, T2: AT + 60}
        world = d.desk.world(LOBART, 10, 0)
        assert world is not None and not world.free(TAVERN, LOBART)  # both places held: none for a third
        await _until(lambda: len(d.fake.acts) == 2)
        # the townsman's walk ends before he got there: his place is let go, hers is kept
        first = next(a["id"] for a in d.fake.acts if a["npc"] == T1)
        await d.fake.send(type="npc_idle", npc=T1, act=first, reason="done")
        await _until(lambda: T1 not in d.desk.walking)
        assert d.desk.reserved[TAVERN] == {T2: AT + 60}


async def test_a_body_taken_over_on_its_way_ends_the_trip(tmp_path):
    """A fight or a flight on the way (the applier's npc_act, past the desk) drops the walk's tracking in the
    bridge: the trip ends at once as one that never got there, not after its timeout, and the npc is free
    for the planner again."""
    async with _day(tmp_path) as d:
        d.fake.auto_idle = False
        option = _take(d)
        await _until(lambda: d.fake.acts)
        rec = _rec(d, VINO)
        assert d.planner.allow(rec, "fight") is None  # a threat still takes the body
        GameApplier(d.desk.link).apply(Decision(VINO, "react", "fight", 1.0, {}, target="WOLF"))
        assert await _over(d, timeout=2) == (VINO, option, "failed: gone")
        fight = d.fake.acts[-1]
        assert (fight["action"], fight["args"]) == ("fight", {"npc": "WOLF"})
        assert not d.desk.walking and not d.desk.reserved and not d.desk.bodies.busy(VINO)
        (failed,) = [e for e in d.journal if e["kind"] == "day_failed"]
        assert failed["why"] == f"gone: replaced by {fight['id']}"
        assert d.planner.allow(rec, "look_at") is None and not d.facts("day")


async def test_the_story_keeps_plot_npcs_and_posts(tmp_path, monkeypatch):
    async with _day(tmp_path) as d:
        xardas = _menu(d, XARDAS)
        assert xardas.stay is None and list(xardas.do) == ["keep_routine", "h1"]
        assert xardas.do["h1"].args == {
            "state": "ZS_STAND_ARMSCROSSED",
            "waypoint": "NW_XARDAS_TOWER_IN1_28",
            "minutes": 60,
        }
        assert d.planner.allow is not None
        assert d.planner.allow(_rec(d, XARDAS), "h1") is None  # his habit is where he stands
        post = _menu(d, GUARD)
        assert (post.stay, post.plan, post.do) == ("стоит на посту", ({}, {}), {})

        # the backstop: a trip in Vino's menu, then he is the plot's, or his spot is someone else's
        _menu(d, VINO)
        rec = _rec(d, VINO)
        assert d.planner.allow(rec, "chat") is None
        d.desk.reserved[SHEEP_09] = {LOBART: AT + 60}
        assert d.planner.allow(rec, "chat") == "место занято"
        d.desk.reserved.clear()
        monkeypatch.setattr(d.guard, "plot", lambda npc: npc == VINO)
        assert d.planner.allow(rec, "chat") == "сюжетный: только слова"


async def test_a_load_ends_the_trip(tmp_path):
    async with _day(tmp_path) as d:
        d.fake.auto_idle = False
        _take(d)
        await _until(lambda: d.fake.acts)
        await d.fake.send(type="loading")
        await _until(lambda: not d.desk._background)

    assert not d.desk.walking and not d.desk.reserved and not d.desk.menus
    assert d.desk.last is None and not d.facts("day")
    assert not [e for e in d.journal if e["kind"] == "day_failed"]


async def test_a_trip_that_never_got_there(tmp_path):
    async with _day(tmp_path) as d:
        d.fake.auto_idle = False
        option = _take(d)
        await _until(lambda: d.fake.acts)
        # the state was entered and left before he got there
        await d.fake.send(type="npc_idle", npc=VINO, act=d.fake.acts[-1]["id"], reason="done")
        assert await _over(d) == (VINO, option, "failed: done")
        assert not d.desk.reserved and not d.facts("day")
        (failed,) = [e for e in d.journal if e["kind"] == "day_failed"]
        assert (failed["npc"], failed["id"], failed["why"]) == (VINO, f"chat:{MALETH}", "done")
        assert "chat" in _menu(d, VINO).do  # the next hour offers it again


async def test_an_older_bridge_gets_no_trips(tmp_path):
    async with _day(tmp_path, protocol=14) as d:
        assert list(_menu(d, VINO).do) == ["keep_routine"]


async def test_the_desk_chains_before_the_gossip_veto(tmp_path):
    async with _day(tmp_path) as d:
        assert d.planner.menu_of == d.desk.menu and d.planner.on_plan == d.desk.take
        d.gossip._sight_busy.add(VINO)  # a sight act of the two talking
        assert d.planner.allow is not None
        assert d.planner.allow(_rec(d, VINO), "look_at") == "болтает"  # the gossip desk's, after this one
        _menu(d, VINO)
        assert d.planner.allow(_rec(d, VINO), "chat") == "занят"
