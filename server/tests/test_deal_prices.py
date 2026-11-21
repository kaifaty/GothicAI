from unittest.mock import AsyncMock

import pytest

from gothic_ai.contracts import Give
from gothic_ai.deal_items import appraise_deal
from gothic_ai.deals import Debt
from gothic_ai.game import GameLink
from gothic_ai.llm import Message, ToolCall, ToolResult
from gothic_ai.llm.fake import FakeBackend, FakeStep
from gothic_ai.memory import MemoryStore
from tests.fake_game import FakeGame, connected
from tests.test_deals import GOLD, POTION, XARDAS, _setup, _shown
from tests.test_dialogue import _until

SWORD = "ITMW_EXPENSIVE_SWORD"


def _item(item: str, count: int = 1) -> dict:
    return {"item": item, "count": count}


async def test_npc_sees_prices_before_barter_and_in_the_open_offer():
    fake = FakeGame()
    fake.item_values = {POTION: 80, SWORD: 3000}
    terms = {"npc_gives": [_item(POTION, 2)], "hero_gives": [_item(SWORD), _item(GOLD, 20)]}
    offer = {**terms, "hero_gives": [_item(SWORD)]}
    llm = FakeBackend(delay=0, script=[
        FakeStep(tool_calls=[ToolCall("appraise_deal", terms, "quote")]),
        FakeStep("Твой меч стоит три тысячи. За эти два эликсира ты переплатишь."),
        FakeStep("Раз ты хочешь мне помочь, я согласен.", [ToolCall("propose_deal", offer)]),
        FakeStep("Условия мы помним."),
    ])
    link_cm, dialogue, _, deals = await _setup(fake, llm, MemoryStore(":memory:"))
    try:
        fake.symbols[SWORD], fake.item_names[SWORD] = "item", "Меч"
        fake.hero_items = {}  # The hero named the sword. Its script price is public even if he has none.
        await dialogue.say_as_player(XARDAS, "Дам меч и двадцать золотых за два эликсира.")
        assert "appraise_deal" in {tool.name for tool in llm.calls[0][2]}
        situation = next(
            m.content for m in llm.calls[0][1] if isinstance(m, Message) and "Твой инвентарь" in m.content
        )
        assert "×3 (цена за штуку — 80 зол.)" in situation and SWORD not in situation
        quote = next(m.output for m in llm.calls[1][1] if isinstance(m, ToolResult) and m.call_id == "quote")
        assert "Эликсир лечения ×2 по 80 зол. = 160 зол." in quote
        assert "Меч ×1 по 3000 зол. = 3000 зол." in quote
        assert "Итого — 160 золотых" in quote and "Итого — 3020 золотых" in quote
        queries = [m for m in fake.received if m["type"] == "story_state" and m.get("items")]
        assert [m["items"] for m in queries] == [sorted([POTION, SWORD])]
        assert not any(m["type"] == "story_eval" for m in fake.received)
        assert not _shown(fake) and not deals.open and not fake.hero_items

        await dialogue.say_as_player(XARDAS, "Я хочу помочь. Давай за один меч, золото оставлю себе.")
        await _until(lambda: len(_shown(fake)) == 1)
        assert not fake.hero_items
        await dialogue.say_as_player(XARDAS, "Напомни, о чём договорились.")
        situation = next(
            m.content for m in llm.calls[-1][1] if isinstance(m, Message) and "Твоё предложение" in m.content
        )
        assert "Итого — 160 золотых" in situation and "Итого — 3000 золотых" in situation
        assert "3020" not in situation
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_offer_includes_items_paid_now_towards_a_debt():
    fake = FakeGame()
    fake.item_values = {SWORD: 3000}
    link_cm, dialogue, _, deals = await _setup(fake, FakeBackend(), MemoryStore(":memory:"))
    try:
        fake.symbols[SWORD], fake.item_names[SWORD] = "item", "Меч"
        deals.debts["owed"] = Debt("owed", XARDAS, "Ксардас", Give(SWORD, 1, "Меч"), due=1)
        snap = await dialogue.link.snapshot(XARDAS)
        args = {"hero_gives": [_item(SWORD)], "repay": ["owed"]}
        result = await deals.propose(XARDAS, "Ксардас", args, snap)
        assert result.status == "accepted" and "Меч ×2 по 3000 зол. = 6000 зол." in result.output
        assert "Итого — 6000 золотых" in deals.situation(XARDAS)
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize("protocol", [64, 65])
async def test_unknown_prices_and_known_zero_stay_distinct(protocol):
    fake = FakeGame(protocol=protocol)
    fake.item_names = {POTION: "Пустая бутылка"}
    fake.item_values = {POTION: 0}
    terms = {
        "npc_gives": [_item(POTION), _item(POTION.lower(), 2)],
        "hero_gives": [_item("UNKNOWN"), _item(GOLD, 30)],
    }
    async with connected(fake) as link:
        result = await appraise_deal(link, terms)
        npc, hero = result.output.splitlines()
        assert result.status == "read"
        assert "Пустая бутылка ×3" in npc
        if protocol == 65:
            assert "по 0 зол. = 0 зол." in npc and "Итого — 0 золотых" in npc
        else:
            assert "цена неизвестна" in npc and "сумма неизвестна" in npc
        assert "UNKNOWN ×1 (цена неизвестна)" in hero and "сумма неизвестна" in hero
        assert "30 золотых" in hero and "Итого — 30" not in hero


async def test_gold_is_currency_and_invalid_or_empty_quotes_do_not_query_the_game():
    link = AsyncMock(spec=GameLink)
    result = await appraise_deal(link, {"hero_gives": [_item(GOLD, 21)]})
    assert result.status == "read" and "Итого — 21 золотой" in result.output
    for terms in ({}, {"npc_gives": [_item(POTION, 0)]}, {"hero_gives": [_item(POTION, 100_001)]}):
        assert (await appraise_deal(link, terms)).status == "rejected"
    link.story_state.assert_not_awaited()
