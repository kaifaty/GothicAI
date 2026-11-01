"""Traces whose first sight is an event of its own (stage 8 step 4б, reports/physics/2026-11-01-traces).

Sightings (sightings.py) keeps the first observation of every visible trace (`TRACE:<id>:<kind>`), once
per npc and trace and in the save branch's memory. For the kinds in `FOUND` that first observation is
also a find:
- a stimulus `trace_found` for the planner: worth a decision, not urgent (brain/stimuli.py);
- what the npc makes of it, added to its line in «Следы, которые ты видел» (the conversation);
- for a body (v58): suspicion by the chest's rule (crimes.SUSPECT_MIN). The finder remembers whom it saw
  at the same place within half an hour of the death (`view.time - age`); each is suspected of murder,
  the hero through Crimes (`crime_suspected`), people through NpcCrimes (`npc_crime_suspected`), both
  with crime "murder" and the body. Never the dead, the finder, a trace or another body. No suspicion
  without the time of death or a place, nor when the finder saw the killing itself.

Only `body` is a find now; blood and missing things are later kinds of the same event. Who killed is
the engine's truth and never comes here: a find is the knowledge «someone was killed here».
"""

from __future__ import annotations

import logging
import time
from collections.abc import Callable, Iterable
from dataclasses import dataclass, field
from typing import TYPE_CHECKING

from .chronicle import Fact
from .journal import Journal
from .protocol import NpcView, Trace

if TYPE_CHECKING:
    from .brain.planner import NpcPlanner
    from .crimes import Crimes
    from .npc_crimes import NpcCrimes
    from .sightings import Sighting, Sightings

log = logging.getLogger(__name__)

FOUND = frozenset({"body"})  # trace kinds whose first sight is a find
HERO = "PLAYER"
SUSPICIONS = ("crime_suspected", "npc_crime_suspected")


@dataclass(frozen=True)
class Found:
    """One npc's find of one trace."""

    npc: str
    trace: str  # the trace's id
    kind: str
    name: str
    place: str  # the book's id, "" when unknown
    body: str = ""  # body: the dead npc's id
    died: int | None = None  # body: game minutes since the first day began; None when unknown
    suspects: tuple[str, ...] = field(default_factory=tuple)  # whom it suspects for it, by evidence


