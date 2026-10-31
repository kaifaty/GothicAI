from collections.abc import AsyncIterator, Collection
from contextlib import asynccontextmanager
from copy import deepcopy
from dataclasses import replace
from typing import Any

import pytest
from pydantic import ValidationError

from gothic_ai import questbook
from gothic_ai.chains import CACHE_KEY, PROPOSE, ChainAuthor, Proposal
from gothic_ai.crimes import Crimes
from gothic_ai.game import GameError
from gothic_ai.llm import ToolCall
from gothic_ai.llm.fake import FakeBackend, FakeStep
from gothic_ai.memory import MemoryStore
from gothic_ai.places import PlaceBook
from gothic_ai.protocol import NpcSpec, NpcThing, RoutineEntry
from gothic_ai.story import QUEST
from gothic_ai.validator import LOG_FAILED, LOG_OBSOLETE, LOG_RUNNING, LOG_SUCCESS, Patches
from tests.fake_game import FakeGame
from tests.test_authoring import BOOK, _author, graph  # noqa: F401 - the graph fixture
from tests.test_dialogue import _until
from tests.test_director import HILDA, LOBART
from tests.test_director import QUEST as VANILLA_QUEST
from tests.test_questbook import FUNCS

YARD = "FARM1_ENTRANCE"
FIELD = "FARM1_FIELD"
GOODS = "ITMI_SAW"
STOLEN = "ITFO_BREAD"


def _talk(description: str) -> dict[str, Any]:
    return {
        "description": description,
        "lines": [{"speaker": "hero", "text": description}, {"speaker": "npc", "text": "Договорились."}],
        "remember": "Ты поговорил с героем об украденном товаре.",
    }


def _proposal() -> dict[str, Any]:
    return {
        "cast": [
            {
                "role": "thief",
                "kind": "townsman",
                "place": YARD,
                "count": 1,
                "about": "Вор, укравший товар торговца.",
                "carries": [GOODS],
            },
            {
                "role": "camp",
                "kind": "bandit",
                "place": FIELD,
                "count": 2,
                "about": "Бандиты, которые скупают краденый товар.",
                "carries": [],
            },
        ],
        "quests": [
            {
                "title": "Украденный товар",
                "give": _talk("Тебе нужна помощь?"),
                "stages": [
                    {"journal": "Надо найти вора.", "goals": [{"kind": "reach", "npc": "thief"}]},
                    {
                        "journal": "Товар находится у вора.",
                        "goals": [{"kind": "have", "npc": "thief", "item": GOODS}],
                    },
                    {
                        "journal": "Надо вернуть товар заказчику.",
                        "goals": [
                            {"kind": "bring", "npc": "giver", "item": GOODS, "talk": _talk("Вот товар.")}
                        ],
                    },
                ],
                "done_journal": "Я вернул украденный товар.",
                "knows": "У тебя украли товар, вор скрывается поблизости.",
                "asked": "найти вора и вернуть товар",
                "gold": 20,
                "xp": 40,
            },
            {
                "title": "Лагерь",
                "give": _talk("Что с теми, кто скупал краденое?"),
                "stages": [
                    {"journal": "Надо очистить лагерь.", "goals": [{"kind": "kill", "npcs": ["camp"]}]},
                    {
                        "journal": "Надо сообщить заказчику о лагере.",
                        "goals": [{"kind": "talk", "npc": "giver", "talk": _talk("Лагерь пуст.")}],
                    },
                ],
                "done_journal": "Я разобрался с бандитами и рассказал заказчику.",
                "knows": "В лагере рядом с вором скупают краденый товар.",
                "asked": "разобраться с бандитами и рассказать об этом",
                "gold": 30,
                "xp": 50,
            },
        ],
        "reason": "Вор ведёт к лагерю скупщиков краденого.",
    }


class StubCasting:
    """Only the casting model is replaced; the characters and their branch records use the real link."""

    def __init__(self, author, *, partial_group: bool = False) -> None:
        self.author = author
        self.calls: list[tuple[Any, ...]] = []
        self.specs: list[NpcSpec] = []
        self.premises: list[str] = []
        self.partial_group = partial_group

    async def _make(self, place: str, point: str, name: str, carries: list[str]) -> NpcSpec:
        spec = NpcSpec(
            npc=f"DIR_NPC_{len(self.specs) + 1}",
            like="VLK_4000_BUERGER",
            at=point,
            name=name,
            items=[NpcThing(item=item, count=1) for item in carries],
            routine=[
                RoutineEntry(start="06:00", end="21:00", state="ZS_STAND_WP", waypoint=point),
                RoutineEntry(start="21:00", end="06:00", state="ZS_STAND_WP", waypoint=point),
            ],
        )
        assert self.author.places.of(spec.at).id == place
        await self.author.patches.introduce(spec)
        self.specs.append(spec)
        return spec

    async def character(
        self, place: str, *, kind: str, about: str, carries: list[str], premise: str = ""
    ) -> NpcSpec:
        self.calls.append(("character", place, kind, about, tuple(carries)))
        self.premises.append(premise)
        return await self._make(place, self.author.places.get(place).points[0], "Рутгер", carries)

    async def bandits(
        self,
        place: str,
        count: int,
        carries: list[str] | None = None,
        *,
        occupied_points: Collection[str] = (),
    ) -> tuple[list[NpcSpec], list[str]]:
        self.calls.append(("bandits", place, count, tuple(carries or []), tuple(occupied_points)))
        points = [p for p in self.author.places.get(place).points if p not in occupied_points]
        assert count <= len(points)
        group = []
        for point in points[:count]:
            group.append(await self._make(place, point, "Бандит", carries or []))
            if self.partial_group:
                self.partial_group = False
                return group, ["игра не сделала второго бандита"]
        return group, []


