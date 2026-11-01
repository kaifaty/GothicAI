"""A body first seen (v58): one trace_found, suspicion only by the chest's rule, and memory across a load."""

import asyncio
import json
from types import SimpleNamespace
from typing import Any

import pytest

from gothic_ai.brain.situation import SituationBuilder
from gothic_ai.brain.stimuli import Stimulus
from gothic_ai.chronicle import Chronicle
from gothic_ai.crimes import SUSPECT_MIN, Crimes
from gothic_ai.found_traces import JUDGE_S, PRESENT_MIN, FoundTraces
from gothic_ai.memory import MemoryStore
from gothic_ai.npc_crimes import NpcCrimes
from gothic_ai.perception import PerceptionDesk
from gothic_ai.protocol import BODY_TRACE_PROTOCOL, PROTOCOL_VERSION, NpcView, parse_game_message
from gothic_ai.sightings import Sighting, Sightings
from tests.fakes import Clock
from tests.test_perception import HILDA, LOBART, MALETH, Social, look, physical_trace
from tests.test_sightings import AT_8, BOOK, Link
from tests.test_sightings import sightings as make_sightings

BODY_ID = "NEWWORLD.ZEN:body:3"  # the world numbers its dead; not the npc symbol
KEY = f"TRACE:{BODY_ID}:body"
YARD, PATH = "NW_FARM1_OUT_01", "NW_FARM1_PATH_01"  # FARM1_OUT, FARM1_PASTURE


class Planner:
    def __init__(self) -> None:
        self.events: list[tuple[str, str, dict[str, Any]]] = []

    def on_event(self, npc: str, kind: str, now: float, **kw: Any) -> None:
        self.events.append((npc, kind, kw))


def body(age: int | None = 50, waypoint: str = "NW_FARM1_OUT_02", **extra: Any) -> dict:
    return physical_trace(BODY_ID, "body", "тело", npc=MALETH, age=age, waypoint=waypoint, **extra)


def seen_view(time: int, *, hero_at: str | None = YARD, hilda_at: str | None = None) -> NpcView:
    """Lobart sees the hero (and Hilda, and Maleth alive) at a place, before the body is found."""
    hero = look("PLAYER", "Я", 8, seen=hero_at is not None, waypoint=hero_at or YARD)
    sees = [look(MALETH, "Малет", 4, waypoint=YARD)]
    if hilda_at is not None:
        sees.append(look(HILDA, "Хильда", 6, waypoint=hilda_at))
    return NpcView.model_validate(
        {
            "npc": LOBART,
            "name": "Лобарт",
            "waypoint": YARD,
            "time": time,
            "hero": hero,
            "sees": sees,
        }
    )


def found_view(time: int, *traces: dict, dead: dict | None = None) -> NpcView:
    """Lobart alone with Maleth's body, the hero far away and unseen, and the trace(s)."""
    dead = dead if dead is not None else look(MALETH, "Малет", 3, kind="body", body="dead", waypoint=YARD)
    return NpcView.model_validate(
        {
            "npc": LOBART,
            "name": "Лобарт",
            "waypoint": YARD,
            "time": time,
            "hero": look("PLAYER", "Я", 40, seen=False),
            "traces": list(traces) or [body()],
            "sees": [dead] if dead else [],
        }
    )


class World:
    def __init__(self, store: MemoryStore, *, protocol: int | None = None, branch: str = "A") -> None:
        self.link = Link()
        self.link.session.branch = branch
        if protocol is not None:
            self.link.hello = SimpleNamespace(protocol=protocol)
        self.seen: Sightings = make_sightings(store, self.link)
        self.chronicle: Chronicle = self.seen.chronicle
        self.crimes = Crimes(self.link, self.chronicle, places=BOOK)  # type: ignore[arg-type]
        self.crimes.sightings = self.seen
        self.seen.crimes = self.crimes
        self.npc_crimes = NpcCrimes(self.link, self.chronicle, crimes=self.crimes)  # type: ignore[arg-type]
        self.clock = Clock()
        self.found = FoundTraces(self.seen, crimes=self.crimes, npc_crimes=self.npc_crimes, clock=self.clock)
        self.planner = Planner()
        self.found.follow(self.planner)  # type: ignore[arg-type]
        self.finds: list[Any] = []
        self.found.on_found.append(self.finds.append)

    def saw(self, *views: NpcView, judge: bool = True) -> None:
        """The views as the desk hands them out; then, unless `judge` is False, a later view of the last
        viewer after JUDGE_S, which decides its finds (only after a view with traces: it extends
        sightings)."""
        for v in views:
            self.seen.desk._seen(v)
        if judge and views and views[-1].traces:
            self.later(views[-1])

    def later(self, view: NpcView, seconds: float = JUDGE_S + 1) -> None:
        self.clock.t += seconds
        self.seen.desk._seen(view.model_copy(update={"time": (view.time or 0) + 1}))

    def suspected(self) -> list[dict]:
        return [*self.chronicle.facts("crime_suspected"), *self.chronicle.facts("npc_crime_suspected")]

    def stimuli(self) -> list[tuple[str, str, dict[str, Any]]]:
        return [e for e in self.planner.events if e[1] == "trace_found"]


