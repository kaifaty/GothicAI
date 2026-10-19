"""Who knows whom, and the rumors about people they pass on (docs/story-graph.md, stage 5).

Two people know each other in two ways:
- they meet: by their daily routines (`story_people`) they are within MEET_CM of each other at the same
  hours, every day — the farm's people, the barracks, the tavern's regulars;
- they have dealings: they share a quest of the scripts (its variable is read or written in both their
  dialogs) — Andre, who sends the hero to Lobart's field, and Lobart. A quest with more than DEAL_MAX people
  in it makes no acquaintances: those are the chapters and the big lines.
Outlaws (bandits...) know only each other: Maleth's quest to kill a bandit is no acquaintance.

A rumor that the hero lied to someone starts with the people around when it came out (quests.unmask,
quests.caught). Then, as the game's time runs, whoever knows it tells whom it meets — at their meeting
hours, a day's meeting passing it P_MEET of the time — or whom it has dealings with, once a day with
some traveller, P_DEAL of the time; to TELLS_PER_DAY people a day at most, while it is news (FRESH_DAYS).
It goes HOPS retellings away from the one lied to: a lie seen through on the spot is talked about nearby,
one the hero took a reward for reaches further. The chances are
drawn from a hash of who, what and which day, so a run repeats. When the game's time jumps (a sleep, the
clock set), the meetings in between are gone through in order, so a rumor travels a chain in one night.

Each retelling is a `rumor` fact and a remark in the listener's memory; trust.py puts it in the situation.

What passes is decided here; how it looks is gossip.py's (`stage`): a pass in a live window (a tick, not a
jump of the clock) may be said aloud near the hero (how: talked) or seen, or wait while the two are busy.
The fact is written first, the scene comes second. A walk to tell someone (daylife.py) is offered only when
the lot would pass it now (`can_tell`), and passes as a visit (how: visited) unless it is said aloud.
"""

from __future__ import annotations

import asyncio
import hashlib
import heapq
import logging
import math
from collections import defaultdict
from collections.abc import Callable, Iterator, Mapping
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any, Literal

from .chronicle import Chronicle, Fact
from .game import GameError, GameLink
from .gamedata.storygraph import StoryGraph, walk
from .people_knowledge import PLAYER, SOURCES, rumor_subject, source_event, source_kind
from .protocol import GameMessage, Session

if TYPE_CHECKING:
    from .crimes import Crimes
    from .dialogue import DialogueManager
    from .gossip import Staging  # gossip imports this module

log = logging.getLogger(__name__)

MEET_CM = 2500  # people at their daily spots this close, at the same hours, meet
DEAL_MAX = 6  # a quest with more people in it makes no acquaintances
OUTLAWS = frozenset({9, 10, 11})  # bandits, convicts, seekers: they have no dealings with the rest
# retellings away from the one lied to: a lie seen through on the spot is talked about nearby, one he took
# a reward for goes further
HOPS = {"caught": 2, "found_out": 3, "broken": 2, "theft": 2, "attack": 3, "murder": 4, "sheep": 2}
HOPS.update({s.kind: s.hops for s in SOURCES.values()})
CRIME_KINDS = frozenset({"theft", "attack", "murder", "sheep"})
TELLS_PER_DAY = 3  # one tells a rumor to this many a day at most: it goes round in chains, not in a flood
FRESH_DAYS = 2  # and only while it is news: the day one heard it and the next
P_MEET = 0.7  # how often a day's meeting passes a rumor on
P_DEAL = 0.5  # how often people with dealings hear it, once a day
DEAL_FROM, DEAL_TO = 8 * 60, 20 * 60  # the hours news comes with some traveller
TICK_S = 3.0
DAY = 24 * 60
# a spread over more game minutes than this is a jump (a sleep, the clock set, a stalled tick): facts only.
# A tick is 3 s = 0-1 game minutes (gamesession.cpp:20-22)
LIVE_MIN = 2
DEFER_MAX_MIN = 60  # a live pass waits at most this long for busy bodies, then it is a fact
SMALLTALK = "ZS_SMALLTALK"


