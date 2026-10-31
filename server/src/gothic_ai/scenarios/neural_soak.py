"""Timed full-world P30 waves; the policy and native world keep their normal clocks."""

from __future__ import annotations

import asyncio
import contextlib
import logging
import os
import time
from collections.abc import Collection, Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Literal

from .. import triggers
from ..bodies import Bodies
from ..bodypilot import ACQUIRE_WINDOW, WORLD_STOP_REASONS, BodyFight, BodyPilot, FightSummary
from ..dialogue import DialogueManager
from ..game import GameError, GameLink
from ..llm.fake import FakeBackend
from ..protocol import BodyLease, GameMessage, LineStarted, NpcSpec, RoutineEntry, Session
from .common import _save_or_fail, _until, _until_async, wait_message
from .neural_load import _policy_resets
from .neural_soak_capture import CaptureTracker
from .neural_soak_metrics import SoakMetrics, release_within_deadline

log = logging.getLogger("gothic_ai.scenarios")
BANDITS = tuple(f"DIR_NPC_SOAK_BANDIT_{i}" for i in range(1, 5))
COMPANION = "DIR_NPC_SOAK_COMPANION"
TALKER = "DIR_NPC_SOAK_TALKER"
REFUGE = "NW_XARDAS_TOWER_VALLEY_01"
PLACES = ("NW_XARDAS_GOBBO_02", "NW_XARDAS_TOWER_VALLEY_09")


async def _soak_player_weapon(
    link: GameLink, state: Literal["none", "melee"], *, timeout: float = 5,
) -> None:
    """Wait for the hero without changing the pilot's shared body subscription."""
    deadline = asyncio.timeout(timeout)
    try:
        async with deadline:
            while True:
                try:
                    await link.debug("player_weapon", state=state)
                    return
                except GameError as error:
                    if str(error) != "the player cannot switch weapons now":
                        raise
                await asyncio.sleep(.1)
    except TimeoutError as error:
        if not deadline.expired():
            raise
        raise GameError(f"neural-soak player_weapon {state}: not ready within {timeout:g} s") from error


@dataclass
class Episode:
    fight: BodyFight
    npc: str
    lease: BodyLease
    target_kind: str | None
    wave: int
    started: float
    last_damage: float
    hp: int | None = None
    stall_broken: int = 0
    break_wait_s: float | None = None
    stalls: list[float] = field(default_factory=list)
    down: bool = False
    ended: float | None = None
    checked: bool = False
    target_attackers_peak: int = 1

    def update_end(self, *, current: bool, finish_started: float | None, now: float) -> None:
        if self.ended is not None:
            return
        if finish_started is not None:
            self.ended = finish_started
        elif not current:
            self.ended = now
        if self.ended is not None and self.ended - self.last_damage >= 15:
            self.stalls.append(self.ended - self.last_damage)


def _load_summaries_ok(
    old_leases: Collection[BodyLease], summaries: Sequence[FightSummary], *, cause: str,
    dead_targets: Collection[str] = (),
) -> bool:
    """A dying target may finish an old fight while the load command is in flight."""
    endings = {"hero": ("hero_down", "hero"), "companion": ("target_down", "companion")}
    known_dead = set(dead_targets) | {cause.removesuffix("_death")}
    death_ends = {endings[target] for target in known_dead if target in endings} if (
        cause in {"hero_death", "companion_death"}
    ) else set()
    dead_kinds = {target for _, target in death_ends}
    expected = set(old_leases)
    return (
        len(summaries) == len(expected)
        and {summary.lease for summary in summaries} == expected
        and all(
            (summary.reason == "loading" or (summary.reason, summary.target_kind) in death_ends
             or (summary.reason == "native_restored"
                 and summary.native_release_reason == "target_changed_or_lost"
                 and summary.target_kind in dead_kinds))
            and summary.fallbacks == 0
            and release_within_deadline(summary)
            for summary in summaries
        )
    )


