import asyncio
import copy
import json
import os
from pathlib import Path

import pytest

from gothic_ai.chronicle import Chronicle
from gothic_ai.dialogue import DialogueManager
from gothic_ai.gamedata.storygraph import StoryGraph, load
from gothic_ai.llm import ToolCall, ToolResult
from gothic_ai.llm.fake import FakeBackend, FakeStep
from gothic_ai.memory import MemoryStore
from gothic_ai.persona import PersonaProvider
from gothic_ai.questlog import QuestLog
from gothic_ai.vanilla import VanillaQuests, _bridge, _claim, _takes
from tests.fake_game import FakeGame, connected
from tests.test_dialogue import _until
from tests.test_director import GRAPH, HAS_20, HILDA, LOBART, QUEST, _node

TO_HILDA = "MIS_LOBART_RUEBENTOHILDA"
BANDIT = "BDT_1_BANDIT"
KILL = "MIS_KILLBANDIT"
ACCEPT = FakeStep("Ладно, верю. Держи плату.", [ToolCall("accept_quest", {"quest": QUEST}, "c1")])


@pytest.fixture
def graph(tmp_path: Path) -> StoryGraph:
    """Lobart's turnips (as in test_director), with the next step (bring them to Hilda) and a kill quest of
    Lobart's: the bandit must be dead."""
    g = copy.deepcopy(GRAPH)
    ending = next(n for n in g["nodes"] if n["id"] == "DIA_LOBART_RUEBENRUNNING")
    ending["effects"].insert(
        2, {"do": "set", "var": TO_HILDA, "value": ["int", 1, "LOG_RUNNING"], "when": HAS_20}
    )
    dead = ["call", "NPC_ISDEAD", ["inst", "BANDIT"]]
    g["nodes"].append(
        _node(
            "DIA_LOBART_BANDITDEAD",
            LOBART,
            [
                {"do": "set", "var": KILL, "value": ["int", 2, "LOG_SUCCESS"], "when": dead},
                {"do": "xp", "amount": ["int", 100], "when": dead},
            ],
            condition=["==", ["var", KILL], ["int", 1]],
            description="Бандит мёртв.",
        )
    )
    g["instances"]["BANDIT"] = BANDIT
    g["quests"] += [
        {
            "var": TO_HILDA,
            "topics": [],
            "running": ["DIA_LOBART_RUEBENRUNNING"],
            "success": [],
            "failed": [],
            "obsolete": [],
            "other": [],
        },
        {
            "var": KILL,
            "topics": [{"topic": "TOPIC_BANDIT", "title": "Бандит у фермы"}],
            "running": [],
            "success": ["DIA_LOBART_BANDITDEAD"],
            "failed": [],
            "obsolete": [],
            "other": [],
        },
    ]
    path = tmp_path / "story.json"
    path.write_text(json.dumps(g, ensure_ascii=False), encoding="utf-8")
    return load(path)


async def _setup(fake: FakeGame, llm: FakeBackend, graph: StoryGraph, store: MemoryStore):
    fake.vars = {QUEST: 1, "KAPITEL": 1, TO_HILDA: 0}
    fake.npc_states = {
        n: {"state": "alive", "name": name} for n, name in [(LOBART, "Лобарт"), (HILDA, "Хильда")]
    }
    fake.item_names = {"ITPL_BEET": "Репа", "ITMI_GOLD": "Золото"}
    fake.instances = {"HILDA": HILDA, "BANDIT": BANDIT}
    link_cm = connected(fake)
    link = await link_cm.__aenter__()
    dialogue = DialogueManager(link, llm, PersonaProvider(None), memory=store)
    chronicle = Chronicle(link, store)
    vanilla = VanillaQuests(link, dialogue, chronicle, graph, hero_distance=lambda npc: 300.0)
    await _until(lambda: chronicle.branch is not None)
    return link_cm, dialogue, chronicle, vanilla


def _effects(fake: FakeGame, npc: str) -> list[list[dict]]:
    return [m["effects"] for m in fake.story if m["type"] == "story_effect" and m["npc"] == npc]


