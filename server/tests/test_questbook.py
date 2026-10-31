from dataclasses import replace
from types import SimpleNamespace
from typing import Any

import pytest

from gothic_ai import questbook
from gothic_ai.chronicle import Chronicle
from gothic_ai.director import Brief, Director, Ending, Offer, Pacing, Proposal
from gothic_ai.game import GameError
from gothic_ai.journal import Journal
from gothic_ai.llm import ToolCall
from gothic_ai.llm.fake import FakeBackend
from gothic_ai.memory import MemoryStore
from gothic_ai.places import PlaceBook
from gothic_ai.questbook import Goal, Ground, Quest, QuestBook, Stage, Talk, build, problems
from gothic_ai.story import QUEST, StoryPatches
from gothic_ai.triggers import Triggers
from gothic_ai.validator import LOG_FAILED, LOG_OBSOLETE, LOG_RUNNING, LOG_SUCCESS, Patches, Story, WorldState
from tests.fake_game import FakeGame, connected
from tests.test_authoring import BOOK, graph  # noqa: F401 - the graph fixture
from tests.test_dialogue import _until
from tests.test_director import HILDA, LOBART, NAMES, SYMBOLS
from tests.test_director import QUEST as VANILLA_QUEST

THIEF = "DIR_NPC_2"
GOODS = "ITFO_WINE"
FREEPOINT = "FP_ROAM_NW_FARM1_FIELD_01"
FUNCS = (
    "NPC_HASITEMS",
    "NPC_GETDISTTOWP",
    "NPC_GETDISTTONPC",
    "NPC_ISDEAD",
    "HLP_GETNPC",
    "WLD_GETDAY",
    "WLD_INSERTITEM",
    "LOG_SETTOPICSTATUS",
    "B_LOGENTRY",
)


def _talk(text: str) -> Talk:
    return Talk(text, [{"speaker": "self", "text": f"{text} — ответ."}])


def _quest(**change: Any) -> Quest:
    """Hilda's wine is stolen: find the thief's hiding place, get the wine back, bring it to her."""
    q = Quest(
        id="DIR_Q1",
        giver=HILDA,
        title="Украденное вино",
        give=_talk("Что случилось?"),
        stages=[
            Stage("У Хильды украли вино. Вор прячется на полях.", [Goal("reach", point="NW_FARM1_FIELD_01")]),
            Stage(
                "Я нашёл, где прячется вор. Вино у него.",
                [Goal("have", npc=THIEF, item=GOODS, count=2, journal="Вино у меня.")],
            ),
            Stage(
                "Надо отнести вино Хильде.",
                [Goal("bring", npc=HILDA, item=GOODS, count=2, talk=_talk("Вот вино."))],
            ),
        ],
        done_journal="Я вернул Хильде вино.",
        asked="найти вора и вернуть вино.",
        gold=20,
        xp=40,
        names={HILDA: "Хильда"},
    )
    return replace(q, **change)


def _ground(**change: Any) -> Ground:
    symbols = {**SYMBOLS, THIEF: "npc", GOODS: "item", "ITMI_SAW": "item"}
    states = {HILDA: "alive", LOBART: "alive", THIEF: "alive"}
    points = PlaceBook(BOOK)
    return replace(
        Ground(
            symbols,
            states,
            plot=lambda n: n == "NONE_100_XARDAS",
            point=lambda p: points.of(p) is not None,
            gold=50,
            xp=50,
        ),
        **change,
    )


def _vanilla_story(graph, *, patches: Patches | None = None, **state: Any) -> Story:  # noqa: F811
    world = WorldState(
        vars={VANILLA_QUEST: 0, "KAPITEL": 1},
        npcs={n: "alive" for n in NAMES},
        symbols=dict(SYMBOLS),
    )
    return Story(graph, replace(world, **state), patches)


def _vanilla_goal(state: str = "running", quest: str = VANILLA_QUEST) -> Quest:
    return _quest(
        stages=[
            Stage("Дождаться работы на ферме.", [Goal("vanilla", quest=quest, state=state)]),
            Stage("Рассказать Хильде.", [Goal("talk", npc=HILDA, talk=_talk("Работа началась."))]),
        ]
    )


def test_a_quest_of_three_stages_is_built_of_the_graphs_primitives():
    ops = build(_quest(days=3, late_journal="Не успел."))
    kinds = [(op["op"], op.get("id") or op.get("node", {}).get("id")) for op in ops]
    assert kinds == [
        ("vars", None),
        ("add_node", "DIR_Q1_GIVE"),
        ("trigger", "DIR_Q1_S1G1"),  # come to the field
        ("trigger", "DIR_Q1_S1"),  # the next stage
        ("trigger", "DIR_Q1_S2G1"),  # the wine
        ("trigger", "DIR_Q1_S2"),
        ("add_node", "DIR_Q1_DONE"),  # the turn-in
        ("trigger", "DIR_Q1_LATE"),
    ]
    assert ops[0]["vars"] == {"DIR_Q1": 0, "DIR_Q1_STAGE": 0, "DIR_Q1_DUE": 0}
    give = ops[1]["node"]
    assert give["effects"][1] == {"do": "set", "var": "DIR_Q1_STAGE", "value": 1}
    assert give["program"] == [  # the deadline, from the day it is given
        {"do": "set", "target": ["var", "DIR_Q1_DUE"], "value": ["+", ["call", "WLD_GETDAY"], ["int", 3]]}
    ]
    running = ["==", ["var", "DIR_Q1"], ["int", 1, "LOG_RUNNING"]]
    reach = ops[2]
    assert reach["on"] == "state" and reach["steps"] == []
    assert reach["when"] == [
        "&&",
        running,
        ["==", ["var", "DIR_Q1_STAGE"], ["int", 1]],
        ["<", ["call", "NPC_GETDISTTOWP", ["inst", "HERO"], ["str", "NW_FARM1_FIELD_01"]], ["int", 500]],
    ]
    # the stage's own trigger begins the next: its stage, its journal entry
    assert ops[3]["when"] == [
        "&&",
        running,
        ["==", ["var", "DIR_Q1_STAGE"], ["int", 1]],
        [">=", ["var", "DIR_Q1_S1G1"], ["int", 1]],
    ]
    assert ops[3]["steps"] == [
        {"do": "set", "target": ["var", "DIR_Q1_STAGE"], "value": ["int", 2]},
        {
            "do": "call",
            "call": [
                "fn",
                "B_LOGENTRY",
                ["str", "Украденное вино"],
                ["str", "Я нашёл, где прячется вор. Вино у него."],
            ],
        },
    ]
    assert ops[4]["steps"][0]["call"][2:] == [["str", "Украденное вино"], ["str", "Вино у меня."]]
    done = ops[6]["node"]
    assert done["npc"] == HILDA and done["condition"] == [
        "&&",
        running,
        ["==", ["var", "DIR_Q1_STAGE"], ["int", 3]],
        [">=", ["call", "NPC_HASITEMS", ["inst", "HERO"], ["sym", GOODS]], ["int", 2]],
    ]
    assert done["effects"] == [
        {"do": "give", "item": GOODS, "count": 2, "to": "self"},
        {"do": "give", "item": "ITMI_GOLD", "count": 20},
        {"do": "xp", "amount": 40},
        {"do": "set", "var": "DIR_Q1", "value": 2},
        {"do": "set", "var": "DIR_Q1_STAGE", "value": 4},
        {
            "do": "journal",
            "topic": "Украденное вино",
            "text": "Я вернул Хильде вино.",
            "status": "LOG_SUCCESS",
        },
    ]
    late = ops[7]
    assert late["when"][-1] == [">", ["call", "WLD_GETDAY"], ["var", "DIR_Q1_DUE"]]
    assert late["steps"][0] == {"do": "set", "target": ["var", "DIR_Q1"], "value": ["int", 3, "LOG_FAILED"]}


