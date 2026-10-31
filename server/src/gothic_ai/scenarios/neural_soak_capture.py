"""Independent live eligibility and capture evidence for each soak-wave actor."""

from __future__ import annotations

import time
from collections import Counter
from collections.abc import Callable
from dataclasses import dataclass, field
from typing import Any

from ..bodypilot import ACQUIRE_WINDOW, BRIDGE_ERRORS, BRIDGE_TIMEOUT, TARGET_IS_HERO, BodyPilot
from ..game import GameLink

VALIDATION_INTERVAL = 0.5
STRUCTURAL_EXCLUSIONS = frozenset({
    "target_not_allowed", "not_one_handed", "not_human", "out_of_range", "no_capacity",
    "previous_wave_owner",
})


def _lease_key(lease: Any) -> tuple[int, int, int, int] | None:
    if lease is None:
        return None
    return (lease.session_epoch, lease.world_epoch, lease.entity_id, lease.control_generation)


@dataclass
class _Attempt:
    fight: Any = None
    lease_key: tuple[int, int, int, int] | None = None
    refusals: Counter[str] = field(default_factory=Counter)
    reason: str | None = None
    completed: bool = False


@dataclass
class _Candidate:
    wave: int
    npc: str
    eligible: bool = False
    captured: bool = False
    first_eligible: float | None = None
    captured_at: float | None = None
    window_start: float | None = None
    window_seconds: float = 0.0
    last_reason: str = "not_in_roster"
    last_emitted: tuple[str, bool] | None = None
    validated_at: float = float("-inf")
    validation_reason: str = "not_in_roster"
    validation_reasons: Counter[str] = field(default_factory=Counter)
    attempts: list[_Attempt] = field(default_factory=list)