async def test_a_lie_to_lobart_is_paid_and_found_out_the_next_morning(graph):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    llm = FakeBackend(script=[ACCEPT, FakeStep("Ну?")], delay=0)
    link_cm, dialogue, chronicle, vanilla = await _setup(fake, llm, graph, store)
    try:
        dialogue.open(LOBART, "Лобарт")
        await vanilla.prepare(LOBART)
        situation = vanilla.situation(LOBART)
        assert "Ты ждёшь от героя: «Убрать поле репы» (id MIS_LOBART_RUEBEN)" in situation
        assert "герой принесёт тебе Репа ×20" in situation and "5 золотых, опыт +50" in situation
        assert "у героя с собой Репа ×0 из 20" in situation  # what Lobart sees for himself
        # he sees the hero has none on him: he may call it a lie to his face; nothing to go and check
        assert [t.name for t in vanilla.tools(LOBART)] == ["accept_quest", "expose_lie"]

        # the hero has no turnips and says they are in the barn; Lobart believes him
        await dialogue.say_as_player(LOBART, "Репа в амбаре, двадцать штук. Плати.")
        await _until(lambda: chronicle.facts("lie") != [])
        assert "accept_quest" in [t.name for t in llm.calls[0][2]]
        assert "Ты ждёшь от героя" in str(llm.calls[0][1])
        effects = _effects(fake, LOBART)[0]
        assert {"do": "set", "var": QUEST, "value": 2} in effects
        assert {"do": "set", "var": TO_HILDA, "value": 1} in effects  # the next step, as the scripts do it
        assert {"do": "xp", "amount": 50} in effects and {
            "do": "give",
            "item": "ITMI_GOLD",
            "count": 5,
        } in effects
        assert not any(e.get("to") == "self" for e in effects)  # nothing to take
        lie = chronicle.facts("lie")[0]
        assert (lie["kind"], lie["truth"], lie["follow"]) == ("fetch", "ITPL_BEET 0 of 20", [TO_HILDA])
        assert "Ты засчитал герою дело «Убрать поле репы»" in vanilla.situation(LOBART)
        assert vanilla.tools(LOBART) == []

        # the same day nothing happens; the next morning he misses the turnips
        run = asyncio.create_task(vanilla.run())
        await asyncio.sleep(1.5)
        assert chronicle.facts("found_out") == []
        fake.day = 2
        fake.people_near = [{"npc": HILDA, "name": "Хильда", "distance": 400, "guild": 0}]
        await _until(lambda: chronicle.facts("rumor") != [], 10)
        run.cancel()
        assert "наутро хватился" in chronicle.facts("found_out")[0]["how"]
        consequences = _effects(fake, LOBART)[-1]
        assert {"do": "refuse_talk", "seconds": 300} in consequences and {
            "do": "attack",
            "reason": "AR_NONE",
        } in consequences
        assert dialogue.conversations[LOBART].intent.attitude == "hostile"
        assert "Ты знаешь, что герой солгал тебе" in vanilla.situation(LOBART)
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_the_truth_is_no_lie(graph):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    llm = FakeBackend(script=[ACCEPT, FakeStep("Ну?")], delay=0)
    link_cm, dialogue, chronicle, vanilla = await _setup(fake, llm, graph, store)
    fake.hero_items = {"ITPL_BEET": 20}
    try:
        dialogue.open(LOBART, "Лобарт")
        await dialogue.say_as_player(LOBART, "Вот твоя репа.")
        await _until(lambda: chronicle.facts("claim") != [])
        assert chronicle.facts("claim")[0]["truth"] is True and chronicle.facts("lie") == []
        await _until(lambda: _effects(fake, LOBART) != [])
        # as in the scripts: Lobart pays, the turnips stay with the hero for Hilda
        assert {"do": "set", "var": TO_HILDA, "value": 1} in _effects(fake, LOBART)[0]
        assert vanilla.told[QUEST].lie is False
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_a_lie_is_mended_when_the_next_step_gets_the_goods(graph):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    llm = FakeBackend(script=[ACCEPT, FakeStep("Ну?")], delay=0)
    link_cm, dialogue, chronicle, vanilla = await _setup(fake, llm, graph, store)
    try:
        dialogue.open(LOBART, "Лобарт")
        await dialogue.say_as_player(LOBART, "Репа в амбаре.")
        await _until(lambda: chronicle.facts("lie") != [])
        fake.vars[TO_HILDA] = 2  # he got turnips after all and brought them to Hilda
        fake.day = 2
        run = asyncio.create_task(vanilla.run())
        await _until(lambda: chronicle.facts("lie_mended") != [], 10)
        run.cancel()
        assert chronicle.facts("found_out") == []
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_a_lie_about_a_killing_is_found_out_when_the_victim_turns_up(graph):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    accept = FakeStep("Мёртв, говоришь? Хорошо.", [ToolCall("accept_quest", {"quest": KILL}, "c1")])
    llm = FakeBackend(script=[accept, FakeStep("Ну?")], delay=0)
    link_cm, dialogue, chronicle, vanilla = await _setup(fake, llm, graph, store)
    fake.vars[KILL] = 1
    fake.npc_states[BANDIT] = {"state": "alive", "name": "Бандит"}
    try:
        dialogue.open(LOBART, "Лобарт")
        await vanilla.prepare(LOBART)
        situation = vanilla.situation(LOBART)
        assert "будут мертвы: Бандит" in situation and "далеко, проверить сам не можешь" in situation
        await dialogue.say_as_player(LOBART, "Бандит мёртв.")
        await _until(lambda: chronicle.facts("lie") != [])
        lie = chronicle.facts("lie")[0]
        assert (lie["kind"], lie["tag"], lie["targets"]) == ("kill", f"{KILL}_ALIVE", [BANDIT])
        await _until(lambda: any(m["type"] == "story_tag" for m in fake.story))
        tag = next(m for m in fake.story if m["type"] == "story_tag")
        assert (tag["tag"], tag["npcs"]) == (f"{KILL}_ALIVE", [BANDIT])
        watch = [m for m in fake.story if m["type"] == "story_watch"][-1]
        assert (watch["npcs"], watch["owner"]) == ([LOBART], "vanilla")

        run = asyncio.create_task(vanilla.run())
        near = [{"npc": LOBART, "distance": 900}]  # behind a wall: Lobart does not see him
        await fake.send(
            type="story_world", tags=[{"tag": f"{KILL}_ALIVE", "alive": 1, "dead": 0, "near": near}]
        )
        await asyncio.sleep(1.5)
        assert chronicle.facts("found_out") == []
        near = [{"npc": LOBART, "distance": 900, "seen": 900}]
        await fake.send(
            type="story_world", tags=[{"tag": f"{KILL}_ALIVE", "alive": 1, "dead": 0, "near": near}]
        )
        await _until(lambda: chronicle.facts("found_out") != [], 5)
        run.cancel()
        assert "увидел живым" in chronicle.facts("found_out")[0]["how"]
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_lies_follow_the_save_branch(graph):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    llm = FakeBackend(script=[ACCEPT, FakeStep("Ну?")], delay=0)
    link_cm, dialogue, chronicle, vanilla = await _setup(fake, llm, graph, store)
    try:
        dialogue.open(LOBART, "Лобарт")
        await dialogue.say_as_player(LOBART, "Репа в амбаре.")
        await _until(lambda: chronicle.facts("lie") != [])
        assert vanilla.told[QUEST].lie
        vanilla._load()  # as after a load of a save made after the lie
        t = vanilla.told[QUEST]
        assert (t.lie, t.kind, t.follow, t.found_out) == (True, "fetch", [TO_HILDA], False)
    finally:
        await link_cm.__aexit__(None, None, None)


def _outputs(llm: FakeBackend) -> str:
    """What came back to the model from its tool calls, all steps."""
    return " ".join(m.output for _, items, _ in llm.calls for m in items if isinstance(m, ToolResult))


