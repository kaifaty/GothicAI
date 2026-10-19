"""Everyday life from concerns (docs/npc-tools.md, wave 2 step 6): what an npc does in its next hour.

The behavioral model chooses an npc's next game hour from a menu, do{activity, where, how long}: keep the
routine, another activity of its own routine or a habit (brain/day.py), or a trip its everyday concerns
suggest. The menu is built here when the planner asks for one (`menu`), from the npc's routine, its role,
the time and the chronicle:
- fresh news (a rumor of the hero it still tells, social.py): go and tell an acquaintance who does not know
  it yet, when the lot of a meeting would let it pass today (`Rumors.can_tell`) and their routines do not
  bring them together within the hour anyway;
- a death it saw (`death_seen`, written here when it sees an acquaintance's body): pray to Innos at the
  nearest prayer spot with room;
- a loss (the hero took its reward by a lie, or did not pay back his debt): drink it away in a tavern.
These everyday concerns live in the menu only, never in its agenda (concerns.py): an npc that wants to
pray does not walk up to the hero about it.

The hour's menu:
- nothing is asked while the npc is on duty (a post, an escort: storyguard.py) or asleep;
- a plot npc keeps where it stands: keep, a habit there, another activity of its routine at that waypoint;
- trips only between TRIP_FROM and TRIP_TO, at most MAX_TRIPS, within TRIP_CM of its routine spot, never
  to someone asleep or standing at a freepoint off the waynet; outlaws neither pray nor go to a tavern;
- a spot has room when the people its routines put there now (not the npc itself) and the trips on their
  way to it are fewer than the most it ever holds at once (`Social.capacity`);
- never the state it is in now (the engine would only move it there), sleep, duty or speeches.

A chosen trip comes back through `take` (planner.on_plan): its spot is held for the stay (a spot holds as
many trips as it has room), the body walks there (`start_activity` with `arrive`) and no other act of the
planner is put on it meanwhile (a newer act would drop the arrival). Only a fight or a flight still takes
the body: the walk then ends at once as replaced (bodies.py). On arrival a chat passes the news by the
code's lot, as a visit (`offer_status`, how: visited); near the hero gossip.py stages it as a scene of the
two. A prayer or a drink is the activity itself. Every arrival leaves a `day` fact: who, what, to whom,
why, where, for how long, what came of it and who saw it. A trip that never got there leaves a journal
entry only, its spot is let go and its concern is offered again the next hour. Npc's off the hero's roster
are never asked: the rumors' schedule covers them (social.py).
"""

from __future__ import annotations

import asyncio
import logging
import math
from collections import Counter
from collections.abc import Callable, Collection, Coroutine, Iterable, Mapping
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any

from .bodies import Bodies, BodyOutcome
from .brain.apply import Decision
from .brain.day import (
    DayMenu,
    DayOption,
    Habit,
    Where,
    asleep_at,
    current_entry,
    duration_text,
    own_options,
    plan_options,
)
from .brain.planner import TAKES_BODY, NpcPlanner, NpcRecord
from .brain.situation import news_short
from .chronicle import Chronicle, Fact
from .game import GameError, GameLink
from .gossip import PROTOCOL as SAY_TO
from .gossip import WORDS_CM, GossipDesk
from .journal import Journal
from .people_knowledge import rumor_subject
from .places import PlaceBook
from .protocol import GameMessage, Loading, NpcView, RoutineEntry, Session
from .social import DAY, OUTLAWS, Rumor, Rumors, Social, Spot
from .storyguard import StoryGuard, asleep

if TYPE_CHECKING:
    from .initiative import InitiativeDesk
    from .perception import PerceptionDesk
    from .talk import TalkDesk

log = logging.getLogger(__name__)

