from __future__ import annotations

import asyncio
import logging
import math
import time
from collections.abc import AsyncIterator, Callable
from typing import Any, cast

from .. import triggers
from ..beasts import BeastDesk
from ..bodies import Bodies
from ..bodypilot import BodyPilot, FightSummary
from ..brain import NpcPlanner
from ..budget import Budget
from ..dialogue import DialogueManager
from ..game import GameError, GameLink
from ..incidents import IncidentDesk
from ..llm import ToolCall
from ..llm.fake import FakeBackend, FakeStep
from ..perception import BEASTS, PerceptionDesk
from ..persona import PersonaProvider
from ..protocol import (
    Beasts,
    BodyLease,
    DebugCmd,
    DialogChosen,
    GameMessage,
    LineStarted,
    NpcDied,
    NpcEvent,
    NpcRoster,
    NpcSpec,
    PerceptionHeld,
    RoutineEntry,
    SayCmd,
    ThreatControlCmd,
)
from ..talk import TalkDesk
from .common import (
    FixedDecider,
    _ByNpc,
    _logged,
    _m5b_player_weapon,
    _OnWords,
    _until,
    _until_async,
    attach_when_free,
)

log = logging.getLogger("gothic_ai.scenarios")


async def neural_scenario(link: GameLink, pilot: BodyPilot | None) -> dict[str, str]:
    """A real one-handed bandit/hero fight, from native acquisition through confirmed release."""
    return await _neural_fight_scenario(link, pilot, companion=False)


async def neural_companion_scenario(link: GameLink, pilot: BodyPilot | None) -> dict[str, str]:
    """A one-handed bandit attacks the hero's swordsman companion, with native combat on both sides."""
    return await _neural_fight_scenario(link, pilot, companion=True)


async def neural_talk_scenario(
    link: GameLink, dialogue: DialogueManager, pilot: BodyPilot | None,
) -> dict[str, str]:
    """A non-bandit knockout, then words, escape and a fresh fight with a hostile bandit."""
    from .neural_knockout import (
        HeroFightWait,
        _hero_release_state,
        _wait_hero_fight,
        neural_knockout_segment,
    )
    from .neural_load import _policy_resets

    if pilot is None:
        raise GameError("scenario neural-talk requires GOTHIC_BODY_URL and --neural-body")
    if dialogue.llm.name != "fake":
        raise GameError("scenario neural-talk requires LLM=fake")
    stalls: list[HeroFightWait] = []
    results = await neural_knockout_segment(link, dialogue, pilot, stalls=stalls)

    def stall_total() -> None:
        results["stall_broken"] = (
            f"done (count={sum(item.stall_broken for item in stalls)}, "
            f"seconds={sum(item.stalled_seconds for item in stalls):.3f})"
        )
        log.info("%-28s %s", "stall_broken", results["stall_broken"])

    stall_total()
    if any(not value.startswith("done") for value in results.values()):
        return results
    await link.wait_world()
    npc, arena = "DIR_NPC_NEURAL_TALK_BANDIT", "NW_XARDAS_TOWER_VALLEY_09"
    lines: list[LineStarted] = []
    attacks: list[str] = []

    def check(name: str, ok: bool, detail: str) -> bool:
        results[name] = f"{'done' if ok else 'failed'} ({detail})"
        log.info("%-28s %s", name, results[name])
        return ok

    def heard(message: GameMessage) -> None:
        if isinstance(message, LineStarted) and message.npc.upper() == npc:
            lines.append(message)

    def attacked(attacker: str) -> None:
        if attacker.upper() == npc:
            attacks.append(attacker)

    def fight_ready() -> bool:
        fight = pilot.fights.get(npc)
        return bool(fight and fight._target_kind == "hero" and fight.lease
                    and link.body_leases.get(fight.lease.entity_id) == fight.lease
                    and fight._decisions >= 5)

    async def idle_after(ended_at: float, summary: FightSummary) -> tuple[bool, str]:
        await asyncio.sleep(max(0, ended_at + 2 - time.monotonic()))
        clean, detail = _hero_release_state(link, pilot, npc, summary)
        return clean, f"after_s={time.monotonic() - ended_at:.3f}, {detail}"

    async def routine() -> bool:
        state = (await link.routine(npc)).current
        return state == "ZS_STAND_ARMSCROSSED"

    def summary_after(previous: set[int]) -> FightSummary | None:
        # A speech-ownership retry can finish an observer before the real fight.
        return next((summary for summary in pilot.summaries
                     if summary.npc == npc and summary.lease is not None and summary.target_kind == "hero"
                     and id(summary) not in previous), None)

    def ended(name: str, summary: FightSummary | None, allowed: set[str], since: float) -> bool:
        active = pilot.fights.get(npc)
        return check(name, bool(summary and summary.reason in allowed and summary.decisions >= 5
                               and summary.fallbacks == 0 and summary.release_confirmed
                               and summary.release_seconds is not None and summary.release_seconds <= 2),
                     f"decisions={summary.decisions if summary else active._decisions if active else 0}, "
                     f"fallback={summary.fallbacks if summary else active._fallbacks if active else 0}, "
                     f"reason={summary.reason if summary else 'active' if active else 'missing'}, "
                     f"release_s={summary.release_seconds if summary else None}, "
                     f"transition_s={time.monotonic() - since:.3f}")

    # This bandit has the same native sword as neural. Give its daily routine a
    # known local waypoint, so returning to it can be observed without issuing stop/calm.
    log.info("neural-talk preparation: place hero, create bandit")
    await link.debug("player_goto", waypoint="NW_XARDAS_TOWER_VALLEY_01")
    await link.story_npc(NpcSpec(
        npc=npc, like="BDT_1002_BANDIT_L", at=arena, name="Бандит", guild="GIL_BDT",
        routine=[RoutineEntry(start="00:00", end="23:59", state="ZS_STAND_ARMSCROSSED", waypoint=arena)],
    ))
    log.info("neural-talk preparation: bandit created, clear wildlife")
    await link.debug("skip_important", near=npc)
    for other in (await link.snapshot(npc, fresh=True)).nearby_npcs:
        if not other.dead and other.guild >= BEASTS:
            await link.debug("kill", instance=other.id, near=npc)
    for view in await link.npc_view([npc]):
        for seen in view.sees:
            if seen.guild >= BEASTS and seen.body != "dead":
                await link.debug("kill", instance=seen.id, near=npc)
    ready = await Bodies(link).act(
        npc, "start_activity", state="ZS_STAND_ARMSCROSSED", waypoint=arena,
        minutes=60, arrive=True, radius=100, timeout=15,
    )
    if ready.status != "arrived":
        raise GameError(f"neural-talk preparation: {ready.status} {ready.detail}")
    weapon = ["call", "NPC_GETEQUIPPEDMELEEWEAPON", ["inst", "SELF"]]
    symbol, sword = await link.story_eval(npc, [
        ["sym", npc], ["&", ["field", weapon, "FLAGS"], ["var", "ITEM_SWD"]],
    ])
    if not sword:
        raise GameError("neural-talk preparation: no one-handed sword")
    await link.debug("player_hp", hp=1200)
    await link.debug("player_equip", item="ITMW_1H_BAU_MACE")
    await _m5b_player_weapon(link, "none")
    # Hold the actual conversation before the hostile guild can see the hero.
    # Production _attack_hero releases it after the scripted words are heard.
    await link.attach(npc)
    await link.debug("teleport", near=npc, distance=250)
    if not await _until(lambda: npc in pilot.people and pilot.people[npc].distance <= 400, 10):
        raise GameError("neural-talk preparation: bandit missing from roster")

    attack = ToolCall("set_intent", {
        "goal": "fight", "target": "PLAYER", "attitude": "hostile", "agenda": None,
    })
    original_llm = dialogue.llm
    dialogue.llm = _ByNpc(
        {"Бандит": FakeBackend(script=[
            FakeStep("Ну что, герой, решил проверить мою шпагу? Сам напросился!", [attack]),
            FakeStep("Опять ты. Что тебе?"),
        ], delay=0.15)}, FakeBackend("Хм.", delay=0),
    )
    link.listeners.append(heard)
    dialogue.on_attack.append(attacked)
    try:
        with _policy_resets() as resets:
            previous = {id(summary) for summary in pilot.summaries}
            mark = len(lines)
            await link.debug("type", text="Бандит, проверим твою шпагу.")
            ready = await _until(lambda: bool(attacks) and fight_ready(), 25)
            first = pilot.fights.get(npc)
            first_lease = first.lease if first else None
            first_client = first.client if first else None
            unleased_attempts = sum(
                s.npc == npc and s.lease is None and id(s) not in previous for s in pilot.summaries
            )
            if not check("talk_to_attack", bool(ready and len(lines) > mark and first_lease
                                                and first_lease.npc == npc and resets.get(first_client)),
                         f"lines={len(lines) - mark}, symbol={symbol}, "
                         f"lease={first_lease.control_generation if first_lease else 0}, "
                         f"decisions={first._decisions if first else 0}, "
                         f"fallback={first._fallbacks if first else 0}, "
                         f"resets={len(resets.get(first_client, []))}, "
                         f"unleased_attempts={unleased_attempts}, attacks={len(attacks)}"):
                snap = await link.snapshot(npc, fresh=True)
                log.info("neural-talk handoff: roster=%s routine=%s hero_hp=%s summaries=%s",
                         pilot.people.get(npc), (await link.routine(npc)).current,
                         snap.player.hp if snap.player else None,
                         [(s.reason, s.decisions, dict(s.acquire_refusals)) for s in pilot.summaries])
                return results

            fled_at = time.monotonic()
            await link.debug("player_goto", waypoint="NW_XARDAS_TOWER_VALLEY_01")
            await _until(lambda: summary_after(previous) is not None, 8)
            summary = summary_after(previous)
            ended_at = time.monotonic()
            if not ended("fight_to_flight", summary, {"target_lost", "npc_unavailable"}, fled_at):
                return results
            clean, detail = await idle_after(ended_at, summary)
            if not check("flight_release", clean, detail):
                return results
            returned = await _until_async(routine, bool, 40)
            state = (await link.routine(npc)).current
            if not check("flight_to_routine", returned,
                         f"state={state}, wait_s={time.monotonic() - fled_at:.3f}"):
                return results

            previous = {id(summary) for summary in pilot.summaries}
            await link.debug("player_hp", hp=1200)
            # Match neural's actual combat loadout once the conversation is over.
            # This watch is still safe while the hero is far and the pilot is idle.
            await _m5b_player_weapon(link, "melee")
            # Match neural's melee start; this segment tests a fresh handoff,
            # then a controlled escape rather than the policy's ability to win.
            await link.debug("teleport", near=npc, distance=250)

            def attacking() -> bool:
                entry = pilot.people.get(npc)
                return bool(entry and entry.activity in {"fight", "body"} and entry.state == "ZS_ATTACK")

            # GIL_BDT sees a returning enemy: another native attack is expected.
            # Only ask for another conversation when that perception did not attack.
            reattacked = await _until(attacking, 5)
            branch = "native_reattack" if reattacked else "talk_then_attack"
            if not reattacked:
                await _m5b_player_weapon(link, "none")
                mark = len(lines)
                await link.debug("type", text="Бандит, я вернулся. Поговорим?")
                answered = await _until(lambda: any(
                    "Опять ты" in dialogue._lines.get(line.line, ("", None))[0]
                    for line in lines[mark:]
                ), 15)
                if not check("return_to_talk", answered,
                             f"branch={branch}, lines={len(lines) - mark}, roster={pilot.people.get(npc)}"):
                    return results
                await _until(lambda: not dialogue.speaking(npc), 12)
                await _m5b_player_weapon(link, "melee")
                await link.story_effect(npc, [{"do": "attack", "reason": "AR_NONE"}])
            ready = await _until(fight_ready, 25)
            second = pilot.fights.get(npc)
            second_lease = second.lease if second else None
            second_client = second.client if second else None
            if not check("return_to_second_fight", bool(
                ready and second_lease and second_lease != first_lease
                and second_client is not first_client and resets.get(second_client)
                and second._fallbacks == 0
            ), f"branch={branch}, lease={second_lease.control_generation if second_lease else 0}, "
               f"previous_lease={first_lease.control_generation if first_lease else 0}, "
               f"decisions={second._decisions if second else 0}, "
               f"fallback={second._fallbacks if second else 0}, "
               f"resets={len(resets.get(second_client, []))}, "
               f"independent_client={second_client is not first_client}, "
               f"distance_cm={pilot.people[npc].distance if npc in pilot.people else None}"):
                return results
            snap = await link.snapshot(npc, fresh=True)
            if snap.player is None:
                raise GameError("neural-talk bandit sample: hero health unavailable")
            waited = await _wait_hero_fight(
                link, pilot, npc, second, lambda: summary_after(previous), label="neural-talk bandit",
                initial_hp=snap.player.hp, max_decisions=20, max_seconds=20,
            )
            stalls.append(waited)
            check("bandit_stall_broken", True,
                  f"count={waited.stall_broken}, seconds={waited.stalled_seconds:.3f}, "
                  f"last10={waited.last_decisions}")
            stall_total()
            if not check("second_fight_sample", bool(
                (second._decisions >= 20 or waited.elapsed_seconds >= 20) and second._fallbacks == 0
                and pilot.fights.get(npc) is second
                and link.body_leases.get(second_lease.entity_id) == second_lease
            ), f"decisions={second._decisions}, elapsed_s={waited.elapsed_seconds:.3f}, "
               f"limit_decisions=20, limit_s=20, fallback={second._fallbacks}, "
               f"hero_hp_start={snap.player.hp}"):
                return results
            fled_at = time.monotonic()
            await link.debug("player_goto", waypoint="NW_XARDAS_TOWER_VALLEY_01")
            await _until(lambda: summary_after(previous) is not None, 8)
            summary = summary_after(previous)
            ended_at = time.monotonic()
            if summary is None:
                ended("fight_end_clean", summary, {"target_lost", "npc_unavailable"}, fled_at)
                return results
            clean, detail = await idle_after(ended_at, summary)
            check("fight_end_clean", bool(
                clean and summary.reason in {"target_lost", "npc_unavailable"}
                and summary.decisions >= 5 and summary.fallbacks == 0
                and summary.release_seconds is not None and summary.release_seconds <= 2
            ), f"decisions={summary.decisions}, fallback={summary.fallbacks}, reason={summary.reason}, "
               f"release_s={summary.release_seconds}, transition_s={time.monotonic() - fled_at:.3f}, "
               f"{detail}")
    finally:
        dialogue.llm = original_llm
        link.listeners.remove(heard)
        dialogue.on_attack.remove(attacked)
    return results


