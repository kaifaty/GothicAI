import copy
import json
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from types import SimpleNamespace
from typing import Any

import pytest

from gothic_ai import social
from gothic_ai.chronicle import Chronicle
from gothic_ai.crime_react import CrimeReact
from gothic_ai.crimes import Crimes
from gothic_ai.deals import Deals
from gothic_ai.dialogue import DialogueManager
from gothic_ai.game import GameError
from gothic_ai.gamedata.storygraph import StoryGraph, load
from gothic_ai.llm.fake import FakeBackend
from gothic_ai.memory import MemoryStore
from gothic_ai.perception import PerceptionDesk
from gothic_ai.persona import PersonaProvider
from gothic_ai.protocol import Look
from gothic_ai.social import DAY, Rumor, Rumors, Social, Spot
from gothic_ai.trust import Trust
from tests.fake_game import FakeGame, connected
from tests.test_dialogue import _until
from tests.test_director import GRAPH, HILDA, LOBART, QUEST, VINO

ANDRE, VATRAS, BANDIT = "MIL_311_ANDRE", "VLK_439_VATRAS", "BDT_1013_BANDIT_L"
FARM, FIELD, CITY = [13608, 1753, -15600], [14678, 1642, -10000], [10480, 320, 4920]  # FIELD: 57 m off


def _person(npc: str, name: str, *routine: tuple[int, int, list[int]], guild: int = 8) -> dict:
    return {
        "npc": npc,
        "name": name,
        "guild": guild,
        "routine": [
            {"start": s, "end": e, "state": "ZS_STAND", "waypoint": "WP", "pos": pos} for s, e, pos in routine
        ],
    }


def _h(hours: float) -> int:
    return int(hours * 60)


# Lobart and Hilda on the farm by day, by the field at night; Vino by the field in the evening (Hilda comes at
# 22, Lobart only at 23); Vatras and Andre in the city
PEOPLE = [
    _person(LOBART, "Лобарт", (_h(7.5), _h(23), FARM), (_h(23), _h(7.5), FIELD)),
    _person(HILDA, "Хильда", (_h(8), _h(22), FARM), (_h(22), _h(8), FIELD)),
    _person(VINO, "Вино", (_h(8), _h(20), [9703, 1343, -9000]), (_h(20), _h(23), FIELD)),
    _person(VATRAS, "Ватрас", (_h(6), _h(22), CITY), guild=14),
    _person(ANDRE, "Андре", (_h(6), _h(22), [10800, 320, 5100]), guild=2),
    _person(BANDIT, "Бандит", (_h(0), _h(24), FARM), guild=9),
]


@pytest.fixture
def graph(tmp_path: Path) -> StoryGraph:
    """The turnips (Lobart, Hilda), and a quest of Andre's that ends at Lobart's (as the field raiders do)."""
    g = copy.deepcopy(GRAPH)
    bugs = "MIS_ANDREHELPLOBART"
    g["nodes"] += [
        {
            **g["nodes"][0],
            "id": "DIA_ANDRE_HELPLOBART",
            "npc": ANDRE,
            "effects": [{"do": "set", "var": bugs, "value": ["int", 1]}],
            "kind": "dialog",
            "parents": [],
        },
        {
            **g["nodes"][0],
            "id": "DIA_LOBART_BUGDEAD",
            "npc": LOBART,
            "effects": [],
            "kind": "dialog",
            "parents": [],
            "condition": ["==", ["var", bugs], ["int", 1]],
        },
        {
            **g["nodes"][0],
            "id": "DIA_BANDIT_HI",
            "npc": BANDIT,
            "effects": [],
            "kind": "dialog",
            "parents": [],
            "condition": ["==", ["var", QUEST], ["int", 1]],
        },
    ]
    g["quests"].append(
        {
            "var": bugs,
            "topics": [],
            "running": ["DIA_ANDRE_HELPLOBART"],
            "success": [],
            "failed": [],
            "obsolete": [],
            "other": [],
        }
    )
    path = tmp_path / "story.json"
    path.write_text(json.dumps(g, ensure_ascii=False), encoding="utf-8")
    return load(path)


def test_who_meets_whom_and_who_has_dealings(graph):
    s = Social(PEOPLE, graph)
    # the farm's day and its night (the beds by the field) are one span each
    assert s.ties[LOBART][HILDA].meets == [(0, _h(7.5)), (_h(8), _h(22)), (_h(23), DAY)]
    assert s.ties[VINO][HILDA].meets == [(_h(22), _h(23))]
    assert LOBART not in s.ties[VINO] or not s.ties[VINO][LOBART].meets
    assert s.ties[VATRAS][ANDRE].meets == [(_h(6), _h(22))]  # 3.6 m apart in the city
    assert LOBART not in s.ties[VATRAS]
    # dealings: the turnips (Lobart, Hilda), Andre's quest that ends at Lobart's
    assert s.ties[LOBART][HILDA].deals == [QUEST]
    assert s.ties[LOBART][ANDRE].deals == ["MIS_ANDREHELPLOBART"] and not s.ties[LOBART][ANDRE].meets
    # an outlaw at the farm all day reads the turnips quest: no acquaintance of theirs
    assert BANDIT not in s.ties[LOBART] and BANDIT not in s.ties[HILDA]
    assert "Андре (MIL_311_ANDRE): дела: MIS_ANDREHELPLOBART" in s.describe(LOBART)


def _rumors(s: Social) -> tuple[Rumors, Rumor, list[dict]]:
    """Rumors without a game: what it records goes into a list."""
    r = Rumors.__new__(Rumors)
    facts: list[dict] = []
    r.social, r.dead, r.rumors, r.tells = s, set(), {}, defaultdict(int)
    r.chronicle = SimpleNamespace(
        record=facts.append,
        facts=lambda kind=None: [f for f in facts if kind is None or f.get("fact") == kind],
    )
    r.dialogue = type("D", (), {"remember": lambda self, *a: None})()
    rumor = Rumor(QUEST, "caught", LOBART, "Лобарт", "Убрать поле репы")
    rumor.known[LOBART] = 0
    r.rumors[rumor.key] = rumor
    return r, rumor, facts


def test_a_rumor_goes_round_the_meetings_in_order(graph, monkeypatch):
    monkeypatch.setattr(social, "P_MEET", 1.0)
    monkeypatch.setattr(social, "P_DEAL", 0.0)
    s = Social(PEOPLE, graph)
    r, rumor, facts = _rumors(s)
    # caught at 9:00; by 23:00 Hilda (on the farm) and, at 22:00 by the field, Vino from her
    assert r.spread(DAY + _h(9), DAY + _h(23)) == 2
    assert rumor.known == {LOBART: 0, HILDA: 1, VINO: 2}
    first, second = facts
    assert (first["from"], first["to"], first["hops"], first["how"]) == (LOBART, HILDA, 1, "met")
    assert (second["from_name"], second["to"], second["hops"]) == ("Хильда", VINO, 2)
    assert second["about_name"] == "Лобарт" and second["day"] == 1
    # the city has no meeting with the farm
    assert VATRAS not in rumor.known and ANDRE not in rumor.known


