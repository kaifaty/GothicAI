import json
from types import SimpleNamespace

from gothic_ai.llm.fake import FakeBackend, FakeStep
from gothic_ai.lore import Lore, Person, candidates, check, dialogs, extract, file, people_of
from gothic_ai.places import PlaceBook
from gothic_ai.protocol import Look, NpcView, PlayerState
from gothic_ai.whereabouts import Whereabouts
from tests.test_knowledge import LOBART, _view, graph  # noqa: F401  (graph: the fixture)

HILDA, ANDRE, VINO, BANDIT = "BAU_951_HILDA", "MIL_311_ANDRE", "BAU_952_VINO", "BDT_1013_BANDIT_L"


def _place(id: str, ru: str, center: list[int], kind: str = "other") -> dict:
    return {"id": id, "ru": ru, "where_ru": f"у места «{ru}»", "en": id, "where_en": id, "center": center,
            "kind": kind}  # fmt: skip


BOOK = PlaceBook(
    {
        "version": 2,
        "world": "NEWWORLD.ZEN",
        "places": [
            _place("FARM1_OUT", "двор фермы Лобарта", [0, 0, 0], "yard"),
            _place("FARM1_FIELD", "поле репы за амбаром", [0, 0, 2000], "field"),
            _place("FARM1_INHOUSE", "дом Лобарта", [0, 0, 500], "house"),
            _place("CITY_MARKET", "рыночная площадь Хориниса", [90000, 0, 0], "street"),
            *[_place(f"ROAD{i}", f"дорога номер {i}", [200000 + i, 0, 0], "road") for i in range(6)],
        ],
        "points": {
            "NW_FARM1_OUT_01": 0,
            "NW_FARM1_FIELD_01": 1,
            "NW_FARM1_INHOUSE_01": 2,
            "NW_CITY_MARKET_01": 3,
            **{f"NW_ROAD{i}": 4 + i for i in range(6)},
        },
    }  # fmt: skip
)
PEOPLE = {
    LOBART: Person(LOBART, "Лобарт", ["FARM1_OUT"]),
    HILDA: Person(HILDA, "Хильда", ["FARM1_INHOUSE"]),
    ANDRE: Person(ANDRE, "Андре", ["CITY_MARKET"]),
    VINO: Person(VINO, "Вино", ["FARM1_FIELD"]),
    BANDIT: Person(BANDIT, "Бандит", ["ROAD0"]),
}


def _fact(dialog: str, place: str | None, who: list[str], ru: str, en: str = "") -> dict:
    return {"dialog": dialog, "place": place, "who": who, "ru": ru, "en": en}


FACTS = {
    "version": 1,
    "world": "NEWWORLD.ZEN",
    "npcs": {
        LOBART: [
            _fact(
                "DIA_LOBART_WORKNOW", "FARM1_FIELD", [], "Репа растёт на поле за амбаром.", "Turnips grow."
            ),
            _fact("DIA_LOBART_RUEBENRUNNING", "FARM1_INHOUSE", [HILDA], "Репу ждёт Хильда в доме."),
            _fact("DIA_LOBART_ANDRE", None, [ANDRE], "Андре платит за хищников."),
            _fact("DIA_LOBART_KAP3", "CITY_MARKET", [], "Орки у ворот города."),
            _fact("DIA_LOBART_HALLO", None, [VINO], "Вино работает на поле."),
        ]
    },
    "names": {HILDA: "Хильда", ANDRE: "Андре", VINO: "Вино"},
}


def test_the_dialogs_the_model_reads_with_the_answers_in_them(graph):  # noqa: F811
    talks = dict(dialogs(graph, LOBART))
    assert talks["DIA_LOBART_WORKNOW"] == [
        "Герой: Я ищу работу.",
        "Ты: Могу предложить поденную работу: собрать репу за амбаром.",
        "[ответ героя: Хорошо...]",
        "Герой: Хорошо...",
        "Ты: Двадцать штук, не меньше.",
    ]
    assert "DIA_LOBART_EXIT" not in talks  # it says nothing
    assert "DIA_LOBART_WORKNOW_OK" not in talks  # the answer is in its dialog


def test_the_places_and_people_the_model_may_tie_facts_to():
    dump = [{"npc": LOBART, "name": "Лобарт", "routine": [
        {"start": 420, "end": 1380, "state": "ZS_STAND_GUARDING", "waypoint": "NW_FARM1_OUT_01"},
        {"start": 1380, "end": 420, "state": "ZS_SLEEP", "waypoint": "NW_FARM1_INHOUSE_01"},
    ]}]  # fmt: skip
    assert people_of(dump, BOOK)[LOBART].homes == ["FARM1_OUT", "FARM1_INHOUSE"]  # the most hours first

    text = "Сходи на рыночную площадь к Андре. Бандит на дороге. Хильда больна."
    places, folk = candidates(LOBART, text, BOOK, PEOPLE)
    ids = [p.id for p in places]
    assert ids[:3] == ["FARM1_OUT", "FARM1_INHOUSE", "FARM1_FIELD"]  # around its home, nearest first
    assert "CITY_MARKET" in ids  # named: «рыночную площадь»
    assert not any(i.startswith("ROAD") for i in ids)  # «дорога» names six places: none
    who = [o.npc for o in folk]
    assert ANDRE in who and HILDA in who and VINO in who  # named; living by its home
    assert BANDIT not in who  # «Бандит» is everyone's name: nobody