async def neural_group_scenario(link: GameLink, pilot: BodyPilot | None) -> dict[str, str]:
    """Three independent P30 leases in a native melee against the hero and his companion."""
    if pilot is None:
        raise GameError("scenario neural-group requires GOTHIC_BODY_URL and --neural-body")
    await link.wait_world()
    if link.hello is None or link.hello.protocol < 45 or pilot.max_fights < 3:
        raise GameError("scenario neural-group requires protocol >=45 and --neural-body-max >=3")
    npcs = tuple(f"DIR_NPC_NEURAL_BANDIT_{index}" for index in range(1, 4))
    companion = "DIR_NPC_NEURAL_COMPANION"
    companion_name = "Спутник-мечник"
    arena = "NW_XARDAS_TOWER_VALLEY_09"
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object) -> None:
        results[name] = f"{'done' if ok else 'failed'} ({detail})"

    await link.debug("player_goto", waypoint="NW_XARDAS_TOWER_VALLEY_01")
    weapon = ["call", "NPC_GETEQUIPPEDMELEEWEAPON", ["inst", "SELF"]]
    symbols: dict[str, int] = {}
    for index, actor in enumerate((*npcs, companion)):
        # Prepare one actor at a time, then park it away from the waypoint. Four
        # native walks to the same occupied spot cannot all finish their AI queues.
        # Each bandit also needs its own symbol: body_watch rejects copies of one.
        if actor == companion:
            await link.story_npc(NpcSpec(
                npc=companion, like="BDT_1002_BANDIT_L", at=arena,
                name=companion_name, guild="GIL_VLK",
                routine=[RoutineEntry(
                    start="00:00", end="23:59", state="ZS_STAND_ARMSCROSSED", waypoint=arena,
                )],
            ))
        else:
            await link.own_npc(actor, like="BDT_1002_BANDIT_L", waypoint=arena)
        await link.debug("skip_important", near=actor)
        for other in (await link.snapshot(actor, fresh=True)).nearby_npcs:
            if not other.dead and other.guild >= BEASTS:
                await link.debug("kill", instance=other.id, near=actor)
        for view in await link.npc_view([actor]):
            for seen in view.sees:
                if seen.guild >= BEASTS and seen.body != "dead":
                    await link.debug("kill", instance=seen.id, near=actor)
        await link.debug("npc_goto", npc=actor, waypoint=arena, face="PC_HERO")
        ready = await Bodies(link).act(
            actor, "start_activity", state="ZS_STAND_ARMSCROSSED", waypoint=arena,
            minutes=60, arrive=True, radius=100, timeout=15,
        )
        if ready.status != "arrived":
            snap = await link.snapshot(actor, fresh=True)
            state = (await link.routine(actor)).current
            raise GameError(
                f"neural-group preparation: {actor} {ready.status} {ready.detail}, "
                f"state={state}, pos={snap.npc.pos}"
            )
        symbol, sword, name = await link.story_eval(actor, [
            ["sym", actor], ["&", ["field", weapon, "FLAGS"], ["var", "ITEM_SWD"]],
            ["field", weapon, "NAME"],
        ])
        if not sword:
            raise GameError(f"neural-group preparation: {actor} has no one-handed sword ({name})")
        symbols[actor] = symbol
        log.info("neural-group arena: npc=%s symbol=%s weapon=%s", actor, symbol, name)
        await link.debug("npc_goto", npc=actor, to=actor, distance=700 + index * 350, face="PC_HERO")
    if len(set(symbols.values())) != 4:
        raise GameError("neural-group preparation: actor symbols are not unique")

    await link.story_effect(companion, [{"do": "party", "value": 1}])
    for actor in (*npcs, companion):
        drawn = await Bodies(link).act(actor, "draw_weapon", kind="melee")
        if drawn.status != "done":
            raise GameError(f"neural-group preparation: {actor} sword draw {drawn.status}")
    await link.debug("player_equip", item="ITMW_1H_BAU_MACE")
    await _m5b_player_weapon(link, "melee")
    await link.debug("npc_goto", npc=npcs[0], waypoint=arena, face="PC_HERO")
    await link.debug("npc_goto", npc=npcs[1], to=npcs[0], distance=-250, face="PC_HERO")
    await link.debug("npc_goto", npc=companion, to=npcs[0], distance=700, face=npcs[0])
    await link.debug("npc_goto", npc=npcs[2], to=companion, distance=250, face=companion)

    # One preparation trigger, removed before combat; no healing while the pilot fights.
    # Native ATTRIBUTE slots: maximum HP = 1, current HP = 0. Read the engine's
    # snapshot as well: a VM-only check can agree with a wrongly indexed write.
    actors = ("HERO", companion, *npcs)
    attributes = (1, 0)
    health_fields = [
        ["field", ["inst", actor], "ATTRIBUTE", attribute]
        for actor in actors for attribute in attributes
    ]

    async def live_health() -> list[int | None]:
        snapshots = [await link.snapshot(actor, fresh=True) for actor in actors[1:]]
        player = snapshots[0].player
        if player is None:
            raise GameError("neural-group preparation: player health unavailable")
        return [player.hp_max, player.hp, *[
            value for snapshot in snapshots for value in (snapshot.npc.hp_max, snapshot.npc.hp)
        ]]

    before = await live_health()
    if len(before) != 10 or any(type(value) is not int for value in before):
        raise GameError(f"neural-group preparation: health unavailable {before}")
    health = [max(minimum, maximum) for minimum, maximum in zip(
        (1200, 900, 1800, 1800, 1800), before[::2], strict=True,
    ) for _ in range(2)]
    health_trigger = "DIR_NEURAL_GROUP_HEALTH"
    await link.patch_graph([triggers.trigger(health_trigger, triggers.STATE, [
        {"do": "call", "call": [
            "call", "NPC_CHANGEATTRIBUTE", ["inst", actor], ["int", attribute],
            ["-", ["int", health[index * 2 + offset]], health_fields[index * 2 + offset]],
        ]}
        for index, actor in enumerate(actors) for offset, attribute in enumerate(attributes)
    ])])
    try:
        if not await _until_async(
            live_health, lambda got: got == health, 5,
        ):
            raise GameError("neural-group preparation: health did not apply")
    finally:
        await link.patch_graph([triggers.removal(health_trigger)])
    log.info("neural-group arena: hp_max/hp before=%s prepared=%s", before, health)

    previous = {id(summary) for summary in pilot.summaries}
    summaries: dict[int, FightSummary] = {}
    completed_at: dict[int, float] = {}
    leases: dict[str, BodyLease] = {}
    leased_entities: dict[tuple[int, int, int, int], int] = {}
    one_handed: set[tuple[int, int, int, int]] = set()
    down_at: dict[tuple[int, int, int, int], float] = {}
    down_reason: dict[tuple[int, int, int, int], str] = {}
    max_leases = 0

    def identity(lease: BodyLease) -> tuple[int, int, int, int]:
        return lease.session_epoch, lease.world_epoch, lease.entity_id, lease.control_generation

    def observe(packet: dict[str, Any]) -> None:
        nonlocal max_leases
        now = time.monotonic()
        for npc in npcs:
            fight = pilot.fights.get(npc)
            if fight is not None and fight.lease is not None:
                leases[npc] = fight.lease
        # Count actual acknowledged ownership, rather than observing/acquiring fights.
        active = {lease.npc for lease in link.body_leases.values() if (
            lease.npc in npcs and pilot.handles(lease.npc)
            and leases.get(lease.npc) == lease
        )}
        max_leases = max(max_leases, len(active))
        for npc, lease in leases.items():
            key = identity(lease)
            for row in packet["observations"]:
                if (
                    row["script_symbol"] != symbols[npc]
                    or (row["entity_id"], row["session_epoch"], row["world_epoch"])
                    != (lease.entity_id, lease.session_epoch, lease.world_epoch)
                    or row["control_generation"] not in {0, lease.control_generation}
                    or row["observation_seq"] <= lease.acquired_observation_seq
                ):
                    continue
                if row["periodic"] and row["control_generation"] == lease.control_generation:
                    if key not in leased_entities:
                        log.info(
                            "neural-group first leased row: npc=%s hp=%s target_hp=%s target_hp_max=%s",
                            npc, row["self"]["hp"], row["target"]["hp"], row["target"]["hp_max"],
                        )
                    leased_entities[key] = row["entity_id"]
                    if row["self"]["weapon_state"] == 2:
                        one_handed.add(key)
                own, target = row["self"], row["target"]
                if key not in down_at:
                    if own["dead"] or own["unconscious"] or own["down"] or own["hp"] <= 0:
                        down_reason[key] = "npc_down"
                    elif target["entity_id"] == lease.target_entity_id and (
                        target["down"] or (target["hp"] is not None and target["hp"] <= 0)
                    ):
                        down_reason[key] = "target_down"
                    if key in down_reason:
                        down_at[key] = now

    def completed() -> bool:
        for summary in pilot.summaries:
            if id(summary) not in previous and summary.npc in npcs:
                summaries[id(summary)] = summary
                previous.add(id(summary))
                # Each fight gets its own clock; the last duel must not inflate earlier releases.
                completed_at[id(summary)] = time.monotonic()
        return set(npcs) <= {summary.npc for summary in summaries.values()} and not any(
            pilot.handles(npc) for npc in npcs
        )

    attack_trigger = "DIR_NEURAL_GROUP_ATTACK"
    link.raw_body_listeners.append(observe)
    try:
        await link.debug("teleport", near=npcs[0], distance=250)
        combat_health = await live_health()
        if combat_health != health:
            raise GameError(f"neural-group preparation: health changed before attack {combat_health}")
        log.info("neural-group combat ready: hp_max/hp=%s", combat_health)
        # All three native B_ATTACK calls run in the same trigger tick, without soft acts
        # that would block body_acquire. Scripts may subsequently choose either party member.
        await link.patch_graph([triggers.trigger(attack_trigger, triggers.STATE, [
            {"do": "call", "call": [
                "fn", "B_ATTACK", triggers.npc(npc), target, ["var", "AR_NONE"], ["int", 0],
            ]}
            for npc, target in zip(npcs, (triggers.HERO, triggers.HERO, triggers.npc(companion)), strict=True)
        ])])
        if not await _until(completed, 192):
            log.info("neural-group timeout: hp_max/hp=%s active_fights=%d",
                     await live_health(), pilot.active_count)
    finally:
        link.raw_body_listeners.remove(observe)
        await link.patch_graph([triggers.removal(attack_trigger)])

    check("neural_group_simultaneous", max_leases == 3, f"max_active_leases={max_leases}, expected=3")
    for index, npc in enumerate(npcs, 1):
        npc_summaries = [summary for summary in summaries.values() if summary.npc == npc]
        decisions = sum(summary.decisions for summary in npc_summaries)
        fallbacks = sum(summary.fallbacks for summary in npc_summaries)
        # A bandit may finish with the companion and then attack the hero. Check
        # every lease and service episode, with a separate end/release clock.
        for episode, summary in enumerate(npc_summaries or [None], 1):
            prefix = f"neural_group_{index}"
            if len(npc_summaries) > 1:
                prefix += f"_fight_{episode}"
            lease = summary.lease if summary else None
            key = identity(lease) if lease else None
            check(f"{prefix}_lease", bool(
                lease and lease.npc == npc and lease.entity_id == leased_entities.get(key)
                and lease.provider == "neural-body" and lease.mode == "realtime" and key in one_handed
                and (summary.target_kind == "hero" or (
                    summary.target_kind == "companion" and summary.target_name == companion_name
                ))
            ), f"npc={npc}, entity={leased_entities.get(key)}, one_handed={key in one_handed}, "
               f"target={summary.target_kind if summary else None}, lease={lease}")
            check(f"{prefix}_decisions", bool(summary) and decisions >= 10,
                  f"decisions_per_bandit={decisions}, minimum=10")
            check(f"{prefix}_fallback", bool(summary) and fallbacks == 0,
                  f"fallback_per_bandit={fallbacks}")
            expected_end = down_reason.get(key)
            if expected_end == "target_down" and summary is not None and summary.target_kind == "hero":
                expected_end = "hero_down"
            check(f"{prefix}_outcome", bool(summary and expected_end and summary.reason == expected_end),
                  f"reason={summary.reason if summary else None}, observed={expected_end}")
            released_in = completed_at[id(summary)] - down_at[key] if key in down_at else None
            check(f"{prefix}_release", bool(
                summary and summary.release_confirmed and released_in is not None and released_in <= 2
            ), f"confirmed={summary.release_confirmed if summary else False}, after_down_s={released_in}")
            p95 = summary.service_p95_ms if summary else None
            check(f"{prefix}_service_p95", p95 is not None and p95 <= 20, f"p95_ms={p95}, maximum_ms=20")
            check(f"{prefix}_summary", summary is not None,
                  f"npc={npc}, decisions={summary.decisions}, fallback={summary.fallbacks}, "
                  f"acquire_refusals={dict(summary.acquire_refusals)}, "
                  f"action_rejections={dict(summary.action_rejections)}, reason={summary.reason}, "
                  f"fight_s={summary.fight_seconds}, service_p50_ms={summary.service_p50_ms}, "
                  f"service_p95_ms={summary.service_p95_ms}" if summary else f"npc={npc}, missing")
    await asyncio.sleep(0.3)
    check("neural_group_idle", bool(
        set(npcs) <= {summary.npc for summary in summaries.values()}
        and all(summary.watch_removed for summary in summaries.values())
        and pilot.active_count == 0 and not any(pilot.handles(npc) for npc in npcs)
        and not link.body_leases and not link.body_latest
    ), f"active_fights={pilot.active_count}, leases={len(link.body_leases)}, "
       f"watched_actors={len(link.body_latest)}, "
       f"watch_removed={[summary.watch_removed for summary in summaries.values()]}")
    return _logged(results)


