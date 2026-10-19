"""Encounters follow the current branch's proven knowledge and the NPC's character.

Private native home warnings keep reading CrimeMemoryRuntime. Mapped homes that vanilla makes
public use the same warning and nonlethal fight through held room perceptions. Murder projection
is deliberately nonlethal: only this table may turn remembered murder into an encounter fight.
"""

from __future__ import annotations

import asyncio
import logging
import time
from collections.abc import Callable, Mapping
from dataclasses import dataclass
from typing import TYPE_CHECKING, Any, Literal

from .characters import Card
from .chronicle import Fact
from .crime_memory import CrimeMemoryRuntime
from .crime_react import CrimeReact, Role
from .crimes import HERO, Crimes
from .game import GameError, GameLink
from .people_knowledge import rumor_subject
from .protocol import (
    GameMessage,
    HoldVerdictCmd,
    Loading,
    NpcActResult,
    NpcEvent,
    NpcIdle,
    NpcRoster,
    PerceptionHeld,
    Session,
)

if TYPE_CHECKING:
    from .amends import Amends
    from .initiative import ImportantApproach

log = logging.getLogger(__name__)
POLL_SECONDS = 1.0
REFUSE_SECONDS = 2
NEAR_LEAVE_CM = 700
ROOM_RANGE_CM = 1000
ROOM_HOLD_SECONDS = 0.5  # the bridge returns unanswered perceptions to scripts after 0.6 s
ROOM_GRACE_SECONDS = 5.0
OWNER_COURAGE = 3
HELP_COURAGE = 4
COWARD_COURAGE = 2
MURDER_REASON = "AR_HUMANMURDEREDHUMAN"
ReactionKind = Literal["demand", "fight", "flee", "refuse", "room"]


def reaction_for(
    kind: str, role: Role, *, courage: int = 3, law_known: bool = False
) -> ReactionKind | None:
    """Knowing a theft never licenses an encounter fight; murder requires a narrow reason."""
    if kind in {"theft", "attack"}:
        return "demand" if role == "owner" and courage >= OWNER_COURAGE else None
    if kind != "murder":
        return None
    if role == "guard":
        return "fight" if law_known else None
    if role == "member" and courage >= HELP_COURAGE:
        return "fight"
    return "flee" if courage <= COWARD_COURAGE else "refuse"


@dataclass(frozen=True)
class Encounter:
    crime: Fact
    role: Role
    reaction: ReactionKind
    source: Literal["personal", "law"] = "personal"


@dataclass
class Active:
    encounter: Encounter
    event: str
    generation: int
    branch: str | None
    refusal_owned: bool = False
    room_house: str | None = None
    room_warning_id: str | None = None
    room_warned_at: float | None = None
    room_fighting: bool = False


