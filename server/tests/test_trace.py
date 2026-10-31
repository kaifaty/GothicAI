import asyncio

from gothic_ai.dialogue import DialogueManager
from gothic_ai.llm import FakeBackend, FakeStep, ToolCall
from gothic_ai.persona import PersonaProvider
from gothic_ai.replay import replay
from gothic_ai.trace import TraceWriter, find_turn, read
from tests.fake_game import FakeGame, connected
from tests.test_dialogue import XARDAS, _until

GIVE = {"item": "ITPO_HEALTH_01", "count": 1}
FOLLOW = {"goal": "follow_hero", "target": None, "attitude": "friendly", "agenda": ""}


async def _talk(llm: FakeBackend, writer: TraceWriter, *phrases: str) -> DialogueManager:
    fake = FakeGame()
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None), trace=writer)
        await link.attach(XARDAS)
        dialogue.open(XARDAS, "Ксардас")
        for text in phrases:
            await asyncio.wait_for(dialogue.say_as_player(XARDAS, text), 5)
        await _until(lambda: not dialogue.conversations[XARDAS].actions)
    return dialogue


async def test_every_turn_is_traced_with_its_instructions_once(tmp_path):
    writer = TraceWriter(tmp_path)
    llm = FakeBackend("Держи, это поможет.", tool_calls=[ToolCall("give_item", GIVE)], delay=0)
    await _talk(llm, writer, "Пойдём со мной", "Ещё раз")

    records = read(writer.path())
    assert [r["type"] for r in records] == ["instructions", "turn", "turn"]  # instructions only once
    turn = records[1]
    assert turn["npc"] == XARDAS and turn["trigger"] == {"kind": "player", "text": "Пойдём со мной"}
    assert turn["instructions"] == records[0]["hash"] and turn["status"] == "completed"
    assert turn["snapshot"]["npc"]["id"] == XARDAS
    (step,) = turn["steps"]
    assert step["added"][-1] == {"role": "user", "content": "Пойдём со мной"}  # the whole request
    assert step["text"] == "Держи, это поможет."
    assert 0 <= step["started_s"] <= step["ended_s"] <= turn["total_s"] + 0.001
    assert [(c["name"], c["outcome"]) for c in step["calls"]] == [("give_item", "accepted")]


async def test_a_turn_without_words_goes_on_and_keeps_the_players_phrase(tmp_path):
    writer = TraceWriter(tmp_path)
    llm = FakeBackend(
        script=[FakeStep("", [ToolCall("set_intent", FOLLOW)]), FakeStep("Идём. Не отставай.")], delay=0
    )
    dialogue = await _talk(llm, writer, "Пойдём со мной")

    assert len(llm.calls) == 2
    second_request = llm.calls[1][1]
    assert any(getattr(i, "output", None) for i in second_request)  # the call's result went back
    history = [(m.role, m.content) for m in dialogue.conversations[XARDAS].history]
    assert ("user", "Пойдём со мной") in history and ("assistant", "Идём. Не отставай.") in history
    (turn,) = [r for r in read(writer.path()) if r["type"] == "turn"]
    assert len(turn["steps"]) == 2
    assert turn["steps"][1]["added"][-1]["type"] == "function_call_output"


async def test_replay_runs_a_recorded_turn_on_another_model(tmp_path):
    writer = TraceWriter(tmp_path)
    await _talk(FakeBackend("Слушай внимательно.", delay=0), writer, "Что мне делать?")
    record, instructions = find_turn(writer.path(), None)
    assert instructions

    other = FakeBackend("Иди в город к лорду Хагену.", delay=0)
    result = await replay(other, record, instructions)

    assert result["original"][0]["text"] == "Слушай внимательно."
    assert result["replay"][0]["text"] == "Иди в город к лорду Хагену."
    assert not result["diverged"]
    sent = other.calls[0][1]
    assert sent[-1].content == "Что мне делать?"  # the same request as recorded
