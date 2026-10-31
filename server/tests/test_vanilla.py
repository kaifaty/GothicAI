import asyncio
import copy
import json
import os
from pathlib import Path

import pytest

from gothic_ai.chronicle import Chronicle
from gothic_ai.dialogue import DialogueManager
from gothic_ai.game import GameError
from gothic_ai.gamedata.storygraph import StoryGraph, load
from gothic_ai.journal import Journal
from gothic_ai.knowledge import Partial, StoryState
from gothic_ai.llm import ToolCall
from gothic_ai.llm.fake import FakeBackend, FakeStep
from gothic_ai.memory import MemoryStore
from gothic_ai.persona import PersonaProvider
from gothic_ai.questlog import QuestLog
from gothic_ai.story import freed
from gothic_ai.trust import WORD_BROKEN, Record
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


@pytest.mark.parametrize("tool", ["accept_quest", "amend_quest", "drop_quest", "check_claim", "expose_lie"])
@pytest.mark.parametrize("checking", [False, True])
async def test_foreign_quest_ids_are_left_to_the_next_extension(graph, tool, checking):
    fake = FakeGame(branch="A")
    link_cm, dialogue, _, vanilla = await _setup(fake, FakeBackend(delay=0), graph, MemoryStore(":memory:"))
    try:
        conv = dialogue.open(LOBART, "Лобарт")
        if checking:
            fake.vars[KILL] = 1
            await vanilla.prepare(LOBART)
            vanilla.checking[LOBART] = vanilla.offers[LOBART].pop(KILL)
        assert await vanilla.handle(dialogue, conv, ToolCall(tool, {"quest": "DIR_Q1"}, "c1"), None) is None
    finally:
        await link_cm.__aexit__(None, None, None)


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
        assert (
            "Можешь снять пункты (amend_quest): ITPL_BEET — Репа (у героя с собой ×0 из 20)."
        ) in situation
        # he sees the hero has none on him: he may call it a lie to his face; nothing to go and check
        assert {t.name for t in vanilla.tools(LOBART)} == {
            "accept_quest", "expose_lie", "amend_quest", "drop_quest"
        }

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
        # The watch is sent after the tag's answer.
        await _until(lambda: any(m["type"] == "story_watch" for m in fake.story))
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
    expose = FakeStep("Врёшь! Вон он стоит.", [ToolCall("expose_lie", {"quest": KILL}, "c2")])
    llm = FakeBackend(script=[expose, FakeStep("Пошёл вон.")], delay=0)
    link_cm, dialogue, chronicle, vanilla = await _setup(fake, llm, graph, store)
    fake.vars.update({QUEST: 2, KILL: 1})
    fake.npc_states[BANDIT] = {"state": "alive", "name": "Бандит", "distance": 1200}
    fake.people_near = [{"npc": HILDA, "name": "Хильда", "distance": 400, "guild": 0}]
    try:
        conv = dialogue.open(LOBART, "Лобарт")
        await vanilla.prepare(LOBART)
        situation = vanilla.situation(LOBART)
        assert "у тебя на глазах: Бандит — жив" in situation
        assert (
            "Засчитать сейчас нельзя: Бандит жив — у тебя на глазах, в 12 м. "
            "Не говори, что отдаёшь награду или рассчитываешься, пока дело не засчитано. "
            "Можешь уличить его во лжи (expose_lie)."
        ) in situation
        assert {t.name for t in vanilla.tools(LOBART)} == {
            "expose_lie", "amend_quest", "drop_quest"
        }
        result = await vanilla.handle(
            dialogue, conv, ToolCall("accept_quest", {"quest": KILL}, "c1"), None
        )
        assert result.status == "rejected" and result.output == (
            "Отклонено: Бандит жив — у тебя на глазах, в 12 м. "
            "Своим глазам ты веришь больше, чем его словам."
        )

        await dialogue.say_as_player(LOBART, "Бандит мёртв, я его убил.")
        await _until(lambda: chronicle.facts("caught") != [])
        assert "accept_quest" not in {t.name for t in llm.calls[0][2]}
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
        assert {t.name for t in vanilla.tools(LOBART)} == {
            "accept_quest", "check_claim", "expose_lie", "amend_quest", "drop_quest"
        }

        await dialogue.say_as_player(LOBART, "Бандит мёртв.")
        await _until(lambda: chronicle.facts("caught") != [])
        await _until(lambda: LOBART not in vanilla.checking)
        go = [m["args"] for m in fake.received if m["type"] == "action" and m["action"] == "go_to_npc"]
        # there, to see; back to the hero, to say it to his face
        assert go == [
            {"npc": BANDIT, "radius": 1800, "timeout_ms": 120_000},
            {"npc": "PLAYER", "radius": 300, "timeout_ms": 120_000},
        ]
        assert "сходил проверить и увидел: Бандит жив, в 18 м" in chronicle.facts("caught")[0]["how"]
        await _until(lambda: len(llm.calls) >= 2)  # he says what he thinks of it
        assert "Он тебе солгал" in str(llm.calls[-1][1])
        assert chronicle.facts("claim") == []
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize("action", ["follow", "stop"])
async def test_a_check_replaced_by_a_new_action_does_not_resume_the_walk(graph, monkeypatch, action):
    fake = FakeGame(branch="A")
    fake.slow_actions = {"go_to_npc"}
    store = MemoryStore(":memory:")
    link_cm, dialogue, chronicle, vanilla = await _setup(fake, FakeBackend(delay=0), graph, store)
    fake.vars[KILL] = 1
    fake.npc_states[BANDIT] = {"state": "dead", "name": "Бандит", "distance": 4000, "seen": False}
    returned = []
    dialogue.free_body = lambda npc, why: returned.append((npc, why))
    try:
        await vanilla.link.attach(LOBART)
        conv = dialogue.open(LOBART, "Лобарт")
        await vanilla.prepare(LOBART)
        offer = vanilla.offers[LOBART][KILL]
        target = offer.reachable()

        async def evidence(*args):
            pytest.fail("A cancelled check must not inspect evidence")

        monkeypatch.setattr(vanilla, "_evidence", evidence)
        check = asyncio.create_task(vanilla._check(offer, target, conv))
        await _until(lambda: LOBART in fake.long_running)
        # Even if the body became visible, a new order has taken over the walk.
        fake.npc_states[BANDIT].update(distance=1000, seen=True)
        args = {"npc": "PLAYER"} if action == "follow" else {}
        await vanilla.link.act(LOBART, action, **args)
        note, ok = await check

        assert (note, ok) == ("Ты не дошёл проверить: герой позвал тебя назад.", False)
        actions = [m for m in fake.received if m["type"] == "action"]
        assert [(m["action"], m["args"].get("npc")) for m in actions] == [
            ("go_to_npc", BANDIT), (action, args.get("npc")),
        ]
        assert fake.vars[KILL] == 1 and _effects(fake, LOBART) == []
        assert all(chronicle.facts(fact) == [] for fact in ("claim", "caught", "lie"))
        assert returned == [(LOBART, "check")]
        assert LOBART not in vanilla.checking
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_a_check_in_progress_cannot_start_another_walk(graph):
    fake = FakeGame(branch="A")
    fake.slow_actions = {"go_to_npc"}
    store = MemoryStore(":memory:")
    link_cm, dialogue, _, vanilla = await _setup(fake, FakeBackend(delay=0), graph, store)
    fake.vars[KILL] = 1
    fake.npc_states[BANDIT] = {"state": "dead", "name": "Бандит", "distance": 4000, "seen": False}
    try:
        await vanilla.link.attach(LOBART)
        conv = dialogue.open(LOBART, "Лобарт")
        await vanilla.prepare(LOBART)
        assert "check_claim" in [t.name for t in vanilla.tools(LOBART)]

        first = await vanilla.handle(dialogue, conv, ToolCall("check_claim", {"quest": KILL}, "c1"), None)
        assert first.status == "accepted"
        await _until(lambda: LOBART in fake.long_running)
        await vanilla.prepare(LOBART)  # another turn refreshes the offers while the body is walking
        assert "check_claim" not in [t.name for t in vanilla.tools(LOBART)]

        second = await vanilla.handle(dialogue, conv, ToolCall("check_claim", {"quest": KILL}, "c2"), None)
        assert (second.output, second.status) == (
            "Отклонено: ты уже идёшь посмотреть сам; что увидишь, то и решит.",
            "rejected",
        )
        go = [m for m in fake.received if m["type"] == "action" and m["action"] == "go_to_npc"]
        assert len(go) == 1
        assert go[0]["args"] == {"npc": BANDIT, "radius": 1800, "timeout_ms": 120_000}

        await fake.send(
            type="action_result",
            id=fake.long_running.pop(LOBART),
            npc=LOBART,
            status="timeout",
            detail="stuck behind a wall",
            distance=4000,
        )
        await _until(lambda: not conv.actions)
        assert LOBART not in vanilla.checking
        await vanilla.prepare(LOBART)
        assert "check_claim" in [t.name for t in vanilla.tools(LOBART)]
        assert len(
            [m for m in fake.received if m["type"] == "action" and m["action"] == "go_to_npc"]
        ) == 1
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


