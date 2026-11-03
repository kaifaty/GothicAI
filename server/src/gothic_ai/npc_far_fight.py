"""Seeded, nonlethal distant duels using G2 melee damage and independent hit clocks."""

from __future__ import annotations

import hashlib
import math
import random
from dataclasses import dataclass
from typing import Any

# Match the scoped deed-fight StateMaxMs in OpenGothic/ai_bridge/npcobserver.cpp.
TIME_LIMIT_S = 120
_TWO_HANDED = (1 << 16) | (1 << 17)  # Item::is2H: ITM_2HD_SWD | ITM_2HD_AXE


@dataclass(frozen=True)
class Coefficients:
    rate_base: float
    rate_per_tier: float
    damage_scale: float
    parry_per_tier: float = 0.0
    starter_rate: float = 1.0
    variant: str = "A"

    @property
    def model(self) -> str:
        return ("hits_v2" if self.variant in ("B", "C") or self.parry_per_tier != 0
                or self.starter_rate != 1 else "hits_v1")

    @classmethod
    def from_mapping(cls, value: Any) -> Coefficients | Parts:
        """Validate the versioned calibration data; there is no fallback calibration."""
        if isinstance(value, dict) and type(value.get("version")) is int and value["version"] == 3:
            return Parts.from_mapping(value)
        if (not isinstance(value, dict) or type(value.get("version")) is not int
                or value["version"] not in (1, 2) or not isinstance(value.get("source"), str)
                or not value["source"].strip()):
            raise ValueError("Invalid far-fight calibration version or source")
        keys = ("rate_base", "rate_per_tier", "damage_scale")
        if value["version"] == 2:
            keys += ("parry_per_tier", "starter_rate")
        numbers = [value.get(key) for key in keys]
        if any(type(number) not in (int, float) or not math.isfinite(number) for number in numbers):
            raise ValueError("Far-fight coefficients must be finite numbers")
        base, tier, scale = numbers[:3]
        parry, starter = numbers[3:] if value["version"] == 2 else (0, 1)
        if base <= 0 or tier < 0 or scale <= 0 or round(5 * scale) < 1:
            raise ValueError("Far-fight coefficients must provide positive hit rates and damage")
        variant = value.get("variant", "A")
        if parry < 0 or starter <= 0 or variant not in ("A", "B", "C"):
            raise ValueError("Invalid far-fight parry, starter rate or model variant")
        return cls(float(base), float(tier), float(scale), float(parry), float(starter), variant)


@dataclass(frozen=True)
class Parts:
    swing_rate: dict[str, dict[str, float]]
    p_hit: dict[str, dict[str, dict[str, float]]]
    damage_multiplier: dict[str, tuple[float, ...]]
    response_delay: tuple[float, ...]

    model = "hits_v3"
    damage_scale = 1.0

    @classmethod
    def from_mapping(cls, value: Any) -> Parts:
        """Require complete measured tables: sparse-cell pooling belongs to calibration."""
        if (not isinstance(value, dict) or type(value.get("version")) is not int
                or value["version"] != 3 or not isinstance(value.get("source"), str)
                or not value["source"].strip()):
            raise ValueError("Invalid far-fight calibration version or source")

        def number(value: Any, *, probability: bool = False, zero: bool = False) -> float:
            if (type(value) not in (int, float) or not math.isfinite(value)
                    or value < 0 or (not zero and value == 0) or (probability and value > 1)):
                raise ValueError("Invalid far-fight part value")
            return float(value)

        try:
            rates, probabilities, multipliers = {}, {}, {}
            for weapon in ("fist", "1h", "2h"):
                rates[weapon], probabilities[weapon] = {}, {}
                for tier in ("0", "1", "2"):
                    rates[weapon][tier] = number(value["swing_rate"][weapon][tier])
                    probabilities[weapon][tier] = {
                        opponent: number(value["p_hit"][weapon][tier][opponent],
                                         probability=True, zero=True)
                        for opponent in ("0", "1", "2")
                    }
                samples = value["damage_multiplier"][weapon]
                if not isinstance(samples, list) or not samples:
                    raise ValueError("Far-fight damage multiplier samples are empty")
                multipliers[weapon] = tuple(number(sample) for sample in samples)
            delays = value["response_delay"]
            if not isinstance(delays, list) or not delays:
                raise ValueError("Far-fight response delay samples are empty")
            return cls(rates, probabilities, multipliers,
                       tuple(number(delay, zero=True) for delay in delays))
        except (KeyError, TypeError) as exc:
            raise ValueError("Incomplete far-fight part tables") from exc


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
    weapon: str = "1h"


def combatant_from_values(npc: str, values: list[int]) -> Combatant:
    """Build the same melee fighter from story_eval and native calibration snapshots."""
    _, hp, _, strength = values[:4]
    hitchance, protection, damage = values[4:7], values[7:15], values[15:23]
    damage_type, one_handed, two_handed, equipped, flags, melee_type, melee_total = values[23:30]
    melee_damage = values[30:38]
    readied_type, readied_total = values[38:40]
    readied_damage = values[40:48]
    # Inventory applies only the readied item's stats. Replace that contribution with the
    # equipped melee weapon, exactly as drawing it would before swordDamage reads NPC.DAMAGE.
    damage = tuple(damage[i] - readied_damage[i] - (readied_total if readied_type & (1 << i) else 0)
                   + melee_damage[i] + (melee_total if melee_type & (1 << i) else 0)
                   for i in range(8))
    talent = (2 if flags & _TWO_HANDED else 1) if equipped else 0
    tier = (two_handed if talent == 2 else one_handed) if equipped else 0
    if hp <= 0 or not 0 <= tier <= 2:
        raise ValueError("Invalid distant combatant HP or melee talent tier")
    return Combatant(npc, hp, strength, damage, melee_type if equipped else damage_type,
                     tuple(protection), hitchance[talent], tier, ("fist", "1h", "2h")[talent])