@dataclass
class Tie:
    """How two people know each other."""

    meets: list[tuple[int, int]] = field(default_factory=list)  # daily, minutes of the day, end > start
    deals: list[str] = field(default_factory=list)  # quests they share
    # daily, when both stand in ZS_SMALLTALK, and the first one's waypoint (in story_people's order)
    talks: list[tuple[int, int, str]] = field(default_factory=list)
    # how they know each other, in words: a character of the director's knows them (cast.Person.ties)
    bonds: list[str] = field(default_factory=list)

    def occasions(self, a: str, b: str, after: int, until: int) -> Iterator[int]:
        """The game minutes in [after, until] they talk: their meetings (from when one already goes on),
        and once a day news through their dealings."""
        for day in range(after // DAY, until // DAY + 1):
            base = day * DAY
            for s, e in self.meets:
                if base + e > after and base + s <= until:
                    yield max(base + s, after)
            if self.deals or self.bonds:
                at = (
                    base + DEAL_FROM + int(_chance("deal", *sorted((a, b)), str(day)) * (DEAL_TO - DEAL_FROM))
                )
                if after <= at <= until:
                    yield at


@dataclass(frozen=True)
class Spot:
    """One entry of a person's daily routine: when, what it does and where."""

    start: int  # minutes of the day; an end before the start goes past midnight, an equal one is all day
    end: int
    state: str  # upper case
    waypoint: str
    pos: tuple[float, float, float] | None  # None: the world has no such waypoint


class Social:
    """The acquaintances of the world's people: whom each meets, and when; whom each has dealings with.
    And each one's day by its routine: where it is at a minute, how many a place holds (daylife.py)."""

    def __init__(
        self,
        people: list[dict[str, Any]],
        graph: StoryGraph | None = None,
        bonds: Mapping[str, Mapping[str, str]] | None = None,
    ) -> None:
        self.names = {p["npc"]: p.get("name") or p["npc"] for p in people}
        self.guilds = {p["npc"]: int(p.get("guild", 0)) for p in people}
        self.ties: dict[str, dict[str, Tie]] = defaultdict(dict)
        # every routine entry in story_people's order (a smalltalk's place is its first one's); then by person
        self._spots = [
            (
                p["npc"],
                Spot(
                    int(r["start"]),
                    int(r["end"]),
                    str(r.get("state") or "").upper(),
                    str(r.get("waypoint") or ""),
                    tuple(r["pos"]) if r.get("pos") else None,
                ),
            )
            for p in people
            for r in p.get("routine", [])
        ]
        self.days: dict[str, list[Spot]] = {p["npc"]: [] for p in people}
        self._at: dict[str, list[tuple[str, Spot]]] = defaultdict(list)  # waypoint (upper case) -> entries
        for npc, spot in self._spots:
            self.days[npc].append(spot)
            self._at[spot.waypoint.upper()].append((npc, spot))
        self._room: dict[str, int] = {}  # capacity, by waypoint
        self._meetings()
        if graph is not None:
            self._dealings(graph)
        # whom the director's characters know, by their description: news goes between them once a day
        for a, known in (bonds or {}).items():
            for b, how in known.items():
                if a in self.names and b in self.names and (tie := self.tie(a, b)) is not None:
                    tie.bonds.append(how)

    def tie(self, a: str, b: str) -> Tie | None:
        """Their tie, made if there is none yet; None if they can't be acquainted (an outlaw and the rest)."""
        if (self.guilds.get(a) in OUTLAWS) != (self.guilds.get(b) in OUTLAWS):
            return None
        if b not in self.ties[a]:
            t = Tie()
            self.ties[a][b] = self.ties[b][a] = t
        return self.ties[a][b]

    def _meetings(self) -> None:
        # (npc, start, end, pos, state, waypoint): what one does there is kept, for their smalltalk
        spots = [
            (npc, s, e, spot.pos, spot.state, spot.waypoint)
            for npc, spot in self._spots
            if spot.pos is not None
            for s, e in _span(spot.start, spot.end)
        ]
        cells: dict[tuple[int, int], list[int]] = defaultdict(list)
        for i, spot in enumerate(spots):
            pos = spot[3]
            cells[(int(pos[0] // MEET_CM), int(pos[2] // MEET_CM))].append(i)
        for (cx, cz), ids in cells.items():
            near = [j for dx in (-1, 0, 1) for dz in (-1, 0, 1) for j in cells.get((cx + dx, cz + dz), [])]
            for i in ids:
                a, s1, e1, p1, st1, wp1 = spots[i]
                for j in near:
                    b, s2, e2, p2, st2, _ = spots[j]
                    if j <= i or a == b or math.dist(p1, p2) > MEET_CM:
                        continue
                    s, e = max(s1, s2), min(e1, e2)
                    if s < e and (tie := self.tie(a, b)) is not None:
                        tie.meets.append((s, e))
                        if st1 == st2 == SMALLTALK:
                            tie.talks.append((s, e, wp1))
        for ties in self.ties.values():
            for tie in ties.values():
                tie.meets = _merged(tie.meets)
                tie.talks = _merged_at(tie.talks)

    def _dealings(self, graph: StoryGraph) -> None:
        """People whose dialogs read or write the same quest's variable."""
        involved: dict[str, set[str]] = defaultdict(set)
        quests = set(graph.quests)
        for node in graph.nodes.values():
            if node.npc not in self.names:
                continue  # not a person of this world
            exprs = [node.condition, *(e.get("when") for e in node.effects)]
            used = {x[1] for e in exprs for x in walk(e) if x[0] == "var" and x[1] in quests}
            used |= {e["var"] for e in node.effects if e.get("do") == "set" and e.get("var") in quests}
            for q in used:
                involved[q].add(node.npc)
        for q, people in involved.items():
            if 1 < len(people) <= DEAL_MAX:
                for a in people:
                    for b in people:
                        if a < b and (tie := self.tie(a, b)) is not None and q not in tie.deals:
                            tie.deals.append(q)

    def describe(self, npc: str) -> list[str]:
        """Its acquaintances, in words (for the console)."""
        out = []
        for other, t in sorted(self.ties.get(npc.upper(), {}).items()):
            how = [f"{_hm(s)}–{_hm(e)}" for s, e in sorted(t.meets)]
            how += [f"дела: {q}" for q in t.deals]
            how += [f"болтают {_hm(s)}–{_hm(e)} у {wp}" for s, e, wp in t.talks]
            how += [f"знакомы: {b}" for b in t.bonds]
            out.append(f"{self.names.get(other, other)} ({other}): {', '.join(how)}")
        return out

    def smalltalk(self, a: str, b: str, minute: int) -> str | None:
        """Where the two stand chatting by their routines at that game minute (the first one's waypoint),
        None if they don't then."""
        tie = self.ties.get(a.upper(), {}).get(b.upper())
        if tie is not None:
            for s, e, wp in tie.talks:
                if s <= minute % DAY < e:
                    return wp
        return None

    def spot(self, npc: str, minute: int) -> Spot | None:
        """Its routine entry at that game minute (any day), None if no entry holds it."""
        return _entry(self.days.get(npc.upper(), []), minute)

    def spots(self) -> Iterator[tuple[str, Spot]]:
        """Every routine entry of the world's people, with whose it is."""
        return iter(self._spots)

    def users(self, waypoint: str, minute: int) -> set[str]:
        """Who is at the waypoint at that game minute by their routines (their entry then is there)."""
        at = self._at.get(waypoint.upper(), [])
        return {npc for npc, spot in at if _entry(self.days[npc], minute) is spot}

    def capacity(self, waypoint: str) -> int:
        """How many the routines put at the waypoint at once, at most over the day; at least 1."""
        wp = waypoint.upper()
        if (room := self._room.get(wp)) is None:
            people = {npc for npc, _ in self._at.get(wp, [])}
            # who is where changes only when one of their entries starts or ends
            turns = {0} | {m % DAY for npc in people for s in self.days[npc] for m in (s.start, s.end)}
            room = self._room[wp] = max(1, max(len(self.users(wp, m)) for m in turns))
        return room


@dataclass
class Rumor:
    """News of a person, as it goes round: who knows it, how many retellings away."""

    quest: str
    kind: str  # found_out: he took a reward for it; caught: he tried to
    about: str  # the one lied to
    about_name: str
    title: str
    known: dict[str, int] = field(default_factory=dict)  # npc -> retellings away from `about`
    since: dict[str, int] = field(default_factory=dict)  # npc -> the game day it heard it (if known)
    silent: set[str] = field(default_factory=set)  # promised the hero in a deal to tell nobody (deals.py)
    said: bool = False  # the lie was his words (title), not a quest: seen false (sightings.py)
    crime: str = ""  # the deed's Chronicle id, also for a lie about that deed
    subject: str = PLAYER
    subject_name: str = "герой"

    def __post_init__(self) -> None:
        self.subject = (self.subject or PLAYER).upper()
        if not self.is_player and self.subject_name == "герой":
            self.subject_name = self.subject

    @property
    def is_player(self) -> bool:
        return (self.subject or PLAYER).upper() == PLAYER

    @property
    def hops(self) -> int:
        return HOPS.get(self.kind, 2)

    def tells(self, npc: str, day: int) -> bool:
        """It still passes the rumor on that day: not too far from the source, and it is still news."""
        heard = self.since.get(npc)
        if npc in self.silent or npc == self.subject:
            return False
        fresh = 4 if self.kind == "murder" else FRESH_DAYS
        return self.known.get(npc, self.hops) < self.hops and (heard is None or day < heard + fresh)

    @property
    def key(self) -> str:
        old = f"{self.kind}:{self.about}:{self.quest}"
        return old if self.is_player else f"subject:{self.subject}:{old}"

    def told(self, teller: str = "") -> str:
        """The remark for the one it is told to (names stay as they are: nothing to decline)."""
        prefix = f"Слух (со слов: {teller}): " if teller else "Слух: "
        return prefix + self.description()

    def description(self) -> str:
        """The same subject-aware content for memories, news and a live retelling."""
        subject = "герой" if self.is_player else self.subject_name or self.subject
        if (source := source_kind(self.kind)) is not None:
            return source.text.format(subject_name=subject, about_name=self.about_name, title=self.title)
        if self.kind in CRIME_KINDS:
            return self.title
        if self.kind == "broken" and self.quest.startswith("meet-"):  # meetings.py
            return (
                f"{subject} обещал человеку по имени {self.about_name} прийти на "
                f"встречу и не пришёл — {self.title}"
            )
        if self.kind == "broken":
            return (
                f"{subject} задолжал человеку по имени {self.about_name} и не вернул "
                f"в срок — {self.title}"
            )
        what = (
            f"взял награду за «{self.title}», а дела не сделал"
            if self.kind == "found_out"
            else f"сказал ему «{self.title}», а это неправда"
            if self.said
            else f"уверял, что сделал «{self.title}», а это неправда"
        )
        return f"{subject} обманул человека по имени {self.about_name} — {what}"


class Rumors:
    """News about people going round the people that know each other, by the game's clock."""

    # class-level defaults: tests build Rumors with __new__
    stage: Callable[[Rumor, str, str], Staging] | None = None  # gossip.py: how a live pass looks
    on_staged: Callable[[Rumor, str, str, Staging, Fact], None] | None = None  # after its fact is written
    gate: Callable[[Rumor, str, str], bool | None] | None = None  # e2e: False holds a pass, True forces it
    clock: int | None = None  # the game minute gone through
    waiting: dict[tuple[str, str, str], int] | None = None  # (rumor, teller, listener) -> first minute
    crimes: Crimes | None = None  # absent with --no-crimes: truth then never seeds a crime rumor

    def __init__(
        self,
        link: GameLink,
        dialogue: DialogueManager,
        chronicle: Chronicle,
        graph: StoryGraph | None = None,
        bonds: Callable[[], Mapping[str, Mapping[str, str]]] = dict,
        crimes: Crimes | None = None,
    ) -> None:
        self.link = link
        self.dialogue = dialogue
        self.chronicle = chronicle
        self.graph = graph
        self.bonds = bonds  # whom the director's characters know (cast.py)
        self.crimes = crimes
        self.social: Social | None = None
        self.rumors: dict[str, Rumor] = {}
        self.dead: set[str] = set()
        self.tells: dict[tuple[str, str, int], int] = defaultdict(int)  # (rumor, teller, day) -> told
        self.clock = None
        chronicle.on_fact.append(self._on_fact)
        link.listeners.append(self._on_message)
        self._load()

    async def run(self) -> None:
        while True:
            await asyncio.sleep(TICK_S)
            await self.link.wait_world()  # the game answers in the world's tick
            try:
                await self.tick()
            except (GameError, TimeoutError) as err:
                log.info("rumors: %s", err or type(err).__name__)

    async def tick(self) -> None:
        """Who met whom since the last tick, and what they told each other."""
        if self.social is None:
            people = await self.link.story_people()
            self.social = Social(people, self.graph, self.bonds())
            if self.crimes is not None:
                self.crimes.people = {p["npc"]: p for p in people}
            log.info("rumors: %d people, %d ties", len(self.social.names), _count(self.social))
        state = await self.link.story_state()
        now = int(state.get("day", 0)) * DAY + int(state.get("hour", 0)) * 60 + int(state.get("minute", 0))
        if self.clock is not None and now > self.clock:
            self.spread(self.clock, now)
        self.clock = now

    def spread(self, after: int, until: int) -> int:
        """The meetings in (after, until], in order: each passes on what one knows and the other does not.
        How many retellings there were. Only a live window (a tick, not a jump of the clock) is staged."""
        social = self.social
        if social is None or not self.rumors:
            return 0
        live = until - after <= LIVE_MIN
        heap: list[tuple[int, str, str]] = []

        def meetings(npc: str, since: int) -> None:
            for other, tie in social.ties.get(npc, {}).items():
                for t in tie.occasions(npc, other, since, until):
                    heapq.heappush(heap, (t, npc, other))

        knowers = {n for r in self.rumors.values() for n in r.known if r.tells(n, after // DAY)}
        for n in sorted(knowers):
            meetings(n, after + 1)
        told = 0
        while heap:
            t, a, b = heapq.heappop(heap)
            day, base = t // DAY, t - t % DAY
            tie = social.ties[a][b]
            # the meeting going on now (absolute minutes); None for a dealings occasion
            meeting = next(((base + s, base + e) for s, e in tie.meets if s <= t % DAY < e), None)
            for r in self.rumors.values():
                if not self._passes(r, a, b, t, meeting is not None):
                    continue
                staging = self._staging(r, a, b) if live else None
                if staging is not None and staging.tier == "wait":
                    if self._waits(r, a, b, t, until, meeting):
                        continue  # no fact, no tell: the next tick tries again
                    staging = None  # waited long enough, or cannot wait: a fact as it always was
                self._pass(r, a, b, t, "met" if meeting is not None else "dealings", staging)
                told += 1
                if r.tells(b, day):
                    meetings(b, t)
        return told

    def can_tell(self, r: Rumor, a: str, b: str) -> bool:
        """a would tell b the rumor if they met now: acquainted, and the checks and lot of a meeting. Changes
        nothing (the day desk asks it for a menu, daylife.py)."""
        if self.social is None or self.clock is None or self.social.ties.get(a, {}).get(b) is None:
            return False
        return self._passes(r, a, b, self.clock, meet=True)

    def offer_status(self, r: Rumor, a: str, b: str, how: str = "met") -> Literal["told", "wait", "no"]:
        """An occasion the schedule does not have now (two acquaintances see each other near the hero,
        gossip.py; a walk to tell one, daylife.py, how: visited; the e2e): the same checks and lot as a
        meeting. told: it passed; wait: they are busy now, ask again later; no: it does not pass."""
        clock = self.clock
        if clock is None or not self.can_tell(r, a, b):
            return "no"
        staging = self._staging(r, a, b)
        if staging is not None and staging.tier == "wait":
            return "wait"
        self._pass(r, a, b, clock, how, staging)
        return "told"

    def tell(self, r: Rumor, a: str, b: str) -> Literal["told", "wait", "no"]:
        """a has set out to tell b (an errand, errands.py): no lot and no day's count, it means to; only the
        living, what each knows and a's word to keep silent (deals.py). how: told, also when near the hero
        it is a scene of two voices (gossip.py)."""
        clock = self.clock
        if clock is None or self.social is None or a in self.dead or b in self.dead:
            return "no"
        if a not in r.known or b in r.known or a in r.silent or a == r.subject:
            return "no"
        if r.crime and (
            not r.tells(a, clock // DAY)
            or (self.social.guilds.get(a) in OUTLAWS) != (self.social.guilds.get(b) in OUTLAWS)
        ):
            return "no"
        staging = self._staging(r, a, b)
        if staging is not None and staging.tier == "wait":
            return "wait"
        self._pass(r, a, b, clock, "told", staging)
        return "told"

    def offer(self, r: Rumor, a: str, b: str) -> bool:
        """offer_status of a meeting: True if it passed (busy now: the view trigger asks again later)."""
        return self.offer_status(r, a, b) == "told"

    def _passes(self, r: Rumor, a: str, b: str, t: int, meet: bool) -> bool:
        """a tells b the rumor at minute t: the living, what each knows, a's tells that day and the lot."""
        day = t // DAY
        if a in self.dead or b in self.dead:
            return False
        if a not in r.known or b in r.known or not r.tells(a, day):
            return False
        forced = self.gate(r, a, b) if self.gate is not None else None
        if forced is not None:
            return forced  # True also skips TELLS_PER_DAY and the lot
        if self.tells.get((r.key, a, day), 0) >= TELLS_PER_DAY:  # a read adds no key
            return False
        return _chance(r.key, a, b, str(day)) < (P_MEET if meet else P_DEAL)

    def _staging(self, r: Rumor, a: str, b: str) -> Staging | None:
        if self.stage is None:
            return None
        try:
            return self.stage(r, a, b)
        except Exception:
            log.exception("rumors: staging %s -> %s failed", a, b)  # the pass stays a plain fact
            return None

    def _waits(self, r: Rumor, a: str, b: str, t: int, until: int, meeting: tuple[int, int] | None) -> bool:
        """A live pass whose bodies are busy now: tried again on the next tick while their meeting goes on
        (it yields again from until+1 then: Tie.occasions; the day's lot is the same), at most DEFER_MAX_MIN.
        A dealings occasion (once a day) never waits."""
        if meeting is None or meeting[1] <= until + 1:
            return False  # it would not come again
        if self.waiting is None:
            self.waiting = {}
        key = (r.key, a, b)
        first = self.waiting.get(key)
        if first is None or first < meeting[0]:
            first = self.waiting[key] = t  # a wait of an earlier meeting is over
        return t - first < DEFER_MAX_MIN

    def _pass(self, r: Rumor, a: str, b: str, t: int, how: str, staging: Staging | None) -> None:
        """b hears it from a: the fact first; then, said or seen, how it looks (gossip.py)."""
        shown = staging if staging is not None and staging.tier in ("words", "sight") else None
        if shown is not None and shown.tier == "words" and how != "told":
            how = "talked"  # an errand's stays told: it went to tell it
        fact = self._learn(r, b, a, t, how, shown.keys() if shown is not None else None)
        if self.waiting:
            self.waiting.pop((r.key, a, b), None)
        if shown is not None and self.on_staged is not None:
            try:
                self.on_staged(r, a, b, shown, fact)
            except Exception:
                log.exception("rumors: staging %s -> %s failed", a, b)  # the fact stays

    def _learn(
        self, r: Rumor, listener: str, teller: str, at: int, how: str, extra: dict[str, Any] | None = None
    ) -> Fact:
        assert self.social is not None
        r.known[listener] = r.known[teller] + 1
        r.since[listener] = at // DAY
        self.tells[(r.key, teller, at // DAY)] += 1
        teller_name = self.social.names.get(teller, teller)
        name = self.social.names.get(listener, listener)
        if not r.about_name or r.about_name == r.about:
            r.about_name = self.social.names.get(r.about, r.about)
        if not r.is_player and (not r.subject_name or r.subject_name in {r.subject, "герой"}):
            r.subject_name = self.social.names.get(r.subject, r.subject)
        fact: Fact = {
            "fact": "rumor",
            "quest": r.quest,
            "kind": r.kind,
            "about": r.about,
            "about_name": r.about_name,
            "title": r.title,
            "subject": r.subject,
            "subject_name": r.subject_name,
            "from": teller,
            "from_name": teller_name,
            "to": listener,
            "hops": r.known[listener],
            "how": how,
            "day": at // DAY,
            **({"said": True} if r.said else {}),
            **({"crime": r.crime, "at": at} if r.crime else {}),
            **(extra or {}),
        }
        self.chronicle.record(fact)
        self.dialogue.remember(listener, name, r.told(teller_name))
        return fact

    # --- the save branch --------------------------------------------------------------------

    def _load(self) -> None:
        self.rumors.clear()
        self.dead.clear()
        self.tells.clear()
        self.waiting = None
        for f in self.chronicle.facts():
            self._on_fact(f)

    def _on_fact(self, f: Fact) -> None:
        if f.get("fact") in SOURCES:
            event = source_event(f, self.chronicle.facts())
            if event is not None:
                r = Rumor(
                    event.key, event.kind, event.about, event.about_name, event.description(),
                    subject=event.subject, subject_name=event.subject_name,
                )
                r.known[event.source] = 0
                r.since[event.source] = event.day
                self.rumors.setdefault(r.key, r)
            return
        match f.get("fact"):
            case "found_out" | "caught":
                r = Rumor(
                    f.get("quest", ""),
                    f["fact"],
                    f.get("npc", ""),
                    f.get("name") or f.get("npc", ""),
                    f.get("title") or f.get("quest", ""),
                    said=bool(f.get("said")),
                    crime=f.get("crime", ""),
                )
                r.known[r.about] = 0
                if "day" in f:
                    r.since[r.about] = int(f["day"])
                if r.crime:
                    self._crime_silence(r)
                self.rumors.setdefault(r.key, r)
            case "crime_seen" if self.crimes is not None:
                crime = next((c for c in self.crimes.facts() if c.get("id") == f.get("crime")), None)
                if crime is None or crime.get("kind") not in CRIME_KINDS or not f.get("npc"):
                    return
                r = next(
                    (r for r in self.rumors.values() if r.crime == crime["id"] and r.kind in CRIME_KINDS),
                    None,
                )
                if r is None:
                    npc = f["npc"]
                    r = Rumor(
                        f"crime:{crime['id']}",
                        crime["kind"],
                        npc,
                        self.social.names.get(npc, npc) if self.social else npc,
                        self.crimes.describe(crime),
                        crime=crime["id"],
                    )
                    self._crime_silence(r)
                    self.rumors[r.key] = r
                r.known[f["npc"]] = 0
                if "day" in crime:
                    r.since[f["npc"]] = int(crime["day"])
            case "testimony" if self.crimes is not None and f.get("npc"):
                for crime in self.crimes.testimony_crimes(f):
                    r = next(
                        (r for r in self.rumors.values() if r.crime == crime and r.kind in CRIME_KINDS),
                        None,
                    )
                    if r is not None and f["npc"] not in r.known:
                        r.known[f["npc"]] = 1
                        r.since[f["npc"]] = int(f.get("minute", int(f.get("day", 0)) * DAY)) // DAY
            case "rumor":
                if f.get("kind") in CRIME_KINDS and self.crimes is None:
                    return
                about = f.get("about") or f.get("from", "")
                for r in self.rumors.values():
                    if (
                        r.quest == f.get("quest") and r.about == about
                        and f.get("kind", r.kind) == r.kind and r.subject == rumor_subject(f)
                    ):
                        if f.get("to", "") not in r.known and "day" in f:
                            self.tells[(r.key, f.get("from", ""), int(f["day"]))] += 1
                        r.known.setdefault(f.get("to", ""), int(f.get("hops", 1)))
                        if "day" in f:
                            r.since.setdefault(f.get("to", ""), int(f["day"]))
                        if not r.about_name or r.about_name == about:
                            r.about_name = f.get("about_name") or f.get("name") or about
                        if not r.is_player and (not r.subject_name or r.subject_name == r.subject):
                            r.subject_name = f.get("subject_name") or r.subject
            case "broken":
                key = f.get("debt") or f.get("meet") or ""  # a debt, or a meeting he did not come to
                r = Rumor(key, "broken", f.get("npc", ""), f.get("name") or "", f.get("what", ""))
                r.known[r.about] = 0
                if "day" in f:
                    r.since[r.about] = int(f["day"])
                self.rumors.setdefault(r.key, r)
            case "deal" if f.get("silence"):
                for key in f["silence"]:
                    if key in self.rumors:
                        self.rumors[key].silent.add(f.get("npc", ""))
            case "forgiven" | "silenced" if f.get("crime"):
                if f.get("cover") and (self.crimes is None or not self.crimes.cover):
                    return
                for r in self.rumors.values():
                    if r.crime == f["crime"]:
                        r.silent.add(f.get("npc", ""))
            case "died":
                self.dead.add(f.get("npc", ""))

    def _crime_silence(self, r: Rumor) -> None:
        """A settlement also applies when a later lie starts another rumor about the same deed."""
        r.silent.update(
            f.get("npc", "")
            for f in self.chronicle.facts()
            if f.get("fact") in {"forgiven", "silenced"}
            and f.get("crime") == r.crime
            and (not f.get("cover") or (self.crimes is not None and self.crimes.cover))
        )

    def _on_message(self, msg: GameMessage) -> None:
        if isinstance(msg, Session):
            self._load()
            self.social = None  # the people of the new world
            self.clock = None

    def people_changed(self) -> None:
        """Someone came into the world (a character of the director's): the next tick takes the people again,
        its day and ties with them."""
        self.social = None

    def describe(self) -> list[str]:
        """The rumors and who knows them (for the console)."""
        names = self.social.names if self.social else {}
        out = []
        for r in self.rumors.values():
            by_hops: dict[int, list[str]] = defaultdict(list)
            for n, h in r.known.items():
                by_hops[h].append(names.get(n, n))
            spread = "; ".join(f"{h}: {', '.join(sorted(ns))}" for h, ns in sorted(by_hops.items()))
            subject = "" if r.is_player else f"{r.subject_name}: "
            out.append(f"{r.kind} {subject}{r.about_name} «{r.title}» — {spread}")
        return out


def _span(start: int, end: int) -> list[tuple[int, int]]:
    """A routine entry as intervals of one day (22:00–08:00 is two)."""
    if start < end:
        return [(start, end)]
    if start == end:
        return [(0, DAY)]
    return [(start, DAY), (0, end)]


def _entry(day: list[Spot], minute: int) -> Spot | None:
    """The entry of a day that holds the game minute: the first one, as the engine takes it."""
    m = minute % DAY
    return next((spot for spot in day if any(s <= m < e for s, e in _span(spot.start, spot.end))), None)


def _merged(spans: list[tuple[int, int]]) -> list[tuple[int, int]]:
    """Spans of a day, overlapping or touching ones as one."""
    out: list[tuple[int, int]] = []
    for s, e in sorted(spans):
        if out and s <= out[-1][1]:
            out[-1] = (out[-1][0], max(out[-1][1], e))
        else:
            out.append((s, e))
    return out


def _merged_at(spans: list[tuple[int, int, str]]) -> list[tuple[int, int, str]]:
    """Spans of a day at places: merged at each place as `_merged` does, never across places; sorted."""
    at: dict[str, list[tuple[int, int]]] = defaultdict(list)
    for s, e, wp in spans:
        at[wp].append((s, e))
    return sorted((s, e, wp) for wp, ss in at.items() for s, e in _merged(ss))


def _hm(minute: int) -> str:
    return f"{minute // 60:02d}:{minute % 60:02d}"


def _chance(*parts: str) -> float:
    """A number in [0, 1) that is the same for the same parts."""
    digest = hashlib.sha1("|".join(parts).encode("utf-8")).digest()
    return int.from_bytes(digest[:4], "big") / 2**32


def _count(social: Social) -> int:
    return sum(len(t) for t in social.ties.values()) // 2