def test_the_facts_that_hold_are_kept():
    facts = [
        {"dialog": "DIA_A", "place": "FARM1_FIELD", "who": [], "ru": "Поле за амбаром.", "en": "a field"},
        {"dialog": "DIA_OTHER", "place": "FARM1_FIELD", "who": [], "ru": "Чужой диалог."},
        {"dialog": "DIA_A", "place": "NOWHERE", "who": ["mil_311_andre"], "ru": "Андре в городе."},
        {"dialog": "DIA_A", "place": "NOWHERE", "who": [], "ru": "Ни места, ни людей."},
        {"dialog": "DIA_A", "place": "FARM1_FIELD", "who": [], "ru": " "},
        "не факт",
    ]
    kept = check(facts, {"DIA_A"}, {"FARM1_FIELD"}, {ANDRE})
    assert kept == [
        {"dialog": "DIA_A", "place": "FARM1_FIELD", "who": [], "ru": "Поле за амбаром.", "en": "a field"},
        {"dialog": "DIA_A", "place": None, "who": [ANDRE], "ru": "Андре в городе.", "en": ""},
    ]


async def test_the_model_writes_an_npcs_facts_and_a_broken_answer_is_asked_again(graph):  # noqa: F811
    answer = [{"dialog": "DIA_LOBART_WORKNOW", "place": "FARM1_FIELD", "who": [], "ru": "Репа за амбаром.",
               "en": "Turnips behind the barn."}]  # fmt: skip
    llm = FakeBackend(script=[FakeStep("не JSON"), FakeStep(json.dumps(answer, ensure_ascii=False))], delay=0)
    facts = await extract(LOBART, graph, BOOK, PEOPLE, llm)
    assert facts == [{**answer[0], "who": []}]
    asked = llm.calls[0][1][0].content
    assert "[DIA_LOBART_WORKNOW]" in asked and "- FARM1_FIELD — поле репы за амбаром (поле)" in asked
    assert f"- {HILDA} — Хильда — у места «дом Лобарта» (FARM1_INHOUSE)" in asked
    data = file("newworld.zen", {LOBART: facts}, PEOPLE)
    assert data["world"] == "NEWWORLD.ZEN" and data["names"] == {}  # no one in its facts


async def _lore(graph, *, sees: list[Look] = ()) -> Lore:  # noqa: F811
    knowledge, _ = await _view(graph)  # chapter 1, the hero has heard nothing yet
    hero = PlayerState(name="Я", pos=(0, 0, 0), ahead=(1, 0), right=(0, -1), distance=0)
    snap = SimpleNamespace(npc=SimpleNamespace(pos=(0.0, 0.0, 0.0)))
    link = SimpleNamespace(snapshots={LOBART: snap}, player=hero)
    view = NpcView(npc=LOBART, name="Лобарт", sees=list(sees))
    desk = SimpleNamespace(view=lambda npc: view)
    return Lore(FACTS, BOOK, knowledge, link=link, desk=desk)  # type: ignore[arg-type]


async def test_an_npc_knows_the_facts_of_dialogs_the_story_lets_it_tell(graph):  # noqa: F811
    lore = await _lore(graph)
    assert [f.dialog for f in lore.known(LOBART)] == ["DIA_LOBART_WORKNOW", "DIA_LOBART_HALLO"]
    # not RUEBENRUNNING: it waits for the hero to bring the turnips; not ANDRE: another npc's dialog the hero
    # has not heard; not KAP3: chapter 3
    assert lore.known("DIR_NPC_GUARD_01") == [] and lore.known(HILDA) == []  # not in the book: nothing


async def test_the_situation_tells_the_facts_of_the_places_around_and_the_people_in_sight(graph):  # noqa: F811
    assert (
        (await _lore(graph)).situation(LOBART)
        == "Что ты знаешь о местах и людях вокруг:\n- Репа растёт на поле за амбаром."
    )
    vino = Look(id=VINO, name="Вино", distance=900)
    shown = (await _lore(graph, sees=[vino])).situation(LOBART)
    assert shown is not None and shown.splitlines()[1] == "- Вино работает на поле."  # in sight: first
    assert (
        (await _lore(graph)).recall(LOBART, "где растёт репа?")
        == "Ты знаешь о местах и людях:\n- Репа растёт на поле за амбаром."
    )


async def test_where_tells_what_it_knows_of_a_place_and_of_someone_not_seen(graph):  # noqa: F811
    lore = await _lore(graph)
    hero = PlayerState(name="Я", pos=(0, 0, 0), ahead=(1, 0), right=(0, -1), distance=0)
    link = SimpleNamespace(snapshots=lore.link.snapshots, player=hero)  # type: ignore[union-attr]
    where = Whereabouts(link, BOOK, lore=lore)  # type: ignore[arg-type]
    field = where.where(LOBART, "FARM1_FIELD")
    assert field.splitlines()[-1] == "Ты знаешь: Репа растёт на поле за амбаром."
    vino = where.where(LOBART, "где Вино?")
    assert vino == "Сам ты его недавно не видел, но знаешь: Вино работает на поле."