@pytest.fixture
def store():
    s = MemoryStore(":memory:")
    s.ensure_branch("A")
    yield s
    s.close()


def test_v58_body_trace_parses_with_the_dead_npc():
    assert PROTOCOL_VERSION >= BODY_TRACE_PROTOCOL == 58
    msg = parse_game_message(
        json.dumps(
            {
                "type": "npc_views",
                "t": 1,
                "views": [
                    {
                        "npc": LOBART,
                        "name": "Лобарт",
                        "time": AT_8,
                        "traces": [
                            {
                                "id": BODY_ID,
                                "kind": "body",
                                "name": "тело",
                                "npc": MALETH,
                                "waypoint": "NW_FARM1_OUT_02",
                                "distance": 300,
                                "age": None,
                            },
                        ],
                    }
                ],
            }
        )
    )
    (trace,) = msg.views[0].traces  # type: ignore[union-attr]
    assert (trace.kind, trace.npc, trace.age) == ("body", MALETH, None)


@pytest.mark.parametrize("protocol", [BODY_TRACE_PROTOCOL - 1, BODY_TRACE_PROTOCOL])
async def test_an_older_bridge_has_no_body_traces(store, protocol):
    w = World(store, protocol=protocol)
    w.saw(seen_view(AT_8), found_view(AT_8 + 60, physical_trace(), body()))
    newer = protocol >= BODY_TRACE_PROTOCOL
    assert (KEY in w.seen.mind(LOBART).traces) == newer
    assert "TRACE:vob:12:moved" in w.seen.mind(LOBART).traces  # older kinds stay
    assert bool(w.stimuli()) == newer and bool(w.suspected()) == newer
    assert ("— тело: Малет; во дворе" in (w.seen.situation(LOBART) or "")) == newer  # the trace line
    desk = PerceptionDesk(w.link, social=Social, places=BOOK, clock=Clock())  # type: ignore[arg-type]
    desk._store(found_view(AT_8 + 60, physical_trace(), body()))
    sight = desk.sight(LOBART)
    assert sight is not None and ("тело: Малет (" in (sight.traces or "")) == newer
    assert "Стол" in (sight.traces or "")
    await asyncio.gather(*w.seen._background, *w.chronicle._background)


async def test_the_first_sight_of_a_body_is_one_find_and_suspicion_of_those_seen_there(store):
    w = World(store)
    w.saw(seen_view(AT_8, hilda_at="NW_FARM1_OUT_02"))
    w.saw(found_view(AT_8 + 60))  # died at AT_8 + 10: the hero and Hilda were there 10 minutes before
    w.saw(found_view(AT_8 + 61), found_view(AT_8 + 90))  # shown again: no second find
    (find,) = w.finds
    assert (find.npc, find.body, find.died, find.place) == (LOBART, MALETH, AT_8 + 10, "FARM1_OUT")
    (stimulus,) = w.stimuli()
    npc, _, kw = stimulus
    assert npc == LOBART and kw["other"] == MALETH and kw["other_name"] == "Малет"
    assert kw["detail"] == "dead for about 50 minutes"
    assert kw["context"] == ""  # whom it suspects is decided later, not in the stimulus
    assert {f["suspect"] for f in w.suspected()} == {"PLAYER", HILDA}
    assert [(f["npc"], f["trace"], set(f["suspects"])) for f in w.chronicle.facts("trace_judged")] == [
        (LOBART, BODY_ID, {"PLAYER", HILDA})
    ]
    hero = w.chronicle.facts("crime_suspected")
    npcs = w.chronicle.facts("npc_crime_suspected")
    assert [(f["npc"], f["suspect"], f["crime"], f["body"], f["died"]) for f in hero] == [
        (LOBART, "PLAYER", "murder", MALETH, AT_8 + 10)
    ]
    assert [(f["npc"], f["suspect"], f["crime"], f["body"]) for f in npcs] == [
        (LOBART, HILDA, "murder", MALETH)
    ]
    assert hero[0]["sighting"]["who"] == "PLAYER" and "FARM1_OUT" in hero[0]["sighting"]["place_ids"]
    assert all(f["suspect"] not in {MALETH, LOBART} for f in w.suspected())
    assert w.crimes.known(LOBART) == []  # a suspicion without a deed id is no knowledge of a real deed
    situation = w.seen.situation(LOBART) or ""
    line = next(row for row in situation.splitlines() if "тело: Малет" in row)
    assert "смерть примерно за 50 мин до того" in line and "здесь кого-то убили" in line
    assert "герой, Хильда" in line and "подозрение, не доказательство" in line
    assert "Около того времени" in line
    await asyncio.gather(*w.seen._background, *w.chronicle._background)