def test_goals_of_every_kind():
    talk = Goal("talk", npc=LOBART, talk=_talk("Ты видел вора?"), journal="Лобарт видел вора.")
    kill = Goal("kill", npcs=[THIEF, "DIR_NPC_3"])
    wait = Goal("wait", days=2)
    saw = Goal("have", item="ITMI_SAW", point="NW_FARM1_OUT_02", count=2)
    q = _quest(
        stages=[
            Stage("Расспросить Лобарта.", [talk, saw]),
            Stage("Разобраться с ворами и выждать.", [kill, wait]),
            Stage("К Хильде.", [Goal("talk", npc=HILDA, talk=_talk("Дело сделано."))]),
        ],
        after="DIR_Q0",
    )
    ops = build(q)
    assert ops[0]["vars"] == {"DIR_Q1": 0, "DIR_Q1_STAGE": 0, "DIR_Q1_DAY": 0, "DIR_Q1_S1G1": 0}
    give = ops[1]["node"]
    assert give["condition"][2] == ["==", ["var", "DIR_Q0"], ["int", 2, "LOG_SUCCESS"]]  # opened by the other
    # the saw lies at its point from the start: twice
    assert give["program"] == 2 * [
        {"do": "call", "call": ["call", "WLD_INSERTITEM", ["sym", "ITMI_SAW"], ["str", "NW_FARM1_OUT_02"]]}
    ]
    ask = ops[2]["node"]  # a talk that is a goal: its dialog sets its variable once
    assert ask["id"] == "DIR_Q1_S1G1_D" and ask["npc"] == LOBART
    assert ["==", ["var", "DIR_Q1_S1G1"], ["int", 0]] in ask["condition"]
    assert {"do": "set", "var": "DIR_Q1_S1G1", "value": 1} in ask["effects"]
    by_id = {op.get("id"): op for op in ops if op["op"] == "trigger"}
    assert by_id["DIR_Q1_S1"]["steps"][-1] == {
        "do": "set",
        "target": ["var", "DIR_Q1_DAY"],
        "value": ["call", "WLD_GETDAY"],
    }  # the stage of waiting notes its day
    assert by_id["DIR_Q1_S2G1"]["when"][-2:] == [
        ["call", "NPC_ISDEAD", ["call", "HLP_GETNPC", ["sym", THIEF]]],
        ["call", "NPC_ISDEAD", ["call", "HLP_GETNPC", ["sym", "DIR_NPC_3"]]],
    ]
    assert by_id["DIR_Q1_S2G2"]["when"][-1] == [
        ">=",
        ["call", "WLD_GETDAY"],
        ["+", ["var", "DIR_Q1_DAY"], ["int", 2]],
    ]
    # the turn-in is a talk: no thing taken
    done = next(op["node"] for op in ops if op["op"] == "add_node" and op["node"]["id"] == "DIR_Q1_DONE")
    assert done["effects"][0] == {"do": "give", "item": "ITMI_GOLD", "count": 20}
    # and it describes itself back
    assert Quest.of(q.to_dict()) == q


@pytest.mark.parametrize(
    ("requirement", "expression"),
    [
        (
            {"kind": "quest", "quest": VANILLA_QUEST, "state": "success"},
            ["==", ["var", VANILLA_QUEST], ["int", LOG_SUCCESS, "LOG_SUCCESS"]],
        ),
        (
            {"kind": "knows", "node": "DIA_LOBART_WORKNOW"},
            ["call", "NPC_KNOWSINFO", ["inst", "HERO"], ["sym", "DIA_LOBART_WORKNOW"]],
        ),
        ({"kind": "chapter", "chapter": 2}, [">=", ["var", "KAPITEL"], ["int", 2]]),
    ],
)
def test_each_vanilla_requirement_is_a_condition_of_the_giving_dialog(requirement, expression):
    q = _quest(requires=[requirement])
    give = build(q)[1]["node"]
    assert give["condition"] == ["&&", ["==", ["var", q.id], ["int", 0]], expression]
    assert Quest.of(q.to_dict()) == q


def test_all_vanilla_requirements_and_the_previous_quest_must_hold_together():
    q = _quest(
        after="DIR_Q0",
        requires=[
            {"kind": "quest", "quest": VANILLA_QUEST, "state": "running"},
            {"kind": "knows", "node": "DIA_LOBART_WORKNOW"},
            {"kind": "chapter", "chapter": 2},
        ],
    )
    assert build(q)[1]["node"]["condition"] == [
        "&&",
        ["==", ["var", q.id], ["int", 0]],
        ["==", ["var", "DIR_Q0"], ["int", LOG_SUCCESS, "LOG_SUCCESS"]],
        ["==", ["var", VANILLA_QUEST], ["int", LOG_RUNNING, "LOG_RUNNING"]],
        ["call", "NPC_KNOWSINFO", ["inst", "HERO"], ["sym", "DIA_LOBART_WORKNOW"]],
        [">=", ["var", "KAPITEL"], ["int", 2]],
    ]


@pytest.mark.parametrize(
    ("state", "value"), [("running", LOG_RUNNING), ("success", LOG_SUCCESS), ("failed", LOG_FAILED)]
)
def test_a_vanilla_goal_waits_for_the_scripts_quest_state(state, value):
    q = _vanilla_goal(state)
    trigger = next(op for op in build(q) if op.get("id") == "DIR_Q1_S1G1")
    assert trigger["on"] == "state" and trigger["steps"] == []
    assert trigger["when"] == [
        "&&",
        ["==", ["var", q.id], ["int", LOG_RUNNING, "LOG_RUNNING"]],
        ["==", ["var", "DIR_Q1_STAGE"], ["int", 1]],
        ["==", ["var", VANILLA_QUEST], ["int", value, f"LOG_{state.upper()}"]],
    ]
    assert Quest.of(q.to_dict()) == q


@pytest.mark.parametrize(
    "requirement",
    [
        {"kind": "quest", "quest": VANILLA_QUEST, "state": "running"},
        {"kind": "quest", "quest": VANILLA_QUEST, "state": "success"},
        {"kind": "knows", "node": "DIA_LOBART_WORKNOW"},
        {"kind": "chapter", "chapter": 1},
        {"kind": "chapter", "chapter": 6},
    ],
)
def test_possible_vanilla_requirements_are_allowed(graph, requirement):  # noqa: F811
    assert problems(_quest(requires=[requirement]), _ground(story=_vanilla_story(graph))) == []


@pytest.mark.parametrize(
    ("requirement", "why"),
    [
        ({"kind": "quest", "quest": "MIS_OTHER", "state": "running"}, "не переменная квеста"),
        ({"kind": "quest", "quest": "KAPITEL", "state": "running"}, "не переменная квеста"),
        ({"kind": "quest", "quest": VANILLA_QUEST, "state": "obsolete"}, "состояние квеста"),
        ({"kind": "quest", "quest": VANILLA_QUEST, "state": "unknown"}, "состояние квеста"),
        ({"kind": "knows", "node": "DIA_LOBART_WORKNOW_OK"}, "вариант ответа"),
        ({"kind": "knows", "node": "DIA_NOWHERE"}, "нет ванильного диалога"),
        ({"kind": "chapter", "chapter": 0}, "от 1 до 6"),
        ({"kind": "chapter", "chapter": 7}, "от 1 до 6"),
        ({"kind": "chapter", "chapter": True}, "от 1 до 6"),
        ({"kind": "chapter", "chapter": 2.5}, "от 1 до 6"),
        ({"kind": "chapter", "chapter": "2"}, "от 1 до 6"),
    ],
)
def test_bad_vanilla_requirements_explain_why_the_quest_is_refused(graph, requirement, why):  # noqa: F811
    errors = problems(_quest(requires=[requirement]), _ground(story=_vanilla_story(graph)))
    assert any(why in error for error in errors), errors


@pytest.mark.parametrize("kind", ["requirement", "goal"])
@pytest.mark.parametrize("status", [LOG_FAILED, LOG_OBSOLETE])
def test_a_closed_scripts_quest_cannot_be_required_to_succeed(graph, kind, status):  # noqa: F811
    q = (
        _quest(requires=[{"kind": "quest", "quest": VANILLA_QUEST, "state": "success"}])
        if kind == "requirement"
        else _vanilla_goal("success")
    )
    story = _vanilla_story(graph, vars={VANILLA_QUEST: status, "KAPITEL": 1})
    errors = problems(q, _ground(story=story))
    assert errors and any(VANILLA_QUEST in error for error in errors), errors


@pytest.mark.parametrize("kind", ["requirement", "goal"])
def test_a_scripts_quest_with_no_living_ending_cannot_be_required_to_succeed(graph, kind):  # noqa: F811
    q = (
        _quest(requires=[{"kind": "quest", "quest": VANILLA_QUEST, "state": "success"}])
        if kind == "requirement"
        else _vanilla_goal("success")
    )
    story = _vanilla_story(graph, npcs={**dict.fromkeys(NAMES, "alive"), LOBART: "dead"})
    errors = problems(q, _ground(story=story))
    assert errors and any(VANILLA_QUEST in error for error in errors), errors


