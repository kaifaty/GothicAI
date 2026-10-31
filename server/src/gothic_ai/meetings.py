"""Meetings with the hero (docs/npc-tools.md, wave 2 step 4: `meet`).

A meeting is a term of a deal (deals.py): where — a place, as for a lead (guide.py) — and when — a game day
and an hour, fields, no free text — and how many hours the npc waits there. The hero gives his word with Y;
the meeting goes into the game's diary.

The server keeps it, not the save (`routine_add`, `set_routine` would stay in the save): as far ahead of the
hour as the walk there takes, the npc's routine gives way to an activity at the place's waypoint
(`start_activity` with `arrive`: it stands there with its arms crossed till the meeting is over), and the
planner gives its body nothing else meanwhile. Cut off (a fight, a dialog, a load), it is given again.
When the clock leaps (the hero sleeps: Wld_SetTime), the engine puts everyone at their routine's spot; the
world went on meanwhile, so an npc due at its meeting by then is put there at once (`start_activity.place`),
and one whose meeting is over by then waited there in vain: he slept through it.
- The hero comes (within MEET_CM of it while it waits): the meeting is kept, a fact `meet_kept`, and the npc
  speaks first, as on its own initiative (initiative.py).
- He does not come by the end: his word is broken (`broken` with `promise: meet`), as with a debt not paid
  back: it takes his word no more (trust.py), the rumor goes round (social.py), and it has it on its mind
  (concerns.py) till it has told him so. Its routine goes on.
- It does not get there itself: no word of his is broken, a fact `meet_failed`.
- It never intended to come: it stays at its routine; at the agreed hour `npc_broken {how: no_show}`
  records its refusal. The hero's word is not broken, even when he does not come either.
Meetings are facts of the chronicle: a load brings them back.
"""

from __future__ import annotations

import asyncio
import logging
import math
import uuid
from collections.abc import Callable, Coroutine
from dataclasses import dataclass
from typing import TYPE_CHECKING, Any, Literal

from .bodies import Bodies
from .brain.planner import TAKES_BODY, NpcPlanner, NpcRecord
from .budget import Budget
from .chronicle import Chronicle, Fact
from .concerns import RANK, Concern
from .game import GameError, GameLink
from .guide import Guide, Way
from .initiative import TOOLS
from .journal import Journal
from .llm import Tool, ToolCall
from .npc_cheat import breach, keeps
from .protocol import GameMessage, Loading, Session, Snapshot
from .turn import ToolOutcome

if TYPE_CHECKING:
    from .dialogue import Conversation, DialogueManager
    from .talk import TalkDesk

log = logging.getLogger(__name__)

DAY = 24 * 60  # game minutes
MEET_CM = 800.0  # the hero this near the npc waiting there: he came
EARLY_MIN = 30  # he may come so many game minutes before the hour
MIN_AHEAD = 10  # a meeting is set at least so far ahead of now (game minutes)...
MAX_DAYS = 3  # ...and at most so many days ahead
MAX_HOURS = 4  # it waits so many hours at most
WALK_M_PER_MIN = 3.0  # a walk: about 3 m a game minute (0.85 m/s, a game minute is about 4.4 s)
WAY_LONGER = 1.5  # the way by the waynet is about this much longer than the straight line
SPARE_MIN = 15  # it sets off so much earlier than the walk takes
LEAD_MIN = (20, 150)  # ...and at least, at most so far ahead of the hour
TICK_S = 5.0  # how often the game's clock is read while a meeting is pending
GAME_MIN_S = 4.0  # a game minute takes a little more real seconds than this
LEAP_MIN = 45  # the clock read so many game minutes further than real time took it: it leapt (a sleep)
RETRY_MIN = 5  # a walk cut off is given again so many game minutes later...
TRIES = 3  # ...so many times at most
WAIT_STATE = "ZS_STAND_ARMSCROSSED"  # how it waits there (TA_Stand_ArmsCrossed)
RADIUS_CM = 300  # start_activity's arrive radius
WALK_S = 600.0  # the walk there at most, real seconds
OPEN = ("agreed", "going", "there")

