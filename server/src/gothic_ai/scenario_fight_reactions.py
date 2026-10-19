"""Live-body regressions for Cavalorn's fights reported on 2026-10-18.

The default decision and speech models are scripted, as in m5. DECIDER_URL can replace the
decision fixture. All perceptions, voice starts, weapon animations, strikes and damage come
from the game; an accepted fight command alone never passes a combat check.
"""

from __future__ import annotations

import asyncio
import logging
import math
import re
import time
from collections import deque
from collections.abc import Awaitable, Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from .bodies import Bodies
from .brain import GameApplier, NpcPlanner
from .brain.planner import PlannerConfig
from .dialogue import DialogueManager
from .executor import Intent
from .game import GameError, GameLink
from .incidents import IncidentDesk
from .protocol import (
    ActionCmd,
    Beasts,
    DebugCmd,
    GameMessage,
    LineStarted,
    NpcActCmd,
    NpcActResult,
    NpcEvent,
    PerceptionHeld,
    ThreatControlCmd,
    TriggerFired,
)
from .scenarios.common import FixedDecider, _until
from .talk import TalkDesk
from .triggers import STATE, removal, trigger

log = logging.getLogger(__name__)

CAVALORN = "BAU_4300_ADDON_CAVALORN"
BANDITS = ("BDT_1014_BANDIT_L", "BDT_1015_BANDIT_L")
ARENA = "NW_FARM1_FIELD_06"
PARKING = "NW_FARM1_PATH_CITY_16"
IDLE = "ZS_STAND_ARMSCROSSED"


@dataclass
class CombatTrace:
    """Keep actual game timestamps, independently of command receipts and wall-clock polling."""

    symbols: dict[int, str]
    armed: dict[str, int] = field(default_factory=dict)
    strikes: dict[str, list[int]] = field(default_factory=dict)
    strike_targets: dict[str, list[tuple[int, int]]] = field(default_factory=dict)
    contacts: dict[str, list[tuple[int, int]]] = field(default_factory=dict)
    received_contacts: dict[str, list[tuple[int, int]]] = field(default_factory=dict)
    event_ids: set[int] = field(default_factory=set)
    native_events: dict[str, deque[dict[str, Any]]] = field(default_factory=dict)
    damage: dict[str, list[tuple[int, int]]] = field(default_factory=dict)
    damage_wall: dict[str, list[float]] = field(default_factory=dict)
    positions: dict[str, tuple[float, float, float]] = field(default_factory=dict)
    target: dict[str, int] = field(default_factory=dict)
    targets_seen: dict[str, set[int]] = field(default_factory=dict)
    entities: dict[str, int] = field(default_factory=dict)
    hp: dict[str, int] = field(default_factory=dict)
    weapons: dict[str, int] = field(default_factory=dict)
    interruptible: dict[str, bool] = field(default_factory=dict)
    latest: dict[str, dict[str, Any]] = field(default_factory=dict)
    latest_rows: dict[str, dict[str, Any]] = field(default_factory=dict)
    combat_states: dict[str, deque[dict[str, Any]]] = field(default_factory=dict)
    game_ms: dict[str, int] = field(default_factory=dict)
    attacking: dict[str, bool] = field(default_factory=dict)
    events: list[tuple[float, GameMessage]] = field(default_factory=list)
    commands: list[tuple[float, str, str, dict[str, Any]]] = field(default_factory=list)
    wolves: list[str] = field(default_factory=list)
    wolf_states: dict[str, list[str]] = field(default_factory=dict)

    def body(self, packet: dict[str, Any]) -> None:
        for row in packet["observations"]:
            who = self.symbols.get(row["script_symbol"])
            if who is None:
                continue
            own, now = row["self"], row["t_game_ms"]
            if who not in self.entities:
                log.info(
                    "fight_reactions body npc=%s entity=%d pos=%s weapon=%d hp=%d",
                    who,
                    row["entity_id"],
                    own["position_world_cm"],
                    own["weapon_state"],
                    own["hp"],
                )
            self.positions[who] = tuple(own["position_world_cm"])
            self.target[who] = row["target"]["entity_id"]
            self.targets_seen.setdefault(who, set()).add(self.target[who])
            self.entities[who] = row["entity_id"]
            self.weapons[who] = own["weapon_state"]
            self.interruptible[who] = own["interruptible"]
            self.latest[who] = own
            self.latest_rows[who] = row
            self.game_ms[who] = now
            state = {
                "body": own.get("body_state_base"), "attack": own["attack_anim"],
                "defence": own.get("defence_active"), "queued": own.get("queued_primitive"),
                "hp": own["hp"], "target": self.target[who],
            }
            history = self.combat_states.setdefault(who, deque(maxlen=256))
            if not history or history[-1]["state"] != state:
                history.append({"game_ms": now, "state": state})
            if own["weapon_state"] >= 2:
                self.armed.setdefault(who, now)
            attacking = bool(own["attack_anim"])
            if attacking and not self.attacking.get(who, False):
                self.strikes.setdefault(who, []).append(now)
                self.strike_targets.setdefault(who, []).append((now, self.target[who]))
                log.info(
                    "fight_reactions strike npc=%s target_entity=%d game_ms=%d", who, self.target[who], now
                )
            self.attacking[who] = attacking
            before = self.hp.get(who)
            if before is not None and own["hp"] < before:
                self.damage.setdefault(who, []).append((now, before - own["hp"]))
                self.damage_wall.setdefault(who, []).append(time.monotonic())
                log.info("fight_reactions damage npc=%s game_ms=%d hp_lost=%d", who, now, before - own["hp"])
            self.hp[who] = own["hp"]
            for event in row.get("events", []):
                if (
                    event["perspective"] != "self"
                    or event["source_entity_id"] != row["entity_id"]
                    or event["event_id"] in self.event_ids
                ):
                    continue
                self.event_ids.add(event["event_id"])
                at, code = event["source_game_ms"], event["code"]
                self.native_events.setdefault(who, deque(maxlen=256)).append({
                    "event": event, "observed_target": self.target[who],
                    "attack_anim": attacking, "body_state_base": own.get("body_state_base"),
                    "defence_active": own.get("defence_active"),
                    "queued_primitive": own.get("queued_primitive"), "hp": own["hp"],
                })
                if code == 3:  # Native hit applied after the real range/focus checks.
                    # Native events omit the victim. This is the target observed with the
                    # event's delivery, retained for diagnostics rather than authoritative attribution.
                    self.contacts.setdefault(who, []).append((at, self.target[who]))
                    log.info(
                        "fight_reactions contact npc=%s observed_target_entity=%d game_ms=%d event_id=%d",
                        who, self.target[who], at, event["event_id"],
                    )
                elif code in (9, 10):  # A contact is real even when armor absorbs all HP loss.
                    self.received_contacts.setdefault(who, []).append((at, code))
                    log.info(
                        "fight_reactions contact received npc=%s code=%d game_ms=%d event_id=%d",
                        who, code, at, event["event_id"],
                    )

    def message(self, msg: GameMessage) -> None:
        if isinstance(msg, Beasts):
            self.wolves = [w.id for w in msg.wolves]
            for wolf in msg.wolves:
                history = self.wolf_states.setdefault(wolf.id, [])
                if not history or history[-1] != wolf.state:
                    history.append(wolf.state)
        if isinstance(msg, LineStarted | NpcEvent | PerceptionHeld):
            self.events.append((time.monotonic(), msg))

    def distance(self, one: str, other: str) -> float | None:
        a, b = self.positions.get(one), self.positions.get(other)
        return math.dist(a, b) if a is not None and b is not None else None

    def horizontal_distance(self, one: str, other: str) -> float | None:
        a, b = self.positions.get(one), self.positions.get(other)
        return math.dist(a[::2], b[::2]) if a is not None and b is not None else None

    def strike_after(self, npc: str, target: str, after: int) -> int | None:
        """A real post-hit strike at the attacker, with the target captured when the strike began."""
        entity = self.entities.get(target)
        return next(
            (t for t, victim in self.strike_targets.get(npc, []) if t >= after and victim == entity), None
        )

    def first_contact(self, npc: str, target: str) -> int | None:
        """Diagnostic applied event with its target observed on delivery, possibly already changed."""
        entity = self.entities.get(target)
        return min((t for t, victim in self.contacts.get(npc, []) if victim == entity), default=None)

    def first_received_contact(self, npc: str) -> int | None:
        return min((t for t, _ in self.received_contacts.get(npc, [])), default=None)

    def counter_interference(self, attacker: str, hit: int, end: int) -> dict[str, list[int]]:
        """The injected contact at H is allowed; further attacks would change the measured response."""
        return {
            "attacks": [t for t in self.strikes.get(attacker, ()) if hit <= t <= end],
            "contacts": [t for t, _ in self.contacts.get(attacker, ()) if hit < t <= end],
        }

    def counter_ready(self, defender: str, attacker: str, after: int) -> bool:
        """A fresh native melee opportunity, rather than proximity to an unseen former target."""
        row = self.latest_rows.get(defender)
        if row is None or row["t_game_ms"] <= after:
            return False
        own, target = row["self"], row["target"]
        velocity = own.get("linear_velocity_cm_s")
        distance = self.distance(defender, attacker)
        return (
            target["entity_id"] == self.entities.get(attacker)
            and all(target.get(k) for k in (
                "visible", "current_valid", "in_w_range", "focus_30", "melee_line_clear",
            ))
            and distance is not None and distance <= 300
            and own["weapon_state"] in (2, 3)
            and not own.get("in_air", True) and not own.get("down", True)
            and velocity is not None and abs(velocity[1]) <= 20
        )


