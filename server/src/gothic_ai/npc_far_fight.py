"""Seeded, nonlethal distant duels using G2 melee damage and independent hit clocks."""

from __future__ import annotations

import hashlib
import math
import random
from dataclasses import dataclass
from typing import Any


@dataclass(frozen=True)
class Coefficients:
    rate_base: float
    rate_per_tier: float
    damage_scale: float

    @classmethod
    def from_mapping(cls, value: Any) -> Coefficients:
        """Validate the versioned calibration data; there is no fallback calibration."""
        if (not isinstance(value, dict) or type(value.get("version")) is not int
                or value["version"] != 1 or not isinstance(value.get("source"), str)
                or not value["source"].strip()):
            raise ValueError("Invalid far-fight calibration version or source")
        numbers = [value.get(key) for key in ("rate_base", "rate_per_tier", "damage_scale")]
        if any(type(number) not in (int, float) or not math.isfinite(number) for number in numbers):
            raise ValueError("Far-fight coefficients must be finite numbers")
        base, tier, scale = numbers
        if base <= 0 or tier < 0 or scale <= 0 or round(5 * scale) < 1:
            raise ValueError("Far-fight coefficients must provide positive hit rates and damage")
        return cls(float(base), float(tier), float(scale))


@dataclass(frozen=True)
class Combatant:
    name: str
    hp: int
    strength: int
    damage: tuple[int, ...]
    damage_type: int
    protection: tuple[int, ...]
    hitchance: int
    tier: int


@dataclass(frozen=True)
class Result:
    winner: str
    loss: int
    duration_s: float
    hits: dict[str, int]
    p_win: float


def hit_damage(
    attacker: Combatant, defender: Combatant, critical: bool, coefficients: Coefficients,
) -> int:
    """Use the drawn weapon's damage mask and NPC damage already including that weapon."""
    value = 0
    for index, damage in enumerate(attacker.damage):
        if not attacker.damage_type & (1 << index) or defender.protection[index] < 0:
            continue
        part = max(attacker.strength + damage - defender.protection[index], 0)
        value += part if critical else (part - 1) // 10
    return round(max(value, 5) * coefficients.damage_scale)


def _duel(
    fighter: Combatant, culprit: Combatant, coefficients: Coefficients, seed: bytes,
) -> tuple[str, int, float, dict[str, int]]:
    rng = random.Random(int.from_bytes(seed, "big"))
    actors = (fighter, culprit)
    hp = [fighter.hp, culprit.hp]
    hits = [0, 0]
    rates = [coefficients.rate_base * (1 + coefficients.rate_per_tier * actor.tier)
             for actor in actors]
    clocks = [rng.expovariate(rate) for rate in rates]
    while True:
        attacker = 0 if clocks[0] < clocks[1] else 1
        defender = 1 - attacker
        critical = rng.randrange(100) < actors[attacker].hitchance
        hp[defender] = max(1, hp[defender] - hit_damage(
            actors[attacker], actors[defender], critical, coefficients,
        ))
        hits[attacker] += 1
        if hp[defender] == 1:
            return (actors[attacker].name, actors[attacker].hp - hp[attacker], clocks[attacker],
                    {actor.name: count for actor, count in zip(actors, hits, strict=True)})
        clocks[attacker] += rng.expovariate(rates[attacker])


def resolve(act: str, fighter: Combatant, culprit: Combatant, coefficients: Coefficients) -> Result:
    """Draw one stable result for the act and estimate the fighter's chance on 400 derived seeds."""
    seed = hashlib.sha256(act.encode("utf-8")).digest()
    winner, loss, duration_s, hits = _duel(fighter, culprit, coefficients, seed)
    wins = sum(_duel(fighter, culprit, coefficients,
                     hashlib.sha256(seed + trial.to_bytes(4, "big")).digest())[0] == fighter.name
               for trial in range(400))
    return Result(winner, loss, duration_s, hits, wins / 400)
