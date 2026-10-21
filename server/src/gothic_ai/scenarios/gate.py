"""A gate guard's paid admission and believed claim in a fresh game."""

from __future__ import annotations

import asyncio
import math
from typing import TYPE_CHECKING, Any

from ..brain.reflex import Reflexes
from ..deals import GOLD
from ..llm import ToolCall
from ..llm.fake import FakeBackend, FakeStep
from ..protocol import DialogChosen, GameMessage, Session
from .common import ClaimScript, FixedDecider, _logged, _save, _until, _until_async, wait_message

if TYPE_CHECKING:
    from ..deals import Deals
    from ..dialogue import DialogueManager
    from ..game import GameLink
    from ..gates import Gates
    from ..talk import TalkDesk

GUARD, PAIR = "MIL_310_STADTWACHE", "MIL_333_STADTWACHE"
CHECKPOINT = "NW_CITY_ENTRANCE_MAIN"
CLAIM = "Меня ждёт Харад: я пришёл поговорить об ученичестве."
ABOUT = "VLK_412_HARAD"


async def gate_scenario(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    deals: Deals,
    gates: Gates,
    slot: int = 7,
) -> dict[str, str]:
    """The model sells passage for 100 gold through Y, then believes a claim after a load.

    The guard keeps his native post and passage perceptions. Only the hero's position and gold are
    staged; SECONDWARN and ATTACK remain active while he crosses the gate after paying.
    """
    results: dict[str, str] = {}
    chosen: list[DialogChosen] = []
    original_llm, original_reflexes = dialogue.llm, talk.reflexes

    def check(name: str, ok: bool, detail: object = "") -> bool:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"
        return ok

    def record(message: GameMessage) -> None:
        if isinstance(message, DialogChosen) and message.npc in {GUARD, PAIR}:
            chosen.append(message)

    async def gold() -> int:
        value = await link.story_eval(GUARD, [
            ["call", "NPC_HASITEMS", ["inst", "HERO"], ["sym", GOLD]],
        ])
        return int(value[0])

    async def passage() -> list[Any]:
        return await link.story_eval(GUARD, [
            ["field", ["inst", npc], "AIVAR", 14] for npc in (GUARD, PAIR)
        ])

    async def ready() -> bool:
        entry = talk.people.get(GUARD)
        return entry is not None and entry.earshot <= 1500 and entry.activity != "down"

    async def quiet() -> bool:
        views = await link.npc_view([GUARD])
        return bool(views) and views[0].state != "ZS_ATTACK"

    async def outside_post() -> list[Any]:
        return await link.story_eval(GUARD, [
            ["call", "NPC_GETDISTTOWP", ["inst", "HERO"], ["str", CHECKPOINT]],
            ["call", "HLP_STRCMP", ["call", "NPC_GETNEARESTWP", ["inst", "SELF"]],
             ["field", ["inst", "SELF"], "WP"]],
        ])

    if not isinstance(dialogue.llm, FakeBackend):
        return _logged({"scripted_model": "failed (gate requires LLM=fake)"})
    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    # The initial connection may be processed after the new world has loaded; the bridge then
    # reports that same fresh session as "connect". No SAVE is used for this scenario.
    if not check("new_game", link.session is not None and link.session.reason in {"new_game", "connect"}):
        return _logged(results)
    await asyncio.sleep(1)
    link.listeners.append(record)
    talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
    try:
        # The hero faces the actual guard from outside and speaks without opening the native menu.
        # No guard is held, moved or stripped of any of his native dialogs.
        await link.debug("teleport", near=GUARD, distance=1200)
        if not check("guard_hears", await _until_async(ready, bool, 20)):
            return _logged(results)
        settled = await _until_async(outside_post, lambda value: value[0] > 700 and bool(value[1]), 30)
        if not check("outside_post", settled, await outside_post()):
            return _logged(results)
        before_gold = await gold()
        if before_gold != 100:
            await link.story_effect(GUARD, [{
                "do": "give", "item": GOLD, "count": abs(100 - before_gold),
                **({"to": "self"} if before_gold > 100 else {}),
            }])
        baseline = await passage()
        if not check("baseline", baseline == [0, 0] and await gold() == 100, baseline):
            return _logged(results)
        if not check("saved_outside", await _save(link, slot) is not None):
            return _logged(results)
        name = talk.people[GUARD].name
        dialogue.llm = ClaimScript(
            said=[("сто золотых", FakeStep("Сто золотых — и проходи.", [ToolCall("propose_deal", {
                "npc_gives": [], "hero_gives": [{"item": GOLD, "count": 100}], "let_pass": True,
            })]))],
            told=[], default="Проходи.", only=name,
        )
        # Both guards have the same display name; supply the intended focus as voice input does.
        talk.hear("Я заплачу сто золотых. Позволишь пройти в город?", GUARD)
        offered = await _until(lambda: GUARD in deals.open, 30)
        window = await link.deal_window()
        if not check("bribe_offered", offered and window is not None, window):
            return _logged(results)
        await asyncio.sleep(1.2)
        pressed = await link.press_deal()
        admitted = await _until_async(passage, lambda value: value == [1, 1], 20)
        if not check("bribe_passes_both", pressed == "sent" and admitted, await passage()):
            return _logged(results)
        check("bribe_paid", await gold() == 0, await gold())
        absolution = await link.story_eval(GUARD, [
            ["var", "MIL_310_PERSONAL_ABSOLUTIONLEVEL"],
            ["+", ["fn", "B_GETCURRENTABSOLUTIONLEVEL", ["inst", "SELF"]], ["int", 1]],
        ])
        check("native_bribe_level", absolution[0] == absolution[1], absolution)

        points = {point["name"]: point for point in await link.waypoints() if not point.get("fp")}
        checkpoint = points[CHECKPOINT]
        streets = [point for point in points.values() if point["name"].startswith("NW_CITY_MAINSTREET_")]
        inside = min(streets, key=lambda point: math.dist(
            (point["x"], point["y"], point["z"]),
            (checkpoint["x"], checkpoint["y"], checkpoint["z"]),
        ))
        chosen.clear()
        safe = True
        for point in (CHECKPOINT, inside["name"]):
            await link.debug("player_goto", waypoint=point)
            await asyncio.sleep(2)
            safe = await quiet() and safe
        snap = await link.snapshot(GUARD, fresh=True)
        crossed = snap.player is not None and math.dist(
            snap.player.pos, (inside["x"], inside["y"], inside["z"]),
        ) < 150
        warnings = [message.node for message in chosen if message.node.endswith(("_SECONDWARN", "_ATTACK"))]
        check("entered_city", crossed, inside["name"])
        check("no_warning_or_attack", safe and not warnings, warnings)

        # Return to a save with no permission and no paid deal: admission by belief is independent.
        loading = asyncio.create_task(wait_message(link, Session, lambda msg: msg.reason == "load", 60))
        await asyncio.sleep(0)
        await link.debug("load", slot)
        await loading
        await link.wait_world()
        if not check("claim_branch", await passage() == [0, 0] and await gold() == 100):
            return _logged(results)
        if not check("claim_guard_hears", await _until_async(ready, bool, 20)):
            return _logged(results)
        dialogue.llm = ClaimScript(
            said=[("ученичестве", FakeStep("Ладно, верю. Проходи.", [ToolCall("let_pass", {
                "basis": "claim", "claim": CLAIM, "about": ABOUT,
            })]))],
            told=[], default="Проходи.", only=name,
        )
        talk.hear(CLAIM, GUARD)
        admitted = await _until_async(passage, lambda value: value == [1, 1], 20)
        check("claim_passes_both", admitted, await passage())
        await _until(lambda: bool(gates.chronicle.facts("gate_claim")), 10)
        facts = gates.chronicle.facts("gate_claim")
        claimed = [fact for fact in facts if fact.get("npc") == GUARD and fact.get("claim") == CLAIM]
        check("claim_recorded", len(claimed) == 1 and claimed[0].get("about") == ABOUT
              and claimed[0].get("believed") is True
              and all(isinstance(claimed[0].get(key), int) for key in ("day", "hour", "minute")), claimed)
        check("claim_free", await gold() == 100, await gold())
        return _logged(results)
    finally:
        dialogue.llm, talk.reflexes = original_llm, original_reflexes
        link.listeners.remove(record)
