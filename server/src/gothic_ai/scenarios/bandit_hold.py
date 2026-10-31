"""Native ranger bandits and a scripted party member wait on renewable parley leases."""

from __future__ import annotations

import asyncio
import math
import time
from collections import Counter
from typing import TYPE_CHECKING

from ..bodies import Bodies
from ..game import GameError, GameLink
from ..protocol import (
    GameMessage,
    HoldVerdictCmd,
    NpcEvent,
    NpcRoster,
    Perception,
    PerceptionHeld,
    RosterEntry,
    ThreatControlCmd,
)
from .common import _logged, _m5b_player_weapon, _until, _until_async, prepare_npc_pose

if TYPE_CHECKING:
    from ..dialogue import DialogueManager

CAVALORN = "BAU_4300_ADDON_CAVALORN"
PRIMARY = "BDT_10302_ADDON_RANGERBANDIT_L"
GANG = {
    f"BDT_{number}_ADDON_RANGERBANDIT_{'M' if number in (10301, 10304) else 'L'}"
    for number in range(10300, 10306)
}
PARTY_POINT, PARK = "NW_FARM2_TO_TAVERN_05", "NW_FARM1_FIELD_06"


async def bandit_hold(link: GameLink, dialogue: DialogueManager) -> dict[str, str]:
    """New game, LLM=fake: two sight directions stay quiet for 30 s, then scripts attack."""
    if dialogue.llm.name != "fake":
        raise GameError("scenario bandit_hold requires LLM=fake and a new game without SAVE")
    await link.wait_world()
    results: dict[str, str] = {}
    roster: dict[str, RosterEntry] = {}
    holds: list[tuple[float, PerceptionHeld]] = []
    native_attacks: list[tuple[float, str]] = []
    damage: list[tuple[float, str]] = []
    responses: list[asyncio.Task[None]] = []
    scripts = False
    first_scripts: asyncio.Future[float] = asyncio.get_running_loop().create_future()

    def check(name: str, ok: bool, detail: object = "") -> bool:
        results[name] = f"{'done' if ok else 'failed'} ({detail})"
        return ok

    async def answer(message: PerceptionHeld, release: bool) -> None:
        await link.send(HoldVerdictCmd(
            hold=message.hold, to="scripts" if release else "parley", ms=None if release else 5000,
        ))
        if release and message.npc in GANG and message.other == "PLAYER" and not first_scripts.done():
            first_scripts.set_result(time.monotonic())

    def record(message: GameMessage) -> None:
        now = time.monotonic()
        if isinstance(message, NpcRoster):
            if message.full:
                roster.clear()
            roster.update((row.id, row) for row in message.npcs)
            native_attacks.extend(
                (now, row.id) for row in message.npcs
                if row.id in GANG | {CAVALORN} and row.state == "ZS_ATTACK"
            )
        elif isinstance(message, PerceptionHeld) and (
            message.kind == "bandit_enemy"
            or message.kind == "party_enemy" and message.npc == CAVALORN
        ):
            if message.npc in GANG or message.kind == "party_enemy":
                holds.append((now, message))
            responses.append(asyncio.create_task(answer(message, scripts)))
        elif isinstance(message, Perception) and message.perc == "ASSESSDAMAGE" and (
            message.npc in {"PLAYER", "HERO", CAVALORN} and message.other in GANG
        ):
            damage.append((now, f"{message.npc}:ASSESSDAMAGE"))
        elif isinstance(message, NpcEvent) and message.other in GANG and (
            message.kind == "hero_attacked" or message.kind == "npc_attack" and message.npc == CAVALORN
        ):
            damage.append((now, f"{message.npc}:{message.kind}"))

    link.listeners.append(record)
    try:
        # Enable before entering sight: the initial native perception must also be held.
        await link.send(ThreatControlCmd(bandits=True))
        await link.debug("time", hour=12, minute=0)
        await link.debug("player_equip", item="ITMW_1H_BAU_MACE")
        await _m5b_player_weapon(link, "none")
        await link.debug("player_hp", hp=1200)
        await link.debug("skip_important", near=CAVALORN)
        await link.debug("player_goto", waypoint=PARK)
        party_ready, party_detail = await prepare_npc_pose(
            link, CAVALORN, PARK, to="PC_HERO", distance=-250,
            require_perception_ready=False,
        )
        party_index = int(await link.evaluate(["var", "AIV_PARTYMEMBER"]))
        await link.story_effect(CAVALORN, [{"do": "party", "value": 1}])
        party_flag = 0

        async def party_marked() -> bool:
            nonlocal party_flag
            [party_flag] = await link.story_eval(CAVALORN, [
                ["field", ["inst", CAVALORN], "AIVAR", party_index],
            ])
            row = roster.get(CAVALORN)
            return bool(party_flag == 1 and row is not None
                        and row.activity == "routine" and row.state == "ZS_STAND_WP")

        marked = await _until_async(party_marked, bool, 5)
        if not check("party_prepared", party_ready and marked,
                     f"party={party_flag}, AIVAR[{party_index}], "
                     f"state={party_detail.get('view', {}).get('state')}, far_waypoint={PARK}"):
            return _logged(results)
        points = {point["name"]: point for point in await link.waypoints()}
        primary = await link.snapshot(PRIMARY, fresh=True)
        approach = points[PARTY_POINT]
        dx, dz = approach["x"] - primary.npc.pos[0], approach["z"] - primary.npc.pos[2]
        length = math.hypot(dx, dz)
        entered = time.monotonic()
        await link.debug("player_goto", pos=(
            primary.npc.pos[0] + 1200 * dx / length, primary.npc.pos[1],
            primary.npc.pos[2] + 1200 * dz / length,
        ))
        if not await _until(lambda: PRIMARY in roster, 5):
            check("scene", False, "ranger bandits missing from the roster")
            return _logged(results)
        present = GANG & roster.keys()
        for npc in sorted(present):
            await link.debug("skip_important", near=npc)
            await link.debug("npc_goto", npc=npc, to=npc, distance=0, face="PC_HERO")
        # Keep the initialized native state and verified party flag when entering the gang's sight.
        await link.debug("npc_goto", npc=CAVALORN, to="PC_HERO", distance=-250, face=PRIMARY)
        party_ready = await _until_async(party_marked, bool, 5)
        # Native alignment and animated sight rays can settle after the placement command returns.
        scene = {}

        async def scene_ready() -> bool:
            primary = await link.snapshot(PRIMARY, fresh=True)
            party = await link.snapshot(CAVALORN, fresh=True)
            hero_distance = primary.player.distance if primary.player else -1
            party_distance = party.player.distance if party.player else -1
            sight, hostility, party_sight, party_hostility, party_flag = await link.story_eval(PRIMARY, [
                ["call", "NPC_CANSEENPC", ["inst", PRIMARY], ["inst", "HERO"]],
                ["==", ["call", "NPC_GETATTITUDE", ["inst", PRIMARY], ["inst", "HERO"]],
                 ["var", "ATT_HOSTILE"]],
                ["call", "NPC_CANSEENPC", ["inst", CAVALORN], ["inst", PRIMARY]],
                ["==", ["call", "NPC_GETATTITUDE", ["inst", CAVALORN], ["inst", PRIMARY]],
                 ["var", "ATT_HOSTILE"]],
                ["field", ["inst", CAVALORN], "AIVAR", party_index],
            ])
            scene.update(primary=primary, party=party, detail=(
                f"hero={hero_distance:.0f} cm, Cavalorn={party_distance:.0f} cm, "
                f"sight={sight}/{party_sight}, hostile={hostility}/{party_hostility}, "
                f"party={party_flag}, gang={sorted(present)}, "
                f"holds={dict(Counter(message.kind for _, message in holds))}, "
                f"ZS_ATTACK={dict(Counter(npc for _, npc in native_attacks))}"
            ))
            return bool(
                party_ready and 1000 <= hero_distance <= 1400 and 0 <= party_distance <= 500
                and sight and hostility and party_sight and party_hostility and party_flag
                and 8 <= primary.world.hour < 23 and PRIMARY in roster and CAVALORN in roster
                and GANG <= roster.keys()
                and all(roster[npc].activity == "routine" for npc in (*present, CAVALORN))
            )

        ready = await _until_async(scene_ready, bool, 5)
        if not check("scene", ready, scene["detail"]):
            return _logged(results)
        primary, party = scene["primary"], scene["party"]

        bandit_seen = await _until(lambda: any(
            message.kind == "bandit_enemy" and message.other == "PLAYER" for _, message in holds
        ), max(0, 10 - (time.monotonic() - entered)))
        first_seen = next((at - entered for at, message in holds
                           if message.kind == "bandit_enemy" and message.other == "PLAYER"), None)
        if not check("bandit_seen", bool(bandit_seen and first_seen is not None and first_seen <= 10),
                     f"first_s={first_seen}, holds={len(holds)}"):
            return _logged(results)

        hero_hp, party_hp = primary.player.hp if primary.player else None, party.npc.hp
        started = time.monotonic()
        health: list[tuple[int | None, int]] = []
        while time.monotonic() - started < 30:
            primary = await link.snapshot(PRIMARY, fresh=True)
            party = await link.snapshot(CAVALORN, fresh=True)
            health.append((primary.player.hp if primary.player else None, party.npc.hp))
            await asyncio.sleep(0.25)
        ended = time.monotonic()
        during = [message for at, message in holds if started <= at <= ended]
        renewals = Counter(message.npc for message in during
                           if message.kind == "bandit_enemy" and message.other == "PLAYER")
        attacks = [npc for at, npc in native_attacks if started <= at <= ended]
        hits = [kind for at, kind in damage if started <= at <= ended]
        check("renewals", max(renewals.values(), default=0) >= 3, dict(renewals))
        check("peace_hero", bool(
            hero_hp == 1200 and health and all(hp == hero_hp for hp, _ in health)
            and not attacks and not hits
        ), f"seconds={ended - started:.2f}, HP={hero_hp}->{health[-1][0]}, "
           f"samples={len(health)}, ZS_ATTACK={attacks}, damage={hits}")
        party_holds = [message for message in during if message.kind == "party_enemy"]
        check("peace_party", bool(
            party_holds and all(hp == party_hp for _, hp in health)
            and not attacks and not hits
        ), f"party_enemy={len(party_holds)}, "
           f"HP={party_hp}->{health[-1][1]}, ZS_ATTACK={attacks}, damage={hits}")
        if any(not outcome.startswith("done") for outcome in results.values()):
            return _logged(results)

        # The shared quiet interval is over. Park the companion so the native release tests the hero.
        parked = await Bodies(link).act(
            CAVALORN, "start_activity", state="ZS_STAND_WP", waypoint=PARK,
            minutes=120, place=True, arrive=True, radius=100, timeout=10,
        )
        if not check("party_parked", parked.status == "arrived", parked.status):
            return _logged(results)
        scripts = True
        try:
            released = await asyncio.wait_for(asyncio.shield(first_scripts), 8)
        except TimeoutError:
            check("scripts_attack", False, "no new held perception within 8 s after renewal stopped")
            return _logged(results)

        async def attacks_hero() -> bool:
            if any(at >= released for at, _ in damage):
                return True
            for npc in sorted(present):
                attacking, target = await link.story_eval(npc, [
                    ["call", "NPC_ISINSTATE", ["inst", "SELF"], ["sym", "ZS_ATTACK"]],
                    ["&&", ["call", "NPC_GETTARGET", ["inst", "SELF"]],
                     ["call", "NPC_ISPLAYER", ["inst", "OTHER"]]],
                ])
                if attacking and target:
                    return True
            return False

        attacked = await _until_async(attacks_hero, bool, max(0, 5 - (time.monotonic() - released)))
        check("scripts_attack", attacked,
              f"after_first_scripts_s={time.monotonic() - released:.2f}, target=HERO or native damage")
    finally:
        link.listeners.remove(record)
        await asyncio.gather(*responses)
        await link.send(ThreatControlCmd(bandits=False))
    return _logged(results)