def _load_cleanup_details(
    link: GameLink, pilot: BodyPilot, old_fights: dict[str, BodyFight], *,
    old_worlds: Collection[tuple[int, int]], new_world: Sequence[int], status: Any,
) -> dict[str, Any]:
    """Check old-world cleanup without rejecting new passive roster observers."""
    fields = ("session_epoch", "world_epoch", "entity_id", "control_generation")

    def identity(value: Any) -> dict[str, Any]:
        return {name: getattr(value, name) for name in fields if hasattr(value, name)}

    def unsafe_row(row: dict[str, Any]) -> bool:
        world = (row["session_epoch"], row["world_epoch"])
        return (world in old_worlds or world != tuple(new_world)
                or row["control_generation"] != 0 or row["control_allowed"])

    old_objects = {id(fight) for fight in old_fights.values()}
    old_remaining = [npc for npc, fight in pilot.fights.items() if id(fight) in old_objects]
    stopped_fights = [dict(npc=npc, reason=fight._reason)
                      for npc, fight in pilot.fights.items() if fight._reason in WORLD_STOP_REASONS]
    fight_leases = [dict(npc=npc, **identity(fight.lease))
                    for npc, fight in pilot.fights.items() if fight.lease is not None]
    pending_fights = [dict(npc=npc, **identity(fight._pending_acquire))
                      for npc, fight in pilot.fights.items()
                      if fight._acquiring or fight._pending_acquire is not None]
    pending_link = [dict(source=source, npc=pending.npc, **identity(pending.cmd))
                    for source in ("_body_acquisitions", "_body_acquire_waiters")
                    for pending in getattr(link, source).values()]
    cached_rows = [identity(row) for row in link.body_latest.values()
                   if unsafe_row(row.model_dump())]
    queued_rows = [dict(npc=npc, **{name: row[name] for name in fields})
                   for npc, fight in pilot.fights.items() for row, _ in fight._rows
                   if unsafe_row(row)]
    unsafe_watch = [npc for npc in pilot._watch_npcs if (
        (fight := pilot.fights.get(npc)) is None or id(fight) in old_objects
        or fight.lease is not None or fight._acquiring or fight._pending_acquire is not None
        or fight._reason in WORLD_STOP_REASONS
    )]
    owned_status = [identity(actor) | {"owner": actor.owner,
                                      "release_pending": actor.release_pending,
                                      "acquire_pending": actor.acquire_pending,
                                      "acquire_request_id": actor.acquire_request_id}
                    for actor in status.actors if (
                        actor.owner != "native" or actor.release_pending
                        or actor.acquire_pending or actor.acquire_request_id
                    )]
    body_without_lease = [npc for npc, entry in pilot.people.items()
                          if entry.activity == "body" and not any(
                              lease.npc == npc for lease in link.body_leases.values()
                          )]
    residuals = dict(
        old_fights=old_remaining, stopped_fights=stopped_fights,
        leases=[dict(npc=lease.npc, **identity(lease)) for lease in link.body_leases.values()],
        fight_leases=fight_leases,
        pending_fights=pending_fights, pending_link=pending_link,
        pending_releases=[dict(npc=npc, **identity(pending.lease))
                          for npc, pending in pilot._releasing.items()],
        cached_rows=cached_rows, queued_rows=queued_rows,
        unsafe_watch=unsafe_watch, pending_unwatch=sorted(pilot._pending_unwatch),
        owned_status=owned_status, body_without_lease=body_without_lease,
    )
    return dict(
        checks={name: not values for name, values in residuals.items()}, residuals=residuals,
        observing_fights=sorted(npc for npc, fight in pilot.fights.items() if (
            id(fight) not in old_objects and fight.lease is None
            and not fight._acquiring and fight._pending_acquire is None
            and fight._reason not in WORLD_STOP_REASONS
        )),
    )


def _has_scheduled_combat_load(load_checks: Sequence[dict[str, Any]]) -> bool:
    return any(item["mid_fight"] and item["cause"].startswith("scheduled_") for item in load_checks)


def _scheduled_load_due(
    elapsed: float, duration: float, scheduled_loads: set[int],
    load_checks: Sequence[dict[str, Any]], *, combat: bool,
) -> int | None:
    # A death reload does not cover the planned combat checkpoint. Reserve a
    # marker for a later wave instead of spending both markers in peaceful gaps.
    if not combat and not _has_scheduled_combat_load(load_checks):
        return None
    return next((minute for minute in (10, 20) if (
        minute not in scheduled_loads
        and elapsed >= minute * (60 if duration >= 1200 else duration / 30)
    )), None)


