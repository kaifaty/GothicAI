import asyncio

from gothic_ai import executor
from gothic_ai.dialogue import DialogueManager
from gothic_ai.executor import Intent
from gothic_ai.llm import FakeBackend, FakeStep, ToolCall, ToolResult
from gothic_ai.memory import MemoryStore
from gothic_ai.persona import PersonaProvider
from gothic_ai.protocol import Snapshot
from tests.fake_game import FakeGame, connected
from tests.test_dialogue import XARDAS, _until

SNAP = Snapshot.model_validate(FakeGame().snapshot(XARDAS))
FOLLOW = {
    "goal": "follow_hero",
    "target": None,
    "attitude": "friendly",
    "agenda": "wants the hero to visit Hagen",
}


def test_give_is_checked_against_the_inventory():
    args, reason = executor.check_give({"item": "itpo_health_01", "count": 10}, SNAP)
    assert args == {"item": "ITPO_HEALTH_01", "count": 3} and reason is None  # capped by what the npc has

    args, reason = executor.check_give({"item": "ITSC_FIREBALL", "count": 1}, SNAP)
    assert (
        args is None
        and "нет предмета ITSC_FIREBALL" in reason
        and "Эликсир лечения (ITPO_HEALTH_01) ×3" in reason
    )

    args, reason = executor.check_give({"item": "ITMW_ZWEIHAENDER1", "count": 1}, SNAP)
    assert args is None  # equipped: not for giving away


def test_intent_targets_must_be_around():
    intent, reason = executor.check_intent(Intent("seek_help"), SNAP)
    assert intent is not None and intent.target == "PLAYER"  # help from the hero unless told otherwise

    intent, reason = executor.check_intent(Intent("seek_help", target="pc_thief"), SNAP)
    assert intent is not None and intent.target == "PC_THIEF"

    intent, reason = executor.check_intent(Intent("flee", target="ORC_WARRIOR"), SNAP)
    assert intent is None and "ORC_WARRIOR" in reason and "PC_THIEF (Диего, 6 м)" in reason


def test_hero_fight_reason_is_checked_after_target_normalisation():
    intent, reason = executor.check_intent(
        Intent("fight", target="player"), SNAP, fight_reason=lambda: "сначала потребуй — ultimatum",
    )
    assert intent is None and reason == "сначала потребуй — ultimatum"
    intent, reason = executor.check_intent(Intent("fight", target="PLAYER"), SNAP, fight_reason=lambda: None)
    assert intent is not None and reason is None
    intent, reason = executor.check_intent(
        Intent("fight", target="PC_THIEF"), SNAP, fight_reason=lambda: "must not be checked",
    )
    assert intent is not None and reason is None


def test_intent_actions():
    assert executor.intent_actions(Intent(), Intent("follow_hero"), following=False) == [
        ("follow", {"npc": "PLAYER"})
    ]
    assert executor.intent_actions(Intent("follow_hero"), Intent("follow_hero"), following=True) == []
    assert executor.intent_actions(Intent("follow_hero"), Intent("keep_routine"), following=True) == [
        ("stop", {})
    ]
    assert executor.intent_actions(Intent(), Intent("seek_help", target="PC_THIEF"), following=False) == [
        ("go_to_npc", {"npc": "PC_THIEF"})
    ]
    assert executor.intent_actions(Intent(), Intent("fight"), following=False) == []  # the reflexes' job


def test_a_goal_there_is_no_more_reads_as_none():
    from gothic_ai.npc_tools import TOOLS

    saved = Intent.from_json('{"goal":"change_plans","attitude":"wary"}')  # remembered before it went
    assert (saved.goal, saved.attitude) == ("none", "wary")
    assert saved.describe_en().startswith("You want nothing in particular.")
    assert "change_plans" not in TOOLS[0].parameters["properties"]["goal"]["enum"]


async def _turn(
    llm: FakeBackend, fake: FakeGame, store: MemoryStore | None = None, text: str = "Дай мне что-нибудь"
):
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None), memory=store)
        changed: list[Intent] = []
        dialogue.on_intent = lambda npc, intent: changed.append(intent)
        await link.attach(XARDAS)
        dialogue.open(XARDAS, "Ксардас")
        await asyncio.wait_for(dialogue.say_as_player(XARDAS, text), 5)
        await _until(lambda: not dialogue.conversations[XARDAS].actions)
        await _until(lambda: any(m["type"] == "chat_text" and m.get("final") for m in fake.received))
    return dialogue, changed


async def test_a_rejected_give_comes_back_within_the_turn_and_never_reaches_the_game():
    llm = FakeBackend(
        script=[
            FakeStep(
                "Держи свиток огненного шара.", [ToolCall("give_item", {"item": "ITSC_FIREBALL", "count": 1})]
            ),
            FakeStep("Хотя нет, такого свитка у меня нет."),
        ],
        delay=0,
    )
    fake = FakeGame()
    dialogue, _ = await _turn(llm, fake)

    assert len(llm.calls) == 2
    result = next(i for i in llm.calls[1][1] if isinstance(i, ToolResult))
    assert result.output.startswith("Отклонено: У тебя нет предмета ITSC_FIREBALL")
    assert result.output.endswith(
        " Ты уже сказал вслух: «Держи свиток огненного шара.». "
        "Если отказ этого не меняет — ничего не говори. "
        "Если меняет — поправься одной короткой фразой, не повторяя сказанного."
    )
    assert not [m for m in fake.received if m["type"] == "action"]
    assert fake.said[-1] == "Хотя нет, такого свитка у меня нет."
    history = dialogue.conversations[XARDAS].history
    assert history[-1].content == "Держи свиток огненного шара. Хотя нет, такого свитка у меня нет."


