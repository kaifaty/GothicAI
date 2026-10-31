"""An npc comes up to the hero and speaks first (docs/npc-tools.md, wave 2 step 1: start_talk).

Whether it comes up is the behavioral model's: `approach_talk` is in its react menu, and it reads the npc's
card and what it wants of the hero (its agenda: the conversational model's, or else its most pressing
concern, concerns.py). The desk takes that choice over from the planner and:
- guards what the code guards, before anything is applied (`allow`): not while busy, asleep or out of
  earshot, not while the hero talks to another, one at a time; a plot npc or one at its post does not leave
  its spot (storyguard.py) and speaks first only when the hero is already close; once spoken about, the same
  occasion waits a game hour (a loop guard: whether to bring it up again later is its character's);
- leaves it to the game when the npc has an important dialog of the scripts waiting: the game speaks first;
- walks the body to the hero (bodies.py) and waits until it is there, and until two people talking next to
  him are done (gossip.py): it does not speak over them;
- makes it the hero's partner (talk.py): his next words without a name go to it;
- lets the conversational model say the first words on the occasion, one call within the budget, with the
  conversation's tools (a deal among them);
- writes the fact `initiative`: who, why, where and who saw it.
Without an occasion the npc only comes up, as before: no words, no budget.
"""

from __future__ import annotations

import asyncio
import hashlib
import logging
from collections.abc import Callable, Coroutine
from contextlib import AbstractAsyncContextManager, nullcontext
from dataclasses import dataclass
from typing import TYPE_CHECKING, Any

from .bodies import Bodies
from .brain.apply import Decision
from .brain.planner import TAKES_BODY, NpcPlanner, NpcRecord
from .budget import Budget
from .chronicle import Chronicle
from .concerns import Concerns
from .game import GameError, GameLink
from .incidents import hears
from .journal import Journal
from .protocol import GameMessage, Loading, NpcActResult, NpcViews, Session
from .storyguard import StoryGuard, asleep

if TYPE_CHECKING:
    from .dialogue import DialogueManager
    from .perception import PerceptionDesk
    from .talk import TalkDesk

log = logging.getLogger(__name__)

ARRIVE_CM = 400.0  # the roster distance that counts as having come up
STAY_CM = 500.0  # an npc that may not leave its spot speaks first only this close
REPEAT_GAME_MIN = 60  # one spoken initiative per occasion per game hour
WATCH_S = 0.25  # while it walks: has the hero spoken to it meanwhile
FLOOR_WAIT_S = 45.0  # come up while two talk next to the hero: it waits this long for them to finish
# the conversation's tools, at most six (docs/npc-tools.md, principle 8)
TOOLS = ("set_intent", "give_item", "recall", "end_conversation", "propose_deal", "withdraw_deal")
CHECKINFO = ["call", "NPC_CHECKINFO", ["inst", "SELF"], ["int", 1]]
IMPORTANT_WAIT_S = 12.0  # native perception gets its next polls after the soft approach
IMPORTANT_TALK_S = 60.0  # a started native conversation may still be playing its lines


@dataclass(frozen=True)
class Occasion:
    key: str
    ru: str  # what it comes up about, for the conversational model
    agenda: str | None