async def test_a_check_in_progress_is_not_offered_or_started_again(graph):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    check = FakeStep("Пойду проверю.", [ToolCall("check_claim", {"quest": KILL}, "c1")])
    llm = FakeBackend(script=[check, FakeStep("Жди, я ещё не посмотрел.")], delay=0)
    link_cm, dialogue, chronicle, vanilla = await _setup(fake, llm, graph, store)
    fake.vars.update({QUEST: 2, KILL: 1})
    fake.npc_states[BANDIT] = {
        "state": "dead", "name": "Бандит", "distance": 4000, "seen": False
    }
    fake.slow_actions = {"go_to_npc"}
    try:
        await vanilla.link.attach(LOBART)
        conv = dialogue.open(LOBART, "Лобарт")
        await vanilla.prepare(LOBART)
        await dialogue.say_as_player(LOBART, "Бандит мёртв.")
        await _until(lambda: LOBART in fake.long_running)
        offer = vanilla.checking[LOBART]
        assert offer.quest == KILL

        await vanilla.prepare(LOBART)
        assert KILL not in vanilla.offers[LOBART]
        assert vanilla.checking[LOBART] is offer
        assert {t.name for t in vanilla.tools(LOBART)}.isdisjoint({"accept_quest", "check_claim"})
        assert vanilla.situation(LOBART) == (
            "Ты пошёл сам посмотреть, сделал ли герой «Бандит у фермы»: что увидишь, то и решит. "
            "Пока не вернёшься, не засчитывай дело и не говори, что отдаёшь награду."
        )
        for tool in ("check_claim", "accept_quest"):
            result = await vanilla.handle(
                dialogue, conv, ToolCall(tool, {"quest": KILL}, "again"), None
            )
            assert result.status == "rejected" and result.output == (
                "Отклонено: ты уже идёшь посмотреть сам; что увидишь, то и решит."
            )

        await dialogue.say_as_player(LOBART, "Ну что там?")
        await _until(lambda: len(llm.calls) >= 2)
        assert {t.name for t in llm.calls[-1][2]}.isdisjoint({"accept_quest", "check_claim"})
        assert [
            m["args"]["npc"]
            for m in fake.received
            if m["type"] == "action" and m["action"] == "go_to_npc"
        ] == [BANDIT]
        assert chronicle.facts("claim") == []

        fake.slow_actions.clear()
        fake.npc_states[BANDIT].update(distance=1800, seen=True)
        action = fake.long_running.pop(LOBART)
        await fake.send(type="action_result", id=action, npc=LOBART, status="done")
        await _until(lambda: chronicle.facts("claim") != [])
        await _until(lambda: LOBART not in vanilla.checking)
        assert chronicle.facts("claim")[0]["checked"] is True
        assert chronicle.facts("caught") == []
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize("distrust", [False, True])
async def test_a_check_in_progress_blocks_another_check_but_not_another_acceptance(graph, distrust):
    other_quest = "MIS_KILLSECONDBANDIT"
    other_npc = "BDT_2_BANDIT"
    ending = copy.deepcopy(graph.nodes["DIA_LOBART_BANDITDEAD"])
    ending.id = "DIA_LOBART_SECONDBANDITDEAD"
    ending.condition = ["==", ["var", other_quest], ["int", 1]]
    ending.effects[0]["var"] = other_quest
    for effect in ending.effects:
        effect["when"] = ["call", "NPC_ISDEAD", ["inst", "SECOND_BANDIT"]]
    graph.nodes[ending.id] = ending
    quest = copy.deepcopy(graph.quests[KILL])
    quest.var = other_quest
    quest.topics = {"TOPIC_SECOND_BANDIT": "Второй бандит у фермы"}
    quest.success = {ending.id}
    graph.quests[other_quest] = quest
    graph.instances["SECOND_BANDIT"] = other_npc

    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    check = FakeStep("Пойду проверю.", [ToolCall("check_claim", {"quest": KILL}, "c1")])
    llm = FakeBackend(script=[check, FakeStep("Правда. Молодец.")], delay=0)
    link_cm, dialogue, chronicle, vanilla = await _setup(fake, llm, graph, store)
    fake.instances["SECOND_BANDIT"] = other_npc
    fake.vars.update({QUEST: 2, KILL: 1, other_quest: 1})
    for npc in (BANDIT, other_npc):
        fake.npc_states[npc] = {
            "state": "dead", "name": "Бандит", "distance": 4000, "seen": False,
        }
    fake.slow_actions = {"go_to_npc"}
    try:
        if distrust:
            chronicle.record({
                "fact": "caught", "quest": KILL, "npc": LOBART,
                "title": "Бандит у фермы", "how": "видел сам",
            })
        await vanilla.link.attach(LOBART)
        conv = dialogue.open(LOBART, "Лобарт")
        await vanilla.prepare(LOBART)
        assert set(vanilla.offers[LOBART]) == {KILL, other_quest}
        assert ("accept_quest" in {t.name for t in vanilla.tools(LOBART)}) is not distrust
        await dialogue.say_as_player(LOBART, "Бандит мёртв.")
        await _until(lambda: LOBART in fake.long_running)
        offer = vanilla.checking[LOBART]
        action = fake.long_running[LOBART]

        await vanilla.prepare(LOBART)
        assert set(vanilla.offers[LOBART]) == {other_quest}
        assert vanilla.offers[LOBART][other_quest].reachable().npc == other_npc
        tools = {t.name for t in vanilla.tools(LOBART)}
        assert "check_claim" not in tools
        assert ("accept_quest" in tools) is not distrust
        situation = vanilla.situation(LOBART)
        assert "(check_claim)" not in situation
        if distrust:
            assert (
                "Засчитать сейчас нельзя: на слово ты ему не веришь, а своими глазами видел не всё."
            ) in situation
        result = await vanilla.handle(
            dialogue, conv, ToolCall("check_claim", {"quest": other_quest}, "again"), None
        )
        assert result.status == "rejected" and result.output == (
            "Отклонено: ты уже идёшь посмотреть сам; что увидишь, то и решит."
        )
        assert vanilla.checking[LOBART] is offer and offer.quest == KILL
        assert [
            m["args"]["npc"]
            for m in fake.received
            if m["type"] == "action" and m["action"] == "go_to_npc"
        ] == [BANDIT]

        if distrust:
            chronicle.record({"fact": "died", "npc": other_npc, "killer": LOBART})
            await _until(lambda: any(f["dead"] == other_npc for f in chronicle.facts("seen_dead")))
            await vanilla.prepare(LOBART)
            assert vanilla.offers[LOBART][other_quest].seen_true()
            assert "accept_quest" in {t.name for t in vanilla.tools(LOBART)}
        result = await vanilla.handle(
            dialogue, conv, ToolCall("accept_quest", {"quest": other_quest}, "accept"), None
        )
        assert result.status == "accepted"
        await _until(lambda: any(f["quest"] == other_quest for f in chronicle.facts("claim")))
        assert vanilla.checking[LOBART] is offer and fake.long_running[LOBART] == action

        fake.slow_actions.clear()
        fake.npc_states[BANDIT].update(distance=1800, seen=True)
        fake.long_running.pop(LOBART)
        await fake.send(type="action_result", id=action, npc=LOBART, status="done")
        await _until(lambda: any(f["quest"] == KILL for f in chronicle.facts("claim")))
        await _until(lambda: LOBART not in vanilla.checking)
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_loading_after_a_check_is_cancelled_before_speech_ends_offers_it_again(graph):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    check = FakeStep("Сам посмотрю.", [ToolCall("check_claim", {"quest": KILL}, "c1")])
    llm = FakeBackend(script=[check], delay=0)
    link_cm, dialogue, chronicle, vanilla = await _setup(fake, llm, graph, store)
    fake.vars.update({QUEST: 2, KILL: 1})
    fake.npc_states[BANDIT] = {
        "state": "dead", "name": "Бандит", "distance": 4000, "seen": False,
    }
    try:
        conv = dialogue.open(LOBART, "Лобарт")
        conv.voice = asyncio.Queue()
        conv.voice.put_nowait(("held", "Сам посмотрю.", None, False, None, False, None))
        conv.voice.get_nowait()  # a line is being voiced: join waits until task_done
        await dialogue.say_as_player(LOBART, "Бандит мёртв.")
        await _until(lambda: bool(conv.actions) and LOBART in vanilla.checking)
        task = next(iter(conv.actions))
        task.cancel()
        await asyncio.gather(task, return_exceptions=True)
        assert task.cancelled()
        assert not any(
            m["type"] == "action" and m["action"] == "go_to_npc" for m in fake.received
        )
        assert chronicle.facts("claim") == []

        vanilla._load()
        await vanilla.prepare(LOBART)
        assert LOBART not in vanilla.checking
        assert KILL in vanilla.offers[LOBART]
        assert "check_claim" in {t.name for t in vanilla.tools(LOBART)}
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_once_lied_to_it_takes_his_word_no_more(graph):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    llm = FakeBackend(
        script=[FakeStep("Не верю."), ACCEPT, FakeStep("Вот это другое дело.")], delay=0
    )
    link_cm, dialogue, chronicle, vanilla = await _setup(fake, llm, graph, store)
    try:
        chronicle.record(
            {"fact": "caught", "quest": KILL, "npc": LOBART, "title": "Бандит у фермы", "how": "видел сам"}
        )
        conv = dialogue.open(LOBART, "Лобарт")
        await vanilla.prepare(LOBART)
        assert "accept_quest" not in {t.name for t in vanilla.tools(LOBART)}
        assert (
            "Засчитать сейчас нельзя: на слово ты ему не веришь, а своими глазами видел не всё. "
            "Не говори, что отдаёшь награду или рассчитываешься, пока дело не засчитано. "
            "Можешь уличить его во лжи (expose_lie)."
        ) in vanilla.situation(LOBART)
        result = await vanilla.handle(
            dialogue, conv, ToolCall("accept_quest", {"quest": QUEST}, "c1"), None
        )
        assert result.status == "rejected"
        assert result.output == f"Отклонено: он уже лгал тебе. {WORD_BROKEN}"
        await dialogue.say_as_player(LOBART, "Репа в амбаре, двадцать штук.")
        await _until(lambda: len(llm.calls) == 1)
        assert "accept_quest" not in {t.name for t in llm.calls[0][2]}
        assert chronicle.facts("claim") == []

        # what it sees for itself it takes: he brings all twenty
        fake.hero_items = {"ITPL_BEET": 20}
        await dialogue.say_as_player(LOBART, "Вот вся репа.")
        await _until(lambda: chronicle.facts("claim") != [])
        assert chronicle.facts("claim")[0]["truth"] is True
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_a_giver_remembers_its_own_kill_before_its_first_turn(graph):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    link_cm, _, chronicle, vanilla = await _setup(fake, FakeBackend(delay=0), graph, store)
    fake.vars[KILL] = 1
    fake.npc_states[BANDIT] = {"state": "dead", "name": "Бандит", "distance": 5000, "seen": False}
    try:
        assert vanilla.offers == {}
        died = {"fact": "died", "npc": BANDIT, "killer": LOBART}
        chronicle.record(died)
        chronicle.record(died)
        await _until(lambda: not vanilla._background)
        assert chronicle.facts("seen_dead") == [
            {"fact": "seen_dead", "npc": LOBART, "dead": BANDIT, "how": "killed"}
        ]
        await vanilla.prepare(LOBART)
        offer = vanilla.offers[LOBART][KILL]
        assert offer.seen_true() and offer.reachable() is None
        assert "Бандит — мёртв: ты убил его сам" in offer.evidence()
        assert "check_claim" not in [t.name for t in vanilla.tools(LOBART)]
        assert len(chronicle.facts("seen_dead")) == 1
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize(
    ("distance", "seen", "witnessed"),
    [(9000, True, True), (500, False, False), (1000, None, True), (3000, None, False)],
)
async def test_a_death_is_remembered_only_when_the_giver_sees_it(graph, distance, seen, witnessed):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    link_cm, _, chronicle, vanilla = await _setup(fake, FakeBackend(delay=0), graph, store)
    fake.vars[KILL] = 1
    fake.npc_states[BANDIT] = {"state": "dead", "name": "Бандит", "distance": distance}
    if seen is not None:
        fake.npc_states[BANDIT]["seen"] = seen
    try:
        chronicle.record({"fact": "died", "npc": BANDIT, "killer": "PLAYER"})
        await _until(lambda: not vanilla._background)
        assert chronicle.facts("seen_dead") == (
            [{"fact": "seen_dead", "npc": LOBART, "dead": BANDIT, "how": "saw_die"}]
            if witnessed
            else []
        )
        sight = [
            m for m in fake.received if m["type"] == "story_state" and m.get("near") == LOBART
        ]
        assert len(sight) == 1 and sight[0]["npcs"] == [BANDIT]
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize("error", [GameError("no game"), TimeoutError("no answer")])
async def test_a_failed_death_observation_is_not_remembered(graph, monkeypatch, error):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    link_cm, _, chronicle, vanilla = await _setup(fake, FakeBackend(delay=0), graph, store)
    fake.vars[KILL] = 1
    original = vanilla.link.story_state

    async def unavailable(**kwargs):
        if kwargs.get("near") == LOBART:
            raise error
        return await original(**kwargs)

    monkeypatch.setattr(vanilla.link, "story_state", unavailable)
    try:
        chronicle.record({"fact": "died", "npc": BANDIT, "killer": "PLAYER"})
        await _until(lambda: not vanilla._background)
        assert chronicle.facts("seen_dead") == []
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize("status", [2, 3, 4])
async def test_a_closed_quest_does_not_remember_a_new_death(graph, status):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    link_cm, _, chronicle, vanilla = await _setup(fake, FakeBackend(delay=0), graph, store)
    fake.vars[KILL] = status
    fake.npc_states[BANDIT] = {"state": "dead", "distance": 1000, "seen": True}
    try:
        chronicle.record({"fact": "died", "npc": BANDIT, "killer": LOBART})
        await _until(lambda: not vanilla._background)
        assert chronicle.facts("seen_dead") == []
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_a_seen_body_is_remembered_once_and_follows_the_save_branch(graph):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    link_cm, _, chronicle, vanilla = await _setup(fake, FakeBackend(delay=0), graph, store)
    fake.vars[KILL] = 1
    fake.npc_states[BANDIT] = {"state": "dead", "name": "Бандит", "distance": 1000, "seen": True}
    try:
        before = store.head("A")
        await vanilla.prepare(LOBART)
        await vanilla.prepare(LOBART)
        assert chronicle.facts("seen_dead") == [
            {"fact": "seen_dead", "npc": LOBART, "dead": BANDIT, "how": "saw_body"}
        ]
        after = store.head("A")
        fake.npc_states[BANDIT].update(distance=5000, seen=False)

        async def load_branch(branch, head):
            await fake.send(type="loading")
            await fake.send(type="session", branch=branch, parent="A", parent_seq=head, reason="load")
            await vanilla.link.evaluate(["int", 1])
            await _until(lambda: chronicle.branch == branch)
            await vanilla.prepare(LOBART)

        await load_branch("after-body", after)
        offer = vanilla.offers[LOBART][KILL]
        assert offer.seen_true() and offer.reachable() is None
        assert "Бандит — мёртв: ты видел тело" in offer.evidence()
        assert len(chronicle.facts("seen_dead")) == 1
        await load_branch("before-body", before)
        offer = vanilla.offers[LOBART][KILL]
        assert not offer.seen_true() and offer.reachable().npc == BANDIT
        assert chronicle.facts("seen_dead") == []
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.fixture
def two_bandits(graph):
    graph.instances["SECOND_BANDIT"] = "BDT_2_BANDIT"
    ending = graph.nodes["DIA_LOBART_BANDITDEAD"]
    for effect in ending.effects:
        effect["when"] = [
            "&&", effect["when"], ["call", "NPC_ISDEAD", ["inst", "SECOND_BANDIT"]]
        ]
    return graph


