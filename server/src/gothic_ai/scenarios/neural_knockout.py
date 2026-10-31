from __future__ import annotations

import asyncio
import logging
import time
from collections.abc import Callable
from dataclasses import dataclass
from typing import Any

from .. import triggers
from ..bodies import Bodies
from ..bodypilot import BodyFight, BodyPilot, FightSummary, PolicyDecisionTrace
from ..dialogue import DialogueManager
from ..game import GameError, GameLink
from ..llm import ToolCall
from ..llm.fake import FakeBackend, FakeStep
from ..protocol import GameMessage, LineStarted, NpcSpec, RoutineEntry
from .common import _ByNpc, _clear_beasts, _m5b_player_weapon, _until, _until_async
from .neural_load import _policy_resets

log = logging.getLogger("gothic_ai.scenarios")


@dataclass(frozen=True)
class HeroFightWait:
    stall_broken: int
    stalled_seconds: float
    last_decisions: tuple[PolicyDecisionTrace, ...]
    elapsed_seconds: float


def _hero_release_state(
    link: GameLink, pilot: BodyPilot, npc: str, summary: FightSummary,
) -> tuple[bool, str]:
    """Check the hero episode's cleanup and the NPC's independently reported owner."""
    lease = summary.lease
    assert lease is not None
    entry = pilot.people.get(npc)
    active = pilot.fights.get(npc)
    hero_fight = bool(active and active._target_kind == "hero" and active.lease)
    own_lease = link.body_leases.get(lease.entity_id) == lease
    cached = link.body_latest.get(lease.entity_id)
    own_cached = bool(cached and (
        cached.entity_id, cached.session_epoch, cached.world_epoch, cached.control_generation,
    ) == (lease.entity_id, lease.session_epoch, lease.world_epoch, lease.control_generation))
    own_watch = hero_fight and npc in pilot._watch_npcs
    # Roster ownership must clear even if local fight/lease state has vanished.
    # Unleased observers of unrelated native fights still don't own this body.
    own_body = bool(entry and entry.activity == "body")
    clean = bool(not hero_fight and not own_lease and not own_cached and not own_watch
                 and not own_body and summary.release_confirmed and summary.watch_removed)
    return clean, (f"hero_leases={int(own_lease)}, hero_watched={int(own_watch)}, "
                   f"cached_rows={int(own_cached)}, hero_body={int(own_body)}, "
                   f"watch_removed={int(summary.watch_removed)}, "
                   f"ignored_observers={int(active is not None and not hero_fight)}")