TRIP_CM = 12000  # a trip goes this far at most, straight from the npc's routine spot
STAY_MIN = 45  # what a trip lasts after the walk there
CM_PER_GAME_MIN = 370  # walking: 45 m in 50 s, and a game hour is ~248 s
TRIP_FROM, TRIP_TO = 6 * 60, 21 * 60  # trips by day only (game minutes of the day)
MAX_TRIPS, MAX_OPTIONS = 2, 8
# arrived: this close to its waypoint; the walk there is waited for this long
TRIP_RADIUS_CM, TRIP_TIMEOUT_S = 500, 240.0
OFFER_TRIES, OFFER_GAP_S = 10, 1.5  # on arrival the news waits this long for busy bodies (gossip.py)
WALK_CM = 1500  # on arrival the one to be told must be this close (story_nearby)
PROTOCOL = SAY_TO  # start_activity's arrive came with say.to (v15): an older bridge gets no trips
# the places (places.json) of the taverns: a ZS_STAND_DRINKING spot there is a tavern's
TAVERNS = frozenset(
    {"CITY_TAVERN", "CITY_HABOUR_TAVERN01", "TAVERNE_IN", "TAVERNE", "CITY_MERCHANT_TAVERN01", "CITY_BEER"}
)
TRIPS = ("chat", "pray", "tavern")  # the kinds of the day options the desk walks
DRINK_STATE = "ZS_STAND_DRINKING"
PRAY_STATES = ("ZS_PRAY_INNOS_FP", "ZS_PRAY_INNOS")
CHAT_STATES = ("ZS_STAND_WP", "ZS_STAND_ARMSCROSSED")  # the second for one already standing at its place
PEOPLE_GUILDS = 16  # GIL_SEPERATOR_HUM: a body of a guild below it is a person's
# what the hero cost the npc, by the fact of it (the tavern's text)
LOSSES = {"found_out": "the reward the hero cheated you of", "broken": "the money the hero did not pay back"}


def routine_entries(person: dict[str, Any]) -> list[RoutineEntry]:
    """A person's day as `story_people` has it (minutes of the day) in the bridge's `routine` form ("HH:MM"),
    for the menus of people the game is not asked about (the probe, the tests)."""
    return [
        RoutineEntry(
            start=_hhmm(int(r["start"])),
            end=_hhmm(int(r["end"])),
            state=str(r.get("state") or ""),
            waypoint=str(r.get("waypoint") or ""),
        )
        for r in person.get("routine", [])
    ]


def trip_minutes(cm: float) -> int:
    """How long a trip this far lasts, from the order: the walk there and the stay."""
    return math.ceil(cm / CM_PER_GAME_MIN) + STAY_MIN


def chat_text(b_name: str, where: str | None, news: str | None, minutes: int) -> str:
    """The chat option as the model reads it; without `news` (the probe's control) it has no reason."""
    text = f"go to {b_name}" + (f" {where}" if where else "")
    if news is not None:
        text += f" to tell the news: {news}"
    return f"{text} ({duration_text(minutes)})"


@dataclass
class DayWorld:
    """What trip_options reads: the people's days, the rumors, the chronicle and the clock. The desk builds
    it from the game; the probe and the tests build it without one."""

    social: Social
    rumors: Iterable[Rumor]
    can_tell: Callable[[Rumor, str, str], bool]  # the lot of a meeting now (Rumors.can_tell)
    dead: Collection[str]
    facts: Callable[[str], list[Fact]]  # the chronicle's facts of a kind
    clock: int  # the game minute, since the first day began
    book: PlaceBook | None
    # waypoint (upper case) -> how many of other npc's trips hold it now
    reserved: Mapping[str, int] = field(default_factory=dict)
    trip_cm: int = TRIP_CM

    def free(self, waypoint: str, npc: str) -> bool:
        """Room for the npc at the waypoint now: those its routines put there now (not the npc itself, not
        the dead) and the trips that hold it are fewer than the most it ever holds at once."""
        wp = waypoint.upper()
        there = self.social.users(wp, self.clock) - {npc.upper()}
        there = {n for n in there if n not in self.dead}
        return len(there) + self.reserved.get(wp, 0) < self.social.capacity(wp)

    def where(self, waypoint: str) -> str | None:
        """The waypoint's place in words ("in Lobart's farmyard"), if the book knows it."""
        place = self.book.of(waypoint) if self.book is not None else None
        return place.where_en if place is not None else None


