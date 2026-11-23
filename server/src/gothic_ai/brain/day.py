"""An npc's day for the behavioral model: what it may do in the next hour.

The menu is built from the npc's own routine (`routine` from the game, protocol v6: entries with time, a
ZS_ state and a waypoint) and its habits (a few lines in the persona). Only activities of the routine it
is in now are offered, never its other routine variants: those belong to the story (a ritual, a death).
A choice becomes `start_activity` for an hour; afterwards the routine goes on by itself.

An npc asleep by its routine has nothing to choose; sleep, duty and speeches are never offered, nor the
state it is in now (the engine would only move it, `startState` keeps the old end time) or a waypoint the
others' routines fill at that time.

Two menus come from it (docs/npc-tools.md, wave 2 step 6). The old one (`plan_options`: ids a1/h1 and
their start_activity args) is the one applied while the new one is measured in shadow. The new `do` menu
is a list of DayOption: each has a short key the model reads (keep_routine, a<n>, h<n>, chat, pray,
tavern) and a stable id for traces and training (`routine:<STATE>@<WAYPOINT>`...), and its text says where
and for how long. Its own part is `own_options`; trips to other people and places (daylife.py) go between
keep and these.
"""

from __future__ import annotations

from collections.abc import Callable, Collection
from dataclasses import dataclass, field
from typing import Any

# what common routine states look like, in the words the model reads (English); every state of NEWWORLD's
# routines has one (counted from the routines' dump)
ACTIVITIES = {
    "ZS_STAND_ARMSCROSSED": "stand about with your arms crossed",
    "ZS_STAND_GUARDING": "stand guard",
    "ZS_STAND_EATING": "stand and eat",
    "ZS_STAND_DRINKING": "stand and drink",
    "ZS_STAND_SWEEPING": "sweep the floor",
    "ZS_STAND_WP": "stand at your place",
    "ZS_SIT_BENCH": "sit on a bench",
    "ZS_SIT_CHAIR": "sit on a chair",
    "ZS_SIT_THRONE": "sit on your throne",
    "ZS_SIT_CAMPFIRE": "sit by the campfire",
    "ZS_SIT_GROUND": "sit on the ground",
    "ZS_GOTOBED": "sleep in your bed",
    "ZS_SLEEP": "sleep in your bed",
    "ZS_SLEEP_GROUND": "sleep on the ground",
    "ZS_SMALLTALK": "chat with the people around",
    "ZS_READ_BOOKSTAND": "read at your bookstand",
    "ZS_STUDY_WP": "study at your desk",
    "ZS_POTION_ALCHEMY": "brew potions at the alchemy bench",
    "ZS_PRACTICE_MAGIC": "practise magic",
    "ZS_PRACTICE_SWORD": "practise with your sword",
    "ZS_SMITH_FIRE": "work at the forge",
    "ZS_SMITH_ANVIL": "hammer at the anvil",
    "ZS_SMITH_COOL": "cool a blade",
    "ZS_SMITH_SHARP": "sharpen blades",
    "ZS_COOK_CAULDRON": "cook at the cauldron",
    "ZS_COOK_STOVE": "cook at the stove",
    "ZS_COOK_PAN": "fry something in a pan",
    "ZS_ROAST_SCAVENGER": "roast meat over the fire",
    "ZS_PICK_FP": "work in the field",
    "ZS_PICK_ORE": "dig for ore",
    "ZS_RAKE_FP": "rake the field",
    "ZS_SWEEP_FP": "sweep the yard",
    "ZS_PRAY_INNOS": "pray to Innos",
    "ZS_PRAY_INNOS_FP": "pray to Innos",
    "ZS_PRAY_SLEEPER": "pray to the Sleeper",
    "ZS_PREACH_VATRAS": "preach",
    "ZS_ANNOUNCE_HEROLD": "read out the news",
    "ZS_PLAY_LUTE": "play the lute",
    "ZS_DANCE": "dance",
    "ZS_SMOKE_JOINT": "smoke a swampweed joint",
    "ZS_SMOKE_WATERPIPE": "smoke the water pipe",
    "ZS_STOMP_HERB": "stomp herbs",
    "ZS_SAW": "saw wood",
    "ZS_REPAIR_HUT": "repair the hut",
    "ZS_WASH_FP": "wash yourself",
    "ZS_PEE": "relieve yourself",
    "ZS_GUARD_PASSAGE": "guard the passage",
    "ZS_GUARD_HAMMER": "guard the hammer",
}
# states that are not everyday activities: never offered
NOT_ACTIVITIES = ("ZS_DEAD", "ZS_TALK", "ZS_ATTACK", "ZS_FLEE", "ZS_GUIDE", "ZS_FOLLOW", "ZS_UNCONSCIOUS")
# every sleep entry of the routines is ZS_GOTOBED; ZS_SLEEP and ZS_SLEEP_GROUND are the scripts' own
SLEEP_STATES = ("ZS_GOTOBED", "ZS_SLEEP")
# never offered as another activity either (prefixes, as NOT_ACTIVITIES): sleep and the pee spot; the sword
# practice draws a weapon (the roster then says "fight" and the planner lets the npc be); the herald's and
# Vatras's speeches and the guards' duty belong to their routines; a smalltalk without a partner is a
# silent stand
NOT_OFFERED = (
    *NOT_ACTIVITIES,
    *SLEEP_STATES,
    "ZS_PEE",
    "ZS_PRACTICE_SWORD",
    "ZS_ANNOUNCE_HEROLD",
    "ZS_PREACH_VATRAS",
    "ZS_GUARD_PASSAGE",
    "ZS_GUARD_HAMMER",
    "ZS_STAND_GUARDING",
    "ZS_SMALLTALK",
)
KEEP = "keep_routine"
OWN_MIN = 60  # an own activity lasts an hour; then the routine goes on by itself
DAY = 24 * 60
Free = Callable[[str], bool]  # the waypoint is free now: others' routines leave room there
Where = Callable[[str], str | None]  # a waypoint's place in words ("in the fields of Lobart's farm"), or None