async def _wait_hero_fight(
    link: GameLink,
    pilot: BodyPilot,
    npc: str,
    fight: BodyFight,
    completed: Callable[[], FightSummary | None],
    *,
    label: str,
    initial_hp: int = 1,
    max_decisions: int | None = None,
    max_seconds: float | None = None,
) -> HeroFightWait:
    """Observe this hero episode with one stall hit, optionally bounded by decisions/time."""
    npc = npc.upper()
    lease = fight.lease
    if lease is None or lease.npc != npc or fight._target_kind != "hero":
        raise GameError(f"{label}: observing combat requires an acquired hero fight")
    started_at = last_damage_at = time.monotonic()
    # KO prepares current HP=1; the bandit sample keeps the hero healthy.
    # Ignore queued pre-preparation HP rather than counting that write as damage.
    last_hp = initial_hp
    baseline_seen = False
    down_seen = False
    stall_broken = 0
    stalled_seconds = 0.0
    last_decisions: tuple[PolicyDecisionTrace, ...] = ()
    finish_deadline: float | None = None

    def episode_summary() -> FightSummary | None:
        summary = completed()
        return summary if (
            summary is not None and summary.npc == npc and summary.lease == lease
            and summary.target_kind == "hero"
        ) else None

    def active_lease() -> bool:
        return bool(
            pilot.fights.get(npc) is fight and fight._target_kind == "hero"
            and link.body_leases.get(lease.entity_id) == lease
        )

    def observe(packet: dict[str, Any]) -> None:
        nonlocal last_hp, baseline_seen, down_seen, last_damage_at
        if fight._target_kind != "hero" or pilot.fights.get(npc) is not fight:
            return
        for row in packet["observations"]:
            if (
                row["entity_id"], row["session_epoch"], row["world_epoch"], row["control_generation"],
            ) != (
                lease.entity_id, lease.session_epoch, lease.world_epoch, lease.control_generation,
            ):
                continue
            own, target = row["self"], row["target"]
            down_seen = down_seen or own["down"] or own["dead"] or own["unconscious"] or own["hp"] <= 0
            if target["entity_id"] != lease.target_entity_id or not target["current_valid"]:
                continue
            hp = target["hp"]
            down_seen = down_seen or bool(target["down"]) or (hp is not None and hp <= 0)
            if type(hp) is not int or (not baseline_seen and hp > initial_hp):
                continue
            baseline_seen = True
            if hp < last_hp:
                last_damage_at = time.monotonic()
            last_hp = hp

    link.raw_body_listeners.append(observe)
    try:
        while True:
            summary = episode_summary()
            if summary is not None:
                if not stall_broken:
                    last_decisions = summary.last_policy_decisions[-10:]
                break
            now = time.monotonic()
            if (max_decisions is not None and fight._decisions >= max_decisions) or (
                max_seconds is not None and now - started_at >= max_seconds
            ):
                break
            if finish_deadline is not None:
                if now >= finish_deadline:
                    break
            elif now - last_damage_at >= 15:
                # A down row or a released original lease can precede its
                # immutable summary. Wait for cleanup without striking again.
                finish_deadline = now + 30
                if not down_seen and active_lease():
                    snapshot = await link.snapshot(npc, fresh=True)
                    if snapshot.player is None:
                        raise GameError(f"{label}: hero position unavailable before stall break")
                    distance = snapshot.player.distance
                    if not down_seen and episode_summary() is None and active_lease() and distance > 450:
                        log.info(
                            "%s stall approach: pre_distance_cm=%.1f teleport_distance_cm=250",
                            label, distance,
                        )
                        await link.debug("teleport", near=npc, distance=250)
                    if not down_seen and episode_summary() is None and active_lease():
                        last_decisions = tuple(fight._policy_decisions)[-10:]
                        stalled_seconds = time.monotonic() - last_damage_at
                        stall_broken = 1
                        log.info(
                            "%s stall_broken count=1 elapsed_s=%.3f no_damage_s=%.3f last10=%s",
                            label, time.monotonic() - started_at, stalled_seconds, last_decisions,
                        )
                        try:
                            await link.debug("player_hit", npc=npc)
                        except GameError as error:
                            if str(error) != "the hit caused no damage":
                                raise
                            log.info("%s stall_broken hit_error=%s", label, error)
                        finish_deadline = time.monotonic() + 30
            await asyncio.sleep(0.05)
    finally:
        link.raw_body_listeners.remove(observe)
    if not last_decisions:
        last_decisions = tuple(fight._policy_decisions)[-10:]
    return HeroFightWait(stall_broken, stalled_seconds, last_decisions, time.monotonic() - started_at)


