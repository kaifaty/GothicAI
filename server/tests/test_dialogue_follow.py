import pytest

from gothic_ai.dialogue import DialogueManager, follow_agreement
from gothic_ai.executor import Intent
from gothic_ai.journal import Journal
from gothic_ai.llm import FakeBackend, ToolCall, ToolResult
from gothic_ai.llm.fake import FakeStep
from gothic_ai.persona import PersonaProvider
from tests.fake_game import FakeGame, connected
from tests.test_dialogue import _until

CAVALORN = "BAU_4300_ADDON_CAVALORN"


@pytest.mark.parametrize(
    ("heard", "reply"),
    [
        (
            "Пойдем, бодитов, пиздеть.",
            "Вот это по делу. Вниз по дороге прячутся бандиты, которые меня ограбили — "
            "пойдём разберёмся с ними вместе. Только держись рядом и прикрывай мне спину.",
        ),
        ("Ну ты идешь, нет?", "Иду, прикроешь мне спину."),
        ("Давай за мной.", "Ладно, я за тобой."),
        ("Кавалорн, давай, иди за мной.", "Веди, я за тобой. Только не лезь вперёд без оглядки."),
        (
            "Ну, ты, если что, не туда идешь, ты в стену куда-то идешь. Иди за мной.",
            "Ладно, веди. Только далеко не уходи: мне ещё нужно осмотреть бандитов, "
            "прежде чем окончательно рассчитаться с тобой.",
        ),
        ("Пойдём к бандитам.", "Хорошо, веди."),
        ("Пойдём к бандитам.", "Веди."),
        ("Пойдём к бандитам.", "Хорошо, ведите!"),
        ("Пойдём к бандитам.", "Ну, тогда веди уж."),
        ("Пойдём к бандитам.", "Давай, веди же!"),
    ],
)
async def test_a_spoken_follow_agreement_repairs_the_missing_intent(heard, reply):
    fake = FakeGame()
    llm = FakeBackend(reply, delay=0)
    entries = []
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None), journal=Journal(entries.append))
        await link.attach(CAVALORN)
        conv = dialogue.open(CAVALORN, "Кавалорн")
        conv.intent = Intent(attitude="friendly", agenda="Help with the bandits")
        await dialogue.say_as_player(CAVALORN, heard)
        await _until(lambda: any(m.get("action") == "follow" for m in fake.received))

    assert conv.intent == Intent("follow_hero", "PLAYER", "friendly", "Help with the bandits")
    note = next(m for m in conv.history if "Ты согласился и идёшь за героем" in m.content)
    assert heard in note.content
    assert any(m.role == "assistant" and m.content == reply for m in conv.history)
    calls = [e for e in entries if e.get("action") == "set_intent"]
    assert len(calls) == 1


