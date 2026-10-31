"""Who asks the decision model, when, and what happens to the answer.

Every npc the game reports has a tier (tiers.py). It gets a decision:
- on an event it noticed (stimuli.py): NEAR npc's on any, MID only on salient ones; urgent events
  of NEAR npc's go to the decider's urgent lane;
- when its action queue runs empty (NEAR), but not more often than every `near_min_gap_s`;
- on a timer with a random phase: NEAR every 6-10 s, MID 30-60 s, FAR about once per game hour
  (a plan, not a reaction); an npc coming in from FAR gets a quick catch-up decision.
Idle and timer reactions of NEAR/MID npc's are only asked when there is something to react to: a fresh
stimulus or the npc's own `agenda` (what it wants from the player). With neither, asking the model
gives a vague answer at best, so the check is skipped and the npc keeps its routine.

The day plan (what to do in the next hour, from the npc's own routine: brain/day.py) runs on its own
timer: about once a game hour, for NEAR/MID npc's every `plan_period_s`. An agenda does not cancel it; a
live stimulus (a scripted one too: the game's scripts are reacting) holds it, and so does a body that is
not doing what its routine has it do now. Without its routine and the game time there is no plan. The
routine is asked from the game in the background, again after `routine_ttl_s` (scripts swap routines) and,
after a failure, with a growing pause, never on every check.

A plan request asks two menus at once, with one state (brain/day.py): `plan`, the old menu of the npc's
own activities, and `do`, the new one, which also has trips to other people and places (the day desk,
daylife.py, builds both through `menu_of` and walks a chosen trip through `on_plan`). Until the new menu
is measured (`apply_day` off), `plan` is applied and `do` is only traced, in shadow; with it on, only `do`
is asked and applied. Without a day desk only the old menu is asked.

Per npc there is at most one pending request (new events merge into it) and one in flight. A pending
request that waits too long is dropped; an answer is applied only if nothing happened to the npc
since the request was built (`version`) and it is still ours. Anything dropped, unanswered or
unsure means the npc keeps its routine: the game's scripts are the fallback.

Background requests are limited by a rate budget and a cap on requests in flight; when the decider
falls behind (expired / errors), MID and FAR timers stretch.
"""

from __future__ import annotations

import asyncio
import itertools
import logging
import random
import time
from collections.abc import Awaitable, Callable
from dataclasses import dataclass, field, replace
from typing import TYPE_CHECKING, Any

from ..protocol import (
    Attached,
    Detached,
    GameMessage,
    Loading,
    NpcActResult,
    NpcEvent,
    NpcIdle,
    NpcRoster,
    PerceptionHeld,
    RosterEntry,
)
from .apply import Applier, Decision
from .day import KEEP, DayMenu, DayOption, Habit, current_entry, plan_ids, plan_options
from .decider import Decider, Lane, Outcome
from .situation import NpcContext, Situation, SituationBuilder, clock_text
from .stimuli import THREAT_CONTEXT, Stimulus, merge, normalize, policy
from .tiers import Placement, Tier, TierConfig, assign_tiers, importance

if TYPE_CHECKING:
    from ..trace import TraceWriter

log = logging.getLogger(__name__)

NO_ACTION = {"continue", KEEP}
# choices that take a held threat over: the body fights, flees or runs shouting for help, the scripts must
# not react too
TAKES_BODY = {"fight", "flee", "call_help"}
BODY_MAX_S = 60.0  # a body taken over without word of it being done is given back after this
INTENT_THREAT_WAIT_S = 10.0  # an NPC can choose to run just before the first threat perception arrives
OVERHEAR_CM = 1000  # npc's this close hear what the player says to someone else
TRACE_PER_MIN = 12  # decision rows per npc a minute: a NEAR npc with an agenda is asked every 6-10 s
# a quiet npc whose routine is being asked is looked at again this soon (no request, no token spent)
FETCH_WAIT_S = 1.0
INVESTIGATION_MAX_S = 60.0  # the bridge ends the walk after at most 45 s; recover a lost receipt/idle
ROSTER_FRESH_S = 2.0  # the bridge reports positions every 0.5 s
HERO_STILL_NEAR_CM = 700.0  # NpcObserver::NearLeave, including its proximity hysteresis
INVESTIGATION_THREATS = {"weapon_drawn", "threatened", "attack", "damage", "combat", "murder", "magic"}


def applied_key(choice: str, target: str | None) -> str:
    """What an action's cooldown is kept under: a fight, flight or call for help is not repeated against the
    same threat, but a new one (a bloodfly after the wolf) gets its own."""
    return f"{choice}/{target}" if choice in TAKES_BODY else choice


@dataclass
class PlannerConfig:
    tiers: TierConfig = field(default_factory=TierConfig)
    near_min_gap_s: float = 2.0
    near_period_s: tuple[float, float] = (6.0, 10.0)
    mid_period_s: tuple[float, float] = (30.0, 60.0)
    far_period_s: tuple[float, float] = (200.0, 280.0)  # a game hour is ~4 real minutes (x14.5)
    catch_up_s: tuple[float, float] = (0.2, 1.5)
    debounce_s: float = 0.2  # non-urgent events this close together make one request
    stale_s: dict[Tier, float] = field(
        default_factory=lambda: {Tier.NEAR: 2.0, Tier.MID: 10.0, Tier.FAR: 60.0}
    )
    deadline_ms: dict[str, int] = field(
        default_factory=lambda: {
            "urgent": 300,
            "near": 1000,
            "mid": 5000,
            "far": 30000,
            "attitude": 5000,
            "escalate": 2000,
        }
    )
    react_threshold: float = 0.5  # below it the npc keeps its routine
    plan_threshold: float = 0.5
    # Reversible gestures and a brief check of a sound need less certainty than approaching the hero.
    option_threshold: dict[str, float] = field(
        default_factory=lambda: {
            "look_at": 0.25,
            "greet": 0.3,
            "investigate": 0.3,  # bounded walk and scan; the engine returns it to its routine
            "approach_talk": 0.6,
            "fight": 0.6,  # a fight is not taken back: surer than for a gesture
        }
    )
    # the same action is not repeated by one npc within this time (walking up to the player twice a minute
    # is pestering, not liveliness)
    cooldown_s: dict[str, float] = field(
        default_factory=lambda: {
            "approach_talk": 120.0,
            "greet": 30.0,
            "comment": 20.0,
            "look_at": 8.0,
            "thank": 30.0,
            "tell_to_leave": 15.0,
        }
    )
    default_cooldown_s: float = 10.0
    ask_attitude: bool = True  # after an urgent reaction, ask in the background how the attitude changed
    # after an urgent reaction or a threat, ask how much the moment calls for the npc's own words (0-4); from
    # this level on the listener of `on_escalate` speaks (by its rules too: the hero hears it, budget).
    # It is the expected level, which sits between the levels: on held-out scenes the labeller's "level 3 or
    # more" is matched best from 2.5 on (89% for g6c; from 3.0 on only 64%, a fifth of the calls)
    escalate_level: float = 2.5
    # NEAR/MID npc's think about their next hour this often (a game hour ~4 min)
    plan_period_s: float = 240.0
    # the day plan is chosen from the new `do` menu (with trips); off: from the old one, `do` in shadow
    apply_day: bool = False
    # an npc's routine is asked again after this long (scripts swap routines), and after a failure only
    # after these pauses (the last one from then on)
    routine_ttl_s: float = 600.0
    routine_retry_s: tuple[float, ...] = (10.0, 30.0, 120.0, 300.0)
    max_inflight: int = 48
    bg_rate: float = 20.0  # background requests per second
    tick_s: float = 0.05


