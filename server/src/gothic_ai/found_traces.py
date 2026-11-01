"""Traces whose first sight is an event of its own (stage 8 step 4б, reports/physics/2026-11-01-traces).

Sightings (sightings.py) keeps the first observation of every visible trace (`TRACE:<id>:<kind>`), once
per npc and trace and in the save branch's memory. For the kinds in `FOUND` that first observation is
also a find:
- a stimulus `trace_found` for the planner at once: worth a decision, not urgent (brain/stimuli.py);
- what the npc makes of it, added to its line in «Следы, которые ты видел» (the conversation);
- for a body (v58): suspicion by the chest's rule (crimes.SUSPECT_MIN). The finder remembers whom it saw
  at the same place within half an hour of the death (`view.time - age`); each is suspected of murder,
  the hero through Crimes (`crime_suspected`), people through NpcCrimes (`npc_crime_suspected`), both
  with crime "murder" and the body. Never the dead, the finder, a trace or another body. No suspicion
  without the time of death or a place, when the finder saw the killing itself (a murder deed it
  witnessed) or when it was there when he died (it saw him alive at that place until `PRESENT_MIN`
  before the death: an npc's killing of an npc has no deed, and the killer must not suspect others).

Suspicion is decided later than the find, on a later view of the finder at least `JUDGE_S` real seconds
after it: the hero's deed (`hero_deed` → `crime_seen`) may come after the view with the body. A
`trace_judged` fact marks the decision in the save branch, so a find saved before its decision is
decided after the load, and none is decided twice.

Only `body` is a find now; blood and missing things are later kinds of the same event. Who killed is
the engine's truth and never comes here: a find is the knowledge «someone was killed here».
"""

from __future__ import annotations

import logging
import time
from collections.abc import Callable, Iterable
from dataclasses import dataclass
from typing import TYPE_CHECKING

from .chronicle import Fact
from .journal import Journal
from .protocol import GameMessage, Loading, NpcView, Session, Trace

if TYPE_CHECKING:
    from .brain.planner import NpcPlanner
    from .crimes import Crimes
    from .npc_crimes import NpcCrimes
    from .sightings import Sighting, Sightings

log = logging.getLogger(__name__)