class CaptureTracker:
    """Count a slot opportunity only after checking the engine's live actor and target.

    ``poll`` must run while a wave is active and ``finish_wave`` before removing its
    actors. The latter freezes one result per actor, so reusing a symbol later cannot
    change the previous wave's denominator. No pilot or game state is modified.
    """

    def __init__(
        self, link: GameLink, pilot: BodyPilot, event_callback: Callable[..., Any], companion: str, *,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        self.link, self.pilot, self.event, self.clock = link, pilot, event_callback, clock
        self.companion = companion.upper()
        self.started = clock()
        self.eligible: set[tuple[int, str]] = set()
        self.captured: set[tuple[int, str]] = set()
        self.records: list[dict[str, Any]] = []
        self._candidates: dict[tuple[int, str], _Candidate] = {}
        self._waves: set[int] = set()
        self._finished: set[int] = set()
        self._lease_waves: dict[tuple[int, int, int, int], int | None] = {}
        # Retain references: object IDs cannot be reused after the pilot's deque wraps.
        self._seen_summaries: dict[int, Any] = {}

    async def _validate(self, item: _Candidate, now: float) -> str:
        entry = self.pilot.people.get(item.npc)
        if entry is None:
            return "not_in_roster"
        if entry.state.upper() != "ZS_ATTACK" or entry.activity not in {"fight", "body"}:
            return "not_attacking"
        if entry.distance > 2000:
            return "out_of_range"
        if now - item.validated_at < VALIDATION_INTERVAL:
            return item.validation_reason
        weapon = ["call", "NPC_GETEQUIPPEDMELEEWEAPON", ["inst", "SELF"]]
        expressions = [
            ["field", ["inst", "SELF"], "GUILD"],
            ["call", "NPC_ISINSTATE", ["inst", "SELF"], ["sym", "ZS_ATTACK"]],
            ["&", ["field", weapon, "FLAGS"], ["var", "ITEM_SWD"]],
            TARGET_IS_HERO,
            ["&&", ["call", "NPC_GETTARGET", ["inst", "SELF"]],
             ["==", ["call", "HLP_GETINSTANCEID", ["inst", "OTHER"]],
              ["call", "HLP_GETINSTANCEID", ["inst", self.companion]]]],
        ]
        try:
            values = await self.link.story_eval(item.npc, expressions, timeout=BRIDGE_TIMEOUT)
        except BRIDGE_ERRORS:
            reason = "live_validation_error"
        else:
            if len(values) != len(expressions) or any(type(value) is not int for value in values):
                reason = "live_validation_error"
            else:
                guild, attacking, one_handed, hero, companion = values
                if not 0 <= guild <= 16:
                    reason = "not_human"
                elif not attacking:
                    reason = "not_attacking"
                elif not one_handed:
                    reason = "not_one_handed"
                elif not (hero or companion):
                    reason = "target_not_allowed"
                else:
                    reason = "eligible"
        item.validated_at, item.validation_reason = now, reason
        return reason

    def _mark_captured(self, item: _Candidate, now: float) -> None:
        if not item.eligible or item.captured:
            return
        item.captured = True
        self.captured.add((item.wave, item.npc))
        # This is when evidence was observed, an upper bound on capture delay.
        item.captured_at = now
        if item.window_start is not None:
            item.window_seconds += now - item.window_start
            item.window_start = None

    def _evidence(self, wave: int) -> None:
        """Read cumulative refusal snapshots without draining the bounded pilot history."""
        candidates = {item.npc: item for item in self._candidates.values() if item.wave == wave}
        for npc, item in candidates.items():
            fight = self.pilot.fights.get(npc)
            if fight is None:
                continue
            identity = _lease_key(fight.lease)
            if identity is not None and self._lease_waves.get(identity, wave) != wave:
                continue
            attempt = next((part for part in item.attempts if part.fight is fight), None)
            if attempt is None:
                attempt = _Attempt(fight=fight)
                item.attempts.append(attempt)
            attempt.lease_key = _lease_key(fight.lease) or attempt.lease_key
            if attempt.lease_key is not None:
                self._lease_waves.setdefault(attempt.lease_key, wave)
            for reason, count in getattr(fight, "_refusals", {}).items():
                attempt.refusals[reason] = max(attempt.refusals[reason], count)
        for summary in self.pilot.summaries:
            if id(summary) in self._seen_summaries:
                continue
            self._seen_summaries[id(summary)] = summary
            item = candidates.get((summary.npc or "").upper())
            if item is None:
                continue
            identity = _lease_key(summary.lease)
            if identity is not None:
                if self._lease_waves.setdefault(identity, wave) != wave:
                    continue  # A late native return still belongs to its old wave.
            attempt = next((part for part in item.attempts if (
                identity is not None and part.lease_key == identity
            )), None)
            if attempt is None:
                # _completed removes the old fight before publishing its summary.
                attempt = next((part for part in item.attempts if (
                    not part.completed and part.fight is not self.pilot.fights.get(item.npc)
                    and (identity is None or part.lease_key in {None, identity})
                )), None)
            if attempt is None:
                attempt = _Attempt()
                item.attempts.append(attempt)
            attempt.lease_key = identity
            attempt.completed, attempt.reason = True, summary.reason
            for reason, count in summary.acquire_refusals:
                attempt.refusals[reason] = max(attempt.refusals[reason], count)
            if identity is not None:
                self._mark_captured(item, self.clock())

    async def poll(self, wave: int, npcs: set[str]) -> None:
        if wave in self._finished:
            return
        if wave not in self._waves:
            self._waves.add(wave)
            self._seen_summaries.update((id(summary), summary) for summary in self.pilot.summaries)
            for summary in self.pilot.summaries:
                if (identity := _lease_key(summary.lease)) is not None:
                    self._lease_waves.setdefault(identity, None)
        now = self.clock()
        for npc in sorted(npc.upper() for npc in npcs if npc.upper() != self.companion):
            self._candidates.setdefault((wave, npc), _Candidate(wave, npc))
        self._evidence(wave)
        for key, item in list(self._candidates.items()):
            if item.wave != wave:
                continue
            reason = await self._validate(item, now)
            occupied = set(self.pilot.fights) | set(getattr(self.pilot, "_releasing", {}))
            # This actor can already own its reservation by the first polling tick.
            # Validate it independently, then count availability before its own slot.
            other_slots = len(occupied - {item.npc})
            fight = self.pilot.fights.get(item.npc)
            identity = _lease_key(fight.lease) if fight is not None else None
            if identity is not None and self._lease_waves.get(identity, wave) != wave:
                reason = "previous_wave_owner"
            if reason == "eligible" and other_slots >= min(4, self.pilot.max_fights):
                reason = "no_capacity"
            item.validation_reasons[reason] += 1
            qualifies = reason == "eligible"
            if qualifies:
                item.eligible = True
                self.eligible.add(key)
                if item.first_eligible is None:
                    item.first_eligible = now
                if item.window_start is None and not item.captured:
                    item.window_start = now
                fight = self.pilot.fights.get(item.npc)
                lease = fight.lease if fight is not None else None
                if (lease is not None and self.link.body_leases.get(lease.entity_id) == lease
                        and self._lease_waves.get(_lease_key(lease)) == wave):
                    self._mark_captured(item, now)
            elif item.window_start is not None:
                item.window_seconds += now - item.window_start
                item.window_start = None
            item.last_reason = reason
            state = (reason, item.captured)
            if item.last_emitted != state:
                item.last_emitted = state
                self.event(
                    "capture_candidate", wave=wave, npc=item.npc, reason=reason,
                    eligible_now=qualifies, eligible=item.eligible, captured=item.captured,
                    other_slots=other_slots, capacity=min(4, self.pilot.max_fights),
                )

    def finish_wave(self, wave: int) -> list[dict[str, Any]]:
        if wave in self._finished:
            return [record for record in self.records if record["wave"] == wave]
        self._evidence(wave)
        self._finished.add(wave)
        now = self.clock()
        records = []
        for item in self._candidates.values():
            if item.wave != wave:
                continue
            if item.window_start is not None:
                item.window_seconds += now - item.window_start
                item.window_start = None
            refusals: Counter[str] = Counter()
            ends: Counter[str] = Counter()
            for attempt in item.attempts:
                refusals.update(attempt.refusals)
                if attempt.reason is not None:
                    ends[attempt.reason] += 1
            status = "captured" if item.captured else "missed" if item.eligible else "excluded"
            if item.captured:
                reason = "captured"
            elif not item.eligible:
                # Removal, escape or load can replace a validated combat exclusion
                # with a final roster absence. Preserve the actual combat cause.
                structural = Counter({
                    reason: count for reason, count in item.validation_reasons.items()
                    if reason in STRUCTURAL_EXCLUSIONS
                })
                reason = structural.most_common(1)[0][0] if structural else item.last_reason
            elif refusals:
                reason = refusals.most_common(1)[0][0]
            elif "watch_refused" in ends:
                reason = "watch_refused"
            elif "acquire_exhausted" in ends or item.window_seconds >= ACQUIRE_WINDOW:
                reason = "acquire_window"
            elif item.last_reason != "eligible":
                reason = item.last_reason
            else:
                reason = "acquire_window_short"
            record = {
                "wave": wave, "npc": item.npc, "status": status,
                "eligible": item.eligible, "captured": item.captured, "reason": reason,
                "exclusion_reason": reason if status == "excluded" else None,
                "miss_reason": reason if status == "missed" else None,
                "acquire_refusals": dict(sorted(refusals.items())),
                "validation_reasons": dict(sorted(item.validation_reasons.items())),
                "end_reasons": dict(sorted(ends.items())),
                "window_s": round(item.window_seconds, 3),
                "first_eligible_s": (
                    round(item.first_eligible - self.started, 3) if item.first_eligible is not None else None
                ),
                "capture_delay_s": (
                    round(item.captured_at - item.first_eligible, 3) if item.captured_at is not None else None
                ),
            }
            records.append(record)
            self.event("capture_result", **record)
        self.records.extend(records)
        return records
