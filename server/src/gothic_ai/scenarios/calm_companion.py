"""The seam between companion defence, talking an attacker down and the optional P30 lease."""
from __future__ import annotations

import asyncio
import time
from typing import Any

from .. import triggers
from ..bodies import Bodies
from ..bodypilot import TARGET_IS_HERO, BodyPilot
from ..dialogue import DialogueManager
from ..executor import Intent
from ..game import GameError, GameLink
from ..llm import ToolCall
from ..llm.fake import FakeBackend, FakeStep
from ..perception import BEASTS
from ..protocol import GameMessage, NpcRoster, NpcSpec, RoutineEntry
from ..talk import TalkDesk
from .common import _ByNpc, _logged, _OnWords, _until, _until_async

BANDIT = "DIR_NPC_CALM_BANDIT"
COMPANION = "DIR_NPC_CALM_COMPANION"
ARENA = "NW_XARDAS_TOWER_VALLEY_09"


def _attacking(who: str) -> list[Any]:
    return ["call", "NPC_ISINSTATE", ["inst", who], ["sym", "ZS_ATTACK"]]


def _targets(who: str) -> list[Any]:
    return ["&&", ["call", "NPC_GETTARGET", ["inst", "SELF"]],
            ["==", ["call", "HLP_GETINSTANCEID", ["inst", "OTHER"]], ["sym", who]]]


async def _prepare(link: GameLink) -> None:
    """Unique swordsmen on the neural arena, with enough health to survive the observation window."""
    await link.debug("player_goto", waypoint="NW_XARDAS_TOWER_VALLEY_01")
    await link.story_npc(NpcSpec(
        npc=BANDIT, like="BDT_1002_BANDIT_L", at=ARENA, name="Бандит",
        routine=[RoutineEntry(
            start="00:00", end="23:59", state="ZS_STAND_ARMSCROSSED", waypoint=ARENA,
        )],
    ))
    await link.story_npc(NpcSpec(
        npc=COMPANION, like="BDT_1002_BANDIT_L", at=ARENA,
        # Same guild as the attacker: no independent guild feud survives a truce
        # with the hero. Orders/party, rather than a guild, make it his companion.
        name="Спутник-мечник",
        routine=[RoutineEntry(
            start="00:00", end="23:59", state="ZS_STAND_ARMSCROSSED", waypoint=ARENA,
        )],
    ))
    for who in (BANDIT, COMPANION):
        await link.debug("skip_important", near=who)
        ready = await Bodies(link).act(
            who, "start_activity", state="ZS_STAND_ARMSCROSSED", waypoint=ARENA,
            minutes=60, arrive=True, radius=100, timeout=15,
        )
        if ready.status != "arrived":
            raise GameError(f"calm_companion preparation: {who}: {ready.status} {ready.detail}")
    for other in (await link.snapshot(BANDIT, fresh=True)).nearby_npcs:
        if not other.dead and other.guild >= BEASTS:
            await link.debug("kill", instance=other.id, near=BANDIT)
    for view in await link.npc_view([BANDIT]):
        for other in view.sees:
            if other.guild >= BEASTS and other.body != "dead":
                await link.debug("kill", instance=other.id, near=BANDIT)
    attributes = await link.story_eval(BANDIT, [
        ["var", "ATR_HITPOINTSMAX"], ["var", "ATR_HITPOINTS"],
    ])
    fields = [["field", ["inst", who], "ATTRIBUTE", attribute]
              for who in (BANDIT, COMPANION) for attribute in attributes]
    health = [max(3000, value) for value in await link.story_eval(BANDIT, fields)]
    trigger = "DIR_CALM_COMPANION_HEALTH"
    await link.patch_graph([triggers.trigger(trigger, triggers.STATE, [
        {"do": "set", "target": field, "value": ["int", value]}
        for field, value in zip(fields, health, strict=True)
    ])])
    try:
        if not await _until_async(lambda: link.story_eval(BANDIT, fields), lambda v: v == health, 5):
            raise GameError("calm_companion preparation: health did not apply")
    finally:
        await link.patch_graph([{"op": "trigger", "id": trigger, "steps": None}])
    await link.debug("player_hp", hp=5000)
    await link.debug("player_equip", item="ITAR_PAL_H")
    await link.story_effect(COMPANION, [{"do": "calm", "attitude": "ATT_FRIENDLY"}])
    await link.debug("npc_goto", npc=BANDIT, waypoint=ARENA, face="PC_HERO")
    await link.debug("teleport", near=BANDIT, distance=300)
    # Behind the hero: placing it in front puts it almost on top of the bandit,
    # which retargets before both sides' original combat targets can be sampled.
    await link.debug("npc_goto", npc=COMPANION, to="PC_HERO", distance=-650, face=BANDIT)