class ImportantApproach:
    """A rule sends a free NPC to its existing important dialog; native perceptions start the talk.

    This path needs neither a behavioral model nor a conversational turn. It never captures the NPC:
    its native routine and perceptions keep running during the soft walk.
    """

    def __init__(
        self, link: GameLink, bodies: Bodies, talk: TalkDesk, guard: StoryGuard, chronicle: Chronicle
    ) -> None:
        self.link, self.bodies, self.talk = link, bodies, talk
        self.guard, self.chronicle = guard, chronicle
        self.active: str | None = None
        self._world = 0
        self.link.listeners.append(self._on_message)

    def _on_message(self, message: GameMessage) -> None:
        if isinstance(message, (Loading, Session)):
            self._world += 1

    async def close(self) -> None:
        self._world += 1
        if self._on_message in self.link.listeners:
            self.link.listeners.remove(self._on_message)

    def _free(self, npc: str) -> bool:
        entry = self.talk.people.get(npc)
        partner = self.talk.partner
        return (
            entry is not None and entry.activity == "routine" and not asleep(entry.state)
            and not self.link.body_owned(npc)
            and (partner is None or partner.npc == npc)
        )

    async def approach(
        self, npc: str, key: str, *, valid: Callable[[], bool], told: Callable[[], bool],
        release: Callable[[], AbstractAsyncContextManager[None]] | None = None,
    ) -> bool:
        npc = npc.upper()
        world = self._world
        if self.active is not None or not valid() or not self._free(npc) or self.bodies.busy(npc):
            return False
        entry = self.talk.people[npc]
        stay = self.guard.body(npc, entry.state) is not None
        if not hears(entry.distance, entry.in_view) or (stay and entry.distance > STAY_CM):
            return False
        self.active = npc
        walk: asyncio.Task[Any] | None = None
        queued: asyncio.Future[NpcActResult] = asyncio.get_running_loop().create_future()
        action = "turn_to" if stay else "go_to_npc"
        act: str | None = None
        replaced = False

        def receipt(message: GameMessage) -> None:
            nonlocal act, replaced
            if not isinstance(message, NpcActResult) or message.npc.upper() != npc:
                return
            if act is not None and message.status == "queued" and message.id != act:
                replaced = True
            if not queued.done() and message.action in {None, action}:
                if message.status == "queued":
                    act = message.id
                queued.set_result(message)

        try:
            state = await self.link.story_state(npcs=[npc])
            if world != self._world or not valid() or not self._free(npc):
                return False
            if state.get("dialog") or state.get("cutscene"):
                return False
            dialog_distance = 0
            if release is not None:
                values = await self.link.story_eval(npc, [["var", "PERC_DIST_DIALOG"]])
                if len(values) == 1 and type(values[0]) is int and values[0] > 0:
                    dialog_distance = values[0]
                if world != self._world or not valid() or not self._free(npc):
                    return False
            self.link.listeners.append(receipt)
            walk = asyncio.create_task(self.bodies.act(npc, action))
            while not queued.done() and not walk.done() and world == self._world and valid():
                await asyncio.wait((queued, walk), timeout=WATCH_S, return_when=asyncio.FIRST_COMPLETED)
            if world != self._world or not valid() or not queued.done() or queued.result().status != "queued":
                return False
            act = queued.result().id
            while not walk.done() and world == self._world and valid():
                current = self.talk.people.get(npc)
                if replaced:
                    return False
                if current is not None and current.distance <= dialog_distance:
                    break
                await asyncio.wait((walk,), timeout=WATCH_S)
            if world != self._world or not valid() or replaced:
                return False
            if walk.done() and walk.result().status != "done":
                if walk.result().status == "gone" and told():
                    self._record_important(npc, entry.name, key, state)
                    return True  # native TALK took over an approach already under way
                return False
            # AI_GotoNpc aims for melee range and can stop against terrain before that distance.
            # Native important TALK is already possible within PERC_DIST_DIALOG, including before
            # the first step when the hero approaches. Its crime gate must not wait for melee range.
            async with release() if release is not None else nullcontext():
                loop = asyncio.get_running_loop()
                deadline = loop.time() + IMPORTANT_WAIT_S
                talk_deadline: float | None = None
                while world == self._world and valid() and not replaced:
                    if told():
                        self._record_important(npc, entry.name, key, state)
                        return True
                    current = self.talk.people.get(npc)
                    talking = current is not None and current.activity == "dialog"
                    if talking and talk_deadline is None:
                        talk_deadline = loop.time() + IMPORTANT_TALK_S
                    if talk_deadline is not None and not talking:
                        return False  # a cancelled native TALK no longer needs the crime gate open
                    if walk.done():
                        if walk.result().status not in {"done", "gone"} and not talking:
                            return False
                    if loop.time() >= (talk_deadline if talk_deadline is not None else deadline):
                        return False
                    await asyncio.sleep(WATCH_S)
            return False
        finally:
            if walk is not None:
                if not queued.done() and not walk.done() and world == self._world:
                    # Cancellation can arrive after the command was sent and before its receipt.
                    # Keep that receipt listener until we know whether there is a walk to cancel.
                    await asyncio.wait((queued, walk), timeout=2, return_when=asyncio.FIRST_COMPLETED)
                # Cancelling the coroutine alone leaves AI_GotoNPC queued in the native body.
                # Only clear our own unfinished soft walk, never a dialog, fight or a new owner.
                owned = act is not None or (queued.done() and queued.result().status == "queued")
                if (owned and not replaced and not walk.done() and world == self._world
                    and not told() and self._free(npc)):
                    try:
                        await self.link.story_eval(npc, [["call", "NPC_CLEARAIQUEUE", ["inst", "SELF"]]])
                    except (GameError, TimeoutError):
                        pass
                if not walk.done():
                    walk.cancel()
                await asyncio.gather(walk, return_exceptions=True)
            if receipt in self.link.listeners:
                self.link.listeners.remove(receipt)
            if self.active == npc:
                self.active = None

    def _record_important(self, npc: str, name: str, key: str, state: dict[str, Any]) -> None:
        self.chronicle.record({
            "fact": "initiative", "npc": npc, "name": name, "occasion": key,
            "why": "требует возместить ущерб", "seen_by": ["PLAYER"],
            **{k: state[k] for k in ("day", "hour", "minute") if k in state},
        })


