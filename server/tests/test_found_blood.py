"""Blood first seen (v60): one trace_found, suspicion of an attack only by the chest's rule."""

import asyncio
import json

import pytest

from gothic_ai.brain.situation import NpcContext, SituationBuilder
from gothic_ai.brain.stimuli import Stimulus
from gothic_ai.crimes import SUSPECT_MIN
from gothic_ai.memory import MemoryStore
from gothic_ai.perception import PerceptionDesk
from gothic_ai.protocol import BLOOD_TRACE_PROTOCOL, PROTOCOL_VERSION, NpcView, parse_game_message
from tests.fakes import Clock
from tests.test_found_traces import World
from tests.test_perception import HILDA, LOBART, MALETH, Social, look, physical_trace
from tests.test_sightings import AT_8, BOOK

BLOOD_ID = "NEWWORLD.ZEN:blood:12"  # named by the oldest stain of the group
KEY = f"TRACE:{BLOOD_ID}:blood"
YARD, PATH = "NW_FARM1_OUT_01", "NW_FARM1_PATH_01"  # FARM1_OUT, FARM1_PASTURE


def blood(age: int | None = 20, waypoint: str = "NW_FARM1_OUT_02", **extra) -> dict:
    return physical_trace(BLOOD_ID, "blood", "кровь", age=age, waypoint=waypoint, **extra)


def seen_view(time: int, *, hero_at: str | None = YARD, hilda_at: str | None = None) -> NpcView:
    """Lobart sees the hero (and Hilda) at a place, before the blood is found."""
    hero = look("PLAYER", "Я", 8, seen=hero_at is not None, waypoint=hero_at or YARD)
    sees = [look(HILDA, "Хильда", 6, waypoint=hilda_at)] if hilda_at is not None else []
    return NpcView.model_validate(
        {"npc": LOBART, "name": "Лобарт", "waypoint": YARD, "time": time, "hero": hero, "sees": sees}
    )


def found_view(time: int, *traces: dict) -> NpcView:
    """Lobart alone by the blood, the hero far away and unseen."""
    return NpcView.model_validate(
        {
            "npc": LOBART,
            "name": "Лобарт",
            "waypoint": YARD,
            "time": time,
            "hero": look("PLAYER", "Я", 40, seen=False),
            "traces": list(traces) or [blood()],
        }
    )


@pytest.fixture
def store():
    s = MemoryStore(":memory:")
    s.ensure_branch("A")
    yield s
    s.close()


def blood_line(w: World) -> str:
    return next((row for row in (w.seen.situation(LOBART) or "").splitlines() if "кровь" in row), "")


def test_v60_blood_trace_parses():
    assert PROTOCOL_VERSION >= BLOOD_TRACE_PROTOCOL == 60
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
                            {"id": BLOOD_ID, "kind": "blood", "name": "лужа крови", "distance": 300, "age": 7}
                        ],
                    }
                ],
            }
        )
    )
    (trace,) = msg.views[0].traces  # type: ignore[union-attr]
    assert (trace.kind, trace.name, trace.age, trace.npc) == ("blood", "лужа крови", 7, None)


@pytest.mark.parametrize("protocol", [BLOOD_TRACE_PROTOCOL - 1, BLOOD_TRACE_PROTOCOL])
async def test_an_older_bridge_has_no_blood_traces(store, protocol):
    w = World(store, protocol=protocol)
    w.saw(seen_view(AT_8 + 30), found_view(AT_8 + 60, physical_trace(), blood()))
    newer = protocol >= BLOOD_TRACE_PROTOCOL
    assert (KEY in w.seen.mind(LOBART).traces) == newer
    assert "TRACE:vob:12:moved" in w.seen.mind(LOBART).traces  # older kinds stay
    assert bool(w.stimuli()) == newer and bool(w.suspected()) == newer
    desk = PerceptionDesk(w.link, social=Social, places=BOOK, clock=Clock())  # type: ignore[arg-type]
    desk._store(found_view(AT_8 + 60, physical_trace(), blood()))
    sight = desk.sight(LOBART)
    assert sight is not None and ("кровь (" in (sight.traces or "")) == newer
    await asyncio.gather(*w.seen._background, *w.chronicle._background)


async def test_the_first_sight_of_blood_is_one_find_and_suspicion_of_those_seen_there(store):
    w = World(store)
    w.saw(seen_view(AT_8 + 30, hilda_at="NW_FARM1_OUT_02"))  # 08:40, ten minutes before the blood
    w.saw(found_view(AT_8 + 60))  # the freshest stain is 20 minutes old: 08:50
    w.saw(found_view(AT_8 + 61), found_view(AT_8 + 90))  # shown again: no second find
    (find,) = w.finds
    assert (find.npc, find.kind, find.body, find.bled, find.place) == (
        LOBART,
        "blood",
        "",
        AT_8 + 40,
        "FARM1_OUT",
    )
    (stimulus,) = w.stimuli()
    npc, _, kw = stimulus
    assert npc == LOBART and kw["other"] == BLOOD_ID and kw["source"] == "blood"
    assert kw["detail"] == "spilled about 20 minutes ago" and kw["context"] == ""
    hero = w.chronicle.facts("crime_suspected")
    npcs = w.chronicle.facts("npc_crime_suspected")
    assert [(f["npc"], f["suspect"], f["crime"], f["trace"], f["bled"]) for f in hero] == [
        (LOBART, "PLAYER", "attack", BLOOD_ID, AT_8 + 40)
    ]
    assert [(f["npc"], f["suspect"], f["crime"], f["trace"]) for f in npcs] == [
        (LOBART, HILDA, "attack", BLOOD_ID)
    ]
    assert all(f["suspect"] != LOBART and "body" not in f for f in w.suspected())
    assert [(f["trace"], set(f["suspects"])) for f in w.chronicle.facts("trace_judged")] == [
        (BLOOD_ID, {"PLAYER", HILDA})
    ]
    assert w.crimes.known(LOBART) == []  # a suspicion without a deed id is no knowledge of a real deed
    line = blood_line(w)
    assert "пролита примерно за 20 мин до того" in line and "здесь кого-то ранили" in line
    assert "герой, Хильда" in line and "подозрение, не доказательство" in line
    await asyncio.gather(*w.seen._background, *w.chronicle._background)


