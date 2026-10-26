"""The director's triggers (docs/director.md, stage 3, layer 1): an event, a condition and a program.

A trigger is a graph_patch operation (`trigger`, protocol v35) that the bridge counts: it has the engine's
events and runs the programs, with the server or without it. The events:
- `state` — the condition, computed four times a second; self: the trigger's npc or the hero, other: the hero;
- `death` — somebody died; self: the dead (the trigger's npc, if it names one), other: the killer, if any;
- `dialog` — a dialog or a choice ran (`node`, of the trigger's npc); self: the npc, other: the hero.

Every trigger counts how many times it fired in a story variable of the director's by its id (DIR_...): the
save keeps it. One that fires once (the default) fires while its condition holds and the count is 0, before a
save as after a load; one that repeats fires whenever its condition comes true. Conditions and programs read
the count as a variable (`fired`).

Here: triggers as data, the conditions the director's code puts into them, and what the server does when one
fires — a fact of the chronicle, `triggered`. How far a story got is in the save, not in the messages: after a
load or a lost connection the code reads the counts (story_state).
"""

from __future__ import annotations

import logging
from typing import Any

from .chronicle import Chronicle
from .game import GameLink
from .gamedata.storygraph import Expr
from .protocol import GameMessage, TriggerFired

log = logging.getLogger(__name__)

Op = dict[str, Any]
Step = dict[str, Any]

STATE, DEATH, DIALOG = "state", "death", "dialog"
HERO: Expr = ["inst", "HERO"]


def trigger(
    id: str,
    on: str,
    steps: list[Step],
    *,
    when: Expr | None = None,
    npc: str | None = None,
    node: str | None = None,
    repeat: bool = False,
    world: str | None = None,
) -> Op:
    """The graph_patch operation of a trigger."""
    op: Op = {"op": "trigger", "id": id.upper(), "on": on, "steps": steps}
    if when is not None:
        op["when"] = when
    if npc:
        op["npc"] = npc.upper()
    if node:
        op["node"] = node.upper()
    if repeat:
        op["repeat"] = True
    if world:
        op["world"] = world.upper()
    return op


def removal(id: str) -> Op:
    """The operation that takes a trigger away; its count stays in the save."""
    return {"op": "trigger", "id": id.upper(), "steps": None}


# --- conditions --------------------------------------------------------------------------------


def npc(symbol: str) -> Expr:
    """An npc by its script symbol, as the scripts get it (Hlp_GetNpc)."""
    return ["call", "HLP_GETNPC", ["sym", symbol.upper()]]


def fired(id: str) -> Expr:
    """The trigger has fired."""
    return [">=", ["var", id.upper()], ["int", 1]]


def hero_near_point(point: str, cm: int) -> Expr:
    """The hero is within `cm` of a waypoint."""
    return ["<", ["call", "NPC_GETDISTTOWP", HERO, ["str", point.upper()]], ["int", cm]]


def hero_near(symbol: str, cm: int) -> Expr:
    """The hero is within `cm` of an npc."""
    return ["<", ["call", "NPC_GETDISTTONPC", HERO, npc(symbol)], ["int", cm]]


def hero_has(item: str, count: int = 1) -> Expr:
    """The hero has at least `count` of an item."""
    return [">=", ["call", "NPC_HASITEMS", HERO, ["sym", item.upper()]], ["int", count]]


def dead(symbol: str) -> Expr:
    """The npc is dead."""
    return ["call", "NPC_ISDEAD", npc(symbol)]


def day_reached(day: int) -> Expr:
    """The game's day `day` has come (the first day is 0)."""
    return [">=", ["call", "WLD_GETDAY"], ["int", day]]


def time_between(start: tuple[int, int], end: tuple[int, int]) -> Expr:
    """The clock is between two times of the day, as Wld_IsTime counts it (over midnight too)."""
    return ["call", "WLD_ISTIME", ["int", start[0]], ["int", start[1]], ["int", end[0]], ["int", end[1]]]


def all_of(*exprs: Expr) -> Expr:
    """All the conditions hold; one that is itself an "and" of others gives those."""
    flat: list[Expr] = []
    for e in exprs:
        flat += e[1:] if e[:1] == ["&&"] else [e]
    return flat[0] if len(flat) == 1 else ["&&", *flat]


class Triggers:
    """What the server does when a trigger fires: a fact of the chronicle, for the director and the npc's."""

    def __init__(self, link: GameLink, chronicle: Chronicle) -> None:
        self.link = link
        self.chronicle = chronicle
        link.listeners.append(self._on_message)

    async def _on_message(self, msg: GameMessage) -> None:
        if isinstance(msg, TriggerFired):
            fact: dict[str, Any] = {"fact": "triggered", "id": msg.id, "count": msg.count}
            if msg.npc:
                fact["npc"] = msg.npc
            if msg.other:
                fact["other"] = msg.other
            if msg.error:
                fact["error"] = msg.error
                log.warning("trigger %s: its program stopped: %s", msg.id, msg.error)
            await self.chronicle.record(fact)

    def fired(self, id: str) -> int:
        """How many times the trigger fired, as the branch's chronicle has it (the save has the truth)."""
        id = id.upper()
        counts = [int(f.get("count", 0)) for f in self.chronicle.facts("triggered") if f.get("id") == id]
        return max(counts, default=0)
