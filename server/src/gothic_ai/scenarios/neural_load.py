from __future__ import annotations

import asyncio
import logging
import time
from collections.abc import Iterator
from contextlib import contextmanager
from typing import Any

from .. import triggers
from ..bodies import Bodies
from ..bodypilot import BodyPilot, FightSummary, PolicyClient
from ..game import GameError, GameLink
from ..perception import BEASTS
from ..protocol import BodyLease, GameMessage, Loading, NpcRoster, Session
from .common import (
    _logged,
    _m5b_player_weapon,
    _save_or_fail,
    _until,
    _until_async,
    wait_message,
)

log = logging.getLogger("gothic_ai.scenarios")


@contextmanager
def _policy_resets() -> Iterator[dict[PolicyClient, list[dict[str, Any]]]]:
    """Observe acknowledged resets, keeping client references across completed fights."""
    resets: dict[PolicyClient, list[dict[str, Any]]] = {}
    original = PolicyClient.call

    async def observed(client: PolicyClient, command: str, **fields: Any) -> dict[str, Any]:
        result = await original(client, command, **fields)
        if command == "reset":
            resets.setdefault(client, []).append(dict(fields))
        return result

    PolicyClient.call = observed
    try:
        yield resets
    finally:
        PolicyClient.call = original


async def neural_load_scenario(link: GameLink, pilot: BodyPilot | None) -> dict[str, str]:
    """Load a peaceful save during three real P30 fights, then acquire a fresh fight."""
    if pilot is None:
        raise GameError("scenario neural-load requires --neural-body and an available body policy")
    await link.wait_world()
    if link.hello is None or link.hello.protocol < 45 or pilot.max_fights < 3:
        raise GameError("scenario neural-load requires protocol >=45 and --neural-body-max >=3")
    npcs = tuple(f"DIR_NPC_NEURAL_LOAD_BANDIT_{index}" for index in range(1, 4))
    arena = "NW_XARDAS_TOWER_VALLEY_09"
    slot = 7
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object) -> None:
        results[name] = f"{'done' if ok else 'failed'} ({detail})"

    # Save the prepared, peaceful arena before the first attack. Restoring these
    # same symbols exercises the pilot's old-world cleanup and later acquisition.
    await link.debug("player_goto", waypoint="NW_XARDAS_TOWER_VALLEY_01")
    symbols: dict[str, int] = {}
    weapon = ["call", "NPC_GETEQUIPPEDMELEEWEAPON", ["inst", "SELF"]]
    for index, npc in enumerate(npcs):
        await link.own_npc(npc, like="BDT_1002_BANDIT_L", waypoint=arena)
        await link.debug("skip_important", near=npc)
        for other in (await link.snapshot(npc, fresh=True)).nearby_npcs:
            if not other.dead and other.guild >= BEASTS:
                await link.debug("kill", instance=other.id, near=npc)
        for view in await link.npc_view([npc]):
            for seen in view.sees:
                if seen.guild >= BEASTS and seen.body != "dead":
                    await link.debug("kill", instance=seen.id, near=npc)
        await link.debug("npc_goto", npc=npc, waypoint=arena, face="PC_HERO")
        ready = await Bodies(link).act(
            npc, "start_activity", state="ZS_STAND_ARMSCROSSED", waypoint=arena,
            minutes=60, arrive=True, radius=100, timeout=15,
        )
        if ready.status != "arrived":
            raise GameError(f"neural-load preparation: {npc} {ready.status} {ready.detail}")
        symbol, sword, name = await link.story_eval(npc, [
            ["sym", npc], ["&", ["field", weapon, "FLAGS"], ["var", "ITEM_SWD"]],
            ["field", weapon, "NAME"],
        ])
        if not sword:
            raise GameError(f"neural-load preparation: {npc} has no one-handed sword ({name})")
        symbols[npc] = symbol
        log.info("neural-load arena: npc=%s symbol=%s weapon=%s", npc, symbol, name)
        await link.debug("npc_goto", npc=npc, to=npc, distance=700 + index * 350, face="PC_HERO")
    if len(set(symbols.values())) != 3:
        raise GameError("neural-load preparation: bandit symbols are not unique")
    for npc in npcs:
        drawn = await Bodies(link).act(npc, "draw_weapon", kind="melee")
        if drawn.status != "done":
            raise GameError(f"neural-load preparation: {npc} sword draw {drawn.status}")
    await link.debug("player_equip", item="ITMW_1H_BAU_MACE")
    await _m5b_player_weapon(link, "melee")
    await link.debug("npc_goto", npc=npcs[0], waypoint=arena, face="PC_HERO")
    await link.debug("npc_goto", npc=npcs[1], to=npcs[0], distance=-250, face="PC_HERO")
    await link.debug("npc_goto", npc=npcs[2], to=npcs[0], distance=600, face="PC_HERO")

    async def live_health() -> list[int | None]:
        snapshots = [await link.snapshot(npc, fresh=True) for npc in npcs]
        player = snapshots[0].player
        if player is None:
            raise GameError("neural-load preparation: player health unavailable")
        return [player.hp_max, player.hp, *[
            value for snapshot in snapshots for value in (snapshot.npc.hp_max, snapshot.npc.hp)
        ]]

    before = await live_health()
    if len(before) != 8 or any(type(value) is not int for value in before):
        raise GameError(f"neural-load preparation: health unavailable {before}")
    health = [max(minimum, maximum) for minimum, maximum in zip(
        (1200, 1800, 1800, 1800), before[::2], strict=True,
    ) for _ in range(2)]
    health_trigger = "DIR_NEURAL_LOAD_HEALTH"
    await link.patch_graph([triggers.trigger(health_trigger, triggers.STATE, [
        {"do": "call", "call": [
            "call", "NPC_CHANGEATTRIBUTE", ["inst", actor], ["int", attribute],
            ["-", ["int", health[index * 2 + offset]],
             ["field", ["inst", actor], "ATTRIBUTE", attribute]],
        ]}
        for index, actor in enumerate(("HERO", *npcs))
        for offset, attribute in enumerate((1, 0))
    ])])
    try:
        if not await _until_async(live_health, lambda got: got == health, 5):
            raise GameError("neural-load preparation: health did not apply")
    finally:
        await link.patch_graph([triggers.removal(health_trigger)])
    saved = await _save_or_fail(link, slot)
    check("neural_load_save", not pilot.active_count and not link.body_leases,
          f"slot={slot}, seq={saved.seq}, fights={pilot.active_count}, leases={len(link.body_leases)}")

    previous = {id(summary) for summary in pilot.summaries}
    old_leases: dict[str, BodyLease] = {}
    old_clients: dict[str, PolicyClient] = {}
    summaries: dict[str, FightSummary] = {}
    completed_at: dict[str, float] = {}
    observed_swords: set[tuple[int, int, int, int]] = set()
    max_active = 0
    loading_at: float | None = None
    roster_after_load: NpcRoster | None = None
    packet_times: list[float] = []

    def identity(lease: BodyLease) -> tuple[int, int, int, int]:
        return lease.session_epoch, lease.world_epoch, lease.entity_id, lease.control_generation

    def collect_summaries() -> None:
        for summary in pilot.summaries:
            if id(summary) not in previous and summary.npc in old_leases and (
                summary.lease == old_leases[summary.npc]
            ):
                previous.add(id(summary))
                summaries[summary.npc] = summary
                completed_at[summary.npc] = time.monotonic()

    def observe_message(message: GameMessage) -> None:
        nonlocal loading_at, roster_after_load
        if isinstance(message, Loading) and loading_at is None:
            loading_at = time.monotonic()
        elif isinstance(message, NpcRoster) and loading_at is not None and message.full:
            roster_after_load = message
        collect_summaries()

    def observe_body(packet: dict[str, Any]) -> None:
        nonlocal max_active
        active = [lease for lease in link.body_leases.values() if (
            lease.npc in npcs and pilot.handles(lease.npc)
            and pilot.fights[lease.npc].lease == lease
        )]
        max_active = max(max_active, len(active))
        if packet["observations"]:
            packet_times.append(time.monotonic())
        for row in packet["observations"]:
            for lease in active:
                if (
                    row["script_symbol"] == symbols[lease.npc]
                    and (
                        row["session_epoch"], row["world_epoch"], row["entity_id"], row["control_generation"],
                    ) == identity(lease)
                    and row["observation_seq"] > lease.acquired_observation_seq
                    and row["periodic"] and row["self"]["weapon_state"] == 2
                ):
                    observed_swords.add(identity(lease))
        collect_summaries()

    attack_trigger = "DIR_NEURAL_LOAD_ATTACK"
    link.listeners.append(observe_message)
    link.raw_body_listeners.append(observe_body)
    try:
        with _policy_resets() as resets:
            def all_active() -> bool:
                for npc in npcs:
                    fight = pilot.fights.get(npc)
                    if fight is not None and fight.lease is not None and (
                        link.body_leases.get(fight.lease.entity_id) == fight.lease
                    ):
                        old_leases[npc] = fight.lease
                        if fight.client is not None:
                            old_clients[npc] = fight.client
                return len(old_leases) == 3 and all(
                    pilot.handles(npc) and pilot.fights[npc].lease == old_leases[npc]
                    and link.body_leases.get(old_leases[npc].entity_id) == old_leases[npc]
                    and identity(old_leases[npc]) in observed_swords
                    and (pilot.policy == "engine" or resets.get(old_clients.get(npc), []))
                    for npc in npcs
                )

            await link.debug("teleport", near=npcs[0], distance=250)
            await link.patch_graph([triggers.trigger(attack_trigger, triggers.STATE, [
                {"do": "call", "call": [
                    "fn", "B_ATTACK", triggers.npc(npc), triggers.HERO, ["var", "AR_NONE"], ["int", 0],
                ]}
                for npc in npcs
            ])])
            acquired = await _until(all_active, 20)
            check("neural_load_three_leases", acquired and max_active == 3,
                  f"active={len(link.body_leases)}, max_active={max_active}, expected=3, "
                  f"decisions={[pilot.fights[npc]._decisions if pilot.handles(npc) else 0 for npc in npcs]}, "
                  f"resets={[len(resets.get(old_clients.get(npc), [])) for npc in npcs]}")
            if not acquired:
                return _logged(results)
            await link.patch_graph([triggers.removal(attack_trigger)])
            loaded = asyncio.create_task(wait_message(link, Session, lambda msg: msg.reason == "load"))
            try:
                await link.debug("load", slot)
                session = await loaded
            finally:
                loaded.cancel()
            await link.wait_world()
            loaded_at = time.monotonic()

            def load_finished() -> bool:
                collect_summaries()
                return len(summaries) == 3 and not any(pilot.handles(npc) for npc in npcs)

            await _until(load_finished, 5)
            for index, npc in enumerate(npcs, 1):
                summary = summaries.get(npc)
                after_loading = completed_at[npc] - loading_at if (
                    npc in completed_at and loading_at is not None
                ) else None
                check(f"neural_load_{index}_loading", bool(
                    summary and summary.reason == "loading" and summary.release_confirmed
                    and summary.release_seconds is not None and summary.release_seconds <= 2
                    and summary.fallbacks == 0
                ), f"decisions={summary.decisions if summary else 0}, "
                   f"fallback={summary.fallbacks if summary else None}, "
                   f"reason={summary.reason if summary else None}, "
                   f"release_s={summary.release_seconds if summary else None}, "
                   f"after_loading_s={after_loading}, "
                   f"confirmed={summary.release_confirmed if summary else False}")
            await asyncio.sleep(max(0, loaded_at + 2 - time.monotonic()))
            collect_summaries()
            body_count = sum(entry.activity == "body" for entry in roster_after_load.npcs) if (
                roster_after_load is not None
            ) else -1
            recent_packets = sum(stamp >= time.monotonic() - 0.5 for stamp in packet_times)
            check("neural_load_idle", bool(
                session.parent == saved.branch and session.parent_seq == saved.seq
                and len(summaries) == 3 and not pilot.active_count and not link.body_leases
                and body_count == 0 and not link.body_latest
                and not pilot._watch_npcs and not pilot._pending_unwatch and recent_packets == 0
            ), f"after_load_s={time.monotonic() - loaded_at:.3f}, fights={pilot.active_count}, "
               f"leases={len(link.body_leases)}, body={body_count}, watched={len(pilot._watch_npcs)}, "
               f"pending_unwatch={len(pilot._pending_unwatch)}, cached_rows={len(link.body_latest)}, "
               f"packets_last_0.5s={recent_packets}")

            npc = npcs[0]
            # Only one post-load attacker: bandits left in earshot would join
            # B_ATTACK through their native guild perceptions, creating extra fights.
            for other in npcs[1:]:
                await link.debug("npc_goto", npc=other, waypoint="NW_XARDAS_TOWER_VALLEY_01")
            await link.debug("npc_goto", npc=npc, waypoint=arena, face="PC_HERO")
            await link.debug("teleport", near=npc, distance=250)
            await link.story_effect(npc, [{"do": "attack", "reason": "AR_NONE"}])

            def fresh_fight() -> bool:
                fight = pilot.fights.get(npc)
                return bool(
                    fight and fight.lease and link.body_leases.get(fight.lease.entity_id) == fight.lease
                    and fight.lease != old_leases.get(npc) and (pilot.policy == "engine" or (
                        fight.client is not old_clients.get(npc) and resets.get(fight.client, [])
                    )) and fight._decisions >= 5
                    and identity(fight.lease) in observed_swords
                )

            reacquired = await _until(fresh_fight, 20)
            fight = pilot.fights.get(npc)
            lease = fight.lease if fight else None
            reset_calls = resets.get(fight.client, []) if fight else []
            check("neural_load_reacquire", bool(
                reacquired and fight and fight._fallbacks == 0 and lease
                and lease.provider == "neural-body" and lease.mode == "realtime"
                and old_leases.get(npc) is not None
                and (lease.session_epoch, lease.world_epoch) != (
                    old_leases[npc].session_epoch, old_leases[npc].world_epoch,
                )
                and (pilot.policy == "engine" or (
                    reset_calls[0]["session_epoch"] == lease.session_epoch
                    and reset_calls[0]["world_epoch"] == lease.world_epoch
                ))
            ), f"decisions={fight._decisions if fight else 0}, "
               f"fallback={fight._fallbacks if fight else None}, resets={len(reset_calls)}, "
               f"new_client={bool(fight and fight.client is not old_clients.get(npc))}, "
               f"old_world={old_leases[npc].world_epoch if npc in old_leases else None}, "
               f"new_world={lease.world_epoch if lease else None}")

            # Finish the post-load probe through a native loss of the distant hero.
            escaped_at = time.monotonic()
            await link.debug("teleport", near=npc, distance=6000)
            await _until(lambda: not pilot.active_count and not link.body_leases, 8)
            summary = next((summary for summary in reversed(pilot.summaries) if (
                summary.npc == npc and summary.lease == lease
            )), None)
            check("neural_load_release", bool(
                summary and summary.reason in {"target_lost", "npc_unavailable"}
                and summary.release_confirmed and summary.watch_removed
                and not pilot.active_count and not link.body_leases and not link.body_latest
            ), f"decisions={summary.decisions if summary else 0}, "
               f"reason={summary.reason if summary else None}, "
               f"release_s={summary.release_seconds if summary else None}, "
               f"after_escape_s={time.monotonic() - escaped_at:.3f}, leases={len(link.body_leases)}")
    finally:
        link.listeners.remove(observe_message)
        link.raw_body_listeners.remove(observe_body)
        await link.patch_graph([triggers.removal(attack_trigger)])
    return _logged(results)