def test_an_ending_takes_what_the_hero_has():
    effects = [
        {"do": "give", "item": "ITPL_BEET", "count": 20, "to": "self", "take": True},
        {"do": "give", "item": "ITMI_GOLD", "count": 5},
    ]
    assert _takes(effects, {"ITPL_BEET": 5}, truth=False) == [
        {"do": "give", "item": "ITPL_BEET", "count": 5, "to": "self"},
        {"do": "give", "item": "ITMI_GOLD", "count": 5},
    ]
    assert _takes(effects, {}, truth=False) == [{"do": "give", "item": "ITMI_GOLD", "count": 5}]
    assert _takes(effects, {"ITPL_BEET": 20}, truth=True)[0]["count"] == 20


async def test_what_the_hero_has_on_him_is_seen_and_the_rest_is_the_lie(graph):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    llm = FakeBackend(script=[ACCEPT, FakeStep("Ну?")], delay=0)
    link_cm, dialogue, chronicle, vanilla = await _setup(fake, llm, graph, store)
    fake.hero_items = {"ITPL_BEET": 5}
    try:
        dialogue.open(LOBART, "Лобарт")
        await vanilla.prepare(LOBART)
        assert "у героя с собой Репа ×5 из 20 — это он отдаст тебе сразу" in vanilla.situation(LOBART)
        await dialogue.say_as_player(LOBART, "Вот пять, остальные пятнадцать в амбаре.")
        await _until(lambda: chronicle.facts("lie") != [])
        lie = chronicle.facts("lie")[0]
        assert (lie["truth"], lie["items"]) == ("ITPL_BEET 5 of 20", "Репа ×15")

        fake.day = 2
        run = asyncio.create_task(vanilla.run())
        await _until(lambda: chronicle.facts("found_out") != [], 10)
        run.cancel()
        assert "(Репа ×15)" in chronicle.facts("found_out")[0]["how"]
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_someone_alive_before_its_eyes_is_not_taken_on_word(graph):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    accept = FakeStep("Мёртв, говоришь?", [ToolCall("accept_quest", {"quest": KILL}, "c1")])
    expose = FakeStep("Врёшь! Вон он стоит.", [ToolCall("expose_lie", {"quest": KILL}, "c2")])
    llm = FakeBackend(script=[accept, expose, FakeStep("Пошёл вон.")], delay=0)
    link_cm, dialogue, chronicle, vanilla = await _setup(fake, llm, graph, store)
    fake.vars[KILL] = 1
    fake.npc_states[BANDIT] = {"state": "alive", "name": "Бандит", "distance": 1200}
    fake.people_near = [{"npc": HILDA, "name": "Хильда", "distance": 400, "guild": 0}]
    try:
        dialogue.open(LOBART, "Лобарт")
        await vanilla.prepare(LOBART)
        assert "у тебя на глазах: Бандит — жив" in vanilla.situation(LOBART)
        assert [t.name for t in vanilla.tools(LOBART)] == ["accept_quest", "expose_lie"]

        await dialogue.say_as_player(LOBART, "Бандит мёртв, я его убил.")
        await _until(lambda: chronicle.facts("caught") != [])
        assert "Отклонено: Бандит жив — у тебя на глазах, в 12 м" in _outputs(llm)
        assert chronicle.facts("claim") == [] and chronicle.facts("lie") == []  # nothing paid
        c = chronicle.facts("caught")[0]
        assert (c["quest"], c["npc"], c["how"]) == (
            KILL,
            LOBART,
            "видел сам: Бандит жив — у тебя на глазах, в 12 м",
        )
        await _until(lambda: chronicle.facts("rumor") != [])
        assert chronicle.facts("rumor")[0]["to"] == HILDA
        assert dialogue.conversations[LOBART].intent.attitude == "wary"
        assert not any(m["type"] == "story_effect" for m in fake.story)  # no beating: he took nothing
        trust = vanilla.trust
        assert "он уже лгал тебе: «Бандит у фермы» — видел сам" in (trust.situation(LOBART) or "")
        assert "ты слышал, что он обманул других: Лобарт («Бандит у фермы»)" in (trust.situation(HILDA) or "")
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_the_npc_goes_to_see_and_catches_the_lie(graph):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    check = FakeStep("Сейчас сам посмотрю.", [ToolCall("check_claim", {"quest": KILL}, "c1")])
    llm = FakeBackend(script=[check, FakeStep("Ах ты лжец!")], delay=0)
    link_cm, dialogue, chronicle, vanilla = await _setup(fake, llm, graph, store)
    fake.vars[KILL] = 1
    fake.npc_states[BANDIT] = {"state": "alive", "name": "Бандит", "distance": 4000}
    try:
        await vanilla.link.attach(LOBART)
        dialogue.open(LOBART, "Лобарт")
        await vanilla.prepare(LOBART)
        assert "Бандит — в 40 м, отсюда не видно: можешь сходить и посмотреть сам" in vanilla.situation(
            LOBART
        )
        # (expose_lie is for the turnips: the hero has none on him)
        assert [t.name for t in vanilla.tools(LOBART)] == ["accept_quest", "check_claim", "expose_lie"]

        await dialogue.say_as_player(LOBART, "Бандит мёртв.")
        await _until(lambda: chronicle.facts("caught") != [])
        await _until(lambda: LOBART not in vanilla.checking)
        go = [m["args"] for m in fake.received if m["type"] == "action" and m["action"] == "go_to_npc"]
        # there, to see; back to the hero, to say it to his face
        assert go == [{"npc": BANDIT, "radius": 1800}, {"npc": "PLAYER", "radius": 300}]
        assert "сходил проверить и увидел: Бандит жив, в 18 м" in chronicle.facts("caught")[0]["how"]
        await _until(lambda: len(llm.calls) >= 2)  # he says what he thinks of it
        assert "Он тебе солгал" in str(llm.calls[-1][1])
        assert chronicle.facts("claim") == []
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_behind_a_wall_is_not_before_its_eyes(graph):
    """The bandit is 10 m from Lobart, behind the barn: Lobart does not see him (the engine's sight), so he
    can neither call it a lie to his face nor refuse on what he sees; he goes, looks that way, and sees."""
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    check = FakeStep("Пойду гляну.", [ToolCall("check_claim", {"quest": KILL}, "c1")])
    llm = FakeBackend(script=[check, FakeStep("Ах ты лжец!")], delay=0)
    link_cm, dialogue, chronicle, vanilla = await _setup(fake, llm, graph, store)
    fake.vars[KILL] = 1
    fake.npc_states[BANDIT] = {"state": "alive", "name": "Бандит", "distance": 1000, "seen": False}
    try:
        await vanilla.link.attach(LOBART)
        dialogue.open(LOBART, "Лобарт")
        await vanilla.prepare(LOBART)
        situation = vanilla.situation(LOBART) or ""
        assert "на глазах" not in situation
        assert "Бандит — в 10 м, отсюда не видно: можешь сходить и посмотреть сам" in situation
        assert "check_claim" in [t.name for t in vanilla.tools(LOBART)]

        await dialogue.say_as_player(LOBART, "Бандит мёртв.")
        await _until(lambda: chronicle.facts("caught") != [])
        acts = [(m["action"], m["args"].get("npc")) for m in fake.received if m["type"] == "action"]
        assert acts[:2] == [("go_to_npc", BANDIT), ("turn_to", BANDIT)]  # there, and looks that way
        assert "сходил проверить и увидел: Бандит жив, в 18 м" in chronicle.facts("caught")[0]["how"]
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_the_npc_goes_to_see_and_pays_for_the_truth(graph):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    check = FakeStep("Сейчас сам посмотрю.", [ToolCall("check_claim", {"quest": KILL}, "c1")])
    llm = FakeBackend(script=[check, FakeStep("Правда. Молодец.")], delay=0)
    link_cm, dialogue, chronicle, vanilla = await _setup(fake, llm, graph, store)
    fake.vars[KILL] = 1
    fake.npc_states[BANDIT] = {"state": "dead", "name": "Бандит", "distance": 4000}
    try:
        await vanilla.link.attach(LOBART)
        dialogue.open(LOBART, "Лобарт")
        await dialogue.say_as_player(LOBART, "Бандит мёртв.")
        await _until(lambda: chronicle.facts("claim") != [])
        claim = chronicle.facts("claim")[0]
        assert (claim["truth"], claim["checked"]) == (True, True)
        assert {"do": "xp", "amount": 100} in _effects(fake, LOBART)[0]
        assert chronicle.facts("caught") == [] and chronicle.facts("lie") == []
        assert "он уже говорил тебе правду: «Бандит у фермы»" in (vanilla.trust.situation(LOBART) or "")
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_once_lied_to_it_takes_his_word_no_more(graph):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    llm = FakeBackend(
        script=[ACCEPT, FakeStep("Не верю."), ACCEPT, FakeStep("Вот это другое дело.")], delay=0
    )
    link_cm, dialogue, chronicle, vanilla = await _setup(fake, llm, graph, store)
    try:
        chronicle.record(
            {"fact": "caught", "quest": KILL, "npc": LOBART, "title": "Бандит у фермы", "how": "видел сам"}
        )
        dialogue.open(LOBART, "Лобарт")
        await dialogue.say_as_player(LOBART, "Репа в амбаре, двадцать штук.")
        await _until(lambda: "Отклонено: он уже лгал тебе" in _outputs(llm))
        assert chronicle.facts("claim") == []

        # what it sees for itself it takes: he brings all twenty
        fake.hero_items = {"ITPL_BEET": 20}
        await dialogue.say_as_player(LOBART, "Вот вся репа.")
        await _until(lambda: chronicle.facts("claim") != [])
        assert chronicle.facts("claim")[0]["truth"] is True
    finally:
        await link_cm.__aexit__(None, None, None)


