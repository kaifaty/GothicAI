"""Native waynet travel to an unseen body and back to the hero."""

from __future__ import annotations

import asyncio
import time
from typing import Any

from .. import vanilla
from ..dialogue import DialogueManager
from ..executor import Intent
from ..game import GameError, GameLink
from ..llm import ToolCall
from ..protocol import ActionResult, ActionType
from .common import _logged, _until_async

CAVALORN = "BAU_4300_ADDON_CAVALORN"
BRAGO = "BDT_1014_BANDIT_L"
BANDITS = (BRAGO, "BDT_1013_BANDIT_L", "BDT_1015_BANDIT_L")
STAIRS = "NW_XARDAS_STAIRS_01"


async def check_walk(link: GameLink, dialogue: DialogueManager) -> dict[str, str]:
    """Check an occluded trip from Xardas's stairs into the bandit cave."""
    if dialogue.llm.name != "fake":
        raise GameError("scenario check_walk requires LLM=fake and a new game without SAVE")
    await link.wait_world()
    for npc in (CAVALORN, *BANDITS):
        await link.debug("skip_important", near=npc)
    for npc in BANDITS:
        await link.debug("kill", npc=npc)
    await link.debug("player_goto", waypoint=STAIRS)
    await link.debug("npc_goto", npc=CAVALORN, waypoint=STAIRS)
    await link.attach(CAVALORN)
    results: dict[str, str] = {}
    try:
        body = (await link.story_state(npcs=[BRAGO], near=CAVALORN))["npcs"][BRAGO]
        results["cant_see_body"] = (
            f"{'done' if body['seen'] is False else 'failed'} (distance={body['distance']:.0f} cm)"
        )
        if not results["cant_see_body"].startswith("done"):
            return _logged(results)

        started = time.monotonic()
        went = await link.act(
            CAVALORN,
            "go_to_npc",
            npc=BRAGO,
            radius=vanilla.LOOK_CM,
            timeout_ms=vanilla.WALK_TIMEOUT_MS,
            timeout=vanilla.WALK_TIMEOUT_MS / 1000 + 10,
        )
        elapsed = time.monotonic() - started
        await link.act(CAVALORN, "turn_to", npc=BRAGO)
        body = (await link.story_state(npcs=[BRAGO], near=CAVALORN))["npcs"][BRAGO]
        where = (await link.snapshot(CAVALORN, fresh=True)).npc
        results["went"] = (
            f"{'done' if went.ok and body['seen'] else 'failed'} "
            f"(status={went.status}, detail={went.detail}, distance={body['distance']:.0f} cm, "
            f"seen={body['seen']}, seconds={elapsed:.1f}, "
            f"waypoint={where.waypoint}, nearest={where.nearest_waypoint}, pos={where.pos})"
        )
        if not results["went"].startswith("done"):
            return _logged(results)

        started = time.monotonic()
        back = await link.act(
            CAVALORN,
            "go_to_npc",
            npc="PLAYER",
            radius=vanilla.BACK_CM,
            timeout_ms=vanilla.WALK_TIMEOUT_MS,
            timeout=vanilla.WALK_TIMEOUT_MS / 1000 + 10,
        )
        results["came_back"] = (
            f"{'done' if back.ok and back.distance is not None and back.distance <= 300 else 'failed'} "
            f"(status={back.status}, detail={back.detail}, distance={back.distance} cm, "
            f"seconds={time.monotonic() - started:.1f})"
        )
        return _logged(results)
    finally:
        await link.detach(CAVALORN)


async def follow_back(link: GameLink, dialogue: DialogueManager) -> dict[str, str]:
    """The model's repeated follow intent recalls a companion from an unfinished body check."""
    if dialogue.llm.name != "fake":
        raise GameError("scenario follow_back requires LLM=fake and a new game without SAVE")
    await link.wait_world()
    for npc in (CAVALORN, *BANDITS):
        await link.debug("skip_important", near=npc)
    for npc in BANDITS:
        await link.debug("kill", npc=npc)
    await link.debug("player_goto", waypoint=STAIRS)
    await link.debug("npc_goto", npc=CAVALORN, waypoint=STAIRS)
    await link.attach(CAVALORN)
    conv = dialogue.open(CAVALORN, "Кавалорн", warm=False)
    conv.intent = Intent("follow_hero", "PLAYER", "friendly")
    results: dict[str, str] = {}
    follows = 0
    original_act = link.act

    async def recorded(npc: str, action: ActionType, /, **args: Any) -> ActionResult:
        nonlocal follows
        if npc == CAVALORN and action == "follow":
            follows += 1
        return await original_act(npc, action, **args)

    link.act = recorded  # type: ignore[method-assign]
    trip = asyncio.create_task(link.act(
        CAVALORN,
        "go_to_npc",
        npc=BRAGO,
        radius=vanilla.LOOK_CM,
        timeout_ms=vanilla.WALK_TIMEOUT_MS,
        timeout=vanilla.WALK_TIMEOUT_MS / 1000 + 10,
    ))
    try:
        away = await _until_async(
            lambda: link.snapshot(CAVALORN, fresh=True),
            lambda snap: snap.npc.busy and snap.npc.action == "go_to_npc"
            and not snap.npc.following and snap.player is not None and snap.player.distance >= 900,
            40,
        )
        snap = await link.snapshot(CAVALORN, fresh=True)
        distance = snap.player.distance if snap.player is not None else -1
        results["walking"] = (
            f"{'done' if away else 'failed'} "
            f"(intent={conv.intent.goal}, busy={snap.npc.busy}, action={snap.npc.action}, "
            f"following={snap.npc.following}, distance={distance:.0f} cm)"
        )
        if not away:
            return _logged(results)

        started = time.monotonic()
        outcome = dialogue._set_intent(conv, ToolCall("set_intent", {
            "goal": "follow_hero", "target": "PLAYER", "attitude": "friendly",
        }), snap)
        recalled = await _until_async(
            lambda: link.snapshot(CAVALORN, fresh=True),
            lambda current: current.npc.following,
            10,
        )
        accepted = outcome.output.startswith("Принято:")
        results["recalled"] = (
            f"{'done' if recalled and accepted and follows == 1 else 'failed'} "
            f"(following={recalled}, follow_count={follows}, reply={outcome.output})"
        )
        if not recalled:
            return _logged(results)
        went = await trip
        results["trip_interrupted"] = (
            f"{'done' if went.status == 'cancelled' else 'failed'} "
            f"(status={went.status}, detail={went.detail})"
        )
        returned = await _until_async(
            lambda: link.snapshot(CAVALORN, fresh=True),
            lambda current: current.npc.following and current.player is not None
            and current.player.distance <= 450,
            40,
        )
        snap = await link.snapshot(CAVALORN, fresh=True)
        final_distance = snap.player.distance if snap.player is not None else -1
        results["came_back"] = (
            f"{'done' if returned and follows == 1 else 'failed'} "
            f"(following={snap.npc.following}, follow_count={follows}, "
            f"distance={distance:.0f} -> {final_distance:.0f} cm, "
            f"seconds={time.monotonic() - started:.1f})"
        )
        return _logged(results)
    finally:
        if not trip.done():
            trip.cancel()
        await asyncio.gather(trip, return_exceptions=True)
        link.act = original_act  # type: ignore[method-assign]
        await link.detach(CAVALORN)
