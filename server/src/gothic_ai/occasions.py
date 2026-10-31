"""Model-free occasions for the showrunner (docs/director.md, stage 4, layer 1).

The branch owns visits, chapter and the quiet clock. The live queue deliberately does not survive a
load: recording an occasion is enough to remember that it happened, even if no writer took it.
"""

from __future__ import annotations

import asyncio
import logging
import time
from collections import deque
from collections.abc import Awaitable, Callable
from typing import Any

from .chronicle import Chronicle, Fact
from .game import GameError, GameLink
from .places import Place, PlaceBook
from .protocol import GameMessage, Loading, Session, WorldLoaded

log = logging.getLogger(__name__)

PRIORITY = {"deed": 0, "vanilla": 1, "chapter": 2, "arrived": 3, "quiet": 4}
# `claim` is recorded after the ending/reward succeeds. Only a truthful accepted claim counts;
# a paid lie is no completed quest (vanilla.py and quests.py).
DEEDS = {"found_out", "story_quest_done", "authored_done", "claim", "crime", "npc_crime"}
LINES = {"story_quest", "chain", "authored"}
DAY_MINUTES = 24 * 60
AWAY_CM = 5000
NEAREST = ["call", "NPC_GETNEARESTWP", ["inst", "HERO"]]


class WaypointUnavailable(GameError):
    """No nearest waypoint of the hero from story_eval this time (a world in between, say); a GameError, so
    the watcher and `next` try again on the next sample instead of stopping."""


class OccasionFailed(Exception):
    """An unexpected handoff failure consumed this occasion; the writer must record its outcome."""

    def __init__(self, occasion: Fact, error: Exception) -> None:
        super().__init__(str(error))
        self.occasion, self.error = occasion, error


def _minutes(fact: Fact) -> int | None:
    if not all(isinstance(fact.get(k), int) for k in ("day", "hour", "minute")):
        return None
    return fact["day"] * DAY_MINUTES + fact["hour"] * 60 + fact["minute"]


async def with_quest_outcomes(
    events: list[Fact], facts: list[Fact], link: GameLink, names: dict[str, str],
) -> list[Fact]:
    if not events:
        return events
    outcomes = {f["quest"]: f for f in facts if f.get("fact") == "quest_outcome"}
    relevant = [
        outcomes[e["quest"]] for e in events if e.get("to") != "running" and e.get("quest") in outcomes
    ]
    if not relevant:
        return events
    dropped = sorted({n for outcome in relevant for n in outcome["drop"]})
    state = await link.story_state(npcs=dropped, items=dropped) if dropped else {}
    names = {
        **names,
        **state.get("items", {}),
        **{n: s["name"] for n, s in state.get("npcs", {}).items() if s.get("name")},
    }
    return [
        {
            **event,
            "outcome": {
                "how": outcome["how"],
                "drop": [names.get(n, n) for n in outcome["drop"]],
                "why": outcome["why"],
                "pay": outcome["pay"],
            },
        } if event.get("to") != "running" and (outcome := outcomes.get(event.get("quest"))) else event
        for event in events
    ]