@dataclass
class Pending:
    purpose: str  # react | plan
    urgent: bool
    created: float
    deadline: float  # dropped if not sent by then
    not_before: float


@dataclass
class Flight:
    rid: str
    purpose: str
    urgent: bool
    version: int
    sent: float
    stimuli: tuple[str, ...]
    situation: Situation
    target: str | None = None  # the newest threat's source: whom a fight or flight is about
    plan_args: dict[str, dict] = field(default_factory=dict)  # plan option -> start_activity args
    # a plan's applied question: "plan" (the old menu), "do", or None when only `do` is asked in shadow
    question: str | None = None
    shadow: str | None = None  # asked along and only traced: "do" while the new menu is in shadow
    options: dict[str, DayOption] = field(default_factory=dict)  # the applied do menu, by wire key
    shadow_menu: dict[str, DayOption] = field(default_factory=dict)  # the do menu in shadow
    # what the applied question did, {question, choice, applied, why}: the shadow row's `code`
    outcome: dict[str, Any] = field(default_factory=dict)
    react_args: dict[str, dict[str, Any]] = field(default_factory=dict)


@dataclass
class RoutineSlot:
    """An npc's routine as the game told it, and how asking for it goes."""

    info: Any | None = None  # RoutineInfo
    fetching: bool = False
    fetched: float = float("-inf")
    failures: int = 0  # in a row
    retry_at: float = 0.0  # after a failure: not asked again before this


@dataclass
class Investigation:
    since: float
    act: str | None = None  # provisional until the bridge's queued receipt identifies this command


@dataclass
class NpcRecord:
    id: str
    name: str
    guild: int = 0
    distance_cm: float = 0.0
    in_view: bool = False
    activity: str = "routine"
    state: str = ""  # the script state it is in (roster v7), e.g. ZS_RAKE_FP; "" when the bridge is older
    moving: bool = False  # walking or running (roster v7)
    tier: Tier = Tier.FAR
    importance: float = 0.0
    version: int = 0
    stimuli: list[Stimulus] = field(default_factory=list)
    next_check: float = 0.0
    last_decision: float | None = None
    last_interaction: float | None = None
    agenda: str | None = None  # what the npc wants from the player, e.g. help with wolves
    # action -> when it was last done; a fight, flight or call for help per threat (`applied_key`)
    last_applied: dict[str, float] = field(default_factory=dict)
    pending: Pending | None = None
    flight: Flight | None = None
    holds: list[tuple[str, float]] = field(default_factory=list)  # threats held from the scripts, (id, when)
    body: tuple[str, float] | None = None  # the body took a threat over: (choice, since); until npc_idle
    waiting_body: tuple[str, float] | None = None  # conversational choice waiting for its threat's source
    missed_urgent: bool = False  # something urgent happened while it was fighting
    last_plan: float = 0.0  # when it last thought of its day (or appeared): the next plan is a period later
    talking: bool = False  # in a conversation with the hero (talk.py has its body): threats only
    roster_at: float = float("-inf")
    investigation: Investigation | None = None
    deferred: list[Stimulus] = field(default_factory=list)  # ordinary observations held during investigation


@dataclass
class PlannerStats:
    sent: dict[str, int] = field(default_factory=lambda: {"urgent": 0, "background": 0})
    outcomes: dict[str, int] = field(default_factory=dict)
    applied: int = 0
    unsure: int = 0
    no_action: int = 0
    discarded: int = 0  # answer came back but the npc had changed meanwhile
    stale: int = 0  # pending dropped before it could be sent
    quiet: int = 0  # idle/timer checks skipped: nothing to react to
    cooldown: int = 0  # decided an action it did just recently
    vetoed: int = 0  # the `allow` hook kept a sure answer from being applied
    events: int = 0
    scripted_events: int = 0
    shadow: int = 0  # answers to the do menu traced in shadow