async def test_amendment_list_distinguishes_same_named_targets(two_bandits):
    fake = FakeGame(branch="A")
    link_cm, dialogue, chronicle, vanilla = await _setup(
        fake, FakeBackend(delay=0), two_bandits, MemoryStore(":memory:")
    )
    fake.instances["SECOND_BANDIT"] = "BDT_2_BANDIT"
    fake.vars.update({QUEST: 2, KILL: 1})
    fake.npc_states.update({
        BANDIT: {"state": "alive", "name": "Бандит", "distance": 500, "seen": True},
        "BDT_2_BANDIT": {"state": "dead", "name": "Бандит", "distance": 9000, "seen": False},
    })
    try:
        chronicle.record({"fact": "seen_dead", "npc": LOBART, "dead": "BDT_2_BANDIT", "how": "saw_die"})
        dialogue.open(LOBART, "Лобарт")
        await vanilla.prepare(LOBART)
        assert (
            f"Можешь снять пункты (amend_quest): {BANDIT} — Бандит (жив, у тебя на глазах); "
            "BDT_2_BANDIT — Бандит (мёртв: ты видел, как он умер)."
        ) in vanilla.situation(LOBART)
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize("unseen", [False, True])
async def test_completion_reminder_requires_every_target_witnessed(two_bandits, unseen):
    fake = FakeGame(branch="A")
    link_cm, dialogue, chronicle, vanilla = await _setup(
        fake, FakeBackend(delay=0), two_bandits, MemoryStore(":memory:")
    )
    fake.instances["SECOND_BANDIT"] = "BDT_2_BANDIT"
    fake.vars.update({QUEST: 2, KILL: 1})
    fake.npc_states.update({
        BANDIT: {"state": "dead", "name": "Бандит", "distance": 500, "seen": True},
        "BDT_2_BANDIT": {"state": "dead", "name": "Второй бандит", "distance": 9000, "seen": False},
    })
    try:
        chronicle.record({"fact": "caught", "npc": LOBART, "quest": QUEST})
        if not unseen:
            chronicle.record({"fact": "seen_dead", "npc": LOBART, "dead": "BDT_2_BANDIT", "how": "saw_die"})
        dialogue.open(LOBART, "Лобарт")
        await vanilla.prepare(LOBART)
        assert vanilla.trust.broken(LOBART)
        assert vanilla.offers[LOBART][KILL].seen_true() is not unseen
        reminder = (
            "Засчитать можно сейчас: отдаёшь награду — вызови accept_quest, без него она не перейдёт."
        )
        assert (reminder in vanilla.situation(LOBART)) is not unseen
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_even_a_distrustful_giver_accepts_deaths_it_witnessed_earlier(two_bandits):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    accept = FakeStep("С ними покончено.", [ToolCall("accept_quest", {"quest": KILL}, "a1")])
    llm = FakeBackend(script=[accept, FakeStep("Держи плату.")], delay=0)
    link_cm, dialogue, chronicle, vanilla = await _setup(fake, llm, two_bandits, store)
    fake.instances["SECOND_BANDIT"] = "BDT_2_BANDIT"
    fake.vars.update({QUEST: 2, KILL: 1})
    fake.npc_states[BANDIT] = {"state": "dead", "name": "Бандит", "distance": 5000, "seen": False}
    fake.npc_states["BDT_2_BANDIT"] = {
        "state": "dead", "name": "Второй бандит", "distance": 1000, "seen": True
    }
    try:
        chronicle.record({"fact": "caught", "npc": LOBART, "quest": KILL, "how": "видел сам"})
        chronicle.record({"fact": "died", "npc": BANDIT, "killer": LOBART})
        chronicle.record({"fact": "died", "npc": "BDT_2_BANDIT", "killer": "PLAYER"})
        await _until(lambda: not vanilla._background)
        fake.npc_states["BDT_2_BANDIT"].update(distance=5000, seen=False)
        dialogue.open(LOBART, "Лобарт")
        await vanilla.prepare(LOBART)
        offer = vanilla.offers[LOBART][KILL]
        assert offer.seen_true() and offer.reachable() is None
        assert "ты убил его сам" in offer.evidence() and "ты видел, как он умер" in offer.evidence()
        assert {t.name for t in vanilla.tools(LOBART)} == {"accept_quest", "amend_quest", "drop_quest"}
        await dialogue.say_as_player(LOBART, "С ними покончено.")
        await _until(lambda: chronicle.facts("claim") != [])
        assert chronicle.facts("claim")[0]["truth"] is True and chronicle.facts("lie") == []
        assert not any(m["type"] == "action" and m["action"] == "go_to_npc" for m in fake.received)
        assert all("check_claim" not in [t.name for t in call[2]] for call in llm.calls)
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_checking_another_body_keeps_the_death_already_witnessed(two_bandits):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    check = FakeStep("Проверю второго.", [ToolCall("check_claim", {"quest": KILL}, "c1")])
    llm = FakeBackend(script=[check, FakeStep("Оба мертвы. Держи плату.")], delay=0)
    link_cm, dialogue, chronicle, vanilla = await _setup(fake, llm, two_bandits, store)
    fake.instances["SECOND_BANDIT"] = "BDT_2_BANDIT"
    fake.vars.update({QUEST: 2, KILL: 1})
    fake.npc_states[BANDIT] = {"state": "dead", "name": "Бандит", "distance": 9000, "seen": False}
    fake.npc_states["BDT_2_BANDIT"] = {
        "state": "dead", "name": "Второй бандит", "distance": 4000, "seen": False
    }
    try:
        chronicle.record({"fact": "died", "npc": BANDIT, "killer": LOBART})
        await _until(lambda: not vanilla._background)
        await vanilla.link.attach(LOBART)
        dialogue.open(LOBART, "Лобарт")
        await vanilla.prepare(LOBART)
        assert vanilla.offers[LOBART][KILL].reachable().npc == "BDT_2_BANDIT"
        await dialogue.say_as_player(LOBART, "Оба бандита мертвы.")
        await _until(lambda: chronicle.facts("claim") != [])
        await _until(lambda: LOBART not in vanilla.checking)
        assert chronicle.facts("claim")[0]["checked"] is True
        assert chronicle.facts("lie") == []
        assert {(f["dead"], f["how"]) for f in chronicle.facts("seen_dead")} == {
            (BANDIT, "killed"), ("BDT_2_BANDIT", "saw_body")
        }
        go = [
            m["args"]["npc"]
            for m in fake.received
            if m["type"] == "action" and m["action"] == "go_to_npc"
        ]
        assert go == ["BDT_2_BANDIT", "PLAYER"]
    finally:
        await link_cm.__aexit__(None, None, None)