State = Literal["agreed", "going", "there", "kept", "missed", "failed", "no_show"]


def clock(minute: int) -> str:
    return f"{minute % DAY // 60:02d}:{minute % 60:02d}"


@dataclass(frozen=True)
class MeetTerm:
    """Where and when: a deal's `meet` (deals.py)."""

    way: Way
    start: int  # game minutes from day 0, 00:00
    end: int

    def when(self) -> str:
        """«день 3, 06:00–08:00»: days as the npc sees them in its situation (from 1)."""
        return f"день {self.start // DAY + 1}, {clock(self.start)}–{clock(self.end)}"

    def text(self) -> str:
        return f"{self.way.where_ru()}, {self.when()}"


@dataclass
class Meeting:
    id: str
    npc: str
    name: str
    term: MeetTerm
    deal: str = ""
    lead_min: int = LEAD_MIN[0]  # it sets off so far ahead of the hour
    state: State = "agreed"
    retry: int = 0  # a walk cut off: not again before this game minute
    tries: int = 0
    raised: bool = False  # missed: it has told him so
    place: bool = False  # the clock leapt past its setting off: there at once, no walk

    def fact(self) -> Fact:
        way = self.term.way
        return {"fact": "meet", "id": self.id, "npc": self.npc, "name": self.name, "place": way.place.id,
                "points": list(way.points), "pos": list(way.pos), "door": way.door, "start": self.term.start,
                "end": self.term.end, "lead_min": self.lead_min, "deal": self.deal}  # fmt: skip