@pytest.mark.parametrize(
    "case",
    ["nobody_seen", "other_place", "before_window", "after_window", "unknown_age", "no_place", "witnessed"],
)
async def test_no_suspicion_without_evidence(store, case):
    w = World(store)
    found_at, age, waypoint = AT_8 + 60, 50, "NW_FARM1_OUT_02"
    if case == "nobody_seen":
        w.saw(seen_view(AT_8, hero_at=None))
    elif case == "other_place":
        w.saw(seen_view(AT_8, hero_at=PATH, hilda_at=PATH))
    elif case == "before_window":  # seen at 08:10, died at 08:10 + 31 + ... beyond half an hour
        w.saw(seen_view(AT_8, hilda_at=YARD))
        age = 60 - SUSPECT_MIN - 1  # died at AT_8 + 31
    elif case == "after_window":
        w.saw(seen_view(AT_8 + 2 * SUSPECT_MIN, hilda_at=YARD))
        found_at, age = AT_8 + 2 * SUSPECT_MIN, 2 * SUSPECT_MIN - 10 + SUSPECT_MIN + 1  # died 31 before
    elif case == "unknown_age":
        w.saw(seen_view(AT_8, hilda_at=YARD))
        age = None
    elif case == "no_place":
        w.saw(seen_view(AT_8, hilda_at=YARD))
        waypoint = "NW_SOMEWHERE_ELSE"
    elif case == "witnessed":
        w.saw(seen_view(AT_8, hilda_at=YARD))
        w.chronicle.record(
            {
                "fact": "crime",
                "id": "M1",
                "kind": "murder",
                "victim": MALETH,
                "waypoint": YARD,
                "day": 1,
                "hour": 8,
                "minute": 20,
            }
        )
        w.chronicle.record({"fact": "crime_seen", "npc": LOBART, "crime": "M1"})
    w.saw(found_view(found_at, body(age=age, waypoint=waypoint)))
    assert w.suspected() == []
    (find,) = w.finds
    assert len(w.chronicle.facts("trace_judged")) == 1
    assert len(w.stimuli()) == 1
    context = w.stimuli()[0][2]["context"]
    line = next(row for row in (w.seen.situation(LOBART) or "").splitlines() if "тело: Малет" in row)
    if case == "witnessed":
        assert "ты видел это сам" in line and "saw the killing" in context
    else:
        assert "кто это сделал — не знаешь" in line and context == ""
    if case == "unknown_age":
        assert "когда умер — не знаешь" in line and find.died is None
    await asyncio.gather(*w.seen._background, *w.chronicle._background)


async def test_the_dead_the_finder_another_body_and_a_beast_are_never_suspects(store):
    w = World(store)
    mind = w.seen.mind(LOBART)
    for who, notable in ((MALETH, []), (LOBART, []), ("BAU_955_DEAD", ["dead"]), ("WOLF", [])):
        mind.sightings.append(
            Sighting(who, who, AT_8, AT_8, ["во дворе"], place_ids=["FARM1_OUT"], notable=notable)
        )
    w.crimes.people["WOLF"] = {"guild": 34}  # GIL_WOLF: not a person
    w.saw(found_view(AT_8 + 60))
    assert w.suspected() == []
    assert len(w.finds) == 1


