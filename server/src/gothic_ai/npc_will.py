"""Character rules for physical nearby deeds and distant outcomes, without a model.

Motives, transfers and starts belong to the save branch. MOTIVES is the complete rule table.
"""

from __future__ import annotations

import asyncio
import logging
import math
import time
from collections import defaultdict, deque
from collections.abc import Callable, Mapping
from dataclasses import dataclass
from typing import TYPE_CHECKING, Any

from .characters import Card
from .chronicle import Chronicle, Fact
from .game import GameError, GameLink
from .memory import MemoryStore
from .npc_crimes import NpcCrimes
from .npc_far import FarWorld
from .people_knowledge import PeopleKnowledge
from .protocol import (
    GameMessage,
    Loading,
    MemoryCmd,
    NpcDeed,
    NpcIdle,
    NpcRoster,
    NpcView,
    NpcViews,
    Session,
)
from .social import Rumors
from .storyguard import StoryGuard, asleep

if TYPE_CHECKING:
    from .blame import Blame

log = logging.getLogger(__name__)
DAY = 24 * 60
SIMULATION_CM = 3000
MEET_CM = 1500
ROUTINE_CM = 2000
OWNER_NEAR_CM = 1500
VIEW_SECONDS = 3.0
RETRY_MINUTES = 60
LOOP_SECONDS = 60.0
LOOP_STARTS = 8


def greed_interval(card: Card | None) -> int:
    """The character supplies the normal frequency; a missing card supplies no motive."""
    if card is None or card.honesty > 2 or card.greed < 4:
        return 0
    return (2 if card.honesty == 1 else 4) * DAY


@dataclass(frozen=True)
class MotiveRule:
    discover: str
    perform: str
    choose: Callable[[Card | None], str | None]
    interval: Callable[[Card | None], int] = lambda _: 0

    def accepts(self, card: Card | None) -> bool:
        return self.choose(card) is not None

    def action(self, card: Card | None) -> str | None:
        return self.choose(card)

    def cooldown(self, card: Card | None) -> int:
        return self.interval(card)


MOTIVES = {
    "reclaim": MotiveRule(
        "_discover_reclaim", "_meet",
        lambda card: None if card is None else "fight" if card.courage >= 3 else "accuse",
    ),
    "suspect": MotiveRule("_discover_suspect", "_meet", lambda _: "accuse"),
    "greed": MotiveRule(
        "_discover_greed", "_steal", lambda card: "steal" if greed_interval(card) else None,
        greed_interval,
    ),
}


class LoopGuard:
    """A backstop for broken rules, rather than a source of ordinary character frequency."""

    def __init__(self, clock: Callable[[], float] = time.monotonic) -> None:
        self.clock = clock
        self.starts: dict[str, deque[float]] = defaultdict(deque)
        self.refused = 0

    def claim(self, npc: str) -> bool:
        now, starts = self.clock(), self.starts[npc.upper()]
        while starts and starts[0] <= now - LOOP_SECONDS:
            starts.popleft()
        if len(starts) >= LOOP_STARTS:
            self.refused += 1
            log.warning("npc will loop: %s attempted more than %s starts in %.0fs", npc, LOOP_STARTS,
                        LOOP_SECONDS)
            return False
        starts.append(now)
        return True