CAVALORN, BRAGO = "BAU_4300_ADDON_CAVALORN", "BDT_1013_BANDIT_L"
KILLBRAGO = "MIS_ADDON_CAVALORN_KILLBRAGO"
BANDITEN, HELFEN, KILLED = (
    "DIA_ADDON_CAVALORN_BANDITEN",
    "DIA_ADDON_CAVALORN_HELFEN",
    "DIA_ADDON_CAVALORN_PCKILLEDBRAGO",
)
PARTY = {"do": "set_field", "field": "AIVAR", "of": ["inst", "SELF"], "index": ["int", 15, "AIV_PARTYMEMBER"]}
VATRAS = "Отнеси его к Ватрасу, магу воды, в город."


def _line(text: str, speaker: str = "SELF") -> dict:
    other = "OTHER" if speaker == "SELF" else "SELF"
    return {"do": "line", "id": "X", "text": text, "speaker": ["inst", speaker], "listener": ["inst", other]}


@pytest.fixture
def cavalorn(tmp_path: Path) -> StoryGraph:
    """Cavalorn's bandits, cut down: he tells of them (BANDITEN), the hero offers his help (HELFEN: the quest
    runs, Cavalorn goes with him) or kills them unasked and says so (PCKILLEDBRAGO, the ending that the
    converter has from B_Addon_Cavalorn_VatrasBrief: the letter to Vatras)."""
    g = copy.deepcopy(GRAPH)
    knows = ["call", "NPC_KNOWSINFO", ["inst", "OTHER"], ["sym", BANDITEN]]
    bandits_dead = ["==", ["fn", "C_BRAGOBANDITSDEAD"], ["int", 1, "TRUE"]]
    g["nodes"] += [
        _node(BANDITEN, CAVALORN, [_line("Бандиты украли у меня кое-что ценное.")], description="Бандиты?"),
        _node(
            HELFEN,
            CAVALORN,
            [
                _line("Могу я помочь тебе с бандитами?", "OTHER"),
                _line("Почему бы и нет. Пошли."),
                {"do": "set", "var": KILLBRAGO, "value": ["int", 1, "LOG_RUNNING"]},
                {
                    "do": "journal_status",
                    "topic": "TOPIC_ADDON_KILLBRAGO",
                    "status": ["int", 1, "LOG_RUNNING"],
                },
                {
                    "do": "journal_entry",
                    "topic": "TOPIC_ADDON_KILLBRAGO",
                    "text": "Бандиты украли у Кавалорна.",
                },
                {**PARTY, "value": ["int", 1, "TRUE"]},
            ],
            condition=["&&", knows, ["==", ["var", KILLBRAGO], ["int", 0]]],
            description="Могу я помочь тебе с бандитами?",
        ),
        _node(
            KILLED,
            CAVALORN,
            [
                _line("С бандитами покончено.", "OTHER"),
                _line(VATRAS),
                {"do": "set", "var": KILLBRAGO, "value": ["int", 2, "LOG_SUCCESS"]},
                {"do": "xp", "amount": ["int", 100, "XP_ADDON_CAVALORN_KILLBRAGO"]},
                {**PARTY, "value": ["int", 0, "FALSE"]},
            ],
            condition=["&&", knows, ["==", ["var", KILLBRAGO], ["int", 0]], bandits_dead],
            description="С бандитами покончено.",
        ),
    ]
    g["functions"]["C_BRAGOBANDITSDEAD"] = {
        "params": 0,
        "body": ["call", "NPC_ISDEAD", ["inst", "AMBUSHER_1013"]],
        "lifted": True,
    }
    g["instances"]["AMBUSHER_1013"] = BRAGO
    g["quests"].append(
        {
            "var": KILLBRAGO,
            "topics": [{"topic": "TOPIC_ADDON_KILLBRAGO", "title": "Бандиты Кавалорна"}],
            "running": [HELFEN],
            "success": [KILLED],
            "failed": [],
            "obsolete": [],
            "other": [],
        }
    )
    path = tmp_path / "story.json"
    path.write_text(json.dumps(g, ensure_ascii=False), encoding="utf-8")
    return load(path)