@asynccontextmanager
async def _setup(
    story_graph,
    script: list[dict[str, Any]] | None = None,
    *,
    partial_group: bool = False,
    extra_points: dict[str, int] | None = None,
    with_crimes: bool = False,
) -> AsyncIterator[tuple[FakeGame, ChainAuthor, StubCasting, FakeBackend]]:
    fake = FakeGame(branch="A")
    steps = [FakeStep("", [ToolCall(PROPOSE, raw, f"chain{i}")]) for i, raw in enumerate(script or [])]
    llm = FakeBackend(script=steps, delay=0)
    link_cm, author, _ = await _author(fake, llm, story_graph, MemoryStore(":memory:"))
    crimes = None
    try:
        data = deepcopy(BOOK)
        data["points"].update(extra_points or {})
        for place, kind in zip(data["places"], ["house", "yard", "field", "street"], strict=True):
            place["kind"] = kind
        author.places = PlaceBook(data)
        author.book.places = author.places
        fake.symbols.update(dict.fromkeys(FUNCS, "func"))
        fake.people = [{"npc": HILDA, "name": "Хильда", "routine": [{"waypoint": "NW_FARM1_INHOUSE_01"}]}]
        casting = StubCasting(author, partial_group=partial_group)
        crimes = (
            Crimes(
                author.link,
                author.chronicle,
                places=author.places,
                names=lambda npc: {HILDA: "Хильда", LOBART: "Лобарт"}.get(npc, npc),
            )
            if with_crimes
            else None
        )
        chains = ChainAuthor(author, casting, crimes=crimes)
        await _until(lambda: bool(fake.graph))
        yield fake, chains, casting, llm
    finally:
        if crimes is not None:
            await crimes.close()
        await link_cm.__aexit__(None, None, None)


def test_a_proposal_parses_roles_and_book_descriptions_without_ids_or_after():
    raw = _proposal()
    parsed = Proposal.of(raw)
    assert parsed.cast[0].role == "thief" and parsed.cast[0].carries == [GOODS]
    assert parsed.cast[1].count == 2
    assert parsed.quests[0].stages[1].goals[0].npc == "thief"
    assert parsed.quests[1].stages[0].goals[0].npcs == ["camp"]
    assert parsed.reason == raw["reason"]


@pytest.mark.parametrize(
    "bad", ["missing_reason", "wrong_count_type", "extra_field", "not_a_quest", "speaker"]
)
def test_malformed_proposals_are_not_coerced_into_valid_ones(bad):
    raw = _proposal()
    if bad == "missing_reason":
        del raw["reason"]
    elif bad == "wrong_count_type":
        raw["cast"][0]["count"] = True
    elif bad == "extra_field":
        raw["quests"][0]["after"] = "DIR_Q999"
    elif bad == "not_a_quest":
        raw["quests"][0] = "украденный товар"
    else:
        raw["quests"][0]["give"]["lines"][0]["speaker"] = "narrator"
    with pytest.raises(ValidationError):
        Proposal.of(raw)


@pytest.mark.parametrize(
    ("case", "expected"),
    [
        ("reserved_role", "уникальный короткий id"),
        ("duplicate_role", "уникальный короткий id"),
        ("unknown_kind", "неизвестный вид"),
        ("role_place", "места 'CITY' нет в сводке"),
        ("goal_place", "места 'CITY' нет в сводке"),
        ("bandit_yard", "жилое"),
        ("occupied_camp", "жилое"),
        ("role_item", "вещи 'ITMI_UNKNOWN' нет в сводке"),
        ("goal_item", "вещи 'ITMI_UNKNOWN' нет в сводке"),
        ("missing_carries", "неоткуда взять"),
        ("gold", "золота 51"),
        ("xp", "опыта 51"),
        ("knows", "нужны knows и asked"),
        ("asked", "нужны knows и asked"),
        ("foreign_npc", f"NPC '{LOBART}' нет в сводке и ролях"),
        ("foreign_kill", "неизвестная роль"),
        ("crowd_npc", "нужен один человек"),
        ("non_bandit_group", "count=1"),
        ("too_many_bandits", "count=1"),
        ("too_few_points", "не хватает разных точек"),
        ("duplicate_title", "поручение «Украденный товар» уже было"),
        ("no_source", "неоткуда взять"),
        ("one_quest", "2–3 квеста"),
        ("no_turn_in", "последняя"),
    ],
)
async def test_every_invalid_description_is_rejected_before_casting(graph, case, expected):  # noqa: F811
    async with _setup(graph) as (fake, chains, casting, _):
        brief = await chains.brief(HILDA)
        assert brief is not None
        raw = _proposal()
        thief, camp = raw["cast"]
        first, second = raw["quests"]
        if case == "reserved_role":
            thief["role"] = "giver"
        elif case == "duplicate_role":
            camp["role"] = thief["role"]
        elif case == "unknown_kind":
            thief["kind"] = "knight"
        elif case == "role_place":
            thief["place"] = "CITY"
        elif case == "goal_place":
            first["stages"][0]["goals"][0] = {"kind": "reach", "place": "CITY"}
        elif case == "bandit_yard":
            camp["place"] = YARD
        elif case == "occupied_camp":
            brief.occupied.add(FIELD)
        elif case == "role_item":
            thief["carries"] = ["ITMI_UNKNOWN"]
        elif case == "goal_item":
            first["stages"][1]["goals"][0]["item"] = "ITMI_UNKNOWN"
        elif case == "missing_carries":
            thief["carries"] = []
        elif case in ("gold", "xp"):
            second[case] = 51
        elif case in ("knows", "asked"):
            second[case] = " "
        elif case == "foreign_npc":
            second["stages"][-1]["goals"][0]["npc"] = LOBART
        elif case == "foreign_kill":
            second["stages"][0]["goals"][0]["npcs"] = [LOBART]
        elif case == "crowd_npc":
            first["stages"][0]["goals"][0]["npc"] = "camp"
        elif case == "non_bandit_group":
            thief["count"] = 2
        elif case == "too_many_bandits":
            camp["count"] = questbook.MAX_KILL + 1
        elif case == "too_few_points":
            camp["count"] = 3
        elif case == "duplicate_title":
            second["title"] = first["title"]
        elif case == "no_source":
            del first["stages"][1]
        elif case == "one_quest":
            raw["quests"] = [first]
        elif case == "no_turn_in":
            second["stages"].pop()
        made, errors = await chains._try(raw, brief)
        assert made is None and any(expected in error for error in errors), errors
        assert casting.calls == [] and fake.cast == {} and brief.roles == {}
        assert chains.book.all() == {} and chains.author.patches.records(QUEST) == []
        assert len(fake.graph) == 1  # the session's initial replace, no proposed graph


