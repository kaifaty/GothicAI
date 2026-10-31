"""The cave bounty script uses the real secret and guide deal terms."""

from gothic_ai.llm import Message, Tool, ToolCall, ToolResult
from gothic_ai.scenarios.bandit_bounty import ASK, DESTINATION, LEAD, NAME, PAYMENT, _bounty_script


async def calls(model, message):
    return [event async for event in model.stream(
        instructions="You are Brago", messages=[message],
        tools=[Tool(name, "") for name in ("tell_secret", "propose_deal")],
    ) if isinstance(event, ToolCall)]


async def test_bounty_script_asks_before_and_after_real_payment():
    model = _bounty_script()
    unpaid = await calls(model, Message("user", ASK))
    assert [(call.name, call.arguments) for call in unpaid] == [
        ("tell_secret", {"secret": "client_name"}),
    ]
    assert not await calls(model, ToolResult("unpaid", "Отклонено: нужен настоящий рычаг."))
    offered = await calls(model, Message("user", PAYMENT))
    assert [(call.name, call.arguments) for call in offered] == [
        ("propose_deal", {
            "npc_gives": [], "hero_gives": [{"item": "ITMI_GOLD", "count": 150}],
            "secret": "client_name",
        }),
    ]
    paid = await calls(model, Message("user", NAME))
    assert [(call.name, call.arguments) for call in paid] == [
        ("tell_secret", {"secret": "client_name"}),
    ]


async def test_bounty_script_offers_to_lead_to_dexters_house():
    model = _bounty_script()
    offered = await calls(model, Message("user", LEAD))
    assert [(call.name, call.arguments) for call in offered] == [
        ("propose_deal", {"npc_gives": [], "hero_gives": [], "lead": DESTINATION}),
    ]
