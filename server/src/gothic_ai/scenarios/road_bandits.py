"""A road gang hails the hero, sells peace, and attacks only after a provocation."""

from __future__ import annotations

import asyncio
import math
import time
from typing import TYPE_CHECKING, Any

from ..bodies import Bodies
from ..brain.reflex import Reflexes
from ..deals import GOLD
from ..game import GameError, GameLink
from ..llm import Message, ToolCall
from ..llm.fake import FakeStep
from ..protocol import GameMessage, NpcActResult, NpcRoster, PerceptionHeld, RosterEntry
from .bandit_hold import CAVALORN, GANG, PARK, PARTY_POINT, PRIMARY
from .common import ClaimScript, FixedDecider, _logged, _m5b_player_weapon, _until, _until_async

if TYPE_CHECKING:
    from ..deals import Deals
    from ..dialogue import DialogueManager
    from ..parley import Parley
    from ..talk import TalkDesk

HAIL = "Чужак подходит"
PAYMENT = "Дам сто золотых за мир со всей вашей шайкой."


def _road_script() -> ClaimScript:
    return ClaimScript(
        said=[
            (HAIL, FakeStep("Стой, чужак. Сто золотых за проход. Минута на ответ.", [ToolCall(
                "ultimatum", {"demand": "Заплати сто золотых за проход", "seconds": 60},
            )])),
            (PAYMENT, FakeStep("Сотню сюда — и четыре часа наши тебя не тронут.", [ToolCall(
                "propose_deal", {
                    "npc_gives": [], "hero_gives": [{"item": GOLD, "count": 100}],
                    "truce": {"hours": 4},
                },
            )])),
        ],
        told=[],
        default="Жду твоего решения.",
    )