FOUND = frozenset({"body"})  # trace kinds whose first sight is a find
HERO = "PLAYER"
SUSPICIONS = ("crime_suspected", "npc_crime_suspected")
JUDGED = "trace_judged"
JUDGE_S = 2.0  # real seconds from a find to its suspicion: the deed the finder witnessed comes first
PRESENT_MIN = 5  # it saw the victim alive there until this many game minutes before the death: it was there


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
        self.on_judged: list[Callable[[Found, list[Fact]], None]] = []
        self._found_at: dict[tuple[str, str], float] = {}  # (npc, trace) -> when found, in this process
        self._judged: set[tuple[str, str]] = set()  # decided in this branch, as far as read
        sightings.on_trace.append(self.found)
        sightings.trace_note = self.note
        # after Sightings.saw: a view first records its new traces, then decides the older finds
        sightings.desk.on_view.append(self.viewed)
        sightings.link.listeners.append(self._on_message)

    def follow(self, planner: NpcPlanner) -> None:
        """Free npc's decide what to do about a find (without the planner it is only known)."""
        self.planner = planner

    def _on_message(self, msg: GameMessage) -> None:
        if isinstance(msg, Session | Loading):
            self._judged.clear()  # another branch: its own decisions, read from its facts
            self._found_at.clear()

    def found(self, viewer: str, trace: Trace, s: Sighting, view: NpcView) -> None:
        """The first sight of a trace by an npc (Sightings.on_trace): once per npc and trace."""
        if trace.kind not in self.kinds:
            return
        viewer = viewer.upper()
        find = self._find(viewer, s)
        self._found_at[(viewer, trace.id)] = self.clock()
        log.info("%s found %s %s at %s", viewer, trace.kind, s.name, find.place)
        self.journal.add(
            "trace_found",
            npc=viewer,
            trace=trace.id,
            trace_kind=trace.kind,
            name=s.name,
            body=find.body,
            place=find.place,
            died=find.died,
        )
        if self.planner is not None:
            self.planner.on_event(
                viewer,
                "trace_found",
                self.clock(),
                other=find.body or trace.id,
                other_name=s.name if s.known and s.name != "тело" else "a stranger",
                distance_cm=trace.distance,
                detail=_since(trace),
                place=trace.waypoint,
                context=self._context(find),
            )
        for listener in self.on_found:
            try:
                listener(find)
            except Exception:
                log.exception("a find listener failed")

    def viewed(self, view: NpcView) -> None:
        """A later view of a finder: its finds old enough are decided (once per branch)."""
        if self.sightings.branch is None:
            return
        npc = view.npc.upper()
        now = self.clock()
        for s in list(self.sightings.mind(npc).traces.values()):
            if s.trace_kind not in self.kinds:
                continue
            trace = trace_id(s)
            key = (npc, trace)
            if key in self._judged:
                continue
            if self._decided(npc, trace):
                self._judged.add(key)
                continue
            found_at = self._found_at.get(key)  # None: found before a load or a restart, long enough ago
            if found_at is not None and now - found_at < JUDGE_S:
                continue
            self.judge(npc, s)

    def judge(self, npc: str, s: Sighting) -> list[Fact]:
        """Suspicion for one find, by what the finder knows now; marked decided in the branch."""
        npc = npc.upper()
        find = self._find(npc, s)
        suspicions = self._suspect(find) if find.body else []
        self._judged.add((npc, find.trace))
        self.chronicle.record(
            {
                "fact": JUDGED,
                "npc": npc,
                "trace": find.trace,
                "kind": find.kind,
                "suspects": list(dict.fromkeys(str(f["suspect"]) for f in suspicions)),
            }
        )
        if suspicions:
            log.info("%s suspects %s for %s", npc, [f["suspect"] for f in suspicions], find.trace)
            self.journal.add(
                "trace_suspected",
                npc=npc,
                trace=find.trace,
                body=find.body,
                suspects=[f["suspect"] for f in suspicions],
            )
        for listener in self.on_judged:
            try:
                listener(find, suspicions)
            except Exception:
                log.exception("a judgement listener failed")
        return suspicions

    def _decided(self, npc: str, trace: str) -> bool:
        return any(f.get("npc") == npc and f.get("trace") == trace for f in self.chronicle.facts(JUDGED))

    def _find(self, npc: str, s: Sighting) -> Found:
        body = s.trace_npc if s.trace_kind == "body" else ""
        died = s.start - s.trace_age if body and s.trace_age is not None else None
        return Found(
            npc, trace_id(s), s.trace_kind, s.name, s.place_ids[0] if s.place_ids else "", body, died
        )

    def _suspect(self, find: Found) -> list[Fact]:
        npc, body, died, place = find.npc, find.body, find.died, find.place
        if died is None or not place:
            return []  # no time of death or place is better than accusing everyone seen that day
        if self.witnessed(npc, body) or self.was_there(npc, body, died, place):
            return []  # it saw the killing, or was there itself: no one is suspected by the trace
        out: list[Fact] = []
        if self.crimes is not None:
            out += self.crimes.suspect_body(npc, body, find.trace, died=died, place=place)
        if self.npc_crimes is not None:
            out += self.npc_crimes.suspect_body(npc, body, find.trace, died=died, place=place)
        return out

    def witnessed(self, npc: str, body: str) -> bool:
        """It saw the killing itself: a deed of murder of this body it witnessed, the hero's or an npc's."""
        body = body.upper()
        seen = [*(self.crimes.seen(npc) if self.crimes is not None else [])]
        seen += self.npc_crimes.seen(npc) if self.npc_crimes is not None else []
        return any(f.get("kind") == "murder" and str(f.get("victim", "")).upper() == body for f in seen)

    def was_there(self, npc: str, body: str, died: int, place: str) -> bool:
        """It saw him alive at that place until PRESENT_MIN game minutes before he died, or later: it was
        there when he died (and may have killed him). No conclusion about who did it.

        Alive: the sighting began before the death. The sighting of the body itself begins at the find, so
        a body found within its first game minute (age 0) is not a sight of him alive."""
        body = body.upper()
        return any(
            s.who == body and place in s.place_ids and s.start < died and s.end >= died - PRESENT_MIN
            for s in self.sightings.mind(npc).sightings
        )

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
        find = self._find(npc.upper(), s)
        if find.body and self.witnessed(npc, find.body):
            return "; здесь его убили, ты видел это сам"
        if (
            find.body
            and find.died is not None
            and find.place
            and self.was_there(npc, find.body, find.died, find.place)
        ):
            return "; здесь кого-то убили, ты был здесь, когда он умер"
        names = list(dict.fromkeys(_name(f) for f in self.suspicions(npc, find.trace)))
        if not names:
            return "; здесь кого-то убили, кто это сделал — не знаешь"
        return (
            f"; здесь кого-то убили. Около того времени ты сам видел здесь: {', '.join(names)} — "
            "это подозрение, не доказательство"
        )

    def _context(self, find: Found) -> str:
        """The find for the behavioral model (English), after the stimulus' own words. Whom it suspects is
        not decided yet (`judge`); only what it knows of the death itself."""
        if find.kind != "body":
            return ""
        if find.body and self.witnessed(find.npc, find.body):
            return " You saw the killing yourself."
        if (
            find.body
            and find.died is not None
            and find.place
            and self.was_there(find.npc, find.body, find.died, find.place)
        ):
            return " You were here when they died."
        return ""


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