def test_dealings_bring_it_to_the_city_once_a_day(graph, monkeypatch):
    monkeypatch.setattr(social, "P_MEET", 1.0)
    monkeypatch.setattr(social, "P_DEAL", 1.0)
    s = Social(PEOPLE, graph)
    r, rumor, facts = _rumors(s)
    # a jump of the clock over a night: Andre hears through his dealings with Lobart, Vatras meets Andre
    r.spread(DAY + _h(9), 2 * DAY + _h(23))
    assert rumor.known[ANDRE] == 1 and rumor.known[VATRAS] == 2
    assert next(f for f in facts if f["to"] == ANDRE)["how"] == "dealings"


def test_retellings_stop_and_the_dead_tell_no_one(graph, monkeypatch):
    monkeypatch.setattr(social, "P_MEET", 1.0)
    monkeypatch.setattr(social, "P_DEAL", 1.0)
    monkeypatch.setitem(social.HOPS, "caught", 1)
    s = Social(PEOPLE, graph)
    r, rumor, _ = _rumors(s)
    r.spread(DAY + _h(9), 3 * DAY)
    assert set(rumor.known) == {LOBART, HILDA, ANDRE}  # the first retelling only
    monkeypatch.setitem(social.HOPS, "caught", 3)
    r2, rumor2, _ = _rumors(s)
    r2.dead.add(HILDA)
    r2.spread(DAY + _h(9), DAY + _h(23))
    assert HILDA not in rumor2.known and VINO not in rumor2.known


def test_the_chances_repeat():
    assert social._chance("a", "b", "1") == social._chance("a", "b", "1") != social._chance("a", "b", "2")


async def test_rumors_on_the_game_clock(graph, monkeypatch):
    monkeypatch.setattr(social, "P_MEET", 1.0)
    monkeypatch.setattr(social, "P_DEAL", 0.0)
    fake = FakeGame(branch="A")
    fake.people = PEOPLE
    fake.hour, fake.minute = 9, 0
    store = MemoryStore(":memory:")
    link_cm = connected(fake)
    link = await link_cm.__aenter__()
    try:
        dialogue = DialogueManager(link, FakeBackend("Ну?", delay=0), PersonaProvider(None), memory=store)
        chronicle = Chronicle(link, store)
        await _until(lambda: chronicle.branch is not None)
        rumors = Rumors(link, dialogue, chronicle, graph)
        chronicle.record(
            {
                "fact": "caught",
                "quest": QUEST,
                "npc": LOBART,
                "name": "Лобарт",
                "title": "Убрать поле репы",
                "how": "видел сам",
            }
        )
        await rumors.tick()  # the people, and the clock
        assert rumors.social is not None and HILDA in rumors.social.ties[LOBART]
        fake.hour = 23
        await rumors.tick()
        heard = {f["to"]: f for f in chronicle.facts("rumor")}
        assert set(heard) == {HILDA, VINO}
        trust = Trust(chronicle)
        assert "ты слышал, что он обманул других: Лобарт («Убрать поле репы», со слов: Хильда)" in (
            trust.situation(VINO) or ""
        )
        assert "Слух (со слов: Хильда): герой обманул человека по имени Лобарт — уверял" in str(
            dialogue.conversations[VINO].history
        )
        # after a load of an earlier save the branch's facts are all there is
        rumors._load()
        assert set(rumors.rumors[f"caught:{LOBART}:{QUEST}"].known) == {LOBART, HILDA, VINO}
    finally:
        await link_cm.__aexit__(None, None, None)


def test_one_tells_a_rumor_to_a_few_a_day(graph, monkeypatch):
    monkeypatch.setattr(social, "P_MEET", 1.0)
    monkeypatch.setattr(social, "P_DEAL", 1.0)
    monkeypatch.setattr(social, "TELLS_PER_DAY", 1)
    s = Social(PEOPLE, graph)
    r, rumor, facts = _rumors(s)
    r.spread(
        DAY + _h(9),
        DAY
        + _h(
            23,
        ),
    )
    assert [f["from"] for f in facts].count(LOBART) == 1  # Hilda by day; Andre waits
    r.spread(DAY + _h(23), 2 * DAY + _h(23))
    assert [f["from"] for f in facts].count(LOBART) == 2 and ANDRE in rumor.known


def test_old_news_is_not_retold(graph, monkeypatch):
    monkeypatch.setattr(social, "P_MEET", 1.0)
    monkeypatch.setattr(social, "P_DEAL", 1.0)
    monkeypatch.setattr(social, "TELLS_PER_DAY", 1)
    monkeypatch.setattr(social, "FRESH_DAYS", 1)
    s = Social(PEOPLE, graph)
    r, rumor, facts = _rumors(s)
    rumor.since[LOBART] = 1  # he caught the hero on day 1
    r.spread(DAY + _h(9), 4 * DAY)
    assert [f["from"] for f in facts].count(LOBART) == 1  # one of Hilda and Andre, that day only


def test_a_lie_he_took_a_reward_for_goes_further(graph, monkeypatch):
    monkeypatch.setattr(social, "P_MEET", 1.0)
    monkeypatch.setattr(social, "P_DEAL", 1.0)
    citizen = "VLK_4000_CITIZEN"  # in the city all day, by Vatras (24 m) and not by Andre (26 m)
    s = Social([*PEOPLE, _person(citizen, "Горожанин", (0, DAY, [10480, 320, 2520]), guild=3)], graph)
    assert set(s.ties[citizen]) == {VATRAS}
    reach = {}
    for kind in ("caught", "found_out"):
        r, rumor, _ = _rumors(s)
        rumor.kind = kind
        r.rumors = {rumor.key: rumor}
        r.spread(DAY + _h(9), 3 * DAY)
        reach[kind] = rumor.known.get(citizen)
    # Lobart -> Andre (dealings) -> Vatras (the city) -> the citizen: the third retelling is only for a lie
    # he took a reward for
    assert reach == {"caught": None, "found_out": 3}


def test_one_who_promised_silence_tells_no_one(graph, monkeypatch):
    monkeypatch.setattr(social, "P_MEET", 1.0)
    monkeypatch.setattr(social, "P_DEAL", 1.0)
    s = Social(PEOPLE, graph)
    r, rumor, facts = _rumors(s)
    # Lobart took the hero's gold for keeping quiet (deals.py): the rumor does not leave him
    r._on_fact({"fact": "deal", "npc": LOBART, "silence": [rumor.key]})
    assert r.spread(DAY + _h(9), 3 * DAY) == 0 and set(rumor.known) == {LOBART}
    # whom he told before still pass it on
    rumor.known[HILDA] = 1
    r.spread(3 * DAY, 3 * DAY + _h(23))
    assert VINO in rumor.known and all(f["from"] != LOBART for f in facts)


@pytest.mark.parametrize("promise", ["lie", "crime", "cover"])
@pytest.mark.parametrize("keeps", [False, True])
def test_npc_intent_controls_silence_and_first_real_retelling_records_betrayal(graph, promise, keeps):
    if promise == "lie":
        r, rumor, facts = _rumors(Social(PEOPLE, graph))
        teller, listeners = LOBART, [HILDA, VINO]
        terms = {"silence": [rumor.key]}
    else:
        r, rumor, facts = _crime_rumors(Social(PEOPLE, graph))
        teller, listeners = HILDA, [LOBART, VINO]
        terms = {"cover": {"crime": "c1"}} if promise == "cover" else {"silence": ["crime:c1"]}
    how = "cover" if promise == "cover" else "silence"
    facts.append({"fact": "npc_intent", "npc": teller, "deal": "D1", "keeps": keeps, "how": [how]})
    deal = {"fact": "deal", "npc": teller, "deal": "D1", **terms}
    facts.append(deal)
    r._on_fact(deal)
    r.clock = DAY + _h(9)

    assert (teller in rumor.silent) is keeps
    assert r.tell(rumor, teller, listeners[0]) == ("no" if keeps else "told")
    assert r.tell(rumor, teller, listeners[1]) == ("no" if keeps else "told")
    betrayals = [f for f in facts if f.get("fact") == "betrayed"]
    assert betrayals == ([] if keeps else [{
        "fact": "betrayed", "npc": teller, "deal": "D1", "how": "told", "to": listeners[0],
    }])