@dataclass(frozen=True)
class Habit:
    """Something the npc likes to do besides its routine (persona): a state at a waypoint (None: where the
    routine has it now)."""

    text: str  # English
    state: str
    waypoint: str | None = None


@dataclass(frozen=True)
class DayOption:
    """One choice of the `do` menu."""

    key: str  # on the wire, what the model reads: keep_routine | a<n> | h<n> | chat | pray | tavern
    id: str  # stable, for traces and training labels: keep_routine, routine:<STATE>@<WAYPOINT>, chat:<NPC>...
    text: str  # English
    args: dict[str, Any] | None  # start_activity {state, waypoint, minutes}; None: keep the routine
    kind: str  # keep | routine | habit | chat | pray | tavern
    concern: str | None = None  # why a trip is offered: news:<rumor key> | death:<npc> | loss:<fact key>
    partner: str | None = None  # chat: whom to tell
    trip: bool = False  # the day desk walks it there (arrive) instead of the applier


@dataclass(frozen=True)
class DayMenu:
    """An npc's menus for the next hour, as the planner asks them."""

    # the old menu (texts, args by id a1/h1): the one applied while the new one is in shadow
    plan: tuple[dict[str, str], dict[str, dict[str, Any]]] = field(default_factory=lambda: ({}, {}))
    do: dict[str, DayOption] = field(default_factory=dict)  # by wire key, keep_routine first
    stay: str | None = None  # why it is not asked at all (on duty, asleep), or None


def option_id(kind: str, state: str, waypoint: str) -> str:
    """The stable id of an own activity: routine:ZS_SIT_CAMPFIRE@FARM_FIRE."""
    return f"{kind}:{state.upper()}@{waypoint.upper()}"


def plan_ids(args: dict[str, dict[str, Any]]) -> dict[str, str]:
    """The old menu's ids (a<n>, h<n>; their args from plan_options) as the stable ids of the do menu."""
    return {
        key: option_id("habit" if key.startswith("h") else "routine", a["state"], a["waypoint"])
        for key, a in args.items()
    }


def describe(state: str) -> str:
    return ACTIVITIES.get(state.upper(), state.upper().removeprefix("ZS_").replace("_", " ").lower())


def _minutes(hhmm: str) -> int:
    h, m = hhmm.split(":")
    return int(h) * 60 + int(m)


def current_entry(entries: list[Any], hour: int, minute: int) -> Any | None:
    now = hour * 60 + minute
    for e in entries:
        start, end = _minutes(e.start), _minutes(e.end)
        if (start <= now < end) if start < end else (now >= start or now < end):
            return e
    return entries[0] if entries else None


def asleep_at(entries: list[Any], hour: int, minute: int) -> bool:
    """Its routine has it asleep at this time (its entry now is a sleep one): it is not asked at all."""
    now = current_entry(entries, hour, minute)
    return now is not None and now.state.upper().startswith(SLEEP_STATES)


def duration_text(minutes: int) -> str:
    """How long an option lasts, as the model reads it."""
    if minutes < 45:
        return "about half an hour"
    return "about an hour" if minutes < 90 else "about two hours"