@pytest.mark.parametrize(
    ("heard", "reply"),
    [
        ("Что будем делать с бандитами?", "Пойдём, разберёмся с ними вместе."),
        ("Расскажи про своё дело.", "Пойдём разберёмся с ними вместе."),
        ("Куда ты идешь?", "Иду, прикроешь мне спину."),
        ("Он сказал: пойдем к бандитам.", "Пошли."),
        ("Не пойдем туда.", "Ладно, пойдем."),
        ("Пойдем к бандитам.", "Не пойду, я остаюсь здесь."),
        ("Пошли к пещере.", "Иди, я останусь у костра."),
        ("Пойдем к бандитам.", "Пойдем, если заплатишь мне сто золотых."),
        ("Пойдем к бандитам.", "Сначала принеси воды, тогда пойдем."),
        ("Пойдем к бандитам.", "Я бы пошел, но позже."),
        ("Пойдем к бандитам.", "Не думаю, что пойдем туда вместе."),
        ("Пойдем к бандитам.", "Может, пойдем, а может, нет."),
        ("Пойдем к бандитам.", "Не сейчас. Пойдем завтра."),
        ("Пойдем к бандитам.", "Пойдем без меня, я занят."),
        ("Пойдем к бандитам.", "Я думаю, что мы пойдем туда вместе."),
        ("Пойдем к бандитам.", "Ты хочешь, чтобы я за тобой шёл?"),
        ("Пойдем к бандитам.", "Пойдем?"),
        ("Ну ты идешь, нет?", "Иду к Лобарту. Ты разбирайся с бандитами сам."),
        ("Пойдем к бандитам.", "Ты сказал «пойдем», а я ничего не обещал."),
        ("Пойдем к бандитам.", "Да, давай поговорим об этом."),
        ("Пойдём к бандитам.", "Пошли за мной, я знаю дорогу."),
        ("Пойдём к бандитам.", "Идём, я поведу."),
        ("Ну ты идёшь, нет?", "Иду, но без тебя."),
        ("Пошли слухи, что бандиты вернулись.", "Пойдём проверим."),
        ("Пойдём ли мы завтра к бандитам?", "Иду, прикроешь мне спину."),
        ("Пойдём к бандитам.", "Веди его к Лобарту."),
        ("Пойдём к бандитам.", "Веди сам, я останусь."),
        ("Пойдём к бандитам.", "Веди, а я тут подожду."),
        ("Пойдём к бандитам.", "Веди?"),
        ("Иди за мной, сказал он.", "Ладно, веди."),
    ],
)
async def test_other_words_do_not_make_a_follow_agreement(heard, reply):
    assert not follow_agreement(heard, reply, "Кавалорн")
    fake = FakeGame()
    async with connected(fake) as link:
        dialogue = DialogueManager(link, FakeBackend(reply, delay=0), PersonaProvider(None))
        conv = dialogue.open(CAVALORN, "Кавалорн")
        await dialogue.say_as_player(CAVALORN, heard)

    assert conv.intent.goal == "none"
    assert not any(m.get("action") == "follow" for m in fake.received)


async def test_the_models_follow_intent_is_not_applied_twice():
    fake = FakeGame()
    llm = FakeBackend(
        "Иду, прикроешь мне спину.",
        delay=0,
        tool_calls=[ToolCall("set_intent", {"goal": "follow_hero", "target": "PLAYER"})],
    )
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None))
        await link.attach(CAVALORN)
        conv = dialogue.open(CAVALORN, "Кавалорн")
        await dialogue.say_as_player(CAVALORN, "Ну ты идешь, нет?")
        await _until(lambda: any(m.get("action") == "follow" for m in fake.received))

    assert conv.intent.goal == "follow_hero"
    assert len([m for m in fake.received if m.get("action") == "follow"]) == 1
    assert not any("Ты согласился и идёшь за героем" in m.content for m in conv.history)


@pytest.mark.parametrize("goal", ["follow_hero", "guard_hero"])
@pytest.mark.parametrize("body", ["walking", "following", "fighting", "unknown"])
async def test_the_models_repeated_companion_intent_resumes_following(goal, body):
    fake = FakeGame()
    llm = FakeBackend(
        script=[
            FakeStep(tool_calls=[ToolCall("set_intent", {"goal": goal, "target": "PLAYER"})]),
            FakeStep("Ладно, возвращаюсь."),
        ],
        delay=0,
    )
    held = []

    async def take_body(npc, why):
        held.append((npc, why))
        return True

    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None))
        dialogue.take_body = take_body
        dialogue.fighting = lambda npc: body == "fighting"
        await link.attach(CAVALORN)
        conv = dialogue.open(CAVALORN, "Кавалорн")
        conv.intent = Intent(goal, "PLAYER")
        snap = await link.snapshot(CAVALORN, fresh=True)
        link.snapshots[CAVALORN] = snap.model_copy(update={
            "npc": snap.npc.model_copy(update={
                "busy": True, "action": "go_to_npc", "following": body == "following",
            }),
        })
        if body == "unknown":
            link.snapshots.pop(CAVALORN)

        await dialogue.say_as_player(CAVALORN, "Хватит бродить, возвращайся ко мне")
        await _until(lambda: not conv.actions)

    follows = [m for m in fake.received if m.get("action") == "follow"]
    assert len(follows) == (1 if body == "walking" else 0)
    result = next(m for m in llm.calls[1][1] if isinstance(m, ToolResult))
    if body == "walking":
        assert follows[0]["args"] == {"npc": "PLAYER"}
        assert held == [(CAVALORN, "follow")]
        assert result.output.startswith("Принято:")
    else:
        assert held == []
        assert result.output == "Намерение не изменилось."