def test_broken_trust_reason_lists_every_cause_in_order():
    record = Record(lied=["lie"], broke=["debt"], crimes=["theft", "attack"])
    assert record.broken_reason() == (
        "он уже лгал тебе; он не сдержал слово; "
        "его дела подорвали твоё доверие (theft; attack)"
    )
    assert Record().broken_reason() == ""


@pytest.mark.parametrize("lied", [False, True])
async def test_accept_quest_names_the_actual_trust_reason(graph, lied):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    llm = FakeBackend(delay=0)
    link_cm, dialogue, chronicle, vanilla = await _setup(fake, llm, graph, store)
    fake.vars[KILL] = 1
    fake.npc_states[BANDIT] = {"state": "alive", "name": "Бандит", "distance": 4000}
    try:
        chronicle.record({"fact": "gate_vouch_broken", "npc": LOBART, "crime_kind": "theft"})
        if lied:
            chronicle.record({"fact": "caught", "npc": LOBART, "quest": QUEST})
        record = vanilla.trust.of(LOBART)
        assert record.crimes and not record.broke
        assert bool(record.lied) is lied
        conv = dialogue.open(LOBART, "Лобарт")
        await vanilla.prepare(LOBART)
        result = await vanilla.handle(dialogue, conv, ToolCall("accept_quest", {"quest": KILL}, "c1"), None)
        assert result is not None and result.status == "rejected"
        reason = (
            "его дела подорвали твоё доверие "
            "(ты поручился за него, а он совершил преступление: «theft»)"
        )
        if lied:
            reason = f"он уже лгал тебе; {reason}"
        else:
            assert "лгал" not in result.output
        assert result.output == (
            f"Отклонено: {reason}. {WORD_BROKEN} Можешь сходить и посмотреть сам (check_claim)."
        )
        assert chronicle.facts("claim") == []
    finally:
        await link_cm.__aexit__(None, None, None)


CAVALORN, BRAGO = "BAU_4300_ADDON_CAVALORN", "BDT_1013_BANDIT_L"
KILLBRAGO = "MIS_ADDON_CAVALORN_KILLBRAGO"
LETTER = "MIS_ADDON_CAVALORN_LETTER2VATRAS"
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
                {"do": "set", "var": LETTER, "value": ["int", 1, "LOG_RUNNING"]},
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
    g["quests"].append(
        {
            "var": LETTER,
            "topics": [{"topic": "TOPIC_LETTER", "title": "Письмо Ватрасу"}],
            "running": [KILLED],
            "success": [],
            "failed": [],
            "obsolete": [],
            "other": [],
        }
    )
    path = tmp_path / "story.json"
    path.write_text(json.dumps(g, ensure_ascii=False), encoding="utf-8")
    return load(path)


async def _cavalorn(fake: FakeGame, llm: FakeBackend, graph: StoryGraph):
    fake.vars = {KILLBRAGO: 0, LETTER: 0, "KAPITEL": 1}
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


async def test_a_distrustful_giver_promises_no_reward_until_every_death_is_seen(cavalorn):
    cavalorn.instances["SECOND_BANDIT"] = "BDT_2_BANDIT"
    helper = cavalorn.functions["C_BRAGOBANDITSDEAD"]
    helper["body"] = [
        "&&", helper["body"], ["call", "NPC_ISDEAD", ["inst", "SECOND_BANDIT"]]
    ]
    ending = cavalorn.nodes[KILLED]
    ending.condition[2] = ["==", ["var", KILLBRAGO], ["int", 1]]
    ending.effects.append({
        "do": "give", "from": ["inst", "SELF"], "to": ["inst", "OTHER"],
        "item": ["sym", "ITMI_GOLD"], "amount": ["int", 50],
    })
    fake = FakeGame(branch="A")
    accept = FakeStep("Теперь сам знаю. Держи плату.", [
        ToolCall("accept_quest", {"quest": KILLBRAGO}, "a1")
    ])
    llm = FakeBackend(script=[accept, FakeStep(VATRAS)], delay=0)
    link_cm, dialogue, chronicle, vanilla = await _cavalorn(fake, llm, cavalorn)
    fake.vars[KILLBRAGO] = 1
    fake.instances["SECOND_BANDIT"] = "BDT_2_BANDIT"
    fake.npc_states[BRAGO].update(state="dead", distance=1000, seen=True)
    fake.npc_states["BDT_2_BANDIT"] = {
        "state": "dead", "name": "Второй бандит", "distance": 4000, "seen": False
    }
    fake.fns["C_BRAGOBANDITSDEAD"] = 1
    try:
        chronicle.record({"fact": "caught", "npc": CAVALORN, "quest": KILLBRAGO, "how": "видел сам"})
        dialogue.open(CAVALORN, "Кавалорн")
        await vanilla.prepare(CAVALORN)
        offer = vanilla.offers[CAVALORN][KILLBRAGO]
        said = offer.said
        assert said == f"«{VATRAS}»"
        assert not offer.seen_true() and offer.reachable().npc == "BDT_2_BANDIT"
        assert [f["dead"] for f in chronicle.facts("seen_dead")] == [BRAGO]
        situation = vanilla.situation(CAVALORN)
        assert (
            "Засчитать сейчас нельзя: на слово ты ему не веришь, а своими глазами видел не всё. "
            "Не говори, что отдаёшь награду или рассчитываешься, пока дело не засчитано. "
            "Можешь сходить и посмотреть сам (check_claim)."
        ) in situation
        assert "Засчитывая" not in situation and VATRAS not in situation
        assert "accept_quest" not in {t.name for t in vanilla.tools(CAVALORN)}
        assert "check_claim" in {t.name for t in vanilla.tools(CAVALORN)}
        assert chronicle.facts("claim") == [] and _effects(fake, CAVALORN) == []

        chronicle.record({"fact": "died", "npc": "BDT_2_BANDIT", "killer": CAVALORN})
        await _until(lambda: not vanilla._background)
        assert {"fact": "seen_dead", "npc": CAVALORN, "dead": "BDT_2_BANDIT", "how": "killed"} in (
            chronicle.facts("seen_dead")
        )
        await vanilla.prepare(CAVALORN)
        assert vanilla.offers[CAVALORN][KILLBRAGO].seen_true()
        situation = vanilla.situation(CAVALORN)
        assert "Засчитать сейчас нельзя" not in situation
        assert f"Засчитывая, в игре ты говоришь (скажи это по смыслу, своими словами): {said}" in situation
        assert "accept_quest" in {t.name for t in vanilla.tools(CAVALORN)}
        await dialogue.say_as_player(CAVALORN, "С бандитами покончено.")
        await _until(lambda: chronicle.facts("claim") != [])
        assert chronicle.facts("claim")[0]["truth"] is True
        effects = _effects(fake, CAVALORN)[0]
        assert {"do": "set", "var": KILLBRAGO, "value": 2} in effects
        assert {"do": "set", "var": LETTER, "value": 1} in effects
        assert {"do": "xp", "amount": 100} in effects
        assert {"do": "give", "item": "ITMI_GOLD", "count": 50} in effects
        assert chronicle.facts("lie") == []
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_checking_and_accepting_a_quest_carries_its_ending_lines_to_the_model(cavalorn):
    fake = FakeGame(branch="A")
    check = FakeStep("Сейчас сам посмотрю.", [ToolCall("check_claim", {"quest": KILLBRAGO}, "c1")])
    llm = FakeBackend(script=[check, FakeStep(VATRAS)], delay=0)
    link_cm, dialogue, chronicle, vanilla = await _cavalorn(fake, llm, cavalorn)
    fake.npc_states[BRAGO].update(state="dead", distance=4000, seen=False)
    fake.fns["C_BRAGOBANDITSDEAD"] = 1
    try:
        await vanilla.link.attach(CAVALORN)
        dialogue.open(CAVALORN, "Кавалорн")
        await vanilla.prepare(CAVALORN)
        said = vanilla.offers[CAVALORN][KILLBRAGO].said
        assert said == f"«{VATRAS}»"
        await dialogue.say_as_player(CAVALORN, "С бандитами покончено.")
        await _until(lambda: CAVALORN not in vanilla.checking and chronicle.facts("claim") != [])
        cue = f" В игре при этом ты говоришь (скажи это по смыслу, своими словами): {said}"
        await _until(lambda: any(cue in str(call[1]) for call in llm.calls))
        assert cue in str(llm.calls[-1][1])
        assert chronicle.facts("claim")[0]["checked"] is True
        assert {"do": "set", "var": LETTER, "value": 1} in _effects(fake, CAVALORN)[0]
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize("reload", [False, True])
async def test_the_givers_mistaken_death_observation_is_not_the_heros_lie(cavalorn, reload):
    fake = FakeGame(branch="A")
    accept = FakeStep("Я сам видел его мёртвым.", [ToolCall("accept_quest", {"quest": KILLBRAGO}, "a1")])
    llm = FakeBackend(script=[accept, FakeStep("Держи плату.")], delay=0)
    link_cm, dialogue, chronicle, vanilla = await _cavalorn(fake, llm, cavalorn)
    fake.npc_states[BRAGO].update(state="dead", distance=1000, seen=True)
    try:
        await vanilla.prepare(CAVALORN)
        assert chronicle.facts("seen_dead")[0]["how"] == "saw_body"
        fake.npc_states[BRAGO].update(state="alive", distance=5000, seen=False)
        fake.fns["C_BRAGOBANDITSDEAD"] = 0
        chronicle.record({"fact": "caught", "npc": CAVALORN, "quest": KILLBRAGO, "how": "видел сам"})
        if reload:
            vanilla._load()
        dialogue.open(CAVALORN, "Кавалорн")
        await vanilla.prepare(CAVALORN)
        assert vanilla.offers[CAVALORN][KILLBRAGO].seen_true()
        await dialogue.say_as_player(CAVALORN, "С бандитами покончено.")
        await _until(lambda: chronicle.facts("claim") != [])
        assert chronicle.facts("claim")[0]["truth"] is False
        assert chronicle.facts("lie") == [] and vanilla.told[KILLBRAGO].lie is False
        vanilla._load()
        assert vanilla.told[KILLBRAGO].lie is False
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_a_seen_death_does_not_make_an_unseen_target_in_a_helper_true(cavalorn):
    cavalorn.instances["SECOND_BANDIT"] = "BDT_2_BANDIT"
    helper = cavalorn.functions["C_BRAGOBANDITSDEAD"]
    helper["body"] = [
        "&&", helper["body"], ["call", "NPC_ISDEAD", ["inst", "SECOND_BANDIT"]]
    ]
    fake = FakeGame(branch="A")
    accept = FakeStep("Верю тебе.", [ToolCall("accept_quest", {"quest": KILLBRAGO}, "a1")])
    llm = FakeBackend(script=[accept, FakeStep("Держи плату.")], delay=0)
    link_cm, dialogue, chronicle, vanilla = await _cavalorn(fake, llm, cavalorn)
    fake.instances["SECOND_BANDIT"] = "BDT_2_BANDIT"
    fake.npc_states[BRAGO].update(state="dead", distance=1000, seen=True)
    fake.npc_states["BDT_2_BANDIT"] = {
        "state": "alive", "name": "Второй бандит", "distance": 9000, "seen": False
    }
    fake.fns["C_BRAGOBANDITSDEAD"] = 0
    try:
        dialogue.open(CAVALORN, "Кавалорн")
        await dialogue.say_as_player(CAVALORN, "Все бандиты мертвы.")
        await _until(lambda: chronicle.facts("lie") != [])
        assert chronicle.facts("claim")[0]["truth"] is False
        assert len(chronicle.facts("lie")) == 1 and vanilla.told[KILLBRAGO].lie is True
        assert [f["dead"] for f in chronicle.facts("seen_dead")] == [BRAGO]
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_a_target_reviving_before_the_givers_eyes_overrides_its_memory(graph):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    llm = FakeBackend(script=[FakeStep("Но он же жив!")], delay=0)
    link_cm, dialogue, chronicle, vanilla = await _setup(fake, llm, graph, store)
    fake.vars.update({QUEST: 2, KILL: 1})
    fake.npc_states[BANDIT] = {"state": "dead", "name": "Бандит", "distance": 1000, "seen": True}
    try:
        await vanilla.prepare(LOBART)
        assert vanilla.offers[LOBART][KILL].seen_true()
        fake.npc_states[BANDIT]["state"] = "alive"
        conv = dialogue.open(LOBART, "Лобарт")
        await vanilla.prepare(LOBART)
        offer = vanilla.offers[LOBART][KILL]
        assert not offer.seen_true() and offer.reachable() is None
        assert "Бандит жив — у тебя на глазах" in offer.seen_false()
        assert "мёртв: ты видел тело" not in offer.evidence()
        assert "accept_quest" not in {t.name for t in vanilla.tools(LOBART)}
        assert "Засчитать сейчас нельзя: Бандит жив — у тебя на глазах, в 10 м" in vanilla.situation(
            LOBART
        )
        result = await vanilla.handle(
            dialogue, conv, ToolCall("accept_quest", {"quest": KILL}, "a1"), None
        )
        assert result.status == "rejected" and result.output == (
            "Отклонено: Бандит жив — у тебя на глазах, в 10 м. "
            "Своим глазам ты веришь больше, чем его словам."
        )
        await dialogue.say_as_player(LOBART, "Бандит мёртв.")
        await _until(lambda: len(llm.calls) == 1)
        assert "accept_quest" not in {t.name for t in llm.calls[0][2]}
        assert chronicle.facts("claim") == []
        assert len(chronicle.facts("seen_dead")) == 1
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_another_quests_ending_is_not_offered_as_a_letter_assignment(cavalorn):
    fake = FakeGame(branch="A")
    link_cm, _, _, vanilla = await _cavalorn(fake, FakeBackend(delay=0), cavalorn)
    fake.npc_states[BRAGO].update(state="dead", distance=1000, seen=True)
    fake.fns["C_BRAGOBANDITSDEAD"] = 1
    try:
        await vanilla.prepare(CAVALORN)
        assert KILLBRAGO in vanilla.offers[CAVALORN]
        assert LETTER not in vanilla.starts[CAVALORN]
        assert not any(start.dialog == KILLED for start in vanilla.starts[CAVALORN].values())
        assert "поручение «Письмо Ватрасу»" not in vanilla.situation(CAVALORN)
    finally:
        await link_cm.__aexit__(None, None, None)


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