async def test_the_find_and_its_suspicion_follow_the_save_branch(tmp_path):
    path = str(tmp_path / "memory.sqlite3")
    store = MemoryStore(path)
    store.ensure_branch("A")
    w = World(store)
    w.saw(seen_view(AT_8), found_view(AT_8 + 60))
    await asyncio.gather(*w.seen._background, *w.chronicle._background)
    head = store.head("A")
    (record,) = [json.loads(r) for r in store.seen("A", LOBART) if "trace_kind" in json.loads(r)]
    assert (record["trace_kind"], record["trace_npc"], record["trace_age"]) == ("body", MALETH, 50)
    assert "culprit" not in record and "other" not in record
    store.close()

    store = MemoryStore(path)
    store.ensure_branch("loaded", "A", head)
    again = World(store, branch="loaded")
    again.saw(found_view(AT_8 + 120))  # the same body after a load: remembered, not found anew
    assert again.finds == [] and again.stimuli() == []
    assert [f["suspect"] for f in again.suspected()] == ["PLAYER"]
    line = next(row for row in (again.seen.situation(LOBART) or "").splitlines() if "тело: Малет" in row)
    assert "здесь кого-то убили" in line and "герой" in line
    store.ensure_branch("before", "A", 0)
    before = World(store, branch="before")
    before.saw(found_view(AT_8 + 120))  # a save from before the find: it finds the body now, nobody seen
    assert len(before.finds) == 1 and before.suspected() == []
    await asyncio.gather(*again.seen._background, *before.seen._background, *before.chronicle._background)
    store.close()


def test_trace_found_is_a_salient_not_urgent_stimulus_with_its_place():
    stimulus = Stimulus(
        "trace_found",
        0.0,
        MALETH,
        300,
        "dead for about 50 minutes",
        "Малет",
        " You do not know who did it.",
        place=YARD,
    )
    assert stimulus.policy.salient and not stimulus.policy.urgent and not stimulus.policy.scripted
    from gothic_ai.brain.situation import NpcContext

    sit = SituationBuilder(places=BOOK).react(NpcContext("BAU_950_LOBART", "Лобарт"), [stimulus], 0.0, None)
    assert (
        "You found the body of Малет in Lobart's farmyard, dead for about 50 minutes. "
        "Someone was killed here. You do not know who did it." in sit.state
    )
    assert "call_guards" in sit.questions["react"]["criteria"]
    nowhere = SituationBuilder().react(NpcContext("BAU_950_LOBART", "Лобарт"), [stimulus], 0.0, None)
    assert "Малет nearby" in nowhere.state and YARD not in nowhere.state


async def test_the_planner_keeps_a_find_without_blaming_the_hero():
    from tests.test_planner import entry, make, step

    planner, decider, _, clock = make()
    planner.on_roster([entry("BAU_950_LOBART", 12)], clock.t)
    planner.on_event(
        "BAU_950_LOBART",
        "trace_found",
        clock.t,
        other=MALETH,
        other_name="Малет",
        detail="dead for about 50 minutes",
        place=YARD,
        context=" You do not know who did it.",
    )
    planner.on_event("BAU_950_LOBART", "trace_found", clock.t, other="", other_name="следы")
    rec = planner.npcs["BAU_950_LOBART"]
    assert rec.last_interaction is None and {s.other for s in rec.stimuli} == {MALETH, ""}
    await step(planner, clock, 1.0)
    assert decider.calls and "You found the body of Малет" in decider.calls[0]["state"]


MERC = look(
    "SLD_801_Y",
    "Наёмник",
    3,
    kind="body",
    body="dead",
    guild=7,
    armor={"item": "ITAR_SLD_L", "name": "Доспех"},
)