@pytest.mark.parametrize("kind", ["house", "yard", "street"])
async def test_a_bandit_cannot_live_in_any_residential_kind(graph, kind):  # noqa: F811
    async with _setup(graph) as (_, chains, casting, _):
        brief = await chains.brief(HILDA)
        assert brief is not None
        brief.places[FIELD] = replace(brief.places[FIELD], kind=kind)
        made, errors = await chains._try(_proposal(), brief)
        assert made is None and any("жилое" in error for error in errors)
        assert casting.calls == []


@pytest.mark.parametrize("max_people", [0, 1, 2])
async def test_the_chain_people_budget_counts_each_bandit_before_casting(graph, max_people):  # noqa: F811
    async with _setup(graph) as (fake, chains, casting, _):
        brief = await chains.brief(HILDA)
        assert brief is not None
        brief.max_people = max_people
        made, errors = await chains._try(_proposal(), brief)
        assert made is None and f"новых персонажей 3, а бюджет цепочки — не больше {max_people}" in errors
        assert casting.calls == [] and fake.cast == {} and brief.roles == {}
        assert chains.book.all() == {}


async def test_a_bad_structure_never_reaches_casting_or_the_book(graph):  # noqa: F811
    async with _setup(graph) as (fake, chains, casting, _):
        brief = await chains.brief(HILDA)
        assert brief is not None
        raw = _proposal()
        raw["cast"][0]["count"] = "1"
        made, errors = await chains._try(raw, brief)
        assert made is None and any("структура предложения" in error for error in errors)
        assert casting.calls == [] and fake.cast == {} and chains.book.all() == {}


async def test_the_brief_keeps_author_context_and_marks_people_and_occupied_places(graph):  # noqa: F811
    async with _setup(graph) as (fake, chains, _, _):
        fake.people.append({"npc": LOBART, "name": "Лобарт", "routine": [{"waypoint": "NW_FARM1_FIELD_01"}]})
        brief = await chains.brief(HILDA.lower())
        assert brief is not None
        assert brief.base.npc == HILDA and brief.base.items[GOODS] == "Пила"
        assert (brief.base.gold, brief.base.xp) == (50, 50)
        assert brief.people == {HILDA: "Хильда", LOBART: "Лобарт"}
        assert FIELD in brief.occupied and not brief.bandit_ok(FIELD) and not brief.bandit_ok(YARD)
        text = brief.text()
        assert "Кто просит: Хильда" in text and "Награда: опыт до 50, золото до 50." in text
        assert f"{FIELD} —" in text and f"{LOBART} — Лобарт" in text


@pytest.mark.parametrize(
    ("npc", "personal", "guild", "guild_attitude", "included"),
    [
        (LOBART, 0, 3, 2, False),
        (LOBART, 2, 3, 0, False),
        (LOBART, 3, 3, 0, False),
        ("DIR_NPC_BANDIT", 2, 9, 2, False),
        (LOBART, 2, 3, 2, True),
        (LOBART, 3, 3, 2, True),
    ],
)
async def test_people_filter_hostility(graph, npc, personal, guild, guild_attitude, included):  # noqa: F811
    async with _setup(graph) as (fake, chains, _, _):
        fake.people.append({"npc": npc, "name": "Сосед", "routine": [{"waypoint": "NW_FARM1_FIELD_01"}]})
        fake.attitudes[npc] = personal
        fake.fields[(npc, "GUILD")] = guild
        fake.fields[("HERO", "GUILD")] = 7
        fake.guild_attitudes[(guild, 7)] = guild_attitude
        brief = await chains.brief(HILDA)
        assert brief is not None
        assert (npc in brief.people) == included
        assert HILDA in brief.people
        assert FIELD in brief.occupied and not brief.bandit_ok(FIELD)
        assert (f"{npc} — Сосед" in brief.text()) == included
        batches = [r for r in fake.story if r.get("type") == "story_eval" and "NPC_GETATTITUDE" in str(r)]
        assert len(batches) == 1 and batches[0]["npc"] == HILDA


async def test_a_hostile_giver_cannot_start_a_chain_with_an_impossible_turn_in(graph):  # noqa: F811
    async with _setup(graph, [_proposal()]) as (fake, chains, casting, llm):
        fake.attitudes[HILDA] = 0
        assert await chains.chain(HILDA) is None
        assert not llm.calls and not casting.calls and not chains.book.all()


@pytest.mark.parametrize("kind", ["talk", "bring"])
async def test_a_hostile_person_from_the_world_cannot_be_a_chain_dialog_target(graph, kind):  # noqa: F811
    async with _setup(graph) as (fake, chains, casting, _):
        fake.people.append({"npc": LOBART, "name": "Лобарт", "routine": [{"waypoint": "NW_FARM1_OUT_01"}]})
        fake.attitudes[LOBART] = 0
        brief = await chains.brief(HILDA)
        assert brief is not None and LOBART not in brief.people
        raw = _proposal()
        raw["quests"][0]["stages"][-1]["goals"] = [
            {"kind": kind, "npc": LOBART, "item": GOODS if kind == "bring" else "", "talk": _talk("Вот.")}
        ]
        made, errors = await chains._try(raw, brief)
        assert made is None and any(f"NPC '{LOBART}' нет в сводке и ролях" in e for e in errors)
        assert not casting.calls and not fake.cast and not chains.book.all()


@pytest.mark.parametrize("kind", ["talk", "bring"])
async def test_even_a_single_new_bandit_cannot_be_a_chain_dialog_target(graph, kind):  # noqa: F811
    async with _setup(graph) as (fake, chains, casting, _):
        brief = await chains.brief(HILDA)
        assert brief is not None
        raw = _proposal()
        raw["cast"][1]["count"] = 1
        raw["quests"][0]["stages"][-1]["goals"] = [
            {"kind": kind, "npc": "camp", "item": GOODS if kind == "bring" else "", "talk": _talk("Вот.")}
        ]
        made, errors = await chains._try(raw, brief)
        assert made is None and any("бандит враждебен герою, talk/bring невозможны" in e for e in errors)
        assert not casting.calls and not fake.cast and not chains.book.all()