async def test_a_rejection_before_any_words_keeps_its_original_output():
    llm = FakeBackend(
        script=[
            FakeStep("", [ToolCall("give_item", {"item": "ITSC_FIREBALL", "count": 1})]),
            FakeStep("Такого свитка у меня нет."),
        ],
        delay=0,
    )
    await _turn(llm, FakeGame())

    assert len(llm.calls) == 2
    result = next(i for i in llm.calls[1][1] if isinstance(i, ToolResult))
    _, reason = executor.check_give({"item": "ITSC_FIREBALL", "count": 1}, SNAP)
    assert result.output == f"Отклонено: {reason}"


async def test_a_rejection_quotes_all_spoken_steps_without_mood_tags():
    llm = FakeBackend(
        script=[
            FakeStep("[neutral] Хм…", [ToolCall("recall", {"query": "свиток"})]),
            FakeStep(
                "[friendly] Держи свиток. Пользуйся.",
                [ToolCall("give_item", {"item": "ITSC_FIREBALL", "count": 1})],
            ),
            FakeStep("Такого свитка у меня нет."),
        ],
        delay=0,
    )
    await _turn(llm, FakeGame())

    assert len(llm.calls) == 3
    results = [i for i in llm.calls[2][1] if isinstance(i, ToolResult)]
    assert "Ты уже сказал вслух" not in results[0].output  # recall is unchanged
    assert "Ты уже сказал вслух: «Хм… Держи свиток. Пользуйся.»" in results[1].output


async def test_rejections_stop_after_three_steps_and_the_last_one_only_talks():
    bad = ToolCall("give_item", {"item": "ITSC_FIREBALL", "count": 1})
    llm = FakeBackend(script=[FakeStep("Держи.", [bad])], delay=0)  # keeps trying
    await _turn(llm, FakeGame())
    assert len(llm.calls) == 3
    assert llm.tool_choices == [None, None, "none"]


async def test_follow_intent_moves_the_body_after_the_line_and_is_remembered():
    store = MemoryStore(":memory:")
    llm = FakeBackend("Хорошо, я пойду с тобой.", tool_calls=[ToolCall("set_intent", FOLLOW)], delay=0)
    fake = FakeGame()
    dialogue, changed = await _turn(llm, fake, store, "Пойдём со мной")

    kinds = [(m["type"], m.get("action")) for m in fake.received if m["type"] in ("say", "action")]
    assert kinds == [("say", None), ("action", "follow")]  # the line first, then the body
    intent = dialogue.conversations[XARDAS].intent
    assert intent.goal == "follow_hero" and intent.attitude == "friendly" and changed == [intent]
    assert len(llm.calls) == 1  # accepted with words: no second step

    # a load of a save made after this keeps the intent
    head = store.head("test-branch")
    store.ensure_branch("after-load", "test-branch", head)
    assert Intent.from_json(store.load("after-load", XARDAS).intent or "") == intent

    # and the next turn shows it to the model
    again = FakeBackend("Идём.", delay=0)
    await _turn(again, FakeGame(), store, "Ну что?")
    situation = again.calls[0][1][-2].content
    assert "Твоё намерение: цель — идти за героем" in situation


async def test_the_same_intent_twice_is_not_carried_out_twice():
    llm = FakeBackend(
        "Идём.", tool_calls=[ToolCall("set_intent", FOLLOW), ToolCall("set_intent", FOLLOW)], delay=0
    )
    fake = FakeGame()
    await _turn(llm, fake)
    assert [m["action"] for m in fake.received if m["type"] == "action"] == ["follow"]


async def test_recall_brings_back_what_the_summary_lost():
    store = MemoryStore(":memory:")
    store.ensure_branch("test-branch")
    store.append(
        "test-branch", XARDAS, "user", "Меня зовут Ронан, я ищу своего брата Бенно.", "день 1, 09:00"
    )
    store.append("test-branch", XARDAS, "assistant", "Брата ищи в городе.", "день 1, 09:00")
    store.add_summary("test-branch", XARDAS, "Герой пришёл к тебе в первый день.", covers=2)
    llm = FakeBackend(
        script=[
            FakeStep("Хм…", [ToolCall("recall", {"query": "брат героя"})]),
            FakeStep("Ты искал брата, Бенно. Так ты его нашёл?"),
        ],
        delay=0,
    )
    dialogue, _ = await _turn(llm, FakeGame(), store, "Помнишь, кого я искал?")

    assert len(llm.calls) == 2
    result = next(i for i in llm.calls[1][1] if isinstance(i, ToolResult))
    assert "[день 1, 09:00] Герой: Меня зовут Ронан, я ищу своего брата Бенно." in result.output
    assert "Ты уже сказал вслух" not in result.output
    history = dialogue.conversations[XARDAS].history
    assert history[-1].content == "Хм… Ты искал брата, Бенно. Так ты его нашёл?"