class NpcPlanner:
    def __init__(
        self,
        decider: Decider,
        applier: Applier,
        builder: SituationBuilder | None = None,
        config: PlannerConfig | None = None,
        *,
        clock: Callable[[], float] = time.monotonic,
        rng: random.Random | None = None,
        agenda_of: Callable[[str], str | None] | None = None,
        routine_of: Callable[[str], Awaitable[Any]] | None = None,
        habits_of: Callable[[str], list[Habit]] | None = None,
        view_of: Callable[[str], tuple[str, ...]] | None = None,
        trace: TraceWriter | None = None,
        facts_of: Callable[[str], tuple[str, ...]] | None = None,
    ) -> None:
        self.decider = decider
        self.trace = trace  # every answered decision as a "brain" row: data for the fast model
        # what the npc remembers about the hero, a few English lines (the state's "What you remember")
        self.facts_of = facts_of
        # what the npc perceives now (perception.py), a few English lines
        self.view_of = view_of
        # what an npc wants from the hero, as its last conversation left it (memory); read when it appears
        self.agenda_of = agenda_of
        # the npc's daily routine from the game (`routine`, protocol v6) and its habits: the plan's menu
        self.routine_of = routine_of
        self.habits_of = habits_of
        self.routines: dict[str, RoutineSlot] = {}
        # how much the moment calls for the npc's own words: (npc, level, the reaction applied or None,
        # flight); told every level, the listener decides (`escalate_level`, or more when it already spoke)
        self.on_escalate: Callable[[str, float, Decision | None, Flight], None] | None = None
        # a reason not to apply a sure choice now, or None (initiative.py: one walk-up at a time)
        self.allow: Callable[[NpcRecord, str], str | None] | None = None
        # takes approach_talk over: walks the npc up and lets it speak; without it the applier walks it
        self.on_approach: Callable[[Decision, NpcRecord], None] | None = None
        # the day desk (daylife.py): an npc's menus for the next hour from its routine (rec, routine info,
        # hour, minute, habits); without it only the old menu is asked
        self.menu_of: Callable[[NpcRecord, Any, int, int, list[Habit]], DayMenu] | None = None
        # takes a chosen trip over (chat, pray, tavern): walks the npc there; without it a trip is not done
        self.on_plan: Callable[[Decision, NpcRecord, DayOption], None] | None = None
        self.applier = applier
        self.crime_rooms = False  # CrimeMeet handles hero room entry; keep it only as model context.
        self.builder = builder or SituationBuilder()
        self.cfg = config or PlannerConfig()
        self.clock = clock
        self.rng = rng or random.Random()
        self.npcs: dict[str, NpcRecord] = {}
        self.game_time: tuple[int, int] | None = None
        self.stats = PlannerStats()
        self.slowdown = 1.0  # stretches MID/FAR timers while the decider falls behind
        self._tokens = self.cfg.bg_rate
        self._refilled = clock()
        self._ids = itertools.count(1)
        self._tasks: set[asyncio.Task[None]] = set()
        self._traced: dict[str, tuple[float, int]] = {}  # npc -> (minute started, rows in it)
        self._world = 0  # a load or a new world: routines asked before it are dropped when they come

    # --- input ----------------------------------------------------------------------------

    def on_message(self, msg: GameMessage) -> None:
        """GameLink listener."""
        now = self.clock()
        match msg:
            case NpcRoster():
                if msg.hour is not None and msg.minute is not None:
                    self.game_time = (msg.hour, msg.minute)
                self.on_roster(msg.npcs, now, full=msg.full)
            case NpcEvent():
                self.on_event(
                    msg.npc,
                    msg.kind,
                    now,
                    other=msg.other,
                    distance_cm=msg.distance,
                    detail=msg.detail,
                    other_name=msg.other_name,
                    victim=msg.victim,
                    position=tuple(msg.at) if msg.at is not None else None,
                    place=msg.place,
                    loudness=msg.loudness,
                    source=msg.source,
                )
            case NpcIdle():
                self.on_idle(msg.npc, now, act=msg.act, reason=msg.reason)
            case NpcActResult():
                self.on_act_result(msg, now)
            case PerceptionHeld():
                self.on_held(msg, now)
            case Attached():
                self._set_activity(msg.npc, "attached")
            case Detached():
                self._set_activity(msg.npc, "routine")
            case Loading():
                self.reset()

    def reset(self) -> None:
        """A load or a new world: nothing known of the npc's holds, their routines and the time included."""
        for task in self._tasks:
            task.cancel()
        self.npcs.clear()
        self._traced.clear()
        self.routines.clear()
        self.game_time = None
        self._world += 1

    def on_roster(self, entries: list[RosterEntry], now: float, *, full: bool = True) -> None:
        seen = set()
        for e in entries:
            npc = e.id.upper()
            seen.add(npc)
            rec = self.npcs.get(npc)
            if rec is None:
                rec = self.npcs[npc] = NpcRecord(npc, e.name, e.guild)
                rec.tier = Tier.OFF  # placed below; a new npc gets a random phase
                rec.last_plan = now  # its first plan comes a period after it appears
                if self.agenda_of is not None:
                    rec.agenda = self.agenda_of(npc)
            rec.distance_cm, rec.in_view, rec.guild = e.distance, e.in_view, e.guild
            rec.roster_at = now
            rec.state, rec.moving = e.state, e.moving
            if rec.activity not in ("dialog", "attached") or e.activity != "routine":
                rec.activity = e.activity
            if rec.activity != "routine":
                rec.waiting_body = None
                self._finish_investigation(rec, now, resume=False)
        if full:
            for npc in [n for n in self.npcs if n not in seen]:
                del self.npcs[npc]  # its flight's answer will find no record and be ignored
        self._retier(now)
        for rec in self.npcs.values():
            if rec.investigation is None and rec.deferred:
                self._resume_deferred(rec, now)

    def on_event(
        self,
        npc: str,
        kind: str,
        now: float,
        *,
        other: str | None = None,
        distance_cm: float | None = None,
        detail: str | None = None,
        other_name: str | None = None,
        victim: str | None = None,
        position: tuple[float, float, float] | None = None,
        place: str | None = None,
        loudness: float | None = None,
        source: str | None = None,
        context_only: bool = False,
    ) -> None:
        rec = self.npcs.get(npc.upper())
        if rec is None or (rec.tier is Tier.OFF and rec.activity != "fight"):
            return
        if kind == "noise" and (rec.activity != "routine" or rec.talking):
            return  # the bridge only reports sounds heard by free npc's; do not retain a late one
        other = (other or "") if kind == "noise" else (other or "PLAYER")
        self.stats.events += 1
        kind, detail = normalize(kind, other, other_name, detail)
        context = ""
        if kind == "monster_killed":
            # the danger from that monster is over (all of its kind share the symbol: one more nearby is
            # sensed again soon); what it was doing stays in the words; the npc itself may have killed it
            gone = {s.kind for s in rec.stimuli if s.policy.threat and s.other == victim}
            rec.stimuli = [s for s in rec.stimuli if not (s.policy.threat and s.other == victim)]
            context = next((THREAT_CONTEXT[k] for k in THREAT_CONTEXT if k in gone), "")
            if other.upper() == rec.id:
                other_name = "you"
        stim = Stimulus(
            kind, now, other, distance_cm, detail, other_name, context, position, place, loudness, source
        )
        p = policy(kind)
        if rec.investigation is not None:
            if p.threat or kind in INVESTIGATION_THREATS:
                self._finish_investigation(rec, now, resume=False)
            else:
                if kind != "noise":
                    self._defer(rec, stim)
                return  # an ordinary observation never cancels the walk or asks for a new action yet
        rec.stimuli = merge(rec.stimuli, stim, now)
        if other == "PLAYER":
            rec.last_interaction = now
        if p.scripted or context_only or (
            self.crime_rooms and kind == "enter_room" and other.upper() in {"PLAYER", "HERO", "PC_HERO"}
        ):  # scripts or crime rules react; retain context for later decisions
            self.stats.scripted_events += 1
            return
        if p.threat and rec.waiting_body is not None:
            choice, since = rec.waiting_body
            rec.waiting_body = None
            if now - since <= INTENT_THREAT_WAIT_S and self.take_body(rec.id, choice) is not None:
                return  # its own words already chose to fight/run; do not overwrite them on the first hit
        if rec.tier is Tier.OFF:
            # fighting (its weapon is out): decided upon once the fight is over, at once if it was urgent,
            # e.g. the hero killed the wolf the npc was fighting
            rec.missed_urgent = rec.missed_urgent or p.urgent
            return
        if rec.talking and not p.threat:
            return  # its body is in a conversation (talk.py); threats are still ours
        if rec.tier is Tier.FAR or (rec.tier is Tier.MID and not p.salient):
            return
        rec.version += 1
        urgent = p.urgent and rec.tier is Tier.NEAR
        self._want(rec, "react", urgent, now, not_before=now if urgent else now + self.cfg.debounce_s)
        if urgent and rec.flight is not None:
            self._spawn(self.decider.cancel(rec.flight.rid))  # its answer is outdated now

    def on_held(self, msg: PerceptionHeld, now: float) -> None:
        """A threat the game holds from the npc's scripts until the model decides (or 0.6 s pass)."""
        if msg.kind in {"bandit_enemy", "party_enemy"}:
            return  # Parley owns these sightings, including late holds for an unknown or owned npc.
        if msg.deed is not None or msg.other.upper() in {"PLAYER", "HERO", "PC_HERO"}:
            # CrimeReact/CrimeMeet settle hero deeds and room entry. Keep their context,
            # including when a malformed hold has no deed id and must fall back to scripts.
            self.on_event(
                msg.npc,
                msg.kind,
                now,
                other=msg.other,
                distance_cm=msg.distance,
                detail=msg.detail,
                other_name=msg.other_name,
                context_only=True,
            )
            return
        rec = self.npcs.get(msg.npc.upper())
        if rec is not None and rec.activity == "body":
            self.applier.verdict(msg.hold, "server", npc=rec.id)
            return  # a late held perception must not hand an owned body back to native scripts
        if rec is None or rec.tier is Tier.OFF:
            self.applier.verdict(msg.hold, "scripts")
            return
        rec.holds.append((msg.hold, now))
        self.on_event(
            msg.npc,
            msg.kind,
            now,
            other=msg.other,
            distance_cm=msg.distance,
            detail=msg.detail,
            other_name=msg.other_name,
        )
        if rec.pending is None and rec.flight is None:  # merged into nothing (a repeat): give it back
            self._settle_holds(rec, now, None)

    def _settle_holds(self, rec: NpcRecord, upto: float, decision: Decision | None) -> None:
        """Held threats up to `upto`: kept from the scripts only if the body took them over, by this
        decision or before (an unsure answer while running for help: it goes on running)."""
        if rec.activity == "body":
            taken = True
        elif decision is not None:
            taken = decision.choice in TAKES_BODY
        else:
            taken = rec.body is not None and self.clock() - rec.body[1] < BODY_MAX_S
        to = "server" if taken else "scripts"
        keep = []
        for hold, at in rec.holds:
            if at <= upto:
                self.applier.verdict(hold, to, npc=rec.id)
            else:
                keep.append((hold, at))
        rec.holds = keep

    def on_act_result(self, msg: NpcActResult, now: float) -> None:
        rec = self.npcs.get(msg.npc.upper())
        if rec is None or rec.investigation is None or msg.action != "investigate":
            return
        if rec.investigation.act is not None:
            return  # a rejected second command must not release the investigation already running
        if msg.status == "queued":
            rec.investigation.act = msg.id
        else:
            self._finish_investigation(rec, now, resume=True)

    @staticmethod
    def _defer(rec: NpcRecord, stimulus: Stimulus) -> None:
        # Do not expire proximity here: after the walk it is checked against a fresh roster instead.
        rec.deferred = [s for s in rec.deferred if (s.kind, s.other) != (stimulus.kind, stimulus.other)]
        rec.deferred.append(stimulus)
        rec.deferred = rec.deferred[-5:]

    def _finish_investigation(self, rec: NpcRecord, now: float, *, resume: bool) -> None:
        if rec.investigation is None:
            if not resume:
                rec.deferred.clear()
            return
        rec.investigation = None
        rec.version += 1  # answers built while its body was occupied are no longer useful
        rec.pending = None
        if resume:
            self._resume_deferred(rec, now)
        else:
            rec.deferred.clear()

    def _resume_deferred(self, rec: NpcRecord, now: float) -> None:
        if rec.talking or rec.activity != "routine":
            rec.deferred.clear()
            return
        if now - rec.roster_at > ROSTER_FRESH_S:
            return  # keep waiting for a current position, never use where the hero was before the walk
        deferred, rec.deferred = rec.deferred, []
        if not deferred or rec.distance_cm > HERO_STILL_NEAR_CM or rec.tier in (Tier.OFF, Tier.FAR):
            return
        for stimulus in deferred:
            if stimulus.kind == "player_near":
                stimulus = replace(stimulus, at=now, distance_cm=rec.distance_cm)
            if stimulus.alive(now):
                rec.stimuli = merge(rec.stimuli, stimulus, now)
        if any(s.other == "PLAYER" for s in deferred):
            # Even an expired greeting/speech is reconsidered as the hero's current presence, without
            # replaying an old choice or pretending that an old gift/utterance happened again.
            rec.stimuli = merge(rec.stimuli, Stimulus("player_near", now, "PLAYER", rec.distance_cm), now)
        if self._has_reason(rec, now):
            self._want(rec, "react", False, now, not_before=now)

    def on_idle(self, npc: str, now: float, *, act: str | None = None, reason: str | None = None) -> None:
        rec = self.npcs.get(npc.upper())
        if rec is not None and rec.investigation is not None:
            if act is None or act != rec.investigation.act:
                return  # an empty script queue or another command's idle is not the end of this walk
            self._finish_investigation(rec, now, resume=reason in ("done", "timeout"))
            return
        if rec is not None:
            rec.body = None  # what the body was doing is over: threats from now on go to the scripts again
        if rec is None or rec.tier is not Tier.NEAR or rec.talking:
            return
        if not self._has_reason(rec, now):
            self.stats.quiet += 1
            return
        earliest = now if rec.last_decision is None else max(now, rec.last_decision + self.cfg.near_min_gap_s)
        self._want(rec, "react", False, now, not_before=earliest)

    def on_player_text(self, partner: str, text: str, now: float) -> None:
        """The player said something to `partner` (talk.py; "" — to nobody): npc's close by overhear it."""
        partner = partner.upper()
        for rec in list(self.npcs.values()):
            if rec.id != partner and rec.distance_cm <= OVERHEAR_CM:
                self.on_event(rec.id, "player_speech", now, distance_cm=rec.distance_cm, detail=text)

    def talking(self, npc: str, on: bool) -> None:
        """The npc is in a conversation with the hero (talk.py decides its body) or no longer is."""
        rec = self.npcs.get(npc.upper())
        if rec is None or rec.talking == on:
            return
        rec.talking = on
        rec.version += 1  # an answer asked before is moot
        if on:
            self._finish_investigation(rec, self.clock(), resume=False)
        if not on:
            self._after_chat(rec.id, self.clock())

    def stirred(self, npc: str, now: float | None = None) -> bool:
        """The npc still has something urgent or threatening going on (its stimuli have not expired)."""
        rec = self.npcs.get(npc.upper())
        now = self.clock() if now is None else now
        return rec is not None and any(
            s.alive(now) and (s.policy.urgent or s.policy.threat) for s in rec.stimuli
        )

    def take_body(self, npc: str, choice: str, *, target: str | None = None) -> Decision | None:
        """The conversational model's intent for a free npc moves its body, like the behavioral model's own
        choice: `flee` or `fight` against the newest threat, while there is one (an intent that comes after
        the danger is moot). In-flight answers about the moment are outdated by it; the threats held now stay
        from the scripts."""
        rec = self.npcs.get(npc.upper())
        if rec is None or rec.activity in ("dialog", "attached", "body"):
            if rec is not None:
                rec.waiting_body = None
            log.info("%s: no body to take (%s)", npc, rec.activity if rec else "not around")
            return None
        now = self.clock()
        target = target or self._threat(rec, now)
        if target is None:
            rec.waiting_body = (choice, now)
            log.info("%s: nothing to %s (%s)", npc, choice, ",".join(s.kind for s in rec.stimuli) or "calm")
            return None
        rec.waiting_body = None
        self._finish_investigation(rec, now, resume=False)
        rec.version += 1
        rec.pending = None
        rec.last_applied[applied_key(choice, target)] = now
        rec.body = (choice, now)
        decision = Decision(
            rec.id, "react", choice, 1.0, {}, tuple(s.kind for s in rec.stimuli), target=target
        )
        self.stats.applied += 1
        self.applier.apply(decision)
        self._settle_holds(rec, now, decision)
        return decision

    def clear_body_intent(self, npc: str) -> None:
        rec = self.npcs.get(npc.upper())
        if rec is not None:
            rec.waiting_body = None

    def set_agenda(self, npc: str, agenda: str | None) -> None:
        """What the npc wants from the player (from memory or the big model's plan); None clears it."""
        rec = self.npcs.get(npc.upper())
        if rec is not None and rec.agenda != agenda:
            rec.agenda = agenda
            rec.version += 1

    # --- scheduling -----------------------------------------------------------------------

    def tick(self, now: float) -> None:
        """Turn due timers into requests and send what the budget allows."""
        self._refill(now)
        for rec in self.npcs.values():
            if rec.investigation is not None and now - rec.investigation.since >= INVESTIGATION_MAX_S:
                self._finish_investigation(rec, now, resume=True)
            if rec.investigation is not None:
                continue  # neither an agenda nor the day timer can take this bounded action's body
            if rec.deferred:
                self._resume_deferred(rec, now)
                if rec.deferred:
                    continue
            if rec.tier is Tier.OFF or rec.talking or rec.pending or rec.flight or rec.next_check > now:
                continue
            soon = False
            if self._plan_due(rec, now):
                info = self._routine_ready(rec, now)  # may start asking for it; never touches next_check
                if info is not None and self._in_routine(rec, info):
                    self._want(rec, "plan", False, now, not_before=now)  # time to think of the day
                    continue
                soon = info is None and self.routines[rec.id].fetching
            if rec.tier is not Tier.FAR and self._has_reason(rec, now):
                self._want(rec, "react", False, now, not_before=now)  # an agenda or a live stimulus
                continue
            if rec.tier is not Tier.FAR:
                self.stats.quiet += 1
            self._schedule(rec, now)
            if soon:
                rec.next_check = min(rec.next_check, now + FETCH_WAIT_S)  # plan once the routine is known

        ready = []
        for rec in self.npcs.values():
            pend = rec.pending
            if (
                pend is None
                or rec.flight is not None
                or rec.investigation is not None
                or pend.not_before > now
            ):
                continue
            if pend.deadline < now:
                rec.pending = None
                self.stats.stale += 1
                self._schedule(rec, now)
                continue
            ready.append(rec)
        # urgent first, then the most important, then the longest waiting
        ready.sort(key=lambda r: (not r.pending.urgent, -r.importance, r.pending.created))
        inflight = sum(1 for r in self.npcs.values() if r.flight is not None)
        for rec in ready:
            urgent = rec.pending.urgent
            if not urgent and (inflight >= self.cfg.max_inflight or self._tokens < 1):
                break
            if not urgent:
                self._tokens -= 1
            self._send(rec, now)
            inflight += 1

    async def run(self) -> None:
        try:
            while True:
                self.tick(self.clock())
                await asyncio.sleep(self.cfg.tick_s)
        finally:
            self.reset()

    def snapshot(self) -> dict:
        tiers: dict[str, int] = {}
        for rec in self.npcs.values():
            tiers[rec.tier.name] = tiers.get(rec.tier.name, 0) + 1
        s = self.stats
        return {
            "npcs": len(self.npcs),
            "tiers": tiers,
            "sent": dict(s.sent),
            "outcomes": dict(s.outcomes),
            "applied": s.applied,
            "unsure": s.unsure,
            "no_action": s.no_action,
            "discarded": s.discarded,
            "stale": s.stale,
            "quiet": s.quiet,
            "cooldown": s.cooldown,
            "vetoed": s.vetoed,
            "shadow": s.shadow,
            "events": s.events,
            "slowdown": round(self.slowdown, 2),
        }

    # --- internals ------------------------------------------------------------------------

    @staticmethod
    def _has_reason(rec: NpcRecord, now: float) -> bool:
        return rec.agenda is not None or any(s.alive(now) for s in rec.stimuli)

    def _plan_due(self, rec: NpcRecord, now: float) -> bool:
        """The hour's plan is due: routines and the clock are known, the timer is up, and a NEAR/MID npc has
        no live stimulus (a scripted one too: the game's scripts are reacting to it). An agenda does not stop
        it."""
        if self.routine_of is None or self.game_time is None:
            return False
        if rec.tier is Tier.FAR:
            return True
        return now - rec.last_plan >= self.cfg.plan_period_s and not any(s.alive(now) for s in rec.stimuli)

    def _in_routine(self, rec: NpcRecord, info: Any) -> bool:
        """It does what its routine has it do now: its roster state is the current entry's (not a script's
        reaction, not an activity of ours still running). Unknown state (bridge < v7): yes."""
        assert self.game_time is not None
        entry = current_entry(info.entries, *self.game_time)
        return not rec.state or entry is None or rec.state.upper() == entry.state.upper()

    def _after_chat(self, npc: str, now: float) -> None:
        """The hero just talked with the npc: it should not walk up to him or greet him again at once."""
        rec = self.npcs.get(npc.upper())
        if rec is not None:
            rec.last_interaction = now
            for choice in ("approach_talk", "greet"):
                rec.last_applied[choice] = now

    def _set_activity(self, npc: str, activity: str) -> None:
        rec = self.npcs.get(npc.upper())
        if rec is not None and rec.activity != activity:
            rec.activity = activity
            if activity != "routine":
                rec.waiting_body = None
                self._finish_investigation(rec, self.clock(), resume=False)
            self._retier(self.clock())

    def _retier(self, now: float) -> None:
        placements = []
        for rec in self.npcs.values():
            since = None if rec.last_interaction is None else now - rec.last_interaction
            rec.importance = importance(rec.distance_cm / 100, rec.in_view, since)
            placements.append(
                Placement(
                    rec.id, rec.distance_cm / 100, rec.in_view, rec.activity == "routine", rec.importance
                )
            )
        prev = {rec.id: rec.tier for rec in self.npcs.values() if rec.tier is not Tier.OFF}
        for npc, tier in assign_tiers(placements, prev, self.cfg.tiers).items():
            rec = self.npcs[npc]
            if tier is rec.tier:
                continue
            old, rec.tier = rec.tier, tier
            rec.version += 1
            missed, rec.missed_urgent = rec.missed_urgent, False
            if tier is Tier.OFF:
                rec.pending = None
            elif missed and tier is not Tier.FAR:
                # back from a fight in which something urgent happened: react to it now
                self._want(rec, "react", tier is Tier.NEAR, now, not_before=now)
            elif old is Tier.OFF and npc not in prev and rec.last_decision is None:
                rec.next_check = now + self.rng.uniform(0, self._period(tier)[1])  # new: spread first checks
            elif tier < old or old is Tier.OFF:
                # coming closer (or back to its routine): decide soon, it may have missed things
                rec.next_check = min(rec.next_check, now + self.rng.uniform(*self.cfg.catch_up_s))
            else:
                rec.next_check = now + self.rng.uniform(0, self._period(tier)[1])
                if rec.pending and tier is Tier.FAR:
                    rec.pending = None  # its reaction is moot out there

    def _want(self, rec: NpcRecord, purpose: str, urgent: bool, now: float, *, not_before: float) -> None:
        stale = now + self.cfg.stale_s[rec.tier] + (not_before - now)
        if rec.pending is None:
            rec.pending = Pending(purpose, urgent, now, stale, not_before)
            return
        p = rec.pending
        if purpose == "react":
            p.purpose = "react"
        p.urgent = p.urgent or urgent
        p.not_before = min(p.not_before, not_before) if urgent else max(p.not_before, not_before)
        p.deadline = max(p.deadline, stale)

    def _send(self, rec: NpcRecord, now: float) -> None:
        pend = rec.pending
        assert pend is not None
        rec.pending = None
        rec.stimuli = [s for s in rec.stimuli if s.alive(now)]
        view = self.view_of(rec.id) if self.view_of is not None else ()
        ctx = NpcContext(
            rec.id,
            rec.name,
            rec.guild,
            rec.distance_cm,
            rec.in_view,
            agenda=rec.agenda,
            view=view,
            facts=self.facts_of(rec.id) if self.facts_of else (),
        )
        clock = clock_text(*self.game_time) if self.game_time else None
        plan_args: dict[str, dict] = {}
        question: str | None = None  # a plan's applied question and the one asked in shadow
        shadow: str | None = None
        options: dict[str, DayOption] = {}
        shadow_menu: dict[str, DayOption] = {}
        react_args: dict[str, dict[str, Any]] = {}
        if pend.purpose == "plan":
            slot = self.routines.get(rec.id)
            if slot is None or slot.info is None or self.game_time is None:
                self._schedule(rec, now)  # gone with a load meanwhile: asked again when due
                return
            habits = self.habits_of(rec.id) if self.habits_of else []
            hour, minute = self.game_time
            if self.menu_of is not None:
                menu = self.menu_of(rec, slot.info, hour, minute, habits)
            else:  # no day desk (m5c, planner_sim): the old menu alone
                menu = DayMenu(plan=plan_options(slot.info.entries, habits, hour, minute, rec.state))
            rec.last_plan = now  # an empty menu too: not looked at again before the next period
            question, shadow = self._day_questions(menu)
            if question is None and shadow is None:
                # nothing to choose between (or on duty, asleep): the routine goes on
                if menu.stay is not None:
                    log.debug("%s: no day plan (%s)", rec.id, menu.stay)
                self._schedule(rec, now)
                return
            old = menu.plan[0] if question == "plan" else None
            do = {key: o.text for key, o in menu.do.items()} if "do" in (question, shadow) else None
            sit = self.builder.day(ctx, clock, old, do)
            if question == "plan":
                plan_args = dict(menu.plan[1])
            elif question == "do":
                options = menu.do
                plan_args = {key: o.args for key, o in options.items() if o.args is not None}
            if shadow == "do":
                shadow_menu = menu.do
            deadline = self.cfg.deadline_ms["far"]
        else:
            sit = self.builder.react(ctx, rec.stimuli, now, clock)
            noise = max((s for s in rec.stimuli if s.kind == "noise"), key=lambda s: s.at, default=None)
            if noise is not None and noise.position is not None:
                react_args["investigate"] = {"at": list(noise.position), "ms": 5000}
            key = "urgent" if pend.urgent else ("near" if rec.tier is Tier.NEAR else "mid")
            deadline = self.cfg.deadline_ms[key]
        lane: Lane = "urgent" if pend.urgent else "background"
        flight = Flight(
            f"{rec.id}#{next(self._ids)}",
            pend.purpose,
            pend.urgent,
            rec.version,
            now,
            tuple(s.kind for s in rec.stimuli),
            sit,
            self._threat(rec),
            plan_args,
            question=question,
            shadow=shadow,
            options=options,
            shadow_menu=shadow_menu,
            react_args=react_args,
        )
        rec.flight = flight
        self.stats.sent[lane] += 1
        self._spawn(self._ask(rec.id, flight, lane, rec.importance, deadline))

    def _day_questions(self, menu: DayMenu) -> tuple[str | None, str | None]:
        """Which day questions to ask: (the applied one, the one in shadow). Each needs two options at least:
        `plan` (the old menu) is applied and `do` asked along in shadow, or `do` alone in shadow when the
        old menu has nothing to choose; with `apply_day` (and a day desk to build it), `do` alone is applied.
        Nothing while the npc stays as it is (on duty, asleep)."""
        if menu.stay is not None:
            return None, None
        do = "do" if len(menu.do) >= 2 else None
        if self.cfg.apply_day and self.menu_of is not None:
            return do, None
        return ("plan" if len(menu.plan[0]) >= 2 else None), do

    @staticmethod
    def _threat(rec: NpcRecord, now: float | None = None) -> str | None:
        """The newest threat's source (of those still alive at `now`): whom a fight or flight is about."""
        return next(
            (
                s.other
                for s in sorted(rec.stimuli, key=lambda s: -s.at)
                if s.policy.threat and (now is None or s.alive(now))
            ),
            None,
        )

    def _routine_ready(self, rec: NpcRecord, now: float) -> Any | None:
        """Its routine if known (a stale one is used while a fresh one is asked); None while it is first
        asked or after a failure. A fetch starts when there is none or it is older than routine_ttl_s, unless
        one runs or a failure's retry time is ahead. Never touches rec.next_check: the react path keeps its
        own timer."""
        slot = self.routines.setdefault(rec.id, RoutineSlot())
        stale = slot.info is None or now - slot.fetched >= self.cfg.routine_ttl_s
        if stale and not slot.fetching and now >= slot.retry_at:
            self._fetch(rec.id, slot)
        return slot.info

    def _fetch(self, npc: str, slot: RoutineSlot) -> None:
        """Ask the game for the npc's routine in the background; a failure waits routine_retry_s, longer
        each time."""
        assert self.routine_of is not None
        ask, world = self.routine_of, self._world
        slot.fetching = True

        async def fetch() -> None:
            try:
                info = await ask(npc)
                if world == self._world:  # not from before a load
                    slot.info, slot.fetched, slot.failures = info, self.clock(), 0
            except Exception as err:  # the game could not tell: no plan for this npc until it can
                slot.failures += 1
                pauses = self.cfg.routine_retry_s
                slot.retry_at = self.clock() + pauses[min(slot.failures, len(pauses)) - 1]
                log.info("routine of %s: %s (asked again in %.0f s)", npc, err, slot.retry_at - self.clock())
            finally:
                slot.fetching = False

        self._spawn(fetch())

    async def _ask(self, npc: str, flight: Flight, lane: Lane, priority: float, deadline_ms: int) -> None:
        sit = flight.situation
        outcome = await self.decider.decide(flight.rid, lane, priority, deadline_ms, sit.state, sit.questions)
        self._on_outcome(npc, flight, outcome, self.clock())

    def _on_outcome(self, npc: str, flight: Flight, outcome: Outcome, now: float) -> None:
        self.stats.outcomes[outcome.kind] = self.stats.outcomes.get(outcome.kind, 0) + 1
        if flight.urgent is False:
            if outcome.kind in ("expired", "error"):
                self.slowdown = min(4.0, self.slowdown * 1.25)
            elif outcome.kind == "answers":
                self.slowdown = max(1.0, self.slowdown * 0.98)
        rec = self.npcs.get(npc)
        if rec is None or rec.flight is not flight:
            return
        rec.flight = None
        if outcome.kind != "answers" or not outcome.answers:
            self._settle_holds(rec, flight.sent, None)
            self._schedule(rec, now)
            return
        # the question whose answer is applied: a plan's may be none (only `do`, in shadow)
        asked = flight.question if flight.purpose == "plan" else flight.purpose
        answer = outcome.answers.get(asked) if asked is not None else None
        if rec.tier is Tier.OFF or rec.version != flight.version or rec.investigation is not None:
            self.stats.discarded += 1  # something happened meanwhile: a new pending request covers it
            self._settled(npc, flight, answer, False, "outdated")
            if rec.activity == "body":
                self._settle_holds(rec, flight.sent, None)
            self._shadow(npc, flight, outcome)
            if rec.pending is None:
                self._schedule(rec, now)
            return
        rec.last_decision = now
        self._schedule(rec, now)
        if not answer:
            self._settle_holds(rec, flight.sent, None)
            self._shadow(npc, flight, outcome)
            return
        decision = self._react_outcome(rec, flight, answer, now)
        if flight.purpose != "react":
            self._shadow(npc, flight, outcome)
            return
        self._settle_holds(rec, flight.sent, decision)
        # what the moment means beyond the body's reaction, asked in the background: how the attitude toward
        # the hero changed (after an urgent reaction about him; threats from others say nothing about him)
        # and whether it calls for the npc's own words
        after = []
        if decision is not None and flight.urgent and self.cfg.ask_attitude and flight.target is None:
            after.append("attitude")
        if self.on_escalate is not None and (flight.urgent or flight.target):
            after.append("escalate")
        if after:
            self._spawn(self._ask_after(npc, flight, decision, after))

    def _react_outcome(self, rec: NpcRecord, flight: Flight, answer: dict, now: float) -> Decision | None:
        """Apply an answer if it is sure enough and not a repeat; the decision, if one was applied."""
        npc = rec.id
        choice, conf = answer["choice"], float(answer["confidence"])
        if flight.purpose == "plan":
            threshold = self.cfg.plan_threshold
        else:
            threshold = self.cfg.option_threshold.get(choice, self.cfg.react_threshold)
        if conf < threshold:
            self.stats.unsure += 1
            self._settled(npc, flight, answer, False, "unsure")
            return None
        if choice in NO_ACTION:
            self.stats.no_action += 1
            self._settled(npc, flight, answer, False, "no action")
            return None
        key = applied_key(choice, flight.target)
        if now - rec.last_applied.get(key, float("-inf")) < self.cfg.cooldown_s.get(
            choice, self.cfg.default_cooldown_s
        ):
            self.stats.cooldown += 1
            self._settled(npc, flight, answer, False, "cooldown")
            return None
        if self.allow is not None and (why := self.allow(rec, choice)) is not None:
            rec.last_applied[key] = now  # not asked to do it again at once
            self.stats.vetoed += 1
            self._settled(npc, flight, answer, False, f"veto: {why}")
            return None
        trip = flight.options.get(choice) if flight.purpose == "plan" else None
        if trip is not None and not trip.trip:
            trip = None  # its own activity: the applier starts it
        if trip is not None and self.on_plan is None:
            # nobody to walk it there: the routine goes on
            self._settled(npc, flight, answer, False, "no desk")
            return None
        rec.last_applied[key] = now
        self.stats.applied += 1
        newest = max(rec.stimuli, key=lambda s: s.at, default=None)
        decision = Decision(
            npc,
            flight.purpose,
            choice,
            conf,
            answer.get("probabilities", {}),
            flight.stimuli,
            latency_ms=(now - flight.sent) * 1000,
            target=flight.target,
            args=(flight.plan_args if flight.purpose == "plan" else flight.react_args).get(choice),
            own=newest is not None and newest.other.upper() == rec.id,
        )
        if choice == "investigate" and flight.purpose == "react":
            # The bridge owns the walk, scan and return. An idle/timer must not send it back to the
            # same noise after it has finished; a newly heard sound gets a fresh observation.
            for stimulus in rec.stimuli:
                if stimulus.kind != "noise" and not stimulus.policy.threat:
                    self._defer(rec, stimulus)
            rec.stimuli = [s for s in rec.stimuli if s.policy.threat]
            rec.investigation = Investigation(now)
            rec.version += 1
            rec.pending = None
        if flight.purpose == "react":
            rec.body = (choice, now) if choice in TAKES_BODY else None
        if choice == "approach_talk" and flight.purpose == "react" and self.on_approach is not None:
            # the desk walks it and lets it speak; the applier would send a second go_to_npc
            log.info("decision %s react=approach_talk handed to the initiative desk", npc)
            self._settled(npc, flight, answer, True, "initiative desk")
            self.on_approach(decision, rec)
        elif trip is not None and self.on_plan is not None:
            # the day desk walks it there and sees what comes of it (arrive); the applier sends nothing
            log.info("decision %s %s=%s handed to the day desk", npc, flight.question, choice)
            self._settled(npc, flight, answer, True, "day desk")
            self.on_plan(decision, rec, trip)
        else:
            self._settled(npc, flight, answer, True, "")
            self.applier.apply(decision)
        return decision

    def _settled(self, npc: str, flight: Flight, answer: dict | None, applied: bool, why: str) -> None:
        """The applied question's row (a plan's is not capped); a plan also keeps what it did, for the row of
        the question asked in shadow. A plan with only `do` in shadow has no row of its own."""
        if flight.purpose != "plan":
            self._trace(npc, flight, answer, applied, why)
            return
        choice = (answer or {}).get("choice")
        flight.outcome = {"question": flight.question, "choice": choice, "applied": applied, "why": why}
        if flight.question is not None:
            self._trace(npc, flight, answer, applied, why, capped=False)

    def _shadow(self, npc: str, flight: Flight, outcome: Outcome) -> None:
        """The answer to the question asked in shadow (the do menu, until it is measured) as a brain row,
        never applied: `code` is what the applied question did, the decider's times and model come along."""
        answer = (outcome.answers or {}).get(flight.shadow) if flight.shadow is not None else None
        if not answer:
            return
        self.stats.shadow += 1
        why = "not asked" if flight.question is None else "no answer"
        code = flight.outcome or {"question": flight.question, "choice": None, "applied": False, "why": why}
        extra = {
            "code": code,
            "infer_ms": outcome.infer_ms,
            "queue_ms": outcome.queue_ms,
            "model": getattr(self.decider, "model", None),
        }
        self._trace(npc, flight, answer, False, "shadow", question=flight.shadow, extra=extra, capped=False)

    async def _ask_after(self, npc: str, flight: Flight, reaction: Decision | None, names: list[str]) -> None:
        """`attitude` and/or `escalate` about the reaction's situation, in one request: the decider reads the
        state once for both (its shared prefix)."""
        questions: dict[str, dict] = {}
        for name in names:
            questions |= getattr(self.builder, name)(flight.situation).questions
        # words are worth little when late; the attitude can wait
        deadline = self.cfg.deadline_ms["escalate" if "escalate" in names else "attitude"]
        sent = self.clock()
        out = await self.decider.decide(
            f"{flight.rid}{''.join(n[0] for n in names)}",
            "background",
            0.0,
            deadline,
            flight.situation.state,
            questions,
        )
        if out.kind != "answers" or not out.answers:
            return
        if a := out.answers.get("attitude"):
            self._trace(npc, flight, a, True, "", question="attitude", questions=questions, sent=sent)
            self.applier.apply(
                Decision(
                    npc,
                    "attitude",
                    str(round(float(a["score"]))),
                    float(a["confidence"]),
                    a.get("probabilities", {}),
                    flight.stimuli,
                    attitude=float(a["score"]),
                )
            )
        if a := out.answers.get("escalate"):
            level = float(a["score"])
            log.info("escalate %s %.1f after %s", npc, level, ",".join(flight.stimuli))
            self._trace(npc, flight, a, True, "", question="escalate", questions=questions, sent=sent)
            if self.on_escalate is not None:
                self.on_escalate(npc, level, reaction, flight)

    def _trace(
        self,
        npc: str,
        flight: Flight,
        answer: dict | None,
        applied: bool,
        why: str,
        *,
        question: str | None = None,
        questions: dict[str, dict] | None = None,
        sent: float | None = None,
        extra: dict[str, Any] | None = None,
        capped: bool = True,
    ) -> None:
        """One answered question as a `brain` row: the state, the options' texts, the answer and what came of
        it (`why` it was not applied); a day question also what each option is (`args`), `extra` adds more.
        At most TRACE_PER_MIN rows per npc a minute, `capped` ones (the day plan's are rare: not counted)."""
        if self.trace is None:
            return
        now = self.clock()
        if capped:
            start, rows = self._traced.get(npc, (now, 0))
            if now - start >= 60.0:
                start, rows = now, 0
            if rows >= TRACE_PER_MIN:
                return
            self._traced[npc] = (start, rows + 1)
        question = question or flight.question or flight.purpose
        criteria = (questions or flight.situation.questions).get(question, {}).get("criteria", {})
        after = question not in (flight.purpose, flight.question)
        rec = self.npcs.get(npc)
        row: dict[str, Any] = {
            "type": "brain",
            "t": round(time.time(), 3),
            "npc": npc,
            "rid": flight.rid,
            "question": question,
            "lane": "urgent" if flight.urgent and not after else "background",
            "tier": rec.tier.name if rec is not None else None,
            "state": flight.situation.state,
            "options": dict(criteria) if isinstance(criteria, dict) else list(criteria),
            "stimuli": list(flight.stimuli),
        }
        answer = answer or {}
        if "score" in answer:
            row["score"] = round(float(answer["score"]), 3)
        else:
            row["choice"] = answer.get("choice")
        row["confidence"] = round(float(answer.get("confidence", 0.0)), 3)
        row["probabilities"] = answer.get("probabilities", {})
        row["applied"] = applied
        row["why"] = why
        row["latency_ms"] = round((now - (flight.sent if sent is None else sent)) * 1000, 1)
        if flight.purpose == "plan" and question in ("plan", "do"):
            row["args"] = self._option_args(flight, question)
        row.update(extra or {})
        self.trace.write(row)

    @staticmethod
    def _option_args(flight: Flight, question: str) -> dict[str, dict[str, Any]]:
        """Per wire key of a day question, what the option is: its stable id and start_activity args (a do
        option also its kind, concern and partner). Training maps a positional key back to its id by it."""
        if question == "do":
            menu = flight.options if flight.question == "do" else flight.shadow_menu
            return {
                key: {"id": o.id, "args": o.args, "kind": o.kind, "concern": o.concern, "partner": o.partner}
                for key, o in menu.items()
            }
        ids = plan_ids(flight.plan_args)
        criteria = flight.situation.questions.get("plan", {}).get("criteria", {})
        return {key: {"id": ids.get(key, key), "args": flight.plan_args.get(key)} for key in criteria}

    def _schedule(self, rec: NpcRecord, now: float) -> None:
        if rec.tier is Tier.OFF:
            return
        lo, hi = self._period(rec.tier)
        rec.next_check = now + self.rng.uniform(lo, hi)

    def _period(self, tier: Tier) -> tuple[float, float]:
        if tier is Tier.NEAR:
            return self.cfg.near_period_s
        lo, hi = self.cfg.mid_period_s if tier is Tier.MID else self.cfg.far_period_s
        return lo * self.slowdown, hi * self.slowdown

    def _refill(self, now: float) -> None:
        self._tokens = min(self.cfg.bg_rate, self._tokens + (now - self._refilled) * self.cfg.bg_rate)
        self._refilled = now

    def _spawn(self, coro) -> None:
        task = asyncio.get_running_loop().create_task(coro)
        self._tasks.add(task)
        task.add_done_callback(self._tasks.discard)