@pytest.mark.parametrize("reason", ["dead", "disabled", "chapter"])
def test_a_requirement_cannot_wait_for_a_dialog_that_will_never_be_offered(graph, reason):  # noqa: F811
    state: dict[str, Any] = {}
    patches = Patches()
    if reason == "dead":
        state["npcs"] = {**dict.fromkeys(NAMES, "alive"), LOBART: "dead"}
    elif reason == "disabled":
        patches.disabled.add("DIA_LOBART_WORKNOW")
    else:
        state["vars"] = {VANILLA_QUEST: 0, "KAPITEL": 3}
    story = _vanilla_story(graph, patches=patches, **state)
    q = _quest(requires=[{"kind": "knows", "node": "DIA_LOBART_WORKNOW"}])
    errors = problems(q, _ground(story=story))
    assert errors and any("DIA_LOBART_WORKNOW" in error for error in errors), errors


def test_a_dialog_already_known_is_allowed_even_when_its_owner_is_dead(graph):  # noqa: F811
    story = _vanilla_story(
        graph,
        npcs={**dict.fromkeys(NAMES, "alive"), LOBART: "dead"},
        knows={"DIA_LOBART_WORKNOW": True},
    )
    q = _quest(requires=[{"kind": "knows", "node": "DIA_LOBART_WORKNOW"}])
    assert problems(q, _ground(story=story)) == []


@pytest.mark.parametrize(
    ("quest", "state"), [("MIS_OTHER", "running"), ("KAPITEL", "running"), (VANILLA_QUEST, "obsolete")]
)
def test_a_vanilla_goal_requires_a_graph_quest_and_a_supported_state(graph, quest, state):  # noqa: F811
    errors = problems(_vanilla_goal(state, quest), _ground(story=_vanilla_story(graph)))
    assert errors and any("стадия 1, цель 1" in error for error in errors), errors


@pytest.mark.parametrize(
    ("status", "state"),
    [(0, "running"), (0, "success"), (LOG_SUCCESS, "success"), (LOG_FAILED, "failed")],
)
def test_a_vanilla_goal_accepts_achievable_or_already_reached_states(graph, status, state):  # noqa: F811
    story = _vanilla_story(graph, vars={VANILLA_QUEST: status, "KAPITEL": 1})
    assert problems(_vanilla_goal(state), _ground(story=story)) == []


def test_the_code_goes_through_a_quest_before_it_is_made():
    assert problems(_quest(), _ground()) == []
    # the last stage ends in one talk
    no_turn_in = _quest(stages=[Stage("Найти вино.", [Goal("have", npc=THIEF, item=GOODS)])])
    assert problems(no_turn_in, _ground()) == [
        "стадия 1, последняя: в ней одна сдача — цель «принести» или «поговорить», а не 0"
    ]
    # the wine has to come from somewhere
    bring_only = _quest(stages=[Stage("Вино.", [Goal("bring", npc=HILDA, item=GOODS, talk=_talk("Вот."))])])
    assert problems(bring_only, _ground()) == [
        "стадия 1, цель 1: ITFO_WINE неоткуда взять: ни точки, где она лежит, ни цели «заполучить» раньше"
    ]
    # who is killed is not talked to after
    deadly = _quest(
        stages=[
            Stage("Убить вора.", [Goal("kill", npcs=[THIEF])]),
            Stage("Сдать вору.", [Goal("talk", npc=THIEF, talk=_talk("Ну?"))]),
        ]
    )
    assert problems(deadly, _ground()) == ["стадия 2, цель 1: DIR_NPC_2 к этому времени убит целью «убить»"]
    # people of the scripts are not killed, the plot's are not the director's
    vanilla = _quest(
        stages=[Stage("Убить.", [Goal("kill", npcs=[LOBART]), Goal("talk", npc=HILDA, talk=_talk("Всё."))])]
    )
    assert problems(vanilla, _ground()) == [
        "стадия 1, цель 1: убить можно только своих персонажей режиссёра (DIR_NPC_…), а не BAU_950_LOBART"
    ]
    assert problems(
        _quest(giver="NONE_100_XARDAS"), _ground(symbols={**SYMBOLS, THIEF: "npc", GOODS: "item"})
    ) == ["заказчик: NONE_100_XARDAS — сюжетный NPC, его не трогаем"]
    dead = _ground(states={HILDA: "dead", THIEF: "alive"})
    assert problems(_quest(), dead) == ["заказчик: BAU_951_HILDA мёртв"]
    # the measure of the reward, the deadline, the quest it opens after, the title
    assert problems(_quest(gold=60, days=40), _ground()) == [
        "золота 60, а можно от 0 до 50",
        "срок — от 1 до 30 дней, а не 40",
        "у квеста со сроком нужна запись журнала о провале",
    ]
    other = _quest(id="DIR_Q7")
    assert problems(_quest(after="DIR_Q5"), _ground()) == [
        "квест DIR_Q5 не найден: этот открывается после него"
    ]
    assert problems(_quest(), _ground(quests={"DIR_Q7": other})) == ["поручение «Украденное вино» уже было"]
    # a point the book of places does not know, a point and an npc at once
    far = _quest(
        stages=[Stage("Туда.", [Goal("reach", point="NW_NOWHERE", npc=THIEF)]), *_quest().stages[1:]]
    )
    assert problems(far, _ground()) == ["стадия 1, цель 1: дойти: или до точки, или до NPC"]
    nowhere = _quest(stages=[Stage("Туда.", [Goal("reach", point="NW_NOWHERE")]), *_quest().stages[1:]])
    assert problems(nowhere, _ground()) == ["стадия 1, цель 1: точки NW_NOWHERE нет в книге мест"]


@pytest.mark.parametrize("point", ["NW_FARM1_FIELD_01", FREEPOINT])
def test_reach_requires_a_waypoint_when_the_ground_knows_point_kinds(point):
    places = PlaceBook({**BOOK, "points": {**BOOK["points"], FREEPOINT: 2}})
    ground = _ground(point=lambda p: places.of(p) is not None, waypoint=places.is_waypoint)
    q = _quest(stages=[Stage("Дойти до поля.", [Goal("reach", point=point)]), *_quest().stages[1:]])
    expected = (
        [f"стадия 1, цель 1: точка {point} — не вейпоинт: цель «дойти» не работает с фрипоинтами"]
        if point == FREEPOINT
        else []
    )
    assert problems(q, ground) == expected


def test_ground_without_point_kinds_keeps_its_existing_reach_validation():
    q = _quest(stages=[Stage("Дойти до поля.", [Goal("reach", point=FREEPOINT)]), *_quest().stages[1:]])
    ground = _ground(point=lambda p: p == FREEPOINT)
    assert ground.waypoint is None
    assert problems(q, ground) == []


@pytest.mark.parametrize("kind", ["have", "bring"])
def test_items_may_be_inserted_at_freepoints(kind):
    goal = Goal(kind, item=GOODS, point=FREEPOINT)
    if kind == "bring":
        goal.npc, goal.talk = HILDA, _talk("Вот вино.")
    q = _quest(
        stages=[
            Stage("Забрать вино с поля.", [goal]),
            Stage("Рассказать Хильде.", [Goal("talk", npc=HILDA, talk=_talk("Всё сделано."))]),
        ]
    )
    assert problems(q, _ground(point=lambda p: p == FREEPOINT, waypoint=lambda p: False)) == []


async def _book(fake: FakeGame, store: MemoryStore, graph) -> tuple[Any, QuestBook, StoryPatches, Chronicle]:  # noqa: F811
    fake.vars = {"KAPITEL": 1, "XP_AMBIENT": 50}
    fake.npc_states = {n: {"state": "alive", "name": name} for n, name in NAMES.items()}
    fake.npc_states[THIEF] = {"state": "alive", "name": "Вор"}
    fake.symbols = {**SYMBOLS, THIEF: "npc", GOODS: "item", **dict.fromkeys(FUNCS, "func")}
    link_cm = connected(fake)
    link = await link_cm.__aenter__()
    patches = StoryPatches(link, store)
    chronicle = Chronicle(link, store)
    director = Director(
        link,
        graph,
        patches,
        chronicle,
        FakeBackend(script=[], delay=0),
        pacing=Pacing(cooldown_s=0),
        settle_s=0,
    )
    book = QuestBook(link, graph, patches, chronicle, director, places=PlaceBook(BOOK))
    await _until(lambda: patches.branch is not None and chronicle.branch is not None)
    return link_cm, book, patches, chronicle