async def test_all_roles_are_alive_placeholders_and_earlier_quests_exist_during_preflight(graph, monkeypatch):  # noqa: F811
    async with _setup(graph) as (fake, chains, casting, _):
        seen = []
        original_problems = questbook.problems

        def check(q, ground):
            if not casting.calls:
                roles = {n for n in ground.symbols if n.startswith("DIR_NPC_CHAIN_")}
                assert len(roles) == 3
                assert all(ground.symbols[n] == "npc" and ground.states[n] == "alive" for n in roles)
                assert not any(n in fake.npc_states for n in roles)
                seen.append((q.id, q.after, set(ground.quests)))
            return original_problems(q, ground)

        monkeypatch.setattr(questbook, "problems", check)
        brief = await chains.brief(HILDA)
        assert brief is not None
        made, errors = await chains._try(_proposal(), brief)
        assert errors == [] and made is not None
        assert seen == [("DIR_Q1", "", set()), ("DIR_Q2", "DIR_Q1", {"DIR_Q1"})]
        first, second = made
        assert all(q.giver == HILDA for q in made)
        assert first.after == "" and second.after == first.id
        assert first.stages[0].goals[0].npc == brief.roles["thief"][0].npc
        assert first.stages[1].goals[0].npc == brief.roles["thief"][0].npc
        assert first.stages[-1].goals[0].npc == HILDA
        assert second.stages[0].goals[0].npcs == [s.npc for s in brief.roles["camp"]]
        assert [c[0] for c in casting.calls] == ["character", "bandits"]
        assert casting.calls[0][1:] == (YARD, "townsman", _proposal()["cast"][0]["about"], (GOODS,))
        assert [thing.item for thing in brief.roles["thief"][0].items] == [GOODS]
        assert len(fake.cast) == 3 and len({s.at for s in brief.roles["camp"]}) == 2
        assert set(chains.book.all()) == {q.id for q in made}
        assert fake.graph[-1]["ops"] == [op for q in made for op in questbook.build(q)]


async def test_reach_and_have_places_are_converted_to_points_by_the_code(graph):  # noqa: F811
    async with _setup(graph) as (_, chains, _, _):
        raw = _proposal()
        raw["quests"][0]["stages"][0]["goals"][0] = {"kind": "reach", "place": FIELD}
        raw["quests"][0]["stages"][1]["goals"][0] = {"kind": "have", "place": YARD, "item": GOODS}
        brief = await chains.brief(HILDA)
        assert brief is not None
        made, errors = await chains._try(raw, brief)
        assert errors == [] and made is not None
        reach = made[0].stages[0].goals[0]
        have = made[0].stages[1].goals[0]
        assert reach.point in brief.places[FIELD].points and reach.npc == ""
        assert have.point in brief.places[YARD].points and have.npc == ""


def _single_proposal() -> dict[str, Any]:
    raw = _proposal()
    raw["cast"] = []
    raw["quests"] = raw["quests"][:1]
    quest = raw["quests"][0]
    quest.update(
        title="Починить изгородь",
        knows="Изгородь у поля разошлась, прежде чем пилить доски, надо осмотреть её.",
        asked="обсудить ремонт, осмотреть изгородь и принести пилу с места работы",
        done_journal="Я осмотрел изгородь и принёс пилу для ремонта.",
        stages=[
            {
                "journal": "Надо обсудить ремонт.",
                "goals": [{"kind": "talk", "npc": "giver", "talk": _talk("Как чинить изгородь?")}],
            },
            {"journal": "Надо осмотреть изгородь.", "goals": [{"kind": "reach", "place": FIELD}]},
            {
                "journal": "Надо взять пилу у поля.",
                "goals": [{"kind": "have", "item": GOODS, "place": FIELD}],
            },
            {
                "journal": "Надо передать пилу для ремонта.",
                "goals": [{"kind": "bring", "npc": "giver", "item": GOODS, "talk": _talk("Вот пила.")}],
            },
        ],
    )
    return raw


def _record_crime(chains: ChainAuthor, **changes: Any) -> dict[str, Any]:
    fact = {
        "fact": "crime",
        "id": "stolen-bread",
        "kind": "theft",
        "victim": HILDA,
        "owner": HILDA,
        "community": "farm",
        "item": STOLEN,
        "item_name": "Хлеб",
        "count": 1,
        "waypoint": "NW_FARM1_INHOUSE_01",
        "place": {"x": 0, "y": 0, "z": 0},
        "day": 2,
        "hour": 10,
        "minute": 15,
        "witnesses": [LOBART],
        **changes,
    }
    chains.chronicle.record(fact)
    return fact


def _return_loot(item: str = STOLEN) -> dict[str, Any]:
    raw = _single_proposal()
    raw["quests"][0].update(
        title="Пропавший хлеб",
        knows="Из дома пропал хлеб. Ты не знаешь, кто его взял.",
        asked="найти пропавший хлеб и вернуть",
        gold=0,
        xp=0,
        stages=[
            {
                "journal": "У меня есть пропавший хлеб.",
                "goals": [{"kind": "have", "npc": "PC_HERO", "item": item}],
            },
            {
                "journal": "Надо вернуть пропавший хлеб.",
                "goals": [{"kind": "bring", "npc": "giver", "item": item, "talk": _talk("Нашёл твой хлеб.")}],
            },
        ],
        done_journal="Я вернул пропавший хлеб, умолчав о том, что сам его взял.",
    )
    return raw