async def _cavalorn(fake: FakeGame, llm: FakeBackend, graph: StoryGraph):
    fake.vars = {KILLBRAGO: 0, "KAPITEL": 1}
    fake.npc_states = {
        CAVALORN: {"state": "alive", "name": "Кавалорн"},
        BRAGO: {"state": "alive", "name": "Браго"},
    }
    fake.instances = {"AMBUSHER_1013": BRAGO}
    link_cm = connected(fake)
    link = await link_cm.__aenter__()
    store = MemoryStore(":memory:")
    dialogue = DialogueManager(link, llm, PersonaProvider(None), memory=store)
    chronicle = Chronicle(link, store)
    vanilla = VanillaQuests(link, dialogue, chronicle, graph, hero_distance=lambda npc: 300.0)
    await _until(lambda: chronicle.branch is not None)
    return link_cm, dialogue, chronicle, vanilla


async def test_a_quest_of_the_scripts_given_in_a_conversation(cavalorn):
    # the hero offers Cavalorn his help in his own words; Cavalorn gives the quest as HELFEN does: it runs,
    # the journal has it, the dialog counts as heard, he is in the hero's party
    fake = FakeGame(branch="A")
    give = FakeStep("Ладно, пошли.", [ToolCall("give_quest", {"quest": KILLBRAGO}, "g1")])
    llm = FakeBackend(script=[give, FakeStep("Ну?")], delay=0)
    link_cm, dialogue, chronicle, vanilla = await _cavalorn(fake, llm, cavalorn)
    try:
        dialogue.open(CAVALORN, "Кавалорн")
        await vanilla.prepare(CAVALORN)
        situation = vanilla.situation(CAVALORN) or ""
        # what he told of the bandits in the conversation counts: BANDITEN, a dialog of his own
        assert f"Ты можешь сам дать герою поручение «Бандиты Кавалорна» (id {KILLBRAGO})" in situation
        assert "«Могу я помочь тебе с бандитами?»" in situation and "«Почему бы и нет. Пошли.»" in situation
        assert "give_quest" in [t.name for t in vanilla.tools(CAVALORN)]
        # he may also say he killed them unasked: Brago lives, far from Cavalorn's eyes
        assert "Герой может сказать, что сам сделал «Бандиты Кавалорна»" in situation

        await dialogue.say_as_player(CAVALORN, "Давай я помогу тебе с бандитами.")
        await _until(lambda: chronicle.facts("quest_given") != [])
        effects = _effects(fake, CAVALORN)[0]
        assert {"do": "set", "var": KILLBRAGO, "value": 1} in effects
        assert {
            "do": "journal",
            "topic": "Бандиты Кавалорна",
            "text": "Бандиты украли у Кавалорна.",
        } in effects
        assert {"do": "knows", "info": HELFEN} in effects and HELFEN in fake.known
        assert {"do": "party", "value": 1} in effects  # he goes with the hero, as the scripts mark it
        # given: not offered again; and the ending for the unasked is not one any more
        fake.vars[KILLBRAGO] = 1
        await vanilla.prepare(CAVALORN)
        assert vanilla.starts[CAVALORN] == {} and vanilla.offers[CAVALORN] == {}
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_an_ending_that_wants_its_quest_not_begun(cavalorn):
    # the hero killed Brago unasked: PCKILLEDBRAGO wants the quest not begun, and Brago dead (the scripts'
    # C_BragoBanditsDead, taken apart: who must be dead); Cavalorn knows what to say, the letter to Vatras
    fake = FakeGame(branch="A")
    accept = FakeStep("Ну, раз так.", [ToolCall("accept_quest", {"quest": KILLBRAGO}, "a1")])
    llm = FakeBackend(script=[accept, FakeStep("Ну?")], delay=0)
    link_cm, dialogue, chronicle, vanilla = await _cavalorn(fake, llm, cavalorn)
    try:
        fake.npc_states[BRAGO]["state"] = "dead"
        fake.fns["C_BRAGOBANDITSDEAD"] = 1
        dialogue.open(CAVALORN, "Кавалорн")
        await vanilla.prepare(CAVALORN)
        offer = vanilla.offers[CAVALORN][KILLBRAGO]
        assert offer.ending == KILLED and offer.claim.dead == [BRAGO] and offer.claim.own == [BANDITEN]
        situation = vanilla.situation(CAVALORN) or ""
        assert "будут мертвы: Браго" in situation and "Засчитывая, в игре ты говоришь" in situation
        assert f"«{VATRAS}»" in situation

        await dialogue.say_as_player(CAVALORN, "Браго мёртв, я сам с ними разобрался.")
        await _until(lambda: chronicle.facts("claim") != [])
        effects = _effects(fake, CAVALORN)[0]
        assert {"do": "set", "var": KILLBRAGO, "value": 2} in effects and {
            "do": "xp",
            "amount": 100,
        } in effects
        assert {"do": "knows", "info": KILLED} in effects and {"do": "party", "value": 0} in effects
        # what he told of the bandits only in the conversation is no lie of the hero's
        assert chronicle.facts("claim")[-1]["truth"] is True and chronicle.facts("lie") == []
    finally:
        await link_cm.__aexit__(None, None, None)