class Occasions:
    def __init__(
        self,
        link: GameLink,
        chronicle: Chronicle,
        places: PlaceBook,
        *,
        poll_s: float = 5.0,
        settle_s: float = 10.0,
        chapter_s: float = 30.0,
        busy_poll_s: float = 1.0,
        gather_s: float = 10.0,
        max_gather_s: float = 30.0,
        clock: Callable[[], float] = time.monotonic,
        crimes: bool = True,
        npc_crimes: bool = True,
    ) -> None:
        self.link, self.chronicle, self.places = link, chronicle, places
        self.poll_s, self.settle_s, self.chapter_s = poll_s, settle_s, chapter_s
        self.busy_poll_s, self.clock = busy_poll_s, clock
        self.gather_s, self.max_gather_s = gather_s, max_gather_s
        self.crimes = crimes
        self.npc_crimes = npc_crimes
        self.current: Place | None = None
        self.waypoint: str | None = None
        self.visited: set[str] = set()
        self.pending: list[Fact] = []
        self._queued_at: dict[int, float] = {}
        self._generation = 0
        self._lock = asyncio.Lock()
        self._changed = asyncio.Event()
        self._wake = asyncio.Event()
        self._events: deque[Fact] = deque()
        self._deaths: list[Fact] = []
        self._npc_deeds: set[tuple[str, str]] = set()
        self._people: list[dict[str, Any]] | None = None
        self._context = "PC_HERO"
        self._residents: dict[str, set[str]] = {}
        self._candidate: str | None = None
        self._since = self.clock()
        self._chapter: int | None = None
        self._chapter_checked = float("-inf")
        self._activity: int | None = None
        self._time: Fact = {}
        self._nearby: list[str] = []
        self._raw_place: str | None = None
        self._distances: dict[str, float] = {}
        chronicle.on_fact.append(self._on_fact)
        link.listeners.append(self._on_message)
        self._reset()

    @property
    def recent(self) -> list[Fact]:
        return self.chronicle.facts("occasion")[-10:]

    def _reset(self) -> None:
        self._generation += 1
        self.current = self.waypoint = None
        self.visited.clear()
        self.pending.clear()
        self._queued_at.clear()
        self._events.clear()
        self._deaths.clear()
        self._npc_deeds.clear()
        self._people = None
        self._context = "PC_HERO"
        self._residents.clear()
        self._candidate = self._raw_place = None
        self._since = self.clock()
        self._chapter = self._activity = None
        self._chapter_checked = float("-inf")
        self._time, self._nearby, self._distances = {}, [], {}
        for fact in self.chronicle.facts():
            match fact.get("fact"):
                case "visited":
                    self.visited.add(fact["place"])
                case "chapter":
                    self._chapter = int(fact["value"])
                case "occasion" | "occasion_clock":
                    if (at := _minutes(fact)) is not None:
                        self._activity = at
                    if (key := self._npc_deed_key(fact.get("deed", {}))) is not None:
                        self._npc_deeds.add(key)
                case _ if fact.get("fact") in LINES:
                    # A quest can be saved before the next sample timestamps it. Start conservatively
                    # at the first sample of that load, never from a clock older than the quest.
                    self._activity = None
        self._changed.set()
        self._wake.set()

    def _on_message(self, message: GameMessage) -> None:
        if isinstance(message, Session):
            # Chronicle registered first: its branch and ancestry are already current here.
            self._reset()
        elif isinstance(message, Loading):
            self._generation += 1
            self.current = self.waypoint = None
            self.pending.clear()
            self._queued_at.clear()
            self._events.clear()
            self._deaths.clear()
            self._changed.set()
        elif isinstance(message, WorldLoaded):
            self._wake.set()

    def _on_fact(self, fact: Fact) -> None:
        if fact.get("fact") in DEEDS | LINES | {"died", "vanilla_quest"}:
            self._events.append(dict(fact))

    async def run(self) -> None:
        while True:
            self._wake.clear()
            try:
                await self.poll()
            except (GameError, TimeoutError) as error:
                log.warning("occasions: %s", error)
            try:
                await asyncio.wait_for(self._wake.wait(), self.poll_s)
            except TimeoutError:
                pass

    def _distance_exprs(self) -> tuple[list[str], list[list[Any]]]:
        ids = {f["place"] for f in self.pending if f["kind"] in {"arrived", "quiet"} and f["place"]}
        owners, exprs = [], []
        for id in sorted(ids):
            if (place := self.places.get(id)) is not None:
                for wp in place.waynet:
                    owners.append(id)
                    exprs.append(["call", "NPC_GETDISTTOWP", ["inst", "HERO"], ["str", wp]])
        return owners, exprs

    def _set_distances(self, owners: list[str], values: list[Any]) -> None:
        self._distances.clear()
        for owner, value in zip(owners, values, strict=True):
            if isinstance(value, (float, int)) and value >= 0:
                self._distances[owner] = min(self._distances.get(owner, float("inf")), value)

    async def poll(self) -> None:
        """One live sample; serialized with `next`, including across save/load messages."""
        async with self._lock:
            generation = self._generation
            if self.link.session is None or self.link.world is None:
                return
            if self._people is None:
                people = await self.link.story_people()
                if generation != self._generation:
                    return
                self._people = people
                # story_eval's SELF is a concrete NPC symbol, not the HERO global. The expression
                # itself reads HERO, so any existing person supplies the otherwise unused context.
                self._context = people[0]["npc"] if people else "PC_HERO"
                for person in people:
                    for entry in person.get("routine", []):
                        if (place := self.places.of(entry.get("waypoint"))) is not None:
                            self._residents.setdefault(place.id, set()).add(person["npc"])
            owners, distances = self._distance_exprs()
            values = await self.link.story_eval(self._context, [NEAREST, ["inst", "HERO"], *distances])
            if generation != self._generation:
                return
            if not values or not isinstance(values[0], str) or not values[0]:
                raise WaypointUnavailable("NPC_GETNEARESTWP through story_eval returned no waypoint")
            waypoint = values[0].upper()
            place = self.places.of(waypoint)
            chapter_due = self.clock() - self._chapter_checked >= self.chapter_s
            residents = self._residents.get(place.id, set()) if place else set()
            state = await self.link.story_state(
                vars=["KAPITEL"] if chapter_due else (), npcs=sorted(residents), near=values[1]
            )
            if generation != self._generation:
                return
            self._set_time(state)
            if not self._time:
                return
            self.waypoint, self._raw_place = waypoint, place.id if place else None
            self._set_distances(owners, values[2:])
            self._nearby = sorted(
                npc
                for npc, info in state.get("npcs", {}).items()
                if info.get("state") == "alive" and info.get("distance", float("inf")) <= AWAY_CM
            )
            if self._activity is None:
                self._reset_quiet()
            if self._candidate != self._raw_place:
                self._candidate, self._since = self._raw_place, self.clock()
                self.current = None
            if place is not None and self.clock() - self._since >= self.settle_s:
                self.current = place
                if place.id not in self.visited:
                    self.visited.add(place.id)
                    self.chronicle.record(
                        {"fact": "visited", "place": place.id, "waypoint": waypoint, **self._time}
                    )
                    if residents or place.kind in {"camp", "cave"}:
                        self._occasion("arrived")
            if chapter_due:
                value = state.get("vars", {}).get("KAPITEL")
                if isinstance(value, int):
                    previous = self._chapter
                    if previous is None or value > previous:
                        self._chapter = value
                        self.chronicle.record({"fact": "chapter", "value": value, **self._time})
                        if previous is not None:
                            self._occasion("chapter", chapter=value)
                    self._chapter_checked = self.clock()
            events = list(self._events)
            vanilla = iter(await with_quest_outcomes(
                [f for f in events if f["fact"] == "vanilla_quest"],
                self.chronicle.facts("quest_outcome"), self.link, {},
            ))
            if generation != self._generation:
                return
            deaths, self._deaths = self._deaths, []
            for fact in events:
                self._events.popleft()
                if fact["fact"] in LINES:
                    self._reset_quiet()
                elif fact["fact"] == "vanilla_quest":
                    self._occasion("vanilla", vanilla=next(vanilla))
                elif self._is_deed(fact):
                    if self.crimes and self._human_killed(fact):
                        # A death scan may precede the passive perception's crime by a frame. Wait
                        # for one more sample before falling back to the older, less detailed deed.
                        self._deaths.append(fact)
                    else:
                        self._occasion(
                            "deed", deed=fact,
                            **({"crime": fact["id"]} if fact["fact"] in {"crime", "npc_crime"} else {}),
                        )
            for fact in deaths:
                if self._is_deed(fact):
                    self._occasion("deed", deed=fact)
            at = _minutes(self._time)
            if self.current is not None and at is not None and at - self._activity >= DAY_MINUTES:
                self._occasion("quiet")
            self._expire()
            self._changed.set()

    def _human_killed(self, fact: Fact) -> bool:
        if fact.get("fact") != "died" or fact.get("killer") != "PLAYER":
            return False
        if "monster" in fact:
            return fact["monster"] is False
        # The bridge emits untagged npc_died only for humans. Tagged creatures need proof.
        return not fact.get("tag") or any(p["npc"] == fact.get("npc") for p in self._people or ())

    def _is_deed(self, fact: Fact) -> bool:
        if (key := self._npc_deed_key(fact)) is not None:
            return self.crimes and self.npc_crimes and key not in self._npc_deeds
        if fact.get("fact") == "crime":
            return self.crimes
        if fact.get("fact") == "claim":
            return fact.get("truth") is True and fact.get("believed") is True
        if self._human_killed(fact):
            # The crime carries the full truth and owns this occasion; the death remains in the
            # Chronicle for its other readers.
            return not self.crimes or not any(
                crime.get("kind") == "murder" and crime.get("victim") == fact.get("npc")
                for crime in self.chronicle.facts("crime")
            )
        return fact.get("fact") in DEEDS

    @staticmethod
    def _npc_deed_key(fact: Fact) -> tuple[str, str] | None:
        if fact.get("fact") == "npc_crime" and isinstance(fact.get("id"), str):
            return "npc_crime", fact["id"]
        return None

    def _set_time(self, state: Fact) -> None:
        self._time = {k: state[k] for k in ("day", "hour", "minute") if isinstance(state.get(k), int)}
        if _minutes(self._time) is None:
            self._time.clear()

    def _reset_quiet(self) -> None:
        self._activity = _minutes(self._time)
        self.chronicle.record({"fact": "occasion_clock", **self._time})

    def _occasion(self, kind: str, **details: Any) -> None:
        if (key := self._npc_deed_key(details.get("deed", {}))) is not None:
            self._npc_deeds.add(key)
        fact = {
            "fact": "occasion",
            "kind": kind,
            "place": self._raw_place,
            "waypoint": self.waypoint,
            "people": list(self._nearby),
            **self._time,
            **details,
        }
        self.chronicle.record(fact)
        self._activity = _minutes(fact)
        self.pending.append(fact)
        self._queued_at[id(fact)] = self.clock()
        self.pending.sort(key=lambda f: PRIORITY[f["kind"]])

    def _expire(self) -> None:
        at = _minutes(self._time)
        if at is None:
            return
        self.pending[:] = [
            f
            for f in self.pending
            if 0 <= at - _minutes(f) < DAY_MINUTES
            and not (
                f["kind"] in {"arrived", "quiet"}
                and self._raw_place != f["place"]
                and self._distances.get(f["place"], 0) > AWAY_CM
            )
        ]
        self._sync_queued_at()

    def _sync_queued_at(self) -> None:
        now = self.clock()
        self._queued_at = {id(f): self._queued_at.get(id(f), now) for f in self.pending}

    def _gathering(self) -> bool:
        self._sync_queued_at()
        if not self._queued_at:
            return False
        times = self._queued_at.values()
        return self.clock() < min(min(times) + self.max_gather_s, max(times) + self.gather_s)

    def _bundle(self) -> Fact:
        first, *others = self.pending
        return {**first, **({"also": [dict(f) for f in others]} if others else {})}

    def _consume(self) -> Fact:
        occasion = self._bundle()
        self.pending.clear()
        self._queued_at.clear()
        return occasion

    def put_back(self, occasion: Fact) -> None:
        """Restore a gathered group if another decision closed its budget during handoff."""
        first = {k: v for k, v in occasion.items() if k != "also"}
        for fact in [first, *occasion.get("also", [])]:
            self.pending.append(fact)
            self._queued_at[id(fact)] = self.clock() - self.max_gather_s
        self.pending.sort(key=lambda f: PRIORITY[f["kind"]])
        self._expire()
        self._changed.set()

    async def next(self, *, ready: Callable[[], Awaitable[bool]] | None = None) -> Fact:
        """Gather and take fresh occasions while the budget and the game allow a decision.

        The best leads; the rest accompany it in `also`. A closed budget or cancellation leaves
        the queue intact. The optional budget check runs before any handoff requests to the bridge.
        Run the watcher alongside the consumer. Every await is guarded against a load of another branch.
        """
        while True:
            self._changed.clear()
            try:
                async with self._lock:
                    generation = self._generation
                    try:
                        can_take = (
                            bool(self.pending) and self.link.world is not None and not self._gathering()
                        )
                        if can_take and ready is not None:
                            can_take = await ready()
                        if (
                            can_take
                            and generation == self._generation
                            and (fact := await self._take()) is not None
                        ):
                            return fact
                    except (GameError, TimeoutError):
                        raise
                    except Exception as error:
                        if generation == self._generation and self.pending:
                            raise OccasionFailed(self._consume(), error) from error
                        raise
            except (GameError, TimeoutError) as error:
                log.warning("occasions waiting: %s", error)
            try:
                await asyncio.wait_for(self._changed.wait(), self.busy_poll_s)
            except TimeoutError:
                pass

    async def _take(self) -> Fact | None:
        generation = self._generation
        if not self.pending or self.link.world is None:
            return None
        state = await self.link.story_state()
        if generation != self._generation:
            return None
        self._set_time(state)
        if not self._time or state.get("dialog") or state.get("cutscene"):
            return None
        # Refresh movement at handoff too: the hero may have left since the last tick.
        owners, exprs = self._distance_exprs()
        values = await self.link.story_eval(self._context, [NEAREST, *exprs])
        if generation != self._generation:
            return None
        if not values or not isinstance(values[0], str) or not values[0]:
            raise WaypointUnavailable("NPC_GETNEARESTWP through story_eval returned no waypoint")
        place = self.places.of(values[0])
        self._raw_place = place.id if place else None
        self._set_distances(owners, values[1:])
        self._expire()
        return self._consume() if self.pending else None