def day_menu(
    npc: str,
    guild: int,
    state: str,
    entries: list[Any],
    habits: list[Habit],
    hour: int,
    minute: int,
    guard: StoryGuard,
    world: DayWorld | None,
    where: Where,
    *,
    trips: bool = True,
) -> DayMenu:
    """Both menus of the npc's next hour: nothing to ask while it is on duty or asleep, a plot npc only stays
    where it stands; else the old menu and the do menu (keep, its trips, its own options, at most
    MAX_OPTIONS). `state` is the roster's (empty: unknown, its entry's is taken); `world` None (the people
    not known yet): no trips and every spot free; `trips` False (an older bridge): no trips. The desk asks it
    for the planner, the probe (evals/npc-decisions/day_probe.py) for the people of the routines' dump."""
    now = current_entry(entries, hour, minute)
    current = state or (now.state if now is not None else "")
    if (why := guard.on_duty(npc, current)) is not None:
        return DayMenu(stay=why)
    if asleep(current) or asleep_at(entries, hour, minute):
        return DayMenu(stay="спит")
    plot = guard.plot(npc)
    free = (lambda wp: world.free(wp, npc)) if world is not None else (lambda wp: True)
    plan = plan_options(entries, habits, hour, minute, state, free, plot)
    keep, *own = own_options(entries, habits, hour, minute, state, where, free, plot)
    found: list[DayOption] = []
    if trips and world is not None and not plot:
        try:
            found = trip_options(npc, guild, state, world)
        except Exception:
            log.exception("day: the trips of %s", npc)  # the menu goes on without them
    return DayMenu(plan, {o.key: o for o in [keep, *found, *own][:MAX_OPTIONS]})


def trip_options(npc: str, guild: int, state: str, world: DayWorld) -> list[DayOption]:
    """The trips the npc's everyday concerns offer now (one of each at most), the nearest first, at most
    MAX_TRIPS: a chat about fresh news, a prayer for a death it saw, a tavern for a loss. `state` is the
    one it is in now (the roster's)."""
    npc, state = npc.upper(), state.upper()
    minute = world.clock % DAY
    here = world.social.spot(npc, minute)
    if not TRIP_FROM <= minute < TRIP_TO or here is None or here.pos is None:
        return []
    found = [_chat_trip(npc, state, here, world)]
    if guild not in OUTLAWS:
        found += [_pray_trip(npc, state, here, world), _tavern_trip(npc, state, here, world)]
    trips = sorted((t for t in found if t is not None), key=lambda t: t[0])
    return [option for _, option in trips[:MAX_TRIPS]]


def _chat_trip(npc: str, state: str, here: Spot, world: DayWorld) -> tuple[float, DayOption] | None:
    """Fresh news it still tells: to the nearest acquaintance the lot would let it tell today, awake and on
    the waynet (a freepoint is walked to in a straight line), within reach, whom its routine does not bring
    it to within the hour anyway."""
    social, t = world.social, world.clock
    day, minute = t // DAY, t % DAY
    best: tuple[float, Rumor, str, Spot] | None = None
    for r in world.rumors:
        if npc not in r.known or not r.tells(npc, day):
            continue
        for b, tie in social.ties.get(npc, {}).items():  # the ties as they are: Social.tie() would make one
            there = social.spot(b, minute)
            if b in world.dead or there is None or there.pos is None or asleep(there.state):
                continue
            wp = there.waypoint.upper()
            if wp.startswith("FP_") or wp in world.reserved:
                continue
            cm = math.dist(here.pos, there.pos)
            if cm > world.trip_cm or (best is not None and cm >= best[0]):
                continue
            if next(tie.occasions(npc, b, t, t + 60), None) is not None or not world.can_tell(r, npc, b):
                continue
            best = (cm, r, b, there)
    if best is None:
        return None
    cm, r, b, there = best
    minutes = trip_minutes(cm)
    news = (news_short(r.kind, r.about_name, about_you=r.about == npc)
            if r.is_player else r.description())
    text = chat_text(social.names.get(b, b), world.where(there.waypoint), news, minutes)
    stand = CHAT_STATES[1] if state == CHAT_STATES[0] else CHAT_STATES[0]
    args = {"state": stand, "waypoint": there.waypoint, "minutes": minutes}
    return cm, DayOption("chat", f"chat:{b}", text, args, "chat", f"news:{r.key}", b, trip=True)