@pytest.mark.parametrize("promise", ["silence", "cover"])
def test_disabling_cheating_restores_raw_crime_promises_on_load(graph, promise):
    r, _, facts = _crime_rumors(Social(PEOPLE, graph))
    facts.extend([
        {"fact": "npc_intent", "npc": HILDA, "deal": "D1", "keeps": False, "how": [promise]},
        {"fact": "deal", "npc": HILDA, "deal": "D1", **(
            {"cover": {"crime": "c1"}} if promise == "cover" else {"silence": ["crime:c1"]}
        )},
    ])
    r.chronicle.npc_cheat_enabled = False
    r._load()
    [rumor] = r.rumors.values()
    r.clock = DAY + _h(9)
    assert HILDA in rumor.silent
    assert r.tell(rumor, HILDA, LOBART) == "no"
    assert not any(f.get("fact") == "betrayed" for f in facts)


def test_a_debt_not_paid_back_goes_round(graph, monkeypatch):
    monkeypatch.setattr(social, "P_MEET", 1.0)
    monkeypatch.setattr(social, "P_DEAL", 0.0)
    s = Social(PEOPLE, graph)
    r, rumor, facts = _rumors(s)
    del r.rumors[rumor.key]
    r._on_fact(
        {
            "fact": "broken",
            "debt": "d1",
            "npc": LOBART,
            "name": "Лобарт",
            "what": "25 золотых до конца дня 2",
            "day": 2,
        }
    )
    broken = r.rumors["broken:BAU_950_LOBART:d1"]
    assert broken.hops == 2
    r.spread(2 * DAY + _h(9), 2 * DAY + _h(23))
    told = next(f for f in facts if f["to"] == HILDA)
    assert (told["kind"], told["title"], told["quest"]) == ("broken", "25 золотых до конца дня 2", "d1")
    assert "задолжал человеку по имени Лобарт и не вернул в срок" in broken.told("Лобарт")


def _crime_rumors(s: Social, kind: str = "theft", witness: str = HILDA) -> tuple[Rumors, Rumor, list[dict]]:
    r, _, facts = _rumors(s)
    r.rumors.clear()
    crime = {"fact": "crime", "id": "c1", "kind": kind, "victim": LOBART, "day": 1}
    facts.extend([crime, {"fact": "crime_seen", "npc": witness, "crime": "c1"}])
    r.chronicle.facts = lambda kind=None: [f for f in facts if kind is None or f.get("fact") == kind]
    r.crimes = SimpleNamespace(
        facts=lambda: r.chronicle.facts("crime"),
        crime=lambda crime_id: next((f for f in r.chronicle.facts("crime") if f["id"] == crime_id), None),
        describe=lambda f: f"герой совершил {f['kind']} (день {f['day']})",
        learn_side=lambda npc, crime: None,
        excuse=lambda npc, crime: {},
        react=None,
        cover=True,
        testimony_crimes=lambda f: f.get("crime_told", []),
    )
    for f in list(facts):
        r._on_fact(f)
    return r, next(iter(r.rumors.values())), facts


@pytest.mark.parametrize("cover", [True, False])
def test_cover_silence_respects_switch_when_loading_rumors(graph, cover):
    r, rumor, facts = _crime_rumors(Social(PEOPLE, graph))
    r.crimes.cover = cover
    silence = {"fact": "silenced", "npc": HILDA, "crime": "c1", "cover": "D1"}
    facts.append(silence)
    r._on_fact(silence)
    assert (HILDA in rumor.silent) is cover
    r._load()
    [rumor] = r.rumors.values()
    assert (HILDA in rumor.silent) is cover
    facts.append({"fact": "silenced", "npc": HILDA, "crime": "c1"})
    r._load()
    assert HILDA in next(iter(r.rumors.values())).silent


@pytest.mark.parametrize("cover", ["D1", ""])
def test_no_crimes_ignores_cover_silence_for_old_caught_rumors(graph, cover):
    r, _, facts = _crime_rumors(Social(PEOPLE, graph))
    r.crimes = None
    facts[:] = [
        {"fact": "caught", "quest": "old-lie", "npc": HILDA, "title": "не брал", "crime": "c1"},
        {"fact": "silenced", "npc": HILDA, "crime": "c1", **({"cover": cover} if cover else {})},
    ]
    r._load()
    [rumor] = r.rumors.values()
    assert (HILDA in rumor.silent) is (not cover)


def test_crime_witnesses_share_one_rumor_and_keep_the_speaker(graph, monkeypatch):
    monkeypatch.setattr(social, "P_MEET", 1.0)
    monkeypatch.setattr(social, "P_DEAL", 0.0)
    r, rumor, facts = _crime_rumors(Social(PEOPLE, graph))
    assert rumor.known == {HILDA: 0} and rumor.quest == "crime:c1"
    assert LOBART not in rumor.known  # a victim who did not see it does not magically know
    r._on_fact({"fact": "crime_seen", "npc": VINO, "crime": "c1"})
    assert len(r.rumors) == 1 and rumor.known[VINO] == 0
    r.spread(DAY + _h(9), DAY + _h(10))
    [told] = [f for f in facts if f.get("fact") == "rumor"]
    assert (told["crime"], told["from"], told["from_name"], told["to"], told["hops"]) == (
        "c1",
        HILDA,
        "Хильда",
        LOBART,
        1,
    )
    assert rumor.told("Хильда") == "Слух (со слов: Хильда): герой совершил theft (день 1)"


def test_justified_murder_upgrades_known_news_and_keeps_its_source_on_reload(graph):
    r, rumor, facts = _crime_rumors(Social(PEOPLE, graph), "murder")
    facts[0]["victim"] = BANDIT
    facts.extend([
        {"fact": "crime_seen", "npc": VINO, "crime": "c1"},
        {"fact": "crime_side", "npc": HILDA, "crime": "c1", "why": "fought"},
    ])
    r.chronicle.on_fact = []
    r.crimes = Crimes(SimpleNamespace(listeners=[]), r.chronicle)
    r.crimes.people = {p["npc"]: {"guild": p["guild"], "npctype": 0} for p in PEOPLE}
    CrimeReact(r.crimes.link, r.crimes)
    r._load()
    [rumor] = r.rumors.values()
    notes = []
    r.dialogue = SimpleNamespace(remember=lambda *args: notes.append(args[-1]))
    r.clock = DAY + _h(10)
    assert r.tell(rumor, VINO, VATRAS) == "told"
    assert r.tell(rumor, VATRAS, LOBART) == "told"
    assert not [f for f in facts if f.get("fact") == "rumor"][-1].get("excuse")
    assert rumor.known[LOBART] == 2
    r.gate = lambda *args: True
    assert r.offer_status(rumor, HILDA, LOBART) == "told"
    upgraded = [f for f in facts if f.get("fact") == "rumor"][-1]
    assert (upgraded["excuse"], upgraded["excuse_by"], upgraded["excuse_by_name"]) == (
        "fought", HILDA, "Хильда",
    )
    assert "Хильда говорит:" in notes[-1] and "дрался с ним" in notes[-1]
    assert rumor.known[LOBART] == 1
    assert r.tell(rumor, HILDA, LOBART) == "no"
    assert r.tell(rumor, LOBART, ANDRE) == "told"
    retold = [f for f in facts if f.get("fact") == "rumor"][-1]
    assert (retold["excuse"], retold["excuse_by"], retold["hops"]) == ("fought", HILDA, 2)
    tells, known = dict(r.tells), dict(rumor.known)
    r._load()
    [restored] = r.rumors.values()
    assert restored.known == known and dict(r.tells) == tells
    assert r.crimes.excuse(ANDRE, "c1")["excuse_by"] == HILDA


