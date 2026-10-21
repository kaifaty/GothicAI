"""The scripted cave scene exercises rejected calls and the real deal tool."""

from gothic_ai.llm import Message, Tool, ToolCall, ToolResult
from gothic_ai.scenarios.bandit_parley import GREETING, PAYMENT, _bandit_script


async def calls(model, message):
    return [event async for event in model.stream(
        instructions="You are Brago", messages=[message],
        tools=[Tool(name, "") for name in ("set_intent", "ultimatum", "propose_deal")],
    ) if isinstance(event, ToolCall)]


async def test_parley_script_repairs_first_attack_in_same_turn():
    model = _bandit_script()
    first = await calls(model, Message("user", GREETING))
    assert [call.name for call in first] == ["set_intent"]
    assert first[0].arguments["target"] == "PLAYER"

    repaired = await calls(model, ToolResult(
        "first", "Отклонено: Без повода сразу не нападаешь: сначала потребуй — ultimatum.",
    ))
    assert [call.name for call in repaired] == ["ultimatum"]
    assert repaired[0].arguments["seconds"] == 10
    assert not await calls(model, Message("user", "[Срок вышел: герой не подчинился]"))


async def test_parley_script_offers_truce_for_gold_and_respects_its_rejection():
    model = _bandit_script()
    offered = await calls(model, Message("user", PAYMENT))
    assert [call.name for call in offered] == ["propose_deal"]
    assert offered[0].arguments["hero_gives"] == [{"item": "ITMI_GOLD", "count": 100}]
    assert offered[0].arguments["truce"] == {"hours": 4}
    assert not await calls(model, ToolResult(
        "mate", "Отклонено: У вас уговор: героя не трогать без нового повода.",
    ))