def _pray_trip(npc: str, state: str, here: Spot, world: DayWorld) -> tuple[float, DayOption] | None:
    """A death it saw, still fresh and not prayed for: the nearest prayer spot with room, within reach. Not
    while it prays or stands at one already."""
    if state in PRAY_STATES:
        return None
    day, done = world.clock // DAY, _done(world, npc)
    seen = [
        f
        for f in world.facts("death_seen")
        if f.get("npc") == npc and _fresh(f, day) and f"death:{f.get('dead')}" not in done
    ]
    spots = _spots(world, PRAY_STATES) if seen else {}
    if not seen or here.waypoint.upper() in spots or (spot := _nearest(npc, here, spots, world)) is None:
        return None
    cm, there = spot
    dead = str(seen[-1].get("dead"))
    dead_name = seen[-1].get("dead_name") or dead
    minutes = trip_minutes(cm)
    text = f"{_at(f'pray to Innos for {dead_name}', there, world)} ({duration_text(minutes)})"
    args = {"state": there.state, "waypoint": there.waypoint, "minutes": minutes}
    return cm, DayOption("pray", f"pray:{dead}", text, args, "pray", f"death:{dead}", trip=True)


def _tavern_trip(npc: str, state: str, here: Spot, world: DayWorld) -> tuple[float, DayOption] | None:
    """A loss, still fresh and not drunk away: the nearest tavern's drinking spot with room, within reach (a
    barkeeper's, a cook's or a herald's spot there is none). Not while it drinks or is in a tavern already."""
    if world.book is None or state == DRINK_STATE or _place(world, here.waypoint) in TAVERNS:
        return None
    day, done = world.clock // DAY, _done(world, npc)
    losses = [
        (int(f["day"]), kind, f"{kind}:{f.get('quest') or f.get('debt') or ''}")
        for kind in LOSSES
        for f in world.facts(kind)
        if f.get("npc") == npc and _fresh(f, day)
    ]
    losses = [loss for loss in losses if f"loss:{loss[2]}" not in done]
    if not losses:
        return None
    spots = {wp: s for wp, s in _spots(world, (DRINK_STATE,)).items() if _place(world, wp) in TAVERNS}
    if (spot := _nearest(npc, here, spots, world)) is None:
        return None
    cm, there = spot
    _, kind, key = max(losses, key=lambda loss: loss[0])  # the latest
    minutes = trip_minutes(cm)
    text = f"{_at(f'drink away {LOSSES[kind]}', there, world)} ({duration_text(minutes)})"
    args = {"state": DRINK_STATE, "waypoint": there.waypoint, "minutes": minutes}
    return cm, DayOption("tavern", f"tavern:{key}", text, args, "tavern", f"loss:{key}", trip=True)


def _spots(world: DayWorld, states: tuple[str, ...]) -> dict[str, Spot]:
    """The waypoints (upper case) of the routine entries in these states, each with one such entry."""
    out: dict[str, Spot] = {}
    for _, spot in world.social.spots():
        if spot.state in states and spot.pos is not None:
            out.setdefault(spot.waypoint.upper(), spot)
    return out


def _nearest(npc: str, here: Spot, spots: dict[str, Spot], world: DayWorld) -> tuple[float, Spot] | None:
    """The nearest of the spots with room for the npc, if it is within reach."""
    assert here.pos is not None
    room = [
        (math.dist(here.pos, s.pos), s)
        for wp, s in spots.items()
        if s.pos is not None and world.free(wp, npc)
    ]
    best = min(room, key=lambda x: x[0], default=None)
    return best if best is not None and best[0] <= world.trip_cm else None


def _done(world: DayWorld, npc: str) -> set[str]:
    """The concerns the npc's trips have seen to (its `day` facts)."""
    return {str(f.get("concern")) for f in world.facts("day") if f.get("npc") == npc}