@dataclass(frozen=True)
class Result:
    winner: str | None
    loss: int
    duration_s: float
    hits: dict[str, int]
    p_win: float
    by_loss: int = 0
    p_timeout: float = 0


def hit_damage(
    attacker: Combatant, defender: Combatant, critical: bool, coefficients: Coefficients | Parts,
) -> int:
    """Use the drawn weapon's damage mask and NPC damage already including that weapon."""
    value = 0
    for index, damage in enumerate(attacker.damage):
        if not attacker.damage_type & (1 << index) or defender.protection[index] < 0:
            continue
        part = max(attacker.strength + damage - defender.protection[index], 0)
        value += part if critical else max(part - 1, 0) // 10
    return round(max(value, 5) * coefficients.damage_scale)


def _duel(
    fighter: Combatant, culprit: Combatant, coefficients: Coefficients | Parts, seed: bytes,
) -> tuple[str | None, int, float, dict[str, int], int]:
    if isinstance(coefficients, Parts):
        return _duel_parts(fighter, culprit, coefficients, seed)
    rng = random.Random(int.from_bytes(seed, "big"))
    actors = (fighter, culprit)
    hp = [fighter.hp, culprit.hp]
    hits = [0, 0]
    rates = [coefficients.rate_base * (1 + coefficients.rate_per_tier * actor.tier)
             * max(0, 1 - coefficients.parry_per_tier * actors[1 - i].tier)
             * (coefficients.starter_rate if i == 0 else 1)
             for i, actor in enumerate(actors)]
    clocks = [rng.expovariate(rate) if rate > 0 else math.inf for rate in rates]
    while True:
        attacker = 0 if clocks[0] < clocks[1] else 1
        if clocks[attacker] > TIME_LIMIT_S:
            return (None, fighter.hp - hp[0], TIME_LIMIT_S,
                    {actor.name: count for actor, count in zip(actors, hits, strict=True)},
                    culprit.hp - hp[1])
        defender = 1 - attacker
        critical = rng.randrange(100) < actors[attacker].hitchance
        hp[defender] = max(1, hp[defender] - hit_damage(
            actors[attacker], actors[defender], critical, coefficients,
        ))
        hits[attacker] += 1
        if hp[defender] == 1:
            return (actors[attacker].name, actors[attacker].hp - hp[attacker], clocks[attacker],
                    {actor.name: count for actor, count in zip(actors, hits, strict=True)}, 0)
        clocks[attacker] += rng.expovariate(rates[attacker])


def _duel_parts(
    fighter: Combatant, culprit: Combatant, parts: Parts, seed: bytes,
) -> tuple[str | None, int, float, dict[str, int], int]:
    rng = random.Random(int.from_bytes(seed, "big"))
    actors = (fighter, culprit)
    hp = [fighter.hp, culprit.hp]
    hits = [0, 0]
    rates = [parts.swing_rate[actor.weapon][str(actor.tier)] for actor in actors]
    probabilities = [parts.p_hit[actor.weapon][str(actor.tier)][str(actors[1 - i].tier)]
                     for i, actor in enumerate(actors)]
    clocks = [rng.expovariate(rates[0]), math.inf]
    while True:
        attacker = 0 if clocks[0] < clocks[1] else 1
        if clocks[attacker] > TIME_LIMIT_S:
            return (None, fighter.hp - hp[0], TIME_LIMIT_S,
                    {actor.name: count for actor, count in zip(actors, hits, strict=True)},
                    culprit.hp - hp[1])
        defender = 1 - attacker
        if rng.random() < probabilities[attacker]:
            critical = rng.randrange(100) < actors[attacker].hitchance
            damage = hit_damage(actors[attacker], actors[defender], critical, parts)
            damage = max(1, math.floor(damage * rng.choice(parts.damage_multiplier[
                actors[attacker].weapon]) + .5))
            hp[defender] = max(1, hp[defender] - damage)
            hits[attacker] += 1
            if hp[defender] == 1:
                return (actors[attacker].name, actors[attacker].hp - hp[attacker], clocks[attacker],
                        {actor.name: count for actor, count in zip(actors, hits, strict=True)}, 0)
            if attacker == 0 and clocks[1] == math.inf:
                # B_AssessDamage activates the defender after the first received hit.
                clocks[1] = clocks[0] + rng.choice(parts.response_delay)
        clocks[attacker] += rng.expovariate(rates[attacker])


def resolve(
    act: str, fighter: Combatant, culprit: Combatant, coefficients: Coefficients | Parts,
) -> Result:
    """Draw one stable result for the act and estimate the fighter's chance on 400 derived seeds."""
    seed = hashlib.sha256(act.encode("utf-8")).digest()
    winner, loss, duration_s, hits, by_loss = _duel(fighter, culprit, coefficients, seed)
    outcomes = [_duel(fighter, culprit, coefficients,
                      hashlib.sha256(seed + trial.to_bytes(4, "big")).digest())[0]
                for trial in range(400)]
    return Result(winner, loss, duration_s, hits, outcomes.count(fighter.name) / 400,
                  by_loss, outcomes.count(None) / 400)