async def test_crime_brief_separates_the_directors_truth_from_peoples_knowledge(graph):  # noqa: F811
    async with _setup(graph, with_crimes=True) as (_, chains, _, _):
        deed = _record_crime(chains)
        chains.chronicle.record({"fact": "crime_seen", "crime": deed["id"], "npc": LOBART})
        chains.chronicle.record(
            {"fact": "rumor", "crime": deed["id"], "from": LOBART, "to": "BAU_952_MALAK", "hops": 1}
        )
        brief = await chains.brief(HILDA, crime=deed["id"])
        assert brief is not None and brief.crime == deed
        assert STOLEN not in brief.base.items and brief.crime_items == {STOLEN: "Хлеб"}
        text = brief.text()
        assert "Правда режиссёра: герой взял Хлеб" in text
        assert "в доме Лобарта (день 2, 10:15)" in text
        assert f"{HILDA} — Хильда; пострадавший; о деле и виновнике не знает" in text
        assert f"{LOBART} — Лобарт; свидетель; видел" in text
        assert "BAU_952_MALAK; узнал о деле; слышал" in text
        assert "Пострадавший может заметить пропажу, не узнав, кто взял вещь" in text
        assert "npc=PC_HERO" in text
        assert chains.crimes.known(HILDA) == []


@pytest.mark.parametrize("cover", [True, False])
async def test_old_cover_testimony_enters_chain_context_when_cover_is_disabled(graph, cover):  # noqa: F811
    async with _setup(graph, with_crimes=True) as (_, chains, _, _):
        deed = _record_crime(chains)
        chains.crimes.cover = cover
        chains.chronicle.record({
            "fact": "testimony", "npc": "REMOTE_KNOWER", "witness": LOBART, "cover": "D1",
            "crime_saw": [deed["id"]], "crime_told": [],
        })
        text = chains._crime_context(deed, {})
        assert ("- REMOTE_KNOWER" in text) is not cover


async def test_the_hero_can_return_his_own_loot_without_spawning_it_or_being_forgiven(graph):  # noqa: F811
    async with _setup(graph, [_return_loot()], with_crimes=True) as (fake, chains, casting, _):
        deed = _record_crime(chains, witnesses=[])
        fake.symbols.update({STOLEN: "item", "PC_HERO": "npc"})
        fake.hero_items[STOLEN] = 1
        made = await chains.chain(HILDA, crime=deed["id"], quest_count=1, max_people=0)
        assert made is not None and len(made) == 1 and not casting.calls
        [quest] = made
        assert quest.stages[0].goals[0].npc == "PC_HERO"
        assert quest.stages[-1].goals[0].item == STOLEN
        assert "WLD_INSERTITEM" not in str(fake.graph[-1])
        assert chains.chronicle.facts("chain")[-1]["crime"] == deed["id"]

        nodes = Patches.of(chains.author.patches.ops()).nodes
        done = nodes[f"{quest.id}_DONE"]
        await fake.send(type="dialog_chosen", npc=HILDA, node=f"{quest.id}_GIVE", made=True)
        await _until(lambda: chains.book.progress(quest.id).state == "given")
        await fake.send(type="trigger_fired", id=f"{quest.id}_S1G1", count=1, npc="PC_HERO", other="PC_HERO")
        await fake.send(type="trigger_fired", id=f"{quest.id}_S1", count=1, npc="PC_HERO", other="PC_HERO")
        await _until(lambda: chains.book.progress(quest.id).stage == 2)
        fake.vars.update({quest.id: LOG_RUNNING, f"{quest.id}_STAGE": 2})
        assert fake.evaluate(done["condition"], HILDA) == 1
        assert fake.give({"npc": HILDA, "effects": done["effects"]}) is None
        assert fake.hero_items[STOLEN] == 0 and fake.npc_items[HILDA][STOLEN] == 1
        await fake.send(type="dialog_chosen", npc=HILDA, node=f"{quest.id}_DONE", made=True)
        await _until(lambda: chains.book.progress(quest.id).state == "done")
        assert chains.chronicle.facts("forgiven") == []
        assert chains.crimes.known(HILDA) == []


async def test_only_the_item_of_the_linked_crime_is_allowed_beyond_the_item_pool(graph):  # noqa: F811
    async with _setup(graph, with_crimes=True) as (fake, chains, casting, _):
        linked = _record_crime(chains)
        _record_crime(chains, id="other-theft", item="ITFO_WINE")
        fake.symbols.update({STOLEN: "item", "ITFO_WINE": "item", "PC_HERO": "npc"})
        brief = await chains.brief(HILDA, crime=linked["id"])
        assert brief is not None
        brief.quest_count = 1
        made, errors = await chains._try(_return_loot("ITFO_WINE"), brief)
        assert made is None and any("вещи 'ITFO_WINE' нет в сводке" in e for e in errors)
        assert not casting.calls and not chains.book.all()


async def test_extra_crime_loot_cannot_be_created_in_a_new_persons_inventory(graph):  # noqa: F811
    async with _setup(graph, with_crimes=True) as (_, chains, casting, _):
        deed = _record_crime(chains)
        brief = await chains.brief(HILDA, crime=deed["id"])
        assert brief is not None
        raw = _proposal()
        raw["cast"][0]["carries"] = [STOLEN]
        made, errors = await chains._try(raw, brief)
        assert made is None and f"вещи '{STOLEN}' нет в сводке" in errors
        assert not casting.calls


@pytest.mark.parametrize("with_crimes", [False, True])
async def test_disabled_or_unknown_crime_does_not_reach_the_builder_model(graph, with_crimes):  # noqa: F811
    async with _setup(graph, [_return_loot()], with_crimes=with_crimes) as (_, chains, _, llm):
        if not with_crimes:
            _record_crime(chains)
        assert await chains.chain(HILDA, crime="stolen-bread", quest_count=1) is None
        assert not llm.calls and not chains.book.all()