def _fresh(f: Fact, day: int) -> bool:
    """Still on its mind: the day it happened and the next (a fact without its day is not)."""
    return "day" in f and day <= int(f["day"]) + 1


def _place(world: DayWorld, waypoint: str) -> str | None:
    place = world.book.of(waypoint) if world.book is not None else None
    return place.id if place is not None else None


def _at(text: str, there: Spot, world: DayWorld) -> str:
    where = world.where(there.waypoint)
    return f"{text} {where}" if where else text


def _hhmm(minute: int) -> str:
    return f"{minute % DAY // 60:02d}:{minute % 60:02d}"


# --- the desk --------------------------------------------------------------------------------------


class DayDesk:
    """The day menus of the npc's the planner asks, the trips they choose, and the deaths they see."""

    def __init__(
        self,
        link: GameLink,
        rumors: Rumors,
        chronicle: Chronicle,
        bodies: Bodies,
        guard: StoryGuard,
        perception: PerceptionDesk,
        talk: TalkDesk,
        book: PlaceBook | None,
        *,
        journal: Journal | None = None,
    ) -> None:
        self.link = link
        self.rumors = rumors
        self.chronicle = chronicle
        self.bodies = bodies
        self.guard = guard
        self.perception = perception
        self.talk = talk
        self.book = book
        self.journal = journal or Journal()
        self.planner: NpcPlanner | None = None  # follow(): the menus, the trips, the veto
        self.gossip: GossipDesk | None = None
        self.initiative: InitiativeDesk | None = None
        self.walking: dict[str, DayOption] = {}  # npc -> the trip it is on, until it is over
        self.menus: dict[str, DayMenu] = {}  # npc -> its last menus: the ones its flight asks
        self.reserved: dict[str, dict[str, int]] = {}  # waypoint -> {npc: until game minute}, per trip
        self.trips = 0  # trips begun (tests, the e2e)
        self.last: tuple[str, DayOption, str] | None = None  # the latest trip's end: npc, option, outcome
        self._here: dict[str, str | None] = {}  # npc -> its routine's waypoint when its menu was built
        self._mourned: set[tuple[str, str]] = set()  # (npc, dead) of the death_seen facts
        self._world = 0  # bumped on every load: a trip of the world before is over
        self._next_allow: Callable[[NpcRecord, str], str | None] | None = None
        self._background: set[asyncio.Task[Any]] = set()
        perception.on_view.append(self._on_view)
        link.listeners.append(self._on_message)
        self._load()

    def follow(
        self,
        planner: NpcPlanner,
        gossip: GossipDesk | None = None,
        initiative: InitiativeDesk | None = None,
    ) -> None:
        """The planner's day hooks (its menus, a chosen trip) and a veto chained before the hooks already
        there (gossip's, which chains the initiative's: set those up first)."""
        self.planner = planner
        self._next_allow, planner.allow = planner.allow, self.allow
        planner.menu_of, planner.on_plan = self.menu, self.take
        self.gossip, self.initiative = gossip, initiative

    # --- the menus --------------------------------------------------------------------------------

    def menu(self, rec: NpcRecord, routine: Any, hour: int, minute: int, habits: list[Habit]) -> DayMenu:
        """Both menus of the npc's next hour (planner.menu_of, `day_menu`), kept as the ones its flight
        asks. Trips need the rumors' clock and a bridge that tells an arrival."""
        npc, entries = rec.id, routine.entries
        now = current_entry(entries, hour, minute)
        self._here[npc] = now.waypoint if now is not None else None
        world = self.world(npc, hour, minute)
        hello = self.link.hello
        trips = self.rumors.clock is not None and hello is not None and hello.protocol >= PROTOCOL
        guard, where = self.guard, self._where
        menu = day_menu(
            npc, rec.guild, rec.state, entries, habits, hour, minute, guard, world, where, trips=trips
        )
        self.menus[npc] = menu
        return menu

    def world(self, npc: str, hour: int, minute: int) -> DayWorld | None:
        """What trip_options reads for the npc at that time of today; None before the rumors know the
        world's people."""
        social, clock = self.rumors.social, self.rumors.clock
        if social is None:
            return None
        now = (clock // DAY if clock is not None else 0) * DAY + hour * 60 + minute
        rumors = self.rumors.rumors.values()
        held = self._held(npc, now)
        return DayWorld(
            social, rumors, self.rumors.can_tell, self.rumors.dead, self.chronicle.facts, now, self.book, held
        )

    def _room(self, rec: NpcRecord, kind: str, waypoint: str) -> bool:
        """The menu's rule for a trip's spot, asked again now: a chat's partner is held by no other trip, a
        prayer's or a drink's spot still has room for the npc (its routines' people and the trips held)."""
        minute = self._now() % DAY
        world = self.world(rec.id, minute // 60, minute % 60)
        if world is None:
            return True  # no people known: no trips were offered either
        wp = waypoint.upper()
        return wp not in world.reserved if kind == "chat" else world.free(wp, rec.id)

    def _held(self, npc: str, now: int) -> Counter[str]:
        """How many of other npc's trips hold each waypoint at that game minute (only those held); a hold
        whose stay is over is let go."""
        held: Counter[str] = Counter()
        for wp, holds in list(self.reserved.items()):
            for who, until in list(holds.items()):
                if until <= now:
                    del holds[who]
                elif who != npc:
                    held[wp] += 1
            if not holds:
                del self.reserved[wp]
        return held

    def _where(self, waypoint: str) -> str | None:
        place = self.book.of(waypoint) if self.book is not None else None
        return place.where_en if place is not None else None

    # --- the planner's hooks ----------------------------------------------------------------------

    def allow(self, rec: NpcRecord, choice: str) -> str | None:
        """Why the planner must not apply this choice now, or None (then the hook before decides). A body
        on its way stays on it; a choice of the day menu is checked once more (a backstop to the menu): the
        story's rules, and for a trip whether the body is free and its spot still has room."""
        if rec.id in self.walking and choice not in TAKES_BODY:
            return "идёт по делу"  # a newer act would drop its arrival
        if (chosen := self._chosen(rec.id, choice)) is not None:
            kind, waypoint = chosen
            trip = kind in TRIPS
            if (why := self.guard.on_duty(rec.id, rec.state)) is not None:
                return why
            if self.guard.plot(rec.id) and (trip or waypoint != self._here.get(rec.id)):
                return "сюжетный: только слова"
            if trip:
                if rec.activity != "routine" or self.bodies.busy(rec.id) or self._elsewhere(rec.id):
                    return "занят"
                partner = self.talk.partner
                if partner is not None and partner.npc == rec.id:
                    return "говорит с героем"
                if not self._room(rec, kind, str(waypoint)):
                    return "место занято"
        return self._next_allow(rec, choice) if self._next_allow is not None else None

    def _chosen(self, npc: str, choice: str) -> tuple[str, str | None] | None:
        """A choice of the menu the npc's flight asked, by the question applied (`do` with apply_day, else
        the old `plan`): its kind (a trip's is one of TRIPS; the old menu's are "routine") and where it goes;
        None for any other (a reaction's)."""
        menu = self.menus.get(npc)
        if menu is None or self.planner is None:
            return None
        if self.planner.cfg.apply_day:
            option = menu.do.get(choice)
            if option is None or option.args is None:
                return None
            return option.kind, option.args.get("waypoint")
        args = menu.plan[1].get(choice)
        return ("routine", args.get("waypoint")) if args is not None else None

    def _elsewhere(self, npc: str) -> bool:
        """It is busy with another desk: a scene of two (gossip.py), a walk up to the hero (initiative.py)."""
        busy = self.gossip is not None and self.gossip.busy(npc)
        return busy or (self.initiative is not None and self.initiative.active == npc)

    def take(self, decision: Decision, rec: NpcRecord, option: DayOption) -> None:
        """A trip chosen (planner.on_plan): its spot is held for the stay, the body walks there."""
        args = option.args or {}
        until = self._now() + int(args.get("minutes", 0))
        self.reserved.setdefault(str(args.get("waypoint", "")).upper(), {})[rec.id] = until
        self.walking[rec.id] = option
        self.trips += 1
        log.info("day: %s goes for %s (%s)", rec.id, option.id, option.concern)
        self.journal.add(
            "day_trip", npc=rec.id, name=rec.name, what=option.kind, to=option.partner, id=option.id
        )
        self._spawn(self._trip(rec.id, rec.name, option, self._world))

    # --- the trip ---------------------------------------------------------------------------------

    async def _trip(self, npc: str, name: str, opt: DayOption, world: int) -> None:
        """The walk there; on arrival what comes of it, and its fact. It is `walking` until this is over."""
        args = opt.args or {}
        since = len(self.chronicle.facts("rumor"))  # a pass to the partner from here on is this trip's
        try:
            outcome = await self.bodies.act(
                npc,
                "start_activity",
                **args,
                arrive=True,
                radius=TRIP_RADIUS_CM,
                timeout=TRIP_TIMEOUT_S,
            )
            if world != self._world:
                return  # a load: the walk and its spot went with the world
            if outcome.status != "arrived":
                self._failed(npc, name, opt, outcome)
                return
            result = await self._chat(npc, opt, since) if opt.kind == "chat" else "done"
            if result is not None and world == self._world:
                self._record(npc, name, opt, result)
        finally:
            if world == self._world and self.walking.get(npc) is opt:
                del self.walking[npc]

    async def _chat(self, npc: str, opt: DayOption, since: int) -> str | None:
        """There: told if the news passed to the partner (by this offer, or by the view trigger on the way:
        gossip.py), knew if the partner knows it now, held if the lot or the busy bodies kept it, not there
        if the partner is not close; None after a load."""
        world, b = self._world, str(opt.partner)
        r = self.rumors.rumors.get(str(opt.concern).removeprefix("news:"))
        if r is None:
            return "held"
        if self._told(r, npc, b, since):
            return "told"
        try:
            near = await self.link.story_nearby(npc, WALK_CM)
        except (GameError, TimeoutError) as err:
            log.info("day: who is near %s: %s", npc, err or type(err).__name__)
            return "not there"  # the teller is not in the world
        if world != self._world:
            return None
        if not any(str(p.get("npc", "")).upper() == b for p in near):
            return "not there"
        status = "no"
        for attempt in range(OFFER_TRIES):
            if attempt:
                await asyncio.sleep(OFFER_GAP_S)
                if world != self._world:
                    return None
            if self._told(r, npc, b, since):
                return "told"
            status = self.rumors.offer_status(r, npc, b, how="visited")
            if status != "wait":
                break
        if status == "told" or self._told(r, npc, b, since):
            return "told"
        return "knew" if b in r.known else "held"

    def _told(self, r: Rumor, a: str, b: str, since: int) -> bool:
        """A rumor fact of `r` from a to b among the facts after the first `since`."""
        return any(
            f.get("from") == a
            and rumor_subject(f) == r.subject
            and f.get("to") == b
            and f.get("quest") == r.quest
            and f.get("about") == r.about
            and f.get("kind", r.kind) == r.kind
            for f in self.chronicle.facts("rumor")[since:]
        )

    def _record(self, npc: str, name: str, opt: DayOption, outcome: str) -> None:
        """The arrival's fact (principle 4): who, what, to whom, why, where, for how long, what came of it and
        who saw it."""
        now, args = self._now(), opt.args or {}
        place = self.book.of(args.get("waypoint")) if self.book is not None else None
        fact: Fact = {"fact": "day", "npc": npc, "name": name, "what": opt.kind}
        if opt.partner is not None:
            fact |= {"to": opt.partner, "to_name": self._name(opt.partner)}
        fact |= {
            "concern": opt.concern,
            "where": place.id if place is not None else None,
            "minutes": args.get("minutes"),
            "outcome": outcome,
            "seen_by": self._seen_by(npc),
            "day": now // DAY,
            "hour": now % DAY // 60,
            "minute": now % 60,
        }
        self.chronicle.record(fact)
        self.last = (npc, opt, outcome)
        self.journal.add("day", npc=npc, name=name, what=opt.kind, to=opt.partner, outcome=outcome)

    def _failed(self, npc: str, name: str, opt: DayOption, outcome: BodyOutcome) -> None:
        """It never got there (gone, a timeout, rejected, lost, failed, or done: in the state and out of it
        before it arrived): a journal entry, its spot is free again and the concern stays."""
        wp = str((opt.args or {}).get("waypoint", "")).upper()
        holds = self.reserved.get(wp, {})
        holds.pop(npc, None)
        if not holds:
            self.reserved.pop(wp, None)
        why = outcome.status + (f": {outcome.detail}" if outcome.detail else "")
        log.info("day: %s did not get there for %s (%s)", npc, opt.id, why)
        self.journal.add("day_failed", npc=npc, name=name, what=opt.kind, id=opt.id, why=why)
        self.last = (npc, opt, f"failed: {outcome.status}")

    def _seen_by(self, npc: str) -> list[str]:
        """The hero if he is close or has the npc on his screen, and the npc's whose views see it."""
        entry = self.talk.people.get(npc)
        hero = entry is not None and (entry.distance <= WORDS_CM or entry.in_view)
        others = sorted(
            v.npc.upper()
            for v in self.perception.views()
            if v.npc.upper() != npc and any(o.id.upper() == npc for o in v.sees)
        )
        return ["PLAYER", *others] if hero else others

    # --- deaths seen ------------------------------------------------------------------------------

    def _on_view(self, view: NpcView) -> None:
        """A body the npc sees: an acquaintance's (a person's, not a creature's) is a death it saw, once."""
        social = self.rumors.social
        if social is None:
            return
        viewer = view.npc.upper()
        known = social.ties.get(viewer, {})
        for o in view.sees:
            dead = o.id.upper()
            if o.kind != "body" or o.guild >= PEOPLE_GUILDS or dead not in known:
                continue
            if (viewer, dead) in self._mourned:
                continue
            self._mourned.add((viewer, dead))
            at = view.time if view.time is not None else self.rumors.clock
            place = self.perception.place(view)
            fact: Fact = {
                "fact": "death_seen",
                "npc": viewer,
                "name": view.name,
                "dead": dead,
                "dead_name": o.name,
                "where": place.id if place is not None else None,
            }
            if at is not None:
                fact |= {"day": at // DAY, "hour": at % DAY // 60, "minute": at % 60}
            self.chronicle.record(fact)
            log.info("day: %s saw %s dead", viewer, dead)

    # --- the game ---------------------------------------------------------------------------------

    def _on_message(self, msg: GameMessage) -> None:
        match msg:
            case Loading():
                self._new_world()
            case Session():
                self._new_world()
                self._load()

    def _new_world(self) -> None:
        """A load: the trips under way are over (no fact), their spots free, the menus asked before moot."""
        self._world += 1
        self.walking.clear()
        self.reserved.clear()
        self.menus.clear()
        self._here.clear()

    def _load(self) -> None:
        """The deaths each npc saw already, from the facts of the save branch."""
        self._mourned = {
            (str(f.get("npc", "")).upper(), str(f.get("dead", "")).upper())
            for f in self.chronicle.facts("death_seen")
        }

    # --- helpers ----------------------------------------------------------------------------------

    def _now(self) -> int:
        """The game minute (since the first day began) as the rumors last read it; 0 before."""
        return self.rumors.clock or 0

    def _name(self, npc: str) -> str:
        entry = self.talk.people.get(npc)
        if entry is not None:
            return entry.name
        social = self.rumors.social
        return social.names.get(npc, npc) if social is not None else npc

    def _spawn(self, coro: Coroutine[Any, Any, Any]) -> None:
        async def run() -> None:
            try:
                await coro
            except (GameError, TimeoutError) as err:
                log.info("day: %s", err or type(err).__name__)
            except Exception:
                log.exception("day: a trip failed")

        task = asyncio.get_running_loop().create_task(run())
        self._background.add(task)
        task.add_done_callback(self._background.discard)