async def test_the_books_ground_knows_waypoints_and_freepoints(graph):  # noqa: F811
    fake = FakeGame(branch="A")
    link_cm, book, _, _ = await _book(fake, MemoryStore(":memory:"), graph)
    try:
        book.places = PlaceBook({**BOOK, "points": {**BOOK["points"], FREEPOINT: 2}})
        ground = await book.ground(_quest())
        assert ground.waypoint is not None
        assert ground.waypoint("NW_FARM1_FIELD_01")
        assert ground.waypoint("nw_farm1_field_01")
        assert ground.point(FREEPOINT) and not ground.waypoint(FREEPOINT)
        assert not ground.waypoint("NW_NOWHERE")
        assert not ground.waypoint("FARM1_FIELD")

        book.places = None
        unknown = await book.ground(_quest())
        assert unknown.waypoint is not None
        assert not unknown.waypoint("NW_FARM1_FIELD_01")
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_the_book_makes_a_quest_and_follows_it(graph):  # noqa: F811
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    link_cm, book, patches, chronicle = await _book(fake, store, graph)
    try:
        assert book.next_id() == "DIR_Q1"
        q = _quest()
        assert await book.make(q) == []
        # one patch: the changes, the record stays on the server
        sent = fake.graph[-1]
        assert not sent.get("replace")
        assert [op["op"] for op in sent["ops"]] == [
            "vars",
            "add_node",
            "trigger",
            "trigger",
            "trigger",
            "trigger",
            "add_node",
        ]
        assert [r["quest"]["id"] for r in patches.records(QUEST)] == ["DIR_Q1"]
        assert all(op["op"] != QUEST for op in patches.ops()) and len(patches.ops()) == 7
        assert book.all() == {"DIR_Q1": q} and book.next_id() == "DIR_Q2"
        assert book.progress("DIR_Q1") == questbook.Progress("open", 0)

        # given, the stages through their triggers, done
        await fake.send(type="dialog_chosen", npc=HILDA, node="DIR_Q1_GIVE", made=True)
        await _until(lambda: book.progress("DIR_Q1").state == "given")
        assert "Ты дал герою поручение «Украденное вино» (id DIR_Q1): найти вора и вернуть вино." in (
            book.situation(HILDA) or ""
        )
        await fake.send(type="trigger_fired", id="DIR_Q1_S1G1", count=1, npc="PC_HERO", other="PC_HERO")
        await fake.send(type="trigger_fired", id="DIR_Q1_S1", count=1, npc="PC_HERO", other="PC_HERO")
        await _until(lambda: book.progress("DIR_Q1").stage == 2)
        await fake.send(type="dialog_chosen", npc=HILDA, node="DIR_Q1_DONE", made=True)
        await _until(lambda: book.progress("DIR_Q1").state == "done")
        assert (
            "Поручение «Украденное вино» герой выполнил, ты заплатил: 20 золотых, опыт +40."
            == book.situation(HILDA)
        )
        facts = [f["fact"] for f in chronicle.facts() if f["fact"].startswith("story_quest")]
        assert facts == ["story_quest", "story_quest_given", "story_quest_stage", "story_quest_done"]
        assert chronicle.facts("quest_outcome") == [{
            "fact": "quest_outcome", "quest": q.id, "npc": HILDA, "how": "done",
            "drop": [], "why": [], "pay": "full",
        }]

        # the same title again is refused
        assert await book.make(_quest(id="DIR_Q2")) == ["поручение «Украденное вино» уже было"]
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_vanilla_requirements_and_goals_survive_the_branch_log_and_load(graph):  # noqa: F811
    fake = FakeGame(branch="A")
    link_cm, book, patches, chronicle = await _book(fake, MemoryStore(":memory:"), graph)
    try:
        fake.vars[VANILLA_QUEST] = 0
        fake.symbols["DIA_LOBART_WORKNOW"] = "info"
        q = replace(
            _vanilla_goal("success"),
            requires=[
                {"kind": "quest", "quest": VANILLA_QUEST, "state": "running"},
                {"kind": "knows", "node": "DIA_LOBART_WORKNOW"},
                {"kind": "chapter", "chapter": 2},
            ],
        )
        assert await book.make(q) == []
        assert patches.records(QUEST) == [{"op": QUEST, "quest": q.to_dict()}]
        assert book.progress(q.id) == questbook.Progress("open", 0)
        original_ops = patches.ops()
        saved_seq = patches.memory.head("A")  # the quest's chronicle fact belongs to the save too
        await _until(lambda: fake.heads[-1:] == [("A", saved_seq)])
        await fake.send(type="session", branch="B", parent="A", parent_seq=saved_seq, reason="load")
        await _until(lambda: patches.branch == "B" and chronicle.branch == "B")
        assert book.all() == {q.id: q}
        assert patches.records(QUEST) == [{"op": QUEST, "quest": q.to_dict()}]
        assert patches.ops() == original_ops
        assert book.progress(q.id) == questbook.Progress("open", 0)
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_a_book_quest_cannot_write_a_scripts_variable(graph, monkeypatch):  # noqa: F811
    fake = FakeGame(branch="A")
    link_cm, book, patches, _ = await _book(fake, MemoryStore(":memory:"), graph)
    try:
        original_build = questbook.build

        def writes_scripts_variable(q):
            ops = original_build(q)
            ops[1]["node"]["effects"].append({"do": "set", "var": VANILLA_QUEST, "value": LOG_SUCCESS})
            return ops

        monkeypatch.setattr(questbook, "build", writes_scripts_variable)
        await _until(lambda: bool(fake.graph))
        before = len(fake.graph)
        errors = await book.make(_quest())
        assert any("квест книги не меняет переменную скриптов" in error for error in errors), errors
        assert book.all() == {} and patches.records(QUEST) == [] and patches.ops() == []
        assert len(fake.graph) == before
    finally:
        await link_cm.__aexit__(None, None, None)


def _chain() -> list[Quest]:
    return [
        _quest(),
        _quest(
            id="DIR_Q2",
            title="Лагерь",
            after="DIR_Q1",
            stages=[
                Stage("Рассказать Хильде о лагере.", [Goal("talk", npc=HILDA, talk=_talk("Лагерь пуст."))])
            ],
            done_journal="Я рассказал Хильде о лагере.",
        ),
    ]


async def test_a_chain_is_checked_with_its_predecessors_and_sent_in_one_patch(graph, monkeypatch):  # noqa: F811
    fake = FakeGame(branch="A")
    link_cm, book, patches, chronicle = await _book(fake, MemoryStore(":memory:"), graph)
    try:
        await _until(lambda: bool(fake.graph))
        before = len(fake.graph)
        checked = []
        original_problems = questbook.problems

        def check(q, ground):
            checked.append((q.id, set(ground.quests)))
            return original_problems(q, ground)

        monkeypatch.setattr(questbook, "problems", check)
        quests = _chain()
        assert await book.make_chain(quests) == (quests, [])
        assert checked == [("DIR_Q1", set()), ("DIR_Q2", {"DIR_Q1"})]
        assert len(fake.graph) == before + 1
        assert fake.graph[-1]["ops"] == [op for q in quests for op in build(q)]
        assert book.all() == {q.id: q for q in quests}
        assert [record["quest"]["id"] for record in patches.records(QUEST)] == ["DIR_Q1", "DIR_Q2"]
        assert [fact["id"] for fact in chronicle.facts("story_quest")] == ["DIR_Q1", "DIR_Q2"]
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize(
    ("change", "error"),
    [
        ({"gold": 51}, "золота 51"),
        ({"after": "DIR_Q9"}, "квест DIR_Q9 не найден"),
        ({"title": "Украденное вино"}, "поручение «Украденное вино» уже было"),
        ({"id": "DIR_Q1"}, "квест DIR_Q1 уже есть"),
    ],
)
async def test_a_bad_later_quest_prevents_the_entire_chain(graph, change, error):  # noqa: F811
    fake = FakeGame(branch="A")
    link_cm, book, patches, chronicle = await _book(fake, MemoryStore(":memory:"), graph)
    try:
        await _until(lambda: bool(fake.graph))
        before = len(fake.graph)
        quests = _chain()
        quests[1] = replace(quests[1], **change)
        made, errors = await book.make_chain(quests)
        assert made is None and any(error in e for e in errors)
        assert len(fake.graph) == before
        assert book.all() == {} and patches.records(QUEST) == [] and patches.ops() == []
        assert chronicle.facts("story_quest") == []
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_a_refused_chain_keeps_only_the_previous_quests(graph, monkeypatch):  # noqa: F811
    fake = FakeGame(branch="A")
    link_cm, book, patches, chronicle = await _book(fake, MemoryStore(":memory:"), graph)
    try:
        old = _quest(id="DIR_Q0", title="Прежнее поручение")
        assert await book.make(old) == []
        before = patches.ops()
        original_patch = book.link.patch_graph

        async def refuse_late(ops, **kwargs):
            # The bridge has applied earlier operations when it rejects a later one.
            if not kwargs.get("replace"):
                ops = [*ops, {"op": "disable", "node": "BAD"}]
            await original_patch(ops, **kwargs)

        monkeypatch.setattr(book.link, "patch_graph", refuse_late)
        made, errors = await book.make_chain(_chain())
        assert made is None and any("no dialog BAD" in error for error in errors)
        assert book.all() == {old.id: old}
        assert [record["quest"]["id"] for record in patches.records(QUEST)] == [old.id]
        assert patches.ops() == before
        assert fake.graph[-1]["replace"] is True and fake.graph[-1]["ops"] == before
        assert [fact["id"] for fact in chronicle.facts("story_quest")] == [old.id]
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize("previous", [0, LOG_RUNNING, LOG_SUCCESS, LOG_FAILED, LOG_OBSOLETE])
def test_the_next_quest_opens_only_when_the_previous_one_succeeded(graph, previous):  # noqa: F811
    first, second = _chain()
    give = next(op["node"] for op in build(second) if op["op"] == "add_node")
    # Evaluate the condition at the current state, without future dialog writers.
    state = WorldState(vars={first.id: previous, second.id: 0})
    assert Story(graph, state).possible(give["condition"]) is (previous == LOG_SUCCESS)