async def _neural_fight_scenario(
    link: GameLink, pilot: BodyPilot | None, *, companion: bool,
) -> dict[str, str]:
    scenario = "neural-companion" if companion else "neural"
    if pilot is None:
        raise GameError(f"scenario {scenario} requires GOTHIC_BODY_URL and --neural-body")
    await link.wait_world()
    npc = "DIR_NPC_NEURAL_BANDIT"
    target = "DIR_NPC_NEURAL_COMPANION" if companion else "PLAYER"
    target_name = "Спутник-мечник" if companion else None
    arena = "NW_XARDAS_TOWER_VALLEY_09"
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object) -> None:
        results[name] = f"{'done' if ok else 'failed'} ({detail})"

    # Prepare while separated: no scripted opening conversation or wildlife in the duel.
    await link.debug("player_goto", waypoint="NW_XARDAS_TOWER_VALLEY_01")
    # Stock BDT_1002 carries a sword but has several instances in the world. Give this
    # new bandit its own symbol so body_watch and the lease identify exactly one actor.
    await link.own_npc(npc, like="BDT_1002_BANDIT_L", waypoint=arena)
    await link.debug("skip_important", near=npc)
    for other in (await link.snapshot(npc, fresh=True)).nearby_npcs:
        if not other.dead and other.guild >= BEASTS:
            await link.debug("kill", instance=other.id, near=npc)
    for view in await link.npc_view([npc]):
        for seen in view.sees:
            if seen.guild >= BEASTS and seen.body != "dead":
                await link.debug("kill", instance=seen.id, near=npc)
    ready = await Bodies(link).act(
        npc, "start_activity", state="ZS_STAND_ARMSCROSSED", waypoint=arena,
        minutes=60, arrive=True, radius=100, timeout=15,
    )
    if ready.status != "arrived":
        raise GameError(f"neural arena preparation: {ready.status} {ready.detail}")
    weapon = ["call", "NPC_GETEQUIPPEDMELEEWEAPON", ["inst", "SELF"]]
    symbol, sword, weapon_name = await link.story_eval(npc, [
        ["sym", npc], ["&", ["field", weapon, "FLAGS"], ["var", "ITEM_SWD"]],
        ["field", weapon, "NAME"],
    ])
    if not sword:
        raise GameError(f"neural arena preparation: {npc} has no one-handed sword ({weapon_name})")
    log.info("neural arena: npc=%s weapon=%s", npc, weapon_name)
    if companion:
        # A separate symbol makes the companion unambiguous. Its native melee loadout and
        # skills match the attacker; the party effect is the server's companion flag as well.
        await link.story_npc(NpcSpec(
            npc=target, like="BDT_1002_BANDIT_L", at=arena,
            name=cast(str, target_name), guild="GIL_VLK",
            routine=[RoutineEntry(
                start="00:00", end="23:59", state="ZS_STAND_ARMSCROSSED", waypoint=arena,
            )],
        ))
        await link.debug("skip_important", near=target)
        ready = await Bodies(link).act(
            target, "start_activity", state="ZS_STAND_ARMSCROSSED", waypoint=arena,
            minutes=60, arrive=True, radius=100, timeout=15,
        )
        if ready.status != "arrived":
            raise GameError(f"neural companion preparation: {ready.status} {ready.detail}")
        [companion_sword] = await link.story_eval(target, [
            ["&", ["field", weapon, "FLAGS"], ["var", "ITEM_SWD"]],
        ])
        if not companion_sword:
            raise GameError(f"neural companion preparation: {target} has no one-handed sword")
        # Stock clones can fall to a few critical hits before six policy decisions.
        # Prepare both swordsmen with at least the hero scenario's 300 HP, once,
        # through an existing director trigger, then verify and remove it before combat.
        attributes = await link.story_eval(npc, [["var", "ATR_HITPOINTSMAX"], ["var", "ATR_HITPOINTS"]])
        health_fields = [
            ["field", ["inst", actor], "ATTRIBUTE", attribute]
            for actor in (npc, target) for attribute in attributes
        ]
        before = await link.story_eval(npc, health_fields)
        if len(before) != 4 or any(type(value) is not int for value in before):
            raise GameError(f"neural companion preparation: health unavailable {before}")
        health = [max(300, maximum) for maximum in before[::2] for _ in range(2)]
        health_trigger = "DIR_NEURAL_COMPANION_HEALTH"
        await link.patch_graph([triggers.trigger(health_trigger, triggers.STATE, [
            {"do": "set", "target": field, "value": ["int", value]}
            for field, value in zip(health_fields, health, strict=True)
        ])])
        try:
            if not await _until_async(
                lambda: link.story_eval(npc, health_fields), lambda got: got == health, 5
            ):
                raise GameError("neural companion preparation: health did not apply")
        finally:
            await link.patch_graph([{"op": "trigger", "id": health_trigger, "steps": None}])
        log.info("neural companion arena: hp_max/hp before=%s prepared=%s", before, health)
        await link.story_effect(target, [{"do": "party", "value": 1}])
        drawn = await Bodies(link).act(target, "draw_weapon", kind="melee")
        if drawn.status != "done":
            raise GameError(f"neural companion preparation: sword draw {drawn.status} {drawn.detail}")
    else:
        # The passive hero survives enough decisions, then falls to actual melee damage.
        await link.debug("player_hp", hp=300)
        await link.debug("player_equip", item="ITMW_1H_BAU_MACE")
        await _m5b_player_weapon(link, "melee")
    drawn = await Bodies(link).act(npc, "draw_weapon", kind="melee")
    if drawn.status != "done":
        raise GameError(f"neural arena preparation: sword draw {drawn.status} {drawn.detail}")
    previous = pilot.last_summary
    leased_entity: int | None = None
    one_handed = False
    down_at: float | None = None
    down_reason: str | None = None

    def observe(packet: dict[str, Any]) -> None:
        nonlocal leased_entity, one_handed, down_at, down_reason
        fight = pilot.fights.get(npc)
        lease = fight.lease if fight is not None else None
        if lease is None or lease.npc != npc:
            return
        for row in packet["observations"]:
            if (
                row["script_symbol"] != symbol
                or (row["entity_id"], row["session_epoch"], row["world_epoch"])
                != (lease.entity_id, lease.session_epoch, lease.world_epoch)
                or row["control_generation"] not in {0, lease.control_generation}
                or row["observation_seq"] <= lease.acquired_observation_seq
            ):
                continue
            if row["periodic"] and row["control_generation"] == lease.control_generation:
                leased_entity = row["entity_id"]
                one_handed |= row["self"]["weapon_state"] == 2
            # Falling can restore native ownership before the next periodic row.
            # These scoped event-only/native rows still independently prove the end.
            own, target = row["self"], row["target"]
            if down_at is None:
                if own["dead"] or own["unconscious"] or own["down"] or own["hp"] <= 0:
                    down_reason = "npc_down"
                elif target["entity_id"] == lease.target_entity_id and (
                    target["down"] or (target["hp"] is not None and target["hp"] <= 0)
                ):
                    down_reason = "target_down" if companion else "hero_down"
                if down_reason is not None:
                    down_at = time.monotonic()

    link.raw_body_listeners.append(observe)
    started = time.monotonic()
    try:
        if companion:
            await link.debug("npc_goto", npc=npc, waypoint=arena, face="PC_HERO")
            await link.debug("npc_goto", npc=target, to=npc, distance=250, face=npc)
            await link.debug("npc_goto", npc=npc, waypoint=arena, face=target)
            # Still within the pilot's 20 m range, outside the swordsmen's melee and
            # weapon-draw perceptions. The passive hero cannot intercept their attacks.
            await link.debug("teleport", near=npc, distance=1500)
            await link.npc_act(npc, "fight", npc=target)
            # fight tracks the entire ZS_ATTACK as a soft act, which blocks body_acquire.
            # A short turn replaces that tracking without changing the native state or target;
            # its npc_idle confirms the soft act has ended before the lease can be granted.
            turned = await Bodies(link).act(npc, "turn_to", npc=target, timeout=4)
            if turned.status != "done":
                raise GameError(
                    f"neural companion preparation: combat handoff {turned.status} {turned.detail}"
                )
            target_matches, attacking = await link.story_eval(npc, [
                ["&&", ["call", "NPC_GETTARGET", ["inst", "SELF"]],
                 ["==", ["call", "HLP_GETINSTANCEID", ["inst", "OTHER"]], ["sym", target]]],
                ["call", "NPC_ISINSTATE", ["inst", "SELF"], ["sym", "ZS_ATTACK"]],
            ])
            check("neural_companion_target", bool(target_matches) and bool(attacking) and (
                pilot.companion(target) or bool(link.party.get(target, (0, 0))[1])),
                  f"target={target}, live_matches={target_matches}, attacking={attacking}, "
                  f"party={link.party.get(target)}")
        else:
            await link.debug("npc_goto", npc=npc, waypoint=arena, face="PC_HERO")
            await link.debug("teleport", near=npc, distance=250)
            await link.story_effect(npc, [{"do": "attack", "reason": "AR_NONE"}])
        finished = await _until(lambda: pilot.last_summary is not previous, 92)
        completed_at = time.monotonic()
    finally:
        link.raw_body_listeners.remove(observe)
    summary = pilot.last_summary if finished else None
    if summary is not None:
        log.info(
            "neural acquire: requests=%d completed=%d request_to_lease_ms p50=%s max=%s",
            summary.acquire_requests, summary.acquire_completed,
            summary.acquire_wait_p50_ms, summary.acquire_wait_max_ms,
        )
        log.info(
            "neural pipeline ms p50/p95: packet_to_request=%s/%s packet_to_response=%s/%s "
            "response_to_action=%s/%s packet_to_action=%s/%s; acquire_refusals=%s action_rejections=%s "
            "transient_rejections_consecutive=%d",
            summary.packet_to_request_p50_ms, summary.packet_to_request_p95_ms,
            summary.packet_to_response_p50_ms, summary.packet_to_response_p95_ms,
            summary.response_to_action_p50_ms, summary.response_to_action_p95_ms,
            summary.packet_to_action_p50_ms, summary.packet_to_action_p95_ms,
            dict(summary.acquire_refusals), dict(summary.action_rejections),
            summary.transient_rejections_consecutive,
        )
    lease = summary.lease if summary else None
    check(
        "neural_lease",
        bool(lease and summary.npc == npc and lease.npc == npc and lease.entity_id == leased_entity
             and lease.provider == "neural-body" and lease.mode == "realtime" and one_handed),
        f"npc={npc}, entity={leased_entity}, sword={bool(sword)}, one_handed={one_handed}, lease={lease}",
    )
    if companion:
        check("neural_companion_lease",
              bool(summary and summary.target_kind == "companion" and summary.target_name == target_name),
              f"kind={summary.target_kind if summary else None}, "
              f"name={summary.target_name if summary else None}, expected={target_name}")
    minimum = 6 if companion else 10
    check("neural_decisions", summary is not None and summary.decisions >= minimum,
          f"decisions={summary.decisions if summary else None}, minimum={minimum}")
    check("neural_fallback", summary is not None and summary.fallbacks == 0,
          f"fallback={summary.fallbacks if summary else None}")
    duration = down_at - started if down_at is not None else None
    check(
        "neural_outcome",
        bool(summary and summary.reason in {"npc_down", "target_down" if companion else "hero_down"}
             and summary.reason == down_reason
             and duration is not None and duration <= 90),
        f"reason={summary.reason if summary else None}, observed={down_reason}, duration_s={duration}",
    )
    # Completion includes policy close and unwatch, so this also bounds the earlier native release.
    released_in = completed_at - down_at if down_at is not None else None
    check(
        "neural_release",
        bool(summary and summary.release_confirmed and released_in is not None and released_in <= 2),
        f"confirmed={summary.release_confirmed if summary else False}, after_down_s={released_in}",
    )
    p95 = summary.service_p95_ms if summary else None
    check("neural_service_p95", p95 is not None and p95 <= 20, f"p95_ms={p95}, maximum_ms=20")
    await asyncio.sleep(0.3)
    check(
        "neural_idle",
        bool(summary and summary.watch_removed and pilot.active_count == 0 and not link.body_leases),
        f"active_fights={pilot.active_count}, leases={len(link.body_leases)}, "
        f"watch_removed={summary.watch_removed if summary else False}",
    )
    return _logged(results)


