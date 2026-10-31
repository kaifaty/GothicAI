"""Between the conversational model and the npc's body: intents, checks and the game actions they need.

The conversational model never moves the body. It sets an *intent* (what the npc wants now, how it
treats the hero, what it wants from the hero) and makes commitments (give an item). Here they are
checked against what the npc actually has and sees, before anything reaches the game: a rejection goes
straight back to the model within the same turn. What passes becomes game actions, sent after the
npc's line; reactions in the moment (gestures, fleeing...) belong to the behavioral model (brain/).
"""

from __future__ import annotations

import json
from collections.abc import Callable
from dataclasses import asdict, dataclass, replace
from typing import Any

from .protocol import Snapshot

GOALS = {
    "none": "ничего особенного",
    "follow_hero": "идти за героем",
    "flee": "бежать от опасности",
    "seek_help": "искать помощи",
    "fight": "драться",
    "guard_hero": "защищать героя",
    "keep_routine": "заниматься своими делами",
}
GOALS_EN = {
    "none": "nothing in particular",
    "follow_hero": "to follow the hero",
    "flee": "to get away from danger",
    "seek_help": "to find help",
    "fight": "to fight",
    "guard_hero": "to protect the hero",
    "keep_routine": "to go about your own business",
}
ATTITUDES = {
    "hostile": "враждебное",
    "wary": "настороженное",
    "neutral": "ровное",
    "friendly": "дружеское",
    "devoted": "преданное",
}
# goals that only make sense while something is going on: dropped by code when the incident is over
INCIDENT_GOALS = frozenset({"flee", "seek_help", "fight", "guard_hero"})
# goals of an npc that goes with the hero: it follows him (and stands up for him, orders.py)
COMPANION_GOALS = frozenset({"follow_hero", "guard_hero"})
AGENDA_MAX = 120
BODY_TARGETS = frozenset({"fight"})  # goals whose target is someone, never a place


@dataclass(frozen=True)
class Intent:
    goal: str = "none"
    target: str | None = None
    attitude: str = "neutral"
    agenda: str = ""  # English, for the behavioral model: what the npc wants from the hero or plans

    def to_json(self) -> str:
        return json.dumps(asdict(self), ensure_ascii=False)

    @classmethod
    def from_json(cls, text: str) -> Intent:
        try:
            data = json.loads(text)
        except json.JSONDecodeError:
            return cls()
        fields = {k: data[k] for k in ("goal", "target", "attitude", "agenda") if k in data}
        if fields.get("goal") not in GOALS:
            fields["goal"] = "none"  # a goal there is no more (change_plans): it would read as its symbol
        return cls(**fields)

    def describe_en(self) -> str:
        """English, for the behavioral model's state."""
        target = "the hero" if self.target == "PLAYER" else self.target
        parts = [f"You want {GOALS_EN.get(self.goal, self.goal)}{f' ({target})' if target else ''}."]
        parts.append(f"Your attitude toward the hero is {self.attitude}.")
        if self.agenda:
            parts.append(f"What you want from the hero: {self.agenda}")
        return " ".join(parts)

    def describe(self) -> str:
        """Russian, for the conversational model's situation block."""
        goal = GOALS.get(self.goal, self.goal)
        parts = [f"цель — {goal}{f' ({self.target})' if self.target else ''}"]
        parts.append(f"отношение к герою — {ATTITUDES.get(self.attitude, self.attitude)}")
        if self.agenda:
            parts.append(f"agenda — {self.agenda}")
        return "; ".join(parts)


def targets(snap: Snapshot | None) -> dict[str, str]:
    """Who and where an intent may point to: id -> what it is (for the model and for the check)."""
    out = {"PLAYER": "герой"}
    if snap is None:
        return out
    for o in snap.nearby_npcs:
        if not o.dead:
            out[o.id] = f"{o.name}, {max(1, round(o.distance / 100))} м"
    for wp in snap.nearby_waypoints:
        out.setdefault(wp.name, f"точка, {max(1, round(wp.distance / 100))} м")
    for wp in (snap.npc.waypoint, snap.npc.nearest_waypoint):
        if wp:
            out.setdefault(wp, "точка, где ты стоишь")
    return out


