"""Errands of the npc's (docs/npc-tools.md, wave 2 step 5: `plan`).

The conversational model sets the npc a goal with `plan`: to go and tell someone what it knows of the hero,
or to take someone a thing, now or at a game hour (when it sets off). The code makes the steps from the
world:
- where the other is then: the spot of its routine at that minute (social.py), on the waynet;
- near the hero (he is by the npc when it sets off, or by where it goes) the body walks for real: an
  activity at that spot (`start_activity` with `arrive`), it looks for the one it came to (`story_nearby`),
  tells it (rumors, `how: told`; in front of the hero a scene of two voices, gossip.py) or gives it the
  thing (`npc_act give`), and its routine goes on;
- far from the hero nothing walks: when it would be there, the outcome is a fact at once (a rumor, or
  `story_effect give` to the npc, v39), no model call;
- the fork: the other is not there — for a telling, another of its guild there hears it instead (the code
  decides; the quick model is not asked yet); nobody — it waits there a while, then gives up with a note.
Goals are facts of the chronicle (`goal`, `goal_done`, `goal_failed`; a thing given: `gave`): a load brings
them back. Never given: a mission item (the bridge marks it, v39), a thing the story graph checks some npc
has (but gold), and the plot npc's things (they take no errands at all).
"""

from __future__ import annotations

import asyncio
import logging
import math
import uuid
from collections import defaultdict
from collections.abc import Callable, Coroutine
from dataclasses import dataclass
from typing import TYPE_CHECKING, Any, Literal

from .bodies import Bodies
from .brain.planner import TAKES_BODY, NpcPlanner, NpcRecord
from .chronicle import Chronicle, Fact
from .concerns import RANK, Concern
from .game import GameError, GameLink
from .gamedata.storygraph import StoryGraph, walk
from .guide import Guide
from .journal import Journal
from .llm import Tool, ToolCall
from .meetings import DAY, MAX_DAYS, WALK_M_PER_MIN, WAY_LONGER, clock
from .protocol import GameMessage, Loading, Session, Snapshot
from .social import Rumor, Rumors, Social
from .turn import ToolOutcome

if TYPE_CHECKING:
    from .dialogue import Conversation, DialogueManager
    from .talk import TalkDesk

log = logging.getLogger(__name__)

PLAN = "plan"
NEAR_CM = 6000.0  # the hero this near the npc, or where it goes: its body walks there for real
FIND_CM = 1500  # there, it looks for the one it came to this near
WAIT_MIN = 20  # ...and waits for it there so many game minutes at most
TICK_S = 5.0  # how often the game's clock is read while a goal is open
RETRY_MIN = 5  # a walk cut off is given again so many game minutes later...
TRIES = 3  # ...so many times at most
WALK_S = 900.0  # the walk there at most, real seconds
GIVE_S = 120.0  # a give near: the walk up and the hand-over, real seconds
TELL_TRIES = 10  # a telling the bodies are busy for (a scene, a talk): asked again...
TELL_EVERY_S = 1.5  # ...so often
WAIT_STATE = "ZS_STAND_WP"  # how it stands there while it looks for the other
FREE_ITEMS = {"ITMI_GOLD"}  # money: the graph checks the gold of 74 dialogs' npc's, no plot thing in it
OPEN = ("agreed", "leaving", "going", "away", "there")

State = Literal["agreed", "leaving", "going", "away", "there", "done", "failed"]
Do = Literal["tell", "give"]

PLAN_TOOL = Tool(
    PLAN,
    "Поручение самому себе: сходить к человеку и рассказать ему, что ты знаешь о герое (tell), или отнести "
    "ему вещь из своего инвентаря (give) — сейчас или в назначенный час. Где его искать, ты знаешь сам: где "
    "он будет в это время по распорядку. Сначала скажи вслух, что сделаешь. Вещи для задания и то, что нужно "
    "по сюжету, не отдают.",
    {
        "type": "object",
        "properties": {
            "do": {"type": "string", "enum": ["tell", "give"], "description": "рассказать или отнести"},
            "who": {"type": "string", "description": "к кому: имя, как его зовут, или id"},
            "item": {"type": "string", "description": "give: id вещи из твоего инвентаря; tell: пусто"},
            "count": {"type": "integer", "description": "give: сколько; tell: 0"},
            "day": {
                "type": "integer",
                "description": "когда выйти: день игры, как в «Время: день N»; 0 — сейчас",
            },
            "hour": {"type": "integer", "description": "час, 0–23 (при day 0 не важен)"},
            "minute": {"type": "integer", "description": "минута, 0–59"},
        },
        "required": ["do", "who", "item", "count", "day", "hour", "minute"],
        "additionalProperties": False,
    },
)