async def calm_companion_scenario(
    link: GameLink, dialogue: DialogueManager, talk: TalkDesk, pilot: BodyPilot | None,
) -> dict[str, str]:
    """A genuine companion defends the hero, then both sides honour the attacker's spoken truce."""
    await link.wait_world()
    if talk.orders is None:
        raise GameError("calm_companion requires companion orders")
    if dialogue.llm.name != "fake":
        raise GameError("calm_companion requires LLM=fake")
    orders = talk.orders
    results = dict.fromkeys((
        "fight", "body", "calm", "released", "bandit_stopped", "companion_stopped", "stays_calm",
    ), "failed (not reached)")

    def check(name: str, ok: bool, detail: object) -> None:
        results[name] = f"{'done' if ok else 'failed'} ({detail})"

    await _prepare(link)
    entries: list[tuple[float, dict[str, Any]]] = []
    attack_seen: list[Any] = []
    calm_views: list[list[Any]] = []
    calm_ack: float | None = None
    [party_aiv] = await link.story_eval(BANDIT, [["var", "AIV_PARTYMEMBER"]])
    attacks_after: list[str] = []
    monitoring = False
    sink, llm = dialogue.journal.sink, dialogue.llm
    effect = link.story_effect

    async def observed_effect(
        who: str, effects: list[dict[str, Any]], timeout: float = 5.0, *, check: bool = False,
    ) -> None:
        nonlocal calm_ack
        calming = who == BANDIT and any(e.get("do") == "calm" for e in effects)

        async def calm_view() -> list[Any]:
            return await link.story_eval(BANDIT, [
                _attacking("SELF"), _attacking(BANDIT), TARGET_IS_HERO, _targets(COMPANION),
                ["call", "NPC_GETTARGET", ["inst", "SELF"]],
                ["field", ["inst", COMPANION], "AIVAR", party_aiv],
            ])

        if calming:
            calm_views.append(await calm_view())
        await effect(who, effects, timeout, check=check)
        if calming:
            calm_ack = time.monotonic()
            calm_views.append(await calm_view())
        if who == BANDIT and any(e.get("do") == "attack" for e in effects):
            # Sample the original victim before the ACK wakes Orders.attacked and the
            # companion's defence can make native AI legitimately retarget the attacker.
            # ZS_ATTACK can be queued before its init has installed a live target.
            async def sample() -> list[Any]:
                attack_seen[:] = await link.story_eval(BANDIT, [
                    _attacking("SELF"), TARGET_IS_HERO, _targets(COMPANION),
                    ["call", "NPC_GETTARGET", ["inst", "SELF"]],
                ])
                return attack_seen

            await _until_async(sample, lambda v: v[:2] == [1, 1], 2)

    def keep(entry: dict[str, Any]) -> None:
        entries.append((time.monotonic(), entry))
        if sink is not None:
            sink(entry)

    def observe(message: GameMessage) -> None:
        if monitoring and isinstance(message, NpcRoster):
            attacks_after.extend(
                e.id for e in message.npcs
                if e.id in {BANDIT, COMPANION} and e.state.upper() == "ZS_ATTACK"
            )

    def journal(kind: str) -> list[float]:
        return [t for t, e in entries if e["kind"] == kind and e.get("npc") == BANDIT]

    fight = {"goal": "fight", "target": "PLAYER", "attitude": "hostile", "agenda": None}
    calm = {"goal": "none", "target": None, "attitude": "wary", "agenda": None}
    dialogue.llm = _ByNpc({"Бандит": _OnWords([
        ("отойди", FakeStep("Сам напросился!", [ToolCall("set_intent", fight)])),
        ("спокуха", FakeStep("Ладно. Живи.", [ToolCall("set_intent", calm)])),
    ], FakeStep("Хм."))}, FakeBackend("Хм.", delay=0))
    dialogue.journal.sink = keep
    link.story_effect = observed_effect  # type: ignore[method-assign]
    link.listeners.append(observe)
    try:
        if not await _until(lambda: BANDIT in talk.people and COMPANION in talk.people, 15):
            raise GameError("calm_companion preparation: swordsmen absent from roster")
        dialogue.open(COMPANION, "Спутник-мечник").intent = Intent("follow_hero", "PLAYER", "friendly")
        talk.hear("За мной!", COMPANION, {"via": "scenario"})
        following = await _until_async(
            lambda: link.snapshot(COMPANION, fresh=True),
            lambda s: s.npc.following and orders.companion(COMPANION), 15,
        )
        if not following:
            raise GameError("calm_companion preparation: companion did not follow")
        attitudes = await link.story_eval(BANDIT, [
            ["call", "NPC_GETATTITUDE", ["inst", BANDIT], ["inst", COMPANION]],
            ["call", "NPC_GETATTITUDE", ["inst", COMPANION], ["inst", BANDIT]],
            ["var", "ATT_HOSTILE"],
        ])
        if any(value is None or value == attitudes[2] for value in attitudes[:2]):
            raise GameError(f"calm_companion preparation: independent guild feud {attitudes}")
        talk.hear("Эй, ты! А ну отойди с дороги.", BANDIT, {"via": "scenario"})

        async def combat() -> list[Any]:
            return [
                *await link.story_eval(BANDIT, [_attacking("SELF"), TARGET_IS_HERO]),
                *await link.story_eval(COMPANION, [_attacking("SELF"), _targets(BANDIT)]),
            ]

        fought = await _until_async(
            combat,
            lambda v: attack_seen[:2] == [1, 1] and v[0] and all(v[2:]) and bool(journal("attack")), 20,
        )
        live = await combat()
        check("fight", fought and orders.companion(COMPANION),
              f"initial bandit attack/hero target/companion target/any target={attack_seen}; "
              f"current bandit attack/hero target, companion attack/bandit target={live}; "
              f"guild attitudes/hostile={attitudes}; "
              f"server_companion={orders.companion(COMPANION)}, "
              f"defence={getattr(orders.fights.get(COMPANION), 'target', None)}")
        if not fought:
            return _logged(results)

        lease = None
        before_summary = pilot.last_summary if pilot else None
        if pilot is not None:
            # The provocation's conversation excludes its partner from body selection. End it,
            # then use neural-companion's native fight/short turn handoff (no selection override).
            talk.end("npc")
            await _until(lambda: BANDIT not in talk.attached, 5)
            await link.npc_act(BANDIT, "fight", npc="PLAYER")
            turned = await Bodies(link).act(BANDIT, "turn_to", npc="PLAYER", timeout=4)
            if turned.status != "done":
                raise GameError(f"calm_companion preparation: handoff {turned.status} {turned.detail}")
            acquired = await _until(lambda: (
                pilot.lease is not None and pilot.lease.npc == BANDIT and pilot._decisions >= 3
            ) or pilot.last_summary is not before_summary, 20)
            lease = pilot.lease
            check("body", acquired and lease is not None and lease.npc == BANDIT
                  and lease.provider == "neural-body" and lease.mode == "realtime"
                  and pilot._decisions >= 3 and pilot._fallbacks == 0,
                  f"lease={lease}, decisions={pilot._decisions}, fallback={pilot._fallbacks}, "
                  f"summary={pilot.last_summary}")
        else:
            check("body", True, "native")
            check("released", True, "native")

        words_at = time.monotonic()
        talk.hear("Воу, воу, ты чего? Спокуха!", BANDIT, {"via": "scenario"})
        to = talk.last[0] if talk.last else None
        calmed = await _until(lambda: bool(journal("calm")), 20)
        calm_at = calm_ack if calm_ack is not None else time.monotonic()
        check("calm", calmed and to is not None and to.id == BANDIT,
              f"addressee={to}, acknowledged calm={calmed}, after_words_s={calm_at - words_at:.3f}; "
              f"before/after self/named attack, hero/companion/any target, party={calm_views}")

        # All deadlines use the same acknowledged calm, not three consecutive five-second waits.
        deadline = calm_at + 5
        bandit_at = companion_at = released_at = None
        summary = None
        following = False
        states: list[Any] = []
        while time.monotonic() <= deadline:
            states = await link.story_eval(BANDIT, [_attacking(BANDIT), _attacking(COMPANION)])
            snap = await link.snapshot(COMPANION, fresh=True)
            now = time.monotonic()
            if not states[0] and bandit_at is None:
                bandit_at = now
            following = snap.npc.following
            if not states[1] and following and COMPANION not in orders.fights and companion_at is None:
                companion_at = now
            if pilot is not None and pilot.last_summary is not before_summary:
                candidate = pilot.last_summary
                if candidate is not None and candidate.npc == BANDIT and candidate.lease == lease:
                    summary = candidate
                    if candidate.release_confirmed and pilot.lease is None and released_at is None:
                        released_at = now
            if bandit_at and companion_at and (pilot is None or released_at):
                break
            await asyncio.sleep(0.1)
        if pilot is not None:
            check("released", lease is not None and released_at is not None
                  and released_at <= deadline and summary is not None and summary.fallbacks == 0,
                  f"release_confirmed={summary.release_confirmed if summary else False}, "
                  f"after_calm_s={released_at - calm_at if released_at else None}, "
                  f"reason={summary.reason if summary else None}, "
                  f"fallback={summary.fallbacks if summary else None}")
        check("bandit_stopped", bandit_at is not None and bandit_at <= deadline,
              f"attack={states[0]}, after_calm_s={bandit_at - calm_at if bandit_at else None}")
        check("companion_stopped", companion_at is not None and companion_at <= deadline,
              f"attack={states[1]}, following={following}, "
              f"after_calm_s={companion_at - calm_at if companion_at else None}")

        snap = await link.snapshot(COMPANION, fresh=True)
        hp = (snap.player.hp if snap.player else None, snap.npc.hp)
        minimum = list(hp)
        samples = 0
        alive = True
        monitoring = True
        started = time.monotonic()
        while time.monotonic() - started < 15:
            states = await link.story_eval(BANDIT, [
                _attacking(BANDIT), _attacking(COMPANION),
                ["call", "NPC_ISDEAD", ["inst", BANDIT]],
            ])
            if any(states[:2]):
                attacks_after.append(str(states[:2]))
            alive &= not bool(states[2])
            bandit = await link.snapshot(BANDIT, fresh=True)
            alive &= bandit.npc.hp > 0
            snap = await link.snapshot(COMPANION, fresh=True)
            current = (snap.player.hp if snap.player else None, snap.npc.hp)
            minimum = [min(a, b) if a is not None and b is not None else None
                       for a, b in zip(minimum, current, strict=True)]
            samples += 1
            await asyncio.sleep(0.25)
        check("stays_calm", not attacks_after and alive and None not in hp and list(hp) == minimum,
              f"seconds={time.monotonic() - started:.2f}, samples={samples}, "
              f"attacks={len(attacks_after)}, examples={attacks_after[:6]}, "
              f"hero/companion hp={hp}, minimum={minimum}, bandit_alive={alive}")
        return _logged(results)
    finally:
        dialogue.journal.sink, dialogue.llm = sink, llm
        link.story_effect = effect  # type: ignore[method-assign]
        link.listeners.remove(observe)