def test_akil_farm_ambush_dialogs_and_choices_are_freed(real_graph):
    disabled = {op["node"] for op in freed(real_graph.nodes.__contains__)}
    npcs = {"SLD_840_ALVARES", "SLD_841_ENGARDO"}
    important = {n.id for n in real_graph.nodes.values() if n.npc in npcs and n.important}
    choices = set(real_graph.node("DIA_ALVARES_ATTACK").choices())
    assert len(important) == 4 and len(choices) == 5
    assert important | choices <= disabled
    for npc in ("ALVARES", "ENGARDO"):
        assert f"DIA_{npc}_EXIT" not in disabled
        assert f"DIA_{npc}_PICKPOCKET" not in disabled


@pytest.mark.parametrize("chapter", [1, 2, 3, 4])
@pytest.mark.parametrize("node_id", ["DIA_AKIL_NACHKAMPF", "DIA_KATI_HALLO", "DIA_RANDOLPH_HALLO"])
def test_akils_rescue_endings_need_no_prior_dialog_or_journal(real_graph, node_id, chapter):
    bandits = [real_graph.npc(npc) for npc in ("ALVARES", "ENGARDO")]
    state = StoryState(
        vars={"KAPITEL": chapter},
        npcs={npc: {"state": "dead"} for npc in bandits},
    )
    partial = Partial(real_graph, state)
    node = real_graph.node(node_id)
    assert partial.truth(node.condition) is (node_id != "DIA_RANDOLPH_HALLO" or chapter < 4)
    for npc in bandits:
        state.npcs[npc]["state"] = "alive"
        assert partial.truth(node.condition) is False
        state.npcs[npc]["state"] = "dead"

    if node_id == "DIA_AKIL_NACHKAMPF":
        ending = next(e for e in node.effects if e.get("var") == "TOPIC_END_AKILSSLDSTILLTHERE")
        assert ending["do"] == "set" and partial.value(ending["value"]) == 1
        assert "AKILS_SLDSTILLTHERE" not in str(ending.get("when"))
        assert "TOPIC_AKILSSLDSTILLTHERE" not in str(ending.get("when"))


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
@pytest.mark.parametrize("pay_args", [{}, {"pay": None}], ids=["omitted", "null"])
async def test_gregs_parent_choice_requires_and_takes_the_clothing_the_hero_has(greg, armor, pay_args):
    fake = FakeGame(branch="A")
    fake.hero_items = {armor: 1}
    accept = FakeStep("Держи плату.", [ToolCall("accept_quest", {"quest": GREG_QUEST, **pay_args}, "a1")])
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
        assert chronicle.facts("quest_outcome")[0]["pay"] == "full"

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