def test_bridge_translates_native_gate_fields_and_checklog(graph):
    passage = {
        "do": "set_field", "field": "AIVAR", "of": ["inst", "SELF"],
        "index": ["int", 14, "AIV_PASSGATE"], "value": ["int", 1, "TRUE"],
    }
    assert _bridge(passage, {}, graph) == {"do": "gate_pass", "npc": "SELF", "value": 1}
    assert _bridge({**passage, "of": ["inst", "HILDA"]}, {}, graph) == {
        "do": "gate_pass", "npc": HILDA, "value": 1,
    }
    assert _bridge({**passage, "value": ["int", 0, "FALSE"]}, {}, graph) == {
        "do": "gate_pass", "npc": "SELF", "value": 0,
    }
    assert _bridge({**passage, "field": "ATTRIBUTE"}, {}, graph) is None
    assert _bridge({**passage, "index": ["int", 13, "AIV_LASTDISTTOWP"]}, {}, graph) is None
    assert _bridge({"do": "call", "fn": "B_CHECKLOG", "args": []}, {}, graph) == {
        "do": "gate_checklog",
    }


@pytest.fixture(scope="module")
def real_graph():
    data = Path(os.environ.get("GOTHIC_AI_DATA", Path.home() / ".local/share/gothic-ai"))
    path = Path(os.environ.get("GOTHIC_STORY_GRAPH", data / "story.json"))
    if not path.exists():
        pytest.skip("needs actual storygraph export (GOTHIC_STORY_GRAPH)")
    return load(path)


async def test_constantino_herbs_accept_quest_keeps_native_passage_and_checklog(real_graph):
    from tests.test_gates import GatesFake

    graph = real_graph
    npc, partner = "MIL_310_STADTWACHE", "MIL_333_STADTWACHE"
    quest, ending = "MIS_ADDON_LESTER_PICKFORCONSTANTINO", "DIA_ADDON_MIL_310_STADTWACHE_CONSTANTINO"
    fake = GatesFake(branch="A")
    accept = FakeStep("Ладно, проходи к Константино.", [ToolCall("accept_quest", {"quest": quest}, "a1")])
    llm = FakeBackend(script=[accept, FakeStep("Ну?")], delay=0)
    store = MemoryStore(":memory:")
    link_cm, dialogue, chronicle, vanilla = await _setup(fake, llm, graph, store)
    fake.vars.update({quest: 1, "MIL_310_SCHONMALREINGELASSEN": 0})
    fake.hero_items = {"ITPL_MANA_HERB_01": 10}
    fake.npc_states.update({npc: {"state": "alive", "name": "Стражник"}, partner: {"state": "alive"}})
    try:
        dialogue.open(npc, "Стражник")
        await vanilla.prepare(npc)
        assert vanilla.offers[npc][quest].ending == ending
        await dialogue.say_as_player(npc, "Я принёс травы Константино.")
        await _until(lambda: chronicle.facts("claim") != [])
        assert fake.vars[quest] == 2 and fake.vars["MIL_310_SCHONMALREINGELASSEN"] == 1
        assert fake.fields.get((npc, "AIVAR", 14)) == 1
        assert fake.fields.get((partner, "AIVAR", 14)) == 1
        assert fake.hero_xp == 100 and ending in fake.known
        assert len(fake.checklog) == 1
        assert chronicle.facts("claim")[-1]["truth"] is True
    finally:
        await link_cm.__aexit__(None, None, None)


GREG = "PIR_1300_ADDON_GREG_NW"
GREG_QUEST = "MIS_ADDON_GREG_BRINGMETOTHECITY"
GREG_BEGIN = "DIA_ADDON_GREG_NW_HALLO_JA"
GREG_PARENT = "DIA_ADDON_GREG_NW_STADTWACHEN"
GREG_END = f"{GREG_PARENT}_KLAMOTTEN"
GREG_TOPIC = "TOPIC_ADDON_GREG_NW"
GREG_TITLE = "Человек с повязкой на глазу"
GREG_BONUS = {"ITWR_MAP_NEWWORLD": 1, "ITFO_ADDON_RUM": 2}


def _has(item):
    return ["call", "NPC_HASITEMS", ["inst", "OTHER"], ["sym", item]]


def test_a_missing_fetch_item_is_not_replaced_by_an_unfulfilled_variable(graph):
    # Pyrokar's eye: without the eye or the prior conversation, retain the fetch claim (and its lie).
    condition = [
        "||",
        _has("ITMI_INNOSEYE_BROKEN_MIS"),
        ["==", ["var", "MIS_SCKNOWSINNOSEYEISBROKEN"], ["int", 1]],
    ]
    assert _claim(None, condition, graph, have={}).items == {"ITMI_INNOSEYE_BROKEN_MIS": 1}


