"""A road gang hails the hero, sells peace, and attacks only after a provocation."""

from __future__ import annotations

import asyncio
import math
import time
from typing import TYPE_CHECKING, Any

from ..bodies import Bodies
from ..brain.reflex import Reflexes
from ..deals import GOLD
from ..game import GameLink
from ..llm import ToolCall
from ..parley import HAIL_CUE
from ..protocol import GameMessage, NpcActResult, NpcRoster, PerceptionHeld, RosterEntry
from .bandit_hold import CAVALORN, GANG, PARK, PARTY_POINT, PRIMARY
from .common import FixedDecider, _logged, _m5b_player_weapon, _until, _until_async, model_off

SCENARIOS = {
    "road_bandits": ("GIVEN", {"save": False, "timeout": 240}),
}

if TYPE_CHECKING:
    from ..deals import Deals
    from ..dialogue import DialogueManager
    from ..parley import Parley
    from ..talk import TalkDesk

PAYMENT = "Дам сто золотых за мир со всей вашей шайкой."


async def road_bandits(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    deals: Deals,
    parley: Parley,
) -> dict[str, str]:
    """New game, GIVEN (LLM=off via auto): a road gang sells peace, then reacts to a provocation.

    given: ultimatum for 100 gold, 60 s on the hail; propose_deal for 100 gold
    and a 4 h truce on the payment reply.
    """
    off = model_off(dialogue)
    await link.wait_world()
    results: dict[str, str] = {}
    roster: dict[str, RosterEntry] = {}
    native_attacks: list[tuple[float, str]] = []
    holds: list[PerceptionHeld] = []
    approaches: list[NpcActResult] = []
    original_reflexes = talk.reflexes
    leader = max(sorted(GANG), key=talk.lead)
    hail_given = "дано: главарь дорожной шайки требует сто золотых за проход"
    off.given(hail_given, who=leader, when=HAIL_CUE,
              line="Стой, чужак. Сто золотых за проход. Минута на ответ.", calls=[ToolCall(
                  "ultimatum", {"demand": "Заплати сто золотых за проход", "seconds": 60},
              )])
    off.given("дано: главарь предлагает перемирие за сто золотых", who=leader, when=PAYMENT,
              line="Сотню сюда — и четыре часа наши тебя не тронут.", calls=[ToolCall(
                  "propose_deal", {
                      "npc_gives": [], "hero_gives": [{"item": GOLD, "count": 100}],
                      "truce": {"hours": 4},
                  },
              )])
    off.silent("дано: главарь молчит (сделка состоялась)", who=leader, when="сделка состоялась")

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

    def hail_count() -> int:
        return off.used.count(hail_given)

    async def gold(npc: str) -> int:
        return int(await link.evaluate([
            "call", "NPC_HASITEMS", ["inst", npc], ["sym", GOLD],
        ]) or 0)

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
        snap = await link.snapshot(PRIMARY, fresh=True)
        cavalorn = await link.snapshot(CAVALORN, fresh=True)
        away = math.dist(snap.npc.pos, cavalorn.npc.pos)
        if not check("prepared", bool(
            ready and parked.status == "arrived" and away > 6000
            and snap.player and snap.player.hp == 1200 and 8 <= snap.world.hour < 23
        ), f"gang={sorted(GANG & roster.keys())}, leader={leader}, "
           f"Cavalorn={away:.0f} cm ({parked.status}), hour={snap.world.hour}"):
            return _logged(results)

        hailed = await _until(lambda: bool(hail_count() and parley.ultimatum(leader)), 30)
        leader_snap = await link.snapshot(leader, fresh=True)
        moved = math.dist(origins[leader].pos, leader_snap.npc.pos)
        turned = abs((leader_snap.npc.rot - origins[leader].rot + 180) % 360 - 180)
        if not check("hail", bool(
            hailed and hail_count() == 1 and any(row.npc == leader for row in approaches)
            and talk.partner is not None and talk.partner.npc == leader
            and (moved > 50 or turned > 5)
            and any(hold.kind == "bandit_enemy" and hold.other == "PLAYER" for hold in holds)
            and not native_attacks
        ), f"after_s={time.monotonic() - entered:.2f}, calls={hail_count()}, "
           f"leader={leader}, move_cm={moved:.0f}, turn_deg={turned:.1f}, "
           f"approaches={[(row.npc, row.action) for row in approaches]}, "
           f"partner={talk.partner.npc if talk.partner else None}, "
           f"holds={len(holds)}, ZS_ATTACK={native_attacks}"):
            return _logged(results)

        await _until(lambda: not dialogue.speaking(leader), 10)
        hero_before, leader_before = await gold("HERO"), await gold(leader)
        talk.hear(PAYMENT, details={"via": "scenario"})
        offered = await _until(lambda: leader in deals.open, 20)
        if not offered:
            check("paid", False, f"no offer; used_givens={off.used}")
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

        initial = await link.snapshot(leader, fresh=True)
        hp_before = initial.player.hp if initial.player else None
        health: list[int | None] = []
        positions = [initial.player.pos] if initial.player else []
        peaceful_at = time.monotonic()
        await link.debug("destruction_traces", text="signal road_walk")
        while time.monotonic() - peaceful_at < 20:
            current = await link.snapshot(leader, fresh=True)
            health.append(current.player.hp if current.player else None)
            if current.player:
                positions.append(current.player.pos)
            await asyncio.sleep(0.25)
        during = [npc for at, npc in native_attacks if at >= peaceful_at]
        travelled = max((math.dist(positions[0], pos) for pos in positions), default=0)
        steps = [math.dist(before, after) for before, after in zip(positions, positions[1:], strict=False)]
        path_cm, max_step = sum(steps), max(steps, default=0)
        moving_samples = sum(step > 5 for step in steps)
        if not check("passes", bool(
            health and all(hp == hp_before == 1200 for hp in health) and not during
            and travelled > 100 and moving_samples >= 8 and max_step <= 150 and path_cm >= 5 * max_step
            and hail_count() == 1 and all(parley.truce(npc) for npc in GANG)
        ), f"seconds={time.monotonic() - peaceful_at:.2f}, samples={len(health)}, "
           f"HP={hp_before}->{health[-1]}, travelled_cm={travelled:.0f}, path_cm={path_cm:.0f}, "
           f"moving_samples={moving_samples}, max_step_cm={max_step:.0f}, "
           f"hail_calls={hail_count()}, ZS_ATTACK={during}"):
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
        talk.reflexes = original_reflexes
        link.listeners.remove(record)
    return _logged(results)