async def test_the_save_has_the_truth_the_book_catches_up(graph):  # noqa: F811
    fake = FakeGame(branch="A")
    link_cm, book, _, chronicle = await _book(fake, MemoryStore(":memory:"), graph)
    try:
        assert await book.make(_quest()) == []
        # the messages were lost (the server was away): the game's variables say where it stands
        fake.vars.update({"DIR_Q1": 1, "DIR_Q1_STAGE": 3})
        await book.prepare(HILDA)
        assert book.progress("DIR_Q1") == questbook.Progress("given", 3)
        fake.vars["DIR_Q1"] = 3
        await book.settle()
        assert book.progress("DIR_Q1").state == "failed"
        assert book.situation(HILDA) == "Поручение «Украденное вино» герой не выполнил в срок."
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_a_refused_change_is_taken_back(graph):  # noqa: F811
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    link_cm, _, patches, _ = await _book(fake, store, graph)
    try:
        await patches.apply({"op": "disable", "node": "DIA_X"})
        with pytest.raises(GameError):
            await patches.make(
                {"op": QUEST, "quest": {}},
                [{"op": "vars", "vars": {"DIR_Q9": 0}}, {"op": "disable", "node": "BAD"}],
            )
        # nothing of it is remembered, and the game gets the branch's changes as they were
        await _until(lambda: fake.graph[-1].get("replace") is True)
        assert fake.graph[-1]["ops"] == [{"op": "disable", "node": "DIA_X"}]
        assert patches.records(QUEST) == [] and patches.ops() == [{"op": "disable", "node": "DIA_X"}]
    finally:
        await link_cm.__aexit__(None, None, None)


def _middle_dialog() -> Quest:
    return _quest(
        stages=[
            Stage("Найти вино.", [Goal("have", item=GOODS, count=2, point="NW_FARM1_OUT_02")]),
            Stage(
                "Отдать Хильде.",
                [Goal("bring", npc=HILDA, item=GOODS, count=2, talk=_talk("Держи вино."))],
            ),
            Stage("Рассказать Лобарту.", [Goal("talk", npc=LOBART, talk=_talk("Вино вернул."))]),
        ]
    )


@pytest.mark.parametrize(
    ("quest", "stage", "completed", "expected"),
    [
        (_quest(), 2, {}, [(3, 1, "DIR_Q1_DONE")]),
        (_middle_dialog(), 2, {}, [(2, 1, "DIR_Q1_S2G1_D")]),
        (_middle_dialog(), 2, {"DIR_Q1_S2G1": 1}, []),
        (_middle_dialog(), 3, {}, []),
        (
            _quest(stages=[Stage("Убить вора.", [Goal("kill", npcs=[THIEF])]), *_middle_dialog().stages[2:]]),
            1,
            {},
            [],
        ),
    ],
)
async def test_dead_ends_are_unfinished_dialogs_in_stage_order(graph, quest, stage, completed, expected):  # noqa: F811
    fake = FakeGame(branch="A")
    link_cm, book, _, _ = await _book(fake, MemoryStore(":memory:"), graph)
    try:
        assert await book.make(quest) == []
        fake.vars.update({quest.id: 1, "DIR_Q1_STAGE": stage, **completed})
        fake.npc_states[HILDA]["state"] = "dead"
        fake.npc_states[THIEF]["state"] = "dead"
        ends = book.dead_ends(await book.world())
        assert [(end.stage, end.goal, end.node) for end in ends] == expected
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_the_book_uses_progress_without_save_variables_and_skips_told_dialogs(graph):  # noqa: F811
    fake = FakeGame(branch="A")
    link_cm, book, patches, _ = await _book(fake, MemoryStore(":memory:"), graph)
    try:
        q = _middle_dialog()
        assert await book.make(q) == []
        state = WorldState(npcs={HILDA: "dead", LOBART: "alive"})
        assert book.dead_ends(state) == []  # not given
        book._fact(q, "story_quest_given")
        book._fact(q, "story_quest_stage", stage=2)
        assert book.dead_ends(state) == [questbook.DeadEnd(q.id, 2, 1, "DIR_Q1_S2G1_D", HILDA)]
        patches._record({"op": "told", "node": "DIR_Q1_S2G1_D"})
        assert book.dead_ends(state) == []
        # The save knows better than a missed chronicle message.
        state.vars[q.id] = LOG_OBSOLETE
        assert book.dead_ends(state) == []
    finally:
        await link_cm.__aexit__(None, None, None)


async def _repair_context(book: QuestBook, fake: FakeGame, q: Quest) -> tuple[Brief, Story]:
    fake.vars.update({q.id: 1, "DIR_Q1_STAGE": 2})
    fake.npc_states[HILDA]["state"] = "dead"
    state = (await book.director.world()).merge(await book.world())
    story = Story(book.graph, state, Patches.of(book.patches.ops()))
    dead = book.dead_ends(state)[0]
    node = story.patches.nodes[dead.node]
    offers = []
    for effect in node["effects"]:
        if questbook._gold(effect):
            offers.append(Offer("E1", effect, "золото"))
        elif effect["do"] == "xp":
            offers.append(Offer("E2", effect, "опыт"))
    return (
        Brief(
            q.id,
            q.title,
            HILDA,
            "Хильда",
            "герой",
            [q.stages[0].journal],
            [Ending(dead.node, HILDA, node["description"], 2, node["condition"], [], offers)],
            {LOBART: "Лобарт: был рядом"},
        ),
        story,
    )


def _handover(brief: Brief, **change: Any) -> Proposal:
    return replace(
        Proposal(
            "handover",
            ending=brief.endings[0].node,
            npc=LOBART,
            description="Принёс вино Хильды.",
            lines=[{"speaker": "npc", "text": "Хильды больше нет. Давай вино сюда."}],
            effects=["E1"],
            journal_now="Хильда умерла. Вино примет Лобарт.",
            journal_done="Я отдал Лобарту вино Хильды.",
            reason="Лобарт ведёт хозяйство.",
        ),
        **change,
    )