class Meetings:
    """The npc's meetings with the hero: what a deal's `meet` may be, and the meetings themselves (a
    TurnExtension: what the npc has agreed on; the planner's hook: its body is the meeting's meanwhile)."""

    def __init__(
        self,
        link: GameLink,
        dialogue: DialogueManager,
        talk: TalkDesk,
        guide: Guide,
        chronicle: Chronicle,
        bodies: Bodies,
        *,
        budget: Budget | None = None,
        journal: Journal | None = None,
    ) -> None:
        self.link = link
        self.dialogue = dialogue
        self.talk = talk
        self.guide = guide
        self.chronicle = chronicle
        self.bodies = bodies
        self.budget = budget
        self.journal = journal or dialogue.journal
        self.meetings: dict[str, Meeting] = {}
        self.now: int | None = None  # the game minute, as last read
        self._read_at: float | None = None  # ...at this real time (the loop's clock)
        self._world = 0
        self._tasks: set[asyncio.Task[Any]] = set()
        self._next_allow: Callable[[NpcRecord, str], str | None] | None = None
        guide.meet_too = True
        link.listeners.append(self._on_message)
        self._load()

    def attach_planner(self, planner: NpcPlanner) -> None:
        """The planner gives the body of an npc on its way to a meeting, or waiting there, nothing else."""
        self._next_allow, planner.allow = planner.allow, self.allow

    def allow(self, rec: NpcRecord, choice: str) -> str | None:
        if choice not in TAKES_BODY and any(
            m.npc == rec.id and m.state in ("going", "there") for m in self.meetings.values()
        ):
            return "на встрече с героем"
        return self._next_allow(rec, choice) if self._next_allow is not None else None

    def _on_message(self, msg: GameMessage) -> None:
        match msg:
            case Loading():
                self._world += 1  # its walk and its activity went with the world: given again
                self.now = self._read_at = None
                for m in self.meetings.values():
                    if m.state in ("going", "there"):
                        m.state, m.retry = "agreed", 0
            case Session():
                self._load()

    def _load(self) -> None:
        """The meetings of the save branch, from its facts."""
        self.meetings.clear()
        for f in self.chronicle.facts():
            match f.get("fact"):
                case "meet":
                    if (m := self._of(f)) is not None:
                        self.meetings[m.id] = m
                case "meet_kept" if (m := self.meetings.get(f.get("meet", ""))) is not None:
                    m.state = "kept"
                case "meet_failed" if (m := self.meetings.get(f.get("meet", ""))) is not None:
                    m.state = "failed"
                case "npc_broken" if f.get("how") == "no_show" and (
                    m := self.meetings.get(f.get("meet", ""))
                ):
                    m.state = "no_show"
                case "broken" if f.get("promise") == "meet" and (m := self.meetings.get(f.get("meet", ""))):
                    m.state = "missed"
                case "meet_raised" if (m := self.meetings.get(f.get("meet", ""))) is not None:
                    m.raised = True

    def _of(self, f: Fact) -> Meeting | None:
        place = self.guide.book.get(str(f.get("place", ""))) if self.guide.book is not None else None
        if place is None:
            return None
        pos = tuple(float(x) for x in f.get("pos", place.center or (0, 0, 0)))
        way = Way(place, tuple(f.get("points", ())), (pos[0], pos[1], pos[2]), bool(f.get("door")))
        term = MeetTerm(way, int(f["start"]), int(f["end"]))
        lead = int(f.get("lead_min", LEAD_MIN[0]))
        return Meeting(
            str(f["id"]), str(f["npc"]), str(f.get("name", "")), term, str(f.get("deal", "")), lead
        )

    # --- the deal's term ------------------------------------------------------------------------

    async def term(self, npc: str, raw: Any, snap: Snapshot | None) -> tuple[MeetTerm | None, str]:
        """A deal's `meet` (where, day, hour, minute, hours); None with "" for none, or why not."""
        raw = raw if isinstance(raw, dict) else {}
        where = str(raw.get("where") or "").strip()
        if not where:
            return None, ""
        now = _now(snap) if snap is not None else self.now
        if now is None:
            return None, "не знаешь, который час"
        try:
            day, hour, minute = int(raw.get("day", 0)), int(raw.get("hour", -1)), int(raw.get("minute", 0))
            hours = int(raw.get("hours", 2))
        except (TypeError, ValueError):
            return None, "встреча — числами: день, час, минута, сколько часов ждёшь"
        if day < 1 or not 0 <= hour <= 23 or not 0 <= minute <= 59:
            return None, "день — номер дня игры, как в «Время: день N»; час 0–23, минута 0–59"
        if not 1 <= hours <= MAX_HOURS:
            return None, f"ждать можно от 1 до {MAX_HOURS} часов"
        start = (day - 1) * DAY + hour * 60 + minute
        if start < now + MIN_AHEAD:
            return (
                None,
                f"это время уже прошло или вот-вот наступит: сейчас день {now // DAY + 1}, {clock(now)}",
            )
        if start > now + MAX_DAYS * DAY:
            return None, f"так далеко вперёд не загадывай: не дальше {MAX_DAYS} дней"
        end = start + hours * 60
        busy = [m for m in self.meetings.values() if m.npc == npc.upper() and m.state in OPEN]
        if any(m.term.start < end and start < m.term.end for m in busy):
            return None, f"в это время у тебя уже встреча: {busy[0].term.text()}"
        way, why = await self.guide.way(npc, where, snap, meet=True)
        if way is None:
            return None, why
        return MeetTerm(way, start, end), ""

    def add(self, npc: str, name: str, term: MeetTerm, deal: str) -> Meeting:
        """The hero said yes: the meeting, its fact, its entry in the game's diary."""
        npc = npc.upper()
        here = self.link.snapshots.get(npc)
        far_m = math.dist(here.npc.pos, term.way.pos) / 100 if here is not None else 0.0
        walk = math.ceil(far_m * WAY_LONGER / WALK_M_PER_MIN)
        lead = min(LEAD_MIN[1], max(LEAD_MIN[0], walk + SPARE_MIN))
        if self.now is None and here is not None:  # the clock from here on: a sleep right away is a leap too
            self.now, self._read_at = _now(here), asyncio.get_running_loop().time()
        m = Meeting(f"meet-{uuid.uuid4().hex[:6]}", npc, name, term, deal, lead)
        self.meetings[m.id] = m
        self.chronicle.record(m.fact())
        self.journal.add("meet", npc=npc, name=name, meet=m.id, what=term.text())
        log.info("meeting %s with %s: %s, sets off %d min ahead", m.id, npc, term.text(), lead)
        self._diary(m, f"{name} будет ждать меня {term.text()}.", "LOG_RUNNING")
        return m

    # --- TurnExtension --------------------------------------------------------------------------

    all_tools: tuple[Tool, ...] = ()

    async def prepare(self, npc: str) -> None:
        return None

    def situation(self, npc: str) -> str | None:
        npc = npc.upper()
        lines = []
        for m in self.meetings.values():
            if m.npc != npc:
                continue
            match m.state:
                case "agreed":
                    lines.append(f"Ты договорился с героем о встрече: {m.term.text()}.")
                case "going":
                    lines.append(f"Ты идёшь на встречу с героем: {m.term.text()}.")
                case "there":
                    lines.append(
                        f"Ты ждёшь героя на встрече: {m.term.way.where_ru()}, до {clock(m.term.end)}."
                    )
                case "no_show":
                    lines.append(f"Ты не пришёл на обещанную встречу с героем: {m.term.text()}.")
                case "missed" if not m.raised:
                    lines.append(
                        f"Герой не пришёл на встречу, о которой вы договорились: {m.term.text()}. Он не "
                        "сдержал слово."
                    )
                    if self.talk.partner is not None and self.talk.partner.npc == npc:
                        self._raised(m)  # it has it in front of it while talking to him: told
        return "\n".join(lines) or None

    def tools(self, npc: str) -> list[Tool]:
        return []

    async def handle(
        self, dialogue: DialogueManager, conv: Conversation, call: ToolCall, snap: Snapshot | None
    ) -> ToolOutcome | None:
        return None

    def concerns(self, npc: str) -> list[Concern]:
        """A meeting he did not come to, not told him of yet: it may come up to him about it (concerns.py)."""
        npc = npc.upper()
        spoken = {f.get("occasion") for f in self.chronicle.facts("initiative") if f.get("npc") == npc}
        out = []
        for m in self.meetings.values():
            key = f"meet_missed:{m.id}"
            if m.npc != npc or m.state != "missed" or m.raised or key in spoken:
                continue
            ru = f"герой не пришёл на встречу, о которой вы договорились: {m.term.text()}"
            out.append(Concern(key, npc, RANK["debt_broken"], "make the hero answer for not coming to the "
                               "meeting he promised", ru))  # fmt: skip
        return out

    # --- the meetings ---------------------------------------------------------------------------

    async def run(self) -> None:
        """The game's clock, read while a meeting is pending."""
        while True:
            await asyncio.sleep(TICK_S)
            if not any(m.state in OPEN for m in self.meetings.values()):
                continue
            await self.link.wait_world()
            try:
                state = await self.link.story_state()
            except (GameError, TimeoutError) as err:
                log.info("meetings: %s", str(err) or type(err).__name__)
                continue
            if all(k in state for k in ("day", "hour", "minute")):
                now = int(state["day"]) * DAY + int(state["hour"]) * 60 + int(state["minute"])
                self.read(now, asyncio.get_running_loop().time())

    def read(self, now: int, at: float) -> None:
        """The game's clock read at real time `at`: further on than the real time since the last read takes
        it, it has leapt."""
        if self.now is not None and self._read_at is not None:
            if now - self.now > (at - self._read_at) / GAME_MIN_S + LEAP_MIN:
                self._leapt(self.now, now)
        self._read_at = at
        self.tick(now)

    def _leapt(self, before: int, now: int) -> None:
        """The clock leapt forward (the hero slept: Wld_SetTime). The engine has put everyone at their
        routine's spot (World::setDayTime), the meeting's npc too, its walk and its activity gone. The world
        went on meanwhile: due at its meeting by now, it is there at once; its meeting over by now, it waited
        there in vain."""
        self._world += 1  # the walk given before the leap: whatever comes of it is no news
        for m in self.meetings.values():
            if m.state not in OPEN or m.tries >= TRIES:
                continue  # over, or it has given up getting there
            if not keeps(self.chronicle, m.deal, "meet"):
                continue
            if now >= m.term.end:
                m.state = "there"  # he slept through it: tick finds him not come
            elif now >= m.term.start - m.lead_min:
                m.state, m.retry, m.place = "agreed", 0, True
            else:
                continue
            log.info("meeting %s: the clock leapt %s → %s, %s", m.id, clock(before), clock(now), m.state)

    def tick(self, now: int) -> None:
        """What comes of each meeting at `now` (a game minute)."""
        self.now = now
        for m in list(self.meetings.values()):
            if m.state in OPEN and not keeps(self.chronicle, m.deal, "meet"):
                if now >= m.term.start:
                    self._no_show(m)
            elif m.state in OPEN and now >= m.term.end:
                self._over(m)
            elif m.state == "agreed" and now >= max(m.term.start - m.lead_min, m.retry):
                m.state = "going"
                self._spawn(self._walk(m, self._world))
            elif m.state == "there" and self._came(m, now):
                self._kept(m)

    def _no_show(self, m: Meeting) -> None:
        """It broke its own word, not the hero's, without trying to walk there."""
        m.state = "no_show"
        breach(
            self.chronicle,
            m.npc,
            m.deal,
            "no_show",
            meet=m.id,
            npc_name=m.name,
            what=m.term.text(),
            **self._when(),
        )
        log.info("meeting %s: %s chose not to come", m.id, m.npc)
        self._diary(m, f"{m.name} не пришёл на обещанную встречу.", "LOG_FAILED")

    async def _walk(self, m: Meeting, world: int) -> None:
        """There, and it waits till the end."""
        now = self.now if self.now is not None else m.term.start
        place, m.place = m.place, False
        out = await self.bodies.act(
            m.npc,
            "start_activity",
            state=WAIT_STATE,
            waypoint=m.term.way.points[0],
            minutes=max(1, m.term.end - now),
            arrive=True,
            radius=RADIUS_CM,
            timeout=WALK_S,
            **({"place": True} if place else {}),
        )
        if world != self._world or m.state != "going":
            return  # a load, or the meeting is over meanwhile
        if out.status == "arrived":
            m.state = "there"
            log.info("meeting %s: %s is there, %s", m.id, m.npc, m.term.text())
            self.journal.add("meet_there", npc=m.npc, name=m.name, meet=m.id)
            self.dialogue.remember(
                m.npc, m.name, f"Ты пришёл на встречу с героем и ждёшь его: {m.term.text()}"
            )
            if self.now is not None and self._came(m, self.now):
                self._kept(m)
            return
        if out.status == "rejected":
            m.place = place  # it did not go at all: still there at once when it does
        if not (out.status == "rejected" and "busy" in out.detail):  # held for a talk: not a try, only later
            m.tries += 1
        log.info("meeting %s: %s's walk there %s (%s), try %d", m.id, m.npc, out.status, out.detail, m.tries)
        m.state = "agreed"
        m.retry = (self.now or now) + RETRY_MIN
        if m.tries >= TRIES:
            m.retry = m.term.end  # it gives up getting there: over at the end, no word of his broken

    def _came(self, m: Meeting, now: int) -> bool:
        entry = self.talk.people.get(m.npc)
        return entry is not None and entry.distance <= MEET_CM and now >= m.term.start - EARLY_MIN

    def _kept(self, m: Meeting) -> None:
        m.state = "kept"
        self.chronicle.record(
            {"fact": "meet_kept", "meet": m.id, "npc": m.npc, "name": m.name, **self._when()}
        )
        self.journal.add("meet_kept", npc=m.npc, name=m.name, meet=m.id)
        log.info("meeting %s kept: the hero came to %s", m.id, m.npc)
        self._diary(m, "Я пришёл на встречу.", "LOG_SUCCESS")
        self._spawn(self._greet(m, self._world))

    async def _greet(self, m: Meeting, world: int) -> None:
        """It speaks first: he came as they agreed (the way initiative.py opens a conversation)."""
        npc, note = m.npc, f"Герой пришёл на встречу, как вы договорились: {m.term.text()}"
        if self.talk.partner is not None or (self.budget is not None and not self.budget.allow(npc)):
            self.dialogue.remember(npc, m.name, note)
            return
        if not await self.talk.listen(npc) or world != self._world:
            self.dialogue.remember(npc, m.name, note)
            return
        if not self.talk.begin(npc, m.name, posture="listen"):
            self.talk.free(npc, "listen")
            self.dialogue.remember(npc, m.name, note)
            return
        self.dialogue.event_turn(
            npc,
            f"[{note}. Заговори с ним первым.]",
            relevant=lambda: self.talk.partner is not None and self.talk.partner.npc == npc,
            tools=TOOLS,
            kind="meet",
        )

    def _over(self, m: Meeting) -> None:
        """The end of the meeting's time: he did not come (it waited), or it did not get there."""
        if m.state == "there":
            m.state = "missed"
            what = m.term.text()
            fact = {"fact": "broken", "promise": "meet", "meet": m.id, "npc": m.npc, "name": m.name}
            self.chronicle.record({**fact, "what": what, **self._when()})
            self.journal.add("meet_missed", npc=m.npc, name=m.name, meet=m.id)
            log.info("meeting %s missed: the hero did not come to %s", m.id, m.npc)
            self.dialogue.remember(
                m.npc,
                m.name,
                f"Герой не пришёл на встречу, о которой вы договорились: {what}. Он не сдержал слово",
            )
            self._diary(m, "Я не пришёл на встречу.", "LOG_FAILED")
            return
        m.state = "failed"
        self.chronicle.record(
            {"fact": "meet_failed", "meet": m.id, "npc": m.npc, "name": m.name, **self._when()}
        )
        self.journal.add("meet_failed", npc=m.npc, name=m.name, meet=m.id)
        log.info("meeting %s failed: %s did not get there", m.id, m.npc)
        self.dialogue.remember(m.npc, m.name, f"Ты так и не дошёл до места встречи с героем: {m.term.text()}")
        self._diary(m, f"{m.name} так и не пришёл.", "LOG_OBSOLETE")

    def _raised(self, m: Meeting) -> None:
        m.raised = True
        self.chronicle.record({"fact": "meet_raised", "meet": m.id, "npc": m.npc})

    def _when(self) -> dict[str, int]:
        if self.now is None:
            return {}
        return {"day": self.now // DAY, "hour": self.now % DAY // 60, "minute": self.now % 60}

    def _diary(self, m: Meeting, text: str, status: str) -> None:
        """Its topic in the game's diary: «Встреча: Малет, день 3»."""
        topic = f"Встреча: {m.name}, день {m.term.start // DAY + 1}"
        effect = {"do": "journal", "topic": topic, "text": text, "status": status}
        self._spawn(self._effect(m.npc, effect))

    async def _effect(self, npc: str, effect: dict[str, Any]) -> None:
        try:
            await self.link.story_effect(npc, [effect])
        except (GameError, TimeoutError) as err:
            log.info("meeting diary of %s: %s", npc, str(err) or type(err).__name__)

    def _spawn(self, coro: Coroutine[Any, Any, Any]) -> None:
        task = asyncio.get_running_loop().create_task(coro)
        self._tasks.add(task)
        task.add_done_callback(self._tasks.discard)


def _now(snap: Snapshot) -> int:
    w = snap.world
    return w.day * DAY + w.hour * 60 + w.minute