async def rescue(
    link: GameLink,
    npc: str = "BAU_954_MALETH",
    planner: NpcPlanner | None = None,
    killer: str = "hero",
    dialogue: DialogueManager | None = None,
) -> dict[str, str]:
    """M5c on the game, an ordinary npc: the hero comes up to the shepherd, a wolf attacks him from behind.
    The body reacts at once (a call for help: he runs from the wolf, shouting, and the scripts keep out of it)
    and the conversational model speaks for him on the move. Then the wolf dies: the danger is over
    (`monster_killed`). Killed by the hero (`killer="hero"`), the shepherd reacts to his rescuer, his attitude
    is asked, and he speaks again; killed by the shepherd himself (`killer="npc"`), he says a word about the
    danger he spoke of. With the server's planner (DECIDER_URL) the live models decide and the server's LLM
    speaks; without it a fixed stand-in decides and the words are scripted."""
    from ..brain import GameApplier
    from ..incidents import FREE_NEW_REASON_S

    await link.wait_world()
    results: dict[str, str] = {}
    lines: list[tuple[float, LineStarted]] = []
    link.listeners.append(
        lambda m: lines.append((time.monotonic(), m)) if isinstance(m, LineStarted) else None
    )
    events: list[NpcEvent | PerceptionHeld] = []
    link.listeners.append(
        lambda m: events.append(m) if isinstance(m, NpcEvent | PerceptionHeld) and m.npc == npc else None
    )
    runner = None
    if planner is None:
        planner = NpcPlanner(
            FixedDecider({"react": ("thank", "call_help", "comment")}, {"escalate": 3.6, "attitude": 4.0}),
            GameApplier(link),
        )
        link.listeners.append(planner.on_message)
        runner = asyncio.create_task(planner.run())
        after = "Спасибо, выручил ты меня." if killer == "hero" else "Хм… а волк-то был не такой уж страшный."
        llm = FakeBackend(script=[FakeStep("Волк! Помогите, волк!"), FakeStep(after)], delay=0)
        dialogue = DialogueManager(link, llm, PersonaProvider(None))
        IncidentDesk(link, dialogue, None, Budget(), None, planner=planner)
        await link.send(ThreatControlCmd(hold=True))
    # the npc's lines as the server sent them: line id -> (when, text); the game reports only the id
    texts: dict[str, tuple[float, str]] = {}
    send = link.send

    async def sending(cmd: Any) -> Any:
        if isinstance(cmd, SayCmd) and cmd.npc.upper() == npc:
            texts[cmd.line] = (time.monotonic(), cmd.text)
        return await send(cmd)

    link.send = sending  # type: ignore[method-assign]
    # when the npc's turns about events began: a line sent after one begun after the kill is about the kill
    # (a turn begins once the one before has sent its lines)
    turns: list[float] = []
    event_turn = dialogue.event_turn if dialogue is not None else None
    if dialogue is not None:

        def turn(who: str, *args: Any, **kwargs: Any) -> Any:
            if who.upper() == npc:
                turns.append(time.monotonic())
            return event_turn(who, *args, **kwargs)

        dialogue.event_turn = turn  # type: ignore[method-assign]
    recent = getattr(planner.applier, "recent", [])
    verdicts = getattr(planner.applier, "verdicts", [])
    # what the npc was doing, by the roster: (when it started, activity); a weapon out is "fight", an
    # original dialog with the hero "dialog"
    doing: list[tuple[float, str]] = []

    def track(m: GameMessage) -> None:
        if isinstance(m, NpcRoster):
            now = next((e.activity for e in m.npcs if e.id == npc), "away")
            if not doing or doing[-1][1] != now:
                doing.append((time.monotonic(), now))

    link.listeners.append(track)
    # what the behavioral model is asked about the npc, word for word (the results quote it)
    asked: list[tuple[str, str]] = []
    decide = planner.decider.decide

    async def recording(rid, lane, priority, deadline_ms, state, questions):  # noqa: ANN001, ANN202
        if rid.startswith(npc):
            asked.append(("+".join(questions), state))
        return await decide(rid, lane, priority, deadline_ms, state, questions)

    planner.decider.decide = recording  # type: ignore[method-assign]

    def decided(since: float, question: str, threat: bool | None = None) -> list:
        return [
            d
            for d in list(recent)[since:]
            if d.npc == npc and d.question == question and (threat is None or bool(d.target) == threat)
        ]

    async def wait(check: Callable[[], Any], timeout: float) -> Any:
        for _ in range(int(timeout / 0.1)):
            if found := check():
                return found
            await asyncio.sleep(0.1)
        return None

    try:
        # 1. the hero next to the npc, looking at it. The npc's important dialog (the start of the game: it
        # has not met the hero) would start as soon as the two are close and calm; lines on the move do not go
        # over a dialog
        await link.debug("skip_important", near=npc)
        await link.debug("teleport", near=npc, distance=600)
        rec = await wait(lambda: (r := planner.npcs.get(npc)) is not None and r.in_view and r, 10)
        results["hero_next_to_npc"] = (
            f"done ({rec.name} {rec.distance_cm / 100:.0f} m away, in view)"
            if rec
            else "failed (not in view)"
        )
        await asyncio.sleep(2.0)  # whatever the npc does about the hero coming up

        # 2. a wolf from behind the npc: the body reacts at once, the words follow on the move
        mark, t0 = len(recent), time.monotonic()
        await link.debug("spawn", instance="WOLF", near=npc, distance=-500)
        reaction = await wait(lambda: decided(mark, "react", threat=True), 15)
        seen = ", ".join(dict.fromkeys(f"{e.type}:{e.kind}/{e.other}" for e in events)) or "none"
        results["threat_reaction"] = (
            f"done ({reaction[0].choice} {reaction[0].confidence:.2f} against {reaction[0].target}, "
            f"{time.monotonic() - t0:.1f} s; events: {seen})"
            if reaction
            else f"failed (no reaction to the wolf; events: {seen}; planner: {planner.snapshot()})"
        )
        first = await wait(lambda: next(((t, m) for t, m in lines if m.npc == npc and t >= t0), None), 12)
        results["words_about_the_wolf"] = (
            f"done ({first[0] - t0:.1f} s after the wolf came: {texts.get(first[1].line, (0, '?'))[1]})"
            if first
            else "failed (no words)"
        )
        holds = {e.hold for e in events if isinstance(e, PerceptionHeld)}
        to = [v for h, v in list(verdicts) if h in holds]
        armed = [t for t, what in doing if what == "fight" and t >= t0]
        took = reaction[0].choice if reaction else None
        results["body_and_scripts"] = (
            f"{'done' if (took == 'fight') == bool(armed) else 'failed'} ({took}: "
            f"{to.count('server')} held from the scripts, {to.count('scripts')} given to them; "
            f"{f'weapon out {armed[0] - t0:.1f} s after the wolf came' if armed else 'no weapon out'})"
        )

        # 3. the wolf dies: the danger is over, the npc thanks its rescuer, or says a word about the danger it
        # beat itself. Soon, while the wolf is still after the npc (one it lost is too far for the npc to see
        # it die), but not before words about something new may follow (asked for right after the reaction)
        await asyncio.sleep(max(0.0, t0 + FREE_NEW_REASON_S + 1.0 - time.monotonic()))
        # the hero runs after it: an npc that ran from the wolf is soon out of the hero's reach (the bridge
        # reports npc's within 30 m of him) and of his hearing (20 m)
        await link.debug("teleport", near=npc, distance=700)
        await asyncio.sleep(1.0)  # the roster (twice a second) has him close: the npc's reaction is urgent
        mark, t1 = len(recent), time.monotonic()
        try:
            # the one after the npc, not a wild one
            await link.debug("kill", instance="WOLF", near=npc, by="PLAYER" if killer == "hero" else npc)
            killed = f"by the {killer}"
        except GameError as err:
            killed = f"not by the hero ({err})"
        ended = await wait(
            lambda: next((e for e in events if isinstance(e, NpcEvent) and e.kind == "monster_killed"), None),
            5,
        )
        results["monster_killed"] = (
            f"done ({killed}: {ended.other} killed {ended.victim} '{ended.detail}')"
            if ended
            else f"failed (no monster_killed; {killed})"
        )
        rec = planner.npcs.get(npc)
        threats = [s.kind for s in rec.stimuli if s.policy.threat and s.other == "WOLF"] if rec else []
        results["threat_over"] = "done" if rec and not threats else f"failed (still: {threats})"
        # an npc that fought the wolf itself reacts once its weapon is put away
        # about the kill (another threat may have come meanwhile)
        thanks = await wait(lambda: [d for d in decided(mark, "react") if "monster_killed" in d.stimuli], 15)
        attitude = await wait(lambda: decided(mark, "attitude"), 6)
        results["rescue_reaction"] = (
            f"done ({thanks[0].choice} {thanks[0].confidence:.2f}"
            f"{f', attitude {attitude[0].attitude:.1f}' if attitude else ', no attitude answer'})"
            if thanks
            else f"failed (nothing applied; planner: {planner.snapshot()})"
        )

        def after_kill() -> tuple[float, LineStarted] | None:
            begun = next((t for t in turns if t >= t1), None)
            return next(
                (
                    (t, m)
                    for t, m in lines
                    if m.npc == npc and begun and texts.get(m.line, (0.0, ""))[0] >= begun
                ),
                None,
            )

        second = await wait(after_kill, 20)
        results["words_after_the_kill"] = (
            f"done ({second[0] - t1:.1f} s after the kill: {texts.get(second[1].line, (0, '?'))[1]})"
            if second
            else "failed (no words)"
        )
        if second:
            await asyncio.sleep(3.0)  # the rest of the turn, for the trace
    finally:
        planner.decider.decide = decide  # type: ignore[method-assign]
        if dialogue is not None:
            dialogue.event_turn = event_turn  # type: ignore[method-assign]
        link.send = send  # type: ignore[method-assign]
        if runner is not None:
            await link.send(ThreatControlCmd(hold=False))
            runner.cancel()
    for name, outcome in results.items():
        log.info("%-24s %s", name, outcome)
    for question, state in dict.fromkeys(asked):
        log.info("asked %s: %s", question, state)
    if doing:
        log.info("doing: %s", ", ".join(f"{what} {t - doing[0][0]:.1f} s" for t, what in doing))
    return results