async def test_handover_replaces_the_same_node_and_keeps_the_required_effects(graph):  # noqa: F811
    fake = FakeGame(branch="A")
    link_cm, book, patches, _ = await _book(fake, MemoryStore(":memory:"), graph)
    try:
        q = _quest()
        assert await book.make(q) == []
        brief, story = await _repair_context(book, fake, q)
        proposal = _handover(brief)
        done, errors = await book.repair(proposal, brief, story)
        assert errors == [] and done is not None
        assert done.node == "DIR_Q1_DONE" and done.npc == LOBART
        node = Patches.of(patches.ops()).nodes[done.node]
        assert node["npc"] == LOBART and node["condition"] == story.patches.nodes[done.node]["condition"]
        assert {"do": "give", "item": GOODS, "count": 2, "to": "self"} in node["effects"]
        assert {"do": "set", "var": q.id, "value": 2} in node["effects"]
        assert {"do": "set", "var": "DIR_Q1_STAGE", "value": 4} in node["effects"]
        assert {"do": "give", "item": "ITMI_GOLD", "count": 20} in node["effects"]
        assert not any(effect["do"] == "xp" for effect in node["effects"])
        journal = next(effect for effect in node["effects"] if effect["do"] == "journal")
        assert journal["text"] == proposal.journal_done and journal["status"] == "LOG_SUCCESS"
        assert len(patches.records(QUEST)) == 2
        repaired = book.all()[q.id]
        assert repaired.turn_in().npc == LOBART and repaired.gold == q.gold and repaired.xp == 0
        assert repaired.repairs == [
            {"goal": "DIR_Q1_S3G1", "dead": HILDA, "npc": LOBART, "reason": proposal.reason}
        ]
        assert book.dead_ends(await book.world()) == []
        await book.settle(LOBART)
        assert "принял поручение умершего (Хильда)" in book.situation(LOBART)
        # The original stage triggers still bring the quest to its original turn-in id.
        fake.vars["DIR_Q1_STAGE"] = 3
        await book.settle(LOBART)
        assert book.progress(q.id) == questbook.Progress("given", 3)
        await fake.send(type="dialog_chosen", npc=LOBART, node=done.node, made=True)
        await _until(lambda: book.progress(q.id).state == "done")
        assert (
            book.situation(LOBART) == "Поручение «Украденное вино» герой выполнил, ты заплатил: 20 золотых."
        )
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_a_middle_stage_handover_keeps_the_goal_variable_and_the_later_turn_in(graph):  # noqa: F811
    fake = FakeGame(branch="A")
    link_cm, book, patches, _ = await _book(fake, MemoryStore(":memory:"), graph)
    try:
        q = _middle_dialog()
        assert await book.make(q) == []
        brief, story = await _repair_context(book, fake, q)
        done, errors = await book.repair(_handover(brief, effects=[]), brief, story)
        assert errors == [] and done is not None and done.node == "DIR_Q1_S2G1_D"
        node = Patches.of(patches.ops()).nodes[done.node]
        assert {"do": "set", "var": "DIR_Q1_S2G1", "value": 1} in node["effects"]
        assert {"do": "give", "item": GOODS, "count": 2, "to": "self"} in node["effects"]
        assert book.all()[q.id].gold == q.gold and book.all()[q.id].xp == q.xp
        assert Patches.of(patches.ops()).nodes["DIR_Q1_DONE"] == story.patches.nodes["DIR_Q1_DONE"]
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize(("status", "value"), [("obsolete", LOG_OBSOLETE), ("failed", LOG_FAILED)])
async def test_closing_a_book_quest_and_settling_it_from_the_save(graph, monkeypatch, status, value):  # noqa: F811
    fake = FakeGame(branch="A")
    link_cm, book, _, chronicle = await _book(fake, MemoryStore(":memory:"), graph)
    try:
        q = _quest()
        assert await book.make(q) == []
        brief, story = await _repair_context(book, fake, q)
        original_effect = book.link.story_effect

        async def effect(npc, effects, **kwargs):
            await original_effect(npc, effects, **kwargs)
            fake.vars.update({effect["var"]: effect["value"] for effect in effects if effect["do"] == "set"})

        monkeypatch.setattr(book.link, "story_effect", effect)
        done, errors = await book.repair(
            Proposal("close", status=status, journal_now="Хильда умерла, поручение закрыто."), brief, story
        )
        assert errors == [] and done is not None and done.kind == "close"
        assert fake.vars[q.id] == value and book.progress(q.id).state == status
        assert book.dead_ends(await book.world()) == []
        journal = fake.story[-1]["effects"][-1]
        assert journal["status"] == ("LOG_OBSOLETE" if status == "obsolete" else "LOG_FAILED")
        assert chronicle.facts(f"story_quest_{status}")[-1]["id"] == q.id
        # A second quest without a closure message is settled from the save as well.
        other = _quest(id="DIR_Q2", title="Другое поручение")
        fake.npc_states[HILDA]["state"] = "alive"
        assert await book.make(other) == []
        fake.vars.update({other.id: value, "DIR_Q2_STAGE": 2})
        await book.settle()
        assert book.progress(other.id).state == status
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_a_refused_handover_rolls_back_the_description_and_the_node(graph, monkeypatch):  # noqa: F811
    fake = FakeGame(branch="A")
    link_cm, book, patches, _ = await _book(fake, MemoryStore(":memory:"), graph)
    try:
        q = _quest()
        assert await book.make(q) == []
        brief, story = await _repair_context(book, fake, q)
        previous = patches.ops()
        original_patch = book.link.patch_graph

        async def refuse(ops, **kwargs):
            if not kwargs.get("replace"):
                raise GameError("dialog refused")
            await original_patch(ops, **kwargs)

        monkeypatch.setattr(book.link, "patch_graph", refuse)
        done, errors = await book.repair(_handover(brief), brief, story)
        assert done is None and errors == ["игра не приняла правку: dialog refused"]
        assert book.all()[q.id] == q and len(patches.records(QUEST)) == 1
        assert patches.ops() == previous
        assert fake.graph[-1]["replace"] is True and fake.graph[-1]["ops"] == previous
    finally:
        await link_cm.__aexit__(None, None, None)


def _blame_quest(crime: str = "NPC_THEFT_1") -> Quest:
    return _quest(stages=[
        Stage("Убедить Хильду, кто вор.", [Goal("blame", crime=crime, journal="Хильда поверила обвинению.")]),
        Stage(
            "Потребовать обещанную награду.", [Goal("talk", npc=HILDA, talk=_talk("Теперь ты знаешь вора."))]
        ),
    ])


def test_blame_builds_a_server_variable_without_a_world_truth_trigger():
    q = _blame_quest()
    assert problems(q, _ground(npc_crimes={"NPC_THEFT_1"})) == []
    ops = build(q)
    assert ops[0]["vars"]["DIR_Q1_S1G1"] == 0
    assert not any(op.get("id") == "DIR_Q1_S1G1" for op in ops)
    stage = next(op for op in ops if op.get("id") == "DIR_Q1_S1")
    assert [">=", ["var", "DIR_Q1_S1G1"], ["int", 1]] in stage["when"]
    assert any("существующее дело NPC" in error for error in problems(q, _ground()))


@pytest.mark.parametrize("truth", [True, False])
async def test_givers_blame_completes_the_goal_and_allows_reward_even_when_false(graph, monkeypatch, truth):  # noqa: F811
    fake = FakeGame(branch="A")
    link_cm, book, patches, chronicle = await _book(fake, MemoryStore(":memory:"), graph)
    try:
        q = _blame_quest()
        chronicle.record({"fact": "npc_crime", "id": "NPC_THEFT_1", "culprit": THIEF})
        assert await book.make(q) == []
        ops = patches.ops()
        stage = next(op for op in ops if op.get("id") == "DIR_Q1_S1")
        fake.vars.update({q.id: LOG_RUNNING, "DIR_Q1_STAGE": 1, "DIR_Q1_S1G1": 0})
        book._fact(q, "story_quest_given")
        effects = book.link.story_effect

        async def apply(npc, changes, **kwargs):
            await effects(npc, changes, **kwargs)
            fake.vars.update({e["var"]: e["value"] for e in changes if e["do"] == "set"})

        monkeypatch.setattr(book.link, "story_effect", apply)
        # Other people's belief and another deed cannot solve the commission.
        chronicle.record({"fact": "blamed", "id": "WRONG_PERSON", "npc": LOBART,
                          "crime": "NPC_THEFT_1", "culprit": THIEF, "truth": truth})
        chronicle.record({"fact": "blamed", "id": "WRONG_DEED", "npc": HILDA,
                          "crime": "OTHER_THEFT", "culprit": THIEF, "truth": truth})
        await book.settle()
        assert fake.vars["DIR_Q1_S1G1"] == 0 and not fake.evaluate(stage["when"], HILDA)
        chronicle.record({"fact": "blamed", "id": "BLAME_1", "npc": HILDA,
                          "crime": "NPC_THEFT_1", "culprit": THIEF, "truth": truth})
        await _until(lambda: bool(chronicle.facts("story_quest_goal")))
        assert fake.vars["DIR_Q1_S1G1"] == 1 and fake.evaluate(stage["when"], HILDA)
        fake.vars["DIR_Q1_STAGE"] = 2
        await fake.send(type="trigger_fired", id="DIR_Q1_S1", count=1, npc="PC_HERO", other="PC_HERO")
        await _until(lambda: book.progress(q.id).stage == 2)
        done = Patches.of(ops).nodes["DIR_Q1_DONE"]
        assert fake.evaluate(done["condition"], HILDA)
        fake.npc_items.setdefault(HILDA, {})["ITMI_GOLD"] = q.gold
        assert fake.give({"npc": HILDA, "effects": done["effects"]}) is None
        assert fake.hero_items["ITMI_GOLD"] == q.gold
        chronicle.record({"fact": "blame_refuted", "blamed": "BLAME_1", "how": "no_goods"})
        await book.settle()
        assert fake.vars["DIR_Q1_S1G1"] == 1
        assert len(chronicle.facts("story_quest_goal")) == 1
    finally:
        await book.close()
        await link_cm.__aexit__(None, None, None)