@pytest.fixture
def greg(tmp_path: Path) -> StoryGraph:
    """Greg's clothing ending: the item requirement is on the parent's choice, not the ending itself."""
    g = copy.deepcopy(GRAPH)
    g["nodes"] += [
        _node(
            "DIA_ADDON_GREG_NW_HALLO",
            GREG,
            [{"do": "choice", "node": GREG_BEGIN, "text": "Почему бы и нет?"}],
            condition=["==", ["var", "GREGLOCATION"], ["int", 0]],
        ),
        _node(
            GREG_BEGIN,
            GREG,
            [
                _line("Ты же поможешь мне пройти мимо стражников, правда?"),
                {"do": "set", "var": GREG_QUEST, "value": ["int", 1, "LOG_RUNNING"]},
            ],
            kind="choice",
            parents=["DIA_ADDON_GREG_NW_HALLO"],
            writes=[GREG_QUEST],
            description="Почему бы и нет?",
        ),
        _node(
            GREG_PARENT,
            GREG,
            [
                {
                    "do": "choice",
                    "node": GREG_END,
                    "text": "Они пропустят тебя в одежде крестьянина.",
                    "when": ["||", _has("ITAR_BAU_L"), _has("ITAR_BAU_M")],
                }
            ],
            condition=[
                "&&",
                ["==", ["var", "GREGLOCATION"], ["int", 0]],
                ["==", ["var", GREG_QUEST], ["int", 1, "LOG_RUNNING"]],
            ],
        ),
        _node(
            GREG_END,
            GREG,
            [
                {
                    "do": "give",
                    "from": ["inst", "OTHER"],
                    "to": ["inst", "SELF"],
                    "item": ["sym", "ITAR_BAU_L"],
                    "amount": ["int", 1],
                    "when": _has("ITAR_BAU_L"),
                },
                {
                    "do": "give",
                    "from": ["inst", "OTHER"],
                    "to": ["inst", "SELF"],
                    "item": ["sym", "ITAR_BAU_M"],
                    "amount": ["int", 1],
                    "when": ["!", _has("ITAR_BAU_L")],
                },
                _line("Отлично! Вот твоя награда."),
                {
                    "do": "create_items",
                    "npc": ["inst", "SELF"],
                    "item": ["sym", "ITMI_GOLD"],
                    "amount": ["int", 50],
                },
                {
                    "do": "give",
                    "from": ["inst", "SELF"],
                    "to": ["inst", "OTHER"],
                    "item": ["sym", "ITMI_GOLD"],
                    "amount": ["int", 50],
                },
                {"do": "xp", "amount": ["int", 100, "XP_GREG_NW_GIVEBAUARMOR"]},
                {"do": "set", "var": GREG_QUEST, "value": ["int", 2, "LOG_SUCCESS"]},
            ],
            kind="choice",
            parents=[GREG_PARENT],
            writes=[GREG_QUEST],
        ),
    ]
    g["quests"].append(
        {
            "var": GREG_QUEST,
            "topics": [{"topic": GREG_TOPIC, "title": GREG_TITLE}],
            "running": [GREG_BEGIN],
            "success": [GREG_END],
            "failed": [],
            "obsolete": [],
            "other": [],
        }
    )
    path = tmp_path / "greg-story.json"
    path.write_text(json.dumps(g, ensure_ascii=False), encoding="utf-8")
    return load(path)


async def _greg(fake: FakeGame, llm: FakeBackend, graph: StoryGraph, *, running=0):
    fake.vars = {GREG_QUEST: running, "GREGLOCATION": 0, "KAPITEL": 1}
    fake.npc_states = {GREG: {"state": "alive", "name": "Грег"}}
    fake.item_names = {
        "ITMI_GOLD": "Золото",
        "ITAR_BAU_L": "Одежда крестьянина",
        "ITAR_BAU_M": "Одежда фермера",
        "ITWR_MAP_NEWWORLD": "Карта земель Хориниса",
        "ITFO_ADDON_RUM": "Ром",
    }
    link_cm = connected(fake)
    link = await link_cm.__aenter__()
    store = MemoryStore(":memory:")
    dialogue = DialogueManager(link, llm, PersonaProvider(None), memory=store)
    chronicle = Chronicle(link, store)
    vanilla = VanillaQuests(link, dialogue, chronicle, graph, hero_distance=lambda npc: 300.0)
    await _until(lambda: chronicle.branch is not None)
    return link_cm, dialogue, chronicle, vanilla