def parse_intent(args: dict[str, Any], current: Intent | None = None) -> Intent:
    """The model's set_intent arguments; an agenda of null keeps the current one."""
    target = args.get("target")
    agenda = args.get("agenda")
    return Intent(
        goal=str(args.get("goal") or "none"),
        target=str(target).strip() or None if target else None,
        attitude=str(args.get("attitude") or "neutral"),
        agenda=(current.agenda if current else "") if agenda is None else str(agenda).strip()[:AGENDA_MAX],
    )


def check_intent(
    intent: Intent, snap: Snapshot | None, seen: dict[str, str] | None = None,
    *, fight_reason: Callable[[], str | None] | None = None,
) -> tuple[Intent | None, str | None]:
    """The intent as it will be carried out, or why not. `seen`: whom it may point to, as the npc perceives
    them (perception.py); else the snapshot's people and points around."""
    if intent.goal not in GOALS:
        return None, f"Нет такой цели: {intent.goal}. Можно: {', '.join(GOALS)}."
    if intent.attitude not in ATTITUDES:
        return None, f"Нет такого отношения: {intent.attitude}. Можно: {', '.join(ATTITUDES)}."
    if intent.goal == "seek_help" and intent.target is None:
        intent = replace(intent, target="PLAYER")
    if intent.target is not None:
        allowed = dict(seen) if seen else targets(snap)
        if intent.goal in BODY_TARGETS and not seen:  # someone to fight, not a place
            allowed = {k: v for k, v in allowed.items() if k == "PLAYER" or k in npc_ids(snap)}
        match = next((t for t in allowed if t.upper() == intent.target.upper()), None)
        if match is None:
            listed = ", ".join(f"{k} ({v})" for k, v in allowed.items())
            return None, f"Цели {intent.target} рядом нет. Можно указать: {listed}."
        intent = replace(intent, target=match)
    if intent.goal == "fight" and intent.target == "PLAYER" and fight_reason is not None:
        if reason := fight_reason():
            return None, reason
    return intent, None


def npc_ids(snap: Snapshot | None) -> set[str]:
    return {o.id for o in snap.nearby_npcs if not o.dead} if snap is not None else set()


def check_give(args: dict[str, Any], snap: Snapshot | None) -> tuple[dict[str, Any] | None, str | None]:
    """give_item arguments as they will be sent (count capped by what the npc has), or why not."""
    item = str(args.get("item", "")).strip().upper()
    try:
        count = int(args.get("count") or 1)
    except (TypeError, ValueError):
        count = 1
    if snap is None:  # nothing to check against: the game checks
        return {"item": item, "count": max(1, count)}, None
    have = {i.item: i for i in snap.npc.inventory if not i.equipped and i.count > 0}
    if item not in have:
        listed = "; ".join(f"{i.name} ({i.item}) ×{i.count}" for i in have.values()) or "пусто"
        return None, f"У тебя нет предмета {item}. Твой инвентарь: {listed}."
    return {"item": item, "count": min(max(1, count), have[item].count)}, None


Action = tuple[str, dict[str, Any]]  # a game action (`action` message) and its arguments


def intent_actions(old: Intent, new: Intent, following: bool) -> list[Action]:
    """What the body does at once when the intent changes. Fleeing is the behavioral model's reaction; a
    fight with someone named starts here (a companion's, orders.py)."""
    acts: list[Action] = []
    if new.goal in COMPANION_GOALS:
        if not following:
            acts.append(("follow", {"npc": "PLAYER"}))
    elif following:
        acts.append(("stop", {}))
    if new.goal == "seek_help" and new.target and new.target != old.target:
        acts.append(("go_to_npc", {"npc": new.target}))
    if new.goal == "fight" and new.target and (old.goal, old.target) != (new.goal, new.target):
        acts.append(("fight", {"npc": new.target}))
    return acts


def describe_targets(snap: Snapshot | None, seen: dict[str, str] | None = None) -> str:
    return ", ".join(f"{k} ({v})" for k, v in (seen or targets(snap)).items())


def same(a: Intent, b: Intent) -> bool:
    return asdict(a) == asdict(b)
