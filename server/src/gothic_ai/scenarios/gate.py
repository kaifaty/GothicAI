"""A gate guard's paid admission and believed claim in a fresh game."""

from __future__ import annotations

import asyncio
import math
from typing import TYPE_CHECKING, Any

from ..brain.reflex import Reflexes
from ..deals import GOLD
from ..llm import ToolCall
from ..llm.fake import FakeBackend, FakeStep
from ..protocol import DialogChosen, GameMessage, LineVoice, Session
from .common import (
    ClaimScript,
    FixedDecider,
    _logged,
    _save,
    _until,
    _until_async,
    prepare_npc_pose,
    wait_message,
)

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
LOTHAR = "PAL_203_LOTHAR"
PASS = "DIA_MIL_310_STADTWACHE_PASSIERSCHEIN"
LETTER = "ITWR_PASSIERSCHEIN"


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


async def gate_vouch_scenario(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    gates: Gates,
    slot: int = 7,
) -> dict[str, str]:
    """A nearby guarantor, a native pass and a morning check in three fresh-save branches."""
    results: dict[str, str] = {}
    chosen: list[DialogChosen] = []
    voiced: list[LineVoice] = []
    facts = gates.chronicle.facts
    original_llm, original_reflexes = dialogue.llm, talk.reflexes

    def check(name: str, ok: bool, detail: object = "") -> bool:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"
        return ok

    def record(message: GameMessage) -> None:
        if isinstance(message, DialogChosen) and message.npc in {GUARD, PAIR}:
            chosen.append(message)
        elif isinstance(message, LineVoice) and message.npc in {GUARD, PAIR}:
            voiced.append(message)

    async def passage() -> list[Any]:
        # A clock leap queues the city's rumor memories ahead of this control read.
        return await link.story_eval(GUARD, [
            ["field", ["inst", npc], "AIVAR", 14] for npc in (GUARD, PAIR)
        ], timeout=30)

    async def ready(npc: str = GUARD) -> bool:
        entry = talk.people.get(npc)
        return entry is not None and entry.earshot <= 1500 and entry.activity != "down"

    async def outside_post() -> list[Any]:
        return await link.story_eval(GUARD, [
            ["call", "NPC_GETDISTTOWP", ["inst", "HERO"], ["str", CHECKPOINT]],
            ["call", "HLP_STRCMP", ["call", "NPC_GETNEARESTWP", ["inst", "SELF"]],
             ["field", ["inst", "SELF"], "WP"]],
        ])

    async def load(label: str) -> bool:
        loading = asyncio.create_task(wait_message(link, Session, lambda msg: msg.reason == "load", 60))
        await asyncio.sleep(0)
        await link.debug("load", slot)
        await loading
        await link.wait_world()
        chosen.clear()
        voiced.clear()
        return check(label, await passage() == [0, 0] and not facts("gate_vouch")
                     and not facts("gate_claim") and await _until_async(ready, bool, 20))

    if not isinstance(dialogue.llm, FakeBackend):
        return _logged({"scripted_model": "failed (gate_vouch requires LLM=fake)"})
    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    if not check("new_game", link.session is not None and link.session.reason in {"new_game", "connect"}):
        return _logged(results)
    await asyncio.sleep(1)
    link.listeners.append(record)
    talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
    try:
        # Keep the native gatekeepers at their posts. Only the hero and guarantor are staged.
        await link.debug("time", hour=14, minute=30)
        await link.debug("teleport", near=GUARD, distance=1200)
        if not check("guard_hears", await _until_async(ready, bool, 20)):
            return _logged(results)
        settled = await _until_async(outside_post, lambda value: value[0] > 700 and bool(value[1]), 30)
        if not check("outside_post", settled, await outside_post()):
            return _logged(results)
        if not check("baseline", await passage() == [0, 0], await passage()):
            return _logged(results)
        if not check("saved_outside", await _save(link, slot) is not None):
            return _logged(results)
        guard_name = talk.people[GUARD].name
        posed, detail = await prepare_npc_pose(
            link, LOTHAR, CHECKPOINT, to=GUARD, distance=650, face="PC_HERO",
            require_perception_ready=False,
        )
        if not check(
            "guarantor_nearby", posed and await _until_async(lambda: ready(LOTHAR), bool, 20), detail,
        ):
            return _logged(results)
        distance = (await link.story_eval(LOTHAR, [
            ["call", "NPC_GETDISTTONPC", ["inst", "SELF"], ["inst", GUARD]],
        ]))[0]
        if not check("guarantor_in_range", 0 <= distance <= 1000, distance):
            return _logged(results)
        dialogue.llm = ClaimScript(
            said=[("поручиться", FakeStep("Я поручусь за тебя. Не посрами моего имени.", [
                ToolCall("vouch_for_hero", {"note": "Герой ищет честную работу в городе."}),
            ]))],
            told=[], default="Держи слово.", only=talk.people[LOTHAR].name,
        )
        talk.hear("Лотар, я ищу честную работу. Ты можешь поручиться за меня?", LOTHAR)
        vouched = await _until(lambda: any(f.get("npc") == LOTHAR for f in facts("gate_vouch")), 30)
        if not check("vouch_recorded", vouched, facts("gate_vouch")):
            return _logged(results)
        await gates.prepare(GUARD)
        situation = gates.situation(GUARD) or ""
        if not check("vouch_visible", "basis vouch" in situation and "Лотар" in situation, situation):
            return _logged(results)
        talk.end()
        if not check("guarantor_released", await _until(lambda: LOTHAR not in talk.attached, 20)):
            return _logged(results)
        dialogue.llm = ClaimScript(
            said=[("поручительство", FakeStep("Лотару я доверяю. Проходи.", [
                ToolCall("let_pass", {"basis": "vouch"}),
            ]))],
            told=[], default="Проходи.", only=guard_name,
        )
        talk.hear("Моё поручительство до тебя дошло. Пропустишь?", GUARD)
        if not check("vouch_passes_both", await _until_async(passage, lambda value: value == [1, 1], 20),
                     await passage()):
            return _logged(results)

        # A real pass uses the native admission program without choosing or voicing its dialog.
        if not await load("native_branch"):
            return _logged(results)
        await link.story_effect(GUARD, [
            {"do": "create", "item": LETTER, "count": 1},
            {"do": "give", "item": LETTER, "count": 1},
        ])
        await gates.prepare(GUARD)
        if not check("native_ground_visible", PASS in (gates.situation(GUARD) or "")):
            return _logged(results)
        dialogue.llm = ClaimScript(
            said=[("пропуск", FakeStep("Пропуск в порядке. Проходи.", [
                ToolCall("let_pass", {"basis": PASS}),
            ]))],
            told=[], default="Проходи.", only=guard_name,
        )
        chosen.clear()
        voiced.clear()
        talk.hear("У меня есть пропуск в город. Вот он.", GUARD)
        if not check("native_passes_both", await _until_async(passage, lambda value: value == [1, 1], 20),
                     await passage()):
            return _logged(results)
        check("native_no_menu_or_lines", not chosen and not voiced and not await link.dialog_menu(),
              {"dialogs": [m.node for m in chosen], "lines": [m.line for m in voiced]})

        # Harad has no relationship with the hero in this save. The guard still may believe the lie.
        if not await load("claim_branch"):
            return _logged(results)
        dialogue.llm = ClaimScript(
            said=[("ученичестве", FakeStep("Ладно, верю. Проходи.", [ToolCall("let_pass", {
                "basis": "claim", "claim": CLAIM, "about": ABOUT,
            })]))],
            told=[], default="Проходи.", only=guard_name,
        )
        talk.hear(CLAIM, GUARD)
        if not check("claim_passes_both", await _until_async(passage, lambda value: value == [1, 1], 20),
                     await passage()):
            return _logged(results)
        await _until(lambda: bool(facts("gate_claim")), 10)
        claim = next((
            f for f in facts("gate_claim") if f.get("npc") == GUARD and f.get("about") == ABOUT
        ), {})
        if not check("claim_is_false", bool(claim) and claim.get("truth") is False, claim):
            return _logged(results)
        await link.debug("time", hour=8, minute=0)
        caught = await _until(lambda: any(f.get("gate_claim") == claim["id"] for f in facts("caught")), 30)
        check("morning_caught", caught, [f for f in facts("caught") if f.get("gate_claim") == claim["id"]])
        check("morning_revokes_both", await _until_async(passage, lambda value: value == [0, 0], 20),
              await passage())
        reentry = await link.story_eval(GUARD, [
            ["var", f"MIL_{number}_SCHONMALREINGELASSEN"] for number in (310, 333)
        ], timeout=30)
        check("native_reentry_reset", reentry == [0, 0], reentry)
        return _logged(results)
    finally:
        dialogue.llm, talk.reflexes = original_llm, original_reflexes
        link.listeners.remove(record)