@pytest.mark.parametrize("error", [None, GameError("guilds unavailable"), TimeoutError("guilds timeout")])
async def test_rumor_tick_prepares_guilds_and_still_spreads_on_read_failure(
    graph, monkeypatch, caplog, error,
):
    monkeypatch.setattr(social, "P_MEET", 1.0)
    monkeypatch.setattr(social, "P_DEAL", 0.0)
    r, rumor, facts = _crime_rumors(Social(PEOPLE, graph), "murder")
    calls = []

    async def prepare():
        calls.append("prepare")
        if error is not None:
            raise error

    async def state():
        calls.append("state")
        return {"day": 1, "hour": 10, "minute": 0}

    r.crimes.sides = SimpleNamespace(prepare=prepare)
    r.link = SimpleNamespace(story_state=state)
    r.clock = DAY + _h(9)
    with caplog.at_level("INFO", logger="gothic_ai.social"):
        await r.tick()
    assert calls == ["prepare", "state"]
    assert LOBART in rumor.known and len([f for f in facts if f.get("fact") == "rumor"]) == 1
    if error is not None:
        assert str(error) in caplog.text


@pytest.mark.parametrize("kind,hops", [("theft", 2), ("sheep", 2), ("attack", 3), ("murder", 4)])
def test_crime_severity_limits_its_retellings(kind, hops, monkeypatch):
    monkeypatch.setattr(social, "P_MEET", 1.0)
    people = [_person(f"N{i}", f"NPC {i}", (_h(8), _h(22), [2000 * i, 0, 0])) for i in range(6)]
    r, rumor, facts = _crime_rumors(Social(people), kind, witness="N0")
    r.spread(DAY + _h(9), DAY + _h(10))
    assert rumor.known == {f"N{i}": i for i in range(hops + 1)}
    assert len([f for f in facts if f.get("fact") == "rumor"]) == hops


def test_murder_remains_news_longer_than_theft():
    for kind, last_day in (("theft", 2), ("attack", 2), ("sheep", 2), ("murder", 4)):
        rumor = Rumor("crime:c1", kind, HILDA, "Хильда", "герой совершил дело", crime="c1")
        rumor.known[HILDA], rumor.since[HILDA] = 0, 1
        assert rumor.tells(HILDA, last_day)
        assert not rumor.tells(HILDA, last_day + 1)


@pytest.mark.parametrize("settlement", ["silenced", "forgiven"])
def test_crime_settlement_stops_the_settled_npc_but_not_other_knowers(graph, monkeypatch, settlement):
    monkeypatch.setattr(social, "P_MEET", 1.0)
    monkeypatch.setattr(social, "P_DEAL", 1.0)
    r, rumor, facts = _crime_rumors(Social(PEOPLE, graph))
    settled = {"fact": settlement, "npc": HILDA, "crime": "c1", "day": 1}
    facts.append(settled)
    r._on_fact(settled)
    assert r.spread(DAY + _h(9), DAY + _h(23)) == 0
    assert LOBART not in rumor.known and HILDA in rumor.silent
    # A different actual witness remains free to tell, including after branch reconstruction.
    facts.append({"fact": "crime_seen", "npc": LOBART, "crime": "c1"})
    r._load()
    [rumor] = r.rumors.values()
    assert HILDA in rumor.silent and rumor.known == {HILDA: 0, LOBART: 0}
    r.spread(DAY + _h(9), DAY + _h(23))
    assert ANDRE in rumor.known  # Lobart can still tell his acquaintance
    assert VINO not in rumor.known  # only Hilda meets Vino, and she promised not to tell
    assert not any(f.get("from") == HILDA for f in facts if f.get("fact") == "rumor")


def test_crime_rumors_reload_only_the_current_branch(graph, monkeypatch):
    monkeypatch.setattr(social, "P_MEET", 1.0)
    monkeypatch.setattr(social, "P_DEAL", 0.0)
    r, _, facts = _crime_rumors(Social(PEOPLE, graph))
    r.spread(DAY + _h(9), DAY + _h(10))
    r._load()
    [rumor] = r.rumors.values()
    assert rumor.known == {HILDA: 0, LOBART: 1}
    facts[:] = [{"fact": "crime", "id": "other", "kind": "murder", "day": 2}]
    r._load()
    assert not r.rumors  # unwitnessed truth in a different branch is nobody's news


def test_crime_rumors_are_disabled_without_crimes(graph):
    r, _, facts = _crime_rumors(Social(PEOPLE, graph))
    r.crimes = None
    r._load()
    assert not r.rumors and not any(f.get("fact") == "rumor" for f in facts)


def test_an_outlaw_crime_witness_tells_only_outlaws(graph, monkeypatch):
    monkeypatch.setattr(social, "P_MEET", 1.0)
    monkeypatch.setattr(social, "P_DEAL", 1.0)
    other = "BDT_1014_BANDIT"
    s = Social([*PEOPLE, _person(other, "Другой разбойник", (0, DAY, FARM), guild=9)], graph)
    r, rumor, _ = _crime_rumors(s, witness=BANDIT)
    r.spread(DAY + _h(9), DAY + _h(23))
    assert rumor.known == {BANDIT: 0, other: 1}


@pytest.mark.parametrize(
    "kind,hops,day,expected",
    [
        ("theft", 0, 1, "told"),
        ("theft", 2, 1, "no"),
        ("theft", 0, 3, "no"),
        ("murder", 0, 4, "told"),
        ("murder", 0, 5, "no"),
    ],
)
def test_explicit_crime_telling_keeps_the_severity_and_freshness_limits(graph, kind, hops, day, expected):
    r, rumor, facts = _crime_rumors(Social(PEOPLE, graph), kind)
    rumor.known[HILDA] = hops
    r.clock = day * DAY + _h(9)
    assert r.tell(rumor, HILDA, LOBART) == expected
    assert len([f for f in facts if f.get("fact") == "rumor"]) == (expected == "told")


def test_explicit_crime_telling_keeps_the_outlaw_boundary(graph):
    other = "BDT_1014_BANDIT"
    s = Social([*PEOPLE, _person(other, "Другой разбойник", (0, DAY, FARM), guild=9)], graph)
    r, rumor, facts = _crime_rumors(s, witness=BANDIT)
    r.clock = DAY + _h(9)
    assert r.tell(rumor, BANDIT, HILDA) == "no"
    assert r.tell(rumor, BANDIT, other) == "told"
    [told] = [f for f in facts if f.get("fact") == "rumor"]
    assert told["to"] == other