async def neural_soak_scenario(
    link: GameLink, dialogue: DialogueManager, pilot: BodyPilot | None, *, log_dir: Path,
) -> dict[str, str]:
    control = os.environ.get("GOTHIC_SOAK_CONTROL") == "1"
    if control and pilot is not None:
        raise GameError("neural-soak control requires --no-neural-body")
    if not control and (pilot is None or pilot.max_fights != 4):
        raise GameError("neural-soak requires an available body policy and --neural-body-max=4")
    if dialogue.llm.name != "fake":
        raise GameError("neural-soak requires LLM=fake")
    duration = float(os.environ.get("GOTHIC_SOAK_SECONDS", "1800"))
    if not 90 <= duration <= 2100:
        raise GameError("GOTHIC_SOAK_SECONDS must be between 90 and 2100")
    await link.wait_world()
    if link.hello is None or link.hello.protocol < 45:
        raise GameError("neural-soak requires protocol >=45")
    metrics = SoakMetrics(link, pilot, log_dir)
    sequence = 0
    wave = 0
    episodes: dict[int, Episode] = {}
    capture = CaptureTracker(link, pilot, metrics.event, COMPANION) if pilot is not None else None
    release_issues: list[dict[str, Any]] = []
    load_checks: list[dict[str, Any]] = []
    conversations: list[bool] = []
    wave_records: list[dict[str, Any]] = []
    active_wave: set[str] = set()
    latest_identity: dict[str, tuple[Any, Any]] = {}
    reacquire_checks: list[bool] = []
    lines: list[LineStarted] = []
    scheduled_loads: set[int] = set()
    started = time.monotonic()
    hero_dead = False
    companion_dead = False
    last_death_check = float("-inf")
    orphan_since: dict[str, float] = {}
    orphan_reported: set[str] = set()
    sampler: asyncio.Task[None] | None = None
    native_attacking: set[str] = set()

    async def sample_loop() -> None:
        while True:
            metrics.sample()
            await asyncio.sleep(.1)

    def pending_probe() -> bool:
        return not control and any(not item["reacquired"] for item in load_checks)

    def death_cause() -> str | None:
        return "hero_death" if hero_dead else "companion_death" if companion_dead else None

    async def calls(expressions: list[list[Any]]) -> None:
        nonlocal sequence
        sequence += 1
        name = f"DIR_SOAK_OP_{sequence}"
        await link.patch_graph([triggers.trigger(name, triggers.STATE, [
            {"do": "call", "call": expression} for expression in expressions
        ])])
        try:
            if not await _until_async(link.triggers, lambda rows: any(
                row["id"] == name and row["count"] >= 1 for row in rows
            ), 5):
                raise GameError(f"neural-soak preparation: trigger {name} did not fire")
        finally:
            await link.patch_graph([triggers.removal(name)])

    async def remove_bandits() -> None:
        # Wld_RemoveNpc removes corpses as well as survivors. Reinsert the same
        # four symbols next wave; own_npc cannot allocate an existing symbol.
        await calls([["call", "WLD_REMOVENPC", ["sym", npc]] for npc in BANDITS])

    async def heal() -> None:
        await check_companion()
        await link.debug("player_hp", hp=1200)
        await calls([
            ["call", "NPC_CHANGEATTRIBUTE", ["inst", COMPANION], ["int", attribute],
             ["-", ["int", 900], ["field", ["inst", COMPANION], "ATTRIBUTE", attribute]]]
            for attribute in (1, 0)
        ])

    async def check_companion() -> None:
        dead, = await link.story_eval(TALKER, [["call", "NPC_ISDEAD", ["inst", COMPANION]]])
        if dead:
            raise GameError("neural-soak preparation: companion dead=True")
        snapshot = await link.snapshot(COMPANION, fresh=True)
        if snapshot.npc.hp <= 0:
            raise GameError(
                f"neural-soak preparation: companion dead={dead}, "
                f"hp={snapshot.npc.hp}/{snapshot.npc.hp_max}, pos={snapshot.npc.pos}"
            )

    def observe(packet: dict[str, Any]) -> None:
        nonlocal hero_dead, companion_dead
        now = time.monotonic()
        for row in packet["observations"]:
            for episode in list(episodes.values()):
                lease = episode.lease
                if (
                    row["entity_id"], row["session_epoch"], row["world_epoch"],
                    row["control_generation"],
                ) != (lease.entity_id, lease.session_epoch, lease.world_epoch, lease.control_generation):
                    continue
                target = row["target"]
                if target["entity_id"] != lease.target_entity_id:
                    continue
                hp = target["hp"]
                if hp is not None:
                    if episode.hp is not None and hp < episode.hp:
                        if now - episode.last_damage >= 15:
                            episode.stalls.append(now - episode.last_damage)
                        episode.last_damage = now
                    episode.hp = hp
                if target["down"] or hp is not None and hp <= 0:
                    episode.down = True
                    if episode.target_kind == "hero" and hp is not None and hp <= 0:
                        hero_dead = True
                    if episode.target_kind == "companion" and hp is not None and hp <= 0:
                        companion_dead = True
                own = row["self"]
                if episode.ended is None and (episode.down or own["down"] or own["hp"] <= 0):
                    metrics.note_fight_end(lease, now)
                    episode.update_end(
                        current=True, finish_started=metrics.finish_started_at(lease), now=now,
                    )

    def heard(message: GameMessage) -> None:
        if isinstance(message, LineStarted) and message.npc.upper() == TALKER:
            lines.append(message)

    async def poll(*, break_stalls: bool = False, force_death_check: bool = False) -> None:
        nonlocal hero_dead, companion_dead, last_death_check, native_attacking
        if control:
            now = time.monotonic()
            if force_death_check or now - last_death_check >= 1:
                last_death_check = now
                npcs = sorted(active_wave) if break_stalls else []
                # Passive observation never takes ownership. Native ZS_ATTACK,
                # rather than pilot handles, determines when this wave is over.
                values = await link.story_eval(TALKER, [
                    ["call", "NPC_ISDEAD", ["inst", "HERO"]],
                    ["call", "NPC_ISDEAD", ["inst", COMPANION]],
                    *[["&&",
                       [">", ["field", ["inst", npc], "ATTRIBUTE", 0], ["int", 0]],
                       ["call", "NPC_ISINSTATE", ["inst", npc], ["sym", "ZS_ATTACK"]]]
                      for npc in npcs],
                ])
                if len(values) != len(npcs) + 2 or any(
                    type(value) not in (int, bool) for value in values
                ):
                    raise GameError(f"neural-soak native state unavailable: {values}")
                hero_dead = hero_dead or bool(values[0])
                companion_dead = companion_dead or bool(values[1])
                if break_stalls:
                    native_attacking = {npc for npc, attacking in zip(npcs, values[2:], strict=True)
                                        if attacking}
            metrics.sample()
            return
        assert pilot is not None and capture is not None
        metrics.sync_summaries()
        now = time.monotonic()
        orphan_now = {npc for npc, entry in pilot.people.items() if (
            entry.activity == "body" and not any(lease.npc == npc for lease in link.body_leases.values())
        )}
        for npc in set(orphan_since) - orphan_now:
            del orphan_since[npc]
            orphan_reported.discard(npc)
        for npc in orphan_now:
            orphan_since.setdefault(npc, now)
            if now - orphan_since[npc] >= 2 and npc not in orphan_reported:
                orphan_reported.add(npc)
                release_issues.append(dict(npc=npc, orphan_seconds=now - orphan_since[npc]))
        if force_death_check or break_stalls and now - last_death_check >= 1:
            last_death_check = now
            hero, companion = await link.story_eval(TALKER, [
                ["call", "NPC_ISDEAD", ["inst", "HERO"]],
                ["call", "NPC_ISDEAD", ["inst", COMPANION]],
            ])
            hero_dead = hero_dead or bool(hero)
            companion_dead = companion_dead or bool(companion)
        if active_wave:
            attackers = active_wave | {npc for npc, entry in pilot.people.items() if (
                entry.state.upper() == "ZS_ATTACK" and entry.activity in {"fight", "body"}
                and entry.guild <= 16 and not pilot._companion(npc)
            )}
            await capture.poll(wave, attackers)
        for npc in active_wave:
            fight = pilot.fights.get(npc)
            if fight is None or fight.lease is None:
                continue
            lease = fight.lease
            if link.body_leases.get(lease.entity_id) != lease:
                continue
            engine = fight.policy == "engine"
            reset_seen = fight._policy_memory_resets > 0 if engine else bool(resets.get(fight.client))
            policy_identity = fight if engine else fight.client
            if id(fight) not in episodes and reset_seen:
                episodes[id(fight)] = Episode(fight, npc, lease, fight._target_kind, wave, now, now)
                previous = latest_identity.get(npc)
                if previous is not None and previous[0] != lease:
                    reacquire_checks.append(bool(
                        previous[1] is not policy_identity and reset_seen and (engine or (
                            resets[fight.client][0]["world_epoch"] == lease.world_epoch
                            and resets[fight.client][0]["session_epoch"] == lease.session_epoch
                        ))
                    ))
                latest_identity[npc] = (lease, policy_identity)
                for loaded in load_checks:
                    old_worlds = loaded["old_worlds"]
                    if (not loaded["reacquired"]
                            and lease.session_epoch == loaded["new_world"][0]
                            and lease.world_epoch >= loaded["new_world"][1]
                            and [lease.session_epoch, lease.world_epoch] not in old_worlds):
                        loaded["reacquired"] = reset_seen
                        metrics.event("load_reacquire", cause=loaded["cause"], npc=npc,
                                      ok=loaded["reacquired"], lease=lease)
        for episode in list(episodes.values()):
            fight, lease = episode.fight, episode.lease
            npc = episode.npc
            current = pilot.fights.get(npc) is fight
            if current:
                episode.target_attackers_peak = max(episode.target_attackers_peak, sum(
                    other.target_entity_id == lease.target_entity_id
                    and (other.session_epoch, other.world_epoch) == (lease.session_epoch, lease.world_epoch)
                    for other in link.body_leases.values()
                ))
            episode.update_end(
                current=current, finish_started=metrics.finish_started_at(lease), now=now,
            )
            if episode.ended is not None and not episode.checked and now >= episode.ended + 2:
                episode.checked = True
                entry = pilot.people.get(npc)
                orphan = bool(entry and entry.activity == "body" and not any(
                    item.npc == npc for item in link.body_leases.values()
                ))
                old_lease = link.body_leases.get(lease.entity_id) == lease
                summary = next((s for s in metrics.summaries if s.lease == lease), None)
                elapsed = metrics.release_elapsed_seconds(summary) if summary else None
                ok = bool(not old_lease and not orphan and summary
                          and release_within_deadline(summary) and elapsed is not None and elapsed <= 2)
                detail = dict(npc=npc, wave=episode.wave, old_lease=old_lease,
                              orphan=orphan, release_s=summary.release_seconds if summary else None,
                              release_late_s=summary.release_late_seconds if summary else None,
                              release_elapsed_s=elapsed,
                              checked_after_s=now - episode.ended)
                metrics.event("release_check", ok=ok, **detail)
                metrics.event(
                    "fight_diagnostics", npc=npc, wave=episode.wave, lease=lease,
                    target_attackers_peak=episode.target_attackers_peak, target_down=episode.down,
                    stall_broken=episode.stall_broken, stalls_s=episode.stalls,
                    stall_count=len(episode.stalls), break_wait_s=episode.break_wait_s,
                )
                if not ok:
                    release_issues.append(detail)
            if (break_stalls and not hero_dead and not companion_dead
                    and current and episode.ended is None and not episode.stall_broken
                    and now - episode.last_damage >= 15):
                stalled = now - episode.last_damage
                episode.stall_broken = 1
                episode.break_wait_s = stalled
                snapshot = await link.snapshot(npc, fresh=True)
                if pilot.fights.get(npc) is not fight or episode.ended is not None or snapshot.npc.hp <= 0:
                    continue
                if snapshot.player and snapshot.player.distance > 450:
                    await link.debug("teleport", near=npc, distance=250)
                if pilot.fights.get(npc) is not fight or episode.ended is not None:
                    continue
                hit = "done"
                try:
                    await link.debug("player_hit", npc=npc)
                except GameError as error:
                    if str(error) != "the hit caused no damage":
                        raise
                    hit = str(error)
                metrics.event("stall_broken", npc=npc, wave=episode.wave,
                              seconds=stalled, hit=hit)
        metrics.sample()

    async def pause(seconds: float, *, combat: bool = False) -> None:
        until = time.monotonic() + seconds
        while time.monotonic() < until:
            await poll(break_stalls=combat)
            await asyncio.sleep(min(.1, max(0, until - time.monotonic())))

    async def load_wave(saved: Any, *, cause: str) -> None:
        nonlocal hero_dead, companion_dead, native_attacking
        dead_targets = [kind for kind, dead in (("hero", hero_dead), ("companion", companion_dead))
                        if dead] if cause in {"hero_death", "companion_death"} else []
        old_fights = dict(pilot.fights) if pilot is not None else {}
        old = {npc: (fight.lease, fight.client) for npc, fight in old_fights.items()
               if fight.lease is not None} if pilot is not None else {}
        old_worlds = (sorted({(lease.session_epoch, lease.world_epoch)
                              for lease, _ in [*latest_identity.values(), *old.values()]}
                            | ({(link.body_session_epoch, link.body_generation)}
                               if link.body_session_epoch is not None
                               and link.body_generation is not None else set())) if not control else
                      [(link.body_session_epoch, link.body_generation)])
        before = len(metrics.summaries)
        rss_before = metrics.sample(force=True)
        metrics.event("load_start", cause=cause, wave=wave, rss_before=rss_before)
        load_started = time.monotonic()
        mid_fight = bool(native_attacking) if control else bool(old)
        waiter = asyncio.create_task(wait_message(link, Session, lambda msg: msg.reason == "load"))
        await asyncio.sleep(0)
        try:
            await link.debug("load", 7)
            session = await waiter
        finally:
            waiter.cancel()
        await link.wait_world()
        hero_dead = companion_dead = False
        if control:
            await link.body_watch(["PLAYER"], rate_hz=20)
        await pause(2.1)
        rss_after = metrics.sample(force=True)
        if control:
            new_world = [link.body_session_epoch, link.body_generation]
        else:
            status = await link.body_control_status()
            new_world = [status.session_epoch, status.world_epoch]
            metrics.sync_summaries()
        summaries = metrics.summaries[before:]
        old_summaries = [s for s in summaries if s.lease in {item[0] for item in old.values()}]
        packets = len(link.body_latest)
        checks = dict(
            save_parent=session.parent == saved.branch and session.parent_seq == saved.seq,
            world_changed=new_world not in [list(scope) for scope in old_worlds],
        )
        ok = all(checks.values())
        detail = dict(cause=cause, ok=ok, mid_fight=mid_fight, wave=wave,
                      dead_targets=dead_targets,
                      old_worlds=[list(scope) for scope in old_worlds], new_world=new_world,
                      seconds=time.monotonic() - load_started,
                      rss_before=rss_before, rss_after=rss_after)
        if pilot is not None:
            checks["summaries"] = _load_summaries_ok(
                [item[0] for item in old.values()], old_summaries, cause=cause,
                dead_targets=dead_targets,
            )
            cleanup = _load_cleanup_details(
                link, pilot, old_fights, old_worlds=old_worlds, new_world=new_world, status=status,
            )
            checks.update(cleanup["checks"])
            ok = all(checks.values())
            detail.update(ok=ok, checks=checks, cleanup_residuals=cleanup["residuals"],
                          old_fight_npcs=sorted(old_fights),
                          old_lease_scopes=[lease.model_dump(include={
                              "npc", "session_epoch", "world_epoch", "entity_id", "control_generation",
                          }) for lease, _ in old.values()],
                          observing_fights=cleanup["observing_fights"],
                          old_fights=len(old), summaries=len(old_summaries),
                          active_count=pilot.active_count, leases=len(link.body_leases),
                          cached_rows=packets, reacquired=False,
                          reasons=[s.reason for s in old_summaries],
                          summary_details=[dict(
                              target_kind=s.target_kind, reason=s.reason,
                              native_release_reason=s.native_release_reason,
                              release_confirmed=s.release_confirmed, fallbacks=s.fallbacks,
                              release_seconds=s.release_seconds,
                              release_late_seconds=s.release_late_seconds,
                          ) for s in old_summaries])
        load_checks.append(detail)
        metrics.event("load_check", **detail)
        # Loading deliberately clears GameLink.party. Re-establish this fixture's
        # declared swordsman on both sides before testing post-load combat.
        await check_companion()
        await link.story_effect(COMPANION, [{"do": "party", "value": 1}])
        native_attacking.clear()

    async def conversation() -> None:
        await _soak_player_weapon(link, "none")
        if control:
            await link.body_watch(["PLAYER"], rate_hz=20)
        await link.debug("player_goto", waypoint=REFUGE)
        await link.debug("npc_goto", npc=TALKER, to="PC_HERO", distance=150, face="PC_HERO")
        await link.attach(TALKER)
        mark = len(lines)
        await link.debug("type", text="Путник, как дорога? Всё спокойно?")
        answered = await _until(lambda: len(lines) > mark, 15)
        conversations.append(answered)
        metrics.event("conversation", wave=wave, answered=answered)
        await _until(lambda: not dialogue.speaking(TALKER), 10)
        await link.detach(TALKER)

    # Prepare a fixed four-symbol pool outside the measured interval. The world
    # is neither frozen nor cleared of its people, wildlife or native fights.
    await link.debug("player_goto", waypoint=REFUGE)
    for npc in BANDITS:
        await link.own_npc(npc, like="BDT_1002_BANDIT_L", waypoint=PLACES[1])
    await remove_bandits()
    for npc, name in ((COMPANION, "Спутник-мечник"), (TALKER, "Путник")):
        await link.story_npc(NpcSpec(
            npc=npc, like="BDT_1002_BANDIT_L" if npc == COMPANION else "NONE_100_XARDAS",
            at=REFUGE, name=name, guild="GIL_VLK" if npc == COMPANION else "GIL_NONE",
            routine=[RoutineEntry(start="00:00", end="23:59", state="ZS_STAND_ARMSCROSSED",
                                  waypoint=REFUGE)],
        ))
        await link.debug("skip_important", near=npc)
        # story_npc uses the authored point, which can float above the floor.
        # Ground the fixture before the initial 30-second gap can accumulate
        # a fall; healing a ZS_Dead NPC later does not revive it.
        await link.debug("npc_goto", npc=npc, waypoint=REFUGE)
        ready = await Bodies(link).act(
            npc, "start_activity", state="ZS_STAND_ARMSCROSSED", waypoint=REFUGE,
            minutes=60, arrive=True, radius=100, timeout=15,
        )
        if ready.status != "arrived":
            raise GameError(f"neural-soak preparation: {npc} {ready.status}")
    await check_companion()
    await link.story_effect(COMPANION, [{"do": "party", "value": 1}])
    await heal()
    await check_companion()
    await link.debug("player_equip", item="ITMW_1H_BAU_MACE")
    saved = await _save_or_fail(link, 7)
    original_llm = dialogue.llm
    dialogue.llm = FakeBackend("Пока спокойно. Держи меч наготове.", delay=.15)
    link.raw_body_listeners.append(observe)
    link.listeners.append(heard)
    try:
        metrics.started = time.monotonic()
        if capture is not None:
            capture.started = metrics.started
        reset_observer = (_policy_resets() if pilot is not None and pilot.policy == "service"
                          else contextlib.nullcontext({}))
        with metrics.observe(), reset_observer as resets:
            started = time.monotonic()
            metrics.event("run_start", control=control, duration_s=duration)
            if control:
                await link.body_watch(["PLAYER"], rate_hz=20)
            sampler = asyncio.create_task(sample_loop())
            # Put the 10-minute checkpoint inside a wave, rather than its gap.
            next_wave = started + (30 if duration >= 1200 else 0)
            while time.monotonic() - started < duration or pending_probe():
                if time.monotonic() - started >= duration + 30:
                    break
                if pending_probe():
                    next_wave = min(next_wave, max(time.monotonic(), started + duration - 15))
                deadline = started + duration + (30 if pending_probe() else 0)
                while time.monotonic() < min(next_wave, deadline):
                    # Once a planned combat load is covered, the second scheduled load
                    # may run in the peaceful gap even if the next wave is later.
                    elapsed = time.monotonic() - started
                    marker = _scheduled_load_due(
                        elapsed, duration, scheduled_loads, load_checks, combat=False,
                    )
                    if marker is not None and not pending_probe():
                        saved = await _save_or_fail(link, 7)
                        await load_wave(saved, cause=f"scheduled_{marker}m")
                        scheduled_loads.add(marker)
                        if duration < 1200:
                            next_wave = min(next_wave, time.monotonic() + 1)
                    await pause(min(.1, max(0, min(next_wave, deadline) - time.monotonic())))
                if time.monotonic() - started >= duration and not pending_probe():
                    break
                await poll(force_death_check=True)
                if cause := death_cause():
                    await load_wave(saved, cause=cause)
                probe_only = time.monotonic() - started >= duration
                wave += 1
                place = PLACES[(wave - 1) % 2]
                count = 3 + (wave % 2 == 0)
                active_wave = set(BANDITS[:count])
                wave_rss_before = metrics.sample(force=True)
                metrics.event("wave_prepare", wave=wave, rss_before=wave_rss_before)
                await link.debug("player_goto", waypoint=REFUGE)
                await check_companion()
                await link.debug("npc_goto", npc=COMPANION, waypoint=REFUGE)
                await heal()
                await link.debug("player_equip", item="ITMW_1H_BAU_MACE")
                await check_companion()
                # Restoring an armed hero can threaten the nearby talker while
                # the post-load graph is replayed. Keep the checkpoint peaceful.
                saved = await _save_or_fail(link, 7)
                try:
                    await _soak_player_weapon(link, "melee", timeout=15)
                except GameError:
                    snapshot = await link.snapshot(TALKER, fresh=True)
                    log.error("neural-soak weapon preparation: hero=%s", snapshot.player)
                    raise
                if control:
                    await link.body_watch(["PLAYER"], rate_hz=20)
                await calls([["call", "WLD_INSERTNPC", ["sym", npc], ["str", PLACES[1]]]
                             for npc in BANDITS[:count]])
                await calls([
                    ["call", "NPC_CHANGEATTRIBUTE", ["inst", npc], ["int", attribute],
                     ["-", ["int", 1800], ["field", ["inst", npc], "ATTRIBUTE", attribute]]]
                    for npc in BANDITS[:count] for attribute in (1, 0)
                ])
                symbols = []
                for index, npc in enumerate(BANDITS[:count]):
                    await link.debug("skip_important", near=npc)
                    await link.debug("npc_goto", npc=npc, waypoint=PLACES[1], face="PC_HERO")
                    ready = await Bodies(link).act(
                        npc, "start_activity", state="ZS_STAND_ARMSCROSSED", waypoint=PLACES[1],
                        minutes=60, arrive=True, radius=150, timeout=15,
                    )
                    if ready.status != "arrived":
                        raise GameError(f"neural-soak preparation: {npc} {ready.status}")
                    weapon = ["call", "NPC_GETEQUIPPEDMELEEWEAPON", ["inst", "SELF"]]
                    symbol, sword, guild = await link.story_eval(npc, [
                        ["sym", npc], ["&", ["field", weapon, "FLAGS"], ["var", "ITEM_SWD"]],
                        ["==", ["field", ["inst", "SELF"], "GUILD"], ["var", "GIL_BDT"]],
                    ])
                    if not sword or not guild:
                        raise GameError(f"neural-soak preparation: {npc} sword={sword} bandit={guild}")
                    symbols.append(symbol)
                    await Bodies(link).act(npc, "draw_weapon", kind="melee")
                    await link.debug("npc_goto", npc=npc, to=npc, distance=700 + index * 350)
                if len(set(symbols)) != count:
                    raise GameError("neural-soak preparation: duplicate symbols")
                # Three attackers initially share the hero. On four-actor waves
                # the fourth starts against the swordsman: compare actual target
                # overlap from the recorded leases rather than infer it from spawn.
                await link.debug("npc_goto", npc=BANDITS[0], waypoint=place, face="PC_HERO")
                for index, npc in enumerate(BANDITS[1:count], 1):
                    await link.debug("npc_goto", npc=npc, to=BANDITS[0],
                                     distance=(-250, 500, 750)[index - 1], face="PC_HERO")
                await link.debug("npc_goto", npc=COMPANION, to=BANDITS[0], distance=1000, face=BANDITS[0])
                await Bodies(link).act(COMPANION, "draw_weapon", kind="melee")
                await link.debug("teleport", near=BANDITS[0], distance=250)
                wave_start = time.monotonic()
                metrics.event("wave_start", wave=wave, place=place, count=count)
                if control:
                    await link.body_watch(list(BANDITS[:count]), rate_hz=20)
                if wave == 1:
                    await link.debug("destruction_traces", text="signal soak_wave")
                await calls([["fn", "B_ATTACK", triggers.npc(npc),
                              triggers.HERO if index < 3 else triggers.npc(COMPANION),
                              ["var", "AR_NONE"], ["int", 0]]
                             for index, npc in enumerate(BANDITS[:count])])
                loaded = False
                combat_deadline = started + duration + (30 if probe_only else 0)
                while time.monotonic() - wave_start < 75 and time.monotonic() < combat_deadline:
                    await poll(break_stalls=True)
                    if probe_only and not pending_probe():
                        break
                    if cause := death_cause():
                        await load_wave(saved, cause=cause)
                        loaded = True
                        break
                    elapsed = time.monotonic() - started
                    marker = _scheduled_load_due(
                        elapsed, duration, scheduled_loads, load_checks, combat=True,
                    )
                    fighting = bool(native_attacking) if control else bool(link.body_leases)
                    if (marker is not None and fighting and not pending_probe()
                            and time.monotonic() - wave_start >= ACQUIRE_WINDOW):
                        # Keep a combat checkpoint, then restore the peaceful
                        # pre-wave save, as neural-load does. Native combat must
                        # not immediately hide old-world ownership behind new leases.
                        checkpoint = await _save_or_fail(link, 8)
                        metrics.event("checkpoint", minute=marker, seq=checkpoint.seq)
                        await load_wave(saved, cause=f"scheduled_{marker}m")
                        scheduled_loads.add(marker)
                        loaded = True
                        break
                    fighting = bool(native_attacking) if control else any(
                        pilot.handles(npc) for npc in active_wave
                    )
                    if time.monotonic() - wave_start > 5 and not fighting:
                        break
                    await asyncio.sleep(.1)
                if not loaded:
                    # The final target_down can remove the last pilot fight
                    # before its death observation reaches this scenario. Check
                    # through the living talker before issuing escape commands.
                    await poll(force_death_check=True)
                    if cause := death_cause():
                        await load_wave(saved, cause=cause)
                        loaded = True
                end = "load" if loaded else "finished"
                if not loaded:
                    fighting = bool(native_attacking) if control else any(
                        pilot.handles(npc) for npc in active_wave
                    )
                    if fighting:
                        end = "escape_75s" if time.monotonic() - wave_start >= 75 else "escape_duration"
                    await link.debug("player_goto", waypoint=REFUGE)
                    # Native combat continues while the hero is moved away.
                    # It may kill the swordsman during that bridge round trip.
                    await poll(force_death_check=True)
                    if cause := death_cause():
                        await load_wave(saved, cause=cause)
                        loaded = True
                        end = "load"
                    else:
                        await link.debug("npc_goto", npc=COMPANION, waypoint=REFUGE)
                        if pilot is not None:
                            await _until(
                                lambda npcs=tuple(active_wave): not any(pilot.handles(npc) for npc in npcs),
                                8,
                            )
                        if control:
                            await link.body_watch(["PLAYER"], rate_hz=20)
                        await pause(2.1)
                        await poll(force_death_check=True)
                        if cause := death_cause():
                            await load_wave(saved, cause=cause)
                            loaded = True
                            end = "load"
                    if capture is not None:
                        capture.finish_wave(wave)
                    if not loaded:
                        await remove_bandits()
                record = dict(wave=wave, place=place, count=count, end=end,
                              seconds=round(time.monotonic() - wave_start, 3),
                              rss_before=wave_rss_before, rss_after=metrics.sample(force=True))
                wave_records.append(record)
                metrics.event("wave_end", **record)
                if capture is not None:
                    capture.finish_wave(wave)
                active_wave.clear()
                native_attacking.clear()
                await heal()
                await conversation()
                await poll()
                first_wave_delay = 30 if duration >= 1200 else 0
                next_wave = max(started + first_wave_delay + wave * 90, time.monotonic())
                if loaded and duration < 1200:
                    # A short smoke run needs a new fight after its final load
                    # to check the fresh client/reset before its deadline.
                    next_wave = time.monotonic() + 1
            await pause(2.1)
            metrics.sample(force=True)
            extra = dict(
                control=control, duration_s=duration, loads_scaled=duration < 1200,
                waves=wave_records, loads=load_checks,
                conversations=conversations, reacquire_checks=reacquire_checks,
                scheduled_loads=sorted(scheduled_loads),
                hero_death=sum(item["cause"] == "hero_death" or "hero" in item["dead_targets"]
                               for item in load_checks),
                companion_death=sum(item["cause"] == "companion_death"
                                    or "companion" in item["dead_targets"] for item in load_checks),
                capture_details=capture.records if capture is not None else [],
                episodes=[dict(npc=e.npc, wave=e.wave, lease=e.lease.model_dump(),
                               stall_broken=e.stall_broken, stalls_s=e.stalls,
                               break_wait_s=e.break_wait_s, stall_count=len(e.stalls),
                               target_attackers_peak=e.target_attackers_peak,
                               target_down=e.down, seconds=(e.ended or time.monotonic()) - e.started)
                          for e in episodes.values()],
            )
            if control:
                for name in ("reacquire_checks", "capture_details", "episodes"):
                    del extra[name]
            extra_checks: dict[str, str] = {}
            loads_ok = all(item["ok"] and (control or item["reacquired"]) for item in load_checks) and (
                all(reacquire_checks)
            ) and (
                duration < 180 or scheduled_loads == {10, 20}
                and _has_scheduled_combat_load(load_checks)
            )
            fresh = f", fresh={sum(reacquire_checks)}/{len(reacquire_checks)}" if not control else ""
            extra_checks["soak_loads"] = (
                f"{'done' if loads_ok else 'failed'} (scheduled={sorted(scheduled_loads)}, "
                f"hero_death={extra['hero_death']}, companion_death={extra['companion_death']}, "
                f"checks={len(load_checks)}{fresh})"
            )
            talk_ok = bool(conversations and all(conversations))
            extra_checks["soak_talk"] = (
                f"{'done' if talk_ok else 'failed'} (answered={sum(conversations)}/{len(conversations)})"
            )
            results = metrics.write_result(
                eligible=len(capture.eligible) if capture is not None else 0,
                captured=len(capture.captured) if capture is not None else 0,
                release_issues=release_issues,
                extra=extra, extra_checks=extra_checks,
            )
            for name, value in results.items():
                log.info("%-28s %s", name, value)
            return results
    finally:
        if sampler is not None:
            sampler.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await sampler
        dialogue.llm = original_llm
        link.raw_body_listeners.remove(observe)
        link.listeners.remove(heard)