@pytest.mark.parametrize(
    "case",
    ["nobody_seen", "other_place", "before_window", "after_window", "unknown_age", "no_place", "witnessed"],
)
async def test_no_suspicion_of_an_attack_without_evidence(store, case):
    w = World(store)
    found_at, age, waypoint = AT_8 + 60, 20, "NW_FARM1_OUT_02"  # the blood at 08:50
    if case == "nobody_seen":
        w.saw(seen_view(AT_8 + 30, hero_at=None))
    elif case == "other_place":
        w.saw(seen_view(AT_8 + 30, hero_at=PATH, hilda_at=PATH))
    elif case == "before_window":  # seen at 08:10, the blood 31 minutes later
        w.saw(seen_view(AT_8, hilda_at=YARD))
        age = 60 - SUSPECT_MIN - 1
    elif case == "after_window":  # seen 31 minutes after the blood, found later
        w.saw(seen_view(AT_8 + 30 + SUSPECT_MIN + 1, hilda_at=YARD))
        found_at, age = AT_8 + 30 + 2 * SUSPECT_MIN, 2 * SUSPECT_MIN
    elif case == "unknown_age":
        w.saw(seen_view(AT_8 + 30, hilda_at=YARD))
        age = None
    elif case == "no_place":
        w.saw(seen_view(AT_8 + 30, hilda_at=YARD))
        waypoint = "NW_SOMEWHERE_ELSE"
    elif case == "witnessed":  # Lobart saw the hero attack Maleth in the yard at 08:30
        w.saw(seen_view(AT_8 + 30, hilda_at=YARD))
        w.chronicle.record(
            {
                "fact": "crime",
                "id": "A1",
                "kind": "attack",
                "victim": MALETH,
                "waypoint": YARD,
                "day": 1,
                "hour": 8,
                "minute": 30,
            }
        )
        w.chronicle.record({"fact": "crime_seen", "npc": LOBART, "crime": "A1"})
    w.saw(found_view(found_at, blood(age=age, waypoint=waypoint)))
    assert w.suspected() == []
    assert len(w.finds) == 1 and len(w.stimuli()) == 1
    assert len(w.chronicle.facts("trace_judged")) == 1
    line = blood_line(w)
    context = w.stimuli()[0][2]["context"]
    if case == "witnessed":
        assert "ты видел это сам" in line and "saw the fight" in context
    else:
        assert "кто и кого — не знаешь" in line and context == ""
    if case == "unknown_age":
        assert "когда пролита — не знаешь" in line and w.finds[0].bled is None
    await asyncio.gather(*w.seen._background, *w.chronicle._background)


async def test_a_body_and_blood_in_one_place_are_two_finds_of_their_own(store):
    """The engine never reports the pool of a body still in the world; blood elsewhere (the wounded walked
    on) is a find of its own, and a body's suspicion of murder does not decide the blood."""
    w = World(store)
    w.saw(seen_view(AT_8 + 30))
    body = physical_trace(
        "NEWWORLD.ZEN:body:3", "body", "тело", npc=MALETH, age=10, waypoint="NW_FARM1_OUT_02"
    )
    w.saw(found_view(AT_8 + 60, body, blood()))
    assert sorted(f.kind for f in w.finds) == ["blood", "body"]
    crimes = sorted((f["crime"], f["trace"]) for f in w.chronicle.facts("crime_suspected"))
    assert crimes == [("attack", BLOOD_ID), ("murder", "NEWWORLD.ZEN:body:3")]
    await asyncio.gather(*w.seen._background, *w.chronicle._background)


def test_trace_found_of_blood_has_its_own_words():
    stimulus = Stimulus(
        "trace_found",
        0.0,
        BLOOD_ID,
        300,
        "spilled about 20 minutes ago",
        "a stranger",
        "",
        place=YARD,
        source="blood",
    )
    assert stimulus.policy.salient and not stimulus.policy.urgent and not stimulus.policy.scripted
    sit = SituationBuilder(places=BOOK).react(NpcContext("BAU_950_LOBART", "Лобарт"), [stimulus], 0.0, None)
    assert (
        "You found blood on the ground in Lobart's farmyard, spilled about 20 minutes ago. "
        "Someone was hurt here." in sit.state
    )
    assert "body" not in sit.state and "call_guards" in sit.questions["react"]["criteria"]
    body = Stimulus("trace_found", 0.0, MALETH, 300, "dead for about 50 minutes", "Малет", "", place=YARD)
    assert body.policy.text.startswith("You found the body of")  # the body's words stay


async def test_no_found_traces_kind_blood_means_only_a_trace(store):
    """A FoundTraces without blood (an older server's kinds) keeps blood an ordinary trace."""
    w = World(store)
    w.found.kinds = frozenset({"body"})
    w.saw(seen_view(AT_8 + 30), found_view(AT_8 + 60))
    assert KEY in w.seen.mind(LOBART).traces and w.finds == [] and w.suspected() == []
    await asyncio.gather(*w.seen._background, *w.chronicle._background)