@pytest.mark.parametrize("model_calls", [0, 1, 2])
@pytest.mark.parametrize("goal", ["follow_hero", "guard_hero"])
async def test_a_follow_agreement_recalls_a_companion_whose_body_is_walking(model_calls, goal):
    fake = FakeGame()
    calls = [ToolCall("set_intent", {"goal": goal, "target": "PLAYER"}) for _ in range(model_calls)]
    llm = FakeBackend("Иду, прикроешь мне спину.", delay=0, tool_calls=calls)
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None))
        await link.attach(CAVALORN)
        conv = dialogue.open(CAVALORN, "Кавалорн")
        conv.intent = Intent(goal, "PLAYER")
        snap = await link.snapshot(CAVALORN, fresh=True)
        link.snapshots[CAVALORN] = snap.model_copy(update={
            "npc": snap.npc.model_copy(update={
                "busy": True, "action": "go_to_npc", "following": False,
            }),
        })

        await dialogue.say_as_player(CAVALORN, "Ну ты идешь, нет?")
        await _until(lambda: not conv.actions)

    assert len([m for m in fake.received if m.get("action") == "follow"]) == 1
    notes = [m for m in conv.history if "Ты согласился и идёшь за героем" in m.content]
    assert len(notes) == (0 if model_calls else 1)


@pytest.mark.parametrize("goal", ["follow_hero", "guard_hero"])
@pytest.mark.parametrize("update", [{"agenda": "Help the hero with the bandits"}, {"attitude": "friendly"}])
async def test_updating_a_companion_intent_does_not_repeat_follow(goal, update):
    fake = FakeGame()
    arguments = {"goal": goal, "target": "PLAYER"}
    llm = FakeBackend(
        "Ладно, возвращаюсь.",
        delay=0,
        tool_calls=[ToolCall("set_intent", arguments), ToolCall("set_intent", arguments | update)],
    )
    changed = []
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None))
        dialogue.on_intent = lambda npc, intent: changed.append(intent)
        await link.attach(CAVALORN)
        conv = dialogue.open(CAVALORN, "Кавалорн")
        conv.intent = Intent(goal, "PLAYER")
        snap = await link.snapshot(CAVALORN, fresh=True)
        link.snapshots[CAVALORN] = snap.model_copy(update={
            "npc": snap.npc.model_copy(update={
                "busy": True, "action": "go_to_npc", "following": False,
            }),
        })

        await dialogue.say_as_player(CAVALORN, "Хватит бродить, возвращайся ко мне")
        await _until(lambda: not conv.actions)

        expected = Intent(goal, "PLAYER", **update)
        assert conv.intent == expected
        assert changed == [expected]
        assert Intent.from_json(dialogue.memory.load(conv.branch, CAVALORN).intent) == expected

    follows = [m for m in fake.received if m.get("action") == "follow"]
    assert len(follows) == 1
    assert follows[0]["args"] == {"npc": "PLAYER"}


async def test_an_event_turn_cannot_make_a_follow_agreement():
    fake = FakeGame()
    async with connected(fake) as link:
        dialogue = DialogueManager(link, FakeBackend("Пошли.", delay=0), PersonaProvider(None))
        conv = dialogue.open(CAVALORN, "Кавалорн")
        await dialogue.event_turn(CAVALORN, "Пойдём к бандитам.")

    assert conv.intent.goal == "none"
    assert not any(m.get("action") == "follow" for m in fake.received)
