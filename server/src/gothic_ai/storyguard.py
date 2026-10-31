"""What an npc's own initiative may not do to the plot (docs/npc-tools.md, wave 2).

An npc that walks up to the hero by itself leaves its spot, and some must not: the plot's npc's (the same
rules the director keeps to), the guards of a passage, whose dialogs warn and attack by where SELF stands,
and whoever the scripts have leading or following the hero right now. They may still speak when the hero
is close. Posts and escorts are not held for a scene either (gossip.py): a held npc runs no routine, so a
guard would stop guarding, and its release resumes the routine, not the escort. Works without the director;
without a story graph the plot is known by instance prefixes only.
"""

from __future__ import annotations

from collections.abc import Iterable

from .gamedata.storygraph import StoryGraph, walk
from .validator import PLOT_NPCS, Rules

# the scripts' guard-passage idiom: FIRSTWARN, SECONDWARN, ATTACK by SELF's nearest waypoint
POST_CALL = "NPC_GETNEARESTWP"
# all a dialog may read of where SELF stands; too broad to block by (Vino's healing reads it)
SELF_PLACE_CALLS = ("NPC_GETDISTTOWP", "NPC_GETNEARESTWP", "NPC_GETDISTTONPC")
POST_STATES = ("ZS_GUARD_PASSAGE",)  # TA_GUARD_PASSAGE
# ZS_GUIDE_PLAYER, ZS_FOLLOW_PLAYER: the scripts have it lead or follow the hero now (the roster's state)
ESCORT_STATES = ("ZS_GUIDE", "ZS_FOLLOW")
# every sleep entry of the routines is ZS_GOTOBED; ZS_SLEEP and ZS_SLEEP_GROUND are the scripts' own
SLEEP_STATES = ("ZS_SLEEP", "ZS_GOTOBED")


def asleep(state: str) -> bool:
    """The npc sleeps: `state` is its script state from the roster."""
    return state.upper().startswith(SLEEP_STATES)


class StoryGuard:
    """Whose body stays where it is."""

    def __init__(self, graph: StoryGraph | None) -> None:
        self.graph = graph
        self.rules = Rules.of(graph) if graph is not None else None
        self.posts = set(_readers(graph, (POST_CALL,))) if graph is not None else set()

    def plot(self, npc: str) -> bool:
        if self.rules is not None:
            return self.rules.plot_npc(npc)
        return npc.upper().startswith(PLOT_NPCS)

    def body(self, npc: str, state: str = "") -> str | None:
        """Why the npc must stay where it is (Russian, for the journal and the console), or None if it may
        walk. `state` is its script state from the roster."""
        if self.plot(npc):
            return "сюжетный: только слова"
        return self.on_duty(npc, state)

    def harm(
        self, npc: str, *, target: str = "", state: str = "", target_state: str = "",
        item: str = "", mission: bool = False,
    ) -> str | None:
        """Why a marked physical deed must not start; both bodies and the real item are protected."""
        for who, current in ((npc, state), (target, target_state)):
            if not who:
                continue
            if who.upper() in {"PLAYER", "HERO", "PC_HERO"}:
                return "дела NPC против героя не поддерживаются"
            if reason := self.body(who, current):
                return f"{who.upper()}: {reason}"
        if mission:
            return f"{item.upper()}: вещь для задания"
        if item and self.graph is not None:
            # Same exact holder checks as errands, rather than treating ordinary bread or money as
            # plot property merely because some quest mentions them.
            from .errands import checked_items

            holders = checked_items(self.graph).get(item.upper(), set())
            if any(who.upper() in holders for who in (npc, target) if who):
                return f"{item.upper()}: вещь должна остаться у своего сюжетного владельца"
        return None

    def on_duty(self, npc: str, state: str = "") -> str | None:
        """Why the npc's body is the scripts' right now (Russian): at its post, or leading or following the
        hero; else None. Such a body is never held, while a plot npc that is not on duty may be."""
        state = state.upper()
        if npc.upper() in self.posts or state.startswith(POST_STATES):
            return "стоит на посту"
        if state.startswith(ESCORT_STATES):
            return "ведёт или сопровождает героя по скриптам"
        return None

    def placed(self) -> dict[str, list[str]]:
        """Every npc whose dialogs read where SELF stands, with those dialogs: for review by hand (console
        `guard`), it blocks nothing."""
        if self.graph is None:
            return {}
        return dict(sorted(_readers(self.graph, SELF_PLACE_CALLS).items()))


def _readers(graph: StoryGraph, calls: Iterable[str]) -> dict[str, list[str]]:
    """npc -> its dialogs whose condition calls one of `calls` on SELF."""
    wanted = set(calls)
    out: dict[str, list[str]] = {}
    for n in graph.nodes.values():
        if n.is_choice:
            continue
        for x in walk(n.condition):
            if x[0] == "call" and x[1] in wanted and x[2:3] == [["inst", "SELF"]]:
                out.setdefault(n.npc, []).append(n.id)
                break
    return out