@dataclass
class Goal:
    id: str
    npc: str
    name: str
    do: Do
    to: str
    to_name: str
    start: int  # the game minute it sets off (minutes from day 0, 00:00)
    rumor: str = ""  # tell: the rumor's key (social.py)
    title: str = ""  # tell: what it is about, for its situation
    item: str = ""  # give
    item_name: str = ""
    count: int = 0
    state: State = "agreed"
    arrive: int = 0  # away (far from the hero): the game minute it would be there
    retry: int = 0  # a walk cut off: not again before this game minute
    tries: int = 0
    subject: str = "PLAYER"  # tell: omitted in old goal facts about the hero

    def what(self) -> str:
        if self.do == "tell":
            news = "о герое" if self.subject == "PLAYER" else "новость"
            return f"рассказать {news} ({self.title}) — кому: {self.to_name}"
        return f"отнести {self.item_name}, {self.count} шт. — кому: {self.to_name}"

    def when(self, now: int | None) -> str:
        if now is not None and self.start <= now + 1:
            return "сейчас"
        return f"в день {self.start // DAY + 1}, {clock(self.start)}"

    def fact(self) -> Fact:
        return {"fact": "goal", "id": self.id, "npc": self.npc, "name": self.name, "do": self.do,
                "to": self.to, "to_name": self.to_name, "start": self.start, "rumor": self.rumor,
                "title": self.title, "item": self.item, "item_name": self.item_name,
                "count": self.count, "subject": self.subject}  # fmt: skip


@dataclass(frozen=True)
class Spot:
    """Where it goes: a waypoint on the waynet, and the other's spot there."""

    waypoint: str
    pos: tuple[float, float, float]


def checked_items(graph: StoryGraph | None) -> dict[str, set[str]]:
    """item -> the npc's the story graph checks have it (Npc_HasItems on an npc, not the hero): a thing that
    must stay where it is. Gold is no such thing (FREE_ITEMS)."""
    out: dict[str, set[str]] = defaultdict(set)
    if graph is None:
        return out
    for n in graph.nodes.values():
        roots = [n.condition, *(e.get("when") for e in [*n.effects, *n.condition_effects])]
        for root in roots:
            for e in walk(root):
                if len(e) < 4 or e[0] != "call" or str(e[1]).upper() != "NPC_HASITEMS":
                    continue
                who, item = e[2], e[3]
                if not isinstance(who, list) or who[:1] != ["inst"] or who[1] in ("OTHER", "HERO"):
                    continue
                if not isinstance(item, list) or item[:1] != ["sym"]:
                    continue
                npc = n.npc if who[1] == "SELF" else graph.npc(str(who[1]))
                out[str(item[1]).upper()].add(npc.upper())
    for item in FREE_ITEMS:
        out.pop(item, None)
    return out