async def test_a_single_errand_uses_book_goals_and_one_atomic_patch(graph):  # noqa: F811
    premise = "Изгородь у поля разошлась: осмотри её и помоги с ремонтом."
    async with _setup(graph, [_single_proposal()]) as (fake, chains, casting, llm):
        made = await chains.chain(HILDA, quest_count=1, max_people=0, premise=premise)
        assert made is not None and len(made) == 1
        [quest] = made
        assert quest.after == "" and quest.giver == HILDA
        assert [g.kind for s in quest.stages for g in s.goals] == ["talk", "reach", "have", "bring"]
        reach, have = quest.stages[1].goals[0], quest.stages[2].goals[0]
        assert reach.point == have.point and have.point and have.npc == ""
        assert not questbook.problems(quest, await chains.book.ground(quest))
        assert not casting.calls and not fake.cast
        assert len(fake.graph) == 2  # initial session replacement plus the whole quest
        assert fake.graph[-1]["ops"] == questbook.build(quest)
        assert list(chains.book.all()) == [quest.id]
        [fact] = chains.chronicle.facts("chain")
        assert fact["quests"] == [quest.id] and fact["roles"] == {}
        instructions, messages, _ = llm.calls[0]
        assert "одно поручение" in instructions and "2–3" not in instructions
        assert "разыгрывать сам замысел" in instructions
        assert premise in messages[0].content and "Число квестов: ровно 1" in messages[0].content
        assert "у него пропала" not in messages[0].content


async def test_place_reach_uses_a_waypoint_but_have_can_still_use_a_freepoint(graph):  # noqa: F811
    freepoints = {f"FP_ROAM_FARM1_FIELD_{i}": 2 for i in range(5)}
    async with _setup(graph, [_single_proposal()], extra_points=freepoints) as (fake, chains, casting, _):
        made = await chains.chain(HILDA, quest_count=1, max_people=0)
        assert made is not None and len(made) == 1
        reach, have = made[0].stages[1].goals[0], made[0].stages[2].goals[0]
        assert reach.point == "NW_FARM1_FIELD_02"
        assert have.point in freepoints
        assert len(fake.graph) == 2 and not casting.calls


async def test_place_with_only_freepoints_cannot_be_a_reach_target_even_with_an_anchor(graph):  # noqa: F811
    freepoints = {f"FP_ROAM_FARM1_FIELD_{i}": 2 for i in range(5)}
    async with _setup(graph, extra_points=freepoints) as (fake, chains, casting, _):
        brief = await chains.brief(HILDA)
        assert brief is not None
        brief.quest_count = 1
        brief.places[FIELD] = replace(brief.places[FIELD], points=tuple(freepoints), anchor="NW_FARM1_OUT_01")
        made, errors = await chains._try(_single_proposal(), brief)
        assert made is None and f"дойти до места {FIELD} нельзя: в нём нет вейпоинтов" in errors
        assert any(
            line.startswith(f"- {FIELD} —") and "дойти: нет" in line for line in brief.text().splitlines()
        )
        assert not casting.calls and not chains.book.all() and len(fake.graph) == 1


async def test_two_quests_requested_as_one_are_rejected_and_explained_to_the_model(graph):  # noqa: F811
    async with _setup(graph, [_proposal(), _single_proposal()]) as (fake, chains, casting, llm):
        made = await chains.chain(HILDA, quest_count=1, max_people=0)
        assert made is not None and len(made) == 1 and len(llm.calls) == 2
        [rejected] = chains.chronicle.facts("chain_rejected")
        assert any("ровно 1" in why and "предложено 2" in why for why in rejected["why"])
        assert all(why in llm.calls[1][1][-1].content for why in rejected["why"])
        assert not casting.calls and len(fake.graph) == 2


async def test_a_single_errand_cannot_add_roles_when_the_stranger_has_used_the_budget(graph):  # noqa: F811
    raw = _single_proposal()
    raw["cast"] = _proposal()["cast"][:1]
    async with _setup(graph, [raw]) as (fake, chains, casting, _):
        assert await chains.chain(HILDA, quest_count=1, max_people=0) is None
        assert not casting.calls and not fake.cast and not chains.book.all()
        assert len(fake.graph) == 1
        assert all(
            any("новых персонажей 1" in why and "не больше 0" in why for why in fact["why"])
            for fact in chains.chronicle.facts("chain_rejected")
        )


async def test_a_dead_role_cannot_be_used_for_a_later_quest_dialog(graph):  # noqa: F811
    async with _setup(graph) as (_, chains, casting, _):
        raw = _proposal()
        raw["quests"][0]["stages"].insert(
            2, {"journal": "Вор не должен уйти.", "goals": [{"kind": "kill", "npcs": ["thief"]}]}
        )
        raw["quests"][1]["stages"][-1]["goals"][0]["npc"] = "thief"
        brief = await chains.brief(HILDA)
        assert brief is not None
        made, errors = await chains._try(raw, brief)
        assert made is None and any("уже убит" in error for error in errors)
        assert casting.calls == []


async def test_rejected_description_is_explained_and_the_second_model_proposal_passes(graph):  # noqa: F811
    invalid = _proposal()
    invalid["cast"][1]["place"] = YARD
    async with _setup(graph, [invalid, _proposal()]) as (fake, chains, casting, llm):
        premise = "Ночью на ферме пропал товар. Следы ведут к скупщикам у поля."
        made = await chains.chain(HILDA, premise=premise)
        assert made is not None and len(made) == 2
        assert len(llm.calls) == 2 and llm.cache_keys == [CACHE_KEY, CACHE_KEY]
        assert all([tool.name for tool in tools] == [PROPOSE] for tools in llm.sent_tools)
        assert all(f"Замысел режиссёра: {premise}" in call[1][0].content for call in llm.calls)
        assert casting.premises == [premise]
        assert "жилое" in llm.calls[1][1][-1].content and "Отклонено" in llm.calls[1][1][-1].content
        assert len(casting.calls) == 2 and len(fake.cast) == 3
        [rejected] = chains.chronicle.facts("chain_rejected")
        assert rejected["attempt"] == 1 and any("жилое" in reason for reason in rejected["why"])
        [fact] = chains.chronicle.facts("chain")
        assert fact["quests"] == [q.id for q in made] and fact["reason"] == _proposal()["reason"]
        assert fact["roles"] == {"thief": ["DIR_NPC_1"], "camp": ["DIR_NPC_2", "DIR_NPC_3"]}