class InitiativeDesk:
    """The npc's that come up to the hero on their own, one at a time."""

    def __init__(
        self,
        link: GameLink,
        dialogue: DialogueManager,
        talk: TalkDesk,
        planner: NpcPlanner,
        concerns: Concerns,
        guard: StoryGuard,
        budget: Budget,
        chronicle: Chronicle,
        *,
        bodies: Bodies,
        perception: PerceptionDesk | None = None,
        journal: Journal | None = None,
    ) -> None:
        self.link = link
        self.dialogue = dialogue
        self.talk = talk
        self.planner = planner
        self.concerns = concerns
        self.guard = guard
        self.budget = budget
        self.chronicle = chronicle
        self.bodies = bodies
        self.perception = perception
        self.journal = journal or Journal()
        self.active: str | None = None  # the npc on its way to the hero
        self.walks = 0  # bodies sent to the hero (tests, scenarios)
        self.spoken: dict[tuple[str, str], int] = {}  # (npc, occasion) -> game minute it spoke of it
        self.now: int | None = None  # the game's clock, minutes since the first day began
        self._world = 0  # bumped on every load: a walk of the world before is over
        self._background: set[asyncio.Task[Any]] = set()
        # two talk next to the hero (gossip.py sets it): the first words wait for them
        self.floor_busy: Callable[[], bool] = lambda: False
        planner.allow = self.allow
        planner.on_approach = self.approach
        planner.facts_of = self.facts
        link.listeners.append(self._on_message)
        self._load()

    def _on_message(self, msg: GameMessage) -> None:
        match msg:
            case Loading():
                self._world += 1
                self.active = None
            case Session():
                self._world += 1
                self.active = None
                self.now = None
                self._load()
            case NpcViews():
                times = [v.time for v in msg.views if v.time is not None]
                if times:
                    self.now = max(times)

    def _load(self) -> None:
        """What it already spoke of, from the facts of the save branch."""
        self.spoken.clear()
        for f in self.chronicle.facts("initiative"):
            if "day" in f:
                at = _minute(int(f["day"]), int(f.get("hour", 0)), int(f.get("minute", 0)))
                self.spoken[(str(f.get("npc", "")).upper(), str(f.get("occasion", "")))] = at

    # --- the planner's hooks ----------------------------------------------------------------------

    def allow(self, rec: NpcRecord, choice: str) -> str | None:
        """Why the planner must not apply this choice now, or None."""
        if self.active == rec.id and choice not in TAKES_BODY:
            return "идёт к герою"  # another act would drop the walk's npc_idle
        if choice != "approach_talk":
            return None
        if rec.activity != "routine":
            return "занят"
        partner = self.talk.partner
        if partner is not None and partner.npc != rec.id:
            return "герой говорит с другим"
        if self.active is not None:
            return "к герою уже идут"
        if not hears(rec.distance_cm, rec.in_view):
            return "герой не услышит"
        state = self._state(rec.id)
        if asleep(state):
            return "спит"
        if (why := self.guard.body(rec.id, state)) is not None and rec.distance_cm > STAY_CM:
            return why
        occ = self.occasion(rec.id, rec.agenda)
        if occ is not None and occ.key.startswith("crime_witness:") and (rec.id, occ.key) in self.spoken:
            return "уже говорил об этом деле"
        if occ is not None and (ago := self._since(rec.id, occ)) is not None and ago < REPEAT_GAME_MIN:
            return "уже говорил об этом"
        return None

    def approach(self, decision: Decision, rec: NpcRecord) -> None:
        """approach_talk, taken over from the planner: the walk, then the first words."""
        self.active = rec.id
        stay = self.guard.body(rec.id, self._state(rec.id)) is not None
        self._spawn(self._run(rec.id, rec.name, stay, self._world))

    def facts(self, npc: str) -> tuple[str, ...]:
        """For the behavioral model's state: it already spoke of its occasion today."""
        occ = self.occasion(npc, self._agenda(npc))
        at = self.spoken.get((npc.upper(), occ.key)) if occ is not None else None
        if at is None or self.now is None or at // 1440 != self.now // 1440:
            return ()
        return (f"You already spoke to the hero about it today at {_hhmm(at)}.",)

    def occasion(self, npc: str, agenda: str | None = None) -> Occasion | None:
        """What it would come up about: its most pressing concern, else the agenda it was given."""
        concerns = self.concerns.of(npc)
        # A witness may still be deciding what to do after speaking. That case must not hide a
        # new concern it has not brought up yet; keep the spoken case only when nothing else waits.
        c = next(
            (
                c for c in concerns
                if not (c.key.startswith("crime_witness:") and (npc.upper(), c.key) in self.spoken)
            ),
            next(iter(concerns), None),
        )
        if c is not None:
            return Occasion(c.key, c.ru, c.en)
        if agenda:
            return Occasion("agenda:" + hashlib.sha1(agenda.encode()).hexdigest()[:8], f"«{agenda}»", agenda)
        return None

    # --- the walk and the words -------------------------------------------------------------------

    async def _run(self, npc: str, name: str, stay: bool, world: int) -> None:
        try:
            await self._approach(npc, name, stay, world)
        except (GameError, TimeoutError) as err:
            log.info("initiative of %s: %s", npc, err)
        finally:
            if self.active == npc:
                self.active = None

    async def _approach(self, npc: str, name: str, stay: bool, world: int) -> None:
        occ = self.occasion(npc, self._agenda(npc))
        state = await self.link.story_state(npcs=[npc])
        if world != self._world:
            return
        if all(k in state for k in ("day", "hour", "minute")):
            self.now = _minute(int(state["day"]), int(state["hour"]), int(state["minute"]))
        if state.get("dialog") or state.get("cutscene"):
            return self._skip(npc, name, "dialog")
        if await self.important(npc):
            return self._skip(npc, name, "important")  # the scripts speak first: the game's own talk
        if world != self._world:
            return
        if occ is not None and not self.budget.can(npc):
            return self._skip(npc, name, "budget")  # without words to say it does not come up
        self.walks += 1
        walk = asyncio.ensure_future(self.bodies.act(npc, "turn_to" if stay else "go_to_npc"))
        # the hero may speak to it on its way: the game then ends the walk (held: gone) or drops it (a look)
        while not walk.done() and not self._partner(npc):
            await asyncio.wait({walk}, timeout=WATCH_S)
        if world != self._world:
            walk.cancel()
            return
        if self._partner(npc):
            walk.cancel()
            if occ is not None:  # he spoke first: it knows what it came for
                self.dialogue.remember(npc, name, f"Ты сам шёл к герою: {occ.ru}")
            return
        outcome = walk.result()
        entry = self.talk.people.get(npc)
        if outcome.status != "done" or entry is None or entry.distance > (STAY_CM if stay else ARRIVE_CM):
            where = f"{entry.distance / 100:.0f} м" if entry is not None else "ушёл"
            return self._skip(npc, name, f"missed: {outcome.status}, {where}")
        if occ is None:
            return  # it came up and says nothing, as before
        if self.floor_busy():  # two talk next to the hero (gossip.py): it waits its turn, not over them
            loop = asyncio.get_running_loop()
            until = loop.time() + FLOOR_WAIT_S
            while self.floor_busy() and not self._partner(npc) and loop.time() < until:  # noqa: ASYNC110
                await asyncio.sleep(WATCH_S)  # the floor is another desk's state: polled, as the walk is
            if world != self._world:
                return
            if self._partner(npc):  # he spoke to it while it waited
                self.dialogue.remember(npc, name, f"Ты сам шёл к герою: {occ.ru}")
                return
            if self.floor_busy():
                return self._skip(npc, name, "рядом болтают")
            # the hero may have gone meanwhile: a scene ends when he walks off (gossip.py, hero_left)
            entry = self.talk.people.get(npc)
            if entry is None or entry.distance > (STAY_CM if stay else ARRIVE_CM):
                where = f"{entry.distance / 100:.0f} м" if entry is not None else "ушёл"
                return self._skip(npc, name, f"missed: waited, {where}")
        if self.talk.partner is not None:
            return self._skip(npc, name, "the hero talks to another")
        if not self.budget.allow(npc):
            return self._skip(npc, name, "budget")
        if not await self.talk.listen(npc):
            return self._skip(npc, name, "not held")
        if not self.talk.begin(npc, name, posture="listen"):
            self.talk.free(npc, "listen")
            return self._skip(npc, name, "the hero talks to another")
        self._record(npc, name, occ)
        self.journal.add("initiative", npc=npc, name=name, occasion=occ.key)
        self.dialogue.event_turn(
            npc,
            _cue(occ),
            relevant=lambda: self.talk.partner is not None and self.talk.partner.npc == npc,
            tools=TOOLS,
            kind="initiative",
        )

    async def important(self, npc: str) -> bool:
        """An important dialog of the scripts waits for the hero: the game's own check (Npc_CheckInfo,
        with the director's dialogs), else the graph's."""
        try:
            value = (await self.link.story_eval(npc, [CHECKINFO]))[0]
        except (GameError, TimeoutError, IndexError) as err:
            log.info("initiative: Npc_CheckInfo of %s: %s", npc, err)
            value = None
        if value is not None:
            return bool(value)
        graph = self.guard.graph
        if graph is None:
            return True  # nothing to tell by: let the game speak
        # a permanent one waits whenever its condition holds (a rent due, a fine), as Npc_CheckInfo has it
        nodes = [n for n in graph.dialogs_of(npc) if n.important]
        if not nodes:
            return False
        once = [n.id for n in nodes if not n.permanent]
        told = (await self.link.story_state(knows=once)).get("knows", {}) if once else {}
        waiting = [n for n in nodes if n.permanent or not told.get(n.id)]
        if not waiting:
            return False
        values = await self.link.story_eval(npc, [n.condition or ["int", 1] for n in waiting])
        return any(v is None or bool(v) for v in values)

    def _record(self, npc: str, name: str, occ: Occasion) -> None:
        now = self.now or 0
        self.spoken[(npc, occ.key)] = now
        where = None
        seen_by = ["PLAYER"]
        if self.perception is not None:
            view = self.perception.view(npc)
            place = self.perception.place(view) if view is not None else None
            where = place.id if place is not None else None
            seen_by += sorted(
                v.npc.upper() for v in self.perception.views() if any(o.id == npc for o in v.sees)
            )
        self.chronicle.record(
            {
                "fact": "initiative",
                "npc": npc,
                "name": name,
                "occasion": occ.key,
                "why": occ.ru,
                "where": where,
                "seen_by": seen_by,
                "day": now // 1440,
                "hour": now % 1440 // 60,
                "minute": now % 60,
            }
        )

    def _skip(self, npc: str, name: str, why: str) -> None:
        log.info("initiative of %s skipped: %s", npc, why)
        self.journal.add("initiative_skipped", npc=npc, name=name, why=why)

    # --- helpers --------------------------------------------------------------------------------

    def _partner(self, npc: str) -> bool:
        return self.talk.partner is not None and self.talk.partner.npc == npc

    def _state(self, npc: str) -> str:
        entry = self.talk.people.get(npc)
        return entry.state if entry is not None else ""

    def _agenda(self, npc: str) -> str | None:
        rec = self.planner.npcs.get(npc.upper())
        return rec.agenda if rec is not None else None

    def _since(self, npc: str, occ: Occasion) -> int | None:
        at = self.spoken.get((npc.upper(), occ.key))
        return None if at is None or self.now is None else self.now - at

    def _spawn(self, coro: Coroutine[Any, Any, Any]) -> None:
        task = asyncio.create_task(coro)
        self._background.add(task)
        task.add_done_callback(self._background.discard)


def _cue(occ: Occasion) -> str:
    if occ.key.startswith("agenda:"):
        return (
            f"[Ты сам подошёл к герою, он тебя не звал. Чего ты от него хочешь: {occ.ru}. Заговори с ним "
            "первым — коротко и в своём характере.]"
        )
    return (
        f"[Ты сам подошёл к герою, он тебя не звал. Повод: {occ.ru}. Заговори с ним первым — коротко, по "
        "делу и в своём характере.]"
    )


def _minute(day: int, hour: int, minute: int) -> int:
    return day * 1440 + hour * 60 + minute


def _hhmm(at: int) -> str:
    return f"{at % 1440 // 60:02d}:{at % 60:02d}"