def test_crime_testimony_recipient_can_retell_the_same_story(graph):
    r, rumor, facts = _crime_rumors(Social(PEOPLE, graph))
    testimony = {
        "fact": "testimony",
        "npc": LOBART,
        "witness": HILDA,
        "crime_told": ["c1"],
        "minute": 2 * DAY + _h(9),
    }
    facts.append(testimony)
    r._on_fact(testimony)
    assert len(r.rumors) == 1 and rumor.known == {HILDA: 0, LOBART: 1}
    assert rumor.since[LOBART] == 2 and rumor.since[HILDA] == 1
    r.clock = 2 * DAY + _h(10)
    assert r.tell(rumor, LOBART, ANDRE) == "told"
    [told] = [f for f in facts if f.get("fact") == "rumor"]
    assert (told["crime"], told["from"], told["to"], told["hops"]) == ("c1", LOBART, ANDRE, 2)
    r._load()
    [restored] = r.rumors.values()
    assert restored.known == {HILDA: 0, LOBART: 1, ANDRE: 2}


# --- staging (gossip.py): how a live pass looks ---------------------------------------------------


@dataclass(frozen=True)
class _Staging:
    """What gossip.Staging gives Rumors: a tier and the fact's keys."""

    tier: str
    where: str | None = None
    seen_by: tuple[str, ...] = ()

    def keys(self) -> dict[str, Any]:
        return {"where": self.where, "seen_by": list(self.seen_by)}


def _staged(r: Rumors, *tiers: str) -> tuple[list[tuple[str, str]], list[tuple]]:
    """Stage every pass with the next of `tiers` (the last one repeats); what was asked, what was staged."""
    asked: list[tuple[str, str]] = []
    staged: list[tuple] = []

    def stage(rumor: Rumor, a: str, b: str) -> _Staging:
        asked.append((a, b))
        return _Staging(tiers[min(len(asked), len(tiers)) - 1], "NW_FARM1", ("PLAYER", a))

    r.stage = stage
    r.on_staged = lambda rumor, a, b, staging, fact: staged.append((a, b, staging.tier, fact))
    return asked, staged


def _smalltalker(npc: str, *routine: tuple[int, int, str, str, list[int]]) -> dict:
    return {
        "npc": npc,
        "name": npc,
        "guild": 3,
        "routine": [
            {"start": s, "end": e, "state": st, "waypoint": wp, "pos": pos} for s, e, st, wp, pos in routine
        ],
    }


def test_routine_state_and_smalltalk_spans():
    a, b, c = "VLK_451_BUERGER", "VLK_452_BUERGER", "VLK_453_BUERGER"
    front, bed = [10000, 300, 5000], [40000, 300, 5000]
    s = Social(
        [
            _smalltalker(
                a,
                (_h(6), _h(13), "ZS_SMALLTALK", "FRONT_03", front),
                (_h(6), _h(13), "ZS_SMALLTALK", "FRONT_03", front),  # the same entry twice
                (_h(13), _h(15), "ZS_SMALLTALK", "FRONT_04", [10300, 300, 5000]),
                (_h(15), _h(6), "ZS_SLEEP", "BED", bed),
            ),
            _smalltalker(
                b,
                (_h(6), _h(20), "ZS_SMALLTALK", "FRONT_03_C", [10420, 300, 5000]),
                (_h(20), _h(6), "ZS_SLEEP", "BED_2", [40000, 300, 9000]),
            ),
            _smalltalker(c, (_h(6), _h(20), "ZS_STAND", "FRONT_05", [10000, 300, 5600])),
        ]
    )
    # deduped, sorted, not merged across the first one's two waypoints; the meetings are one span
    assert s.ties[a][b].talks == [(_h(6), _h(13), "FRONT_03"), (_h(13), _h(15), "FRONT_04")]
    assert s.ties[a][b].meets == [(_h(6), _h(15))]
    assert s.ties[a][c].meets and not s.ties[a][c].talks  # one only stands there
    assert "болтают 06:00–13:00 у FRONT_03" in s.describe(a)[0]
    assert s.smalltalk(a, b, _h(7)) == "FRONT_03"
    assert s.smalltalk(b, a, 3 * DAY + _h(14)) == "FRONT_04"  # any day, either way round
    assert s.smalltalk(a, b, _h(16)) is None and s.smalltalk(a, c, _h(7)) is None
    assert s.smalltalk(a, "VLK_999_NOBODY", _h(7)) is None
    assert not Social(PEOPLE).ties[LOBART][HILDA].talks  # ZS_STAND


def test_each_ones_day_and_how_many_a_place_holds():
    a, b, c, priest, late = "NOV_601_NOVIZE", "NOV_602_NOVIZE", "NOV_603_NOVIZE", "KDF_500_PYROKAR", "VLK_404"
    church, yard, hut, far = [1000, 0, 1000], [30000, 0, 1000], [60000, 0, 1000], [90000, 0, 1000]
    s = Social(
        [
            _smalltalker(
                a,
                (_h(8), _h(9), "zs_pray_innos", "CHURCH", church),
                (_h(9), _h(22), "ZS_STAND", "YARD", yard),
                (_h(22), _h(8), "ZS_SLEEP", "BED", []),  # no such point in the world
            ),
            _smalltalker(
                b, (_h(8), _h(9), "ZS_PRAY_INNOS", "CHURCH", church), (_h(9), _h(8), "ZS_STAND", "HUT", hut)
            ),
            _smalltalker(
                c,
                (_h(7.5), _h(9), "ZS_PRAY_INNOS", "CHURCH", church),
                (_h(9), _h(7.5), "ZS_STAND", "HUT", hut),
            ),
            _smalltalker(
                priest,
                (_h(21), _h(5), "ZS_PRAY_INNOS", "CHURCH", church),
                (_h(5), _h(21), "ZS_STAND", "FAR", far),
            ),
            # its first entry holds 8:00-12:00, so the overlapping church hour is never its (the engine's way)
            _smalltalker(
                late,
                (_h(8), _h(12), "ZS_STAND", "FAR", far),
                (_h(8), _h(9), "ZS_PRAY_INNOS", "CHURCH", church),
            ),
        ]
    )
    # each one's day, its entries as the routine has them; one without a point is kept, without a position
    bed = s.days[a][2]
    assert (bed.start, bed.end, bed.state, bed.waypoint, bed.pos) == (_h(22), _h(8), "ZS_SLEEP", "BED", None)
    assert s.days[a][0] == Spot(_h(8), _h(9), "ZS_PRAY_INNOS", "CHURCH", (1000, 0, 1000))
    assert [npc for npc, _ in s.spots()] == [a] * 3 + [b] * 2 + [c] * 2 + [priest] * 2 + [late] * 2
    assert s.ties[a][b].meets == [(_h(8), _h(9))]  # the bed has no position: no meeting there
    # where one is at a minute: any day, past midnight, by any case of its name
    assert s.spot(a, _h(8.5)).waypoint == "CHURCH"
    assert s.spot(a, _h(23)) is bed and s.spot(a, 3 * DAY + _h(7)) is bed and s.spot(a.lower(), _h(3)) is bed
    # an entry ends where the next begins
    assert s.spot(a, _h(22)) is bed and s.spot(a, _h(8)) is s.days[a][0]
    assert s.spot(priest, _h(4) + 59).waypoint == "CHURCH" and s.spot(priest, _h(5)).waypoint == "FAR"
    assert s.spot(late, _h(8.5)).waypoint == "FAR" and s.spot("VLK_999_NOBODY", 0) is None
    # who is at a place then, and how many it holds at once over the day
    assert s.users("CHURCH", DAY + _h(8.5)) == {a, b, c}
    assert s.users("CHURCH", _h(7.75)) == {c} and s.users("CHURCH", _h(23)) == {priest}
    assert s.users("CHURCH", _h(6)) == set() and s.users("NOWHERE", _h(8.5)) == set()
    assert s.capacity("CHURCH") == 3 and s.capacity("church") == 3
    assert s.capacity("FAR") == 2 and s.capacity("BED") == 1 and s.capacity("NOWHERE") == 1