async def test_retry_after_game_refusal_keeps_the_cast_and_shows_its_roles(graph, monkeypatch):  # noqa: F811
    async with _setup(graph, [_proposal(), _proposal()]) as (fake, chains, casting, llm):
        original_patch = chains.link.patch_graph
        proposed = 0

        async def refuse_first(ops, **kwargs):
            nonlocal proposed
            if not kwargs.get("replace"):
                proposed += 1
                if proposed == 1:
                    assert chains.book.all() == {}
                    raise GameError("лагерь временно не принят")
            return await original_patch(ops, **kwargs)

        monkeypatch.setattr(chains.link, "patch_graph", refuse_first)
        made = await chains.chain(HILDA, max_people=3)
        assert made is not None and proposed == 2 and len(llm.calls) == 2
        assert len(casting.calls) == 2 and len(casting.specs) == 3 and len(fake.cast) == 3
        repeated = llm.calls[1][1][0].content
        assert "Готовые роли" in repeated and "thief:" in repeated and "camp:" in repeated
        assert all(spec.npc in repeated for spec in casting.specs)
        assert "Новых персонажей в этой цепочке — не больше 3" in repeated
        assert "лагерь временно не принят" in llm.calls[1][1][-1].content
        assert len(chains.author.patches.records(QUEST)) == 2
        assert [q.id for q in made] == ["DIR_Q1", "DIR_Q2"]


async def test_a_partial_group_is_completed_on_retry_without_replacing_the_existing_bandit(graph):  # noqa: F811
    async with _setup(graph, [_proposal(), _proposal()], partial_group=True) as (fake, chains, casting, llm):
        made = await chains.chain(HILDA, max_people=3)
        assert made is not None and len(llm.calls) == 2
        assert [c[0] for c in casting.calls] == ["character", "bandits", "bandits"]
        first_bandit = casting.specs[1]
        assert casting.calls[1][2] == 2 and casting.calls[2][2] == 1
        assert casting.calls[2][-1] == (first_bandit.at,)
        assert len(fake.cast) == 3 and len({s.at for s in casting.specs[1:]}) == 2
        targets = made[1].stages[0].goals[0].npcs
        assert targets == [s.npc for s in casting.specs[1:]] and targets[0] == first_bandit.npc
        assert first_bandit.npc in llm.calls[1][1][0].content
        assert "игра не сделала второго бандита" in llm.calls[1][1][-1].content


@pytest.mark.parametrize("change", ["changed", "omitted"])
async def test_a_retry_must_keep_the_description_of_already_introduced_roles(graph, change):  # noqa: F811
    async with _setup(graph, partial_group=True) as (fake, chains, casting, _):
        brief = await chains.brief(HILDA)
        assert brief is not None
        assert (await chains._try(_proposal(), brief))[0] is None
        raw = _proposal()
        if change == "changed":
            raw["cast"][0]["about"] = "Другой человек с другим делом."
        else:
            raw["cast"].pop(0)
        calls = len(casting.calls)
        made, errors = await chains._try(raw, brief)
        assert made is None and any("уже поставлена" in error for error in errors)
        assert len(casting.calls) == calls and len(fake.cast) == 2
        assert chains.book.all() == {}


@pytest.mark.parametrize("state", [0, LOG_RUNNING, LOG_SUCCESS, LOG_FAILED, LOG_OBSOLETE])
async def test_after_blocks_the_next_offer_until_the_first_quest_succeeds(graph, state):  # noqa: F811
    async with _setup(graph, [_proposal()]) as (fake, chains, _, _):
        made = await chains.chain(HILDA)
        assert made is not None
        first, second = made
        fake.vars[first.id] = state
        give = Patches.of(chains.author.patches.ops()).nodes[f"{second.id}_GIVE"]
        assert fake.evaluate(give["condition"], HILDA) == int(state == LOG_SUCCESS)


async def test_a_vanilla_response_opens_only_its_first_quest_and_informs_the_giver(graph):  # noqa: F811
    async with _setup(graph, [_proposal()]) as (fake, chains, _, llm):
        fake.vars[VANILLA_QUEST] = LOG_SUCCESS
        requires = [{"kind": "quest", "quest": VANILLA_QUEST, "state": "success"}]
        premise = "После уборки репы на ферме стало видно, что из сарая пропал товар."
        made = await chains.chain(HILDA, premise=premise, requires=requires, follows=VANILLA_QUEST)
        assert made is not None
        first, second = made
        assert first.requires == requires and second.requires == [] and second.after == first.id
        for q, raw in zip(made, _proposal()["quests"], strict=True):
            assert q.knows.startswith(raw["knows"])
            assert graph.quests[VANILLA_QUEST].title in q.knows and premise in q.knows
            assert VANILLA_QUEST not in q.knows
        brief = llm.calls[0][1][0].content
        assert "Поручение отвечает на дело «Убрать поле репы»." in brief and VANILLA_QUEST not in brief
        nodes = Patches.of(chains.author.patches.ops()).nodes
        assert fake.evaluate(nodes[f"{first.id}_GIVE"]["condition"], HILDA) == 1
        fake.vars[VANILLA_QUEST] = LOG_RUNNING
        assert fake.evaluate(nodes[f"{first.id}_GIVE"]["condition"], HILDA) == 0
        fake.vars[first.id] = LOG_SUCCESS
        assert fake.evaluate(nodes[f"{second.id}_GIVE"]["condition"], HILDA) == 1
        chains.book.quests.clear()
        chains.book._version = -1
        assert [chains.book.all()[q.id] for q in made] == made


async def test_only_two_failed_proposals_are_spent_and_no_cast_is_left(graph):  # noqa: F811
    raw = _proposal()
    raw["quests"][1]["gold"] = 51
    async with _setup(graph, [raw]) as (fake, chains, casting, llm):
        assert await chains.chain(HILDA) is None
        assert len(llm.calls) == 2 and casting.calls == [] and fake.cast == {}
        assert [fact["attempt"] for fact in chains.chronicle.facts("chain_rejected")] == [1, 2]
        assert chains.chronicle.facts("chain") == [] and chains.book.all() == {}


