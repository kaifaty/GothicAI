"""The npc leads the hero to a place (docs/npc-tools.md, wave 2 step 4: `lead`).

The hero asks the way and the npc leads him there: a term of a deal (deals.py) — the greedy takes a fee, the
kind leads for nothing, and either way he says yes with Y. Where to is a place of the book (places.py), by its
id or its name as `where` finds it (whereabouts.py): a public one, or one of its own (where its routine has
it); into another's house only up to its way in. Not the plot's npc's, the posts and escorts (storyguard.py),
one whose routine the scripts have set to lead or follow the hero (ZS_GUIDE…, ZS_FOLLOW…), nor a companion: it
walks with him already and goes first by `go_to` (orders.py).

On the way its body is held (talk.py) and walks by the waynet (`go_to_waypoint`); what it answers him is said
on the move. The hero falls behind — farther than LAG_CM, and farther from the place than the npc — and it
stops, turns to him, calls him and waits (WAIT_S at most, then it goes back to its business); once he comes
up, it goes on. There it turns to him and says so, and the chronicle has a fact `led`.
"""

from __future__ import annotations

import asyncio
import logging
import math
import time
from collections.abc import Callable
from dataclasses import dataclass
from typing import TYPE_CHECKING, Any, Literal

from .chronicle import Chronicle
from .game import GameError, GameLink
from .journal import Journal
from .llm import Tool, ToolCall
from .lore import npc_pos
from .places import Place, PlaceBook
from .protocol import GameMessage, Loading, RoutineEntry, Session, Snapshot
from .storyguard import ESCORT_STATES, StoryGuard
from .turn import ToolOutcome

if TYPE_CHECKING:
    from .dialogue import Conversation, DialogueManager
    from .talk import TalkDesk
    from .whereabouts import Whereabouts

log = logging.getLogger(__name__)

LEAD = "lead"  # what its body is held for: an errand, the conversation waits for it (talk.py)
LEAD_CM = 80000.0  # it leads this far at most (800 m, a straight line)
ARRIVE_CM = 500.0  # there: this near the place's point
LAG_CM = 1200.0  # the hero this far behind: it waits for him...
CLOSE_CM = 600.0  # ...till he is this near again
WAIT_S = 60.0  # it waits for him so long, then goes back to its business
CALLS = 2  # it calls him so many times on one way; then it waits in silence
WAY_S = 600.0  # the whole way at most
LEG_MS = 60000  # one go_to_waypoint: a longer way is walked in legs
EVERY_S = 0.5  # how often it looks back (an attached npc's snapshot comes twice a second)
ROUTINE_S = 600.0  # an npc's routine, as read for whether it may lead, holds this long
STUCK_S = 6.0  # it has not moved this long on its way...
STUCK_CM = 100.0  # ...farther than this: it is stuck
UNSTICK = 2  # it gets back onto the waynet so many times on one way, then gives the way up

How = Literal["arrived", "lost", "failed", "taken"]


@dataclass(frozen=True)
class Way:
    """Where the npc leads the hero: a place, or another's house up to its way in."""

    place: Place
    points: tuple[str, ...]  # it walks to the nearest of these waypoints (none: the nearest to pos)
    pos: tuple[float, float, float]
    door: bool = False  # up to its way in only

    def title(self) -> str:
        """For the deal's window and the npc: «мельница фермы Лобарта», «дом Лобарта, до входа»."""
        return f"{self.place.ru}, до входа" if self.door else self.place.ru

    def where_ru(self) -> str:
        return f"у входа: {self.place.ru}" if self.door else self.place.where_ru


@dataclass
class Lead:
    npc: str
    name: str
    way: Way
    deal: str
    state: Literal["starting", "going", "waiting"] = "starting"
    calls: int = 0
    stopping: bool = False  # it stops the walk itself (to wait for him): the walk's `cancelled` is its own