def test_retold_rumor_keeps_said(graph, monkeypatch):
    monkeypatch.setattr(social, "P_MEET", 1.0)
    monkeypatch.setattr(social, "P_DEAL", 0.0)
    s = Social(PEOPLE, graph)
    r, rumor, facts = _rumors(s)
    rumor.said = True  # his own words, seen false (sightings.py)
    r.spread(DAY + _h(9), DAY + _h(23))
    assert [(f["to"], f["hops"], f.get("said")) for f in facts] == [(HILDA, 1, True), (VINO, 2, True)]
    # two retellings away it is still his words, not a quest he claimed
    deals = Deals.__new__(Deals)
    deals.chronicle = type("C", (), {"facts": lambda self, kind=None: facts})()
    known, _, _ = deals.lies_of(VINO)
    assert known[rumor.key].words(VINO) == (
        "обманул человека по имени Лобарт: сказал «Убрать поле репы», а это неправда"
    )
    r2, _, facts2 = _rumors(s)
    r2.spread(DAY + _h(9), DAY + _h(23))
    assert facts2 and all("said" not in f for f in facts2)


def test_a_live_pass_is_staged_fact_first(graph, monkeypatch):
    monkeypatch.setattr(social, "P_MEET", 1.0)
    monkeypatch.setattr(social, "P_DEAL", 0.0)
    r, rumor, facts = _rumors(Social(PEOPLE, graph))
    seen: list[list[dict]] = []
    asked, staged = _staged(r, "words")
    on_staged = r.on_staged
    r.on_staged = lambda *args: (seen.append(list(facts)), on_staged(*args))
    assert r.spread(DAY + _h(9), DAY + _h(9) + 1) == 1  # a tick: the farm's meeting goes on
    assert asked == [(LOBART, HILDA)]
    [(a, b, tier, fact)] = staged
    assert (a, b, tier) == (LOBART, HILDA, "words")
    assert facts == [fact] and seen == [[fact]]  # the fact was written before the scene
    assert (fact["how"], fact["hops"], fact["where"], fact["seen_by"]) == (
        "talked",
        1,
        "NW_FARM1",
        ["PLAYER", LOBART],
    )
    assert r.tells[(rumor.key, LOBART, 1)] == 1


def test_far_and_sight_keep_how(graph, monkeypatch):
    monkeypatch.setattr(social, "P_MEET", 1.0)
    monkeypatch.setattr(social, "P_DEAL", 0.0)
    s = Social(PEOPLE, graph)
    # far: only the fact, as it always was
    r, _, facts = _rumors(s)
    _, staged = _staged(r, "fact")
    r.spread(DAY + _h(9), DAY + _h(9) + 1)
    assert [f["how"] for f in facts] == ["met"] and "where" not in facts[0] and not staged
    # seen, not heard: how it passed stays, where and who saw it come with it
    r, _, facts = _rumors(s)
    _, staged = _staged(r, "sight")
    r.spread(DAY + _h(9), DAY + _h(9) + 1)
    assert [(f["how"], f["where"], f["seen_by"]) for f in facts] == [("met", "NW_FARM1", ["PLAYER", LOBART])]
    assert [(a, b, tier) for a, b, tier, _ in staged] == [(LOBART, HILDA, "sight")]
    # dealings (Andre, once a day), seen
    monkeypatch.setattr(social, "P_MEET", 0.0)
    monkeypatch.setattr(social, "P_DEAL", 1.0)
    at = next(s.ties[LOBART][ANDRE].occasions(LOBART, ANDRE, DAY, 2 * DAY - 1))
    r, _, facts = _rumors(s)
    _staged(r, "sight")
    r.spread(at - 1, at)
    assert [(f["to"], f["how"], f["where"]) for f in facts] == [(ANDRE, "dealings", "NW_FARM1")]


def test_a_time_jump_stages_nothing(graph, monkeypatch):
    monkeypatch.setattr(social, "P_MEET", 1.0)
    monkeypatch.setattr(social, "P_DEAL", 1.0)
    s = Social(PEOPLE, graph)
    # a sleep of 10 h; a jump whose target minute is the start of the farm's day (8:00); 3 minutes at once
    for after, until in (
        (DAY + _h(9), DAY + _h(19)),
        (DAY + _h(7.5) + 10, DAY + _h(8)),
        (DAY + 477, DAY + 480),
    ):
        before, _, facts_before = _rumors(s)
        before.spread(after, until)
        r, _, facts = _rumors(s)
        asked, staged = _staged(r, "words")
        r.spread(after, until)
        assert facts == facts_before and facts and not asked and not staged
        assert {f["how"] for f in facts} <= {"met", "dealings"}


def test_a_busy_pass_waits_while_the_meeting_goes_on(graph, monkeypatch):
    monkeypatch.setattr(social, "P_MEET", 1.0)
    monkeypatch.setattr(social, "P_DEAL", 0.0)
    s = Social(PEOPLE, graph)
    # busy: no fact, no tell; the next tick asks again; then they are free and talk
    r, rumor, facts = _rumors(s)
    asked, _ = _staged(r, "wait", "wait", "words")
    assert r.spread(DAY + _h(9), DAY + _h(9) + 1) == 0
    assert not facts and not any(r.tells.values()) and HILDA not in rumor.known
    assert r.waiting == {(rumor.key, LOBART, HILDA): DAY + _h(9) + 1}
    assert r.spread(DAY + _h(9) + 1, DAY + _h(9) + 2) == 0
    assert r.spread(DAY + _h(9) + 2, DAY + _h(9) + 3) == 1
    assert asked == [(LOBART, HILDA)] * 3 and [f["how"] for f in facts] == ["talked"] and not r.waiting

    def ticks(r: Rumors, facts: list[dict], start: int) -> int:
        """Tick a minute at a time from `start` until a fact: the minute it came."""
        for m in range(start, start + DAY):
            r.spread(m, m + 1)
            if facts:
                return m + 1
        raise AssertionError("no fact")

    # busy for good: a fact as it always was after DEFER_MAX_MIN
    r, _, facts = _rumors(s)
    _, staged = _staged(r, "wait")
    assert ticks(r, facts, DAY + _h(9)) == DAY + _h(9) + 1 + social.DEFER_MAX_MIN
    assert [f["how"] for f in facts] == ["met"] and not staged and not r.waiting
    # busy until the meeting ends (the farm's day, at 22:00): the last minute it can come is 21:59
    r, _, facts = _rumors(s)
    _staged(r, "wait")
    assert ticks(r, facts, DAY + _h(21.5)) == DAY + _h(22) - 1 and [f["how"] for f in facts] == ["met"]
    # dealings come once a day: no waiting
    monkeypatch.setattr(social, "P_MEET", 0.0)
    monkeypatch.setattr(social, "P_DEAL", 1.0)
    at = next(s.ties[LOBART][ANDRE].occasions(LOBART, ANDRE, DAY, 2 * DAY - 1))
    r, _, facts = _rumors(s)
    asked, _ = _staged(r, "wait")
    assert r.spread(at - 1, at) == 1 and asked == [(LOBART, ANDRE)]
    assert [(f["to"], f["how"]) for f in facts] == [(ANDRE, "dealings")] and not r.waiting


