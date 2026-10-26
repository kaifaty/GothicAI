"""Where a place or someone is, as the npc knows it: the `where` tool (docs/npc-tools.md М3).

A place: of what kind, how far, which way from the hero, where its ways in are, what lies next to it and whom
the npc saw there lately. Someone: where the npc sees him now or saw him last (sightings.py), and that place
as above. Where places are and of what kind, the book of places says (places.py); whom it saw, the npc's own
memory.

The place is looked for by its id («Места рядом»), by its name («таверна»), or by its kind («пещера»): the
nearest of the kind within WHERE_CM.
"""

from __future__ import annotations

import logging
from collections.abc import Sequence
from typing import TYPE_CHECKING

from .game import GameLink
from .knowledge import roots
from .llm import Tool, ToolCall
from .lore import Lore, npc_pos
from .places import KINDS, Place, PlaceBook
from .protocol import Snapshot
from .sightings import HERO, Sightings, ago
from .turn import ToolOutcome

if TYPE_CHECKING:
    from .dialogue import Conversation, DialogueManager
    from .perception import PerceptionDesk

log = logging.getLogger(__name__)

WHERE = "where"
WHERE_CM = 20000.0  # a place named or of a kind is looked for this near (200 m)
SEEN_THERE = 4  # whom it saw in a place, at most
WHERE_TOOL = Tool(
    WHERE,
    "Вспомнить, где место или человек: какого рода место, далеко ли и в какой стороне от героя, где в него "
    "вход, что рядом, кого ты там видел; где ты видел человека последний раз.",
    {
        "type": "object",
        "properties": {
            "what": {
                "type": "string",
                "description": "место — id из «Мест рядом», название («таверна») или род («пещера»); или кто "
                "— имя, «бандиты»",
            }
        },
        "required": ["what"],
        "additionalProperties": False,
    },
)
# the kinds by the roots of their Russian words: «пещеру» -> cave
_KIND_ROOTS = {r: kind for kind, (ru, _) in KINDS.items() for r in roots(ru)}