async def orders_scenario(
    link: GameLink, dialogue: DialogueManager, talk: TalkDesk, npc: str = "NONE_100_XARDAS"
) -> dict[str, str]:
    """Orders to a companion on the game (orders.py): the hero's words go in through the bridge's input
    line with his own focus (the attack with a set focus, through the server), the body obeys at once,
    no model is asked. Each check notes how long from the words to the game's answer for the first
    action ("in N ms")."""
    from contextlib import asynccontextmanager

    from ..executor import Intent

    await link.wait_world()
    results: dict[str, str] = {}
    calls: list[tuple[float, float, str, str]] = []  # (sent, answered, action, status)
    act = link.act

    async def recorded(who: str, action: Any, /, **kw: Any) -> Any:
        t = time.monotonic()
        res = await act(who, action, **kw)
        calls.append((t, time.monotonic(), action, res.status))
        return res

    link.act = recorded  # type: ignore[method-assign]
    llm = FakeBackend("Это не приказ, а разговор.", delay=0)
    dialogue.llm = llm
    if not await _until(lambda: npc in talk.people, 20):
        link.act = act  # type: ignore[method-assign]
        return {"roster": f"failed ({npc} is not around the hero)"}
    initial = await link.snapshot(npc, fresh=True)
    home = initial.npc.waypoint or initial.npc.nearest_waypoint
    # The open southeast corridor used by m5b keeps the mage's shots away from the
    # road's slopes and trees. Finish the tower state before placing an ordinary idle.
    arena = "NW_XARDAS_TOWER_VALLEY_09"
    await link.debug("player_hp", hp=5000)
    await link.debug("player_goto", pos=[23472, 1032, -10816])
    await link.debug("skip_important", near=npc)
    await link.debug("npc_goto", npc=npc, waypoint=arena, face="PC_HERO")

    async def clear_animals() -> None:
        for other in (await link.snapshot(npc, fresh=True)).nearby_npcs:
            if not other.dead and other.guild >= BEASTS:
                await link.debug("kill", instance=other.id, near=npc)
        for view in await link.npc_view([npc]):
            for seen in view.sees:
                if seen.guild >= BEASTS and seen.body != "dead":
                    await link.debug("kill", instance=seen.id, near=npc)

    for attempt in range(3):
        # A newly placed native pose can see a rat hidden from the old reading pose.
        # Only this setup is retried; no order or combat assertion has started yet.
        await clear_animals()
        await attach_when_free(link, npc)
        await link.act(npc, "return_to_routine")
        ready = await Bodies(link).act(
            npc, "start_activity", state="ZS_STAND_ARMSCROSSED", waypoint=arena,
            minutes=60, place=True, arrive=True, radius=100, timeout=15,
        )
        if ready.status == "arrived":
            break
        state = (await link.routine(npc)).current
        snap = await link.snapshot(npc, fresh=True)
        reason = (
            f"{ready.status} {ready.detail}; state={state}; "
            f"pos={snap.npc.pos}; nearby={snap.nearby_npcs}"
        )
        log.info("orders arena preparation reset %d/3: %s", attempt + 1, reason)
        if state != "ZS_ATTACK" or attempt == 2:
            raise GameError(f"orders arena preparation: {reason}")
    await clear_animals()
    if not await talk.hold(npc, "follow"):
        link.act = act  # type: ignore[method-assign]
        return {"roster": f"failed (cannot hold {npc} for the arena setup)"}
    await link.act(npc, "turn_to", npc="PLAYER")
    await link.debug("teleport", near=npc, distance=900)
    ready = await _until(
        lambda: npc in talk.people
        and 750 <= talk.people[npc].distance <= 1150
        and talk.people[npc].earshot <= 1200,
        10,
    )
    snap = await link.snapshot(npc, fresh=True)
    level = snap.player is not None and abs(snap.player.pos[1] - snap.npc.pos[1]) <= 200
    if not ready or not level:
        link.act = act  # type: ignore[method-assign]
        return {"roster": f"failed (open arena setup: {talk.people.get(npc)}, same floor {level})"}
    dialogue.open(npc, "Ксардас").intent = Intent("follow_hero", "PLAYER", "friendly")

    companion = "BAU_4300_ADDON_CAVALORN"
    watched = (npc, "PC_HERO", "WOLF", companion)
    symbols = await link.story_eval(npc, [["sym", who] for who in watched])
    names = dict(zip(symbols, (npc, "PLAYER", "WOLF", companion), strict=True))

    async def diagnose(stage: str, who: str = npc) -> None:
        try:
            snap = await link.snapshot(who, fresh=True)
            views = await link.npc_view([who])
            log.info(
                "orders snapshot stage=%s npc=%s pos=%s hp=%d/%d busy=%s action=%s "
                "hero=%s roster=%s views=%s",
                stage, who, snap.npc.pos, snap.npc.hp, snap.npc.hp_max,
                snap.npc.busy, snap.npc.action,
                snap.player.model_dump() if snap.player else None,
                talk.people.get(who), [view.model_dump() for view in views],
            )
        except (GameError, TimeoutError) as err:
            log.info("orders snapshot stage=%s failed: %s", stage, err)

    @asynccontextmanager
    async def observe(stage: str, actors: list[str]) -> AsyncIterator[None]:
        last: dict[str, tuple[int, int, int]] = {}

        def body(packet: dict[str, Any]) -> None:
            for row in packet["observations"]:
                who = names.get(row["script_symbol"], str(row["script_symbol"]))
                own, target = row["self"], row["target"]
                now, hp, foe = row["t_game_ms"], own["hp"], target["entity_id"]
                before = last.get(who)
                if before is not None and now - before[0] < 1000 and (hp, foe) == before[1:]:
                    continue
                last[who] = (now, hp, foe)
                log.info(
                    "orders body stage=%s npc=%s game_ms=%d %s",
                    stage, who, now,
                    {"entity": row["entity_id"], "pos": own["position_world_cm"],
                     "velocity": own["linear_velocity_cm_s"], "hp": hp,
                     "weapon": own["weapon_state"], "body": own["body_state_base"],
                     "attack": own["attack_anim"], "gates": own["gates"],
                     "target": foe, "target_pos": target["position_world_cm"],
                     "target_hp": target["hp"], "target_visible": target["visible"]},
                )

        link.raw_body_listeners.append(body)
        try:
            await link.body_watch(actors, rate_hz=10)
            yield
        finally:
            link.raw_body_listeners.remove(body)
            try:
                await link.body_watch([])
            except (GameError, TimeoutError) as err:
                log.info("orders body watch cleanup failed: %s", err)

    def dist() -> float:
        return talk.people[npc].distance / 100 if npc in talk.people else -1.0

    given: set[str] = set()

    async def order(
        text: str, action: str, focus: str | None = None, timeout: float = 15
    ) -> tuple[bool, str]:
        await diagnose(f"before_{action}")
        # WOLF is a shared instance symbol; its position/HP arrive through Xardas's
        # actual target, while body_watch requires each actor's symbol to be unique.
        async with observe(action, [npc, "PLAYER"]):
            if action == "fight" and talk.orders is not None and npc in talk.orders.fights:
                raise GameError("orders attack preparation: automatic defence started before the order")
            t0 = time.monotonic()
            given.add(text)
            if focus is None:
                await link.debug("type", text=text)
            else:
                talk.hear(text, focus, {"via": "scenario"})
            ok = await _until(lambda: any(a == action and t >= t0 for t, _, a, _ in calls), timeout)
            first = next(((t1, a, st) for t, t1, a, st in calls if t >= t0), None)
            took = f"{(first[0] - t0) * 1000:.0f} ms to {first[1]} {first[2]}" if first else "no action"
            done = ok and all(st == "done" for t, _, a, st in calls if t >= t0 and a == action)
            if not done:
                await diagnose(f"failed_{action}")
            return done, took

    ok, took = await order("Ксардас, ко мне!", "follow")
    came = await _until(lambda: 0 <= dist() <= 5, 20)
    results["come"] = f"{'done' if ok and came else 'failed'} (in {took}; {dist():.1f} m from the hero)"
    if not ok or not came:
        await diagnose("come_failed")

    ok, took = await order("Стой!", "stop")
    await link.debug("teleport", near=npc, distance=900)
    await asyncio.sleep(4)
    stood = dist()
    results["halt"] = f"{'done' if ok and stood >= 7.5 else 'failed'} (in {took}; stays {stood:.1f} m behind)"
    if not ok or stood < 7.5:
        await diagnose("halt_failed")

    ok, took = await order("За мной!", "follow")
    closer = await _until(lambda: 0 <= dist() <= stood - 2, 15)
    results["follow"] = f"{'done' if ok and closer else 'failed'} (in {took}; {stood:.1f} -> {dist():.1f} m)"
    if not ok or not closer:
        await diagnose("follow_failed")

    ok, took = await order("К оружию!", "draw_weapon")
    results["draw"] = f"{'done' if ok else 'failed'} (in {took})"

    # The follow check is over. Keep the passive hero behind and to the side of
    # the mage, away from the projectile lane. Its end point is on a slope, so
    # stage this separate combat check on the pasture's tested flat ground.
    await link.act(npc, "stop")
    await link.act(npc, "return_to_routine")
    await _until(lambda: npc not in talk.attached, 5)
    attack_arena = "NW_FARM1_FIELD_06"
    await link.debug("player_goto", waypoint=attack_arena)
    await link.debug("npc_goto", npc=npc, waypoint=attack_arena)
    await clear_animals()
    if not await talk.hold(npc, "follow"):
        raise GameError("orders attack preparation: cannot hold the mage")
    await link.act(npc, "stop")
    await asyncio.sleep(0.5)  # settle on npc_goto's native ground before choosing the lane
    dialogue.open(npc, "Ксардас").intent = Intent("follow_hero", "PLAYER", "friendly")
    await diagnose("attack_arena_placed")
    origin = (await link.snapshot(npc, fresh=True)).npc.pos
    wolf, ray = None, {}
    for step in range(8):
        await clear_animals()
        angle = step * math.pi / 4
        dx, dz = math.cos(angle), math.sin(angle)
        await link.debug("player_goto", pos=[origin[0] + 500 * dx, origin[1], origin[2] + 500 * dz])
        await link.act(npc, "turn_to", npc="PLAYER")
        # debug spawn independently tests support and the observer's native ray.
        try:
            await link.debug("spawn", instance="WOLF", near=npc, distance=1400)
        except GameError as err:
            if not str(err).startswith("no visible supported ground"):
                raise
            log.info("orders attack preparation lane=%d: %s", step, err)
            continue
        await link.debug(
            "player_goto",
            pos=[origin[0] - 400 * dx - 500 * dz, origin[1], origin[2] - 400 * dz + 500 * dx],
        )
        await link.act(npc, "turn_to", npc="WOLF")
        await asyncio.sleep(1)
        views = await link.npc_view([npc])
        wolf = next(
            (other for view in views for other in view.sees
             if other.id == "WOLF" and other.kind == "monster" and other.body != "dead"),
            None,
        )
        response = await link._request(
            DebugCmd(req=link._next_id("r"), cmd="npc_los", npc=npc, to="WOLF"), 5,
        )
        ray = response.value or {}
        automatic = talk.orders is not None and npc in talk.orders.fights
        log.info(
            "orders attack preparation lane=%d: wolf=%s native_ray=%s automatic=%s",
            step, wolf, ray, automatic,
        )
        if automatic:
            raise GameError("orders attack preparation: automatic defence before the order")
        if (
            wolf is not None and wolf.distance > 1000 and wolf.hero_cm is not None and wolf.hero_cm > 1500
            and (ray.get("center_clear") or ray.get("head_clear"))
        ):
            break
    else:
        raise GameError(f"orders attack preparation: no clear lane; wolf={wolf}; ray={ray}")
    ok, took = await order("В атаку!", "fight", focus="WOLF", timeout=60)
    over = await _until(lambda: any(a == "fight" for _, _, a, _ in calls) and calls[-1][2] == "follow", 60)
    fight = next((st for _, _, a, st in reversed(calls) if a == "fight"), "-")
    results["attack"] = (
        f"{'done' if ok and over and fight == 'done' else 'failed'} "
        f"(in {took}; fight {fight}, follows after {over})"
    )
    if not ok or not over or fight != "done":
        await diagnose("attack_failed")

    ok, took = await order("Убери оружие.", "remove_weapon")
    results["sheathe"] = f"{'done' if ok else 'failed'} (in {took})"

    ok, took = await order("Свободен.", "stop")
    free = await _until(lambda: npc not in talk.attached, 10)
    goal = dialogue.conversations[npc].intent.goal
    results["dismiss"] = (
        f"{'done' if ok and free and goal == 'keep_routine' else 'failed'} "
        f"(in {took}; detached {free}, goal {goal})"
    )

    # Combat incidents may speak on their own. Only the hero's orders must skip a dialogue turn.
    order_calls = sum(1 for _, messages, _ in llm.calls if messages and messages[-1].content in given)
    results["no_model"] = (
        f"{'done' if not order_calls else 'failed'} "
        f"({order_calls} order model call(s), {len(llm.calls) - order_calls} incident call(s))"
    )
    link.act = act  # type: ignore[method-assign]
    if home is not None:
        await link.debug("npc_goto", npc=npc, waypoint=home)
    async with observe("companion_attack_words", [companion, "PLAYER"]):
        await diagnose("companion_attack_words_before", companion)
        attack_words = await _companion_attack_words(link, dialogue, talk)
        if any(not result.startswith("done") for result in attack_words.values()):
            await diagnose("companion_attack_words_failed", companion)
        results.update(attack_words)
    for name, outcome in results.items():
        log.info("%-12s %s", name, outcome)
    return results