class CrimeMeet:
    def __init__(
        self,
        link: GameLink,
        crimes: Crimes,
        reactions: CrimeReact,
        memory: CrimeMemoryRuntime,
        *,
        amends: Amends | None = None,
        approach: ImportantApproach | None = None,
        cards: Mapping[str, Card] | None = None,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        self.link, self.crimes, self.reactions, self.memory = link, crimes, reactions, memory
        self.chronicle, self.law = crimes.chronicle, reactions.law
        self.amends, self.approach = amends, approach
        self.cards = reactions.cards if cards is None else cards
        self.clock = clock
        self._generation = 0
        self._loading = False
        self._closed = False
        self._near: dict[str, NpcEvent] = {}
        self._rooms: dict[str, NpcEvent] = {}
        self._room_checked: set[str] = set()
        self._active: dict[str, Active] = {}
        self._done: set[tuple[str, str, str, str]] = set()
        self._demands: dict[str, asyncio.Task[None]] = {}
        self._room_tasks: set[asyncio.Task[None]] = set()
        self._room_pending: set[str] = set()
        self._room_holds: set[str] = set()
        self._receipts: dict[str, NpcActResult] = {}
        self._idles: dict[str, NpcIdle] = {}
        self._latest_actions: dict[str, str] = {}
        self._receipt_wake = asyncio.Event()
        self._wake = asyncio.Event()
        self._lock = asyncio.Lock()
        self._reason_index: int | None = None
        self.link.listeners.append(self._on_message)
        self.chronicle.on_fact.append(self._on_fact)

    def _on_message(self, message: GameMessage) -> None:
        if isinstance(message, (Loading, Session)):
            self._generation += 1
            self._loading = isinstance(message, Loading)
            self._near.clear()
            self._rooms.clear()
            self._room_checked.clear()
            self._active.clear()
            self._done.clear()
            self._receipts.clear()
            self._idles.clear()
            self._latest_actions.clear()
            self._reason_index = None
            self._room_pending.clear()
            self._room_holds.clear()
            for task in (*self._demands.values(), *self._room_tasks):
                if not task.done() and not task.cancelling():
                    task.cancel()
            self._wake.set()
        elif (isinstance(message, PerceptionHeld) and message.kind == "enter_room"
              and message.other.upper() in HERO):
            if self._closed or message.hold in self._room_holds:
                return
            self._room_holds.add(message.hold)
            self._room_pending.add(message.npc.upper())
            task = asyncio.create_task(self._held_room(message, self._generation))
            self._room_tasks.add(task)
            task.add_done_callback(self._room_tasks.discard)
        elif isinstance(message, NpcEvent) and message.other.upper() in HERO:
            npc = message.npc.upper()
            if message.kind == "player_near":
                self._near[npc] = message
                self._wake.set()
            elif message.kind == "enter_room" and (message.distance or 0) <= ROOM_RANGE_CM:
                self._rooms[npc] = message
                self._room_checked.discard(npc)
                self._wake.set()
        elif isinstance(message, NpcRoster):
            roster = {entry.id.upper(): entry for entry in message.npcs}
            for npc in tuple(self._near.keys() | self._rooms.keys()):
                entry = roster.get(npc)
                if entry is None and not message.full:
                    continue
                if entry is None or entry.distance > NEAR_LEAVE_CM:
                    self._near.pop(npc, None)
                    self._done = {key for key in self._done if key[0] != npc}
                if entry is None or entry.distance > ROOM_RANGE_CM:
                    self._rooms.pop(npc, None)
                    self._room_checked.discard(npc)
        elif isinstance(message, NpcActResult):
            self._receipts[message.id] = message
            if message.status == "queued":
                self._latest_actions[message.npc.upper()] = message.id
            if len(self._receipts) > 128:
                self._receipts.pop(next(iter(self._receipts)))
            self._receipt_wake.set()
            self._wake.set()
        elif isinstance(message, NpcIdle) and message.act is not None:
            self._idles[message.act] = message
            if len(self._idles) > 128:
                self._idles.pop(next(iter(self._idles)))
            self._wake.set()

    def _on_fact(self, fact: Fact) -> None:
        if fact.get("fact") == "rumor" and rumor_subject(fact) != "PLAYER":
            return
        if fact.get("fact") in {
            "crime", "crime_seen", "rumor", "testimony", "crime_known", "forgiven", "silenced",
            "absolved", "died",
        }:
            # A model may be writing a demand. Cancel it before its next await can install stale work.
            for npc, task in tuple(self._demands.items()):
                active = self._active.get(npc)
                if active is not None and not self._valid(npc, active):
                    if not task.done() and not task.cancelling():
                        task.cancel()
            self._wake.set()

    def _settled(self, npc: str, crime: str) -> bool:
        return any(
            fact.get("npc", "").upper() == npc
            and (
                fact.get("fact") == "died"
                or (
                    fact.get("fact") in {"silenced", "forgiven"}
                    and fact.get("crime") == crime
                    and (self.crimes.cover or not fact.get("cover"))
                )
            )
            for fact in self.chronicle.facts()
        )

    def _absolved(self, crime: Fact) -> bool:
        return any(
            fact.get("community") == crime.get("community") and crime["id"] in fact.get("crimes", [])
            for fact in self.chronicle.facts("absolved")
        )

    def _cases(self, npc: str) -> list[tuple[Fact, Literal["personal", "law"]]]:
        personal = {
            known.crime["id"]: (known.crime, "personal")
            for known in self.crimes.known(npc)
            if known.degree in {"seen", "heard"} and known.suspect == "PLAYER"
            and not known.forgiven and not known.silenced
        }
        known = {
            (fact.get("community"), fact.get("crime"))
            for fact in self.chronicle.facts("crime_known")
        }
        for crime in self.crimes.facts():
            if (crime.get("kind") == "murder"
                and (crime.get("community"), crime["id"]) in known
                and self.law.is_guard(npc, crime.get("community"))):
                personal[crime["id"]] = (crime, "law")
        return [
            (crime, source) for crime, source in personal.values()
            if not self._settled(npc, crime["id"])
            and not (self.law.is_guard(npc, crime.get("community")) and self._absolved(crime))
        ]

    def _valid(self, npc: str, active: Active) -> bool:
        return (
            not self._closed and not self._loading and active.generation == self._generation
            and active.branch == self.chronicle.branch
            and any(crime["id"] == active.encounter.crime["id"] for crime, _ in self._cases(npc))
        )

    def _free(self, npc: str) -> bool:
        body_owned = getattr(self.link, "body_owned", lambda _: False)
        entry = self.reactions.roster.get(npc)
        return (
            not body_owned(npc) and (entry is None or (
                entry.activity == "routine" and entry.state.upper() not in {
                    "ZS_SLEEP", "ZS_GOTOBED", "ZS_UNCONSCIOUS", "ZS_DEAD", "ZS_CLEARROOM",
                }
            ))
        )

    async def _person(self, npc: str, generation: int) -> bool:
        person = self.crimes.people.get(npc, {})
        if "guild" in person and "npctype" in person:
            return True
        values = await self.link.story_eval(npc, [
            ["field", ["inst", "SELF"], "GUILD"], ["field", ["inst", "SELF"], "NPCTYPE"],
        ])
        if generation != self._generation or len(values) != 2 or any(type(v) is not int for v in values):
            return False
        self.crimes.people[npc] = {**person, "guild": values[0], "npctype": values[1]}
        return True

    async def _queue(self, npc: str, action: str, /, **args: Any) -> str | None:
        async with asyncio.timeout(2):
            action_id = await self.link.npc_act(npc, action, **args)  # type: ignore[arg-type]
            while action_id not in self._receipts:
                self._receipt_wake.clear()
                await self._receipt_wake.wait()
            return action_id if self._receipts.pop(action_id).status == "queued" else None

    async def _act(self, npc: str, action: str, /, **args: Any) -> bool:
        return await self._queue(npc, action, **args) is not None

    def _record(self, npc: str, active: Active, **details: Any) -> None:
        encounter = active.encounter
        self._done.add((npc, encounter.crime["id"], encounter.reaction, active.event))
        self.chronicle.record({
            "fact": "crime_meet", "npc": npc, "crime": encounter.crime["id"],
            "reaction": encounter.reaction, "event": active.event, "source": encounter.source,
            "role": encounter.role, **details,
        })

    def _home_case(self, npc: str) -> tuple[Encounter, str] | None:
        """Public portal ownership is not evidence: use the household's actual sleep routines."""
        house = self.reactions.home(npc)
        if house is None or not house.startswith("place:"):
            return None  # without the map we cannot prove that the NPC is currently in its own home
        for crime, source in reversed(self._cases(npc)):
            if source != "personal" or crime.get("kind") != "theft":
                continue
            owner = str(crime.get("owner") or crime.get("victim") or "")
            role = self.reactions.role(npc, crime)
            if role in {"owner", "member"} and self.reactions.home(owner) == house:
                return Encounter(crime, role, "room"), house
        return None

    async def _held_room(self, held: PerceptionHeld, generation: int) -> None:
        """Take only a proven household warning; uncertain and private entries stay native."""
        npc, branch = held.npc.upper(), self.chronicle.branch
        verdict = "scripts"
        started: Active | None = None
        try:
            async with asyncio.timeout(ROOM_HOLD_SECONDS):
                candidate = self._home_case(npc)
                if candidate is not None and not self._loading:
                    encounter, house = candidate
                    active = Active(encounter, "enter_room", generation, branch, room_house=house)
                    views, values = await asyncio.gather(
                        self.link.npc_view([npc]),
                        self.link.story_eval(npc, [
                            ["call", "NPC_ISINPLAYERSROOM", ["inst", "SELF"]],
                            ["call", "NPC_CANSEENPC", ["inst", "SELF"], ["inst", "HERO"]],
                            ["call", "NPC_GETDISTTONPC", ["inst", "SELF"], ["inst", "HERO"]],
                            ["call", "WLD_GETPLAYERPORTALGUILD"],
                        ]),
                    )
                    view = next((view for view in views if view.npc.upper() == npc), None)
                    place = (
                        self.crimes.places.of(view.nearest_waypoint)
                        if self.crimes.places is not None and view is not None else None
                    )
                    present = (
                        self._valid(npc, active) and view is not None and view.indoors
                        and view.hero is not None and view.hero.seen
                        and place is not None and "place:" + place.id == house
                        and len(values) == 4 and all(type(value) is int for value in values)
                        and bool(values[0]) and bool(values[1]) and 0 <= values[2] <= ROOM_RANGE_CM
                    )
                    if present and values[3] == 15:
                        old = self._active.get(npc)
                        if old is not None and old.room_house == house and self._valid(npc, old):
                            verdict = "server"  # the same warning's grace and fight keep running
                        else:
                            if old is not None:
                                await self._clear(npc, old)
                            if self._valid(npc, active) and self._free(npc):
                                active.room_warning_id = await self._queue(npc, "say_svm", svm="GETOUTOFHERE")
                                if active.room_warning_id is not None:
                                    if self._valid(npc, active):
                                        self._active[npc] = started = active
                                        verdict = "server"
                                    else:
                                        await self._clear(npc, active)
                    elif present:
                        # The private portal already runs the correct ZS_ClearRoom. Its native
                        # perception is released below; the ordinary room event observes that state.
                        self._rooms[npc] = NpcEvent(
                            type="npc_event", npc=npc, kind="enter_room", distance=values[2],
                        )
        except (GameError, TimeoutError, ConnectionError) as error:
            log.debug("crime room %s: %s", npc, error)
        except asyncio.CancelledError:
            pass  # a load discarded this world's perceptions; close returns a pending hold below
        except Exception:
            log.exception("crime room %s", npc)
        finally:
            if generation == self._generation and branch == self.chronicle.branch:
                self._room_pending.discard(npc)
                if started is not None and not self._valid(npc, started):
                    verdict = "scripts"
                try:
                    await self.link.send(HoldVerdictCmd(hold=held.hold, to=verdict))  # type: ignore[arg-type]
                except (GameError, ConnectionError):
                    pass
                self._wake.set()

    async def _finish_room(self, npc: str, active: Active, outcome: str) -> None:
        if active.generation == self._generation and active.branch == self.chronicle.branch:
            self._record(npc, active, outcome=outcome)
        await self._clear(npc, active)
        self._rooms.pop(npc, None)
        self._done.discard((npc, active.encounter.crime["id"], "room", "enter_room"))

    async def _watch_room(self, npc: str, active: Active) -> None:
        if active.room_warned_at is None:
            idle = self._idles.get(active.room_warning_id or "")
            if self._latest_actions.get(npc) != active.room_warning_id:
                await self._finish_room(npc, active, "interrupted")
                return
            if idle is not None:
                if idle.reason != "done":
                    await self._finish_room(npc, active, idle.reason)
                    return
                active.room_warned_at = self.clock()
                self._record(npc, active, svm="GETOUTOFHERE", outcome="warned")
        values = await self.link.story_eval(npc, [
            ["call", "WLD_GETPLAYERPORTALGUILD"],
            ["call", "NPC_GETNEARESTWP", ["inst", "HERO"]],
            ["call", "NPC_ISINPLAYERSROOM", ["inst", "SELF"]],
            ["call", "NPC_GETDISTTONPC", ["inst", "SELF"], ["inst", "HERO"]],
        ])
        if not self._valid(npc, active):
            await self._finish_room(npc, active, "settled")
            return
        place = self.crimes.places.of(values[1]) if (
            len(values) == 4 and isinstance(values[1], str) and self.crimes.places is not None
        ) else None
        if len(values) != 4 or any(type(values[index]) is not int for index in (0, 2, 3)):
            return  # unavailable position is not evidence that the hero stayed to defy the warning
        if (values[0] != 15 or not values[2] or not 0 <= values[3] <= ROOM_RANGE_CM
            or place is None or "place:" + place.id != active.room_house):
            await self._finish_room(npc, active, "left")
        elif (active.room_warned_at is not None and not active.room_fighting
              and self.clock() - active.room_warned_at >= ROOM_GRACE_SECONDS
              and self._free(npc)):
            if await self._act(npc, "fight", npc="PLAYER", reason="AR_CLEARROOM"):
                active.room_fighting = True
                if self._valid(npc, active):
                    self._record(npc, active, outcome="fight", reason="AR_CLEARROOM")
                else:
                    await self._finish_room(npc, active, "settled")

    async def _clear(self, npc: str, active: Active) -> None:
        task = self._demands.pop(npc, None)
        if task is not None and task is not asyncio.current_task():
            if not task.done() and not task.cancelling():
                task.cancel()
            await asyncio.gather(task, return_exceptions=True)
        if active.generation != self._generation or active.branch != self.chronicle.branch:
            return
        if self._active.get(npc) is active:
            self._active.pop(npc, None)
        await self.memory.project(npc)
        if active.generation != self._generation or active.branch != self.chronicle.branch:
            return
        if any(f.get("npc", "").upper() == npc for f in self.chronicle.facts("died")):
            return
        if getattr(self.link, "body_owned", lambda _: False)(npc):
            return
        reaction = active.encounter.reaction
        if active.refusal_owned:
            await self.link.story_eval(npc, [
                ["call", "NPC_SETREFUSETALK", ["inst", "SELF"], ["int", 0]],
            ])
        if reaction == "room" and active.room_house is not None and not active.room_fighting:
            # A queued warning owns no later native action. Only cancel it while its own tracking
            # is still pending; a completed/failed/replaced warning must not clear another queue.
            if (active.room_warning_id is not None and active.room_warned_at is None
                and active.room_warning_id not in self._idles
                and self._latest_actions.get(npc) == active.room_warning_id and self._free(npc)):
                await self._resume(npc)
            return
        if reaction in {"fight", "room"}:
            # The reason and native state must still belong to our tracked encounter. Calm itself
            # also checks that the actual target is PLAYER, never another NPC's unrelated fight.
            if self._reason_index is None:
                values = await self.link.story_eval(npc, [["var", "AIV_ATTACKREASON"]])
                if len(values) != 1 or type(values[0]) is not int:
                    return
                if active.generation != self._generation or active.branch != self.chronicle.branch:
                    return
                self._reason_index = values[0]
            reason = MURDER_REASON if reaction == "fight" else "AR_CLEARROOM"
            if (reaction == "room" and active.room_house is None
                and self.crimes.people.get(npc, {}).get("guild") in {1, 2, 6, 7}):
                return  # these guilds expel intruders even without a remembered crime
            values = await self.link.story_eval(npc, [
                ["call", "NPC_ISINSTATE", ["inst", "SELF"], ["sym", "ZS_ATTACK"]],
                ["==", ["field", ["inst", "SELF"], "AIVAR", self._reason_index], ["var", reason]],
                ["call", "NPC_ISINSTATE", ["inst", "SELF"], ["sym", "ZS_CLEARROOM"]],
            ])
            if active.generation != self._generation or active.branch != self.chronicle.branch:
                return
            if len(values) == 3 and values[0] and values[1]:
                await self.link.story_effect(npc, [{"do": "calm", "attitude": "ATT_NEUTRAL"}])
            elif reaction == "room" and len(values) == 3 and values[2]:
                await self._resume(npc)
        elif reaction == "flee":
            values = await self.link.story_eval(npc, [
                ["call", "NPC_ISINSTATE", ["inst", "SELF"], ["sym", "ZS_FLEE"]],
            ])
            if active.generation == self._generation and len(values) == 1 and values[0]:
                await self._resume(npc)

    async def _resume(self, npc: str) -> None:
        await self.link.story_eval(npc, [
            ["call", "NPC_CLEARAIQUEUE", ["inst", "SELF"]],
            ["call", "AI_CONTINUEROUTINE", ["inst", "SELF"]],
        ])

    async def _demand(self, npc: str, active: Active) -> None:
        try:
            if self.amends is None or self.approach is None or not self._valid(npc, active):
                return
            storylet = await self.amends.demand(npc, active.encounter.crime["id"])
            if storylet is None or not self._valid(npc, active):
                return
            if storylet.stage != "open":
                return  # the same important demand was already heard in this branch
            approached = await self.approach.approach(
                npc, "crime:" + active.encounter.crime["id"],
                valid=lambda: self._valid(npc, active) and npc in self._near,
                told=lambda: storylet.stage != "open",
                release=lambda: self.memory.suspend(npc),
            )
            if active.generation == self._generation and active.branch == self.chronicle.branch:
                if approached:
                    self._record(npc, active)
                else:
                    # A busy partner can make the soft approach decline. Reuse its existing
                    # storylet on the next poll, without asking the writer for another one.
                    self._done.discard((npc, active.encounter.crime["id"], "demand", active.event))
        except (GameError, TimeoutError, ConnectionError) as error:
            log.debug("crime demand %s: %s", npc, error)

    async def _encounter(self, npc: str) -> None:
        active = self._active.get(npc)
        if npc in self._room_pending or (active is not None and active.room_house is not None):
            return
        generation, branch = self._generation, self.chronicle.branch
        if not await self._person(npc, generation):
            return
        if generation != self._generation or branch != self.chronicle.branch:
            return
        courage = self.cards[npc].courage if npc in self.cards else 3
        candidates = []
        for crime, source in self._cases(npc):
            role = self.reactions.role(npc, crime)
            if role is None:
                continue
            reaction = reaction_for(crime.get("kind", ""), role, courage=courage, law_known=source == "law")
            if reaction is not None:
                candidates.append(Encounter(crime, role, reaction, source))
        # Native home warnings take precedence over walking the victim out of its house to talk.
        if npc in self._rooms:
            rooms = [
                Encounter(crime, role, "room")
                for crime, source in self._cases(npc)
                if source == "personal" and crime.get("kind") == "theft"
                and (role := self.reactions.role(npc, crime)) in {"owner", "member"}
                and self.reactions.home(npc) is not None
                and self.reactions.home(npc) == self.reactions.home(
                    str(crime.get("owner") or crime.get("victim") or "")
                )
            ]
            if rooms:
                old = self._active.get(npc)
                if old is not None and old.encounter.reaction == "demand":
                    await self._clear(npc, old)
                await self.memory.project(npc)
                entry = self.reactions.roster.get(npc)
                if entry is not None and entry.state.upper() == "ZS_CLEARROOM":
                    active = Active(rooms[-1], "enter_room", generation, branch)
                    key = (npc, active.encounter.crime["id"], "room", "enter_room")
                    if key not in self._done and self._valid(npc, active):
                        self._active[npc] = active
                        self._record(npc, active)
                    return
                if npc not in self._room_checked:
                    self._room_checked.add(npc)
                    return  # give native perceptions one poll to start the home warning
                self._rooms.pop(npc, None)
                self._room_checked.discard(npc)
        if npc not in self._near or not candidates or not self._free(npc):
            return
        priority = {"fight": 4, "flee": 3, "refuse": 2, "demand": 1}
        encounter = max(candidates, key=lambda row: (priority[row.reaction], row.crime.get("day", 0)))
        key = (npc, encounter.crime["id"], encounter.reaction, "player_near")
        if key in self._done or npc in self._demands:
            return
        if encounter.reaction == "fight":
            views = await self.link.npc_view([npc])
            if (generation != self._generation or branch != self.chronicle.branch
                or not any(view.npc.upper() == npc and view.hero is not None and view.hero.seen
                           for view in views)):
                return  # proximity crosses walls; native sight must identify the murderer
        old = self._active.get(npc)
        if old is not None:
            await self._clear(npc, old)
        active = Active(encounter, "player_near", generation, branch)
        if not self._valid(npc, active) or not self._free(npc):
            return
        self._active[npc] = active
        if encounter.reaction == "demand":
            if self.amends is None or self.approach is None:
                self._active.pop(npc, None)
                return
            self._done.add(key)  # a missing writer result must not spend quota every poll
            task = asyncio.create_task(self._demand(npc, active))
            self._demands[npc] = task

            def finished(done: asyncio.Task[None]) -> None:
                if self._demands.get(npc) is done:
                    self._demands.pop(npc, None)
                if not done.cancelled() and done.exception() is not None:
                    log.error("crime demand %s failed", npc, exc_info=done.exception())

            task.add_done_callback(finished)
            return
        await self.memory.project(npc)
        if not self._valid(npc, active) or not self._free(npc):
            await self._clear(npc, active)
            return
        if encounter.reaction == "fight":
            accepted = await self._act(npc, "fight", npc="PLAYER", reason=MURDER_REASON)
        else:
            refused = await self.link.story_eval(npc, [["call", "NPC_REFUSETALK", ["inst", "SELF"]]])
            if not self._valid(npc, active):
                await self._clear(npc, active)
                return
            active.refusal_owned = refused == [0]
            if active.refusal_owned:
                await self.link.story_effect(npc, [{"do": "refuse_talk", "seconds": REFUSE_SECONDS}])
            if not self._valid(npc, active):
                await self._clear(npc, active)
                return
            if encounter.reaction == "flee":
                accepted = await self._act(npc, "flee", npc="PLAYER", svm="HELP")
            else:
                accepted = await self._act(npc, "say_svm", svm="NOTNOW", overlay=True)
                if self._valid(npc, active):
                    accepted = await self._act(npc, "turn_away") and accepted
        if self._valid(npc, active) and accepted:
            self._record(npc, active)
        else:
            await self._clear(npc, active)

    async def tick(self) -> None:
        async with self._lock:
            for npc, active in tuple(self._active.items()):
                try:
                    if active.room_house is not None:
                        if self._valid(npc, active):
                            await self._watch_room(npc, active)
                        else:
                            await self._finish_room(npc, active, "settled")
                    elif not self._valid(npc, active) or (
                        active.encounter.reaction in {"demand", "flee", "refuse"} and npc not in self._near
                    ):
                        await self.memory.project(npc)
                        await self._clear(npc, active)
                    elif active.refusal_owned:
                        await self.link.story_effect(npc, [{"do": "refuse_talk", "seconds": REFUSE_SECONDS}])
                except (GameError, TimeoutError, ConnectionError) as error:
                    log.debug("crime encounter clear %s: %s", npc, error)
            for npc in sorted(self._near.keys() | self._rooms.keys()):
                try:
                    await self._encounter(npc)
                except (GameError, TimeoutError, ConnectionError) as error:
                    log.debug("crime encounter %s: %s", npc, error)

    def summary(self, crime: str | None = None) -> str:
        return "\n".join(
            f"  {fact['npc']}: {fact['reaction']} ({fact['event']}; {fact.get('source', 'personal')})"
            + (f" {fact['outcome']}" if fact.get("outcome") else "")
            + (f" {fact['svm']}" if fact.get("svm") else "")
            for fact in self.chronicle.facts("crime_meet") if crime is None or fact.get("crime") == crime
        )

    def refuses(self, npc: str) -> bool:
        """The microphone and native TALK share the same current, canonical refusal rule."""
        if self._closed or self._loading or self.chronicle.branch is None:
            return False
        npc = npc.upper()
        courage = self.cards[npc].courage if npc in self.cards else 3
        return any(
            reaction_for(crime.get("kind", ""), role, courage=courage, law_known=source == "law")
            in {"refuse", "flee"}
            for crime, source in self._cases(npc)
            if (role := self.reactions.role(npc, crime)) is not None
        )

    async def run(self) -> None:
        while not self.link.closed.is_set():
            self._wake.clear()
            await self.link.wait_world()
            await self.tick()
            try:
                await asyncio.wait_for(self._wake.wait(), POLL_SECONDS)
            except TimeoutError:
                pass

    async def close(self) -> None:
        self._closed = True
        if self._on_message in self.link.listeners:
            self.link.listeners.remove(self._on_message)
        if self._on_fact in self.chronicle.on_fact:
            self.chronicle.on_fact.remove(self._on_fact)
        tasks = (*self._demands.values(), *self._room_tasks)
        for task in tasks:
            if not task.done() and not task.cancelling():
                task.cancel()
        for npc, active in tuple(self._active.items()):
            try:
                await self._clear(npc, active)
            except (GameError, TimeoutError, ConnectionError):
                pass
        if tasks:
            await asyncio.gather(*tasks, return_exceptions=True)
        if self.approach is not None:
            await self.approach.close()