@pytest.mark.parametrize(
    ("dead", "npc", "line", "other_name"),
    [
        ({}, MALETH, "тело; во дворе", "a stranger"),  # the bridge says only «тело»; the body is out of sight
        (look("BAU_960_X", "Батрак", 3, kind="body", body="dead"), MALETH, "тело; во дворе", "a stranger"),
        (look(MALETH, "Малет", 3, kind="body", body="dead"), MALETH, "тело: Малет; во дворе", "Малет"),
        # Lobart never met this mercenary: a stranger by his look
        (MERC, "SLD_801_Y", "тело: незнакомец, по виду наёмник; во дворе", "a stranger"),
    ],
)
async def test_whose_body_is_how_the_finder_sees_it(store, dead, npc, line, other_name):
    trace = {**body(), "npc": npc}
    w = World(store)
    w.saw(found_view(AT_8 + 60, trace, dead=dead))
    situation = w.seen.situation(LOBART) or ""
    assert line in situation and "тело: тело" not in situation
    assert w.stimuli()[0][2]["other_name"] == other_name
    desk = PerceptionDesk(w.link, social=Social, places=BOOK, clock=Clock())  # type: ignore[arg-type]
    desk._store(found_view(AT_8 + 60, trace, dead=dead))
    traces = desk.sight(LOBART).traces or ""  # type: ignore[union-attr]
    assert line.split(";")[0] in traces and "тело: тело" not in traces
    await asyncio.gather(*w.seen._background, *w.chronicle._background)


@pytest.mark.parametrize(("seen_before", "there"), [(PRESENT_MIN - 2, True), (20, False)])
async def test_a_finder_who_was_there_when_he_died_suspects_no_one(store, seen_before, there):
    """An npc's killing of an npc has no deed: Lobart, who saw Maleth alive right before the death, may be
    his killer and must not suspect the hero he saw there; 20 minutes before is an ordinary find."""
    w = World(store)
    w.saw(seen_view(AT_8))  # the hero and Maleth alive in the yard
    found_at = AT_8 + 60
    w.saw(found_view(found_at, body(age=found_at - AT_8 - seen_before)))  # died `seen_before` after it
    line = next(row for row in (w.seen.situation(LOBART) or "").splitlines() if "тело: Малет" in row)
    context = w.stimuli()[0][2]["context"]
    if there:
        assert w.suspected() == [] and "ты был здесь, когда он умер" in line
        assert context == " You were here when they died." and "герой" not in line
    else:
        assert [f["suspect"] for f in w.suspected()] == ["PLAYER"] and "подозрение" in line
        assert context == ""
    await asyncio.gather(*w.seen._background, *w.chronicle._background)


async def test_suspicion_waits_for_the_deed_seen_after_the_find(store):
    """The view with the body may come before `hero_deed`: the witness's crime_seen arrives later, and the
    trace must not make it suspect the hero a second time."""
    w = World(store)
    w.saw(seen_view(AT_8))
    view = found_view(AT_8 + 60)
    w.saw(view, judge=False)
    assert len(w.stimuli()) == 1  # the stimulus at once
    w.later(view, JUDGE_S / 2)  # too early: nothing is decided
    assert w.chronicle.facts("trace_judged") == []
    w.chronicle.record(
        {
            "fact": "crime",
            "id": "M1",
            "kind": "murder",
            "victim": MALETH,
            "waypoint": YARD,
            "day": 1,
            "hour": 8,
            "minute": 20,
        }
    )
    w.chronicle.record({"fact": "crime_seen", "npc": LOBART, "crime": "M1"})
    w.clock.t += 600  # no view for a long while: the decision waits for the next one
    w.later(view)
    assert w.suspected() == [] and len(w.chronicle.facts("trace_judged")) == 1
    w.later(view)
    assert len(w.chronicle.facts("trace_judged")) == 1  # decided once
    await asyncio.gather(*w.seen._background, *w.chronicle._background)


async def test_a_find_saved_before_its_decision_is_decided_once_after_the_load(tmp_path):
    path = str(tmp_path / "memory.sqlite3")
    store = MemoryStore(path)
    store.ensure_branch("A")
    w = World(store)
    w.saw(seen_view(AT_8))
    w.saw(found_view(AT_8 + 60), judge=False)  # saved right after the find
    await asyncio.gather(*w.seen._background, *w.chronicle._background)
    head = store.head("A")
    assert w.chronicle.facts("trace_judged") == []
    store.close()

    store = MemoryStore(path)
    store.ensure_branch("loaded", "A", head)
    again = World(store, branch="loaded")
    again.saw(found_view(AT_8 + 61), judge=False)  # the first view after the load decides it
    again.later(found_view(AT_8 + 62))
    assert again.finds == [] and again.stimuli() == []
    assert [f["suspect"] for f in again.suspected()] == ["PLAYER"]
    assert len(again.chronicle.facts("trace_judged")) == 1
    await asyncio.gather(*again.seen._background, *again.chronicle._background)
    store.close()