async def _companion_attack_words(
    link: GameLink, dialogue: DialogueManager, talk: TalkDesk, npc: str = "BAU_4300_ADDON_CAVALORN"
) -> dict[str, str]:
    """2026-10-18: «бей волка» without focus belongs to Cavalorn, before a wolf bites the hero.

    Retreat during setup keeps an automatic monster defence from satisfying the attack check. The
    explicit attack must replace it; the wolf's tone model and judge must never receive the words.
    """
    from ..executor import Intent

    beasts = talk.beasts
    if beasts is None or talk.orders is None:
        return {"wolf_ready": "failed (no beasts or companion orders)"}
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object) -> None:
        results[name] = f"{'done' if ok else 'failed'} ({detail})"

    def dist() -> float:
        return talk.people[npc].distance / 100 if npc in talk.people else -1.0

    await link.debug("player_hp", hp=5000)
    await link.debug("skip_important", near=npc)
    for _ in range(2):
        try:
            await link.debug("kill", instance="YGOBBO_GREEN", near=npc, by=npc)
        except GameError:
            pass
    arena = "NW_FARM1_FIELD_06"
    await link.debug("player_goto", waypoint=arena)
    await asyncio.sleep(1)  # the arena's roster, rather than the one still at Cavalorn's fire
    for _ in range(8):  # clear the arena's own wolves before its one controlled encounter
        if not beasts.wolves:
            break
        wolf = min(beasts.wolves.values(), key=lambda w: w.distance)
        await link.debug("kill", instance=wolf.id.split("#", 1)[0], by="PLAYER")
        await _until(lambda wolf=wolf: wolf.id not in beasts.wolves, 5)
    # Stage the free NPC first: teleporting the hero from the fire after follow starts ends that order
    # with follow_ended (the hero went too far). Its first real follow now covers only two metres.
    await link.debug("player_goto", waypoint=arena)
    await link.debug("npc_goto", npc=npc, waypoint=arena, face="PC_HERO")
    if not await talk.hold(npc, "follow"):
        return {"wolf_ready": f"failed (cannot hold {npc} for the arena setup)"}
    await link.act(npc, "turn_to", npc="PLAYER")
    await link.debug("teleport", near=npc, distance=700)
    if not await _until(
        lambda: npc in talk.people
        and 500 <= talk.people[npc].distance <= 900
        and talk.people[npc].earshot <= 1000,
        15,
    ):
        return {"wolf_ready": f"failed (Cavalorn {dist():.1f} m from the hero)"}
    dialogue.open(npc, "Кавалорн").intent = Intent("follow_hero", "PLAYER", "friendly")
    talk.hear("Кавалорн, за мной!", None, {"via": "scenario"})
    following = await _until(
        lambda: 0 <= dist() <= 5
        and npc in talk.attached
        and getattr(talk.orders.standing.get(npc), "kind", "") == "follow",
        20,
    )
    check("wolf_follow", following, f"Cavalorn {dist():.1f} m from the hero")
    if not following:
        return results

    snap = await link.snapshot(npc, fresh=True)
    if snap.player is None:
        return {**results, "wolf_ready": "failed (no hero in Cavalorn's snapshot)"}
    await link.act(npc, "stop")
    origin = (await link.snapshot(npc, fresh=True)).npc.pos
    # Native waypoints provide ground support and preserve the tagged death used below.
    # debug spawn checks support too, but story_tag("WOLF") would tag an older instance.
    lanes: dict[int, tuple[float, dict[str, Any]]] = {}
    for point in await link.waypoints():
        dx, dz = point["x"] - origin[0], point["z"] - origin[2]
        distance = math.hypot(dx, dz)
        if point["fp"] or not 1300 <= distance <= 1600 or abs(point["y"] - origin[1]) > 100:
            continue
        lane = round(math.atan2(dz, dx) / (math.pi / 4)) % 8
        score = abs(distance - 1400)
        if lane not in lanes or score < lanes[lane][0]:
            lanes[lane] = (score, point)
    wolf, ray, wolf_id = None, {}, ""
    hp = snap.player.hp
    for lane, (_, point) in sorted(lanes.items()):
        dx, dz = point["x"] - origin[0], point["z"] - origin[2]
        distance = math.hypot(dx, dz)
        dx, dz = dx / distance, dz / distance
        await link.debug(
            "player_goto", pos=[origin[0] + 500 * dx, origin[1], origin[2] + 500 * dz],
        )
        # Refresh retreat on every attempt; its automatic-defence suppression expires.
        standing = talk.orders.standing.get(npc)
        talk.hear("Кавалорн, отступай!", None, {"via": "scenario"})
        if not await _until(
            lambda standing=standing: talk.orders.standing.get(npc) is not standing
            and getattr(talk.orders.standing.get(npc), "kind", "") == "retreat", 5,
        ):
            raise GameError("orders Cavalorn attack preparation: cannot retreat")
        if not await _until_async(
            lambda: link.snapshot(npc, fresh=True), lambda snap: snap.npc.following, 5,
        ):
            raise GameError("orders Cavalorn attack preparation: retreat did not start following")
        await link.act(npc, "stop")
        await link.act(npc, "turn_to", npc="PLAYER")
        await asyncio.sleep(1)  # let the beast roster catch up with the hero's new position
        before = set(beasts.wolves)
        await link.story_spawn("WOLF", tag="companion_attack_words", waypoint=point["name"])
        await _until(
            lambda before=before: any(w.id not in before and w.id.startswith("WOLF#")
                                     and w.distance < 1300 for w in beasts.wolves.values()), 5,
        )
        wolf_id = next(
            (w.id for w in beasts.wolves.values()
             if w.id not in before and w.id.startswith("WOLF#") and w.distance < 1300), "",
        )
        ready = await _until(
            lambda wolf_id=wolf_id: wolf_id in beasts.wolves
            and beasts.wolves[wolf_id].state == "threaten", 15,
        )
        # Only a fresh wolf that has growled may reach the actual attack checks.
        await link.debug(
            "player_goto",
            pos=[origin[0] - 400 * dx - 500 * dz, origin[1], origin[2] - 400 * dz + 500 * dx],
        )
        await link.act(npc, "turn_to", npc="WOLF")
        await asyncio.sleep(1)
        views = await link.npc_view([npc])
        wolf = next(
            (other for view in views for other in view.sees
             if other.id == "WOLF" and other.kind == "monster" and other.body != "dead"),
            None,
        )
        response = await link._request(
            DebugCmd(req=link._next_id("r"), cmd="npc_los", npc=npc, to="WOLF"), 5,
        )
        ray = response.value or {}
        automatic = npc in talk.orders.fights
        snap = await link.snapshot(npc, fresh=True)
        log.info(
            "orders Cavalorn attack preparation lane=%d waypoint=%s wolf_id=%s wolf_spawn_pos=%s "
            "Cavalorn_pos=%s ready=%s wolf=%s native_ray=%s automatic=%s",
            lane, point["name"], wolf_id, [point["x"], point["y"], point["z"]],
            snap.npc.pos, ready, wolf, ray, automatic,
        )
        if automatic:
            raise GameError("orders Cavalorn attack preparation: automatic defence before the order")
        if (
            ready and wolf is not None and wolf.distance > 1000
            and wolf.hero_cm is not None and wolf.hero_cm > 1500
            and (ray.get("center_clear") or ray.get("head_clear"))
        ):
            check("wolf_ready", True, f"{wolf_id} spawned at {point['name']}, before its first attack")
            break
        await link.debug("kill", instance="WOLF", near=npc, by="PLAYER")
        await _until(lambda wolf_id=wolf_id: wolf_id not in beasts.wolves, 5)
    else:
        raise GameError(f"orders Cavalorn attack preparation: no clear lane; wolf={wolf}; ray={ray}")

    actions: list[tuple[float, str, str]] = []  # dispatch time, target, wolf state at dispatch
    finished: list[str] = []
    states: list[tuple[float, str]] = []
    killed: list[str] = []
    judged: list[str] = []
    heard: list[str] = []
    act, judge, hears, quick = link.act, beasts.judge, beasts.hears, beasts.quick
    tone_model = FakeBackend("hostile", delay=0)
    beasts.quick = tone_model

    async def record_act(who: str, action: Any, /, **kw: Any) -> Any:
        mine = who.upper() == npc and action == "fight"
        if mine:
            w = beasts.wolves.get(wolf_id)
            actions.append((time.monotonic(), str(kw.get("npc", "")), w.state if w else "gone"))
        result = await act(who, action, **kw)
        if mine:
            finished.append(result.status)
        return result

    async def record_judge(text: str, hearers: Any) -> Any:
        judged.append(text)
        return await judge(text, hearers)

    def record_hears(text: str, *args: Any) -> Any:
        heard.append(text)
        return hears(text, *args)

    def on_beasts(msg: GameMessage) -> None:
        if isinstance(msg, Beasts):
            states.extend((time.monotonic(), w.state) for w in msg.wolves if w.id == wolf_id)
        elif isinstance(msg, NpcDied) and msg.tag == "companion_attack_words":
            killed.append(msg.killer.upper())

    link.act = record_act  # type: ignore[method-assign]
    beasts.judge = record_judge  # type: ignore[method-assign]
    beasts.hears = record_hears  # type: ignore[method-assign]
    link.listeners.append(on_beasts)
    try:
        mark = time.monotonic()
        talk.hear("Бей волка!", None, {"via": "scenario"})
        dispatched = await _until(lambda: bool(actions), 10)
        # A weak wolf can die to Cavalorn's first blow between the 500 ms beast reports. Its tagged
        # death names the actual killer, so that is also proof of the companion's real fight.
        fought = await _until(lambda: any(state == "fight" for _, state in states) or npc in killed, 15)
        to = talk.last[0] if talk.last else None
        check(
            "attack_without_focus",
            dispatched
            and fought
            and to is not None
            and to.id == npc
            and to.why == "order"
            and actions[0][1].split("#", 1)[0] == wolf_id.split("#", 1)[0],
            f"to {to}; fights {actions}, wolf fought Cavalorn {fought}, killers {killed}",
        )
        before_bite = (
            dispatched
            and actions[0][2] != "attack"
            and not any(state == "attack" and t <= actions[0][0] for t, state in states)
        )
        snap = await link.snapshot(npc, fresh=True)
        after_hp = snap.player.hp if snap.player is not None else None
        check(
            "attack_before_bite",
            before_bite and hp is not None and after_hp == hp,
            f"dispatch in {actions[0][0] - mark:.2f} s, hp {hp} -> {after_hp}"
            if actions
            else f"no fight, hp {hp} -> {after_hp}",
        )
        check(
            "wolf_unheard",
            not heard and not judged and not tone_model.calls,
            f"hears {len(heard)}, judge {len(judged)}, tone model {len(tone_model.calls)}",
        )
        await _until(lambda: bool(finished), 60)
    finally:
        link.act = act  # type: ignore[method-assign]
        beasts.judge = judge  # type: ignore[method-assign]
        beasts.hears = hears  # type: ignore[method-assign]
        beasts.quick = quick
        link.listeners.remove(on_beasts)
    return results