async def neural_knockout_segment(
    link: GameLink, dialogue: DialogueManager, pilot: BodyPilot,
    *, stalls: list[HeroFightWait] | None = None,
) -> dict[str, str]:
    """A citizen's ordinary AR_NONE fight, real unconsciousness and native recovery."""
    if dialogue.llm.name != "fake":
        raise GameError("scenario neural-talk requires LLM=fake")
    await link.wait_world()
    npc = "DIR_NPC_NEURAL_TALK_CITIZEN"
    arena = "NW_XARDAS_TOWER_VALLEY_09"
    parked_at = "NW_FARM1_FIELD_06"
    hp_trigger = "DIR_NEURAL_TALK_KNOCKOUT_HP"
    results: dict[str, str] = {}
    lines: list[LineStarted] = []

    def check(name: str, ok: bool, detail: str) -> bool:
        key = f"knockout_{name}"
        results[key] = f"{'done' if ok else 'failed'} ({detail})"
        log.info("%-28s %s", key, results[key])
        return ok

    def heard(message: GameMessage) -> None:
        if isinstance(message, LineStarted) and message.npc.upper() == npc:
            lines.append(message)

    def summary_after(previous: set[int]) -> FightSummary | None:
        return next((summary for summary in pilot.summaries if (
            summary.npc == npc and summary.lease is not None and summary.target_kind == "hero"
            and id(summary) not in previous
        )), None)

    await link.debug("player_goto", waypoint="NW_XARDAS_TOWER_VALLEY_01")
    await link.story_npc(NpcSpec(
        npc=npc, like="BDT_1002_BANDIT_L", at=arena, name="Мечник", guild="GIL_VLK",
        routine=[RoutineEntry(start="00:00", end="23:59", state="ZS_STAND_ARMSCROSSED", waypoint=arena)],
    ))
    await link.debug("skip_important", near=npc)
    log.info("neural-talk knockout preparation: beasts_cleared=%d", await _clear_beasts(link, npc))
    ready = await Bodies(link).act(
        npc, "start_activity", state="ZS_STAND_ARMSCROSSED", waypoint=arena,
        minutes=60, arrive=True, radius=100, timeout=15,
    )
    if ready.status != "arrived":
        raise GameError(f"neural-talk knockout preparation: {ready.status} {ready.detail}")
    weapon = ["call", "NPC_GETEQUIPPEDMELEEWEAPON", ["inst", "SELF"]]
    symbol, sword = await link.story_eval(npc, [
        ["sym", npc], ["&", ["field", weapon, "FLAGS"], ["var", "ITEM_SWD"]],
    ])
    if not sword:
        raise GameError("neural-talk knockout preparation: citizen has no one-handed sword")
    await link.debug("player_hp", hp=1200)
    await link.debug("player_equip", item="ITMW_1H_BAU_MACE")
    await _m5b_player_weapon(link, "none")
    await link.debug("teleport", near=npc, distance=250)
    if not await _until(lambda: npc in pilot.people and pilot.people[npc].distance <= 400, 10):
        raise GameError("neural-talk knockout preparation: citizen missing from roster")

    original_llm = dialogue.llm
    dialogue.llm = _ByNpc({"Мечник": FakeBackend(script=[FakeStep(
        "Хочешь проверить мою шпагу? Ну что ж, защищайся!", [ToolCall("set_intent", {
            "goal": "fight", "target": "PLAYER", "attitude": "hostile", "agenda": None,
        })],
    )], delay=0.15)}, FakeBackend("Хм.", delay=0))
    link.listeners.append(heard)
    last_row: dict[str, Any] = {}

    def observe(packet: dict[str, Any]) -> None:
        fight = pilot.fights.get(npc)
        if fight is None or fight.lease is None:
            return
        lease = fight.lease
        for row in packet["observations"]:
            if (row["entity_id"], row["session_epoch"], row["world_epoch"]) != (
                lease.entity_id, lease.session_epoch, lease.world_epoch,
            ) or row["target"]["entity_id"] != lease.target_entity_id:
                continue
            own, target = row["self"], row["target"]
            last_row.update({
                "game_ms": row["t_game_ms"], "npc_pos": own["position_world_cm"],
                "hero_pos": target["position_world_cm"], "hero_hp": target["hp"],
                "hero_max_hp": target["hp_max"], "hero_weapon": target["weapon_state"],
                "hero_down": target["down"], "distance_cm": target["fight_distance_cm"],
            })

    link.raw_body_listeners.append(observe)
    try:
        with _policy_resets() as resets:
            previous = {id(summary) for summary in pilot.summaries}

            def fight_ready() -> bool:
                fight = pilot.fights.get(npc)
                return bool(
                    fight and fight.lease and link.body_leases.get(fight.lease.entity_id) == fight.lease
                    and fight._decisions >= 5
                )

            log.info("neural-talk knockout before speech: beasts_cleared=%d", await _clear_beasts(link, npc))
            await link.debug("type", text="Мечник, проверим твою шпагу.")
            ready = await _until(fight_ready, 25)
            fight = pilot.fights.get(npc)
            lease = fight.lease if fight else None
            client = fight.client if fight else None
            if not check("talk_to_attack", bool(
                ready and lines and lease and lease.npc == npc
                and lease.provider == "neural-body" and lease.mode == "realtime"
                and (pilot.policy == "engine" or resets.get(client)) and fight._fallbacks == 0
            ), f"lines={len(lines)}, symbol={symbol}, "
               f"lease={lease.control_generation if lease else 0}, "
               f"decisions={fight._decisions if fight else 0}, "
               f"fallback={fight._fallbacks if fight else 0}, resets={len(resets.get(client, []))}"):
                return results

            async def draw_hero() -> bool:
                try:
                    await link.debug("player_weapon", state="melee")
                    return True
                except GameError as error:
                    if "cannot switch weapons" not in str(error):
                        raise
                    return False

            drawn = await _until_async(draw_hero, bool, 8)
            if drawn:
                drawn = await _until(lambda: last_row.get("hero_weapon") == 2, 5)
            if not check("hero_draws", drawn,
                         f"drawn={int(drawn)}, weapon_state={last_row.get('hero_weapon')}, "
                         f"decisions={fight._decisions}, leases={len(link.body_leases)}"):
                return results

            # Native attribute 0 is current HP; leave maximum HP (slot 1) at
            # 1200 so the unconscious script can recover the hero normally.
            await link.patch_graph([triggers.trigger(hp_trigger, triggers.STATE, [{
                "do": "call", "call": [
                    "call", "NPC_CHANGEATTRIBUTE", ["inst", "HERO"], ["int", 0],
                    ["-", ["int", 1], ["field", ["inst", "HERO"], "ATTRIBUTE", 0]],
                ],
            }])])
            try:
                applied = await _until_async(link.triggers, lambda current: any(
                    item["id"] == hp_trigger and item["count"] >= 1 for item in current
                ), 5)
            finally:
                await link.patch_graph([triggers.removal(hp_trigger)])
            snap = await link.snapshot(npc, fresh=True)
            if not check("lower_current_hp", bool(
                applied and snap.player and snap.player.hp_max == 1200
            ), f"trigger_count={int(applied)}, hp={snap.player.hp if snap.player else None}, "
               f"hp_max={snap.player.hp_max if snap.player else None}"):
                return results

            down_at = time.monotonic()
            waited = await _wait_hero_fight(
                link, pilot, npc, fight, lambda: summary_after(previous), label="neural-talk knockout",
            )
            if stalls is not None:
                stalls.append(waited)
            check("stall_broken", True,
                  f"count={waited.stall_broken}, seconds={waited.stalled_seconds:.3f}, "
                  f"last10={waited.last_decisions}")
            summary = summary_after(previous)
            ended_at = time.monotonic()
            if not check("fight_to_down", bool(
                summary and summary.reason == "hero_down" and summary.decisions >= 5
                and summary.fallbacks == 0 and summary.release_confirmed
                and summary.release_seconds is not None and summary.release_seconds <= 2
            ), f"decisions={summary.decisions if summary else fight._decisions}, "
               f"fallback={summary.fallbacks if summary else fight._fallbacks}, "
               f"reason={summary.reason if summary else 'active'}, "
               f"release_s={summary.release_seconds if summary else None}, "
               f"transition_s={time.monotonic() - down_at:.3f}, row={last_row}"):
                return results

            async def hero_status() -> list[int]:
                return await link.story_eval(npc, [
                    ["call", "NPC_ISINSTATE", ["inst", "HERO"], ["sym", "ZS_UNCONSCIOUS"]],
                    ["call", "NPC_ISDEAD", ["inst", "HERO"]],
                ])

            unconscious, dead = await hero_status()
            snap = await link.snapshot(npc, fresh=True)
            if not check("real_unconscious", bool(
                unconscious == 1 and dead == 0 and snap.player and snap.player.hp > 0
            ), f"unconscious={unconscious}, dead={dead}, hp={snap.player.hp if snap.player else None}, "
               f"after_down_s={time.monotonic() - ended_at:.3f}"):
                return results

            log.info("neural-talk knockout after down: beasts_cleared=%d", await _clear_beasts(link, npc))
            await asyncio.sleep(max(0, ended_at + 2 - time.monotonic()))
            clean, detail = _hero_release_state(link, pilot, npc, summary)
            if not check("release", clean,
                         f"after_down_s={time.monotonic() - ended_at:.3f}, {detail}"):
                return results

            nearest = float("inf")

            async def routine() -> bool:
                nonlocal nearest
                snapshot = await link.snapshot(npc, fresh=True)
                if snapshot.player:
                    nearest = min(nearest, snapshot.player.distance)
                entry = pilot.people.get(npc)
                return bool(
                    entry and entry.activity == "routine" and entry.state != "ZS_ATTACK"
                    and (await link.routine(npc)).current == "ZS_STAND_ARMSCROSSED"
                )

            returned = await _until_async(routine, bool, 45)
            if not check("native_to_routine", returned,
                         f"closest_cm={nearest:.0f}, wait_s={time.monotonic() - ended_at:.3f}, "
                         f"roster={pilot.people.get(npc)}"):
                return results

            status: list[int] = []

            async def standing() -> bool:
                status[:] = await hero_status()
                return status == [0, 0]

            up = await _until_async(standing, bool, 35)
            snap = await link.snapshot(npc, fresh=True)
            if not check("hero_gets_up", bool(up and snap.player and snap.player.hp > 0),
                         f"standing={int(up)}, hp={snap.player.hp if snap.player else None}, "
                         f"unconscious={status[0] if status else None}, "
                         f"dead={status[1] if status else None}, "
                         f"wait_s={time.monotonic() - ended_at:.3f}"):
                return results

            # Keep this citizen out of the later bandit's fight through a normal
            # native activity, after the knockout and recovery are complete.
            await link.debug("npc_goto", npc=npc, waypoint=parked_at, face="PC_HERO")
            parked = await Bodies(link).act(
                npc, "start_activity", state="ZS_STAND_ARMSCROSSED", waypoint=parked_at,
                minutes=60, arrive=True, radius=100, timeout=15,
            )
            distance, arena_distance = await link.story_eval(npc, [
                ["call", "NPC_GETDISTTOWP", ["inst", "SELF"], ["str", parked_at]],
                ["call", "NPC_GETDISTTOWP", ["inst", "SELF"], ["str", arena]],
            ])
            check("park_citizen", bool(
                parked.status == "arrived" and distance <= 100 and arena_distance > 6000
                and pilot.fights.get(npc) is not fight and link.body_leases.get(lease.entity_id) != lease
            ), f"waypoint={parked_at}, distance_cm={distance}, status={parked.status}, "
               f"arena_distance_cm={arena_distance}, minimum_arena_cm=6000, "
               f"leases={int(link.body_leases.get(lease.entity_id) == lease)}")
    finally:
        dialogue.llm = original_llm
        link.listeners.remove(heard)
        link.raw_body_listeners.remove(observe)
    return results
