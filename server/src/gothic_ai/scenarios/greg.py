"""Consequences of Greg's given clothing errand and reward decisions."""

from __future__ import annotations

import asyncio
from typing import TYPE_CHECKING

from ..brain.reflex import Reflexes
from ..llm import ToolCall
from ..protocol import Session
from .common import (
    FixedDecider,
    _clear_beasts,
    _logged,
    _save,
    _until,
    _until_async,
    log,
    model_off,
    wait_message,
)

SCENARIOS = {
    "greg": ("GIVEN", {"save": False, "timeout": 240}),
}

if TYPE_CHECKING:
    from ..dialogue import DialogueManager
    from ..game import GameLink
    from ..talk import TalkDesk
    from ..vanilla import VanillaQuests

GREG = "PIR_1300_ADDON_GREG_NW"
QUEST = "MIS_ADDON_GREG_BRINGMETOTHECITY"
CLOTHES = "ITAR_BAU_L"
MAP = "ITWR_MAP_NEWWORLD"
RUM = "ITFO_ADDON_RUM"
GOLD = "ITMI_GOLD"


async def greg(
    link: GameLink, dialogue: DialogueManager, talk: TalkDesk, vanilla: VanillaQuests, slot: int = 7
) -> dict[str, str]:
    """Give and accept Greg's native quest as declared givens. Reload the branch saved before the
    clothes were supplied: accepting the same claim without them must record the truth."""
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> bool:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"
        return ok

    off = model_off(dialogue)
    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    await asyncio.sleep(1)
    facts = vanilla.chronicle.facts

    async def state() -> int | None:
        return (await link.story_state(vars=[QUEST])).get("vars", {}).get(QUEST)

    async def inventory() -> list[int]:
        exprs = [
            ["call", "NPC_HASITEMS", ["inst", who], ["sym", item]]
            for who, item in (
                ("HERO", GOLD),
                ("HERO", MAP),
                ("HERO", RUM),
                ("HERO", CLOTHES),
                ("SELF", CLOTHES),
                ("HERO", "ITAR_BAU_M"),
            )
        ]
        return [int(n or 0) for n in await link.story_eval(GREG, exprs)]

    async def say(text: str) -> bool:
        await link.debug("teleport", near=GREG, distance=250)
        if not await _until(lambda: GREG in talk.people and talk.people[GREG].distance < 600, 20):
            return False
        conv = dialogue.conversations.get(GREG)
        before = conv.reply if conv else None
        talk.hear(text, GREG, {"via": "scenario"})

        def answered() -> bool:
            conv = dialogue.conversations.get(GREG)
            return bool(
                conv and conv.reply and conv.reply is not before and conv.reply.done() and not conv.actions
            )

        return await _until(answered, 60)

    await link.debug("skip_important", near=GREG)
    # The nearest waypoint can be farther than talking distance from Greg.
    await link.debug("teleport", near=GREG, distance=250)
    await _clear_beasts(link, GREG)
    log.info("given: дано: решающая модель оставляет Грега слушать героя")
    talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
    if not check("near", await _until(lambda: GREG in talk.people, 20), GREG):
        return _logged(results)
    before = await inventory()
    if not check("new_game", await state() == 0 and before[3] == before[5] == 0, before):
        return _logged(results)
    name = talk.people[GREG].name
    off.given(
        "дано: Грег даёт поручение добыть одежду крестьянина", who=GREG, when="помогу",
        calls=[ToolCall("give_quest", {"quest": QUEST}, "greg_give")],
    )
    off.silent("дано: Грег молчит (поручение выдано)", who=GREG, when="помогу")
    off.given(
        "дано: Грег засчитывает принесённую одежду", who=GREG, when="принёс одежду",
        calls=[ToolCall("accept_quest", {"quest": QUEST}, "greg_accept")],
    )
    off.silent("дано: Грег молчит (поручение засчитано)", who=GREG, when="принёс одежду")
    off.given(
        "дано: Грег верит заявлению об одежде за углом и засчитывает поручение", who=GREG, when="за углом",
        calls=[ToolCall("accept_quest", {"quest": QUEST}, "greg_accept_missing")],
    )
    off.silent("дано: Грег молчит (заявление об одежде принято)", who=GREG, when="за углом")

    await vanilla.prepare(GREG)
    situation = vanilla.situation(GREG) or ""
    check(
        "promised_before_giving",
        "Ты можешь сам дать герою поручение «Человек с повязкой на глазу»" in situation
        and all(s in situation for s in ("50 золотых", "100", "опыт", "Карта земель Хориниса", "Ром ×2")),
        situation,
    )
    answered = await say(f"{name}, я помогу тебе попасть в город.")
    given = await _until(lambda: any(f.get("quest") == QUEST for f in facts("quest_given")), 15)
    if not check("given", answered and given and await state() == 1, facts("quest_given")[-1:]):
        return _logged(results)

    # Keep an independent branch with the quest running and neither farmer outfit supplied.
    saved = await _save(link, slot)
    if not check("clean_save", saved is not None):
        return _logged(results)
    await link.story_effect(
        GREG,
        [{"do": "create", "item": CLOTHES, "count": 1}, {"do": "give", "item": CLOTHES, "count": 1}],
    )
    supplied = await inventory()
    if not check("clothes_supplied", supplied[3] == 1, supplied):
        return _logged(results)
    await vanilla.prepare(GREG)
    situation = vanilla.situation(GREG) or ""
    check(
        "promised_on_turn_in",
        "Награда, как обещано" in situation
        and all(s in situation for s in ("50 золотых", "100", "опыт", "Карта земель Хориниса", "Ром ×2")),
        situation,
    )
    answered = await say(f"{name}, я принёс одежду крестьянина. Давай награду.")
    expected = [before[0] + 50, before[1] + 1, before[2] + 2, 0, before[4] + 1, 0]
    paid = await _until_async(inventory, lambda counts: counts == expected, 40)
    after = await inventory()
    claim = next((f for f in facts("claim") if f.get("quest") == QUEST), {})
    check("reward", answered and paid, f"{before} -> {after}; expected {expected}")
    check("success", await state() == 2 and claim.get("truth") is True, claim)
    check("clothes_transferred", after[3] == 0 and after[4] == before[4] + 1, after)
    check("honest", not any(f.get("quest") == QUEST for f in facts("lie")))

    loading = asyncio.create_task(wait_message(link, Session, lambda msg: msg.reason == "load", 60))
    await asyncio.sleep(0)
    await link.debug("load", slot)
    await loading
    await link.wait_world()
    await asyncio.sleep(1)
    clean = await inventory()
    if not check(
        "without_clothes_branch",
        await state() == 1 and clean == before and QUEST not in vanilla.told,
        clean,
    ):
        return _logged(results)
    await vanilla.prepare(GREG)
    offer = vanilla.offers.get(GREG, {}).get(QUEST)
    if not check(
        "clothes_required",
        offer is not None and offer.claim.kind == "fetch" and CLOTHES in offer.claim.items,
        offer.claim.items if offer else "no offer",
    ):
        return _logged(results)
    answered = await say(f"{name}, одежду крестьянина я оставил для тебя за углом. Засчитай поручение.")
    await _until(lambda: any(f.get("quest") == QUEST for f in facts("lie")), 15)
    claim = next((f for f in facts("claim") if f.get("quest") == QUEST), None)
    lie = next((f for f in facts("lie") if f.get("quest") == QUEST), None)
    check(
        "missing_clothes_is_a_lie",
        answered and claim is not None and claim.get("truth") is False and lie is not None,
        {"claim": claim, "lie": lie},
    )
    return _logged(results)