class _Rolls:
    """The dice of a scenario: the rolls it is to come up with, in turn (0.5 once they run out)."""

    def __init__(self) -> None:
        self.queue: list[float] = []

    def random(self) -> float:
        return self.queue.pop(0) if self.queue else 0.5


WOLF_HERO = "NW_XARDAS_PATH_FARM1_05"  # on the road from Xardas's tower to the farm


async def wolf_scenario(link: GameLink, talk: TalkDesk, beasts: BeastDesk) -> dict[str, str]:
    """Wolves hear the hero (docs/beasts.md) on the road below Xardas's tower. A wolf growls at him; once
    he starts to speak it waits for his words past its four seconds; soothed, it lets him pass: sits down,
    watches him go by, lets him be once he has gone and come back. Then a wolf each: a hostile phrase
    (it attacks), «Стой!» with no one looked at (the wolf's, not a companion order) and then walking right
    up to it (it bites), a threat (it runs), a soothing that fails (it attacks). The dice are the
    scenario's; the tone is the quick model's with QUICK_URL, the rules' else."""
    await link.wait_world()
    results: dict[str, str] = {}
    rng, rolls = beasts.rng, _Rolls()
    beasts.rng = rolls  # type: ignore[assignment]

    def check(name: str, ok: bool, detail: str) -> None:
        results[name] = f"{'done' if ok else 'failed'} ({detail})"
        log.info("%-12s %s", name, results[name])

    def state(wolf: str | None) -> str:
        e = beasts.wolves.get(wolf or "")
        return e.state if e is not None else "gone"

    def dist(wolf: str | None) -> float:
        e = beasts.wolves.get(wolf or "")
        return round(e.distance / 100, 1) if e is not None else -1.0

    async def hero_hp() -> int:
        snap = await link.snapshot("NONE_100_XARDAS", fresh=True)
        return snap.player.hp or 0 if snap.player is not None else 0

    def cm(a: dict[str, Any], b: dict[str, Any]) -> float:
        return float(((a["x"] - b["x"]) ** 2 + (a["z"] - b["z"]) ** 2) ** 0.5)

    points = {p["name"]: p for p in await link.waypoints()}
    hero = points[WOLF_HERO]
    ground = [p for p in points.values() if abs(p["y"] - hero["y"]) < 250 and not p["fp"]]
    at = min(ground, key=lambda p: abs(cm(p, hero) - 1000))  # ~10 m off: it growls at him
    # beyond the wolf and 4.5 m aside of it (he walks by); back behind him, 19 m from it (gone, still listed)
    dx, dz = (at["x"] - hero["x"]) / cm(at, hero), (at["z"] - hero["z"]) / cm(at, hero)
    past = (at["x"] + dx * 300 - dz * 450, at["y"], at["z"] + dz * 300 + dx * 450)
    away = (hero["x"] - dx * 1000, hero["y"], hero["z"] - dz * 1000)
    log.info("wolf at %s, %.0f m from the hero", at["name"], cm(at, hero) / 100)

    async def kill(wolf: str | None) -> None:
        if wolf in beasts.wolves:
            await link.debug("kill", instance=wolf.split("#", 1)[0], by="PLAYER")  # the nearest of its kind
            await _until(lambda: wolf not in beasts.wolves, 5)

    async def clear() -> list[str]:
        """No wolf about but the scenario's (the road's own young wolf, a pack)."""
        killed = []
        for _ in range(8):
            if not beasts.wolves:
                break
            wolf = min(beasts.wolves.values(), key=lambda w: w.distance).id
            try:
                await kill(wolf)
            except GameError as err:
                log.info("could not kill %s: %s", wolf, err)
                break
            killed.append(wolf)
        return killed

    async def wolf_at_him(n: int) -> str | None:
        """A wolf at its point, the hero at his; the wolf, once it growls at him."""
        await link.debug("player_goto", waypoint=WOLF_HERO)
        await asyncio.sleep(1)
        await clear()
        before = set(beasts.wolves)
        await link.story_spawn("WOLF", tag=f"beast_{n}", waypoint=at["name"])
        await _until(lambda: any(w not in before for w in beasts.wolves), 10)
        wolf = next((w for w in beasts.wolves if w not in before), None)
        await _until(lambda: state(wolf) == "threaten", 15)
        return wolf if state(wolf) == "threaten" else None

    def said(text: str, focus: str | None = "WOLF") -> float:
        talk.hear(text, focus, {"via": "scenario"})
        return time.monotonic()

    def verdict() -> str:
        v, t = (beasts.verdicts[0] if beasts.verdicts else None), beasts.last_tone
        if v is None or t is None:
            return "no verdict"
        return f"{v.outcome}, chance {v.chance:.2f}, tone {t[0]} by {t[1]} in {t[2]} ms"

    await link.debug("player_hp", hp=5000)  # the bites of the failing tries do not kill him
    await link.debug("player_goto", waypoint=WOLF_HERO)
    await asyncio.sleep(2)
    log.info("cleared: %s", await clear() or "nobody about")

    # 1. a wolf growls at him, waits for his words, lets him pass and be
    wolf = await wolf_at_him(1)
    check("threatens", wolf is not None, f"{wolf} {state(wolf)} at {dist(wolf)} m")
    listening = beasts.listen()  # as the talk key does
    listens = await _until(lambda: state(wolf) == "listen", 3)
    hp = await hero_hp()
    await asyncio.sleep(6)  # past its four seconds of growling
    still = state(wolf)
    check(
        "listens",
        listens and still == "listen",
        f"{listening}: {still} after 6 s, hp {hp} -> {await hero_hp()}",
    )
    rolls.queue = [0.0]
    t0 = said("Тихо, тихо, хороший мальчик. Я просто мимо пройду.")
    calm = await _until(lambda: state(wolf) == "calm", 5)
    to = talk.last[0] if talk.last else None
    check(
        "calm",
        calm and to is not None and to.why == "beast",
        f"{verdict()}, in {time.monotonic() - t0:.1f} s, said to {to.id if to else None}",
    )
    sits = await _until(lambda: (e := beasts.wolves.get(wolf or "")) is not None and e.sitting, 10)
    check("sits", sits, f"{state(wolf)}, sitting {sits}, hp {await hero_hp()}")
    await link.debug("player_goto", pos=past)
    await asyncio.sleep(4)
    check("passes", state(wolf) == "calm" and await hero_hp() == hp, f"{state(wolf)} at {dist(wolf)} m")
    await link.debug("player_goto", pos=away)
    over = await _until(lambda: state(wolf) == "routine", 5)
    await link.debug("player_goto", waypoint=WOLF_HERO)
    await asyncio.sleep(8)
    e = beasts.wolves.get(wolf or "")
    check(
        "lets_be",
        over and e is not None and e.state == "routine" and e.tolerant and await hero_hp() == hp,
        f"{state(wolf)} at {dist(wolf)} m, tolerant {e.tolerant if e else None}",
    )
    await kill(wolf)

    # 2. hostility: it attacks at once, no dice
    wolf = await wolf_at_him(2)
    t0 = said("Иди сюда, тварь, я тебя прикончу!")
    went = await _until(lambda: state(wolf) == "attack", 4)
    check(
        "hostile", wolf is not None and went, f"{verdict()}, {state(wolf)} in {time.monotonic() - t0:.1f} s"
    )
    await kill(wolf)

    # 3. «Стой!» looking at no one is the wolf's; let pass, then walked right up to, it bites
    wolf = await wolf_at_him(3)
    rolls.queue = [0.0]
    said("Стой!", None)
    calm = await _until(lambda: state(wolf) == "calm", 5)
    to = talk.last[0] if talk.last else None
    check(
        "halt_word", calm and to is not None and to.why == "beast", f"{verdict()}, to {to.id if to else None}"
    )
    await asyncio.sleep(1)
    await link.debug("player_goto", waypoint=at["name"])
    bit = await _until(lambda: state(wolf) == "attack", 4)
    check("too_close", bit, f"{state(wolf)} at {dist(wolf)} m")
    await kill(wolf)

    # 4. a threat scares it off
    wolf = await wolf_at_him(4)
    rolls.queue = [0.0]
    said("Пошёл вон! Брысь!")
    ran = await _until(lambda: state(wolf) == "flee", 4)
    d0 = dist(wolf)
    await asyncio.sleep(3)
    check(
        "scared",
        ran and (dist(wolf) > d0 or wolf not in beasts.wolves),
        f"{verdict()}, {d0} -> {dist(wolf)} m",
    )
    await kill(wolf)

    # 5. a soothing the dice say no to: it did not believe him
    wolf = await wolf_at_him(5)
    rolls.queue = [0.99]
    said("Тихо, тихо, хороший. Я тебя не трону.")
    went = await _until(lambda: state(wolf) == "attack", 4)
    check("disbelief", wolf is not None and went, f"{verdict()}, {state(wolf)}")

    # 6. the author's case: it bites him already, he talks it down; it steps back and lets him be
    bites = await _until(lambda: state(wolf) == "attack" and dist(wolf) < 3, 10)
    rolls.queue = [0.0]
    said("Стой! Хороший мальчик, я тут мимо пройду.")
    calm = await _until(lambda: state(wolf) == "calm", 4)
    back = await _until(lambda: dist(wolf) >= 4, 5)
    hp = await hero_hp()
    await asyncio.sleep(4)
    check(
        "calm_bite",
        bites and calm and back and state(wolf) == "calm" and await hero_hp() == hp,
        f"{verdict()}, {state(wolf)} at {dist(wolf)} m, hp {hp} -> {await hero_hp()}",
    )
    await kill(wolf)

    beasts.rng = rng
    return results


