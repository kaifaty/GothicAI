import json
import os
from pathlib import Path

import pytest

from gothic_ai.dialogue import DialogueManager
from gothic_ai.gamedata.storygraph import StoryGraph, load
from gothic_ai.knowledge import Knowledge, Partial, StoryState, brief
from gothic_ai.llm import ToolCall, ToolResult
from gothic_ai.llm.fake import FakeBackend, FakeStep
from gothic_ai.memory import MemoryStore
from gothic_ai.persona import PersonaProvider
from gothic_ai.story import StoryPatches, freed
from tests.fake_game import FakeGame, connected
from tests.test_dialogue import _until
from tests.test_director import _line, _node

LOBART = "BAU_950_LOBART"
QUEST = "MIS_LOBART_RUEBEN"
GIL_BAU = 8
BRAGO = "BDT_1014_BANDIT_L"
BANDIT = "BDT_1013_BANDIT_L"
KILLER = "DIA_BDT_1014_BANDIT_KILLER"
AMBUSH = "DIA_BDT_1013_BANDIT_WHERE"
AMBUSH_LINE = "Это он, точно! Держи его!"


def _knows(info: str) -> list:
    return ["call", "NPC_KNOWSINFO", ["inst", "OTHER"], ["sym", info]]


def _chapter(op: str, n: int) -> list:
    return [op, ["var", "KAPITEL"], ["int", n]]


HAS_20 = [">=", ["call", "NPC_HASITEMS", ["inst", "OTHER"], ["sym", "ITPL_BEET"]], ["int", 20]]

GRAPH = {
    "version": 1,
    "nodes": [
        _node("DIA_LOBART_HALLO", LOBART, [_line("Что ты тут ошиваешься на моей ферме?")], description=""),
        _node(
            "DIA_LOBART_WORKNOW",
            LOBART,
            [
                _line("Я ищу работу.", "OTHER"),
                _line("Могу предложить поденную работу: собрать репу за амбаром."),
                {
                    "do": "journal_entry",
                    "topic": "TOPIC_RUEBEN",
                    "text": "Лобарт хочет, чтобы я собрал репу.",
                },
                {"do": "choice", "node": "DIA_LOBART_WORKNOW_OK", "text": "Хорошо..."},
            ],
            condition=["&&", _knows("DIA_LOBART_HALLO"), _chapter("<", 3)],
            description="Я ищу работу.",
        ),
        {
            **_node(
                "DIA_LOBART_WORKNOW_OK",
                LOBART,
                [_line("Хорошо...", "OTHER"), _line("Двадцать штук, не меньше.")],
                description="Хорошо...",
            ),
            "kind": "choice",
        },
        _node(
            "DIA_LOBART_RUEBENRUNNING",
            LOBART,
            [_line("Отнеси её моей жене."), _line("Но ты не собрал и двадцати!", when=["!", HAS_20])],
            condition=["&&", ["==", ["var", QUEST], ["int", 1, "LOG_RUNNING"]], HAS_20],
            description="Вот твоя репа!",
            reads=[QUEST],
        ),
        _node(
            "DIA_LOBART_MOREWORK",
            LOBART,
            [_line("Сначала собери репу.")],
            condition=_knows("DIA_LOBART_WORKNOW"),
            description="Есть ещё работа?",
        ),
        _node(
            "DIA_LOBART_ANDRE",
            LOBART,
            [_line("Прикончи этих полевых хищников.")],
            condition=_knows("DIA_ANDRE_HILFBAUERLOBART"),  # another npc's dialog: the story
            description="Меня послал Андре.",
        ),
        _node(
            "DIA_LOBART_KAP3",
            LOBART,
            [_line("Орки у ворот города!")],
            condition=_chapter("==", 3),
            description="Что нового?",
        ),
        _node(
            "DIA_LOBART_AFTER",
            LOBART,
            [_line("Будем держаться.")],
            condition=_knows("DIA_LOBART_KAP3"),
            description="И что теперь?",
        ),
        _node("DIA_LOBART_EXIT", LOBART, [], description="КОНЕЦ"),
        _node(
            "DIA_BAU_1_STANDARD",
            "$INSTANCE_HELP",
            [
                _line("Что новенького?", "OTHER"),
                _line("Онар нанял наёмников.", when=_chapter("==", 1)),
                _line("Говорят, в Долине драконы!", when=_chapter("==", 3)),
            ],
            condition=["int", 1],
            description="Что новенького?",
        ),
        _node(
            "DIA_VLK_6_STANDARD",
            "$INSTANCE_HELP",
            [_line("В городе полно бандитов.", when=_chapter("==", 1))],
            description="Что нового?",
        ),
    ],
    "functions": {},
    "instances": {},
    "vars": {},
    "quests": [
        {
            "var": QUEST,
            "topics": [{"topic": "TOPIC_RUEBEN", "title": "Убрать поле репы"}],
            "running": ["DIA_LOBART_WORKNOW_OK"],
            "success": ["DIA_LOBART_RUEBENRUNNING"],
            "failed": [],
            "obsolete": [],
            "other": [],
        }
    ],
}