class Guide:
    """The npc's leading the hero: what a deal's `lead` may be, and the way itself (a TurnExtension: what it
    is doing on the way)."""

    def __init__(
        self,
        link: GameLink,
        dialogue: DialogueManager,
        talk: TalkDesk,
        book: PlaceBook | None,
        guard: StoryGuard,
        chronicle: Chronicle,
        *,
        whereabouts: Whereabouts | None = None,
        companion: Callable[[str], bool] = lambda npc: False,
        journal: Journal | None = None,
    ) -> None:
        self.link = link
        self.dialogue = dialogue
        self.talk = talk
        self.book = book
        self.guard = guard
        self.chronicle = chronicle
        self.whereabouts = whereabouts  # a place by its name, as `where` finds it
        self.companion = companion
        self.journal = journal or dialogue.journal
        self.leads: dict[str, Lead] = {}  # npc -> its way under way
        self.meet_too = False  # meetings.py is on: the npc is told it may set the hero a meeting too
        self._tasks: dict[str, asyncio.Task[None]] = {}
        self._routines: dict[str, tuple[float, list[RoutineEntry]]] = {}
        self._may: dict[str, str | None] = {}  # npc -> why it may not lead (None: it may), as of its turn
        link.listeners.append(self._on_message)

    def _on_message(self, msg: GameMessage) -> None:
        if isinstance(msg, Loading):  # the game lets every body go with the world
            for task in self._tasks.values():
                task.cancel()
        if isinstance(msg, Loading | Session):
            self._routines.clear()

    # --- TurnExtension -----------------------------------------------------------------------------

    all_tools: tuple[Tool, ...] = ()

    async def prepare(self, npc: str) -> None:
        npc = npc.upper()
        self._may[npc] = None if self.book is None else (await self._free(npc))[0]

    def situation(self, npc: str) -> str | None:
        npc = npc.upper()
        lead = self.leads.get(npc)
        if lead is None:
            if self.book is None or npc not in self._may or self._may[npc] is not None:
                return None
            if self.meet_too:
                return (
                    "Можешь, если захочешь, отвести героя к месту (lead) или назначить ему встречу (meet): "
                    "это условия сделки, даром или за плату. Где место и как туда пройти — where."
                )
            return (
                "Можешь отвести героя к месту, если захочешь: условие lead в сделке, даром или за плату. "
                "Где место и как туда пройти — where."
            )
        if npc == "BDT_1014_BANDIT_L" and lead.way.place.id == "CASTLEMINE_HUT":
            return "Ты ведёшь героя к Декстеру за награду; " + (
                "он отстал: ты ждёшь его." if lead.state == "waiting" else "он идёт за тобой."
            )
        if lead.state == "waiting":
            return f"Ты ведёшь героя — {lead.way.title()}, но он отстал: ты ждёшь его."
        return f"Ты ведёшь героя — {lead.way.title()}; он идёт за тобой."

    def tools(self, npc: str) -> list[Tool]:
        return []

    async def handle(
        self, dialogue: DialogueManager, conv: Conversation, call: ToolCall, snap: Snapshot | None
    ) -> ToolOutcome | None:
        return None

    # --- where to ----------------------------------------------------------------------------------

    async def way(
        self, npc: str, what: str, snap: Snapshot | None, *, meet: bool = False
    ) -> tuple[Way | None, str]:
        """Where the npc would lead the hero for a deal's `lead` (`what`: a place's id or name), or wait for
        him for its `meet` (meetings.py: there it may be already, and it needs a waypoint to stand at); or why
        not (Russian, for the model)."""
        npc, what = npc.upper(), what.strip()
        if self.book is None:
            return None, "ты не знаешь здешних мест"
        if npc in self.leads and not meet:
            return None, "ты уже ведёшь героя"
        why, entries = await self._free(npc)
        if why is not None:
            return None, why
        here = snap.npc.pos if snap is not None else npc_pos(npc, self.link, self.book, None)
        place = self.book.get(what) if what else None
        if place is None and what and self.whereabouts is not None:
            place = self.whereabouts.named(what, here)
        if place is None or place.center is None or not place.points:
            return None, f"ты не знаешь такого места: «{what}». Найди его через where и назови по id"
        own = {p.id for e in entries if (p := self.book.of(e.waypoint)) is not None}
        # Dexter's outdoor hut cluster has no entrance in the book; Brago knows this way himself.
        dexter = npc == "BDT_1014_BANDIT_L" and place.id == "CASTLEMINE_HUT" and not meet
        if place.kind == "house" and place.id not in own and not dexter:
            door = self.book.way_in(place, here)
            if door is None:
                return None, f"{place.ru} — чужой дом, а где в него вход, ты не знаешь"
            way = Way(place, (door.outside,), door.pos, door=True)
        else:
            way = Way(place, place.waynet, place.center)
        if meet and not way.points:
            return None, f"{place.ru}: там негде встать и ждать. Выбери место у дороги или у дома"
        if here is not None:
            far = math.dist(here, way.pos)
            if far > LEAD_CM:
                go = "пойдёшь" if meet else "поведёшь"
                return None, f"{place.ru} — слишком далеко отсюда ({round(far / 100)} м), туда ты не {go}"
            at = self.book.of(snap.npc.nearest_waypoint) if snap is not None else None
            if not meet and (far <= ARRIVE_CM or (not way.door and at is not None and at.id == place.id)):
                return None, f"вы и так здесь: {place.where_ru}"
        return way, ""

    async def free(self, npc: str) -> str | None:
        """Why the npc may not leave its spot (None: it may): for a lead, and for an errand (errands.py)."""
        return (await self._free(npc.upper()))[0]

    async def _free(self, npc: str) -> tuple[str | None, list[RoutineEntry]]:
        """Why the npc may not leave its spot to lead the hero (None: it may), and its routine."""
        if self.companion(npc):
            return "ты и так идёшь с героем: пойди первым (go_to), он пойдёт за тобой", []
        entry = self.talk.people.get(npc)
        if (why := self.guard.body(npc, entry.state if entry is not None else "")) is not None:
            return f"тебе нельзя уходить отсюда ({why})", []
        entries = await self._routine(npc)
        if any(e.state.upper().startswith(ESCORT_STATES) for e in entries):
            return "тебе нельзя уходить: по распорядку ты уже ведёшь или сопровождаешь героя", entries
        return None, entries

    async def _routine(self, npc: str) -> list[RoutineEntry]:
        cached = self._routines.get(npc)
        if cached is not None and time.monotonic() - cached[0] < ROUTINE_S:
            return cached[1]
        try:
            entries = (await self.link.routine(npc)).entries
        except (GameError, TimeoutError) as err:
            log.info("%s: no routine for a lead: %s", npc, str(err) or type(err).__name__)
            return []
        self._routines[npc] = (time.monotonic(), entries)
        return entries

    # --- the way -----------------------------------------------------------------------------------

    def start(self, npc: str, name: str, way: Way, deal: str) -> None:
        """The hero said yes to a deal with `lead`: the npc tells him to come along, and leads him."""
        npc = npc.upper()
        conv = self.dialogue.open(npc, name)
        lead = self.leads[npc] = Lead(npc, name, way, deal)
        log.info("%s leads the hero to %s%s", npc, way.place.id, " (its way in)" if way.door else "")
        self.journal.add("lead", npc=npc, name=name, place=way.place.id, door=way.door, deal=deal)
        turn = self.dialogue.event_turn(
            npc,
            f"[Герой согласился, сделка состоялась: ты ведёшь его — {way.title()}. Скажи коротко, чтобы шёл "
            "за тобой.]",
            tools=(),
            kind="lead",
        )
        task = asyncio.get_running_loop().create_task(self._run(conv, lead, turn))
        self._tasks[npc] = task
        conv.actions.add(task)  # a commitment under way: no gesture cuts it off (dialogue.body_busy)
        task.add_done_callback(conv.actions.discard)
        task.add_done_callback(lambda t: self._tasks.pop(npc, None) if self._tasks.get(npc) is t else None)

    async def _run(self, conv: Conversation, lead: Lead, turn: asyncio.Task[None] | None) -> None:
        npc = lead.npc
        try:
            # it says «за мной» first, then sets off
            if turn is None:
                turn = conv.reply
            if turn is not None and not turn.done():
                await asyncio.wait({turn})
            if conv.voice is not None:
                await conv.voice.join()
            if not await self.talk.hold(npc, LEAD):
                await self._over(lead, "taken", "Ты не смог пойти: тебя не отпускают")
                return
            self.talk.stop_listening(npc)  # it walks ahead: what it answers him is said on the move
            how = await self._walk(lead)
            if how == "arrived":
                how = await self._there(lead)
            await self._over(lead, how)
        finally:
            if self.leads.get(npc) is lead:
                del self.leads[npc]
            self.talk.free(npc, LEAD)

    async def _walk(self, lead: Lead) -> How:
        """The way with the hero after it, waiting for him as he falls behind: how it ended."""
        deadline = time.monotonic() + WAY_S
        unstuck = 0
        while time.monotonic() < deadline:
            lead.state, lead.stopping = "going", False
            go = asyncio.ensure_future(self._go(lead))
            behind = asyncio.ensure_future(self._until(lambda: self._behind(lead), deadline))
            stuck = asyncio.ensure_future(self._stuck(lead))
            await asyncio.wait({go, behind, stuck}, return_when=asyncio.FIRST_COMPLETED)
            got_stuck = stuck.done() and not go.done()
            if not go.done():
                lead.stopping = True
                if got_stuck:  # back onto the waynet where it is, then the way again from there
                    unstuck += 1
                    await self._back_on_way(lead)
                else:  # he fell behind (or the time is up): it stops and turns to him
                    await self._act(lead.npc, "turn_to", npc="PLAYER")
            behind.cancel()
            stuck.cancel()
            status = await go
            hero = self._hero(lead)
            log.info(
                "%s on the way to %s: %s%s, %s", lead.npc, lead.way.place.id, status, " (stuck)" * got_stuck,
                f"the hero {hero[0] / 100:.0f} m off, it {hero[2] / 100:.0f} m from there" if hero else "",
            )  # fmt: skip
            if status == "done":
                return "arrived"
            if status == "timeout":
                continue  # a long way: the next leg
            if status != "cancelled":
                return "failed"
            if not lead.stopping:
                return "taken"  # something else took its body: a fight, an order, the console
            if got_stuck:
                if unstuck > UNSTICK:
                    return "failed"
                continue
            if time.monotonic() >= deadline or not await self._wait(
                lead, min(deadline, time.monotonic() + WAIT_S)
            ):
                return "lost"
        return "lost"

    async def _stuck(self, lead: Lead) -> None:
        """Done once the npc has not moved for STUCK_S on its way (a fence, a cart, the hero in the way).
        Not while its line is heard: the game starts its walk after it (a live «за мной» takes seconds)."""
        last: tuple[tuple[float, float, float], float] | None = None
        while True:
            await asyncio.sleep(EVERY_S)
            snap = self.link.snapshots.get(lead.npc)
            if snap is None:
                continue
            now = time.monotonic()
            if last is None or math.dist(snap.npc.pos, last[0]) > STUCK_CM or self.dialogue.audible(lead.npc):
                last = (snap.npc.pos, now)
            elif now - last[1] >= STUCK_S:
                return

    async def _back_on_way(self, lead: Lead) -> None:
        """To the waypoint nearest to where it stands: the game plans the way anew from there."""
        snap = self.link.snapshots.get(lead.npc)
        if snap is None:
            return
        log.info("%s is stuck on its way to %s at %s", lead.npc, lead.way.place.id, snap.npc.pos)
        await self._act(lead.npc, "go_to_waypoint", waypoint="", pos=list(snap.npc.pos), radius=200)

    async def _go(self, lead: Lead) -> str:
        way = lead.way
        try:
            res = await self.link.act(
                lead.npc,
                "go_to_waypoint",
                waypoint=way.points[0] if way.points else "",
                points=list(way.points),
                pos=list(way.pos),
                radius=ARRIVE_CM,
                pace="hero",  # it runs while he runs (v38): walking, it would be left behind by him
                timeout_ms=LEG_MS,
                timeout=LEG_MS / 1000 + 30,  # its lines first: the game starts the clock after them
            )
        except (GameError, TimeoutError) as err:
            return f"error: {str(err) or type(err).__name__}"
        return res.status

    async def _wait(self, lead: Lead, until: float) -> bool:
        """It waits for the hero, calling him the first few times; whether he came."""
        lead.state = "waiting"
        hero = self._hero(lead)
        if lead.calls < CALLS and hero is not None:
            lead.calls += 1
            self.dialogue.event_turn(
                lead.npc,
                f"[Герой отстал: он в {round(hero[0] / 100)} м позади. Позови его — пусть идёт за тобой.]",
                tools=(),
                kind="lead",
            )
        return await self._until(lambda: self._came(lead), until)

    async def _there(self, lead: Lead) -> How:
        """It is there: it waits for the hero, if he is not with it, and turns to him."""
        if not self._came(lead) and not await self._wait(lead, time.monotonic() + WAIT_S):
            return "lost"
        await self._act(lead.npc, "turn_to", npc="PLAYER")
        return "arrived"

    async def _over(self, lead: Lead, how: How, why: str = "") -> None:
        """The way is over: a fact, what the npc remembers of it, its words."""
        npc, way = lead.npc, lead.way
        snap = self.link.snapshots.get(npc)
        when = {"day": snap.world.day, "hour": snap.world.hour, "minute": snap.world.minute} if snap else {}
        self.chronicle.record(
            {"fact": "led", "npc": npc, "name": lead.name, "place": way.place.id, "door": way.door,
             "deal": lead.deal, "how": how, **when}
        )  # fmt: skip
        self.journal.add("led", npc=npc, name=lead.name, place=way.place.id, how=how)
        log.info("%s led the hero to %s: %s", npc, way.place.id, how)
        match how:
            case "arrived":
                if self.talk.partner is not None and self.talk.partner.npc == npc:
                    await self.talk.listen(npc)  # it stands by him while the conversation lasts
                self.dialogue.note(npc, f"Ты привёл героя, куда он просил: {way.where_ru()}")
                turn = self.dialogue.event_turn(
                    npc, f"[Вы пришли: {way.where_ru()}. Скажи герою, что пришли.]", kind="lead"
                )
                await self._said(npc, turn)
            case "lost":
                self.dialogue.note(
                    npc, f"Ты вёл героя ({way.title()}), но он отстал и не пришёл; ты вернулся к своим делам"
                )
            case "failed":
                self.dialogue.note(npc, f"Ты не смог дойти с героем ({way.title()}): дорогу не пройти")
                turn = self.dialogue.event_turn(
                    npc, "[Ты не смог довести героя: дорогу туда не пройти. Скажи ему.]", kind="lead"
                )
                await self._said(npc, turn)
            case "taken":
                self.dialogue.note(npc, why or f"Тебя отвлекли, и ты не довёл героя ({way.title()})")

    async def _said(self, npc: str, turn: asyncio.Task[None] | None) -> None:
        """Its words are said before its body goes back to its business."""
        if turn is not None:
            await asyncio.wait({turn})
        conv = self.dialogue.conversations.get(npc)
        if conv is not None and conv.voice is not None:
            await conv.voice.join()

    # --- helpers -----------------------------------------------------------------------------------

    def _hero(self, lead: Lead) -> tuple[float, float, float] | None:
        """How far the hero is from the npc, how far each of them is from where it leads him."""
        snap = self.link.snapshots.get(lead.npc)
        if snap is None or snap.player is None:
            return None
        to = lead.way.pos
        return snap.player.distance, math.dist(snap.player.pos, to), math.dist(snap.npc.pos, to)

    def _behind(self, lead: Lead) -> bool:
        hero = self._hero(lead)
        return hero is not None and hero[0] > LAG_CM and hero[1] > hero[2]

    def _came(self, lead: Lead) -> bool:
        """He is with it again: close, or ahead of it on the way."""
        hero = self._hero(lead)
        return hero is not None and (hero[0] <= CLOSE_CM or hero[1] <= hero[2])

    async def _until(self, cond: Callable[[], bool], until: float) -> bool:
        while not cond():
            if time.monotonic() >= until:
                return False
            await asyncio.sleep(EVERY_S)
        return True

    async def _act(self, npc: str, action: Any, /, **args: Any) -> str:
        try:
            # its line first, if it is saying one (the game starts the action after it)
            return (await self.link.act(npc, action, timeout=30, **args)).status
        except (GameError, TimeoutError) as err:
            log.info("%s %s: %s", npc, action, str(err) or type(err).__name__)
            return "failed"