class Errands:
    """The npc's errands (a TurnExtension: `plan`, and what it has set out to do; the planner's hook: its body
    is the errand's on the way)."""

    def __init__(
        self,
        link: GameLink,
        dialogue: DialogueManager,
        talk: TalkDesk,
        guide: Guide,
        chronicle: Chronicle,
        bodies: Bodies,
        *,
        rumors: Callable[[], Rumors | None],
        graph: StoryGraph | None = None,
        journal: Journal | None = None,
    ) -> None:
        self.link = link
        self.dialogue = dialogue
        self.talk = talk
        self.guide = guide
        self.chronicle = chronicle
        self.bodies = bodies
        self.rumors = rumors
        self.gate_news: Callable[[str, str], tuple[str, str] | None] | None = None
        self.gate_told: Callable[[str, str, str], Coroutine[Any, Any, None]] | None = None
        self.checked = checked_items(graph)
        self.journal = journal or dialogue.journal
        self.goals: dict[str, Goal] = {}
        self.now: int | None = None  # the game minute, as last read
        self._may: dict[str, str | None] = {}  # npc -> why it may not go (None: it may), as of its turn
        self._world = 0
        self._tasks: set[asyncio.Task[Any]] = set()
        self._next_allow: Callable[[NpcRecord, str], str | None] | None = None
        link.listeners.append(self._on_message)
        self._load()

    def social(self) -> Social | None:
        rumors = self.rumors()
        return rumors.social if rumors is not None else None

    def attach_planner(self, planner: NpcPlanner) -> None:
        """The planner gives the body of an npc on its way, or looking for the other there, nothing else."""
        self._next_allow, planner.allow = planner.allow, self.allow

    def allow(self, rec: NpcRecord, choice: str) -> str | None:
        if choice not in TAKES_BODY and any(
            g.npc == rec.id and g.state in ("going", "there") for g in self.goals.values()
        ):
            return "идёт по поручению"
        return self._next_allow(rec, choice) if self._next_allow is not None else None

    def _on_message(self, msg: GameMessage) -> None:
        match msg:
            case Loading():
                self._world += 1  # its walk went with the world: given again
                self.now = None
                for g in self.goals.values():
                    if g.state in ("leaving", "going", "there"):
                        g.state, g.retry = "agreed", 0
            case Session():
                self._load()

    def _load(self) -> None:
        """The goals of the save branch, from its facts."""
        self.goals.clear()
        for f in self.chronicle.facts():
            match f.get("fact"):
                case "goal":
                    g = Goal(
                        str(f["id"]), str(f["npc"]), str(f.get("name", "")), f.get("do", "tell"),
                        str(f["to"]), str(f.get("to_name", "")), int(f["start"]), str(f.get("rumor", "")),
                        str(f.get("title", "")), str(f.get("item", "")), str(f.get("item_name", "")),
                        int(f.get("count", 0)), subject=str(f.get("subject") or "PLAYER").upper(),
                    )  # fmt: skip
                    self.goals[g.id] = g
                case "goal_done" if (g := self.goals.get(f.get("goal", ""))) is not None:
                    g.state = "done"
                case "goal_failed" if (g := self.goals.get(f.get("goal", ""))) is not None:
                    g.state = "failed"

    # --- the tool -------------------------------------------------------------------------------

    all_tools: tuple[Tool, ...] = (PLAN_TOOL,)

    async def prepare(self, npc: str) -> None:
        npc = npc.upper()
        self._may[npc] = await self.guide.free(npc)

    def tools(self, npc: str) -> list[Tool]:
        npc = npc.upper()
        return (
            [PLAN_TOOL] if npc in self._may and self._may[npc] is None and self.social() is not None else []
        )

    def situation(self, npc: str) -> str | None:
        npc = npc.upper()
        lines = []
        for g in self.goals.values():
            if g.npc != npc:
                continue
            match g.state:
                case "agreed" | "away":
                    lines.append(f"Ты собрался {g.when(self.now)}: {g.what()}.")
                case "leaving" | "going" | "there":
                    lines.append(f"Ты идёшь по поручению: {g.what()}.")
        if not lines and self.tools(npc):
            lines.append(
                "Можешь сам сходить к человеку: рассказать ему о герое или отнести вещь, сейчас или в "
                "назначенный час (plan)."
            )
        return "\n".join(lines) or None

    async def handle(
        self, dialogue: DialogueManager, conv: Conversation, call: ToolCall, snap: Snapshot | None
    ) -> ToolOutcome | None:
        if call.name != PLAN:
            return None
        npc = conv.npc.upper()
        goal, why = await self.plan(npc, conv.persona.name, call.arguments, snap)
        if goal is None:
            return ToolOutcome(f"Не выйдет: {why}.", "rejected")
        return ToolOutcome(f"Принято: {goal.when(self.now)} ты пойдёшь {goal.what()}.", "accepted")

    async def plan(self, npc: str, name: str, raw: Any, snap: Snapshot | None) -> tuple[Goal | None, str]:
        """The goal of a `plan` call, recorded, or None with why not (Russian, for the model)."""
        raw = raw if isinstance(raw, dict) else {}
        npc = npc.upper()
        why = self._may[npc] if npc in self._may else await self.guide.free(npc)
        if why:
            return None, why
        social = self.social()
        if social is None:
            return None, "ты не знаешь, где кого искать"
        do = str(raw.get("do") or "")
        if do not in ("tell", "give"):
            return None, "do — tell (рассказать) или give (отнести)"
        to, why = self._who(npc, str(raw.get("who") or ""), social)
        if to is None:
            return None, why
        now = _now(snap) if snap is not None else self.now
        if now is None:
            return None, "ты не знаешь, который час"
        start, why = _start(raw, now)
        if start is None:
            return None, why
        if (
            busy := next((g for g in self.goals.values() if g.npc == npc and g.state in OPEN), None)
        ) is not None:
            return None, f"у тебя уже есть поручение: {busy.what()}"
        if self._spot(to, start, social) is None:
            return None, f"ты не знаешь, где в это время будет {social.names.get(to, to)}"
        goal = Goal(
            f"goal-{uuid.uuid4().hex[:6]}",
            npc,
            name,
            do,
            to,
            social.names.get(to, to),
            start,  # type: ignore[arg-type]
        )
        if do == "tell":
            news = self.gate_news(npc, to) if self.gate_news is not None else None
            if news is not None:
                goal.rumor, goal.title = news
            else:
                rumor = self._rumor(npc, to)
                if rumor is None:
                    return None, f"{goal.to_name} уже знает все новости, которые ты можешь рассказать"
                goal.rumor, goal.subject = rumor.key, rumor.subject
                goal.title = rumor.title if rumor.is_player else rumor.description()
        else:
            why = self._thing(goal, str(raw.get("item") or ""), raw.get("count"), snap)
            if why:
                return None, why
        self.goals[goal.id] = goal
        self.chronicle.record(goal.fact())
        self.journal.add("goal", npc=npc, name=name, goal=goal.id, what=goal.what(), start=start)
        log.info("goal %s of %s: %s, %s", goal.id, npc, goal.what(), goal.when(now))
        return goal, ""

    def _who(self, npc: str, raw: str, social: Social) -> tuple[str | None, str]:
        """The npc it means, by id or by name (its acquaintances first)."""
        raw = raw.strip()
        if not raw:
            return None, "к кому — имя или id"
        if raw.upper() in social.names:
            who = raw.upper()
        else:
            low = raw.lower()
            named = [p for p, n in social.names.items() if n.lower() == low] or [
                p for p, n in social.names.items() if low in n.lower() or n.lower() in low
            ]
            named = [p for p in named if p != npc]
            known = [p for p in named if p in social.ties.get(npc, {})]
            pick = known or named
            if not pick:
                return None, f"ты не знаешь, кто такой «{raw}»"
            if len(pick) > 1:
                return None, f"«{raw}» — таких несколько, назови по имени или id"
            who = pick[0]
        if who == npc:
            return None, "это ты сам"
        rumors = self.rumors()
        if rumors is not None and who in rumors.dead:
            return None, f"{social.names.get(who, who)} мёртв"
        return who, ""

    def _rumor(self, npc: str, to: str) -> Rumor | None:
        """News it can pass to the other: its own discovery first, then the freshest."""
        rumors = self.rumors()
        if rumors is None:
            return None
        known = [
            r for r in rumors.rumors.values()
            if npc in r.known and to not in r.known and npc not in r.silent and r.subject != npc
        ]
        known.sort(key=lambda r: (r.about != npc, -r.since.get(npc, 0)))
        return known[0] if known else None

    def _thing(self, goal: Goal, item: str, count: Any, snap: Snapshot | None) -> str:
        """Why it may not give the thing (empty: it may), and the goal's item set."""
        item = item.strip().upper()
        try:
            n = int(count or 1)
        except (TypeError, ValueError):
            return "count — число"
        if not item or n <= 0:
            return "что отнести — id вещи и сколько"
        have = next((i for i in snap.npc.inventory if i.item.upper() == item), None) if snap else None
        if have is None:
            return f"у тебя нет {item}"
        if have.count < n:
            return f"у тебя только {have.count} шт.: {have.name}"
        if have.mission:
            return f"{have.name} — вещь для задания: такие не отдают"
        holders = self.checked.get(item, set())
        if goal.npc in holders or goal.to in holders:
            who = goal.name if goal.npc in holders else goal.to_name
            return f"{have.name} по сюжету должна быть у {who}: её не отдают"
        goal.item, goal.item_name, goal.count = item, have.name, n
        return ""

    def _spot(self, npc: str, minute: int, social: Social | None = None) -> Spot | None:
        """Where the npc is at that minute by its routine: its spot, on the waynet (a freepoint's place's
        anchor: the game walks to a freepoint straight, into fences, guide.py)."""
        social = social or self.social()
        spot = social.spot(npc, minute) if social is not None else None
        if spot is None or spot.pos is None:
            return None
        waypoint = spot.waypoint
        if waypoint.upper().startswith("FP_"):
            book = self.guide.book
            place = book.of(waypoint) if book is not None else None
            if place is None or place.anchor is None:
                return None
            waypoint = place.anchor
        return Spot(waypoint, spot.pos)

    # --- the errands ----------------------------------------------------------------------------

    async def run(self) -> None:
        """The game's clock, read while a goal is open."""
        while True:
            await asyncio.sleep(TICK_S)
            if not any(g.state in OPEN for g in self.goals.values()):  # "there" waits by the clock too
                continue
            await self.link.wait_world()
            try:
                state = await self.link.story_state()
            except (GameError, TimeoutError) as err:
                log.info("errands: %s", str(err) or type(err).__name__)
                continue
            if all(k in state for k in ("day", "hour", "minute")):
                self.tick(int(state["day"]) * DAY + int(state["hour"]) * 60 + int(state["minute"]))

    def tick(self, now: int) -> None:
        """What comes of each goal at `now` (a game minute)."""
        self.now = now
        for g in list(self.goals.values()):
            if g.state == "agreed" and now >= max(g.start, g.retry):
                g.state = "leaving"
                self._spawn(self._set_off(g, now, self._world))
            elif g.state == "away" and now >= g.arrive:
                self._far(g)

    async def _set_off(self, g: Goal, now: int, world: int) -> None:
        """It sets off: in front of the hero for real, else only in the world's reckoning. Where the hero is
        now: a fresh snapshot (the cached one may be from before he went off, a teleport, a load)."""
        hero, here = await self._now_where(g.npc, now)
        if world != self._world or g.state != "leaving":
            return
        there = self._spot(g.to, now)
        if there is None or here is None:
            return self._fail(g, f"не знал, где {g.to_name}")
        walk = _walk_min(here, there.pos)
        there = self._spot(g.to, now + walk) or there
        near = hero is not None and (
            math.dist(hero, here) <= NEAR_CM or math.dist(hero, there.pos) <= NEAR_CM
        )
        if near:
            g.state = "going"
            log.info("goal %s: %s sets off to %s (%s), %d min", g.id, g.npc, g.to, there.waypoint, walk)
            self.journal.add("goal_going", npc=g.npc, goal=g.id, to=g.to, waypoint=there.waypoint)
            self._spawn(self._walk(g, there, walk, self._world))
        else:
            g.state, g.arrive = "away", now + walk
            log.info(
                "goal %s: %s goes to %s far from the hero, there at %s", g.id, g.npc, g.to, clock(g.arrive)
            )

    async def _now_where(
        self, npc: str, now: int
    ) -> tuple[tuple[float, float, float] | None, tuple[float, float, float] | None]:
        """The hero and the npc, where they are now."""
        try:
            snap = await self.link.snapshot(npc, fresh=True)
            return (snap.player.pos if snap.player is not None else None), snap.npc.pos
        except (GameError, TimeoutError) as err:
            log.info("errands: no snapshot of %s: %s", npc, str(err) or type(err).__name__)
        cached = self.link.snapshots.get(npc)
        social = self.social()
        spot = social.spot(npc, now) if social is not None else None
        here = cached.npc.pos if cached is not None else spot.pos if spot is not None else None
        hero = self.link.player
        return (hero.pos if hero is not None else None), here

    async def _walk(self, g: Goal, there: Spot, walk: int, world: int) -> None:
        """There for real, and what it came for."""
        if not (g.do == "give" and await self._by(g.npc, g.to)):  # a give to one right by it: no walk first
            out = await self.bodies.act(
                g.npc,
                "start_activity",
                state=WAIT_STATE,
                waypoint=there.waypoint,
                minutes=walk + WAIT_MIN,
                arrive=True,
                radius=500,
                timeout=WALK_S,
            )
            if world != self._world or g.state != "going":
                return  # a load, or it is over meanwhile
            if out.status != "arrived":
                return self._cut_off(g, out.status, out.detail)
        g.state = "there"
        await self._find(g, world)

    def _cut_off(self, g: Goal, status: str, detail: str) -> None:
        if not (status == "rejected" and "busy" in detail):  # held for a talk: not a try, only later
            g.tries += 1
        log.info("goal %s: %s's walk %s (%s), try %d", g.id, g.npc, status, detail, g.tries)
        if g.tries >= TRIES:
            return self._fail(g, f"так и не дошёл туда, где {g.to_name}")
        g.state, g.retry = "agreed", (self.now or g.start) + RETRY_MIN

    async def _by(self, npc: str, other: str) -> bool:
        try:
            return any(p.get("npc") == other for p in await self.link.story_nearby(npc, FIND_CM))
        except (GameError, TimeoutError):
            return False

    async def _find(self, g: Goal, world: int) -> None:
        """It looks for the other there. The fork: another of its guild hears it, or it waits, then gives
        up."""
        social = self.social()
        until = (self.now or g.start) + WAIT_MIN
        while world == self._world and g.state == "there":
            try:
                people = await self.link.story_nearby(g.npc, FIND_CM)
            except (GameError, TimeoutError) as err:
                log.info("goal %s: %s", g.id, str(err) or type(err).__name__)
                people = []
            if any(p.get("npc") == g.to for p in people):
                return await self._near(g, g.to)
            if (
                g.do == "tell" and not g.rumor.startswith("gate:")
                and social is not None and (guild := social.guilds.get(g.to))
            ):
                rumor = self._of(g)
                others = sorted(
                    (p for p in people if p.get("guild") == guild and p.get("npc") not in (g.npc, g.to)
                     and (rumor is None or p.get("npc") not in rumor.known)),
                    key=lambda p: float(p.get("distance", 0)),
                )  # fmt: skip
                if others:
                    log.info("goal %s: %s is not there, %s hears it instead", g.id, g.to, others[0]["npc"])
                    return await self._near(g, str(others[0]["npc"]))
            if self.now is not None and self.now >= until:
                return self._fail(g, f"не застал на месте: {g.to_name}")
            await asyncio.sleep(TICK_S)

    def _of(self, g: Goal) -> Rumor | None:
        rumors = self.rumors()
        return rumors.rumors.get(g.rumor) if rumors is not None else None

    async def _near(self, g: Goal, to: str) -> None:
        """What it came for, in the world as it is: tell it (a scene in front of the hero), or hand it
        over."""
        if g.do == "give":
            out = await self.bodies.act(g.npc, "give", npc=to, item=g.item, count=g.count, timeout=GIVE_S)
            if out.status != "done":
                return self._fail(g, f"не отдал ({out.status}: {out.detail}) — кому: {g.to_name}")
            seen = await self._seen_by(g.npc, to)
            self._gave(g, to, seen)
            return self._done(g, to, "near")
        if g.rumor.startswith("gate:"):
            return await self._tell_gate(g, to, "near", self._world)
        rumors, rumor = self.rumors(), self._of(g)
        if rumors is None or rumor is None:
            return self._fail(g, "забыл, о чём хотел рассказать")
        for _ in range(TELL_TRIES):
            status = rumors.tell(rumor, g.npc, to)
            if status != "wait":
                break
            await asyncio.sleep(TELL_EVERY_S)
        if status == "wait":
            return self._fail(g, f"так и не улучил минуту рассказать — кому: {g.to_name}")
        self._done(g, to, "near" if status == "told" else "knew")

    def _far(self, g: Goal) -> None:
        """Far from the hero, at the minute it would be there: the outcome as a fact, no walk, no model."""
        social, rumors = self.social(), self.rumors()
        spot = social.spot(g.to, g.arrive) if social is not None else None
        there = spot is not None and "SLEEP" not in spot.state and (rumors is None or g.to not in rumors.dead)
        if g.do == "give":
            if not there:
                return self._fail(g, f"не застал на месте: {g.to_name}")
            self._spawn(self._give_far(g, self._world))
            return
        if g.rumor.startswith("gate:"):
            if not there:
                return self._fail(g, f"не застал на месте: {g.to_name}")
            g.state = "there"
            self._spawn(self._tell_gate(g, g.to, "far", self._world))
            return
        rumor = self._of(g)
        if rumors is None or rumor is None:
            return self._fail(g, "забыл, о чём хотел рассказать")
        to = g.to if there else self._stand_in(g, rumor)
        if to is None:
            return self._fail(g, f"не застал на месте: {g.to_name}")
        status = rumors.tell(rumor, g.npc, to)
        self._done(g, to, "far" if status == "told" else "knew")

    async def _tell_gate(self, g: Goal, to: str, how: str, world: int) -> None:
        """Gate news is addressed to this person: another guard cannot receive it in its place."""
        if to != g.to or self.gate_told is None:
            return self._fail(g, f"не передал новость адресату: {g.to_name}")
        try:
            await self.gate_told(g.rumor, g.npc, to)
        except (GameError, TimeoutError) as err:
            if world == self._world:
                self._fail(g, f"не передал новость: {str(err) or type(err).__name__}")
            return
        if world == self._world:
            self._done(g, to, how)

    def _stand_in(self, g: Goal, rumor: Rumor) -> str | None:
        """Another of the other's guild by its spot at that minute, by its routine, awake (the far fork)."""
        social, rumors = self.social(), self.rumors()
        there = self._spot(g.to, g.arrive)
        if social is None or there is None or not (guild := social.guilds.get(g.to)):
            return None
        best: tuple[float, str] | None = None
        for npc, spot in social.spots():
            if npc in (g.npc, g.to) or social.guilds.get(npc) != guild or npc in rumor.known:
                continue
            if rumors is not None and npc in rumors.dead:
                continue
            if spot.pos is None or "SLEEP" in spot.state or social.spot(npc, g.arrive) is not spot:
                continue
            d = math.dist(spot.pos, there.pos)
            if d <= FIND_CM and (best is None or d < best[0]):
                best = (d, npc)
        return best[1] if best is not None else None

    async def _give_far(self, g: Goal, world: int) -> None:
        effect = {"do": "give", "item": g.item, "count": g.count, "to": "npc", "npc": g.to}
        try:
            await self.link.story_effect(g.npc, [effect], check=True)
        except (GameError, TimeoutError) as err:
            if world == self._world:
                self._fail(g, f"не отдал ({str(err) or type(err).__name__}) — кому: {g.to_name}")
            return
        if world != self._world or g.state != "away":
            return
        self._gave(g, g.to, [])
        self._done(g, g.to, "far")

    async def _seen_by(self, npc: str, to: str) -> list[str]:
        try:
            people = await self.link.story_nearby(npc, FIND_CM)
        except (GameError, TimeoutError):
            people = []
        seen = [str(p["npc"]) for p in people if p.get("npc") not in (npc, to)]
        entry = self.talk.people.get(npc)
        if entry is not None and entry.distance <= FIND_CM:
            seen.append("PLAYER")
        return seen

    def _gave(self, g: Goal, to: str, seen: list[str]) -> None:
        social = self.social()
        there = self._spot(to, self.now or g.start)
        self.chronicle.record({
            "fact": "gave", "from": g.npc, "from_name": g.name, "to": to,
            "to_name": social.names.get(to, to) if social is not None else to, "item": g.item,
            "item_name": g.item_name, "count": g.count, "where": there.waypoint if there else "",
            "seen_by": seen, "goal": g.id, **self._when(),
        })  # fmt: skip
        self.journal.add("gave", npc=g.npc, to=to, item=g.item, count=g.count, goal=g.id)

    def _done(self, g: Goal, to: str, how: str) -> None:
        g.state = "done"
        social = self.social()
        to_name = social.names.get(to, to) if social is not None else to
        fact: Fact = {"fact": "goal_done", "goal": g.id, "npc": g.npc, "name": g.name, "to": to,
                      "to_name": to_name, "how": how, **self._when()}  # fmt: skip
        if to != g.to:
            fact["instead_of"] = g.to
        self.chronicle.record(fact)
        self.journal.add("goal_done", npc=g.npc, goal=g.id, to=to, how=how)
        log.info("goal %s done (%s): %s -> %s", g.id, how, g.npc, to)
        if g.do == "give":
            note = f"Ты отнёс {g.item_name}, {g.count} шт. — кому: {to_name}"
        elif how == "knew":
            news = "о герое" if g.subject == "PLAYER" else f"новость ({g.title})"
            note = f"Ты пришёл рассказать {news}, а он уже знал — кому: {to_name}"
        else:
            news = "о герое" if g.subject == "PLAYER" else "новость"
            note = f"Ты рассказал {news} ({g.title}) — кому: {to_name}"
            if to != g.to:
                note += f"; не застал: {g.to_name}"
        self.dialogue.remember(g.npc, g.name, note)

    def _fail(self, g: Goal, why: str) -> None:
        """A dead end: a note for it; a thing it was to give stays its own, and on its mind (a concern)."""
        g.state = "failed"
        self.chronicle.record(
            {"fact": "goal_failed", "goal": g.id, "npc": g.npc, "name": g.name, "why": why, **self._when()}
        )
        self.journal.add("goal_failed", npc=g.npc, goal=g.id, why=why)
        log.info("goal %s failed: %s %s", g.id, g.npc, why)
        self.dialogue.remember(g.npc, g.name, f"Поручение не вышло: {g.what()} — {why}")

    def concerns(self, npc: str) -> list[Concern]:
        """A thing it was to take someone and did not: it may tell the hero so (concerns.py). Until the day is
        over."""
        npc = npc.upper()
        today = self.now // DAY if self.now is not None else None
        out = []
        for f in self.chronicle.facts("goal_failed"):
            g = self.goals.get(f.get("goal", ""))
            if g is None or g.npc != npc or g.do != "give" or f.get("day") != today:
                continue
            ru = f"не вышло поручение: {g.what()} — {f.get('why', '')}"
            out.append(Concern(f"errand_failed:{g.id}", npc, RANK["errand"], "tell the hero your errand for "
                               "him came to nothing", ru))  # fmt: skip
        return out

    def _when(self) -> dict[str, int]:
        if self.now is None:
            return {}
        return {"day": self.now // DAY, "hour": self.now % DAY // 60, "minute": self.now % 60}

    def _spawn(self, coro: Coroutine[Any, Any, Any]) -> None:
        task = asyncio.get_running_loop().create_task(coro)
        self._tasks.add(task)
        task.add_done_callback(self._tasks.discard)


def _now(snap: Snapshot) -> int:
    w = snap.world
    return w.day * DAY + w.hour * 60 + w.minute


def _start(raw: dict[str, Any], now: int) -> tuple[int | None, str]:
    """When it sets off: day 0 now, else a game day (from 1, as in its situation) and an hour."""
    try:
        day, hour, minute = int(raw.get("day") or 0), int(raw.get("hour") or 0), int(raw.get("minute") or 0)
    except (TypeError, ValueError):
        return None, "когда — числами: день, час, минута (день 0 — сейчас)"
    if day <= 0:
        return now, ""
    if not 0 <= hour <= 23 or not 0 <= minute <= 59:
        return None, "час 0–23, минута 0–59"
    start = (day - 1) * DAY + hour * 60 + minute
    if start < now - 1:
        return None, f"это время уже прошло: сейчас день {now // DAY + 1}, {clock(now)}"
    if start > now + MAX_DAYS * DAY:
        return None, f"так далеко вперёд не загадывай: не дальше {MAX_DAYS} дней"
    return start, ""


def _walk_min(a: tuple[float, float, float], b: tuple[float, float, float]) -> int:
    """Game minutes of a walk from a to b by the waynet (about WAY_LONGER the straight line)."""
    return max(1, math.ceil(math.dist(a, b) / 100 * WAY_LONGER / WALK_M_PER_MIN))