def test_a_wait_of_an_earlier_meeting_is_over(graph, monkeypatch):
    monkeypatch.setattr(social, "P_MEET", 1.0)
    monkeypatch.setattr(social, "P_DEAL", 0.0)
    r, rumor, facts = _rumors(Social(PEOPLE, graph))
    _staged(r, "wait")
    # busy at the end of the night by the field (till 7:30); the farm's day is another meeting: it waits anew
    r.spread(DAY + _h(7), DAY + _h(7) + 1)
    assert r.waiting == {(rumor.key, LOBART, HILDA): DAY + _h(7) + 1}
    r.spread(DAY + _h(8), DAY + _h(8) + 1)
    assert not facts and r.waiting == {(rumor.key, LOBART, HILDA): DAY + _h(8) + 1}


def test_a_slow_tick_is_live_and_cannot_wait_past_its_meeting(graph, monkeypatch):
    monkeypatch.setattr(social, "P_MEET", 1.0)
    monkeypatch.setattr(social, "P_DEAL", 0.0)
    s = Social(PEOPLE, graph)
    # a tick of LIVE_MIN game minutes (a slow round trip) is still live
    r, _, facts = _rumors(s)
    asked, _ = _staged(r, "words")
    r.spread(DAY + _h(9), DAY + _h(9) + social.LIVE_MIN)
    assert asked == [(LOBART, HILDA)] and [f["how"] for f in facts] == ["talked"]
    # busy at 21:58 in a tick to 21:59: the farm's meeting ends at 22:00, so the next tick would not bring
    # them together again; a fact now, not a wait that is never tried again
    r, _, facts = _rumors(s)
    _staged(r, "wait")
    r.spread(DAY + _h(22) - 3, DAY + _h(22) - 1)
    assert [(f["to"], f["how"]) for f in facts] == [(HILDA, "met")] and not r.waiting


def test_gate_holds_and_forces(graph, monkeypatch):
    monkeypatch.setattr(social, "P_MEET", 1.0)
    monkeypatch.setattr(social, "P_DEAL", 1.0)
    s = Social(PEOPLE, graph)
    r, rumor, facts = _rumors(s)
    r.gate = lambda rumor, a, b: False
    assert r.spread(DAY + _h(9), 2 * DAY) == 0 and not facts
    # True: past the lot and the day's tells; None: as always
    monkeypatch.setattr(social, "P_MEET", 0.0)
    monkeypatch.setattr(social, "P_DEAL", 0.0)
    r.gate = lambda rumor, a, b: True if (a, b) == (LOBART, HILDA) else None
    r.tells[(rumor.key, LOBART, 1)] = social.TELLS_PER_DAY
    assert r.spread(DAY + _h(9), DAY + _h(9) + 1) == 1
    assert [(f["to"], f["how"]) for f in facts] == [(HILDA, "met")] and ANDRE not in rumor.known
    # it does not raise the dead nor teach those who know
    r2, rumor2, facts2 = _rumors(s)
    r2.dead.add(HILDA)
    r2.gate = lambda rumor, a, b: True
    assert r2.spread(DAY + _h(9), DAY + _h(9) + 1) == 0 and not facts2


def test_offer_uses_the_same_checks(graph, monkeypatch):
    monkeypatch.setattr(social, "P_MEET", 1.0)
    s = Social(PEOPLE, graph)
    r, rumor, facts = _rumors(s)
    assert not r.offer(rumor, LOBART, HILDA)  # no clock yet
    r.clock = DAY + _h(9)
    assert not r.offer(rumor, LOBART, VATRAS) and LOBART not in s.ties.get(VATRAS, {})  # unacquainted
    assert not r.offer(rumor, HILDA, VINO)  # Hilda does not know it
    r.dead.add(HILDA)
    assert not r.offer(rumor, LOBART, HILDA)
    r.dead.clear()
    rumor.silent.add(LOBART)
    assert not r.offer(rumor, LOBART, HILDA)
    rumor.silent.clear()
    r.tells[(rumor.key, LOBART, 1)] = social.TELLS_PER_DAY
    assert not r.offer(rumor, LOBART, HILDA)
    r.tells.clear()
    monkeypatch.setattr(social, "P_MEET", 0.0)
    assert not r.offer(rumor, LOBART, HILDA)  # the lot
    monkeypatch.setattr(social, "P_MEET", 1.0)
    asked, staged = _staged(r, "wait")
    assert not r.offer(rumor, LOBART, HILDA) and asked == [(LOBART, HILDA)]
    assert not facts and not any(r.tells.values()) and not r.waiting  # busy: the view asks again later
    r.stage = r.on_staged = None
    assert r.offer(rumor, LOBART, HILDA) and [(f["to"], f["how"]) for f in facts] == [(HILDA, "met")]
    assert not r.offer(rumor, LOBART, HILDA)  # she knows it now
    _, staged = _staged(r, "words")
    assert r.offer(rumor, LOBART, ANDRE)  # a tie by dealings only
    assert facts[-1]["how"] == "talked" and [(a, b) for a, b, *_ in staged] == [(LOBART, ANDRE)]
    assert r.tells[(rumor.key, LOBART, 1)] == 2


def test_can_tell_is_offer_without_its_effects(graph, monkeypatch):
    monkeypatch.setattr(social, "P_MEET", 1.0)
    r, rumor, facts = _rumors(Social(PEOPLE, graph))
    assert not r.can_tell(rumor, LOBART, HILDA)  # no clock yet
    r.clock = DAY + _h(9)
    asked, staged = _staged(r, "words")
    # Hilda (they meet), Vatras (unacquainted), Vino from Hilda (she does not know it), Andre (dealings)
    pairs = [(LOBART, HILDA), (LOBART, VATRAS), (HILDA, VINO), (LOBART, ANDRE)]
    assert [r.can_tell(rumor, a, b) for a, b in pairs] == [True, False, False, True]
    # asked again: the same; nothing told, staged, counted or waited for
    assert [r.can_tell(rumor, a, b) for a, b in pairs] == [True, False, False, True]
    assert not facts and not asked and not staged and not r.tells and not r.waiting
    assert set(rumor.known) == {LOBART} and VATRAS not in r.social.ties[LOBART]  # no tie made
    monkeypatch.setattr(social, "P_MEET", 0.0)
    assert not r.can_tell(rumor, LOBART, HILDA) and not r.offer(rumor, LOBART, HILDA)  # the lot
    monkeypatch.setattr(social, "P_MEET", 1.0)
    for a, b in pairs:  # and it says what offer does (Hilda knows it by her turn)
        assert r.can_tell(rumor, a, b) == r.offer(rumor, a, b)
    assert [f["to"] for f in facts] == [HILDA, VINO, ANDRE] and not r.can_tell(rumor, LOBART, HILDA)
    r.tells[(rumor.key, LOBART, 1)] = social.TELLS_PER_DAY
    rumor.known.pop(HILDA)
    assert not r.can_tell(rumor, LOBART, HILDA)  # his tells of the day are spent
    r.gate = lambda rumor, a, b: True
    assert r.can_tell(rumor, LOBART, HILDA)  # the e2e's gate, as for a meeting