class Whereabouts:
    """The `where` tool of the conversational model (a TurnExtension)."""

    def __init__(
        self,
        link: GameLink,
        book: PlaceBook | None,
        *,
        sightings: Sightings | None = None,
        desk: PerceptionDesk | None = None,
        lore: Lore | None = None,
    ) -> None:
        self.link = link
        self.book = book
        self.sightings = sightings
        self.desk = desk
        self.lore = lore  # what it knows of places and people from its dialogs (lore.py)

    # --- TurnExtension ----------------------------------------------------------------------------

    all_tools: tuple[Tool, ...] = (WHERE_TOOL,)

    async def prepare(self, npc: str) -> None:
        if self.sightings is not None:
            await self.sightings.prepare(npc)

    def situation(self, npc: str) -> str | None:
        return None

    def tools(self, npc: str) -> list[Tool]:
        return [WHERE_TOOL] if self.book is not None else []

    async def handle(
        self, dialogue: DialogueManager, conv: Conversation, call: ToolCall, snap: Snapshot | None
    ) -> ToolOutcome | None:
        if call.name != WHERE:
            return None
        await self.prepare(conv.npc)
        what = str(call.arguments.get("what", "")).strip()
        answer = self.where(conv.npc, what)
        log.info("%s: where %r -> %s", conv.npc, what, answer.split("\n", 1)[0])
        return ToolOutcome(answer, "read")

    # --- where ------------------------------------------------------------------------------------

    def where(self, npc: str, what: str) -> str:
        """The answer to `where{what}` for the npc."""
        if self.book is None:
            return "Ты не знаешь здешних мест."
        if not what:
            return "Скажи, какое место или кого ищешь."
        here = npc_pos(npc, self.link, self.book, self.desk)
        if (place := self.book.get(what)) is not None:
            return self.place(npc, place, here)
        if (person := self.person(npc, what, here)) is not None:
            return person
        if (place := self.named(what, here)) is not None:
            return self.place(npc, place, here)
        return f"Ты не знаешь поблизости такого места: «{what}». И не видел недавно никого с таким именем."

    def place(self, npc: str, place: Place, here: Sequence[float] | None) -> str:
        """A place in words: its kind, how far and which way, its ways in, what is next to it, whom the npc
        saw there."""
        assert self.book is not None
        if here is not None:
            hero, ahead, right = self._hero()
            text = self.book.line_ru(place, self.book.distance(place, here), hero, ahead, right, mine=True)
        else:
            text = f"{place.id} — {place.ru}: {place.kind_ru}"
            if ways := self.book.ways_in_ru(place):
                text += f"; {ways}"
        lines = [text + ("; под крышей." if place.inside else ".")]
        near = [n for n in (self.book.get(i) for i in place.neighbours) if n is not None]
        if near:
            lines.append("Рядом: " + "; ".join(f"{n.ru} ({n.id})" for n in near[:4]) + ".")
        if seen := self._seen_in(npc, place):
            lines.append("Ты там видел: " + "; ".join(seen) + ".")
        if self.lore is not None and (known := self.lore.of_place(npc, place.id)):
            lines.append("Ты знаешь: " + " ".join(f.ru for f in known))
        return "\n".join(lines)

    def person(self, npc: str, what: str, here: Sequence[float] | None) -> str | None:
        """Where the npc sees or last saw whom `what` names; None: nobody it saw is called so."""
        q = what.lower()
        words = roots(what)
        view = self.desk.view(npc) if self.desk is not None else None
        for o in view.sees if view is not None else ():
            if _names(q, words, o.name):
                place = self.book.of(o.waypoint) if self.book is not None and o.waypoint else None
                at = f" — {place.where_ru} ({place.id})" if place is not None else ""
                return f"{o.name} у тебя на виду, в {round(o.distance / 100)} м{at}."
        for s in reversed(self.sightings.mind(npc).sightings if self.sightings is not None else []):
            hit = "геро" in q if s.who == HERO else _names(q, words, s.name)
            if not hit:
                continue
            who = "героя" if s.who == HERO else s.name
            line = f"Последний раз ты видел {who} {ago(max(0, self.sightings.now - s.end))}"
            place = self.book.get(s.place_ids[-1]) if self.book is not None and s.place_ids else None
            if place is None:
                return line + (f" — {s.places[-1]}." if s.places else ".")
            return line + f" — {place.where_ru}.\n" + self.place(npc, place, here)
        return self._known(npc, q, words, here)

    def _known(self, npc: str, q: str, words: set[str], here: Sequence[float] | None) -> str | None:
        """Whom it has not seen but knows of from its dialogs (lore.py): «где Ватрас?»."""
        if self.lore is None:
            return None
        for f in self.lore.known(npc):
            for who in f.who:
                if not _names(q, words, self.lore.names.get(who, "")):
                    continue
                facts = self.lore.of_person(npc, who)
                text = "Сам ты его недавно не видел, но знаешь: " + " ".join(x.ru for x in facts)
                place = next((self.book.get(x.place) for x in facts if x.place), None) if self.book else None
                return text + ("\n" + self.place(npc, place, here) if place is not None else "")
        return None

    def named(self, what: str, here: Sequence[float] | None) -> Place | None:
        """The place `what` names, the nearest that fits best; or the nearest of the kind it names (a lead
        of a deal too, guide.py)."""
        assert self.book is not None
        words = roots(what)
        places = self.book.near(here, WHERE_CM, len(self.book.places)) if here is not None else []
        best: tuple[int, float, Place] | None = None
        for p, d in places:
            score = len(words & roots(p.ru))
            if score and (best is None or (score, -d) > (best[0], -best[1])):
                best = (score, d, p)
        kind = next((_KIND_ROOTS[w] for w in words if w in _KIND_ROOTS), None)
        if kind is not None and here is not None:
            # «пещера»: the nearest cave, unless the words name one place better than by its kind alone
            nearest = self.book.nearest(here, kind, WHERE_CM)
            if nearest is not None and (best is None or best[0] <= 1 or best[2].kind != kind):
                return nearest[0]
        return best[2] if best is not None else None

    # --- helpers ----------------------------------------------------------------------------------

    def _seen_in(self, npc: str, place: Place) -> list[str]:
        if self.sightings is None:
            return []
        out: list[str] = []
        told: set[str] = set()
        for s in reversed(self.sightings.mind(npc).sightings):
            if place.id not in s.place_ids or s.who in told:
                continue
            told.add(s.who)
            who = "героя" if s.who == HERO else (f"тело: {s.name}" if "dead" in s.notable else s.name)
            out.append(f"{who} — {ago(max(0, self.sightings.now - s.end))}")
            if len(out) >= SEEN_THERE:
                break
        return out

    def _hero(self) -> tuple[Sequence[float] | None, Sequence[float] | None, Sequence[float] | None]:
        player = self.link.player
        if player is None:
            return None, None, None
        return player.pos, player.ahead, player.right


def _names(q: str, words: set[str], name: str) -> bool:
    """Whether the words name him: by his name («Браго»), or by his look («бандиты»)."""
    low = name.lower()
    if len(low) >= 3 and low[:4] in q:
        return True
    return any(w in words for w in roots(name) if len(w) >= 4)