async def test_a_chain_can_use_people_from_the_brief_without_new_roles(graph):  # noqa: F811
    raw = _proposal()
    raw["cast"] = []
    for q in raw["quests"]:
        q["stages"] = [
            {
                "journal": "Надо поговорить с заказчиком.",
                "goals": [{"kind": "talk", "npc": "giver", "talk": _talk("Дело сделано.")}],
            }
        ]
    async with _setup(graph, [raw]) as (_, chains, casting, llm):
        made = await chains.chain(HILDA, max_people=0)
        assert made is not None and len(made) == 2 and not casting.calls
        assert "Замысел режиссёра:" not in llm.calls[0][1][0].content
        assert "Новых персонажей в этой цепочке — не больше 0" in llm.calls[0][1][0].content
        assert chains.chronicle.facts("chain")[-1]["roles"] == {}
        assert made[1].after == made[0].id


async def test_asked_for_someone_new_a_chain_of_the_people_around_is_refused(graph):  # noqa: F811
    raw = _proposal()
    raw["cast"] = []
    for q in raw["quests"]:
        q["stages"] = [
            {
                "journal": "Надо поговорить с заказчиком.",
                "goals": [{"kind": "talk", "npc": "giver", "talk": _talk("Дело сделано.")}],
            }
        ]
    async with _setup(graph, [raw, _proposal()]) as (_, chains, casting, llm):
        made = await chains.chain(HILDA, new_people=True)
        assert made is not None and len(casting.calls) == 2
        assert "нужен новый человек" in llm.calls[0][1][0].content
        assert "cast пуст" in llm.calls[1][1][-1].content
        [rejected] = chains.chronicle.facts("chain_rejected")
        assert any("cast пуст" in why for why in rejected["why"])


def _find_npc_thief(crime: str) -> dict[str, Any]:
    raw = _single_proposal()
    raw["quests"][0]["stages"] = [
        {"journal": "Нужно убедить Хильду, кто вор.", "goals": [{"kind": "blame", "crime": crime}]},
        {"journal": "Хильда поверила обвинению.",
         "goals": [{"kind": "talk", "npc": "giver", "talk": _talk("Теперь ты знаешь, кто вор.")}]},
    ]
    return raw


async def test_npc_crime_builder_knows_only_the_linked_deed_and_can_ask_to_convince_giver(graph):  # noqa: F811
    crime = "npc-stolen-bread"
    async with _setup(graph, [_find_npc_thief(crime)], with_crimes=True) as (_, chains, casting, llm):
        deed = _record_crime(chains, fact="npc_crime", id=crime, culprit="BAU_952_MALAK")
        chains.chronicle.record({"fact": "npc_crime_seen", "npc": LOBART, "crime": crime})
        chains.chronicle.record({"fact": "blamed", "id": "BLAME_1", "npc": HILDA, "crime": crime,
                                 "culprit": LOBART, "truth": False, "because": "hero_words"})
        made = await chains.chain(HILDA, crime=crime, quest_count=1, max_people=0)
        assert made is not None and len(made) == 1 and not casting.calls
        goal = made[0].stages[0].goals[0]
        assert (goal.kind, goal.crime, goal.npc) == ("blame", deed["id"], "")
        context = llm.calls[0][1][0].content
        assert "убедить заказчика, кто вор" in context
        assert "даже если герой подставил невиновного" in context
        assert f"{LOBART} — Лобарт; видел" in context
        assert f"{HILDA} — Хильда; винит: {LOBART}; со слов/основание: герой" in context
        assert "truth" not in context and "False" not in context


async def test_npc_crime_actual_item_can_be_returned_without_creating_another_copy(graph):  # noqa: F811
    async with _setup(graph, [_return_loot()], with_crimes=True) as (fake, chains, casting, _):
        deed = _record_crime(chains, fact="npc_crime", id="npc-loot", culprit="BAU_952_MALAK")
        fake.symbols.update({STOLEN: "item", "PC_HERO": "npc"})
        fake.hero_items[STOLEN] = 1
        made = await chains.chain(HILDA, crime=deed["id"], quest_count=1, max_people=0)
        assert made is not None and not casting.calls
        assert made[0].stages[-1].goals[0].item == STOLEN
        assert "WLD_INSERTITEM" not in str(fake.graph[-1])


@pytest.mark.parametrize("linked", ["hero", "other_npc", "disabled_npc"])
async def test_blame_goal_cannot_target_an_unlinked_or_disabled_npc_deed(graph, linked):  # noqa: F811
    async with _setup(graph, with_crimes=True) as (_, chains, casting, _):
        deed = _record_crime(chains)
        _record_crime(chains, fact="npc_crime", id="npc-other", culprit="BAU_952_MALAK")
        if linked != "hero":
            deed = _record_crime(chains, fact="npc_crime", id="npc-linked", culprit="BAU_952_MALAK")
        if linked == "disabled_npc":
            chains.npc_crimes = False
            assert await chains.brief(HILDA, crime=deed["id"]) is None
            return
        brief = await chains.brief(HILDA, crime=deed["id"])
        assert brief is not None
        brief.quest_count = 1
        made, errors = await chains._try(_find_npc_thief("npc-other"), brief)
        assert made is None and "blame: нужен id связанного дела NPC" in errors
        assert not casting.calls and not chains.book.all()


async def test_refuting_the_latest_accusation_does_not_restore_an_older_builder_belief(graph):  # noqa: F811
    async with _setup(graph, with_crimes=True) as (_, chains, _, _):
        deed = _record_crime(chains, fact="npc_crime", id="npc-loot", culprit="BAU_952_MALAK")
        for number, culprit in enumerate([LOBART, "BAU_961_VINO"], 1):
            chains.chronicle.record({"fact": "blamed", "id": f"BLAME_{number}", "npc": HILDA,
                                     "crime": deed["id"], "culprit": culprit, "truth": False})
        chains.chronicle.record({"fact": "blame_refuted", "blamed": "BLAME_2", "how": "no_goods"})
        text = chains._crime_context(deed, {})
        assert "винит:" not in text
        assert "пострадавший; может заметить пропажу" in text