class NpcWill:
    def __init__(
        self, link: GameLink, chronicle: Chronicle, npc_crimes: NpcCrimes, *,
        cards: Mapping[str, Card] | None = None, guard: StoryGuard | None = None,
        people: PeopleKnowledge | None = None, rumors: Rumors | None = None,
        blame: Blame | None = None,
        memory: MemoryStore | None = None, busy: Callable[[str], bool] = lambda _: False,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        self.link, self.chronicle, self.npc_crimes = link, chronicle, npc_crimes
        self.cards = cards if cards is not None else npc_crimes.cards
        self.guard, self.people = guard or npc_crimes.guard, people or PeopleKnowledge(chronicle)
        self.rumors, self.memory, self.busy, self.clock = rumors, memory, busy, clock
        self.blame = blame
        self.roster = npc_crimes.roster
        self.views: dict[str, NpcView] = {}
        self._view_at: dict[str, float] = {}
        self._hours: dict[str, int] = {}
        self._active: dict[str, str] = {}
        self._starting: dict[str, list[Fact]] = {}
        self._generation = 0
        self._now: int | None = None
        self._await_clock = False
        self._lock = asyncio.Lock()
        self._tasks: set[asyncio.Task[Any]] = set()
        self.loop_guard = LoopGuard(clock)
        self._scheduled: asyncio.Task[Any] | None = None
        self.far_world = FarWorld(self)
        link.listeners.append(self._on_message)
        chronicle.on_fact.append(self._on_fact)

    @property
    def enabled(self) -> bool:
        return self.npc_crimes.enabled

    async def close(self) -> None:
        if self._on_message in self.link.listeners:
            self.link.listeners.remove(self._on_message)
        if self._on_fact in self.chronicle.on_fact:
            self.chronicle.on_fact.remove(self._on_fact)
        for task in tuple(self._tasks):
            task.cancel()
        if self._tasks:
            await asyncio.gather(*tuple(self._tasks), return_exceptions=True)

    def _on_message(self, message: GameMessage) -> None:
        if isinstance(message, (Loading, Session)):
            self._generation += 1
            self._now = None
            self.views.clear()
            self._view_at.clear()
            self._hours.clear()
            self._active.clear()
            self._starting.clear()
            self._await_clock = True
            for task in tuple(self._tasks):
                task.cancel()
            self._scheduled = None
        elif isinstance(message, NpcViews):
            for view in message.views:
                npc = view.npc.upper()
                self.views[npc], self._view_at[npc] = view, self.clock()
                if view.time is not None:
                    self._now = view.time
                    self._await_clock = False
            self._kick()
        elif isinstance(message, NpcRoster):
            if self._now is not None and message.hour is not None and message.minute is not None:
                minute = message.hour * 60 + message.minute
                previous = self._now % DAY
                # A roster has no day: only accept forward time, including midnight.
                if minute >= previous or previous - minute > DAY // 2:
                    self._now += (minute - previous) % DAY
            self._kick()
        elif isinstance(message, NpcIdle) and message.act:
            active = self._active.get(message.npc.upper())
            attempt = next((a for a in self.attempts()
                            if a["id"] == active and a.get("action") == message.act), None)
            if attempt is not None:
                self._active.pop(message.npc.upper(), None)
                self._finished(attempt, message)

    def _on_fact(self, fact: Fact) -> None:
        if fact.get("fact") == "npc_crime" and fact.get("kind") == "theft":
            npc = str(fact.get("culprit") or "")
            if npc in self._starting:
                self._starting[npc].append(fact)
            else:
                attempt = next((a for a in reversed(self.attempts())
                                if a.get("kind") == "greed" and self._matches(a, fact)
                                and self._active.get(npc) == a["id"]), None)
                if attempt is not None:
                    self._transferred(attempt, fact)
        if fact.get("fact") in {
            "npc_crime", "npc_crime_seen", "npc_crime_suspected", "rumor", "blamed", "blame_refuted",
        }:
            self._kick()

    def _kick(self) -> None:
        if not self.enabled or (self._scheduled is not None and not self._scheduled.done()):
            return

        async def run() -> None:
            try:
                await self.tick()
            except (GameError, TimeoutError, ConnectionError) as error:
                log.debug("npc will: %s", error)
            except Exception:
                log.exception("npc will rule failed")

        self._scheduled = asyncio.create_task(run())
        self._tasks.add(self._scheduled)
        self._scheduled.add_done_callback(self._tasks.discard)

    async def tick(self, now: int | None = None) -> None:
        """Check character rules on the game hour, and pending claims on roster meetings."""
        if not self.enabled or self.chronicle.branch is None:
            return
        async with self._lock:
            generation = self._generation
            if now is not None:
                self._now = now
                self._await_clock = False
            self._refresh_clock()
            if self._now is None:
                return
            await self.far_world.refresh(generation)
            if generation != self._generation:
                return
            self._recover_attempts()
            for rule in MOTIVES.values():
                await getattr(self, rule.discover)(generation)
                if generation != self._generation:
                    return
            for motive in self.pending():
                if generation != self._generation or not self.enabled:
                    return
                rule = MOTIVES.get(motive.get("kind", ""))
                if rule is None or not rule.accepts(self.cards.get(motive["npc"])):
                    continue
                try:
                    await getattr(self, rule.perform)(motive, rule, generation)
                except (GameError, TimeoutError, ConnectionError) as error:
                    log.debug("npc will %s waits: %s", motive["id"], error)

    evaluate = tick

    def pending(self) -> list[Fact]:
        begun = {f.get("motive") for f in self.attempts()
                 if f.get("kind") != "greed" or f.get("outcome") == "committed"}
        return [f for f in self.chronicle.facts("npc_motive") if f.get("id") not in begun
                and not (f.get("crime") and self._resolved(f["crime"]))
                and self._belief_current(f)]

    def _belief_current(self, motive: Fact) -> bool:
        if self.blame is None or not motive.get("crime"):
            return not motive.get("blamed")
        belief = self.blame.active(motive["npc"], motive["crime"])
        if motive.get("blamed"):
            return belief is not None and belief["id"] == motive["blamed"]
        # A deliberate attribution replaces a mere suspicion or an earlier piece of hearsay.
        return belief is None

    def _crime(self, motive: Fact) -> Fact | None:
        if motive.get("blamed") and self.blame is not None:
            belief = self.blame.active(motive["npc"], motive["crime"])
            if belief is not None and belief["id"] == motive["blamed"]:
                return self.blame.crime_for(belief)
            return None
        return self.npc_crimes.crime(motive["crime"])

    @staticmethod
    def _at(fact: Fact) -> int:
        return int(fact.get("at", int(fact.get("day", 0)) * DAY
                            + int(fact.get("hour", 0)) * 60 + int(fact.get("minute", 0))))

    def _refresh_clock(self) -> None:
        # Rumors keeps ticking when the hero leaves all NPC views. It resets its clock on
        # Session; do not read a previous world's clock while loading is still in progress.
        if not self._await_clock and self.rumors is not None and self.rumors.clock is not None:
            self._now = max(self._now or 0, self.rumors.clock)

    def _matches(self, attempt: Fact, crime: Fact) -> bool:
        return bool(
            crime.get("kind") == "theft" and crime.get("culprit") == attempt.get("npc")
            and crime.get("owner") == attempt.get("owner")
            and crime.get("item") == attempt.get("item")
            and (not crime.get("chest") or crime["chest"] == attempt.get("target"))
            and self._at(crime) >= self._at(attempt)
        )

    def _attempt_history(self) -> tuple[
        list[Fact], list[tuple[int, Fact]], dict[tuple[str, str], Fact], dict[tuple[str, str], int],
    ]:
        facts = self.chronicle.facts()
        starts: list[tuple[int, Fact]] = []
        ordinals: dict[str, int] = defaultdict(int)
        latest: dict[tuple[str, str], Fact] = {}
        outcomes: dict[tuple[str, str], Fact] = {}
        terminal: dict[tuple[str, str], int] = {}
        for index, fact in enumerate(facts):
            if fact.get("fact") == "npc_will":
                motive = str(fact.get("motive") or "legacy")
                ordinals[motive] += 1
                start = {**fact, "id": fact.get("id") or f"{motive}:attempt:{ordinals[motive]}"}
                starts.append((index, start))
                latest[(start["npc"], start["action"])] = start
            elif fact.get("fact") == "npc_will_outcome":
                identity = fact.get("attempt")
                if not identity:
                    # Old outcomes had only the process-local action id. Attach them only to
                    # their preceding start; a restarted GameLink may reuse that same id later.
                    preceding = latest.get((fact["npc"], fact["action"]))
                    if preceding is None or preceding.get("motive") != fact.get("motive"):
                        continue
                    identity = preceding["id"]
                key = (identity, fact["action"])
                terminal.setdefault(key, index)
                old = outcomes.get(key)
                if old is None or old.get("outcome") != "committed":
                    outcomes[key] = fact
        return facts, starts, outcomes, terminal

    def attempts(self) -> list[Fact]:
        """Project immutable starts and their outcomes without changing Chronicle's source facts.

        A transfer is authoritative even when closing the chest was interrupted. Older starts
        lacking outcomes can still be proved by their subsequent physical deed; absent that
        proof, a load recovers the motive rather than inventing a completed theft.
        """
        facts, starts, outcomes, terminal = self._attempt_history()
        motives = {f.get("id"): f for f in facts if f.get("fact") == "npc_motive"}
        result = []
        for index, start in starts:
            motive = motives.get(start.get("motive"), {})
            attempt = {**{key: motive[key] for key in ("owner", "item") if key in motive}, **start}
            key = (start["id"], start["action"])
            outcome = outcomes.get(key)
            if outcome is not None:
                attempt.update({key: outcome[key] for key in (
                    "outcome", "reason", "detail", "retry_at", "crime", "finished_at",
                ) if key in outcome})
            if start.get("kind") == "greed" and attempt.get("outcome") != "committed":
                stop = next((i for i, other in starts
                             if i > index and other.get("npc") == start.get("npc")), len(facts))
                stop = min(stop, terminal.get(key, len(facts)))
                crime = next((f for f in facts[index + 1:stop]
                              if f.get("fact") == "npc_crime" and self._matches(attempt, f)), None)
                if crime is not None:
                    attempt.update(outcome="committed", reason="native_transfer", crime=crime["id"],
                                   finished_at=self._at(crime))
            result.append(attempt)
        return result

    def _outcome(self, attempt: Fact, outcome: str, reason: str, *, at: int | None = None,
                 detail: str = "", crime: str = "") -> None:
        _, _, outcomes, _ = self._attempt_history()
        previous = outcomes.get((attempt["id"], attempt["action"]))
        if previous is not None and (outcome != "committed" or previous.get("outcome") == "committed"):
            return
        finished = self._now if at is None else at
        if finished is None:
            return
        self.chronicle.record({
            "fact": "npc_will_outcome", "npc": attempt["npc"], "kind": attempt["kind"],
            "target": attempt["target"], "motive": attempt["motive"], "attempt": attempt["id"],
            "action": attempt["action"],
            "outcome": outcome, "reason": reason, "finished_at": finished,
            **({"far": True} if attempt.get("far") else {}),
            **({"detail": detail} if detail else {}), **({"crime": crime} if crime else {}),
            **({"retry_at": finished + RETRY_MINUTES}
               if attempt["kind"] == "greed" and outcome == "aborted" else {}),
        })

    def _transferred(self, attempt: Fact, crime: Fact) -> None:
        self._outcome(attempt, "committed", "native_transfer", at=self._at(crime), crime=crime["id"])

    def _finished(self, attempt: Fact, receipt: NpcIdle) -> None:
        self._refresh_clock()
        if attempt["kind"] == "greed" and attempt.get("outcome") == "committed":
            crime = self.npc_crimes.crime(str(attempt.get("crime") or ""))
            if crime is not None:
                self._transferred(attempt, crime)
            return
        completed = attempt["kind"] != "greed" and receipt.reason in {"done", "arrived"}
        self._outcome(attempt, "completed" if completed else "aborted", receipt.reason or "unknown",
                      detail=receipt.detail or "")

    def _recover_attempts(self) -> None:
        for attempt in self.attempts():
            if attempt["kind"] != "greed":
                continue
            if attempt.get("outcome") == "committed":
                crime = self.npc_crimes.crime(str(attempt.get("crime") or ""))
                if crime is not None:
                    self._transferred(attempt, crime)
            elif not attempt.get("outcome") and self._active.get(attempt["npc"]) != attempt["id"]:
                self._outcome(attempt, "aborted", "load_interrupted")

    def _retry_ready(self, motive: Fact) -> bool:
        last = next((a for a in reversed(self.attempts()) if a.get("motive") == motive["id"]), None)
        return last is None or (
            last.get("outcome") == "aborted" and (self._now or 0) >= last.get("retry_at", math.inf)
        )

    def _stamp(self) -> dict[str, int]:
        now = self._now or 0
        return {"at": now, "day": now // DAY, "hour": now % DAY // 60, "minute": now % 60}

    def _remember(self, npc: str, kind: str, target: str, key: str, why: str, **extra: Any) -> None:
        if any(f.get("id") == key for f in self.chronicle.facts("npc_motive")):
            return
        self.chronicle.record({
            "fact": "npc_motive", "id": key, "npc": npc, "kind": kind, "target": target,
            "why": why, **extra, **self._stamp(),
        })

    def _resolved(self, crime: str) -> bool:
        return self.npc_crimes.settled(crime) or any(
            f.get("crime") == crime for f in self.chronicle.facts("npc_crime_restitution")
        )

    async def _discover_reclaim(self, generation: int) -> None:
        if self.blame is not None:
            for belief in self.blame.beliefs():
                crime = self.blame.crime_for(belief)
                if crime is None or crime.get("kind") != "theft" or self._resolved(crime["id"]):
                    continue
                npc, culprit, cid = belief["npc"], belief["culprit"], belief["crime"]
                self._remember(npc, "reclaim", culprit, f"reclaim:{belief['id']}",
                               "считает его виновным в пропаже", crime=cid, blamed=belief["id"])
        for crime in self.npc_crimes.facts():
            owner, culprit, cid = crime.get("owner"), crime.get("culprit"), crime.get("id")
            if crime.get("kind") != "theft" or not owner or not culprit or self._resolved(cid):
                continue
            # Layer four already acted for a witness owner on the original scene. Its completed
            # reaction must not turn into a second, delayed attempt of the same personal claim.
            if any(f.get("crime") == cid and f.get("npc") == owner
                   for f in self.chronicle.facts("npc_crime_reaction")):
                continue
            if any(k.kind == "npc_theft" and k.subject == culprit and
                   k.key == f"npc_crime_seen:{cid}" for k in self.people.of(owner)):
                self._remember(owner, "reclaim", culprit, f"reclaim:{owner}:{cid}",
                               "знает, кто украл его вещь", crime=cid)

    async def _discover_suspect(self, generation: int) -> None:
        for suspicion in self.chronicle.facts("npc_crime_suspected"):
            npc, target, cid = suspicion.get("npc"), suspicion.get("suspect"), suspicion.get("crime")
            if not npc or not target or target in {"PLAYER", "HERO", "PC_HERO"} or not cid:
                continue
            if self.npc_crimes.crime(cid) is not None and not self._resolved(cid):
                self._remember(npc, "suspect", target, f"suspect:{npc}:{cid}:{target}",
                               "вспомнил его около сундука во время пропажи; доказательств нет", crime=cid)

    def _cool(self, npc: str, card: Card | None) -> bool:
        interval = greed_interval(card)
        if not interval:
            return False
        transfers = [f["finished_at"] for f in self.attempts()
                     if f.get("npc") == npc and f.get("kind") == "greed"
                     and f.get("outcome") == "committed"]
        return not transfers or (self._now or 0) - max(transfers) >= interval

    def _free(self, npc: str) -> bool:
        entry = self.roster.get(npc)
        # The engine sets AiNormal strictly inside 3000 cm (worldobjects.cpp). Do not use the
        # planner's tier hysteresis, which can retain a T2 actor already outside that boundary.
        return bool(
            entry is not None and entry.distance < SIMULATION_CM and entry.activity == "routine"
            and not asleep(entry.state) and not self.guard.body(npc, entry.state)
            and not self.busy(npc) and not getattr(self.link, "body_owned", lambda _: False)(npc)
            and npc not in self._active
        )

    async def _local(self, npc: str, target: str, generation: int, *,
                     chest: tuple[float, float, float] | None = None) -> bool:
        """A fresh position check prevents walking to a remote chest in an ordinary routine place."""
        actor = await self.link.snapshot(npc, fresh=True)
        if (generation != self._generation or actor.npc.id.upper() != npc or actor.player is None
                or math.dist(actor.npc.pos, actor.player.pos) >= SIMULATION_CM):
            return False
        other = await self.link.snapshot(target, fresh=True)
        if (generation != self._generation or other.npc.id.upper() != target or other.player is None
                or math.dist(other.npc.pos, other.player.pos) >= SIMULATION_CM):
            return False
        if chest is not None:
            return (math.dist(chest, actor.player.pos) < SIMULATION_CM
                    and math.dist(actor.npc.pos, chest) <= ROUTINE_CM)
        return math.dist(actor.npc.pos, other.npc.pos) <= MEET_CM

    async def _discover_greed(self, generation: int) -> None:
        social = self.rumors.social if self.rumors is not None else None
        if social is None:
            return
        await self._discover_far_greed(generation)
        if generation != self._generation:
            return
        for npc in list(self.roster):
            card = self.cards.get(npc)
            if not self._free(npc) or not self._cool(npc, card):
                continue
            hour = (self._now or 0) // 60
            if self._hours.get(npc) == hour:
                continue
            self._hours[npc] = hour
            if any(f.get("npc") == npc and f.get("kind") == "greed" for f in self.pending()):
                continue
            spots = [s.pos for s in social.days.get(npc, ()) if s.pos is not None]
            if not spots:
                continue
            for owner in list(self.roster):
                if owner == npc or not self._free(owner):
                    continue
                try:
                    chests = await self.link.npc_chests(owner)
                except (GameError, TimeoutError, ConnectionError) as error:
                    log.debug("npc will chests of %s unavailable: %s", owner, error)
                    continue
                for chest in chests:
                    if generation != self._generation:
                        return
                    if (chest.why != "owner" or chest.distance <= OWNER_NEAR_CM
                            or not any(math.dist(chest.pos, pos) <= ROUTINE_CM for pos in spots)):
                        continue
                    if not await self._local(npc, owner, generation, chest=chest.pos):
                        continue
                    for item in chest.items:
                        if item.count <= 0:
                            continue
                        args = {"chest": chest.id, "what": item.item, "count": 1, "deed": True}
                        try:
                            reason = await self.npc_crimes.check_command(npc, "steal", args)
                        except (GameError, TimeoutError, ConnectionError) as error:
                            log.debug("npc will target %s unavailable: %s", item.item, error)
                            continue
                        if generation != self._generation:
                            return
                        if reason or args.get("expected_owner") != owner:
                            continue
                        self._remember(
                            npc, "greed", chest.id, f"greed:{npc}:{self._now}",
                            f"честность {card.honesty}, жадность {card.greed}; чужой сундук в обычном месте",
                            owner=owner, item=item.item,
                        )
                        break
                    else:
                        continue
                    break
                else:
                    continue
                break

    async def _far_chest(self, npc: str, owner: str, chest: Any, generation: int) -> bool:
        world = self.far_world
        spot = world.routine(npc)
        if (not world.free(npc) or chest.why != "owner"
                or spot is None or spot.pos is None
                or math.dist(spot.pos, chest.pos) > ROUTINE_CM):
            return False
        if not await world.far(npc, generation) or not await world.far(owner, generation):
            return False
        actor, other = await world.position(npc, generation), await world.position(owner, generation)
        return bool(generation == self._generation and actor is not None and other is not None
                    and math.dist(actor.pos, chest.pos) <= ROUTINE_CM
                    and math.dist(other.pos, chest.pos) > OWNER_NEAR_CM)

    async def _discover_far_greed(self, generation: int) -> None:
        world, hour = self.far_world, (self._now or 0) // 60
        for npc in world.all_names():
            card = self.cards.get(npc)
            if (not world.free(npc) or not self._cool(npc, card)
                    or self._hours.get(npc) == hour or not await world.far(npc, generation)):
                continue
            self._hours[npc] = hour
            if any(f.get("npc") == npc and f.get("kind") == "greed" for f in self.pending()):
                continue
            for owner in world.all_names():
                if owner == npc or not await world.far(owner, generation):
                    continue
                try:
                    chests = await self.link.npc_chests(owner)
                except (GameError, TimeoutError, ConnectionError):
                    continue
                for chest in chests:
                    if not await self._far_chest(npc, owner, chest, generation):
                        continue
                    for item in chest.items:
                        if item.count <= 0:
                            continue
                        args = {"chest": chest.id, "what": item.item, "count": 1, "deed": True}
                        if await self.npc_crimes.check_command(npc, "steal", args):
                            continue
                        if generation != self._generation:
                            return
                        if args.get("expected_owner") != owner:
                            continue
                        self._remember(
                            npc, "greed", chest.id, f"greed:{npc}:{self._now}",
                            f"честность {card.honesty}, жадность {card.greed}; чужой сундук вдали",
                            owner=owner, item=item.item, far=True,
                        )
                        break
                    else:
                        continue
                    break
                else:
                    continue
                break

    async def _view(self, npc: str, generation: int) -> NpcView | None:
        views = await self.link.npc_view([npc])
        if generation != self._generation or not any(view.npc.upper() == npc for view in views):
            return None
        for view in views:
            who = view.npc.upper()
            self.views[who], self._view_at[who] = view, self.clock()
        if self.clock() - self._view_at.get(npc, -math.inf) > VIEW_SECONDS:
            return None
        return self.views.get(npc)

    async def _steal(self, motive: Fact, rule: MotiveRule, generation: int) -> None:
        npc, owner = motive["npc"], motive["owner"]
        if await self.far_world.far(npc, generation):
            await self._steal_far(motive, generation)
            return
        if (not self._free(npc) or not self._free(owner) or not self._cool(npc, self.cards.get(npc))
                or not self._retry_ready(motive)):
            return
        chest = next((c for c in await self.link.npc_chests(owner) if c.id == motive["target"]), None)
        if (generation != self._generation or chest is None or chest.why != "owner"
                or chest.distance <= OWNER_NEAR_CM):
            return
        social = self.rumors.social if self.rumors is not None else None
        if social is None or not any(
            spot.pos is not None and math.dist(chest.pos, spot.pos) <= ROUTINE_CM
            for spot in social.days.get(npc, ())
        ):
            return
        if not any(item.item == motive["item"] and item.count > 0 for item in chest.items):
            return
        view = await self._view(npc, generation)
        # This is the thief's belief, never a truth query for actual observers. A watcher behind
        # its back can still become an honest engine witness of the subsequent deed.
        if view is None or view.sees or (view.hero is not None and view.hero.seen):
            return
        args = {"chest": chest.id, "what": motive["item"], "count": 1, "deed": True}
        if await self.npc_crimes.check_command(npc, "steal", args):
            return
        if args.get("expected_owner") != owner:
            return
        if not await self._local(npc, owner, generation, chest=chest.pos):
            return
        if generation != self._generation or not self._free(npc) or not self._free(owner):
            return
        if not self.loop_guard.claim(npc):
            return
        self._starting[npc] = []
        started_at = self._stamp()
        try:
            action = await self.npc_crimes._act(npc, "steal", **args)
            self._started(motive, "steal", action, generation, started_at=started_at)
        finally:
            self._starting.pop(npc, None)

    async def _steal_far(self, motive: Fact, generation: int) -> None:
        npc, owner = motive["npc"], motive["owner"]
        if not self._cool(npc, self.cards.get(npc)) or not self._retry_ready(motive):
            return
        chest = next((c for c in await self.link.npc_chests(owner) if c.id == motive["target"]), None)
        if chest is None or not await self._far_chest(npc, owner, chest, generation):
            return
        if not any(item.item == motive["item"] and item.count > 0 for item in chest.items):
            return
        if not await self.far_world.alone(npc, generation):
            return
        args = {"chest": chest.id, "what": motive["item"], "count": 1, "deed": True}
        if await self.npc_crimes.check_command(npc, "steal", args) or args.get("expected_owner") != owner:
            return
        witnesses = await self.far_world.witnesses(npc, generation)
        metadata = {}
        for who in dict.fromkeys([npc, owner, *witnesses]):
            values = await self.link.story_eval(who, [
                ["field", ["inst", "SELF"], "GUILD"],
                ["field", ["inst", "SELF"], "NPCTYPE"],
            ])
            if generation != self._generation or len(values) != 2 or any(type(v) is not int for v in values):
                return
            metadata[who] = {"guild": values[0], "npctype": values[1]}
        if generation != self._generation or not self.loop_guard.claim(npc):
            return
        started_at = self._stamp()
        action = f"far:{motive['id']}:{len(self.attempts()) + 1}"
        await self.link.story_effect(npc, [{
            "do": "chest_take", "chest": chest.id, "item": motive["item"], "count": 1,
        }], check=True)
        if generation != self._generation:
            return
        self._started(motive, "steal", action, generation, started_at=started_at, far=True)
        attempt = next(a for a in reversed(self.attempts()) if a["action"] == action)
        self.npc_crimes.record(NpcDeed(
            type="npc_deed", id=action, kind="take_item", culprit=npc,
            culprit_guild=metadata[npc]["guild"], culprit_npctype=metadata[npc]["npctype"],
            item=motive["item"], count=1, owner=owner, owner_npc_guild=metadata[owner]["guild"],
            owner_npctype=metadata[owner]["npctype"], chest=chest.id, chest_owner=owner,
            chest_owner_guild=metadata[owner]["guild"], chest_owner_npctype=metadata[owner]["npctype"],
            position=dict(zip(("x", "y", "z"), chest.pos, strict=True)), waypoint=chest.waypoint,
            day=started_at["day"], hour=started_at["hour"], minute=started_at["minute"],
            witnesses=witnesses,
            witnesses_metadata=[{"npc": who, **metadata[who]} for who in witnesses],
        ), far=True)
        self._active.pop(npc, None)
        crime = self.npc_crimes.crime(action)
        if crime is not None:
            self._outcome(attempt, "committed", "far_transfer", crime=action)

    async def meeting(self, npc: str, target: str, generation: int) -> str | None:
        if await self.far_world.meeting(npc, target, generation):
            return "far"
        if not self._free(npc) or not self._free(target):
            return None
        view = await self._view(npc, generation)
        if (view is not None and any(o.id.upper() == target and o.distance <= MEET_CM for o in view.sees)
                and await self._local(npc, target, generation)
                and generation == self._generation and self._free(npc) and self._free(target)):
            return "near"
        return None

    async def fight_far(self, crime: Fact, fighter: str, generation: int, *,
                        role: str = "reclaim") -> Fact | None:
        return await self.far_world.fight(crime, fighter, generation, role=role)

    async def _meet(self, motive: Fact, rule: MotiveRule, generation: int) -> None:
        npc, target, cid = motive["npc"], motive["target"], motive["crime"]
        if not self._resolved(cid) and await self.far_world.meeting(npc, target, generation):
            await self._meet_far(motive, rule, generation)
            return
        if not self._free(npc) or not self._free(target) or self._resolved(cid):
            return
        if motive["kind"] == "reclaim" and (
            any(key[1:] == (cid, npc) for key in self.npc_crimes._reclaiming)
            or self._reacted(motive)
        ):
            return
        view = await self._view(npc, generation)
        if view is None or not any(o.id.upper() == target and o.distance <= MEET_CM for o in view.sees):
            return
        if not await self._local(npc, target, generation):
            return
        if not self._free(npc) or not self._free(target) or generation != self._generation:
            return
        act = rule.action(self.cards.get(npc))
        if not self.loop_guard.claim(npc):
            return
        started_at = self._stamp()
        if act == "fight":
            crime = self._crime(motive)
            if crime is None:
                return
            action = await self.npc_crimes._fight(
                crime, npc, self.npc_crimes._generation, role="reclaim", svm="DIRTYTHIEF",
            )
        else:
            action = await self.npc_crimes._act(
                npc, "say_svm", svm="DIRTYTHIEF", npc=target, overlay=True,
            )
        if generation != self._generation:
            return
        self._started(motive, str(act), action, generation, started_at=started_at)
        if act == "accuse":
            self._accused(motive)

    async def _meet_far(self, motive: Fact, rule: MotiveRule, generation: int) -> None:
        npc, cid = motive["npc"], motive["crime"]
        if motive["kind"] == "reclaim" and (
            any(key[1:] == (cid, npc) for key in self.npc_crimes._reclaiming)
            or self._reacted(motive)
        ):
            return
        if motive.get("blamed") and self.blame is not None:
            crime = self._crime(motive)
            if crime is None:
                return
            values = await self.link.story_eval(npc, [
                ["call", "NPC_HASITEMS", ["inst", motive["target"]], ["sym", crime["item"]]],
            ])
            if generation != self._generation or not self._belief_current(motive):
                return
            if len(values) != 1 or type(values[0]) is not int:
                return
            if values[0] <= 0:
                belief = self.blame.active(npc, cid)
                if belief is not None:
                    await self.blame.refute(belief, how="no_goods")
                return
        if not self.loop_guard.claim(npc):
            return
        act = str(rule.action(self.cards.get(npc)))
        action = f"far:{motive['id']}"
        if act == "fight":
            crime = self._crime(motive)
            if crime is None or await self.fight_far(crime, npc, generation) is None:
                return
        if generation != self._generation or self.npc_crimes.settled(cid):
            return
        self._started(motive, act, action, generation, far=True)
        self._active.pop(npc, None)
        attempt = next(a for a in reversed(self.attempts()) if a["action"] == action)
        if act == "accuse":
            self._accused(motive, far=True)
        self._outcome(attempt, "completed", "far_meeting")

    def _reacted(self, motive: Fact) -> bool:
        original = self.npc_crimes.crime(motive["crime"])
        return any(
            f.get("crime") == motive["crime"] and f.get("npc") == motive["npc"]
            and f.get("culprit", (original or {}).get("culprit")) == motive["target"]
            and (not motive.get("blamed") or f.get("blamed") == motive["blamed"])
            for f in self.chronicle.facts("npc_crime_reaction")
        )

    def _started(self, motive: Fact, act: str, action: str, generation: int, *,
                 started_at: dict[str, int] | None = None, far: bool = False) -> None:
        if generation != self._generation:
            return
        ordinal = 1 + sum(f.get("motive") == motive["id"] for f in self.chronicle.facts("npc_will"))
        identity = f"{motive['id']}:attempt:{ordinal}"
        if action not in self.npc_crimes._completed:
            self._active[motive["npc"]] = identity
        attempt = {
            "fact": "npc_will", "id": identity, "npc": motive["npc"], "kind": motive["kind"],
            "target": motive["target"], "act": act, "action": action, "motive": motive["id"],
            **({"far": True} if far else {}),
            **({key: motive[key] for key in ("crime", "owner", "item") if key in motive}),
            **(started_at if started_at is not None else self._stamp()),
        }
        self.chronicle.record(attempt)
        for crime in self._starting.get(motive["npc"], []):
            if self._matches(attempt, crime):
                self._transferred(attempt, crime)
        if receipt := self.npc_crimes._completed.get(action):
            current = next(a for a in self.attempts() if a["id"] == identity)
            self._finished(current, receipt)

    def _name(self, npc: str) -> str:
        entry = self.roster.get(npc)
        return entry.name if entry is not None else npc

    def _accused(self, motive: Fact, *, far: bool = False) -> None:
        npc, target = motive["npc"], motive["target"]
        witnesses = {npc}
        for observer, view in self.views.items():
            if far:
                break
            if self.clock() - self._view_at.get(observer, -math.inf) > VIEW_SECONDS:
                continue
            if any(o.id.upper() == npc and o.distance <= MEET_CM for o in view.sees):
                witnesses.add(observer)
        witnesses.discard(target)
        self.chronicle.record({
            "fact": "npc_accused", "id": motive["id"], "npc": npc, "name": self._name(npc),
            "target": target, "target_name": self._name(target), "kind": motive["kind"],
            "crime": motive["crime"], "witnesses": sorted(witnesses),
            **({"far": True} if far else {}),
            "witness_names": {who: self._name(who) for who in witnesses}, **self._stamp(),
        })
        branch = self.chronicle.branch
        if self.memory is None or branch is None:
            return
        name = self._name(target)
        note = (f"Ты думаешь, что {name} украл твою вещь, и обвинил его; доказательств нет."
                if motive["kind"] == "suspect" else f"Ты обвинил {name} в краже своей вещи.")
        seq = self.memory.append(branch, npc, "system", note)
        task = asyncio.create_task(self.link.send(MemoryCmd(branch=branch, seq=seq)))
        self._tasks.add(task)
        task.add_done_callback(self._tasks.discard)