def test_offer_status_says_why_it_did_not_pass(graph, monkeypatch):
    monkeypatch.setattr(social, "P_MEET", 1.0)
    r, rumor, facts = _rumors(Social(PEOPLE, graph))
    assert r.offer_status(rumor, LOBART, HILDA) == "no"  # no clock yet
    r.clock = DAY + _h(9)
    asked, staged = _staged(r, "wait", "sight")
    # busy now: no fact, no tell, ask again later
    assert r.offer_status(rumor, LOBART, HILDA, how="visited") == "wait"
    assert not facts and not any(r.tells.values()) and not r.waiting and HILDA not in rumor.known
    # one walked up to the other: seen, not heard, it stays a visit
    assert r.offer_status(rumor, LOBART, HILDA, how="visited") == "told"
    assert [(f["to"], f["hops"], f["how"], f["where"]) for f in facts] == [(HILDA, 1, "visited", "NW_FARM1")]
    assert [(a, b, tier) for a, b, tier, _ in staged] == [(LOBART, HILDA, "sight")]
    assert r.offer_status(rumor, LOBART, HILDA, how="visited") == "no"  # she knows it now
    monkeypatch.setattr(social, "P_MEET", 0.0)
    assert r.offer_status(rumor, LOBART, ANDRE, how="visited") == "no"  # the lot
    assert asked == [(LOBART, HILDA)] * 2  # a no is never staged
    # said aloud near the hero: talked, whatever the occasion
    monkeypatch.setattr(social, "P_MEET", 1.0)
    _staged(r, "words")
    assert r.offer_status(rumor, LOBART, ANDRE, how="visited") == "told" and facts[-1]["how"] == "talked"
    # offer is a meeting's: how met
    r.stage = r.on_staged = None
    assert r.offer(rumor, HILDA, VINO) is True and facts[-1]["how"] == "met"


def test_busy_chain_in_one_spread(monkeypatch):
    monkeypatch.setattr(social, "P_MEET", 1.0)
    farm = [(_h(8), _h(22), FARM)]
    s = Social(
        [
            _person(LOBART, "Лобарт", *farm),
            _person(HILDA, "Хильда", (_h(8), _h(22), [FARM[0] + 1500, FARM[1], FARM[2]])),
            _person(VINO, "Вино", (_h(8), _h(22), [FARM[0] + 3000, FARM[1], FARM[2]])),  # 30 m from Lobart
        ]
    )
    assert VINO in s.ties[HILDA] and VINO not in s.ties[LOBART]
    r, rumor, facts = _rumors(s)
    busy: set[str] = set()
    asked: list[tuple[str, str]] = []

    def stage(rumor: Rumor, a: str, b: str) -> _Staging:
        asked.append((a, b))
        return _Staging("wait" if busy & {a, b} else "words", "NW_FARM1")

    r.stage = stage
    r.on_staged = lambda rumor, a, b, staging, fact: busy.update((a, b))  # their scene begins
    t = DAY + _h(9) + 1
    assert r.spread(t - 1, t) == 1
    # Hilda heard it and would pass it on to Vino the same minute, but she stands in her scene
    assert asked == [(LOBART, HILDA), (HILDA, VINO)] and [f["to"] for f in facts] == [HILDA]
    assert VINO not in rumor.known and r.waiting == {(rumor.key, HILDA, VINO): t}
    busy.clear()  # the scene is over
    assert r.spread(t, t + 1) == 1
    assert [(f["from"], f["to"], f["hops"], f["how"]) for f in facts[1:]] == [(HILDA, VINO, 2, "talked")]
    assert not r.waiting


def test_new_attributes_have_class_defaults(graph, monkeypatch):
    monkeypatch.setattr(social, "P_MEET", 1.0)
    monkeypatch.setattr(social, "P_DEAL", 0.0)
    bare = Rumors.__new__(Rumors)
    assert (bare.stage, bare.on_staged, bare.gate, bare.clock, bare.waiting) == (None,) * 5
    r, rumor, facts = _rumors(Social(PEOPLE, graph))
    assert r.spread(DAY + _h(9), DAY + _h(9) + 1) == 1  # nothing staged: as always
    r2, _, _ = _rumors(Social(PEOPLE, graph))
    _staged(r2, "wait")
    assert r2.spread(DAY + _h(9), DAY + _h(9) + 1) == 0 and r2.waiting and Rumors.waiting is None
    r2.chronicle.facts = lambda kind=None: []
    r2._load()  # a load: the waits are gone with the rumors
    assert r2.waiting is None and not r2.rumors


def test_a_failing_stage_leaves_a_plain_fact(graph, monkeypatch):
    monkeypatch.setattr(social, "P_MEET", 1.0)
    monkeypatch.setattr(social, "P_DEAL", 0.0)
    r, _, facts = _rumors(Social(PEOPLE, graph))

    def stage(rumor: Rumor, a: str, b: str) -> _Staging:
        raise RuntimeError("bug")

    r.stage = stage
    assert r.spread(DAY + _h(9), DAY + _h(9) + 1) == 1 and [f["how"] for f in facts] == ["met"]
    r2, _, facts2 = _rumors(Social(PEOPLE, graph))
    _staged(r2, "words")
    r2.on_staged = lambda *args: 1 / 0
    assert r2.spread(DAY + _h(9), DAY + _h(9) + 1) == 1 and [f["how"] for f in facts2] == ["talked"]


def test_whom_a_character_of_the_directors_knows(graph, monkeypatch):
    monkeypatch.setattr(social, "P_DEAL", 1.0)
    garet = _person("DIR_NPC_1", "Гарет", (_h(6), _h(21), CITY))  # in the city: he never meets Hilda
    s = Social(
        [*PEOPLE, garet], graph, bonds={"DIR_NPC_1": {HILDA: "берёт у неё молоко"}, "DIR_NPC_2": {HILDA: "x"}}
    )
    tie = s.ties["DIR_NPC_1"][HILDA]
    assert tie is s.ties[HILDA]["DIR_NPC_1"] and tie.bonds == ["берёт у неё молоко"] and tie.meets == []
    assert "DIR_NPC_2" not in s.ties  # not in the world
    assert "Хильда (BAU_951_HILDA): знакомы: берёт у неё молоко" in s.describe("DIR_NPC_1")
    # news goes between them once a day, as through dealings
    assert len(list(tie.occasions("DIR_NPC_1", HILDA, 0, 3 * DAY))) == 3


def test_hilda_calls_a_character_she_knows_by_name():
    class Link:
        listeners: list = []

    garet = _person("DIR_NPC_1", "Гарет", (_h(6), _h(21), CITY))
    look = Look(id="DIR_NPC_1", name="Гарет", distance=150)
    for bonds, label in (({}, "незнакомец"), ({"DIR_NPC_1": {HILDA: "берёт у неё молоко"}}, "Гарет")):
        s = Social([*PEOPLE, garet], None, bonds=bonds)
        assert PerceptionDesk(Link(), social=lambda s=s: s).label(HILDA, look) == label  # type: ignore[arg-type]
