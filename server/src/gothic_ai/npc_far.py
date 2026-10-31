"""Distance-level NPC results using live positions and the current routine as fallback.

A refresh is one tick's world, never a persistent cache. Routines decide distant meetings and sleep;
engine positions decide distances whenever the bridge has them. Distant witness truth has no ray or
field-of-view gate: those cannot be reconstructed outside the ordinary simulation.
"""

from __future__ import annotations

import asyncio
import math
from dataclasses import dataclass
from typing import Any

from .chronicle import Fact
from .game import GameError
from .social import DAY, Social, Spot
from .storyguard import asleep

SIMULATION_CM = 3000
HUMAN_GUILD_LIMIT = 16
_HEROES = frozenset({"PLAYER", "HERO", "PC_HERO"})


def _position(value: Any) -> tuple[float, float, float] | None:
    if (isinstance(value, (list, tuple)) and len(value) == 3
            and all(type(v) in (float, int) and math.isfinite(v) for v in value)):
        return tuple(float(v) for v in value)
    return None


@dataclass(frozen=True)
class FarPosition:
    pos: tuple[float, float, float]
    state: str
    waypoint: str
    source: str  # engine or routine


class FarWorld:
    def __init__(self, will: Any) -> None:
        self.will = will
        self._key: tuple[int, int | None] | None = None
        self._runtime: dict[str, dict[str, Any]] = {}
        self._positions: dict[str, FarPosition | None] = {}
        self._hero: tuple[float, float, float] | None = None
        self._fight_lock = asyncio.Lock()

    @property
    def social(self) -> Social | None:
        rumors = self.will.rumors
        return rumors.social if rumors is not None else None

    def all_names(self) -> list[str]:
        social = self.social
        names = set(social.days if social is not None else ()) | set(self.will.roster)
        crimes = self.will.npc_crimes.crimes
        if crimes is not None:
            names.update(crimes.people)
        return sorted({n.upper() for n in names if n.upper() not in _HEROES})

    def routine(self, npc: str) -> Spot | None:
        social = self.social
        return social.spot(npc, self.will._now or 0) if social is not None else None

    def _current(self, generation: int) -> bool:
        return generation == self.will._generation

    async def refresh(self, generation: int) -> None:
        """Fetch real positions once per rules tick, including the hero in the same response."""
        self._key = None
        self._runtime.clear()
        self._positions.clear()
        self._hero = None
        if not self._current(generation):
            return
        if self.social is None:
            self._key = (generation, self.will._now)
            return
        try:
            state = await self.will.link.story_state(npcs=[*self.all_names(), "PC_HERO"])
        except (GameError, TimeoutError, ConnectionError):
            state = {}
        if not self._current(generation):
            return
        for npc, entry in state.get("npcs", {}).items():
            if isinstance(entry, dict):
                self._runtime[npc.upper()] = entry
        hero = self._runtime.pop("PC_HERO", {})
        self._hero = _position(hero.get("pos"))
        # Older bridges have no batch position. A fresh snapshot also supplies the hero, without
        # trusting GameLink.player from before a teleport or a load.
        if self._hero is None:
            present = next((n for n, entry in self._runtime.items()
                            if entry.get("state") == "alive"), None)
            present = present or next(iter(self.will.roster), None)
            if present is not None:
                await self._snapshot(present, generation)
        if self._current(generation):
            self._key = (generation, self.will._now)

    async def _ensure(self, generation: int) -> bool:
        if not self._current(generation):
            return False
        if self._key != (generation, self.will._now):
            await self.refresh(generation)
        return self._current(generation)

    async def _snapshot(self, npc: str, generation: int) -> FarPosition | None:
        try:
            snapshot = await self.will.link.snapshot(npc, fresh=True)
        except (GameError, TimeoutError, ConnectionError, KeyError):
            return None
        if not self._current(generation) or snapshot.npc.id.upper() != npc:
            return None
        if snapshot.player is not None:
            self._hero = snapshot.player.pos
        if snapshot.npc.hp <= 0:
            self._runtime[npc] = {"state": "dead"}
            self._positions[npc] = None
            return None
        spot = self.routine(npc)
        entry = self.will.roster.get(npc)
        result = FarPosition(
            snapshot.npc.pos, spot.state if spot else entry.state if entry else "",
            spot.waypoint if spot else snapshot.npc.waypoint or snapshot.npc.nearest_waypoint or "",
            "engine",
        )
        self._positions[npc] = result
        return result

    async def position(self, npc: str, generation: int) -> FarPosition | None:
        npc = npc.upper()
        if not await self._ensure(generation):
            return None
        if npc in self._positions:
            return self._positions[npc]
        runtime = self._runtime.get(npc, {})
        if runtime.get("state") in {"dead", "absent"}:
            self._positions[npc] = None
            return None
        spot = self.routine(npc)
        entry = self.will.roster.get(npc)
        state = spot.state if spot else entry.state if entry else ""
        waypoint = spot.waypoint if spot else ""
        if (pos := _position(runtime.get("pos"))) is not None:
            result = FarPosition(pos, state, waypoint, "engine")
        else:
            result = await self._snapshot(npc, generation)
            if not self._current(generation):
                return None
            if result is None and self._runtime.get(npc, {}).get("state") == "dead":
                return None
            if result is None and spot is not None and spot.pos is not None:
                result = FarPosition(spot.pos, spot.state, spot.waypoint, "routine")
        self._positions[npc] = result
        return result

    async def far(self, npc: str, generation: int) -> bool:
        if self.social is None:
            return False
        position = await self.position(npc, generation)
        if position is None or not self._current(generation):
            return False
        if self._hero is not None:
            return math.dist(position.pos, self._hero) >= SIMULATION_CM
        return False

    def human(self, npc: str) -> bool:
        npc = npc.upper()
        entry = self.will.roster.get(npc)
        guild = entry.guild if entry is not None and "guild" in entry.model_fields_set else None
        if guild is None and self.social is not None:
            guild = self.social.guilds.get(npc)
        crimes = self.will.npc_crimes.crimes
        if guild is None and crimes is not None:
            guild = crimes.people.get(npc, {}).get("guild")
        return type(guild) is int and 0 <= guild < HUMAN_GUILD_LIMIT

    def free(self, npc: str) -> bool:
        npc = npc.upper()
        entry, spot = self.will.roster.get(npc), self.routine(npc)
        if (not self.human(npc) or self._runtime.get(npc, {}).get("state") in {"dead", "absent"}
                or (spot is None and entry is None)):
            return False
        states = [spot.state] if spot is not None else []
        if entry is not None:
            if entry.activity != "routine":
                return False
            states.append(entry.state)
        return bool(
            all(not asleep(state) and not self.will.guard.body(npc, state) for state in states)
            and not self.will.busy(npc)
            and not getattr(self.will.link, "body_owned", lambda _: False)(npc)
            and npc not in self.will._active
        )

    async def meeting(self, a: str, b: str, generation: int) -> bool:
        if not await self._ensure(generation) or not self.free(a) or not self.free(b):
            return False
        first, second, social = self.routine(a), self.routine(b), self.social
        if first is None or second is None or social is None:
            return False
        same_place = bool(first.waypoint and first.waypoint.upper() == second.waypoint.upper())
        tie = social.ties.get(a.upper(), {}).get(b.upper())
        now = (self.will._now or 0) % DAY
        if not same_place:
            same_place = bool(tie is not None and any(s <= now < e for s, e in tie.meets))
        return same_place and await self.far(a, generation) and await self.far(b, generation)

    async def alone(self, thief: str, generation: int) -> bool:
        """The thief's belief uses its real view, independently of distant witness truth."""
        views = await self.will.link.npc_view([thief])
        if not self._current(generation):
            return False
        view = next((v for v in views if v.npc.upper() == thief.upper()), None)
        return bool(view is not None and not view.sees and not (view.hero and view.hero.seen))

    async def witnesses(self, thief: str, generation: int) -> list[str]:
        actor = await self.position(thief, generation)
        if actor is None:
            return []
        names = [n for n in self.all_names() if n != thief.upper() and self.human(n)
                 and (spot := self.routine(n)) is not None and not asleep(spot.state)
                 and self._runtime.get(n, {}).get("state") not in {"dead", "absent"}]
        views = await self.will.link.npc_view(names) if names else []
        if not self._current(generation):
            return []
        ranges = {v.npc.upper(): v.theft_range for v in views}
        witnesses = []
        for npc in names:
            radius = ranges.get(npc)
            if type(radius) not in (int, float) or not math.isfinite(radius) or radius <= 0:
                continue
            position = await self.position(npc, generation)
            if not self._current(generation):
                return []
            if position is not None and math.dist(actor.pos, position.pos) <= radius:
                witnesses.append(npc)
        return witnesses

    async def fight(
        self, crime: Fact, fighter: str, generation: int, *, role: str = "reclaim",
    ) -> Fact | None:
        """Compare level, strength and HP in that order; the defender loses an exact tie."""
        async with self._fight_lock:
            return await self._fight(crime, fighter, generation, role=role)

    async def _fight(
        self, crime: Fact, fighter: str, generation: int, *, role: str,
    ) -> Fact | None:
        if not self._current(generation) or self.will.npc_crimes.settled(crime["id"]):
            return None
        culprit, cid = crime["culprit"], crime["id"]
        action = f"far:{cid}:{fighter}:{role}:{culprit}:{crime.get('blamed', '')}"
        chronicle = self.will.chronicle
        previous = next((f for f in chronicle.facts("npc_crime_defense")
                         if f.get("act") == action and f.get("crime") == cid), None)
        if previous is not None:
            return previous
        if not self.free(fighter) or not self.free(culprit):
            raise GameError("дальние участники заняты или защищены сюжетом")
        actor, target = self.routine(fighter), self.routine(culprit)
        if reason := self.will.guard.harm(
            fighter, target=culprit, state=actor.state if actor else "",
            target_state=target.state if target else "", item=crime.get("item", ""),
        ):
            raise GameError(reason)
        exprs = [expression for npc in (fighter, culprit) for expression in (
            ["field", ["inst", npc], "LEVEL"],
            ["field", ["inst", npc], "ATTRIBUTE", 4],
            ["field", ["inst", npc], "ATTRIBUTE", 0],
        )]
        theft = crime["kind"] == "theft"
        if theft:
            exprs.append(["call", "NPC_HASITEMS", ["inst", culprit], ["sym", crime["item"]]])
        values = await self.will.link.story_eval(fighter, exprs)
        if not self._current(generation) or self.will.npc_crimes.settled(cid):
            return None
        if len(values) != len(exprs) or any(type(v) is not int for v in values):
            raise GameError("не удалось проверить уровень, силу и HP дальних участников")
        if values[2] <= 0 or values[5] <= 0:
            raise GameError("дальний участник уже повержен")
        wins = tuple(values[:3]) > tuple(values[3:6])
        returned = 0
        outcome = "defeated" if wins else "defender_down"
        reason = "level_strength_hp" if values[:3] != values[3:6] else "equal_stats_defender_loses"
        already_returned = any(f.get("crime") == cid
                               for f in chronicle.facts("npc_crime_restitution"))
        deeds = self.will.npc_crimes
        restitution = (deeds._generation, cid)
        if wins and theft:
            returned = min(max(0, int(crime.get("count", 0))), max(0, values[6]))
            if already_returned or restitution in deeds._restitution_started:
                outcome, returned = "resolved_elsewhere", 0
            elif not returned:
                outcome = "no_goods"
            else:
                # Share the native defense reservation: a body fight already resolving the same
                # goods must not race a distant result across the simulation boundary.
                deeds._restitution_started.add(restitution)
                try:
                    await self.will.link.story_effect(culprit, [{
                        "do": "give", "item": crime["item"], "count": returned,
                        "to": "npc", "npc": crime["owner"],
                    }], check=True)
                except GameError:
                    # An explicit rejection proves that nothing moved. A lost response or timeout
                    # is uncertain, so those retain the reservation against a duplicate transfer.
                    deeds._restitution_started.discard(restitution)
                    raise
                if not self._current(generation):
                    return None
                chronicle.record({
                    "fact": "npc_crime_restitution", "crime": cid, "npc": crime["owner"],
                    "culprit": culprit, "fighter": fighter, "item": crime["item"], "count": returned,
                    "how": "defeated_thief", "far": True,
                    **({"blamed": crime["blamed"]} if crime.get("blamed") else {}),
                })
                outcome = "reclaimed"
        chronicle.record({
            "fact": "npc_crime_reaction", "crime": cid, "npc": fighter,
            "culprit": culprit,
            "reaction": "fight", "role": role, "act": action, "far": True,
            **({"blamed": crime["blamed"]} if crime.get("blamed") else {}),
        })
        result = {
            "fact": "npc_crime_defense", "crime": cid, "culprit": culprit,
            "fighter": fighter, "act": action, "outcome": outcome, "reason": reason,
            "item": crime.get("item", ""), "count": crime.get("count", 0), "returned": returned,
            "far": True,
            **({"blamed": crime["blamed"]} if crime.get("blamed") else {}),
        }
        chronicle.record(result)
        return result