async def test_blame_goal_and_its_completion_follow_the_saved_branch_without_duplication(graph, monkeypatch):  # noqa: F811
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    link_cm, book, patches, chronicle = await _book(fake, store, graph)
    try:
        q = _blame_quest()
        chronicle.record({"fact": "npc_crime", "id": "NPC_THEFT_1", "culprit": THIEF})
        assert await book.make(q) == []
        fake.vars.update({q.id: LOG_RUNNING, "DIR_Q1_STAGE": 1, "DIR_Q1_S1G1": 0})
        book._fact(q, "story_quest_given")
        before = store.head("A")
        effects = book.link.story_effect

        async def apply(npc, changes, **kwargs):
            await effects(npc, changes, **kwargs)
            fake.vars.update({e["var"]: e["value"] for e in changes if e["do"] == "set"})

        monkeypatch.setattr(book.link, "story_effect", apply)
        chronicle.record({"fact": "blamed", "id": "BLAME_1", "npc": HILDA,
                          "crime": "NPC_THEFT_1", "culprit": LOBART, "truth": False})
        await _until(lambda: len(chronicle.facts("story_quest_goal")) == 1)
        after = store.head("A")
        await _until(lambda: fake.heads[-1:] == [("A", after)])
        await fake.send(type="session", branch="B", parent="A", parent_seq=after, reason="load")
        await _until(lambda: patches.branch == "B" and chronicle.branch == "B")
        assert book.all()[q.id] == q and fake.vars["DIR_Q1_S1G1"] == 1
        calls = len(fake.story)
        await book.settle()
        assert len(fake.story) == calls and len(chronicle.facts("story_quest_goal")) == 1
        # Loading the earlier save rolls both belief and completion back.
        fake.vars["DIR_Q1_S1G1"] = 0
        await fake.send(type="session", branch="C", parent="A", parent_seq=before, reason="load")
        await _until(lambda: patches.branch == "C" and chronicle.branch == "C")
        await book.settle()
        assert book.all()[q.id] == q and fake.vars["DIR_Q1_S1G1"] == 0
        assert not chronicle.facts("blamed") and not chronicle.facts("story_quest_goal")
    finally:
        await book.close()
        await link_cm.__aexit__(None, None, None)


async def test_amending_advances_the_current_stage_and_survives_the_saved_branch(graph, monkeypatch):  # noqa: F811
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    link_cm, book, patches, chronicle = await _book(fake, store, graph)
    entries = []
    book.journal = Journal(entries.append)
    try:
        q = _quest(names={HILDA: "Хильда", GOODS: "Вино"})
        assert await book.make(q) == []
        fake.vars.update({q.id: LOG_RUNNING, "DIR_Q1_STAGE": 1, "DIR_Q1_S1G1": 0})
        book._fact(q, "story_quest_given")
        before = store.head("A")
        stage = next(op for op in patches.ops() if op.get("id") == "DIR_Q1_S1")
        assert not fake.evaluate(stage["when"], HILDA)
        effects = book.link.story_effect

        async def apply(npc, changes, **kwargs):
            await effects(npc, changes, **kwargs)
            fake.vars.update({e["var"]: e["value"] for e in changes if e["do"] == "set"})

        monkeypatch.setattr(book.link, "story_effect", apply)
        conv = SimpleNamespace(npc=HILDA, persona=SimpleNamespace(name="Хильда"))
        result = await book.handle(
            None, conv,
            ToolCall("amend_quest", {"quest": q.id, "drop": ["dir_q1_s1g1", "DIR_Q1_S1G1"],
                                    "why": "Не нужно никуда идти"}, "a1"), None,
        )
        assert result.status == "accepted"
        assert fake.vars["DIR_Q1_S1G1"] == 1 and fake.evaluate(stage["when"], HILDA)
        assert fake.story[-1]["effects"] == [{"do": "set", "var": "DIR_Q1_S1G1", "value": 1}]
        assert chronicle.facts("quest_terms") == [{
            "fact": "quest_terms", "quest": q.id, "npc": HILDA, "name": "Хильда",
            "drop": ["DIR_Q1_S1G1"], "why": "Не нужно никуда идти",
        }]
        assert entries[-1]["kind"] == "quest_amended"
        # The stage trigger advances without the hero visiting the point.
        fake.vars["DIR_Q1_STAGE"] = 2
        await fake.send(type="trigger_fired", id="DIR_Q1_S1", count=1, npc="PC_HERO", other="PC_HERO")
        await _until(lambda: book.progress(q.id).stage == 2)
        situation = book.situation(HILDA)
        assert "Снято тобой: дойти до поля — «Не нужно никуда идти»" in situation
        assert "Можешь снять цели (amend_quest): DIR_Q1_S2G1 — добыть Вино ×2" in situation
        assert "DIR_Q1_S1G1 —" not in situation and "DIR_Q1_S3G1 —" not in situation
        after = store.head("A")
        await _until(lambda: fake.heads[-1:] == [("A", after)])
        await fake.send(type="session", branch="B", parent="A", parent_seq=after, reason="load")
        await _until(lambda: patches.branch == "B" and chronicle.branch == "B")
        assert book.situation(HILDA) == situation
        await fake.send(type="dialog_chosen", npc=HILDA, node="DIR_Q1_DONE", made=True)
        await _until(lambda: book.progress(q.id).state == "done")
        assert chronicle.facts("quest_outcome") == [{
            "fact": "quest_outcome", "quest": q.id, "npc": HILDA, "how": "amended",
            "drop": ["DIR_Q1_S1G1"], "why": ["Не нужно никуда идти"], "pay": "full",
        }]
        # An earlier save restores the agreement before the amendment.
        fake.vars.update({"DIR_Q1_STAGE": 1, "DIR_Q1_S1G1": 0})
        await fake.send(type="session", branch="C", parent="A", parent_seq=before, reason="load")
        await _until(lambda: patches.branch == "C" and chronicle.branch == "C")
        assert not chronicle.facts("quest_terms") and not chronicle.facts("quest_outcome")
        assert "Снято тобой:" not in book.situation(HILDA)
        assert "DIR_Q1_S1G1 — дойти до поля" in book.situation(HILDA)
    finally:
        await book.close()
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize("completion", ["trigger", "dialog_var"])
async def test_completed_goals_of_the_current_stage_cannot_be_amended(graph, completion):  # noqa: F811
    fake = FakeGame(branch="A")
    link_cm, book, _, chronicle = await _book(fake, MemoryStore(":memory:"), graph)
    try:
        q = _quest()
        if completion == "dialog_var":
            q.stages[0].goals[0] = Goal("talk", npc=LOBART, talk=_talk("Ты видел вора?"))
        q.stages[0].goals.append(Goal("wait", days=1))
        assert await book.make(q) == []
        fake.vars.update({q.id: LOG_RUNNING, "DIR_Q1_STAGE": 1, "DIR_Q1_S1G1": 0})
        book._fact(q, "story_quest_given")
        if completion == "trigger":
            Triggers(book.link, chronicle)
            await fake.send(type="trigger_fired", id="DIR_Q1_S1G1", count=1, npc="PC_HERO", other="PC_HERO")
            await _until(lambda: chronicle.facts("triggered"))
        else:
            fake.vars["DIR_Q1_S1G1"] = 1
            await book.prepare(HILDA)
        assert book.progress(q.id).stage == 1
        situation = book.situation(HILDA)
        assert "DIR_Q1_S1G1 —" not in situation
        assert "DIR_Q1_S1G2 — подождать 1 дн." in situation
        conv = SimpleNamespace(npc=HILDA, persona=SimpleNamespace(name="Хильда"))
        result = await book.handle(
            None, conv,
            ToolCall("amend_quest", {"quest": q.id, "drop": ["DIR_Q1_S1G1"], "why": "Передумала"}, "a1"),
            None,
        )
        assert result.status == "rejected" and "эта цель уже выполнена" in result.output
        assert not chronicle.facts("quest_terms") and not fake.story
        result = await book.handle(
            None, conv,
            ToolCall("amend_quest", {"quest": q.id, "drop": ["DIR_Q1_S1G2"], "why": "Не жди"}, "a2"),
            None,
        )
        assert result.status == "accepted"
        assert fake.story[-1]["effects"] == [{"do": "set", "var": "DIR_Q1_S1G2", "value": 1}]
        assert chronicle.facts("quest_terms")[0]["drop"] == ["DIR_Q1_S1G2"]
    finally:
        await book.close()
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize("goal, reason", [
    ("DIR_Q1_S1G1", "этот пункт не снимается"),
    ("DIR_Q1_S3G1", "этот пункт не снимается"),
    ("DIR_Q1_S4G1", "такого пункта нет в уговоре"),
])
async def test_vanilla_turn_in_and_unknown_goals_cannot_be_removed(graph, goal, reason):  # noqa: F811
    fake = FakeGame(branch="A")
    link_cm, book, _, chronicle = await _book(fake, MemoryStore(":memory:"), graph)
    try:
        q = _quest()
        q.stages[0].goals = [Goal("vanilla", quest=VANILLA_QUEST, state="running")]
        assert await book.make(q) == []
        book._fact(q, "story_quest_given")
        conv = SimpleNamespace(npc=HILDA, persona=SimpleNamespace(name="Хильда"))
        result = await book.handle(
            None, conv, ToolCall("amend_quest", {"quest": q.id, "drop": [goal], "why": "Передумала"}, "a1"),
            None,
        )
        assert result.status == "rejected" and reason in result.output
        assert not chronicle.facts("quest_terms") and not fake.story
    finally:
        await book.close()
        await link_cm.__aexit__(None, None, None)


