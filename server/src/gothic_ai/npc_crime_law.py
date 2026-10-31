"""Community news and guard encounters for NPC cases, isolated from the hero's law.

Reports and completed interventions are projections of Chronicle's current save branch. Near the
hero the existing nonlethal body action owns the result; distant meetings use NpcWill's shared rule.
"""

from __future__ import annotations

import asyncio
import logging
from typing import TYPE_CHECKING, Any

from .chronicle import Chronicle, Fact
from .crime_law import LAW_COMMUNITIES, REPORT_DELAYS, game_minutes
from .crimes import COMMUNITIES
from .game import GameError, GameLink
from .npc_crimes import NpcCrimes
from .protocol import GameMessage, Loading, NpcIdle, NpcRoster, NpcViews, Session

if TYPE_CHECKING:
    from .npc_will import NpcWill

log = logging.getLogger(__name__)


class NpcCrimeLaw:
    def __init__(
        self, link: GameLink, chronicle: Chronicle, npc_crimes: NpcCrimes, *, will: NpcWill,
    ) -> None:
        self.link, self.chronicle, self.npc_crimes, self.will = link, chronicle, npc_crimes, will
        self._generation = 0
        self._pending: dict[str, tuple[str, str]] = {}
        self._tasks: set[asyncio.Task[Any]] = set()
        self._scheduled: asyncio.Task[Any] | None = None
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

    def known(self, community: str) -> list[Fact]:
        ids = {
            fact.get("crime") for fact in self.chronicle.facts("npc_crime_known")
            if fact.get("community") == community
        }
        return [crime for crime in self.npc_crimes.facts()
                if crime.get("community") == community and crime.get("id") in ids]

    def inform(self, crime: Fact, *, via: str, at: int) -> Fact | None:
        community = crime.get("community")
        if not self.enabled or community not in LAW_COMMUNITIES:
            return None
        if any(f.get("id") == crime["id"] for f in self.known(community)):
            return None
        fact: Fact = {
            "fact": "npc_crime_known", "crime": crime["id"], "community": community,
            "via": via.upper(), "at": at,
        }
        self.chronicle.record(fact)
        return fact

    def deliver(self, now: int) -> list[Fact]:
        """A guard reports immediately; an ordinary witness needs the hero law's same delay.

        Only direct witnesses from the victim's community can report. Neither hero crime facts nor
        rumors addressed to the hero can turn into an NPC case (or the other way round).
        """
        law = self.npc_crimes.law
        if not self.enabled or law is None:
            return []
        crimes = {crime["id"]: crime for crime in self.npc_crimes.facts()}
        routes = []
        for seen in self.chronicle.facts("npc_crime_seen"):
            crime = crimes.get(seen.get("crime"))
            if crime is None or crime.get("community") not in LAW_COMMUNITIES:
                continue
            npc, community = str(seen.get("npc") or "").upper(), crime["community"]
            at = game_minutes(crime)
            if not npc or at is None or crime.get("kind") not in REPORT_DELAYS:
                continue
            guard = law.is_guard(npc, community)
            if not guard and law.community_of(npc) != community:
                continue
            due = at if guard else at + REPORT_DELAYS[crime["kind"]]
            if due > now:
                continue
            if not guard and any(
                f.get("npc", "").upper() == npc and (dead := game_minutes(f)) is not None
                and dead < due for f in self.chronicle.facts("died")
            ):
                continue
            routes.append((due, npc, crime))
        out = []
        for due, npc, crime in sorted(routes, key=lambda route: (route[0], route[1])):
            if fact := self.inform(crime, via=npc, at=due):
                out.append(fact)
        return out

    def _on_message(self, message: GameMessage) -> None:
        if isinstance(message, (Loading, Session)):
            self._generation += 1
            self._pending.clear()
            for task in tuple(self._tasks):
                task.cancel()
            self._scheduled = None
        elif isinstance(message, (NpcViews, NpcRoster)):
            self._kick()
        elif isinstance(message, NpcIdle) and message.act:
            self._finished(message)

    def _on_fact(self, fact: Fact) -> None:
        kind = fact.get("fact")
        if kind == "npc_crime_seen":
            # A direct guard's knowledge is true at the original observation, even before a clock
            # packet arrives. Reports by other witnesses still wait for the actual game clock.
            crime = self.npc_crimes.crime(str(fact.get("crime") or ""))
            law = self.npc_crimes.law
            npc = str(fact.get("npc") or "").upper()
            if crime is not None and law is not None and law.is_guard(npc, crime.get("community")):
                if (at := game_minutes(crime)) is not None:
                    self.inform(crime, via=npc, at=at)
        elif kind == "npc_crime_reaction" and fact.get("reaction") == "fight":
            if fact.get("role") in {"guard", "law"} and fact.get("act"):
                self._pending[str(fact["crime"])] = (str(fact["npc"]), str(fact["act"]))
        elif kind == "npc_crime_defense":
            crime = self.npc_crimes.crime(str(fact.get("crime") or ""))
            if crime is not None and self._guard_action(crime, str(fact.get("act") or "")):
                self._settle(
                    crime, str(fact.get("fighter") or ""), str(fact.get("outcome") or "fight"),
                    far=bool(fact.get("far")),
                )

    def _kick(self) -> None:
        if not self.enabled or (self._scheduled is not None and not self._scheduled.done()):
            return

        async def run() -> None:
            try:
                await self.tick()
            except (GameError, TimeoutError, ConnectionError) as error:
                log.debug("NPC law waits: %s", error)
            except Exception:
                log.exception("NPC law failed")

        self._scheduled = asyncio.create_task(run())
        self._tasks.add(self._scheduled)
        self._scheduled.add_done_callback(self._tasks.discard)

    async def _metadata(self, npc: str, generation: int) -> None:
        crimes = self.npc_crimes.crimes
        if crimes is None:
            return
        person = crimes.people.get(npc, {})
        if "guild" in person and "npctype" in person:
            return
        values = await self.link.story_eval(npc, [
            ["field", ["inst", "SELF"], "GUILD"],
            ["field", ["inst", "SELF"], "NPCTYPE"],
        ])
        if generation == self._generation and len(values) == 2 and all(type(v) is int for v in values):
            crimes.people[npc] = {**person, "guild": values[0], "npctype": values[1]}

    def _guard_action(self, crime: Fact, action: str = "") -> Fact | None:
        return next((reaction for reaction in reversed(self.chronicle.facts("npc_crime_reaction"))
                     if reaction.get("crime") == crime["id"] and reaction.get("reaction") == "fight"
                     and reaction.get("role") in {"guard", "law"}
                     and (not action or reaction.get("act") == action)), None)

    def _settle(self, crime: Fact, guard: str, how: str, *, far: bool = False) -> Fact | None:
        if not self.enabled or self.npc_crimes.settled(crime["id"]):
            return None
        fact: Fact = {
            "fact": "npc_crime_settled", "crime": crime["id"], "by": guard, "how": how,
            **({"far": True} if far else {}),
        }
        self.chronicle.record(fact)
        self._pending.pop(crime["id"], None)
        return fact

    def _finished(self, message: NpcIdle) -> None:
        for cid, (guard, action) in tuple(self._pending.items()):
            if action != message.act or guard != message.npc.upper():
                continue
            crime = self.npc_crimes.crime(cid)
            if crime is not None and crime.get("kind") == "attack":
                self._settle(crime, guard, message.detail or message.reason or "fight")

    async def tick(self, now: int | None = None) -> None:
        if not self.enabled or self.chronicle.branch is None:
            return
        # Motives and law own the same bodies and transfers. One lock makes a completed settlement
        # visible before the owner's next motive or another guard's meeting is evaluated.
        async with self.will._lock:
            generation = self._generation
            if now is None:
                self.will._refresh_clock()
                now = self.will._now
            else:
                self.will._now = now
                self.will._await_clock = False
            if now is None:
                return
            await self.will.far_world.refresh(self.will._generation)
            if generation != self._generation:
                return
            witnesses = {str(f.get("npc") or "").upper()
                         for f in self.chronicle.facts("npc_crime_seen")}
            for npc in sorted(witnesses - {""}):
                try:
                    await self._metadata(npc, generation)
                except (GameError, TimeoutError, ConnectionError):
                    continue
                if generation != self._generation:
                    return
            self.deliver(now)
            for community in LAW_COMMUNITIES:
                for crime in self.known(community):
                    if generation != self._generation:
                        return
                    if self.npc_crimes.settled(crime["id"]):
                        continue
                    existing = self._guard_action(crime)
                    if existing is not None:
                        # Layer four keeps priority on its original scene. Project any saved final
                        # result too: an old branch may have the defense but no law settlement yet.
                        defense = next((fact for fact in reversed(
                            self.chronicle.facts("npc_crime_defense")
                        ) if fact.get("crime") == crime["id"] and fact.get("act") == existing.get("act")
                            and fact.get("fighter") == existing.get("npc")), None)
                        if defense is not None:
                            self._settle(
                                crime, existing["npc"], str(defense.get("outcome") or "fight"),
                                far=bool(defense.get("far")),
                            )
                            continue
                        pending = crime["id"] in self._pending
                        completed = (self.npc_crimes._completed.get(str(existing.get("act") or ""))
                                     if pending else None)
                        if completed is not None:
                            self._finished(completed)
                        elif not pending:
                            # Loading cancels the old body watcher. A later honest meeting closes
                            # that one interrupted intervention; it must never order a second fight.
                            mode = await self.will.meeting(
                                existing["npc"], crime["culprit"], self.will._generation,
                            )
                            if mode is not None and generation == self._generation:
                                self._settle(crime, existing["npc"], "interrupted", far=mode == "far")
                        continue
                    if not crime.get("far") and now <= (game_minutes(crime) or 0):
                        continue  # the original on-scene reaction has first use of these bodies
                    candidates = self._guards(community)
                    for guard in sorted(candidates):
                        law = self.npc_crimes.law
                        if guard == crime["culprit"] or law is None:
                            continue
                        try:
                            await self._metadata(guard, generation)
                            if not law.is_guard(guard, community):
                                continue
                            await self._encounter(crime, guard, generation)
                        except (GameError, TimeoutError, ConnectionError) as error:
                            log.debug("NPC law %s, %s waits: %s", crime["id"], guard, error)
                        if generation != self._generation:
                            return
                        if self.npc_crimes.settled(crime["id"]) or self._guard_action(crime):
                            break

    def _guards(self, community: str) -> set[str]:
        # The world can contain thousands of ordinary people. Query missing NPCTYPE only for a
        # possible guard; membership still goes through CrimeLaw's exact community rule afterward.
        guilds = {"city": {1, 2}, "oldcamp": {1, 2}, "farm": {7}, "monastery": {4}}[community]
        candidates = set(COMMUNITIES[community].lawkeepers)
        crimes = self.npc_crimes.crimes
        if crimes is not None:
            candidates.update(npc for npc, data in crimes.people.items() if data.get("guild") in guilds)
        candidates.update(npc for npc, entry in self.npc_crimes.roster.items() if entry.guild in guilds)
        world = self.will.far_world
        if world.social is not None:
            candidates.update(npc for npc in world.all_names() if world.social.guilds.get(npc) in guilds)
        return candidates

    def _restitution_claimed(self, crime: Fact, guard: str, *, far: bool) -> bool:
        cid = crime["id"]
        if any(f.get("crime") == cid for f in self.chronicle.facts("npc_crime_restitution")):
            self._settle(crime, guard, "already_returned", far=far)
            return True
        return (self.npc_crimes._generation, cid) in self.npc_crimes._restitution_started

    async def _encounter(self, crime: Fact, guard: str, generation: int) -> None:
        culprit, cid = crime["culprit"], crime["id"]
        mode = await self.will.meeting(guard, culprit, self.will._generation)
        if mode is None or generation != self._generation:
            return
        actor, target = self.npc_crimes.roster.get(guard), self.npc_crimes.roster.get(culprit)
        protection = self.npc_crimes.guard
        if protection.harm(
            guard, target=culprit, state=actor.state if actor else "",
            target_state=target.state if target else "", item=crime.get("item", ""),
        ):
            return
        if crime.get("kind") == "theft":
            owner, item = str(crime.get("owner") or ""), str(crime.get("item") or "")
            owner_entry = self.npc_crimes.roster.get(owner)
            if not owner or not item or protection.harm(
                culprit, target=owner, state=target.state if target else "",
                target_state=owner_entry.state if owner_entry else "",
                item=item, mission=bool(crime.get("mission")),
            ):
                return
            if self._restitution_claimed(crime, guard, far=mode == "far"):
                return
            values = await self.link.story_eval(culprit, [
                ["call", "NPC_HASITEMS", ["inst", "SELF"], ["sym", item]],
            ])
            if generation != self._generation or len(values) != 1:
                return
            count = values[0]
            if type(count) is not int or count <= 0:
                return
            if self._restitution_claimed(crime, guard, far=mode == "far"):
                return
            card = self.will.cards.get(culprit)
            if card is None or card.courage <= 3:
                # Body defense and distant fights use this same claim before any transfer. Spare
                # ordinary items of the stolen type must never pay for the same theft twice.
                reservation = (self.npc_crimes._generation, cid)
                self.npc_crimes._restitution_started.add(reservation)
                transfer_started = False
                amount = min(int(crime["count"]), count)
                try:
                    if mode == "near":
                        await self.npc_crimes._act(guard, "say_svm", svm="DIRTYTHIEF", overlay=True)
                    if generation != self._generation:
                        return
                    transfer_started = True
                    await self.link.story_effect(culprit, [{
                        "do": "give", "item": item, "count": amount, "to": "npc", "npc": owner,
                    }], check=True)
                except GameError:
                    self.npc_crimes._restitution_started.discard(reservation)
                    raise
                finally:
                    if not transfer_started:
                        self.npc_crimes._restitution_started.discard(reservation)
                if generation == self._generation:
                    self.chronicle.record({
                        "fact": "npc_crime_restitution", "crime": cid, "npc": owner,
                        "culprit": culprit, "fighter": guard, "item": item, "count": amount,
                        "how": "surrendered", **({"far": True} if mode == "far" else {}),
                    })
                    self._settle(crime, guard, "surrendered", far=mode == "far")
                return
        if mode == "far":
            result = await self.will.fight_far(crime, guard, self.will._generation, role="law")
            if result is not None and generation == self._generation:
                self._settle(crime, guard, str(result.get("outcome") or "fight"), far=True)
        else:
            action = await self.npc_crimes._fight(
                crime, guard, self.npc_crimes._generation, role="law",
                svm="DIRTYTHIEF" if crime.get("kind") == "theft" else "HELP",
            )
            if generation == self._generation:
                self._pending[cid] = (guard, action)
                if completed := self.npc_crimes._completed.get(action):
                    self._finished(completed)