async def test_greg_knows_the_whole_reward_before_giving_the_quest(greg):
    fake = FakeGame(branch="A")
    give = FakeStep("Помоги мне, и я заплачу.", [ToolCall("give_quest", {"quest": GREG_QUEST}, "g1")])
    llm = FakeBackend(script=[give, FakeStep("Договорились.")], delay=0)
    link_cm, dialogue, chronicle, vanilla = await _greg(fake, llm, greg)
    try:
        dialogue.open(GREG, "Грег")
        await vanilla.prepare(GREG)
        before = vanilla.situation(GREG) or ""
        assert f"Ты можешь сам дать герою поручение «{GREG_TITLE}»" in before
        for reward in ("50 золотых", "опыт +100", "Карта земель Хориниса", "Ром ×2"):
            assert reward in before
        assert "give_quest" in [t.name for t in vanilla.tools(GREG)]

        await dialogue.say_as_player(GREG, "Как я могу тебе помочь?")
        await _until(lambda: chronicle.facts("quest_given") != [])
        assert {"do": "set", "var": GREG_QUEST, "value": 1} in _effects(fake, GREG)[0]
        fake.vars[GREG_QUEST] = 1  # FakeGame keeps unchecked script effects as messages only.
        await vanilla.prepare(GREG)
        running = vanilla.situation(GREG) or ""
        assert f"Ты ждёшь от героя: «{GREG_TITLE}»" in running
        assert vanilla.starts[GREG] == {}
        for reward in ("50 золотых", "опыт +100", "Карта земель Хориниса", "Ром ×2"):
            assert reward in running
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize("armor", ["ITAR_BAU_L", "ITAR_BAU_M"])
async def test_gregs_parent_choice_requires_and_takes_the_clothing_the_hero_has(greg, armor):
    fake = FakeGame(branch="A")
    fake.hero_items = {armor: 1}
    accept = FakeStep("Держи плату.", [ToolCall("accept_quest", {"quest": GREG_QUEST}, "a1")])
    llm = FakeBackend(script=[accept, FakeStep("Прощай.")], delay=0)
    link_cm, dialogue, chronicle, vanilla = await _greg(fake, llm, greg, running=1)
    try:
        ending = greg.nodes[GREG_END]
        condition = vanilla._condition(ending)
        claim = _claim(None, condition, greg, GREG, have={armor: 1})
        assert claim.items == {armor: 1}
        dialogue.open(GREG, "Грег")
        await vanilla.prepare(GREG)
        offer = vanilla.offers[GREG][GREG_QUEST]
        assert offer.claim.items == {armor: 1}
        assert offer.seen_true()

        await dialogue.say_as_player(GREG, "Вот одежда крестьянина, в ней тебя пропустят.")
        await _until(lambda: chronicle.facts("vanilla_reward") != [])
        assert chronicle.facts("claim")[0]["truth"] is True
        assert chronicle.facts("lie") == []
        effects = _effects(fake, GREG)[0]
        assert {"do": "give", "item": armor, "count": 1, "to": "self"} in effects
        assert not any(e.get("to") == "self" and e.get("item") != armor for e in effects)
        assert {"do": "give", "item": "ITMI_GOLD", "count": 50} in effects
        assert {"do": "xp", "amount": 100} in effects
        assert {"do": "set", "var": GREG_QUEST, "value": 2} in effects
        assert {i: fake.hero_items[i] for i in GREG_BONUS} == GREG_BONUS
        assert chronicle.facts("vanilla_reward")[0]["quest"] == GREG_QUEST

        # QuestLog can observe this same success after accept_quest already paid it.
        chronicle.record({"fact": "vanilla_quest", "quest": GREG_QUEST, "to": "success", "giver": GREG})
        await _until(lambda: not vanilla._background)
        assert len(chronicle.facts("vanilla_reward")) == 1
        assert {i: fake.hero_items[i] for i in GREG_BONUS} == GREG_BONUS
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_gregs_missing_clothing_is_a_fetch_lie_with_the_truth_recorded(greg):
    fake = FakeGame(branch="A")
    accept = FakeStep("Ладно, верю.", [ToolCall("accept_quest", {"quest": GREG_QUEST}, "a1")])
    llm = FakeBackend(script=[accept, FakeStep("Прощай.")], delay=0)
    link_cm, dialogue, chronicle, vanilla = await _greg(fake, llm, greg, running=1)
    try:
        dialogue.open(GREG, "Грег")
        await vanilla.prepare(GREG)
        offer = vanilla.offers[GREG][GREG_QUEST]
        assert offer.claim.kind == "fetch"
        assert list(offer.claim.items.values()) == [1]
        assert set(offer.claim.items) <= {"ITAR_BAU_L", "ITAR_BAU_M"}
        assert not offer.seen_true()
        await dialogue.say_as_player(GREG, "Одежду я оставил у дороги. Плати сейчас.")
        await _until(lambda: chronicle.facts("lie") != [])
        assert chronicle.facts("claim")[0]["truth"] is False
        lie = chronicle.facts("lie")[0]
        assert lie["kind"] == "fetch" and "0 of 1" in lie["truth"]
        assert not any(e.get("to") == "self" for e in _effects(fake, GREG)[0])
        assert vanilla.told[GREG_QUEST].lie
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_gregs_native_success_bonus_is_once_and_follows_the_save_branch(greg):
    fake = FakeGame(branch="A")
    link_cm, _, chronicle, vanilla = await _greg(fake, FakeBackend(delay=0), greg, running=1)
    questlog = QuestLog(vanilla.link, greg, chronicle, settle_s=0)
    try:
        await _until(lambda: GREG_QUEST in questlog.states)
        before_reward = chronicle.memory.head("A")
        fake.vars[GREG_QUEST] = 2
        # The native menu reports its parent dialog, whose choice writes success.
        await fake.send(type="dialog_chosen", npc=GREG, node=GREG_PARENT, made=False)
        await _until(lambda: chronicle.facts("vanilla_reward") != [])
        transition = chronicle.facts("vanilla_quest")[0]
        assert (transition["quest"], transition["node"], transition["giver"]) == (
            GREG_QUEST,
            GREG_PARENT,
            GREG,
        )
        assert {i: fake.hero_items[i] for i in GREG_BONUS} == GREG_BONUS
        bonus_effects = [
            m for m in fake.story if m["type"] == "story_effect" and m.get("check") and m["npc"] == GREG
        ]
        assert len(bonus_effects) == 1
        for item, count in GREG_BONUS.items():
            assert {"do": "create", "item": item, "count": count} in bonus_effects[0]["effects"]
            assert {"do": "give", "item": item, "count": count, "to": "hero"} in bonus_effects[0]["effects"]
        after_reward = chronicle.memory.head("A")

        async def succeed_again():
            fake.vars[GREG_QUEST] = 1
            await questlog.poll()
            fake.vars[GREG_QUEST] = 2
            await questlog.poll()
            await _until(lambda: not vanilla._background)

        await succeed_again()
        assert len(chronicle.facts("vanilla_reward")) == 1
        assert {i: fake.hero_items[i] for i in GREG_BONUS} == GREG_BONUS

        async def load_branch(branch, head):
            fake.vars[GREG_QUEST] = 1
            await fake.send(type="loading")
            await fake.send(type="session", branch=branch, parent="A", parent_seq=head, reason="load")
            await vanilla.link.evaluate(["int", 1])
            await _until(lambda: chronicle.branch == branch and GREG_QUEST in questlog.states)

        await load_branch("after-reward", after_reward)
        await succeed_again()
        assert len(chronicle.facts("vanilla_reward")) == 1
        assert {i: fake.hero_items[i] for i in GREG_BONUS} == GREG_BONUS

        fake.hero_items.clear()  # Loading a save from before payment restores the hero's inventory too.
        await load_branch("before-reward", before_reward)
        assert chronicle.facts("vanilla_reward") == []
        await succeed_again()
        assert len(chronicle.facts("vanilla_reward")) == 1
        assert {i: fake.hero_items[i] for i in GREG_BONUS} == GREG_BONUS
    finally:
        await questlog.close()
        await link_cm.__aexit__(None, None, None)