class FoundTraces:
    """The finds of npc's: stimuli, suspicion and what each knows of them."""

    def __init__(
        self,
        sightings: Sightings,
        *,
        crimes: Crimes | None = None,
        npc_crimes: NpcCrimes | None = None,
        journal: Journal | None = None,
        kinds: Iterable[str] = FOUND,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        self.sightings = sightings
        self.chronicle = sightings.chronicle
        self.crimes = crimes
        self.npc_crimes = npc_crimes
        self.journal = journal or Journal()
        self.kinds = frozenset(kinds)
        self.clock = clock
        self.planner: NpcPlanner | None = None
        self.on_found: list[Callable[[Found], None]] = []
        sightings.on_trace.append(self.found)
        sightings.trace_note = self.note

    def follow(self, planner: NpcPlanner) -> None:
        """Free npc's decide what to do about a find (without the planner it is only known)."""
        self.planner = planner

    def found(self, viewer: str, trace: Trace, s: Sighting, view: NpcView) -> None:
        """The first sight of a trace by an npc (Sightings.on_trace): once per npc and trace."""
        if trace.kind not in self.kinds:
            return
        viewer = viewer.upper()
        body = s.trace_npc if trace.kind == "body" else ""
        died = None  # the time of death, by the engine's age of the body: unknown without either
        if body and trace.age is not None and view.time is not None:
            died = view.time - trace.age
        place = s.place_ids[0] if s.place_ids else ""
        suspicions = self._suspect(viewer, trace.id, body, died, place) if body else []
        find = Found(
            viewer,
            trace.id,
            trace.kind,
            s.name,
            place,
            body,
            died,
            tuple(dict.fromkeys(str(f["suspect"]) for f in suspicions)),
        )
        log.info("%s found %s %s at %s; suspects %s", viewer, trace.kind, s.name, place, find.suspects)
        self.journal.add(
            "trace_found",
            npc=viewer,
            trace=trace.id,
            trace_kind=trace.kind,
            name=s.name,
            body=body,
            place=place,
            died=died,
            suspects=list(find.suspects),
        )
        if self.planner is not None:
            self.planner.on_event(
                viewer,
                "trace_found",
                self.clock(),
                other=body or trace.id,
                other_name=s.name if s.known and s.name != "тело" else "a stranger",
                distance_cm=trace.distance,
                detail=_since(trace),
                place=trace.waypoint,
                context=self._context(find, suspicions),
            )
        for listener in self.on_found:
            try:
                listener(find)
            except Exception:
                log.exception("a find listener failed")

    def _suspect(self, npc: str, trace: str, body: str, died: int | None, place: str) -> list[Fact]:
        if died is None or not place or self.witnessed(npc, body):
            return []  # no time of death or place is better than accusing everyone seen that day
        out: list[Fact] = []
        if self.crimes is not None:
            out += self.crimes.suspect_body(npc, body, trace, died=died, place=place)
        if self.npc_crimes is not None:
            out += self.npc_crimes.suspect_body(npc, body, trace, died=died, place=place)
        return out

    def witnessed(self, npc: str, body: str) -> bool:
        """It saw the killing itself: a deed of murder of this body it witnessed, the hero's or an npc's."""
        body = body.upper()
        seen = [*(self.crimes.seen(npc) if self.crimes is not None else [])]
        seen += self.npc_crimes.seen(npc) if self.npc_crimes is not None else []
        return any(f.get("kind") == "murder" and str(f.get("victim", "")).upper() == body for f in seen)

    def suspicions(self, npc: str, trace: str) -> list[Fact]:
        """Its suspicions for the find of this trace, the hero's and the people's."""
        npc = npc.upper()
        return [
            f
            for kind in SUSPICIONS
            for f in self.chronicle.facts(kind)
            if f.get("npc") == npc and f.get("crime") == "murder" and f.get("trace") == trace
        ]

    def note(self, npc: str, s: Sighting) -> str:
        """What the npc makes of a find, after its line in «Следы, которые ты видел»."""
        if s.trace_kind != "body" or "body" not in self.kinds:
            return ""
        if s.trace_npc and self.witnessed(npc, s.trace_npc):
            return "; здесь его убили, ты видел это сам"
        names = list(dict.fromkeys(_name(f) for f in self.suspicions(npc, trace_id(s))))
        if not names:
            return "; здесь кого-то убили, кто это сделал — не знаешь"
        return (
            f"; здесь кого-то убили. Около того времени ты сам видел здесь: {', '.join(names)} — "
            "это подозрение, не доказательство"
        )

    def _context(self, find: Found, suspicions: list[Fact]) -> str:
        """The find for the behavioral model (English), after the stimulus' own words."""
        if find.kind != "body":
            return ""
        if find.body and self.witnessed(find.npc, find.body):
            return " You saw the killing yourself."
        if not suspicions:
            return " You do not know who did it."
        names = dict.fromkeys(
            "the hero" if f.get("suspect") == HERO else str(f.get("sighting", {}).get("name") or f["suspect"])
            for f in suspicions
        )
        return f" Around that time you saw {', '.join(names)} here; a suspicion, not proof."


def trace_id(s: Sighting) -> str:
    """The trace's id of its observation (`TRACE:<id>:<kind>`)."""
    return s.who.removeprefix("TRACE:").removesuffix(f":{s.trace_kind}")


def _name(fact: Fact) -> str:
    if fact.get("suspect") == HERO:
        return "герой"
    return str(fact.get("sighting", {}).get("name") or fact.get("suspect", ""))


def _since(trace: Trace) -> str:
    if trace.kind != "body":
        return f"{trace.age} minutes old" if trace.age is not None else "of unknown age"
    if trace.age is None:
        return "you cannot tell how long ago they died"
    if trace.age < 90:
        return f"dead for about {max(5, round(trace.age / 5) * 5)} minutes"
    hours = round(trace.age / 60)
    return "dead for about two hours" if hours == 2 else f"dead for about {hours} hours"