async def companion_scenario(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    npc: str = "BAU_4300_ADDON_CAVALORN",
    bandit: str = "BDT_1013_BANDIT_L",
    cave: tuple[str, ...] = ("BDT_1014_BANDIT_L", "BDT_1015_BANDIT_L"),
    perception: PerceptionDesk | None = None,
) -> dict[str, str]:
    """The hero's companion on the game (orders.py), as the user played it on 2026-10-13: Cavalorn walked
    after a running hero, watched a bandit beat him and only talked, and did not understand "go into the
    cave"; the bandits below Xardas's tower ran their scripted ambush. Checks:
    - pace: a companion left behind runs to the hero (it walked within 12 m before);
    - freed: the bandit at the steps does not start his scripted "Where are you going?" (story.FREED);
    - go_to: told to go into the cave first, it walks into the bandits' cave and waits there; the freed
      bandits in it do not fall on it;
    - attack: the bandit's own model decides to attack the hero (scripted): the scripts' attack;
    - defend: the companion, told «Погоди.» 20 m off (an order now: 2026-10-14 the model took it for «I
      no longer need you», and Cavalorn stood by while the bandits beat the hero 20 m from him), fights
      him at once, with no order, and follows the hero again after;
    - party: it is the scripts' party member while it goes with the hero, so that
    - xp: the bandit it beats gives the hero the experience (ZS_Dead, ZS_Unconscious);
    - sees_cave, focus (with `perception`, step M1 of docs/npc-tools.md): in the cave, having looked around,
      it sees a bandit there (the other may be behind the cave's bend), with its id for its intent; whom the
      hero faces is his focus in its view, and a target.
    The models are scripted (LLM=fake); the hero wears heavy armor, so that he lives through the beating."""
    from ..executor import Intent

    await link.wait_world()
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    calls: list[tuple[float, str, str, dict[str, Any]]] = []  # (sent, action, status, args)
    act = link.act

    async def recorded(who: str, action: Any, /, **kw: Any) -> Any:
        t = time.monotonic()
        res = await act(who, action, **kw)
        if who.upper() == npc:
            calls.append((t, action, res.status, kw))
        return res

    link.act = recorded  # type: ignore[method-assign]
    chosen: list[DialogChosen] = []
    link.listeners.append(lambda m: chosen.append(m) if isinstance(m, DialogChosen) else None)
    entries: list[dict[str, Any]] = []
    sink = dialogue.journal.sink

    def keep(entry: dict[str, Any]) -> None:
        entries.append(entry)
        if sink is not None:
            sink(entry)

    dialogue.journal.sink = keep
    fight = {"goal": "fight", "target": "PLAYER", "attitude": "hostile", "agenda": None}
    go = ToolCall("go_to", {"place": "XARDAS_BANDITS"})
    dialogue.llm = _ByNpc(
        {
            "Кавалорн": _OnWords(
                [
                    ("пещеру первым", FakeStep("Пойду первым.", [go])),
                    ("пришёл первым", FakeStep("Тут двое.")),
                ],
                FakeStep("Держись, я с тобой!"),
            ),
            "Бандит": FakeBackend(
                script=[FakeStep("Ну всё, сам напросился!", [ToolCall("set_intent", fight)])], delay=0
            ),
        },
        FakeBackend("Хм.", delay=0),
    )

    def dist(who: str = npc) -> float:
        return talk.people[who].distance / 100 if who in talk.people else -1.0

    def since(t0: float, action: str) -> list[tuple[float, str, str, dict[str, Any]]]:
        return [c for c in calls if c[0] >= t0 and c[1] == action]

    try:
        await link.debug("player_equip", item="ITAR_PAL_H")
    except GameError as err:
        log.info("companion: no armor for the hero: %s", err)
    await link.debug("skip_important", near=npc)  # Cavalorn's own greeting would stop the hero
    for _ in range(2):  # the goblins by his fire: they would beat the hero down before anything is checked
        try:
            await link.debug("kill", instance="YGOBBO_GREEN", near=npc, by=npc)
        except GameError as err:
            log.info("companion: goblin: %s", err)
    await link.debug("player_goto", to=npc, distance=300)
    if not await _until(lambda: 0 <= dist() <= 15, 20):
        link.act = act  # type: ignore[method-assign]
        return _logged({"roster": f"failed ({npc} is not around the hero: {dist():.1f} m)"})
    dialogue.open(npc, "Кавалорн").intent = Intent("follow_hero", "PLAYER", "friendly")
    talk.hear("Кавалорн, за мной!", None, {"via": "scenario"})
    following = await _until(lambda: since(0, "follow") and 0 <= dist() <= 5, 20)
    check("follow", following, f"{dist():.1f} m")

    # pace: the hero is suddenly 11+ m off; a run covers it in about 2 s, a walk in 5 or more
    await link.debug("player_goto", near=npc, distance=1100)
    t0 = time.monotonic()
    await _until(lambda: dist() >= 9, 3)
    d0 = dist()
    caught_up = await _until(lambda: 0 <= dist() <= 5, 20)
    took = time.monotonic() - t0
    speed = (d0 - 5) / took if took > 0 else 0.0
    check("pace", caught_up and speed >= 2.5, f"{d0:.1f} m -> 5 m in {took:.1f} s, {speed:.1f} m/s")

    # freed: next to the bandit at the steps, nothing starts on its own
    await link.debug("player_goto", to=bandit, distance=300)
    near = await _until(lambda: 0 <= dist(bandit) <= 6, 15)
    await asyncio.sleep(8)
    ambush = [c.node for c in chosen if c.npc.upper() == bandit]
    check("freed", near and not ambush, f"{dist(bandit):.1f} m from him, dialogs {ambush or 'none'}")

    # go_to: "into the cave first"; the bandits there let it be
    await _until(lambda: 0 <= dist() <= 6, 30)  # it has caught up from its fire: it hears its name
    t0 = time.monotonic()
    talk.hear("Кавалорн, иди в пещеру первым.", None, {"via": "scenario"})
    walked = await _until(lambda: since(t0, "go_to_waypoint"), 20)
    arrived = await _until(lambda: any(c[2] == "done" for c in since(t0, "go_to_waypoint")), 100)
    walk = since(t0, "go_to_waypoint")
    snap = await link.snapshot(npc, fresh=True)
    standing = talk.orders.standing.get(npc) if talk.orders else None
    check(
        "go_to",
        walked and arrived and standing is not None and standing.kind == "go",
        f"{walk[-1][2] if walk else 'not sent'}, at {snap.npc.waypoint or snap.npc.nearest_waypoint}",
    )
    # it looks around in there before it says what it found
    told = await _until(
        lambda: any(
            e["kind"] == "npc" and e.get("npc") == npc and "Тут" in e.get("text", "") for e in entries
        ),
        90,
    )
    snap = await link.snapshot(npc, fresh=True)
    log.info("companion: %s at %s, told %s", npc, snap.npc.waypoint or snap.npc.nearest_waypoint, told)
    if perception is not None:
        await perception.fresh(npc)
        sight = perception.sight(npc, snap)
        sees = set(sight.targets) if sight is not None else set()
        why = sight.people if sight is not None else "no view"
        if not set(cave) & sees:
            # why not: its senses range, the engine's own sight with and without the eyes' angle
            me = ["inst", npc]
            probe = [f"senses {await link.evaluate(['field', me, 'SENSES_RANGE'])}"]
            near = {o.id: o.distance for o in snap.nearby_npcs}
            for other in cave:
                them = ["inst", other]
                sees_it = await link.evaluate(["call", "NPC_CANSEENPC", me, them])
                free = await link.evaluate(["call", "NPC_CANSEENPCFREELOS", me, them])
                probe.append(f"{other} {near.get(other, -1) / 100:.1f} m: sees {sees_it}, free {free}")
            why += " | " + "; ".join(probe)
        check("sees_cave", bool(set(cave) & sees), why)
    hp = snap.npc.hp
    await asyncio.sleep(6)
    snap = await link.snapshot(npc, fresh=True)
    fought = since(t0, "fight")
    check("cave_quiet", not fought and snap.npc.hp == hp, f"hp {hp} -> {snap.npc.hp}, fights {len(fought)}")
    talk.hear("Кавалорн, за мной!", None, {"via": "scenario"})
    back = await _until(lambda: 0 <= dist() <= 6, 40)
    check("back", back, f"{dist():.1f} m")

    # the bandit's own decision; the gang in the cave would join a fight it hears: out of it for this check
    for other in cave:
        try:
            await link.debug("kill", npc=other)
        except GameError as err:
            log.info("companion: kill %s: %s", other, err)
    await link.debug("teleport", near=bandit, distance=250)  # facing him: he is the hero's focus
    await _until(lambda: 0 <= dist(bandit) <= 5 and 0 <= dist() <= 8, 15)
    if perception is not None:
        view = await perception.fresh(npc)
        focus = view.hero.focus if view is not None and view.hero is not None else None
        sight = perception.sight(npc)
        check(
            "focus",
            focus == bandit and sight is not None and bandit in sight.targets,
            f"hero looks at {focus}",
        )
    # «Погоди.» 20 m from the bandit, then the hero goes up to him alone
    hero = ["inst", "HERO"]
    party = await link.evaluate(["field", ["inst", npc], "AIVAR", 15])
    check("party", party == 1, f"AIV_PARTYMEMBER {party}")
    await link.debug("player_goto", near=bandit, distance=2000)
    await _until(lambda: 0 <= dist() <= 6 and dist(bandit) >= 15, 20)
    talk.hear("Кавалорн, погоди.", None, {"via": "scenario"})
    halted = await _until(
        lambda: talk.orders is not None and getattr(talk.orders.standing.get(npc), "kind", "") == "halt", 5
    )
    await link.debug("teleport", near=bandit, distance=250)
    await _until(lambda: 0 <= dist(bandit) <= 5 and dist() >= 12, 15)
    far = dist()
    exp = await link.evaluate(["field", hero, "EXP"])
    t0 = time.monotonic()
    talk.hear("Эй, ты! А ну отойди с дороги.", bandit, {"via": "scenario"})
    attacked = await _until(lambda: any(e["kind"] == "attack" for e in entries), 20)
    check("attack", attacked, "the scripts' attack" if attacked else "no attack")
    defended = await _until(
        lambda: any(c[3].get("npc", "").upper() == bandit for c in since(t0, "fight")), 30
    )
    started = since(t0, "fight")
    check(
        "defend",
        halted and defended and far >= 12,
        f"halted {halted} {far:.1f} m off, fights {started[0][0] - t0:.1f} s after the words"
        if started
        else f"halted {halted} {far:.1f} m off, no fight",
    )
    down = await _until(lambda: bandit not in talk.people or talk.people[bandit].activity == "down", 90)
    await asyncio.sleep(1)
    gained = await link.evaluate(["field", hero, "EXP"])
    check("xp", down and gained > exp, f"exp {exp} -> {gained}")
    after = await _until(lambda: calls[-1][1] == "follow" and 0 <= dist() <= 8, 30)
    defends = [e for e in entries if e["kind"] == "defend"]
    check("after", down and after, f"bandit down {down}, follows {after}, stood up {len(defends)} time(s)")
    link.act = act  # type: ignore[method-assign]
    return _logged(results)


async def calm_scenario(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    bandit: str = "BDT_1013_BANDIT_L",
    gang: tuple[str, ...] = ("BDT_1014_BANDIT_L", "BDT_1015_BANDIT_L"),
) -> dict[str, str]:
    """Talking an attacker down (the user, 2026-10-14: attacked by a bandit, he spoke to him and was told
    nobody heard; he wants to calm one with words). The bandit at the steps decides to attack the hero (its
    model scripted); the hero, looking at no one, says «Воу, воу, ты чего? Спокуха!». Checks:
    - attack: the scripts' attack, the bandit in ZS_ATTACK;
    - addressed: the words go to whoever is at the hero, though not in his focus;
    - calm: its model calls the fight off (scripted) and the effect `calm` goes to the game;
    - stopped: it is out of the attack state;
    - stays_calm: 15 s on it has not fallen on the hero again, and the hero has not lost health.
    The gang in the cave is out of it (a fight's noise would bring them). The hero wears heavy armor."""

    await link.wait_world()
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    entries: list[dict[str, Any]] = []
    sink = dialogue.journal.sink

    def keep(entry: dict[str, Any]) -> None:
        entries.append(entry)
        if sink is not None:
            sink(entry)

    dialogue.journal.sink = keep
    fight = {"goal": "fight", "target": "PLAYER", "attitude": "hostile", "agenda": None}
    calm = {"goal": "none", "target": None, "attitude": "wary", "agenda": None}
    dialogue.llm = _ByNpc(
        {
            "Бандит": _OnWords(
                [
                    ("отойди", FakeStep("Ну всё, сам напросился!", [ToolCall("set_intent", fight)])),
                    ("спокуха", FakeStep("Ладно, ладно. Живи пока.", [ToolCall("set_intent", calm)])),
                ],
                FakeStep("Хм."),
            )
        },
        FakeBackend("Хм.", delay=0),
    )

    def state() -> str:
        e = talk.people.get(bandit)
        return e.state.upper() if e is not None else "?"

    async def hero_hp() -> int:
        snap = await link.snapshot(bandit, fresh=True)
        return snap.player.hp if snap.player is not None and snap.player.hp is not None else -1

    try:
        await link.debug("player_equip", item="ITAR_PAL_H")
    except GameError as err:
        log.info("calm: no armor for the hero: %s", err)
    for other in gang:
        try:
            await link.debug("kill", npc=other)
        except GameError as err:
            log.info("calm: kill %s: %s", other, err)
    await link.debug("teleport", near=bandit, distance=250)
    if not await _until(lambda: bandit in talk.people and talk.people[bandit].distance <= 500, 15):
        return _logged({"near": f"failed ({bandit} is not near the hero)"})

    talk.hear("Эй, ты! А ну отойди с дороги.", bandit, {"via": "scenario"})
    attacked = await _until(
        lambda: any(e["kind"] == "attack" for e in entries) and state() == "ZS_ATTACK", 20
    )
    check("attack", attacked, f"state {state()}")

    hp = await hero_hp()
    talk.hear("Воу, воу, ты чего? Спокуха!", None, {"via": "scenario"})
    to = talk.last[0] if talk.last else None
    check(
        "addressed",
        to is not None and to.id == bandit and to.why == "attacker",
        f"{to.why if to else 'nobody'}",
    )
    calmed = await _until(lambda: any(e["kind"] == "calm" for e in entries), 20)
    check("calm", calmed, next((e["attitude"] for e in entries if e["kind"] == "calm"), "no effect"))
    stopped = await _until(lambda: state() != "ZS_ATTACK", 10)
    check("stopped", stopped, f"state {state()}")
    after = await hero_hp()
    again = False
    t_end = time.monotonic() + 15
    while time.monotonic() < t_end:
        again = again or state() == "ZS_ATTACK"
        await asyncio.sleep(0.5)
    last = await hero_hp()
    check(
        "stays_calm",
        stopped and not again and last >= after,
        f"hero hp {hp} -> {after} -> {last}, state {state()}",
    )
    return _logged(results)