async def test_only_a_given_quest_and_its_current_or_future_goals_can_be_amended(graph):  # noqa: F811
    fake = FakeGame(branch="A")
    link_cm, book, _, chronicle = await _book(fake, MemoryStore(":memory:"), graph)
    try:
        q = _quest()
        assert await book.make(q) == []
        conv = SimpleNamespace(npc=HILDA, persona=SimpleNamespace(name="Хильда"))
        call = ToolCall("amend_quest", {"quest": q.id, "drop": ["DIR_Q1_S1G1"], "why": "Передумала"}, "a1")
        assert (await book.handle(None, conv, call, None)).status == "rejected"
        book._fact(q, "story_quest_given")
        book._fact(q, "story_quest_stage", stage=2)
        assert (await book.handle(None, conv, call, None)).status == "rejected"
        call.arguments["drop"] = ["DIR_Q1_S2G1"]
        assert (await book.handle(None, conv, call, None)).status == "accepted"
        assert {t.name for t in book.tools(HILDA)} == {"drop_quest"}
        assert not chronicle.facts("quest_outcome")
    finally:
        await book.close()
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize("npc", [HILDA, LOBART])
async def test_the_giver_or_repair_successor_can_amend_and_drop_a_given_quest(graph, monkeypatch, npc):  # noqa: F811
    fake = FakeGame(branch="A")
    link_cm, book, _, chronicle = await _book(fake, MemoryStore(":memory:"), graph)
    entries = []
    book.journal = Journal(entries.append)
    try:
        q = _quest(repairs=[{"goal": "DIR_Q1_S3G1", "dead": HILDA, "npc": LOBART, "reason": "Принял"}])
        assert await book.make(q) == []
        fake.vars.update({q.id: LOG_RUNNING, "DIR_Q1_STAGE": 1})
        book._fact(q, "story_quest_given")
        assert {t.name for t in book.tools(npc)} == {"amend_quest", "drop_quest"}
        stranger = SimpleNamespace(npc=THIEF, persona=SimpleNamespace(name="Вор"))
        call = ToolCall("drop_quest", {"quest": q.id, "why": "Вино нашлось"}, "d1")
        assert (await book.handle(None, stranger, call, None)).status == "rejected"
        conv = SimpleNamespace(npc=npc, persona=SimpleNamespace(name=q.name(npc)))
        result = await book.handle(
            None, conv,
            ToolCall("amend_quest", {"quest": q.id, "drop": ["DIR_Q1_S2G1"], "why": "Хватит бутылки"}, "a1"),
            None,
        )
        assert result.status == "accepted"  # A later stage may be changed immediately, too.
        pending = []
        dialogue = SimpleNamespace(after_speech=lambda conv, job: pending.append(job))
        result = await book.handle(dialogue, conv, call, None)
        assert result.status == "accepted" and len(pending) == 1
        result = await book.handle(dialogue, conv, call, None)
        assert result.status == "rejected" and "поручение уже отменяется" in result.output
        result = await book.handle(
            dialogue, conv,
            ToolCall("amend_quest", {"quest": q.id, "drop": ["DIR_Q1_S1G1"], "why": "Не ходи"}, "a2"),
            None,
        )
        assert result.status == "rejected" and "поручение уже отменяется" in result.output
        assert len(pending) == 1
        assert not chronicle.facts("quest_outcome")
        effects = book.link.story_effect
        calls = len(fake.story)

        async def apply(npc, changes, **kwargs):
            await effects(npc, changes, **kwargs)
            fake.vars.update({e["var"]: e["value"] for e in changes if e["do"] == "set"})

        monkeypatch.setattr(book.link, "story_effect", apply)
        _, ok = await pending[0]()
        assert ok and fake.vars[q.id] == LOG_OBSOLETE
        assert len(fake.story) == calls + 1
        assert fake.story[-1]["effects"] == [
            {"do": "set", "var": q.id, "value": LOG_OBSOLETE},
            {"do": "journal", "topic": q.title, "text": "", "status": "LOG_OBSOLETE"},
        ]
        assert chronicle.facts("quest_outcome") == [{
            "fact": "quest_outcome", "quest": q.id, "npc": npc, "how": "dropped",
            "drop": ["DIR_Q1_S2G1"], "why": ["Хватит бутылки", "Вино нашлось"], "pay": "none",
        }]
        assert entries[-1]["kind"] == "quest_dropped"
        assert not chronicle.facts("story_quest_obsolete")
        await book.settle(npc)
        assert book.progress(q.id).state == "obsolete"
        assert len(chronicle.facts("story_quest_obsolete")) == 1
        assert book.tools(npc) == []
    finally:
        await book.close()
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize("error", [GameError("no game"), TimeoutError("no answer")])
async def test_a_failed_drop_can_be_retried(graph, monkeypatch, error):  # noqa: F811
    fake = FakeGame(branch="A")
    link_cm, book, _, chronicle = await _book(fake, MemoryStore(":memory:"), graph)
    try:
        q = _quest()
        assert await book.make(q) == []
        book._fact(q, "story_quest_given")
        conv = SimpleNamespace(npc=HILDA, persona=SimpleNamespace(name="Хильда"))
        pending = []
        dialogue = SimpleNamespace(after_speech=lambda conv, job: pending.append(job))
        call = ToolCall("drop_quest", {"quest": q.id, "why": "Вино нашлось"}, "d1")
        effects = book.link.story_effect

        async def unavailable(*args, **kwargs):
            raise error

        monkeypatch.setattr(book.link, "story_effect", unavailable)
        assert (await book.handle(dialogue, conv, call, None)).status == "accepted"
        _, ok = await pending[0]()
        assert not ok and not chronicle.facts("quest_outcome") and not fake.story
        monkeypatch.setattr(book.link, "story_effect", effects)
        assert (await book.handle(dialogue, conv, call, None)).status == "accepted"
        assert len(pending) == 2
        _, ok = await pending[1]()
        assert ok and len(fake.story) == 1
        assert [f["how"] for f in chronicle.facts("quest_outcome")] == ["dropped"]
    finally:
        await book.close()
        await link_cm.__aexit__(None, None, None)