async def test_amending_is_immediate_accumulates_and_survives_loading(two_bandits):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    link_cm, dialogue, chronicle, vanilla = await _setup(fake, FakeBackend(delay=0), two_bandits, store)
    fake.vars.update({QUEST: 2, KILL: 1})
    fake.instances["SECOND_BANDIT"] = "BDT_2_BANDIT"
    fake.npc_states.update({
        BANDIT: {"state": "alive", "name": "Первый бандит", "distance": 500, "seen": True},
        "BDT_2_BANDIT": {"state": "alive", "name": "Второй бандит", "distance": 9000},
    })
    entries = []
    vanilla.journal = Journal(entries.append)
    try:
        chronicle.record({"fact": "caught", "npc": LOBART, "quest": QUEST})
        conv = dialogue.open(LOBART, "Лобарт")
        await vanilla.prepare(LOBART)
        assert f"{BANDIT} — Первый бандит" in vanilla.situation(LOBART)
        result = await vanilla.handle(
            dialogue, conv,
            ToolCall("amend_quest", {"quest": KILL, "drop": [BANDIT], "why": "Пощадил его"}, "a1"),
            None,
        )
        assert result.status == "accepted"
        assert chronicle.facts("quest_terms") == [{
            "fact": "quest_terms", "quest": KILL, "npc": LOBART, "name": "Лобарт",
            "drop": [BANDIT], "why": "Пощадил его",
        }]
        assert entries[-1]["kind"] == "quest_amended"
        assert _effects(fake, LOBART) == []  # No spoken line is needed to change the agreement.
        situation = vanilla.situation(LOBART)
        assert "Снято тобой: Первый бандит — «Пощадил его»" in situation
        assert "Засчитать можно, когда будут мертвы: Второй бандит." in situation
        offer = vanilla.offers[LOBART][KILL]
        assert offer.alive_in_view() is None and offer.seen_false() is None
        assert "Первый бандит" not in (offer.evidence() or "")
        vanilla._load()
        await vanilla.prepare(LOBART)
        assert vanilla.offers[LOBART][KILL].claim.dead == ["BDT_2_BANDIT"]
        result = await vanilla.handle(
            dialogue, conv,
            ToolCall("amend_quest", {
                "quest": KILL, "drop": ["BDT_2_BANDIT"], "why": "Он ушёл",
            }, "a2"),
            None,
        )
        assert result.status == "accepted" and len(chronicle.facts("quest_terms")) == 2
        vanilla._load()
        await vanilla.prepare(LOBART)
        offer = vanilla.offers[LOBART][KILL]
        assert offer.claim.dead == [] and offer.targets == []
        assert offer.seen_true() and offer.reachable() is None
        assert "Ты снял все условия: можешь засчитать (accept_quest)" in vanilla.situation(LOBART)
        assert "amend_quest" not in {t.name for t in vanilla.tools(LOBART)}
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize("term", ["MIS_BANDIT_PERMISSION", "C_BANDIT_PERMISSION", "MISSING_TERM"])
async def test_an_amendment_cannot_remove_variables_helpers_or_unknown_terms(graph, term):
    ending = graph.nodes["DIA_LOBART_BANDITDEAD"]
    guard = [
        "&&", ["call", "NPC_ISDEAD", ["inst", "BANDIT"]],
        ["==", ["var", "MIS_BANDIT_PERMISSION"], ["int", 1]],
        ["fn", "C_BANDIT_PERMISSION"],
    ]
    for effect in ending.effects:
        effect["when"] = guard
    graph.functions["C_BANDIT_PERMISSION"] = {"params": 0, "body": ["int", 1], "lifted": True}
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    link_cm, dialogue, chronicle, vanilla = await _setup(fake, FakeBackend(delay=0), graph, store)
    fake.vars.update({QUEST: 2, KILL: 1, "MIS_BANDIT_PERMISSION": 1})
    try:
        conv = dialogue.open(LOBART, "Лобарт")
        await vanilla.prepare(LOBART)
        assert vanilla.offers[LOBART][KILL].claim.vars == {"MIS_BANDIT_PERMISSION": 1}
        result = await vanilla.handle(
            dialogue, conv,
            ToolCall("amend_quest", {"quest": KILL, "drop": [term], "why": "Передумал"}, "a1"),
            None,
        )
        assert result.status == "rejected" and "Отклонено:" in result.output
        assert term in result.output and chronicle.facts("quest_terms") == []
        assert vanilla.offers[LOBART][KILL].claim.dead == [BANDIT]
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize("tool", ["amend_quest", "drop_quest"])
async def test_the_main_plot_cannot_be_amended_or_dropped(graph, tool):
    chapter = copy.deepcopy(graph.nodes["DIA_HILDA_HALLO"])
    chapter.id = "DIA_CHAPTER_CHANGE"
    chapter.effects = [{"do": "chapter", "chapter": 2}]
    chapter.reads = {KILL}
    graph.nodes[chapter.id] = chapter
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    link_cm, dialogue, chronicle, vanilla = await _setup(fake, FakeBackend(delay=0), graph, store)
    fake.vars.update({QUEST: 2, KILL: 1})
    try:
        conv = dialogue.open(LOBART, "Лобарт")
        await vanilla.prepare(LOBART)
        assert not {"amend_quest", "drop_quest"} & {t.name for t in vanilla.tools(LOBART)}
        args = {"quest": KILL, "why": "Передумал"}
        if tool == "amend_quest":
            args["drop"] = [BANDIT]
        result = await vanilla.handle(dialogue, conv, ToolCall(tool, args, "a1"), None)
        assert result.status == "rejected" and "главного сюжета" in result.output
        assert chronicle.facts("quest_terms") == [] and chronicle.facts("quest_outcome") == []
        assert _effects(fake, LOBART) == []
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize("tool", ["amend_quest", "drop_quest"])
async def test_a_giver_cannot_change_a_quest_it_did_not_ask_for(cavalorn, tool):
    fake = FakeGame(branch="A")
    link_cm, dialogue, chronicle, vanilla = await _cavalorn(fake, FakeBackend(delay=0), cavalorn)
    try:
        conv = dialogue.open(CAVALORN, "Кавалорн")
        await vanilla.prepare(CAVALORN)
        assert not vanilla.offers[CAVALORN][KILLBRAGO].asked
        assert not {"amend_quest", "drop_quest"} & {t.name for t in vanilla.tools(CAVALORN)}
        for quest in (KILLBRAGO, "MIS_NO_SUCH_QUEST"):
            args = {"quest": quest, "why": "Передумал"}
            if tool == "amend_quest":
                args["drop"] = [BRAGO]
            result = await vanilla.handle(dialogue, conv, ToolCall(tool, args, "a1"), None)
            if quest in cavalorn.quests:
                assert result.status == "rejected"
            else:
                assert result is None  # another extension may own the quest
        assert chronicle.facts("quest_terms") == [] and chronicle.facts("quest_outcome") == []
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize("drop_all", [False, True])
async def test_a_spared_target_before_its_eyes_does_not_block_a_distrustful_giver(two_bandits, drop_all):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    accept = FakeStep("С ними покончено.", [ToolCall("accept_quest", {"quest": KILL}, "a1")])
    llm = FakeBackend(script=[accept, FakeStep("Держи плату.")], delay=0)
    link_cm, dialogue, chronicle, vanilla = await _setup(fake, llm, two_bandits, store)
    fake.instances["SECOND_BANDIT"] = "BDT_2_BANDIT"
    fake.vars.update({QUEST: 2, KILL: 1})
    fake.npc_states.update({
        BANDIT: {"state": "alive", "name": "Пощажённый", "distance": 500, "seen": True},
        "BDT_2_BANDIT": {
            "state": "alive" if drop_all else "dead", "name": "Второй", "distance": 9000, "seen": False,
        },
    })
    try:
        chronicle.record({"fact": "caught", "npc": LOBART, "quest": QUEST})
        if not drop_all:
            chronicle.record({"fact": "seen_dead", "npc": LOBART, "dead": "BDT_2_BANDIT", "how": "killed"})
        conv = dialogue.open(LOBART, "Лобарт")
        await vanilla.prepare(LOBART)
        dropped = [BANDIT, "BDT_2_BANDIT"] if drop_all else [BANDIT]
        result = await vanilla.handle(
            dialogue, conv,
            ToolCall("amend_quest", {"quest": KILL, "drop": dropped, "why": "Пощадил"}, "m1"),
            None,
        )
        assert result.status == "accepted" and vanilla.trust.broken(LOBART)
        assert "check_claim" not in {t.name for t in vanilla.tools(LOBART)}
        await dialogue.say_as_player(LOBART, "С ними покончено.")
        await _until(lambda: chronicle.facts("quest_outcome") != [])
        assert {"do": "set", "var": KILL, "value": 2} in _effects(fake, LOBART)[0]
        assert {"do": "xp", "amount": 100} in _effects(fake, LOBART)[0]
        assert chronicle.facts("lie") == [] and fake.npc_states[BANDIT]["state"] == "alive"
        assert chronicle.facts("quest_outcome") == [{
            "fact": "quest_outcome", "quest": KILL, "npc": LOBART, "how": "amended",
            "drop": dropped, "why": ["Пощадил"], "pay": "full",
        }]
        assert not any(m["type"] == "action" and m["action"] == "go_to_npc" for m in fake.received)
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize("hearsay_is_true", [False, True])
async def test_amended_helper_counts_only_unseen_remaining_targets_as_lies(cavalorn, hearsay_is_true):
    cavalorn.nodes[KILLED].condition[2] = ["==", ["var", KILLBRAGO], ["int", 1]]
    cavalorn.instances.update({"SECOND_BANDIT": "BDT_2_BANDIT", "THIRD_BANDIT": "BDT_3_BANDIT"})
    helper = cavalorn.functions["C_BRAGOBANDITSDEAD"]
    helper["body"] = [
        "&&", helper["body"], ["call", "NPC_ISDEAD", ["inst", "SECOND_BANDIT"]],
        ["call", "NPC_ISDEAD", ["inst", "THIRD_BANDIT"]],
    ]
    fake = FakeGame(branch="A")
    accept = FakeStep("Верю тебе.", [ToolCall("accept_quest", {"quest": KILLBRAGO}, "a1")])
    llm = FakeBackend(script=[accept, FakeStep("Держи плату.")], delay=0)
    link_cm, dialogue, chronicle, vanilla = await _cavalorn(fake, llm, cavalorn)
    fake.vars[KILLBRAGO] = 1
    fake.instances.update({"SECOND_BANDIT": "BDT_2_BANDIT", "THIRD_BANDIT": "BDT_3_BANDIT"})
    fake.npc_states.update({
        BRAGO: {"state": "alive", "name": "Пощажённый", "distance": 500, "seen": True},
        "BDT_2_BANDIT": {"state": "alive", "name": "Видел мёртвым", "distance": 9000, "seen": False},
        "BDT_3_BANDIT": {
            "state": "dead" if hearsay_is_true else "alive", "name": "Со слов героя",
            "distance": 9000, "seen": False,
        },
    })
    fake.fns["C_BRAGOBANDITSDEAD"] = 0
    try:
        chronicle.record({"fact": "seen_dead", "npc": CAVALORN, "dead": "BDT_2_BANDIT", "how": "saw_body"})
        conv = dialogue.open(CAVALORN, "Кавалорн")
        await vanilla.prepare(CAVALORN)
        result = await vanilla.handle(
            dialogue, conv,
            ToolCall("amend_quest", {"quest": KILLBRAGO, "drop": [BRAGO], "why": "Пощадил"}, "m1"),
            None,
        )
        assert result.status == "accepted"
        await dialogue.say_as_player(CAVALORN, "С бандитами покончено.")
        await _until(lambda: chronicle.facts("quest_outcome") != [])
        assert chronicle.facts("claim")[0]["truth"] is False
        if hearsay_is_true:
            assert chronicle.facts("lie") == []
        else:
            assert chronicle.facts("lie")[0]["targets"] == ["BDT_3_BANDIT"]
            assert vanilla.told[KILLBRAGO].targets == ["BDT_3_BANDIT"]
        assert {"do": "set", "var": LETTER, "value": 1} in _effects(fake, CAVALORN)[0]
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize("helper", [False, True])
async def test_a_dropped_item_is_no_lie_and_the_ending_takes_only_what_the_hero_has(graph, helper):
    ending = graph.nodes["DIA_LOBART_RUEBENRUNNING"]
    ending.effects.insert(0, {
        "do": "give", "from": ["inst", "OTHER"], "to": ["inst", "SELF"],
        "item": ["sym", "ITPL_BEET"], "amount": ["int", 20], "when": HAS_20,
    })
    if helper:
        graph.functions["C_TURNIPS_BROUGHT"] = {"params": 0, "body": HAS_20, "lifted": True}
        for effect in ending.effects:
            if effect.get("when") == HAS_20:
                effect["when"] = ["fn", "C_TURNIPS_BROUGHT"]
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    llm = FakeBackend(script=[ACCEPT, FakeStep("Хватит и этого.")], delay=0)
    link_cm, dialogue, chronicle, vanilla = await _setup(fake, llm, graph, store)
    fake.hero_items = {"ITPL_BEET": 5}
    fake.fns["C_TURNIPS_BROUGHT"] = 0
    try:
        conv = dialogue.open(LOBART, "Лобарт")
        await vanilla.prepare(LOBART)
        result = await vanilla.handle(
            dialogue, conv,
            ToolCall("amend_quest", {"quest": QUEST, "drop": ["ITPL_BEET"], "why": "Хватит пяти"}, "m1"),
            None,
        )
        assert result.status == "accepted"
        assert vanilla.offers[LOBART][QUEST].claim.items == {}
        await dialogue.say_as_player(LOBART, "Вот что осталось.")
        await _until(lambda: chronicle.facts("quest_outcome") != [])
        effects = _effects(fake, LOBART)[0]
        assert {"do": "give", "item": "ITPL_BEET", "count": 5, "to": "self"} in effects
        assert {"do": "set", "var": TO_HILDA, "value": 1} in effects
        assert {"do": "xp", "amount": 50} in effects and chronicle.facts("lie") == []
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_dropping_a_death_does_not_waive_a_helpers_story_state_or_broken_trust(graph):
    ending = graph.nodes["DIA_LOBART_BANDITDEAD"]
    for effect in ending.effects:
        effect["when"] = ["&&", effect["when"], ["fn", "C_BANDIT_PERMISSION"]]
    graph.functions["C_BANDIT_PERMISSION"] = {
        "params": 0, "body": ["==", ["var", "MIS_BANDIT_PERMISSION"], ["int", 1]], "lifted": True,
    }
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    link_cm, dialogue, chronicle, vanilla = await _setup(fake, FakeBackend(delay=0), graph, store)
    fake.vars.update({QUEST: 2, KILL: 1, "MIS_BANDIT_PERMISSION": 1})
    fake.fns["C_BANDIT_PERMISSION"] = 1
    try:
        chronicle.record({"fact": "caught", "npc": LOBART, "quest": QUEST})
        conv = dialogue.open(LOBART, "Лобарт")
        await vanilla.prepare(LOBART)
        result = await vanilla.handle(
            dialogue, conv,
            ToolCall("amend_quest", {"quest": KILL, "drop": [BANDIT], "why": "Пощадил"}, "m1"),
            None,
        )
        assert result.status == "accepted"
        offer = vanilla.offers[LOBART][KILL]
        assert offer.claim.dead == [] and not offer.seen_true()
        assert "Ты снял все условия" not in vanilla.situation(LOBART)
        result = await vanilla.handle(dialogue, conv, ToolCall("accept_quest", {"quest": KILL}, "a1"), None)
        assert result.status == "rejected" and "он уже лгал тебе" in result.output
        assert chronicle.facts("claim") == [] and chronicle.facts("quest_outcome") == []
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_a_giver_can_drop_a_required_conversation(graph):
    ending = graph.nodes["DIA_LOBART_BANDITDEAD"]
    for effect in ending.effects:
        effect["when"] = [
            "&&", effect["when"],
            ["call", "NPC_KNOWSINFO", ["inst", "OTHER"], ["sym", "DIA_HILDA_HALLO"]],
        ]
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    accept = FakeStep("Ладно.", [ToolCall("accept_quest", {"quest": KILL}, "a1")])
    llm = FakeBackend(script=[accept], delay=0)
    link_cm, dialogue, chronicle, vanilla = await _setup(fake, llm, graph, store)
    fake.vars.update({QUEST: 2, KILL: 1})
    fake.npc_states[BANDIT] = {"state": "dead", "name": "Бандит", "distance": 500, "seen": True}
    try:
        conv = dialogue.open(LOBART, "Лобарт")
        await vanilla.prepare(LOBART)
        assert "DIA_HILDA_HALLO" in vanilla.offers[LOBART][KILL].claim.knows
        result = await vanilla.handle(
            dialogue, conv,
            ToolCall("amend_quest", {
                "quest": KILL, "drop": ["DIA_HILDA_HALLO"], "why": "Я сам ей расскажу",
            }, "m1"),
            None,
        )
        assert result.status == "accepted"
        assert vanilla.offers[LOBART][KILL].claim.knows == []
        await dialogue.say_as_player(LOBART, "С бандитом покончено.")
        await _until(lambda: chronicle.facts("quest_outcome") != [])
        assert {"do": "set", "var": KILL, "value": 2} in _effects(fake, LOBART)[0]
        assert chronicle.facts("lie") == [] and "DIA_HILDA_HALLO" not in fake.known
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_accepting_without_pay_keeps_experience_continuation_and_takes_items(greg):
    greg.nodes[GREG_END].effects += [
        {"do": "set", "var": TO_HILDA, "value": ["int", 1]},
        {"do": "journal_status", "topic": GREG_TOPIC, "status": ["int", 2, "LOG_SUCCESS"]},
        {"do": "create_items", "npc": ["inst", "OTHER"], "item": ["sym", "ITFO_BREAD"], "amount": ["int", 2]},
    ]
    fake = FakeGame(branch="A")
    fake.hero_items = {"ITAR_BAU_L": 1}
    accept = FakeStep(
        "Засчитываю, но платить не стану.",
        [ToolCall("accept_quest", {"quest": GREG_QUEST, "pay": "none"}, "a1")],
    )
    llm = FakeBackend(script=[accept, FakeStep("Прощай.")], delay=0)
    link_cm, dialogue, chronicle, vanilla = await _greg(fake, llm, greg, running=1)
    try:
        conv = dialogue.open(GREG, "Грег")
        await vanilla.prepare(GREG)
        assert "опыт" in vanilla.situation(GREG) and "в любом случае" in vanilla.situation(GREG)
        result = await vanilla.handle(
            dialogue, conv, ToolCall("accept_quest", {"quest": GREG_QUEST, "pay": "invalid"}, "x1"), None,
        )
        assert result.status == "rejected"
        await dialogue.say_as_player(GREG, "Вот одежда.")
        await _until(lambda: chronicle.facts("quest_outcome") != [])
        effects = _effects(fake, GREG)[0]
        assert {"do": "give", "item": "ITAR_BAU_L", "count": 1, "to": "self"} in effects
        assert not any(e["do"] == "give" and e.get("to") != "self" for e in effects)
        assert {"do": "xp", "amount": 100} in effects
        assert {"do": "set", "var": GREG_QUEST, "value": 2} in effects
        assert {"do": "set", "var": TO_HILDA, "value": 1} in effects
        assert {"do": "journal", "topic": GREG_TITLE, "text": "", "status": "LOG_SUCCESS"} in effects
        assert chronicle.facts("vanilla_reward") == []
        assert not any(item in fake.hero_items for item in GREG_BONUS)
        assert chronicle.facts("quest_outcome") == [{
            "fact": "quest_outcome", "quest": GREG_QUEST, "npc": GREG, "how": "done",
            "drop": [], "why": [], "pay": "none",
        }]
        assert "опыт" in str(conv.history) and "в любом случае" in str(conv.history)
        for reload in (False, True):
            if reload:
                vanilla._load()
            chronicle.record({"fact": "vanilla_quest", "quest": GREG_QUEST, "to": "success", "giver": GREG})
            await _until(lambda: not vanilla._background)
            assert chronicle.facts("vanilla_reward") == []
            assert len(_effects(fake, GREG)) == 1
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_dropping_a_quest_closes_its_journal_without_an_ending_or_reward(graph):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    drop = FakeStep(
        "Больше не нужно.", [ToolCall("drop_quest", {"quest": QUEST, "why": "Урожай пропал"}, "d1")]
    )
    llm = FakeBackend(script=[drop], delay=0)
    link_cm, dialogue, chronicle, vanilla = await _setup(fake, llm, graph, store)
    entries = []
    vanilla.journal = Journal(entries.append)
    try:
        dialogue.open(LOBART, "Лобарт")
        await dialogue.say_as_player(LOBART, "Что с репой?")
        await _until(lambda: chronicle.facts("quest_outcome") != [])
        effects = _effects(fake, LOBART)[0]
        assert {"do": "set", "var": QUEST, "value": 4} in effects
        assert {"do": "journal", "topic": "Убрать поле репы", "text": "", "status": "LOG_OBSOLETE"} in effects
        assert not any(e["do"] in ("xp", "give") for e in effects)
        assert not any(e.get("var") == TO_HILDA for e in effects)
        assert any(e["kind"] == "quest_dropped" for e in entries)
        assert chronicle.facts("claim") == [] and chronicle.facts("vanilla_reward") == []
        assert chronicle.facts("quest_outcome") == [{
            "fact": "quest_outcome", "quest": QUEST, "npc": LOBART, "how": "dropped",
            "drop": [], "why": ["Урожай пропал"], "pay": "none",
        }]
        fake.vars[QUEST] = 4  # FakeGame retains unchecked effects as messages only.
        vanilla._load()
        await vanilla.prepare(LOBART)
        assert QUEST not in vanilla.offers[LOBART] and vanilla.tools(LOBART) == []
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_dropping_a_quest_leaves_a_shared_journal_topic_open(graph):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    drop = FakeStep(
        "Больше не нужно.", [ToolCall("drop_quest", {"quest": QUEST, "why": "Урожай пропал"}, "d1")]
    )
    llm = FakeBackend(script=[drop], delay=0)
    link_cm, dialogue, chronicle, vanilla = await _setup(fake, llm, graph, store)
    for topic in graph.quests[QUEST].topics:
        vanilla._topic_uses[topic] = 2  # another quest writes to it too
    try:
        dialogue.open(LOBART, "Лобарт")
        await dialogue.say_as_player(LOBART, "Что с репой?")
        await _until(lambda: chronicle.facts("quest_outcome") != [])
        effects = _effects(fake, LOBART)[0]
        assert {"do": "set", "var": QUEST, "value": 4} in effects
        assert not any(e["do"] == "journal" for e in effects)
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize("drop_all", [False, True])
async def test_an_old_check_uses_terms_amended_during_its_walk(two_bandits, monkeypatch, drop_all):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    link_cm, dialogue, chronicle, vanilla = await _setup(fake, FakeBackend(delay=0), two_bandits, store)
    fake.instances["SECOND_BANDIT"] = "BDT_2_BANDIT"
    fake.vars.update({QUEST: 2, KILL: 1})
    fake.npc_states.update({
        BANDIT: {"state": "alive", "name": "Пощажённый", "distance": 4000, "seen": False},
        "BDT_2_BANDIT": {
            "state": "alive" if drop_all else "dead", "name": "Второй", "distance": 9000, "seen": False,
        },
    })
    cues = []
    monkeypatch.setattr(vanilla, "_react", lambda conv, cue: cues.append(cue))
    try:
        if not drop_all:
            chronicle.record({"fact": "seen_dead", "npc": LOBART, "dead": "BDT_2_BANDIT", "how": "killed"})
        await vanilla.link.attach(LOBART)
        conv = dialogue.open(LOBART, "Лобарт")
        await vanilla.prepare(LOBART)
        old = vanilla.offers[LOBART].pop(KILL)
        target = old.reachable()
        dropped = [BANDIT, "BDT_2_BANDIT"] if drop_all else [BANDIT]

        async def walk(conv, npc, radius):
            if npc == BANDIT:
                fake.npc_states[BANDIT].update(distance=1000, seen=True)
                await vanilla.prepare(LOBART)
                assert KILL not in vanilla.offers[LOBART] and vanilla.checking[LOBART] is old
                result = await vanilla.handle(
                    dialogue, conv,
                    ToolCall("amend_quest", {"quest": KILL, "drop": dropped, "why": "Пощадил"}, "m1"),
                    None,
                )
                assert result.status == "accepted"
                assert "accept_quest" not in result.output and "Можешь засчитать" not in result.output
                if drop_all:
                    assert "Ты уже идёшь посмотреть сам; что увидишь, то и решит." in result.output
                else:
                    assert old.needs in result.output
                assert old.claim.dead == ([] if drop_all else ["BDT_2_BANDIT"])
            return ""

        monkeypatch.setattr(vanilla, "_go", walk)
        note, ok = await vanilla._check(old, target, conv)
        assert ok and "Ты засчитал герою" in note
        assert old.claim.dead == ([] if drop_all else ["BDT_2_BANDIT"])
        assert chronicle.facts("caught") == [] and chronicle.facts("lie") == []
        assert chronicle.facts("claim")[0]["checked"] is True
        assert chronicle.facts("quest_outcome") == [{
            "fact": "quest_outcome", "quest": KILL, "npc": LOBART, "how": "amended",
            "drop": dropped, "why": ["Пощадил"], "pay": "full",
        }]
        assert {"do": "set", "var": KILL, "value": 2} in _effects(fake, LOBART)[0]
        assert cues and "Он тебе солгал" not in cues[0]
        assert LOBART not in vanilla.checking
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_an_old_check_does_not_play_an_ending_after_the_quest_was_dropped(graph, monkeypatch):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    link_cm, dialogue, chronicle, vanilla = await _setup(fake, FakeBackend(delay=0), graph, store)
    fake.vars.update({QUEST: 2, KILL: 1})
    fake.npc_states[BANDIT] = {"state": "alive", "name": "Бандит", "distance": 4000, "seen": False}
    cues = []
    entries = []
    vanilla.journal = Journal(entries.append)
    monkeypatch.setattr(vanilla, "_react", lambda conv, cue: cues.append(cue))
    try:
        await vanilla.link.attach(LOBART)
        conv = dialogue.open(LOBART, "Лобарт")
        await vanilla.prepare(LOBART)
        old = vanilla.offers[LOBART].pop(KILL)
        target = old.reachable()

        async def walk(conv, npc, radius):
            fake.npc_states[BANDIT].update(distance=1000, seen=True)
            await vanilla.prepare(LOBART)
            assert KILL not in vanilla.offers[LOBART] and vanilla.checking[LOBART] is old
            result = await vanilla.handle(
                dialogue, conv,
                ToolCall("drop_quest", {"quest": KILL, "why": "Нужда отпала"}, "d1"), None,
            )
            assert result.status == "accepted"
            await _until(lambda: chronicle.facts("quest_outcome") != [])
            await vanilla.prepare(LOBART)
            assert {t.name for t in vanilla.tools(LOBART)}.isdisjoint({"amend_quest", "drop_quest"})
            for name in ("drop_quest", "amend_quest"):
                repeated = await vanilla.handle(
                    dialogue, conv,
                    ToolCall(name, {"quest": KILL, "drop": [BANDIT], "why": "Нужда отпала"}, "again"),
                    None,
                )
                assert repeated.status == "rejected"
            return ""

        monkeypatch.setattr(vanilla, "_go", walk)
        note, ok = await vanilla._check(old, target, conv)
        assert ok and "уже закрыто" in note
        assert chronicle.facts("claim") == [] and chronicle.facts("caught") == []
        assert chronicle.facts("lie") == [] and cues == []
        assert len(chronicle.facts("quest_outcome")) == 1
        assert chronicle.facts("quest_outcome")[0]["how"] == "dropped"
        assert len([e for e in entries if e["kind"] == "quest_dropped"]) == 1
        effects = _effects(fake, LOBART)
        assert len(effects) == 1 and {"do": "set", "var": KILL, "value": 4} in effects[0]
        assert effects[0].count({
            "do": "journal", "topic": "Бандит у фермы", "text": "", "status": "LOG_OBSOLETE",
        }) == 1
        assert not any(e["do"] in ("xp", "give") for e in effects[0])
        assert LOBART not in vanilla.checking
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize("away", [0, 1])
async def test_an_optional_helper_branch_does_not_block_witnessed_amended_terms(cavalorn, away):
    cavalorn.nodes[KILLED].condition[2] = ["==", ["var", KILLBRAGO], ["int", 1]]
    cavalorn.instances.update({"SECOND_BANDIT": "BDT_2_BANDIT", "THIRD_BANDIT": "BDT_3_BANDIT"})
    first = ["call", "NPC_ISDEAD", ["inst", "AMBUSHER_1013"]]
    second = ["call", "NPC_ISDEAD", ["inst", "SECOND_BANDIT"]]
    third = ["call", "NPC_ISDEAD", ["inst", "THIRD_BANDIT"]]
    cavalorn.functions["C_BRAGOBANDITSDEAD"]["body"] = [
        "||", ["&&", first, second, third],
        ["&&", second, third, ["==", ["var", "BDT_1013_AWAY"], ["int", 1]]],
    ]
    fake = FakeGame(branch="A")
    accept = FakeStep("С ними покончено.", [ToolCall("accept_quest", {"quest": KILLBRAGO}, "a1")])
    llm = FakeBackend(script=[accept, FakeStep("Держи плату.")], delay=0)
    link_cm, dialogue, chronicle, vanilla = await _cavalorn(fake, llm, cavalorn)
    fake.vars.update({KILLBRAGO: 1, "BDT_1013_AWAY": away})
    fake.instances.update({"SECOND_BANDIT": "BDT_2_BANDIT", "THIRD_BANDIT": "BDT_3_BANDIT"})
    fake.npc_states.update({
        BRAGO: {"state": "alive", "name": "Пощажённый", "distance": 500, "seen": True},
        "BDT_2_BANDIT": {"state": "dead", "name": "Второй", "distance": 9000, "seen": False},
        "BDT_3_BANDIT": {"state": "dead", "name": "Третий", "distance": 9000, "seen": False},
    })
    fake.fns["C_BRAGOBANDITSDEAD"] = away
    try:
        chronicle.record({"fact": "caught", "npc": CAVALORN, "quest": KILLBRAGO})
        for dead in ("BDT_2_BANDIT", "BDT_3_BANDIT"):
            chronicle.record({"fact": "seen_dead", "npc": CAVALORN, "dead": dead, "how": "saw_die"})
        conv = dialogue.open(CAVALORN, "Кавалорн")
        await vanilla.prepare(CAVALORN)
        result = await vanilla.handle(
            dialogue, conv,
            ToolCall("amend_quest", {"quest": KILLBRAGO, "drop": [BRAGO], "why": "Пощадил"}, "m1"),
            None,
        )
        assert result.status == "accepted" and vanilla.trust.broken(CAVALORN)
        offer = vanilla.offers[CAVALORN][KILLBRAGO]
        assert offer.claim.dead == ["BDT_2_BANDIT", "BDT_3_BANDIT"] and offer.seen_true()
        assert "check_claim" not in {t.name for t in vanilla.tools(CAVALORN)}
        await dialogue.say_as_player(CAVALORN, "С ними покончено.")
        await _until(lambda: chronicle.facts("quest_outcome") != [])
        effects = _effects(fake, CAVALORN)[0]
        assert {"do": "set", "var": KILLBRAGO, "value": 2} in effects
        assert {"do": "set", "var": LETTER, "value": 1} in effects
        assert not any(e.get("var") == "BDT_1013_AWAY" for e in effects)
        assert fake.vars["BDT_1013_AWAY"] == away and fake.npc_states[BRAGO]["state"] == "alive"
        assert chronicle.facts("lie") == []
        assert chronicle.facts("quest_outcome")[0]["how"] == "amended"
    finally:
        await link_cm.__aexit__(None, None, None)