def _others(
    entries: list[Any],
    habits: list[Habit],
    hour: int,
    minute: int,
    current_state: str,
    free: Free,
    here_only: bool,
    *,
    catalog: Collection[str] | None = None,
    soonest: bool = False,
) -> tuple[list[Any], list[tuple[Habit, str]]]:
    """What else the npc may do now: the other entries of its routine (each state and waypoint once, in
    routine order or, `soonest`, the soonest to start first) and its habits with their waypoints.

    Left out: anything while it sleeps, sleep and the states of NOT_OFFERED, an option in the state it is
    in now (`current_state`, the roster's; unknown: its entry's), as the engine would only move it there and
    keep the old end time, and a waypoint that is not `free` (others' routines fill it). `here_only` (a plot
    npc): only what it can do where it stands, its entry's waypoint."""
    now = current_entry(entries, hour, minute)
    here = now.waypoint if now is not None else None
    current = (current_state or (now.state if now is not None else "")).upper()
    if asleep_at(entries, hour, minute) or current.startswith(SLEEP_STATES):
        return [], []

    def offered(state: str, waypoint: str) -> bool:
        state = state.upper()
        return (
            not state.startswith(NOT_OFFERED)
            and state != current
            and (catalog is None or state in catalog)
            and (not here_only or waypoint == here)
            and free(waypoint)
        )

    at = hour * 60 + minute
    order = sorted(entries, key=lambda e: (_minutes(e.start) - at) % DAY) if soonest else entries
    seen = {(now.state, now.waypoint)} if now is not None else set()
    routine = []
    for e in order:
        if (e.state, e.waypoint) in seen or not offered(e.state, e.waypoint):
            continue
        seen.add((e.state, e.waypoint))
        routine.append(e)
    found = []
    for h in habits:
        waypoint = h.waypoint or here
        if not waypoint or (h.state, waypoint) in seen or not offered(h.state, waypoint):
            continue
        seen.add((h.state, waypoint))
        found.append((h, waypoint))
    return routine, found


def _activity_args(state: str, waypoint: str, catalog_only: bool) -> dict[str, Any]:
    args = {"state": state, "waypoint": waypoint, "minutes": OWN_MIN}
    if catalog_only:
        args["catalog_only"] = True
    return args


def plan_options(
    entries: list[Any],
    habits: list[Habit],
    hour: int,
    minute: int,
    current_state: str = "",
    free: Free = lambda wp: True,
    here_only: bool = False,
    *,
    catalog: Collection[str] | None = None,
) -> tuple[dict[str, str], dict[str, dict[str, Any]]]:
    """The old menu (option id -> text) and what each option does (id -> start_activity args): the other
    activities of its routine (a1, a2...) and its habits (h<n>), each for an hour. What is left out: see
    `_others`; only keep while it sleeps."""
    now = current_entry(entries, hour, minute)
    texts = {
        KEEP: f"keep to your usual routine now: {describe(now.state)}" if now else "keep to your routine"
    }
    args: dict[str, dict[str, Any]] = {}
    routine, found = _others(entries, habits, hour, minute, current_state, free, here_only, catalog=catalog)
    for e in routine:
        key = f"a{len(args) + 1}"
        texts[key] = f"{describe(e.state)} (you usually do it at {e.start}-{e.end})"
        args[key] = _activity_args(e.state, e.waypoint, catalog is not None)
    for h, waypoint in found:
        key = f"h{len(args) + 1}"
        texts[key] = h.text
        args[key] = _activity_args(h.state, waypoint, catalog is not None)
    return texts, args


def own_options(
    entries: list[Any],
    habits: list[Habit],
    hour: int,
    minute: int,
    current_state: str,
    where: Where,
    free: Free = lambda wp: True,
    here_only: bool = False,
    *,
    catalog: Collection[str] | None = None,
) -> list[DayOption]:
    """The do menu's own part: keep first, then the other activities of its routine (a<n>, the soonest to
    start first) and its habits (h<n>, continuing the count), each for an hour, with where they are
    (`where`: `PlaceBook.of(wp).where_en`). Left out as in the old menu (`_others`); only keep while it
    sleeps. The caller puts trips after keep and cuts the menu to its size."""
    now = current_entry(entries, hour, minute)
    usual = f"keep to your usual routine: {_at(describe(now.state), now.waypoint, where)}" if now else None
    options = [DayOption(KEEP, KEEP, usual or "keep to your routine", None, "keep")]
    routine, found = _others(
        entries, habits, hour, minute, current_state, free, here_only, catalog=catalog, soonest=True
    )
    long = duration_text(OWN_MIN)
    for e in routine:
        text = f"{_at(describe(e.state), e.waypoint, where)} ({long}; usually {e.start}-{e.end})"
        args = _activity_args(e.state, e.waypoint, catalog is not None)
        options.append(
            DayOption(f"a{len(options)}", option_id("routine", e.state, e.waypoint), text, args, "routine")
        )
    for h, waypoint in found:
        text = f"{_at(h.text, waypoint, where)} ({long})"
        args = _activity_args(h.state, waypoint, catalog is not None)
        options.append(
            DayOption(f"h{len(options)}", option_id("habit", h.state, waypoint), text, args, "habit")
        )
    return options


def _at(text: str, waypoint: str, where: Where) -> str:
    """`text` and where it is done, when the book knows the place."""
    place = where(waypoint)
    return f"{text} {place}" if place else text