def combat_shouts(log_file: Path, npc: str) -> list[int]:
    """Only successful native audio playback logs count; a queued or suppressed SVM is insufficient."""
    if not log_file.exists():
        return []
    pattern = re.compile(rf"\[ai\] {re.escape(npc)} combat shout \S+ audio_ms=(\d+) game_ms=(\d+)")
    return [int(t) for ms, t in pattern.findall(log_file.read_text(errors="replace")) if int(ms) > 0]


async def fight_reactions(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    incidents: IncidentDesk,
    planner: NpcPlanner | None,
    case: str,
    log_dir: str,
    *,
    native_body: Callable[[str], Awaitable[bool]] | None = None,
) -> dict[str, str]:
    """wolf: intercept before its bite; sight: bloodfly at 30 m; bandits: two armed responses;
    flee: a coward runs from Cavalorn, while the unarmed hero stands still.
    """
    await link.wait_world()
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object) -> None:
        results[name] = f"{'done' if ok else 'failed'} ({detail})"
        log.info("fight_reactions %-26s %s", name, results[name])

    runner: asyncio.Task[None] | None = None
    if planner is None:
        planner = NpcPlanner(
            FixedDecider(
                {"react": ("flee", "continue") if case == "flee" else ("fight", "continue")},
                {"escalate": 3.6},
                npcs=BANDITS,
            ),
            GameApplier(link, native_body=native_body),
            config=PlannerConfig(ask_attitude=False),
        )
        link.listeners.append(planner.on_message)
        incidents.follow(planner)
        runner = asyncio.create_task(planner.run())
    old_traits = planner.builder.traits
    traits = dict(old_traits)
    for bandit in BANDITS:
        traits[bandit] = (
            "A terrified, cowardly bandit, outmatched by Cavalorn. Runs from armed attackers."
            if case == "flee"
            else "A bold, hardened bandit. Defends himself against armed attackers."
        )
    planner.builder.traits = traits
    old_allow = planner.allow
    preparing = {CAVALORN, *BANDITS}

    def setup_allow(record: Any, choice: str) -> str | None:
        if record.id in preparing:
            return "scenario_setup"
        return old_allow(record, choice) if old_allow is not None else None

    # A live model may greet the teleported hero and replace start_activity while we arrange
    # the actors. Restore its ordinary authority before any measured threat is introduced.
    planner.allow = setup_allow

    async def finish_preparation() -> None:
        # A setup answer already in flight must not acquire authority when the gate opens.
        # Use the planner's normal conversation lifecycle to age it out, preserving any
        # real listen/follow hold, then let all setup requests settle behind the gate.
        for npc in preparing:
            record = planner.npcs.get(npc)
            if record is not None:
                was_talking = record.talking
                planner.talking(npc, not was_talking)
                planner.talking(npc, was_talking)
        if not await _until(
            lambda: all(
                record.flight is None and record.pending is None
                for npc in preparing
                if (record := planner.npcs.get(npc)) is not None
            ),
            10,
        ):
            raise GameError("fight_reactions setup: planner requests did not settle")
        preparing.clear()
        planner.allow = old_allow

    trace = CombatTrace({})
    parked: set[str] = set()
    link.listeners.append(trace.message)
    link.raw_body_listeners.append(trace.body)
    send = link.send
    body_watch = link.body_watch

    async def recorded(cmd: Any) -> None:
        if isinstance(cmd, ActionCmd | NpcActCmd):
            trace.commands.append((time.monotonic(), cmd.npc.upper(), cmd.action, dict(cmd.args)))
            if cmd.npc.upper() == CAVALORN and cmd.action == "fight":
                target = str(cmd.args.get("npc", "")).upper()
                log.info(
                    "fight_reactions companion fight target=%s companion_cm=%s hero_cm=%s hp=%s",
                    target,
                    trace.distance(CAVALORN, target),
                    trace.distance("PLAYER", target),
                    trace.hp,
                )
        await send(cmd)

    async def setup_program(npc: str, tag: str, calls: list[Any]) -> None:
        """Execute native setup through a one-time program; story_eval accepts only predicates."""
        id = f"DIR_FIGHT_SETUP_{case.upper()}_{tag}"
        fired: asyncio.Future[TriggerFired] = asyncio.get_running_loop().create_future()

        def heard(msg: GameMessage) -> None:
            if isinstance(msg, TriggerFired) and msg.id == id and not fired.done():
                fired.set_result(msg)

        link.listeners.append(heard)
        try:
            await link.patch_graph([
                trigger(id, STATE, [{"do": "call", "call": call} for call in calls], npc=npc),
            ])
            result = await asyncio.wait_for(fired, 5)
            log.info("fight_reactions setup program %s result=%s", id, result)
            if result.error:
                raise GameError(f"fight_reactions setup: {id}: {result.error}")
        finally:
            link.listeners.remove(heard)
            await link.patch_graph([removal(id)])

    async def ready_companion_sword() -> None:
        # Attaching ends the idle script but its pose can still have occupied hands.
        # AI_ReadyMeleeWeapon drops its request if that pose cannot switch weapons.
        neutral = await link.act(CAVALORN, "play_ani", name="S_RUN")
        if neutral.status != "done":
            raise GameError(f"fight_reactions setup: neutral pose {neutral.status}: {neutral.detail}")
        ready = await _until(
            lambda: (
                trace.latest[CAVALORN].get("body_state_base") in (0, 1, 2, 3)
                and trace.interruptible.get(CAVALORN)
                and not any(trace.latest[CAVALORN].get("gates", {}).values())
            ),
            5,
        )
        before = await link.snapshot(CAVALORN, fresh=True)
        log.info(
            "fight_reactions companion pre-draw neutral=%s body=%s snapshot=%s",
            ready,
            trace.latest.get(CAVALORN),
            before.model_dump(),
        )
        if not ready:
            raise GameError("fight_reactions setup: companion did not reach switchable neutral pose")
        drawn = await link.act(CAVALORN, "draw_weapon", kind="melee")
        actual = await _until(
            lambda: trace.weapons.get(CAVALORN) in (2, 3) and trace.interruptible.get(CAVALORN),
            5,
        )
        after = await link.snapshot(CAVALORN, fresh=True)
        log.info(
            "fight_reactions companion post-draw receipt=%s actual=%s body=%s snapshot=%s",
            drawn,
            actual,
            trace.latest.get(CAVALORN),
            after.model_dump(),
        )
        if drawn.status != "done" or not actual:
            raise GameError("fight_reactions setup: companion melee preparation did not finish")

    async def grounded(npc: str, stage: str, hp: int, low: int, high: int) -> None:
        stable_at: int | None = None
        stable_y: float | None = None
        ready = False
        until = time.monotonic() + 5
        while time.monotonic() < until:
            own = trace.latest[npc]
            now = trace.game_ms[npc]
            y = own["position_world_cm"][1]
            distance = trace.distance(npc, CAVALORN)
            velocity = own.get("linear_velocity_cm_s")
            on_ground = (
                own["hp"] == hp and hp > 0
                and not own["in_air"] and not own["down"]
                and velocity is not None and abs(velocity[1]) <= 20
                and distance is not None and low <= distance <= high
            )
            if not on_ground:
                stable_at = None
            elif stable_at is None or stable_y is None or abs(y - stable_y) > 5:
                stable_at = now
                stable_y = y
            elif now - stable_at >= 750:
                ready = True
                break
            await asyncio.sleep(0.05)
        snapshot = await link.snapshot(CAVALORN if npc == "PLAYER" else npc, fresh=True)
        actual_hp = (
            snapshot.player.hp if npc == "PLAYER" and snapshot.player is not None else snapshot.npc.hp
        )
        log.info(
            "fight_reactions stage=%s npc=%s ready=%s body_game_ms=%s body=%s snapshot=%s",
            stage, npc, ready, trace.game_ms[npc], trace.latest[npc], snapshot.model_dump(),
        )
        if not ready or actual_hp != hp:
            raise GameError(f"fight_reactions setup: {npc} not safely grounded for {stage}")

    link.send = recorded  # type: ignore[method-assign]
    await link.send(ThreatControlCmd(hold=True))
    try:
        await link.debug("player_goto", waypoint=ARENA)
        await link.debug("player_equip", item="ITAR_PAL_H")
        await link.debug("player_hp", hp=1000)
        await link.debug("skip_important", near=CAVALORN)
        await link.debug("npc_goto", npc=CAVALORN, waypoint=ARENA)
        ready = await Bodies(link).act(
            CAVALORN,
            "start_activity",
            state=IDLE,
            waypoint=ARENA,
            minutes=60,
            arrive=True,
            radius=100,
            timeout=15,
        )
        if ready.status != "arrived":
            raise GameError(f"fight_reactions setup: Cavalorn idle {ready.status}: {ready.detail}")
        anchor = (await link.snapshot(CAVALORN, fresh=True)).npc.pos

        # The field is an open, gently sloping pasture. Move actors with the existing debug API:
        # npc_goto(to=PC_HERO,distance=0) supports arbitrary ground positions without a new command.
        def point(x: float, z: float = 0) -> list[float]:
            return [anchor[0] + x, anchor[1], anchor[2] + z]

        async def put(npc: str, x: float, z: float = 0, face: str | None = None) -> None:
            await link.debug("player_goto", pos=point(x, z))
            await link.debug("npc_goto", npc=npc, to="PC_HERO", distance=0, face=face)

        # No stock wolf or sheep can become an unintended target or mask whose damage was observed.
        animals = {
            n.id
            for n in (await link.snapshot(CAVALORN, fresh=True)).nearby_npcs
            if n.guild >= 16 and not n.dead
        }
        animals.update(
            n.id
            for v in await link.npc_view([CAVALORN])
            for n in v.sees
            if n.guild >= 16 and n.body != "dead"
        )
        for other in animals:
            if other != CAVALORN:
                try:
                    await link.debug("kill", instance=other, near=CAVALORN)
                except GameError as err:
                    if str(err) != "it can't die now":
                        raise

        # The hero stays passive throughout all cases. No player attack or threat is injected.
        await link.debug("player_goto", pos=point(-900))
        if not await _until(lambda: CAVALORN in talk.people, 10):
            raise GameError("fight_reactions setup: Cavalorn absent from roster")
        dialogue.open(CAVALORN, "Кавалорн").intent = Intent("follow_hero", "PLAYER", "friendly")

        if case in {"wolf", "sight"}:
            monster = f"DIR_NPC_FIGHT_{case.upper()}"
            kind = "WOLF" if case == "wolf" else "BLOODFLY"
            # Neither the hero nor Cavalorn overlaps the creature during preparation.
            if case == "wolf":
                points = [
                    p for p in await link.waypoints()
                    if p["name"].startswith("NW_FARM1_") and not p["fp"]
                    and abs(p["y"] - anchor[1]) < 500
                ]
                def position(p: dict[str, Any]) -> tuple[float, float, float]:
                    return p["x"], p["y"], p["z"]

                candidates = [p for p in points if 1700 <= math.dist(anchor, position(p)) <= 2400]
                if not candidates:
                    raise GameError(
                        f"fight_reactions setup: no authored wolf point 17–24 m away; points={points}"
                    )
                wolf_point = min(candidates, key=lambda p: abs(math.dist(anchor, position(p)) - 1800))
                log.info("fight_reactions authored wolf=%s", wolf_point)
                await link.debug("player_goto", waypoint=PARKING)
            else:
                await link.debug("player_goto", pos=point(300))
            await link.debug("npc_goto", npc=CAVALORN, waypoint=ARENA, face="PC_HERO")
            if case == "wolf":
                await link.own_npc(monster, like=kind, waypoint=wolf_point["name"])
                await link.attach(monster)
                parked.add(monster)
            else:
                await link.own_npc(monster, like=kind, distance=5000)
                await link.debug("npc_goto", npc=monster, to=CAVALORN, distance=3000)
            if case == "sight":
                creature_pos = (await link.snapshot(monster, fresh=True)).npc.pos
                lane = None
                # Choose an open 30 m line using an independent, unrestricted collision ray.
                # Move only the viewer around the creature: nobody approaches within aggro range.
                for step in range(8):
                    angle = math.pi + step * math.pi / 4
                    dx, dz = math.cos(angle), math.sin(angle)
                    at = [creature_pos[0] + 3000 * dx, creature_pos[1], creature_pos[2] + 3000 * dz]
                    await link.debug("player_goto", pos=at)
                    await link.debug("npc_goto", npc=CAVALORN, to="PC_HERO", distance=0, face=monster)
                    await link.debug("player_goto", pos=[at[0] + 900 * dx, at[1], at[2] + 900 * dz])
                    response = await link._request(
                        DebugCmd(req=link._next_id("r"), cmd="npc_los", npc=CAVALORN, to=monster),
                        5,
                    )
                    ray = response.value or {}
                    log.info("fight_reactions sight lane=%d origin=%s native_ray=%s", step, at, ray)
                    if 2850 <= ray.get("distance_cm", 0) <= 3250 and (
                        ray.get("center_clear") or ray.get("head_clear")
                    ):
                        lane = (await link.snapshot(CAVALORN, fresh=True)).npc.pos
                        break
                if lane is None:
                    raise GameError("fight_reactions setup: no independently verified clear 30 m sight line")
                anchor = lane
            if case == "sight":
                await link.debug("player_goto", pos=point(-900))
            if not await talk.hold(CAVALORN, "follow"):
                raise GameError("fight_reactions setup: cannot hold companion")
            await link.act(CAVALORN, "stop")
            before_turn = await link.snapshot(CAVALORN, fresh=True)
            turned = await link.act(CAVALORN, "turn_to", npc=monster)
            if turned.status != "done":
                raise GameError(f"fight_reactions setup: face monster {turned.status}: {turned.detail}")
            # Marked party members get the companion sight range in the bridge.
            await link.story_effect(CAVALORN, [{"do": "party", "value": 1}])
            symbols = await link.story_eval(CAVALORN, [["sym", n] for n in (CAVALORN, monster, "PC_HERO")])
            trace.symbols = dict(zip(symbols, (CAVALORN, monster, "PLAYER"), strict=True))
            await link.body_watch([CAVALORN, monster, "PLAYER"])
            if not await _until(lambda: all(n in trace.positions for n in (CAVALORN, monster, "PLAYER")), 5):
                raise GameError("fight_reactions setup: missing body observations")
            if case == "wolf":
                await grounded(monster, "wolf_spawn", trace.hp[monster], 1600, 2600)
                await ready_companion_sword()

            if case == "sight":
                await finish_preparation()
                companion = await link.snapshot(CAVALORN, fresh=True)
                creature = await link.snapshot(monster, fresh=True)
                party, senses, guild, hostile, to_companion, to_hero, native_los = await link.story_eval(
                    CAVALORN,
                    [
                        ["field", ["inst", CAVALORN], "AIVAR", 15],
                        ["field", ["inst", CAVALORN], "SENSES_RANGE"],
                        ["field", ["inst", monster], "GUILD"],
                        ["var", "ATT_HOSTILE"],
                        ["call", "NPC_GETATTITUDE", ["inst", monster], ["inst", CAVALORN]],
                        ["call", "NPC_GETATTITUDE", ["inst", monster], ["inst", "PC_HERO"]],
                        ["call", "NPC_CANSEENPCFREELOS", ["inst", CAVALORN], ["inst", monster]],
                    ],
                )
                dx = creature.npc.pos[0] - companion.npc.pos[0]
                dz = creature.npc.pos[2] - companion.npc.pos[2]
                yaw = math.radians(companion.npc.rot)
                facing = (dx * math.cos(yaw) + dz * math.sin(yaw)) / max(1, math.hypot(dx, dz))
                log.info(
                    "fight_reactions sight setup companion_pos=%s rot_before_turn=%.1f rot=%.1f "
                    "monster_pos=%s facing_cos=%.3f party=%s senses=%s monster_guild=%s "
                    "att_to_companion=%s att_to_hero=%s hostile=%s native_los_at_range=%s",
                    companion.npc.pos,
                    before_turn.npc.rot,
                    companion.npc.rot,
                    creature.npc.pos,
                    facing,
                    party,
                    senses,
                    guild,
                    to_companion,
                    to_hero,
                    hostile,
                    native_los,
                )
                if party != 1 or guild < 16 or hostile not in (to_companion, to_hero) or facing < 0.866:
                    raise GameError(
                        "fight_reactions setup: sight requires party, hostile monster and facing it"
                    )
                views = await link.npc_view([CAVALORN])
                log.info("fight_reactions sight view=%s", [v.model_dump() for v in views])
                seen = next((o for v in views for o in v.sees if o.id.upper() == monster), None)
                distance = trace.distance(CAVALORN, monster)
                check(
                    "bloodfly_30m",
                    seen is not None and distance is not None and 2850 <= distance <= 3250,
                    f"visible={seen is not None}, actual_cm={distance}, "
                    f"seen_cm={seen.distance if seen else None}",
                )
            else:
                start_hp = trace.hp.get("PLAYER")
                origin = trace.positions[monster]
                if (distance := trace.distance(monster, "PLAYER")) is None or distance <= 1500:
                    raise GameError(
                        f"fight_reactions setup: wolf already within hero aggro range: {distance}"
                    )
                guild, native_attack = await link.story_eval(
                    monster,
                    [
                        ["field", ["inst", "SELF"], "GUILD"],
                        ["call", "NPC_ISINSTATE", ["inst", "SELF"], ["sym", "ZS_MM_ATTACK"]],
                    ],
                )
                log.info(
                    "fight_reactions wolf pre-hunt guild=%s native_attack=%s beasts=%s body=%s",
                    guild,
                    native_attack,
                    trace.wolves,
                    trace.latest.get(monster),
                )
                if guild != 27:
                    raise GameError(f"fight_reactions setup: wolf has unexpected guild {guild}")
                trace.targets_seen[monster] = set()
                await finish_preparation()
                # Only now bring the passive hero into native hunting range: observations,
                # initial HP and the companion's ready sword precede any possible attack.
                wolf_pos, companion_pos = trace.positions[monster], trace.positions[CAVALORN]
                toward = (companion_pos[0] - wolf_pos[0], companion_pos[2] - wolf_pos[2])
                length = math.hypot(*toward)
                hero_at = [
                    wolf_pos[0] + 1350 * toward[0] / length,
                    companion_pos[1],
                    wolf_pos[2] + 1350 * toward[1] / length,
                ]
                await link.debug("player_goto", pos=hero_at)
                await grounded("PLAYER", "hero_hunt_position", start_hp, 200, 1400)
                hero_snapshot = await link.snapshot(monster, fresh=True)
                response = await link._request(
                    DebugCmd(req=link._next_id("r"), cmd="npc_los", npc=CAVALORN, to=monster), 5,
                )
                ray = response.value or {}
                hero_cm = (
                    math.dist(hero_snapshot.npc.pos, hero_snapshot.player.pos)
                    if hero_snapshot.player is not None else 0
                )
                origin = hero_snapshot.npc.pos
                hero_xz_cm = (
                    math.dist(origin[::2], hero_snapshot.player.pos[::2])
                    if hero_snapshot.player is not None else 0
                )
                log.info(
                    "fight_reactions wolf ready hero_cm=%s hero_xz_cm=%s native_ray=%s body=%s",
                    hero_cm, hero_xz_cm, ray, trace.latest[monster],
                )
                if not 1200 <= hero_cm <= 1500 or not (ray.get("center_clear") or ray.get("head_clear")):
                    raise GameError(
                        "fight_reactions setup: wolf hunt requires clear sight and hero 12–15 m away"
                    )
                await link.detach(monster)
                parked.remove(monster)
                # Install the game's monster attack, with OTHER and target both the hero.
                # Owned instances need not be advertised by the optional wolf voice desk.
                await setup_program(
                    monster,
                    "HUNT",
                    [
                        ["call", "NPC_CLEARAIQUEUE", ["inst", "SELF"]],
                        ["call", "NPC_SETTARGET", ["inst", "SELF"], ["inst", "OTHER"]],
                        [
                            "call", "AI_STARTSTATE", ["inst", "SELF"],
                            ["sym", "ZS_MM_ATTACK"], ["int", 0], ["str", ""],
                        ],
                    ],
                )
                hunting = False
                until = time.monotonic() + 5
                while time.monotonic() < until:
                    native_attack = (await link.story_eval(
                        monster,
                        [["call", "NPC_ISINSTATE", ["inst", "SELF"], ["sym", "ZS_MM_ATTACK"]]],
                    ))[0]
                    hunting = (
                        bool(native_attack)
                        and trace.entities.get("PLAYER") in trace.targets_seen.get(monster, set())
                        and math.dist(origin[::2], trace.positions.get(monster, origin)[::2]) >= 100
                        and (current_xz := trace.horizontal_distance(monster, "PLAYER")) is not None
                        and current_xz < hero_xz_cm - 75
                        and (velocity := trace.latest[monster]["linear_velocity_cm_s"]) is not None
                        and abs(velocity[1]) < 300
                    )
                    if hunting:
                        break
                    await asyncio.sleep(0.1)
                wolf_snapshot = await link.snapshot(monster, fresh=True)
                log.info(
                    "fight_reactions wolf setup hunting=%s native_attack=%s beasts=%s "
                    "target=%s hero_entity=%s "
                    "wolf_pos=%s origin=%s hero_cm=%s view=%s",
                    hunting,
                    native_attack,
                    trace.wolves,
                    trace.target.get(monster),
                    trace.entities.get("PLAYER"),
                    trace.positions.get(monster),
                    origin,
                    trace.distance(monster, "PLAYER"),
                    [v.model_dump() for v in await link.npc_view([CAVALORN])],
                )
                if not hunting:
                    raise GameError(
                        f"fight_reactions setup: wolf did not hunt hero; snapshot={wolf_snapshot}"
                    )
                await _until(lambda: trace.first_received_contact(monster) is not None, 20)
                # Wait through the encounter, so an early command cannot hide a late bite.
                await _until(lambda: trace.hp.get(monster, 1) <= 0 or bool(trace.damage.get("PLAYER")), 25)
                first_companion = next(iter(trace.strikes.get(CAVALORN, [])), None)
                first_wolf = next(iter(trace.strikes.get(monster, [])), None)
                first_hero_bite = trace.strike_after(monster, "PLAYER", 0)
                first_companion_bite = trace.strike_after(monster, CAVALORN, 0)
                # The hero is passive and Cavalorn is the wolf's sole attacker. A victim's
                # native receive event survives a target clear/retarget in that same tick.
                first_companion_contact = trace.first_received_contact(monster)
                companion_applied = trace.first_contact(CAVALORN, monster)
                first_wolf_contact = trace.first_contact(monster, "PLAYER")
                first_hero_contact = trace.first_received_contact("PLAYER")
                hero_damage = trace.damage.get("PLAYER", [])
                check(
                    "intercepts_before_bite",
                    first_companion_contact is not None
                    and (first_hero_contact is None or first_companion_contact < first_hero_contact)
                    and bool(trace.damage.get(monster))
                    and not hero_damage
                    and not trace.strikes.get("PLAYER"),
                    f"first_companion_contact_game_ms={first_companion_contact}, "
                    f"first_hero_received_contact_game_ms={first_hero_contact}, "
                    f"companion_applied_observed_target_game_ms={companion_applied}, "
                    f"wolf_applied_observed_hero_game_ms={first_wolf_contact}, "
                    f"first_companion_game_ms={first_companion}, first_wolf_game_ms={first_wolf}, "
                    f"first_wolf_to_hero_game_ms={first_hero_bite}, "
                    f"first_wolf_to_companion_game_ms={first_companion_bite}, "
                    f"wolf_strike_targets={trace.strike_targets.get(monster)}, "
                    f"hero_hp={start_hp}->{trace.hp.get('PLAYER')}, wolf_hp={trace.hp.get(monster)}",
                )
        else:
            # Both real cave bandits stand idle beside the unarmed hero. The second is held
            # listening, matching the ownership gap in the user's game.
            for i, bandit in enumerate(BANDITS):
                await link.debug("skip_important", near=bandit)
                await link.debug("npc_goto", npc=bandit, waypoint=ARENA)
                ready = await Bodies(link).act(
                    bandit,
                    "start_activity",
                    state=IDLE,
                    waypoint=ARENA,
                    minutes=60,
                    arrive=True,
                    radius=100,
                    timeout=15,
                )
                if ready.status != "arrived":
                    raise GameError(f"fight_reactions setup: {bandit} idle {ready.status}: {ready.detail}")
                if case == "bandits":
                    # Stock bandits have 40 HP: Cavalorn can kill one with its first hit,
                    # leaving no living defender on which to measure the post-hit response.
                    # Set this baseline once, before combat; every measured hit stays native.
                    hp, maximum = await link.story_eval(
                        bandit,
                        [["field", ["inst", "SELF"], "ATTRIBUTE", i] for i in (0, 1)],
                    )
                    await setup_program(
                        bandit,
                        f"HP_{i}",
                        [
                            [
                                "call", "NPC_CHANGEATTRIBUTE", ["inst", "SELF"],
                                ["int", 1], ["int", 500 - maximum],
                            ],
                            [
                                "call", "NPC_CHANGEATTRIBUTE", ["inst", "SELF"],
                                ["int", 0], ["int", 500 - hp],
                            ],
                        ],
                    )
                    baseline = (await link.snapshot(bandit, fresh=True)).npc
                    log.info(
                        "fight_reactions bandit baseline npc=%s hp=%s/%s -> %s/%s",
                        bandit,
                        hp,
                        maximum,
                        baseline.hp,
                        baseline.hp_max,
                    )
                    if (baseline.hp, baseline.hp_max) != (500, 500):
                        raise GameError(f"fight_reactions setup: {bandit} HP baseline was not installed")
                await put(bandit, 1200, i * 500)
            await put(CAVALORN, -600, 0, face=BANDITS[0])
            await link.debug("player_goto", pos=point(400, -650))
            if not await talk.hold(CAVALORN, "follow"):
                raise GameError("fight_reactions setup: cannot hold companion")
            await link.act(CAVALORN, "stop")
            if not await talk.hold(BANDITS[1], "listen"):
                raise GameError("fight_reactions setup: cannot hold listening bandit")
            watch = [CAVALORN, *BANDITS, "PLAYER"]
            watched: tuple[tuple[str, ...], int] | None = None
            watch_stats: dict[str, Any] = {}

            async def passive_body_watch(
                npcs: list[str], rate_hz: int = 20, timeout: float = 5.0,
            ) -> dict[str, Any]:
                nonlocal watched, watch_stats
                # The pilot shares one native subscription with this passive trace. Keep
                # all four actors and their order when it watches or unwatches one fighter.
                actors = list(dict.fromkeys([*watch, *(npc.upper() for npc in npcs)]))
                if len(actors) > 4:
                    raise GameError("fight_reactions setup: passive body watch exceeds four actors")
                signature = (tuple(actors), rate_hz)
                if watched != signature or watch_stats.get("generation") != link.body_generation:
                    watch_stats = await body_watch(actors, rate_hz=rate_hz, timeout=timeout)
                    watched = signature
                return watch_stats

            link.body_watch = passive_body_watch  # type: ignore[method-assign]
            symbols = await link.story_eval(
                CAVALORN, [["sym", "PC_HERO" if n == "PLAYER" else n] for n in watch]
            )
            trace.symbols = dict(zip(symbols, watch, strict=True))
            await link.body_watch(watch)
            if not await _until(lambda: all(n in trace.positions for n in watch), 5):
                raise GameError("fight_reactions setup: missing bandit body observations")
            await ready_companion_sword()
            if not await _until(lambda: all(n in planner.npcs for n in BANDITS), 5):
                raise GameError("fight_reactions setup: bandits absent from planner roster")
            await finish_preparation()

            if case == "flee":
                bandit = BANDITS[1]
                before = trace.distance(bandit, CAVALORN)
                started = time.monotonic()
                assert talk.orders is not None
                talk.orders.fight_intent(CAVALORN, bandit)
                fled = await _until(
                    lambda: any(
                        t >= started and who == bandit and action == "flee" and args.get("npc") == CAVALORN
                        for t, who, action, args in trace.commands
                    ),
                    15,
                )
                # Distance alone is not enough: the pursuer moves too. Require the coward's
                # own body to move at least 3 m, with its flee command aimed at Cavalorn.
                origin = trace.positions.get(bandit)
                moved = await _until(
                    lambda: (
                        origin is not None
                        and bandit in trace.positions
                        and math.dist(origin, trace.positions[bandit]) >= 300
                    ),
                    8,
                )
                check(
                    "flees_companion",
                    fled and moved and trace.target.get(bandit) == trace.entities.get(CAVALORN),
                    f"flee_target={CAVALORN if fled else None}, moved={moved}, "
                    f"distance_cm={before}->{trace.distance(bandit, CAVALORN)}",
                )
                check(
                    "hero_passive",
                    not trace.strikes.get("PLAYER") and not trace.damage.get("PLAYER"),
                    f"hero_strikes={trace.strikes.get('PLAYER', [])}, "
                    f"hero_damage={trace.damage.get('PLAYER', [])}",
                )
            else:
                assert talk.orders is not None
                charge_started = time.monotonic()
                talk.orders.fight_intent(CAVALORN, BANDITS[0])

                def both_prepared() -> bool:
                    for bandit in BANDITS:
                        threat = any(
                            t >= charge_started
                            and isinstance(m, NpcEvent | PerceptionHeld)
                            and m.npc == bandit
                            and m.kind == "npc_threat"
                            and m.other == CAVALORN
                            for t, m in trace.events
                        )
                        draw = trace.armed.get(bandit)
                        shouts = combat_shouts(Path(log_dir) / "game.log", bandit)
                        hit = next(iter(trace.damage.get(bandit, [])), (None, 0))[0]
                        if not (
                            threat and draw is not None and shouts
                            and (hit is None or (draw < hit and any(t < hit for t in shouts)))
                        ):
                            return False
                    return True

                prepared = await _until(both_prepared, 15)
                check(
                    "both_respond_before_contact",
                    prepared,
                    f"draw_game_ms={trace.armed}, native_shout_game_ms="
                    f"{ {b: combat_shouts(Path(log_dir) / 'game.log', b) for b in BANDITS} }",
                )
                if not prepared:
                    return results

                async def stop_companion() -> None:
                    stopped = await link.act(CAVALORN, "stop")
                    if stopped.status != "done":
                        raise GameError(f"fight_reactions setup: companion stop {stopped}")
                    if not await _until(lambda: CAVALORN not in talk.orders.fights, 3):
                        raise GameError("fight_reactions setup: prior companion duel did not stop")

                async def place_idle(npc: str, waypoint: str) -> None:
                    receipts: dict[str, NpcActResult] = {}

                    def heard(msg: GameMessage) -> None:
                        if isinstance(msg, NpcActResult):
                            receipts[msg.id] = msg

                    link.listeners.append(heard)
                    try:
                        act = await link.npc_act(
                            npc, "start_activity", state=IDLE, waypoint=waypoint,
                            place=True, arrive=False, minutes=60,
                        )
                        if not await _until(lambda: act in receipts, 5):
                            raise GameError(f"fight_reactions setup: {npc} placement receipt missing")
                        ready = receipts[act]
                        log.info(
                            "fight_reactions placement npc=%s waypoint=%s result=%s", npc, waypoint, ready
                        )
                        if ready.status != "queued":
                            raise GameError(
                                f"fight_reactions setup: {npc} placement {ready.status}: {ready.detail}"
                            )
                    finally:
                        link.listeners.remove(heard)
                    # Physical completion is proved by grounded(), not by leaving a long idle state.

                async def hit_from_companion(bandit: str) -> int:
                    # Test the defender's response to a confirmed contact, independently of
                    # whether Cavalorn's native tactics choose or land an attack animation.
                    close = await _until(
                        lambda: (
                            (distance := trace.distance(CAVALORN, bandit)) is not None
                            and distance <= 300
                            and trace.target.get(CAVALORN) == trace.entities.get(bandit)
                        ), 15,
                    )
                    if not close:
                        raise GameError("fight_reactions setup: companion did not reach the defender")
                    # A second native blow can restart hit recovery or make the defender block.
                    # Stop BEFORE H: stop clears the fight queue, but an animation already in
                    # progress can still land. Require a fresh, neutral, armed body before H.
                    preparing.add(CAVALORN)
                    planner.allow = setup_allow
                    await stop_companion()
                    stopped_at = trace.game_ms[CAVALORN]
                    quiet = await _until(
                        lambda: (
                            trace.game_ms[CAVALORN] > stopped_at
                            and not trace.attacking.get(CAVALORN)
                            and trace.interruptible.get(CAVALORN)
                            and trace.weapons.get(CAVALORN) in (2, 3)
                            and trace.latest[CAVALORN].get("body_state_base") in (0, 1, 2, 3)
                            and trace.latest[CAVALORN].get("queued_primitive") == 0
                            and not any(trace.latest[CAVALORN].get("gates", {}).values())
                        ), 5,
                    )
                    distance = trace.distance(CAVALORN, bandit)
                    log.info(
                        "fight_reactions counter attacker quiet=%s stopped_game_ms=%s "
                        "ready_game_ms=%s distance_cm=%s body=%s defender_body_owned=%s",
                        quiet, stopped_at, trace.game_ms[CAVALORN], distance,
                        trace.latest[CAVALORN], link.body_owned(bandit),
                    )
                    if not quiet:
                        raise GameError("fight_reactions setup: counter attacker is not quiet")
                    # Proximity during a moving duel is not a melee opportunity: the defender
                    # may face away or pursue an old unseen position. Stage the contact before H,
                    # without attaching/commanding its fight, then prove native visibility/range.
                    await link.debug("npc_goto", npc=bandit, to=CAVALORN, distance=180, face=CAVALORN)
                    placed_at = trace.game_ms[bandit]
                    ready = await _until(lambda: trace.counter_ready(bandit, CAVALORN, placed_at), 5)
                    log.info(
                        "fight_reactions counter defender ready=%s npc=%s placed_game_ms=%s "
                        "row=%s body_owned=%s",
                        ready, bandit, placed_at, trace.latest_rows[bandit], link.body_owned(bandit),
                    )
                    if not ready:
                        raise GameError(
                            "fight_reactions setup: defender has no fresh native melee opportunity"
                        )
                    response = await link._request(
                        DebugCmd(req=link._next_id("r"), cmd="npc_hit", npc=bandit, by=CAVALORN), 5,
                    )
                    proof = response.value or {}
                    hit = proof.get("game_ms")
                    log.info("fight_reactions guaranteed hit evidence=%s", proof)
                    if not (
                        type(hit) is int
                        and proof.get("attacker") == CAVALORN and proof.get("victim") == bandit
                        and proof.get("hp_before", 0) > proof.get("hp_after", 0) > 0
                    ):
                        raise GameError("fight_reactions setup: native hit evidence is invalid")
                    observed = await _until(
                        lambda: (
                            any(t == hit for t, _ in trace.contacts.get(CAVALORN, ()))
                            and (hit, 9) in trace.received_contacts.get(bandit, ())
                            and trace.hp.get(bandit, proof["hp_before"]) <= proof["hp_after"]
                        ), 5,
                    )
                    if not observed:
                        raise GameError("fight_reactions setup: native hit is absent from body history")
                    return hit

                # Both defenders have reacted. Isolate their post-hit measurements so a native
                # target switch or two opponents pinning Cavalorn cannot prevent the first hit.
                await stop_companion()
                parked_hp = (await link.snapshot(BANDITS[1], fresh=True)).npc.hp
                preparing.add(BANDITS[1])
                planner.allow = setup_allow
                await place_idle(BANDITS[1], PARKING)
                await grounded(BANDITS[1], "before_hold", parked_hp, 4000, 50000)
                await link.attach(BANDITS[1])
                parked.add(BANDITS[1])
                await grounded(BANDITS[1], "parked", parked_hp, 4000, 50000)
                await finish_preparation()
                for bandit in BANDITS:
                    if bandit in parked:
                        await stop_companion()
                        await grounded(bandit, "before_return", parked_hp, 4000, 50000)
                        preparing.add(bandit)
                        planner.allow = setup_allow
                        companion_pos = (await link.snapshot(CAVALORN, fresh=True)).npc.pos
                        points = [
                            p for p in await link.waypoints()
                            if p["name"].startswith("NW_FARM1_FIELD_")
                            and not p["fp"]
                            and abs(p["y"] - companion_pos[1]) < 500
                            and 1300 <= math.dist(companion_pos, (p["x"], p["y"], p["z"])) <= 2200
                        ]
                        if not points:
                            raise GameError("fight_reactions setup: no authored return point 13–22 m away")
                        return_points = sorted(
                            points,
                            key=lambda p: abs(math.dist(companion_pos, (p["x"], p["y"], p["z"])) - 1800),
                        )
                        await link.detach(bandit)
                        parked.remove(bandit)
                        for return_point in return_points:
                            # Cavalorn's previous duel can end beside a tree or a fence.
                            # Distance to an authored point alone does not prove a clear lane.
                            log.info("fight_reactions authored return point=%s", return_point)
                            await place_idle(bandit, return_point["name"])
                            await link.attach(bandit)
                            parked.add(bandit)
                            await grounded(bandit, "returned", parked_hp, 1200, 2500)
                            turned = await link.act(CAVALORN, "turn_to", npc=bandit)
                            if turned.status != "done":
                                raise GameError(
                                    f"fight_reactions setup: face defender {turned.status}: {turned.detail}"
                                )
                            views = await link.npc_view([CAVALORN])
                            seen = any(n.id.upper() == bandit for view in views for n in view.sees)
                            response = await link._request(
                                DebugCmd(req=link._next_id("r"), cmd="npc_los", npc=CAVALORN, to=bandit), 5,
                            )
                            ray = response.value or {}
                            distance = ray.get("distance_cm", 0)
                            log.info(
                                "fight_reactions returned defender npc=%s seen=%s distance_cm=%s "
                                "native_ray=%s view=%s",
                                bandit, seen, distance, ray, [view.model_dump() for view in views],
                            )
                            if (
                                seen and 1200 <= distance <= 2500
                                and (ray.get("center_clear") or ray.get("head_clear"))
                            ):
                                break
                            await link.detach(bandit)
                            parked.remove(bandit)
                        else:
                            raise GameError(
                                "fight_reactions setup: returned defender is not visibly in range"
                            )
                        await finish_preparation()
                        await link.detach(bandit)
                        parked.remove(bandit)
                    talk.orders.fight_intent(CAVALORN, bandit)
                    hit = await hit_from_companion(bandit)
                    if hit is not None:
                        complete = await _until(
                            lambda b=bandit, h=hit: (
                                trace.strike_after(b, CAVALORN, h) is not None
                                or trace.game_ms.get(b, 0) >= h + 3000
                            ), 10,
                        )
                        if not complete:
                            raise GameError(
                                f"fight_reactions setup: incomplete 3000 ms observation window; "
                                f"npc={bandit}, hit={hit}, last_game_ms={trace.game_ms.get(bandit)}"
                            )
                    first = trace.strike_after(bandit, CAVALORN, hit) if hit is not None else None
                    interference = trace.counter_interference(CAVALORN, hit, first or hit + 3000)
                    log.info(
                        "fight_reactions counter timeline npc=%s hit=%s first=%s states=%s contacts=%s",
                        bandit, hit, first,
                        {n: [s for s in trace.combat_states.get(n, ()) if s["game_ms"] >= hit]
                         for n in (bandit, CAVALORN)},
                        {n: [c for c in trace.contacts.get(n, ()) if c[0] >= hit]
                         for n in (bandit, CAVALORN)},
                    )
                    threat = next(
                        (
                            t
                            for t, m in trace.events
                            if t >= charge_started
                            and isinstance(m, NpcEvent | PerceptionHeld)
                            and m.npc == bandit
                            and m.kind == "npc_threat"
                            and m.other == CAVALORN
                        ),
                        None,
                    )
                    voices = [
                        m
                        for t, m in trace.events
                        if t >= charge_started and isinstance(m, LineStarted) and m.npc == bandit and m.voiced
                    ]
                    armed = trace.armed.get(bandit)
                    # Game time is authoritative for draw/strike/damage. Voice events carry wall time;
                    # compare those with the arrival time of the first actual damage observation.
                    hit_wall = next(iter(trace.damage_wall.get(bandit, [])), None)
                    voice_before = hit_wall is not None and any(
                        charge_started <= t < hit_wall
                        and isinstance(m, LineStarted)
                        and m.npc == bandit
                        and m.voiced
                        for t, m in trace.events
                    )
                    shouts = combat_shouts(Path(log_dir) / "game.log", bandit)
                    native_voice_before = hit is not None and any(t < hit for t in shouts)
                    check(
                        f"{bandit}_prepares",
                        threat is not None
                        and (voice_before or native_voice_before)
                        and hit is not None
                        and armed is not None
                        and armed < hit,
                        f"threat={threat is not None}, voiced={len(voices)}, native_shout_game_ms={shouts}, "
                        f"draw_game_ms={armed}, first_hit_game_ms={hit}",
                    )
                    check(
                        f"{bandit}_fights_3s",
                        hit is not None and first is not None and first <= hit + 3000
                        and not any(interference.values()),
                        f"first_hit_game_ms={hit}, first_return_strike_game_ms={first}, "
                        f"delay_ms={first - hit if first is not None and hit is not None else None}, "
                        f"companion_interference={interference}",
                    )
                    if hit is not None and (first is None or first - hit > 3000):
                        rows = {n: trace.latest_rows.get(n) for n in (bandit, CAVALORN)}
                        native_events = {
                            n: [
                                e for e in trace.native_events.get(n, ())
                                if e["event"]["source_game_ms"] >= hit
                            ]
                            for n in (bandit, CAVALORN)
                        }
                        snapshot = await link.snapshot(bandit, fresh=True)
                        log.error(
                            "fight_reactions failed counter npc=%s first_hit_game_ms=%s "
                            "last_game_ms=%s body_owned=%s rows=%s native_events=%s commands=%s snapshot=%s",
                            bandit, hit, trace.game_ms.get(bandit), link.body_owned(bandit), rows,
                            native_events, [c for c in trace.commands if c[1] in {bandit, CAVALORN}][-20:],
                            snapshot.model_dump(),
                        )
                    await finish_preparation()
                    # Its metrics are complete. Remove it without a player blow, then stage the
                    # other still-living defender; no damage/voice/strike history is reset.
                    if bandit == BANDITS[0]:
                        await link.debug("kill", npc=bandit)
                check(
                    "hero_passive",
                    not trace.strikes.get("PLAYER") and not trace.damage.get("PLAYER"),
                    f"hero_strikes={trace.strikes.get('PLAYER', [])}, "
                    f"hero_damage={trace.damage.get('PLAYER', [])}",
                )
    finally:
        link.body_watch = body_watch  # type: ignore[method-assign]
        for npc in parked:
            await link.detach(npc)
        link.send = send  # type: ignore[method-assign]
        link.listeners.remove(trace.message)
        link.raw_body_listeners.remove(trace.body)
        await link.body_watch([])
        planner.builder.traits = old_traits
        planner.allow = old_allow
        if runner is not None:
            await link.send(ThreatControlCmd(hold=False))
            runner.cancel()
            await asyncio.gather(runner, return_exceptions=True)
            link.listeners.remove(planner.on_message)
    return results