@pytest.fixture
def graph(tmp_path: Path) -> StoryGraph:
    path = tmp_path / "story.json"
    path.write_text(json.dumps(GRAPH, ensure_ascii=False), encoding="utf-8")
    return load(path)


@pytest.fixture
def bandit_graph(tmp_path: Path) -> StoryGraph:
    path = tmp_path / "bandits.json"
    path.write_text(
        json.dumps(
            {
                "version": 1,
                "nodes": [
                    _node(KILLER, BRAGO, [_line(AMBUSH_LINE)], description="", important=True),
                    _node(
                        AMBUSH,
                        BANDIT,
                        [
                            _line("Куда ты идёшь?"),
                            {"do": "choice", "node": "DIA_BANDIT_ATTACK", "text": "Прочь с дороги!"},
                        ],
                        description="Куда ты идёшь?",
                    ),
                    _node(
                        "DIA_BANDIT_ATTACK",
                        BANDIT,
                        [_line("Тогда тебе конец!")],
                        kind="choice",
                        parents=[AMBUSH],
                        description="Прочь с дороги!",
                    ),
                ],
                "functions": {},
                "instances": {},
                "vars": {},
                "quests": [],
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    return load(path)


class _Link:
    world = "NEWWORLD.ZEN"


def _view(graph: StoryGraph, knows: set[str] = frozenset(), **vars: int):
    k = Knowledge(_Link(), graph)  # type: ignore[arg-type]
    own = k.dialogs(LOBART)
    state = StoryState(
        vars={"KAPITEL": 1, QUEST: 0, **vars},
        knows={n.id: n.id in knows for n in own} | {"DIA_ANDRE_HILFBAUERLOBART": False},
        fields={("SELF", "GUILD"): GIL_BAU},
    )
    view = k.view(LOBART, state, GIL_BAU, own, k.ambient(GIL_BAU, 1, named=True), k.quests(LOBART))
    k.views[LOBART] = view
    return k, view


def test_three_valued_story_logic(graph):
    p = Partial(graph, StoryState(vars={"KAPITEL": 1}, npcs={"BDT_1": {"state": "dead"}}))
    assert p.truth(None) is True
    assert p.truth(["&&", _chapter("<", 3), HAS_20]) is None  # the items: can't say from the story
    assert p.truth(["&&", _chapter("==", 3), HAS_20]) is False  # the chapter rules it out
    assert p.truth(["||", _chapter("==", 1), HAS_20]) is True
    assert p.truth(["!", HAS_20]) is None
    assert p.truth(["call", "NPC_ISDEAD", ["inst", "BDT_1"]]) is True
    assert p.truth(["==", ["var", "UNKNOWN"], ["int", 0]]) is None
    assert p.truth(["==", ["field", ["inst", "OTHER"], "GUILD"], ["int", 7]]) is None


def test_what_lobart_knows_in_chapter_1(graph):
    _, view = _view(graph, knows={"DIA_LOBART_HALLO"})
    assert [t.node.id for t in view.told] == ["DIA_LOBART_HALLO"]
    # the work he can offer, and more work once they have talked of it (only the talk has to get there);
    # no exits in the menu
    assert [(t.node.id, t.sure) for t in view.open] == [
        ("DIA_LOBART_MOREWORK", True),
        ("DIA_LOBART_WORKNOW", True),
    ]
    # hidden: the reward for a quest not begun, what waits for Andre, chapter 3 and what waits for it
    text = brief(view)
    assert "сейчас глава 1; что будет дальше, ты не знаешь" in text
    assert "Ты — крестьянин." in text
    assert "- «Я ищу работу.» — Могу предложить поденную работу: собрать репу за амбаром." in text
    assert "- «Есть ещё работа?» — Сначала собери репу." in text
    assert "Что говорят вокруг: «Онар нанял наёмников.»" in text  # his guild's talk, this chapter's
    for later in ("Орки", "драконы", "Андре", "Отнеси", "бандитов", "КОНЕЦ"):
        assert later not in text


def test_after_the_hero_took_the_job(graph):
    _, view = _view(graph, knows={"DIA_LOBART_HALLO", "DIA_LOBART_WORKNOW"}, MIS_LOBART_RUEBEN=1)
    assert [t.node.id for t in view.told] == ["DIA_LOBART_HALLO", "DIA_LOBART_WORKNOW"]
    assert [(t.node.id, t.sure) for t in view.open] == [
        ("DIA_LOBART_MOREWORK", True),
        ("DIA_LOBART_RUEBENRUNNING", False),  # waits for the turnips the hero brings
    ]
    text = brief(view)
    assert "«Убрать поле репы»: идёт. Суть: Лобарт хочет, чтобы я собрал репу." in text
    assert "- «Вот твоя репа!»\n" in text  # it waits for the turnips: the topic, not what he'd say then


def test_chapter_3(graph):
    _, view = _view(graph, knows={"DIA_LOBART_HALLO"}, KAPITEL=3)
    assert [(t.node.id, t.sure) for t in view.open] == [("DIA_LOBART_AFTER", True), ("DIA_LOBART_KAP3", True)]
    text = brief(view)
    assert "- «Что нового?» — Орки у ворот города!" in text
    assert "«Говорят, в Долине драконы!»" in text and "работу" not in text


def test_recall_gives_the_topic_in_full(graph):
    k, _ = _view(graph, knows={"DIA_LOBART_HALLO", "DIA_LOBART_WORKNOW"}, MIS_LOBART_RUEBEN=1)
    found = k.recall(LOBART, "сколько репы собрать?")
    assert found is not None and found.startswith("Ты знаешь:\n[«Я ищу работу.» — герой это уже слышал")
    assert "Ты: «Могу предложить поденную работу: собрать репу за амбаром.»" in found
    assert "Если герой ответит «Хорошо...», ты: «Двадцать штук, не меньше.»" in found
    assert k.recall(LOBART, "драконы") is None  # chapter 3 talk: not yet


@pytest.mark.parametrize("heard", [False, True])
async def test_freed_dialogs_are_absent_from_open_and_told_knowledge(bandit_graph, heard):
    fake = FakeGame()
    fake.vars = {"KAPITEL": 1}
    if heard:
        fake.known = {KILLER, AMBUSH}
    async with connected(fake) as link:
        vanilla = Knowledge(link, bandit_graph)
        await vanilla.prepare(BRAGO)
        assert AMBUSH_LINE in vanilla.situation(BRAGO)
        assert AMBUSH_LINE in vanilla.recall(BRAGO, "Это он, точно")
        topics = vanilla.views[BRAGO].told if heard else vanilla.views[BRAGO].open
        assert [t.node.id for t in topics] == [KILLER]
        await vanilla.prepare(BANDIT)
        assert "Тогда тебе конец!" in vanilla.recall(BANDIT, "конец")

        patches = StoryPatches(
            link, MemoryStore(":memory:"), static=freed(lambda node: node in bandit_graph.nodes)
        )
        knowledge = Knowledge(link, bandit_graph, patches)
        for npc in (BRAGO, BANDIT):
            await knowledge.prepare(npc)
            assert knowledge.dialogs(npc) == []
            assert knowledge.views[npc].open == [] and knowledge.views[npc].told == []
        assert "Это он, точно" not in knowledge.situation(BRAGO)
        assert knowledge.recall(BRAGO, "Это он, точно") is None
        assert knowledge.recall(BANDIT, "конец") is None  # disabled parent hides its attack choice

        await patches.apply({"op": "enable", "node": KILLER})
        await knowledge.prepare(BRAGO)
        assert AMBUSH_LINE in knowledge.situation(BRAGO)
        assert AMBUSH_LINE in knowledge.recall(BRAGO, "Это он, точно")


async def test_branch_disable_enable_and_load_change_npc_knowledge(bandit_graph):
    fake = FakeGame(branch="A")
    fake.vars = {"KAPITEL": 1}
    async with connected(fake) as link:
        patches = StoryPatches(link, MemoryStore(":memory:"))
        knowledge = Knowledge(link, bandit_graph, patches)
        await _until(lambda: len(fake.graph) == 1)
        await knowledge.prepare(BRAGO)
        assert AMBUSH_LINE in knowledge.situation(BRAGO)

        await patches.apply({"op": "disable", "node": KILLER})
        await knowledge.prepare(BRAGO)
        assert "Это он, точно" not in knowledge.situation(BRAGO)
        assert knowledge.recall(BRAGO, "Это он, точно") is None

        await patches.apply({"op": "enable", "node": KILLER})
        await knowledge.prepare(BRAGO)
        assert AMBUSH_LINE in knowledge.situation(BRAGO)
        assert AMBUSH_LINE in knowledge.recall(BRAGO, "Это он, точно")

        # Loading the save made after disable forgets the later enable.
        await fake.send(type="session", branch="B", parent="A", parent_seq=1, reason="load")
        await _until(lambda: patches.branch == "B" and len(fake.graph) == 4)
        await knowledge.prepare(BRAGO)
        assert "Это он, точно" not in knowledge.situation(BRAGO)
        assert knowledge.recall(BRAGO, "Это он, точно") is None


async def test_disabled_choices_and_ambient_dialogs_do_not_leak_into_recall(graph):
    fake = FakeGame()
    fake.vars = {"KAPITEL": 1, QUEST: 0}
    fake.fields = {("SELF", "GUILD"): GIL_BAU}
    async with connected(fake) as link:
        patches = StoryPatches(link, MemoryStore(":memory:"))
        knowledge = Knowledge(link, graph, patches)
        await _until(lambda: len(fake.graph) == 1)
        await knowledge.prepare(LOBART)
        assert "Двадцать штук, не меньше." in knowledge.recall(LOBART, "работа")
        assert "Онар нанял наёмников." in knowledge.situation(LOBART)

        await patches.apply({"op": "disable", "node": "DIA_LOBART_WORKNOW_OK"})
        await patches.apply({"op": "disable", "node": "DIA_BAU_1_STANDARD"})
        await knowledge.prepare(LOBART)
        assert "Могу предложить поденную работу" in knowledge.recall(LOBART, "работа")
        assert "Двадцать штук" not in knowledge.recall(LOBART, "работа")
        assert knowledge.recall(LOBART, "Двадцать штук") is None
        assert "Онар" not in knowledge.situation(LOBART)
        assert knowledge.recall(LOBART, "Онар") is None


async def test_the_npc_knows_in_its_turn_and_recalls_the_rest(graph):
    fake = FakeGame()
    fake.vars = {"KAPITEL": 1, QUEST: 1}
    fake.known = {"DIA_LOBART_HALLO", "DIA_LOBART_WORKNOW"}
    fake.fields = {("SELF", "GUILD"): GIL_BAU, ("SELF", "VOICE"): 5}
    llm = FakeBackend(
        script=[
            FakeStep("Хм…", [ToolCall("recall", {"query": "работа, репа"}, "c1")]),
            FakeStep("Двадцать штук, не меньше."),
        ],
        delay=0,
    )
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None))
        knowledge = Knowledge(link, graph)
        dialogue.extensions.append(knowledge)
        dialogue.open(LOBART, "Лобарт")
        await dialogue.say_as_player(LOBART, "Сколько репы тебе надо?")
        await _until(lambda: len(llm.calls) == 2)

    situation = str(llm.calls[0][1][-2].content)
    assert "Что ты знаешь (сейчас глава 1" in situation
    assert "«Убрать поле репы»: идёт" in situation
    asked = next(m for m in fake.story if m["type"] == "story_eval")["exprs"]
    assert ["field", ["inst", "SELF"], "GUILD"] in asked and ["field", ["inst", "OTHER"], "GUILD"] in asked
    result = next(i for i in llm.calls[1][1] if isinstance(i, ToolResult))
    assert "Ты знаешь:\n[«Я ищу работу.»" in result.output


@pytest.mark.skipif(
    not (
        Path(os.environ.get("GOTHIC_AI_DATA", Path.home() / ".local/share/gothic-ai")) / "story.json"
    ).exists(),
    reason="no story graph of the game on this machine",
)
def test_on_the_games_own_story():
    data = Path(os.environ.get("GOTHIC_AI_DATA", Path.home() / ".local/share/gothic-ai"))
    graph = load(data / "story.json")
    k = Knowledge(_Link(), graph)  # type: ignore[arg-type]

    def view(npc: str, guild: int, knows: set[str], **vars: int):
        own = k.dialogs(npc)
        state = StoryState(
            vars=vars, knows={n.id: n.id in knows for n in own}, fields={("SELF", "GUILD"): guild}
        )
        return k.view(npc, state, guild, own, k.ambient(guild, None, named=True), k.quests(npc))

    xardas = {"KAPITEL": 1, "XARDAS_GOESTORITUALINNOSEYE": 0, "SATURAS_KLAUEINSMEER": 0}
    ch1 = view("NONE_100_XARDAS", 0, {"DIA_XARDAS_HELLO"}, **xardas)
    ids = {t.node.id for t in [*ch1.told, *ch1.open]}
    assert "DIA_XARDAS_TODO" in ids
    # the ritual of chapter 3, and what waits for it
    assert not ids & {"DIA_XARDAS_BACKFROMOW", "DIA_XARDAS_RITUALREQUEST", "DIA_XARDAS_BEREIT"}
    ch3 = view("NONE_100_XARDAS", 0, {"DIA_XARDAS_HELLO"}, **{**xardas, "KAPITEL": 3})
    ids = {t.node.id for t in ch3.open}
    assert "DIA_XARDAS_BACKFROMOW" in ids and "DIA_XARDAS_TODO" not in ids

    lobart = view("BAU_950_LOBART", 8, set(), KAPITEL=1, MIS_LOBART_RUEBEN=0)
    ids = {t.node.id for t in lobart.open}
    assert "DIA_LOBART_WORKNOW" in ids and "DIA_LOBART_RUEBENRUNNING" not in ids
    assert lobart.rumors  # the farmers' talk of chapter 1