async def road_bandits(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    deals: Deals,
    parley: Parley,
) -> dict[str, str]:
    """New game, LLM=fake: the native road encounter uses production parley decisions."""
    if dialogue.llm.name != "fake":
        raise GameError("scenario road_bandits requires LLM=fake and a new game without SAVE")
    await link.wait_world()
    results: dict[str, str] = {}
    roster: dict[str, RosterEntry] = {}
    native_attacks: list[tuple[float, str]] = []
    holds: list[PerceptionHeld] = []
    approaches: list[NpcActResult] = []
    original_llm, original_reflexes = dialogue.llm, talk.reflexes
    script = _road_script()

    def check(name: str, ok: bool, detail: object = "") -> bool:
        results[name] = f"{'done' if ok else 'failed'} ({detail})"
        return ok

    def record(message: GameMessage) -> None:
        if isinstance(message, NpcRoster):
            if message.full:
                roster.clear()
            roster.update((row.id, row) for row in message.npcs)
            native_attacks.extend(
                (time.monotonic(), row.id) for row in message.npcs
                if row.id in GANG and row.state == "ZS_ATTACK"
            )
        elif isinstance(message, PerceptionHeld) and message.npc in GANG:
            holds.append(message)
        elif isinstance(message, NpcActResult) and message.npc in GANG:
            if message.action in {"turn_to", "go_to_npc"} and message.status == "queued":
                approaches.append(message)

    def hail_calls() -> list[tuple[Any, ...]]:
        return [call for call in script.calls if call[1] and isinstance(call[1][-1], Message)
                and HAIL in call[1][-1].content]

    async def gold(npc: str) -> int:
        return int(await link.evaluate([
            "call", "NPC_HASITEMS", ["inst", npc], ["sym", GOLD],
        ]) or 0)

    dialogue.llm = script
    talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
    link.listeners.append(record)
    try:
        if not check("new_game", not deals.chronicle.facts("deal") and not deals.chronicle.facts("truce"),
                     "no prior deals or truces"):
            return _logged(results)
        await link.debug("time", hour=12, minute=0)
        await link.debug("player_goto", waypoint=PARK)
        await link.debug("skip_important", near=CAVALORN)
        parked = await Bodies(link).act(
            CAVALORN, "start_activity", state="ZS_STAND_ARMSCROSSED", waypoint=PARK,
            minutes=120, place=True, arrive=True, radius=100, timeout=15,
        )
        await link.debug("player_equip", item="ITMW_1H_BAU_MACE")
        await _m5b_player_weapon(link, "none")
        await link.debug("player_hp", hp=1200)
        current_gold = await gold("HERO")
        if current_gold != 200:
            transfer: dict[str, Any] = {"do": "give", "item": GOLD, "count": abs(200 - current_gold)}
            if current_gold > 200:
                transfer["to"] = "self"
            await link.story_effect(PRIMARY, [transfer])
        points = {point["name"]: point for point in await link.waypoints()}
        origins = {npc: (await link.snapshot(npc, fresh=True)).npc for npc in sorted(GANG)}
        primary = origins[PRIMARY]
        point = points[PARTY_POINT]
        dx, dz = point["x"] - primary.pos[0], point["z"] - primary.pos[2]
        length = math.hypot(dx, dz)
        entered = time.monotonic()
        await link.debug("player_goto", pos=(
            primary.pos[0] + 1400 * dx / length, primary.pos[1], primary.pos[2] + 1400 * dz / length,
        ))
        ready = await _until(lambda: GANG <= roster.keys(), 10)
        leader = max(sorted(GANG), key=talk.lead)
        snap = await link.snapshot(PRIMARY, fresh=True)
        cavalorn = await link.snapshot(CAVALORN, fresh=True)
        away = math.dist(snap.npc.pos, cavalorn.npc.pos)
        if not check("prepared", bool(
            ready and parked.status == "arrived" and away > 6000
            and snap.player and snap.player.hp == 1200 and 8 <= snap.world.hour < 23
        ), f"gang={sorted(GANG & roster.keys())}, leader={leader}, "
           f"Cavalorn={away:.0f} cm ({parked.status}), hour={snap.world.hour}"):
            return _logged(results)

        hailed = await _until(lambda: bool(hail_calls() and parley.ultimatum(leader)), 30)
        leader_snap = await link.snapshot(leader, fresh=True)
        moved = math.dist(origins[leader].pos, leader_snap.npc.pos)
        turned = abs((leader_snap.npc.rot - origins[leader].rot + 180) % 360 - 180)
        calls = hail_calls()
        tools = {tool.name for tool in calls[0][2]} if calls else set()
        if not check("hail", bool(
            hailed and len(calls) == 1 and any(row.npc == leader for row in approaches)
            and talk.partner is not None and talk.partner.npc == leader
            and (moved > 50 or turned > 5)
            and {"ultimatum", "propose_deal", "set_intent", "recall"} <= tools
            and any(hold.kind == "bandit_enemy" and hold.other == "PLAYER" for hold in holds)
            and not native_attacks
        ), f"after_s={time.monotonic() - entered:.2f}, calls={len(calls)}, "
           f"leader={leader}, move_cm={moved:.0f}, turn_deg={turned:.1f}, "
           f"approaches={[(row.npc, row.action) for row in approaches]}, "
           f"partner={talk.partner.npc if talk.partner else None}, "
           f"tools={sorted(tools)}, holds={len(holds)}, ZS_ATTACK={native_attacks}"):
            return _logged(results)

        await _until(lambda: not dialogue.speaking(leader), 10)
        hero_before, leader_before = await gold("HERO"), await gold(leader)
        talk.hear(PAYMENT, details={"via": "scenario"})
        offered = await _until(lambda: leader in deals.open, 20)
        if not offered:
            check("paid", False, f"no offer; tool_outputs={script.outputs}")
            return _logged(results)
        await asyncio.sleep(1.2)
        pressed = await link.press_deal()
        accepted = await _until(lambda: all(parley.truce(npc) for npc in GANG), 15)
        hero_after, leader_after = await gold("HERO"), await gold(leader)
        fact = next((fact for fact in reversed(deals.chronicle.facts("truce"))
                     if fact.get("npc") == leader), None)
        if not check("paid", bool(
            pressed == "sent" and accepted and fact and GANG <= set(fact.get("members", []))
            and hero_after == hero_before - 100 and leader_after == leader_before + 100
            and not native_attacks
        ), f"Y={pressed}, hero_gold={hero_before}->{hero_after}, "
           f"leader_gold={leader_before}->{leader_after}, members={fact.get('members') if fact else []}, "
           f"ZS_ATTACK={native_attacks}"):
            return _logged(results)

        hp_before = (await link.snapshot(leader, fresh=True)).player.hp
        health: list[int | None] = []
        positions: list[tuple[float, float, float]] = []
        peaceful_at = time.monotonic()
        walked = 0
        while time.monotonic() - peaceful_at < 20:
            elapsed = time.monotonic() - peaceful_at
            if walked < 3 and elapsed >= 5 * (walked + 1):
                await link.debug("player_goto", to=sorted(GANG)[walked], distance=250)
                walked += 1
            current = await link.snapshot(leader, fresh=True)
            health.append(current.player.hp if current.player else None)
            if current.player:
                positions.append(current.player.pos)
            await asyncio.sleep(0.25)
        during = [npc for at, npc in native_attacks if at >= peaceful_at]
        travelled = max((math.dist(positions[0], pos) for pos in positions), default=0)
        if not check("passes", bool(
            health and all(hp == hp_before == 1200 for hp in health) and not during and walked == 3
            and travelled > 100 and len(hail_calls()) == 1 and all(parley.truce(npc) for npc in GANG)
        ), f"seconds={time.monotonic() - peaceful_at:.2f}, samples={len(health)}, "
           f"HP={hp_before}->{health[-1]}, positions={walked + 1}, travelled_cm={travelled:.0f}, "
           f"hail_calls={len(hail_calls())}, ZS_ATTACK={during}"):
            return _logged(results)

        # A real weapon transition supplies the production perception and breaks the gang's truce.
        await _m5b_player_weapon(link, "melee")
        drawn = time.monotonic()
        attackers: set[str] = set()
        first_attack: dict[str, float] = {}

        async def attacks_hero() -> bool:
            for npc in sorted(GANG):
                attacking, target = await link.story_eval(npc, [
                    ["call", "NPC_ISINSTATE", ["inst", "SELF"], ["sym", "ZS_ATTACK"]],
                    ["&&", ["call", "NPC_GETTARGET", ["inst", "SELF"]],
                     ["call", "NPC_ISPLAYER", ["inst", "OTHER"]]],
                ])
                if attacking and target:
                    attackers.add(npc)
                    first_attack.setdefault(npc, time.monotonic() - drawn)
            return len(attackers) >= 2

        attacked = await _until_async(attacks_hero, bool, 5)
        first_two = sorted(first_attack.values())[:2]
        check("provoked", bool(
            attacked and len(first_two) == 2 and first_two[-1] <= 5
            and all(parley.truce(npc) is None for npc in GANG)
        ), f"after_draw_s={time.monotonic() - drawn:.2f}, attackers={first_attack}, "
           f"truce_broken={deals.chronicle.facts('truce_broken')}")
    finally:
        dialogue.llm, talk.reflexes = original_llm, original_reflexes
        link.listeners.remove(record)
    return _logged(results)
