"""Deterministic reactions to a witnessed deed, before its held perception reaches scripts.

The planner only remembers these events as context. A rule owns each hold exactly once; missing
evidence or a busy body returns it to vanilla before the bridge's 600 ms deadline. Beliefs remain
in Crimes/Chronicle, including when a witness chooses silence or to ignore somebody else's loss.
"""

from __future__ import annotations

import asyncio
import logging
import time
from collections import deque
from collections.abc import Awaitable, Callable, Mapping
from dataclasses import dataclass
from typing import Any, Literal

from .characters import Card
from .chronicle import Fact
from .crime_law import CrimeLaw
from .crimes import HERO, Crimes
from .game import GameError, GameLink
from .protocol import (
    GameMessage,
    HeroDeed,
    HoldVerdictCmd,
    Loading,
    NpcActResult,
    NpcRoster,
    PerceptionHeld,
    RosterEntry,
    Session,
)

log = logging.getLogger(__name__)

HOLD_SECONDS = 0.5  # the bridge releases after 0.6 s; leave a frame for our verdict
SHOUT_RANGE_CM = 2500
RECLAIM_SECONDS = 180.0
GUILD_RETRY_SECONDS = 5.0
OWNER_FIGHT_COURAGE = 3
HELP_COURAGE = 4
COWARD_COURAGE = 2
REPORT_HONESTY = 4
SILENT_HONESTY = 2

Role = Literal["owner", "ally", "guard", "member", "stranger"]
ReactionKind = Literal["fight", "call_guards", "shout", "demand", "flee", "silent", "ignore"]


@dataclass(frozen=True)
class Reaction:
    kind: ReactionKind
    svm: str = ""
    fight_reason: str = ""


def reaction_for(
    kind: str,
    role: Role,
    *,
    courage: int = 3,
    honesty: int = 3,
    guards: bool = False,
) -> Reaction | None:
    """The first, deliberately explicit table. None means that vanilla must decide."""
    if role == "ally":
        return Reaction("ignore")
    if kind == "theft":
        if role == "guard" or (role == "owner" and courage >= OWNER_FIGHT_COURAGE):
            return Reaction("fight", "DIRTYTHIEF", "AR_THEFT")
        if role == "owner":
            return Reaction("call_guards", "GUARDS") if guards else Reaction("demand", "HANDSOFF")
        if role == "member":
            if honesty <= SILENT_HONESTY:
                return Reaction("silent")
            if honesty >= REPORT_HONESTY and guards:
                return Reaction("call_guards", "GUARDS")
            return Reaction("shout", "DIRTYTHIEF")
        return Reaction("ignore")
    if kind == "attack":
        if role == "owner":
            return None  # the victim's own ASSESSDAMAGE remains vanilla
        if role == "guard" or (role == "member" and courage >= HELP_COURAGE):
            return Reaction("fight", "HELP", "AR_GUARDSTOPSFIGHT")
        if role == "member":
            return Reaction("flee", "HELP") if courage <= COWARD_COURAGE else Reaction("shout", "HELP")
        return Reaction("ignore")
    if kind == "murder":
        if role == "owner":
            return None
        if role == "guard" or (role == "member" and courage >= HELP_COURAGE):
            return Reaction("fight", "HELP", "AR_HUMANMURDEREDHUMAN")
        if role == "member" or courage <= COWARD_COURAGE:
            return Reaction("flee", "HELP")
        return Reaction("silent")
    if kind == "sheep":
        if role in {"owner", "guard"}:
            return Reaction("fight", "", "AR_SHEEPKILLER")
        return Reaction("shout", "HELP") if role == "member" else Reaction("ignore")
    return None


class _Fallback(Exception):
    pass


class CrimeReact:
    def __init__(
        self,
        link: GameLink,
        crimes: Crimes,
        law: CrimeLaw | None = None,
        *,
        cards: Mapping[str, Card] | None = None,
        project: Callable[[str], Awaitable[None]] | None = None,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        self.link, self.crimes = link, crimes
        crimes.react = self
        self.law = law or CrimeLaw(crimes)
        self.cards = cards if cards is not None else {}
        self.project = project
        self.clock = clock
        self.history: deque[Fact] = deque(maxlen=500)
        self.roster: dict[str, RosterEntry] = {}
        self._deeds: dict[str, HeroDeed] = {}
        self._holds: set[str] = set()
        self._tasks: set[asyncio.Task[None]] = set()
        self._receipts: dict[str, NpcActResult] = {}
        self._receipt_wake = asyncio.Event()
        self._fact_wake = asyncio.Event()
        self._generation = 0
        self._prepared = False
        self._guild_retry_at = 0.0
        self._closed = False
        self._reclaiming: set[tuple[int, str, str]] = set()
        self._restitution_started: set[tuple[int, str]] = set()
        link.listeners.append(self._on_message)
        crimes.chronicle.on_fact.append(self._on_fact)
        crimes.on_noncrime.append(self._on_noncrime)

    async def prepare(self) -> None:
        """Read actual homes once per world; no routine-prefix guesses for stateless households."""
        if not self._prepared:
            generation = self._generation
            people = await self.link.story_people()
            if generation != self._generation:
                return
            for person in people:
                npc = str(person.get("npc", "")).upper()
                if npc:
                    self.crimes.people[npc] = {**self.crimes.people.get(npc, {}), **person}
            self._prepared = True
        if self.clock() < self._guild_retry_at:
            return
        self._guild_retry_at = self.clock() + GUILD_RETRY_SECONDS
        try:
            await self.crimes.sides.prepare()
        except (GameError, TimeoutError) as error:
            log.debug("crime reactions: guilds unavailable: %s", error)

    async def run(self) -> None:
        while not self.link.closed.is_set():
            await self.link.wait_world()
            try:
                await self.prepare()
            except (GameError, TimeoutError) as error:
                log.debug("crime reactions: people unavailable: %s", error)
            # Session resets the cache; a short poll only loads metadata, never makes decisions.
            try:
                await asyncio.wait_for(self.link.closed.wait(), 1.0)
            except TimeoutError:
                pass

    async def close(self) -> None:
        self._closed = True
        if self.crimes.react is self:
            self.crimes.react = None
        if self._on_message in self.link.listeners:
            self.link.listeners.remove(self._on_message)
        if self._on_fact in self.crimes.chronicle.on_fact:
            self.crimes.chronicle.on_fact.remove(self._on_fact)
        if self._on_noncrime in self.crimes.on_noncrime:
            self.crimes.on_noncrime.remove(self._on_noncrime)
        tasks = tuple(self._tasks)
        for task in tasks:
            task.cancel()
        if tasks:
            await asyncio.gather(*tasks, return_exceptions=True)

    def _sleep_house(self, npc: str) -> str | None:
        """A house in the existing place book, identified by actual native sleep routines."""
        if self.crimes.places is None:
            return None
        houses = set()
        for routine in self.crimes.people.get(npc.upper(), {}).get("routine", []):
            if str(routine.get("state", "")).upper() not in {"ZS_GOTOBED", "ZS_SLEEP"}:
                continue
            place = self.crimes.places.of(routine.get("waypoint"))
            if place is None or place.kind != "house" or not place.inside:
                return None
            houses.add(place.id)
        return next(iter(houses)) if len(houses) == 1 else None

    def home(self, npc: str) -> str | None:
        """The best household identity for diagnostics; rooms can subdivide a mapped house."""
        house = self._sleep_house(npc)
        if house is not None:
            return "place:" + house
        room = self.crimes.people.get(npc.upper(), {}).get("home_room")
        return "room:" + room if room else None

    def role(self, npc: str, crime: Fact) -> Role | None:
        role = self.base_role(npc, crime)
        if role == "owner":
            return role
        if self.crimes.side(npc, crime["id"]) is not None:
            return "ally"
        if role == "stranger" and self.crimes.does_not_blame(npc, crime):
            return "ally"
        return role

    def base_role(self, npc: str, crime: Fact) -> Role | None:
        """Connection to the victim before a personal side or a heard justification."""
        npc = npc.upper()
        deed = self._deeds.get(crime["id"])
        owners = {str(crime.get("owner", "")).upper(), str(crime.get("victim", "")).upper()}
        if deed is not None and crime.get("kind") == "theft":
            owners.update(self.crimes._npc(n) for n in (deed.owner, deed.room_owner, deed.chest_owner) if n)
        if npc in owners:
            return "owner"
        person = self.crimes.people.get(npc, {})
        community = crime.get("community")
        if self.law.is_guard(npc, community, person):
            return "guard"
        if "guild" not in person or (person.get("guild") in {1, 2, 3} and "npctype" not in person):
            return None
        # Bandits do not take the side of a victim merely because both belong to BL.
        if community and community != "bl" and self.law.community_of(npc, person) == community:
            return "member"
        if not community:
            owner = str(crime.get("owner") or crime.get("victim") or "").upper()
            home = self.crimes.people.get(owner, {})
            if person.get("guild") == home.get("guild"):
                # BED waypoints can lack BSP rooms (Lobart's beds do), and one house may have
                # several rooms. The existing mapped house is a stronger identity than a sector.
                observer_house, owner_house = self._sleep_house(npc), self._sleep_house(owner)
                if observer_house is not None and owner_house is not None:
                    return "member" if observer_house == owner_house else "stranger"
                observer_room, owner_room = person.get("home_room"), home.get("home_room")
                if observer_room and owner_room:
                    return "member" if observer_room == owner_room else "stranger"
                return None  # missing rooms and unresolved house maps are uncertainty, not separation
            if owner and not home:
                return None  # without the owner's household, an outsider classification is a guess
        return "stranger"

    def summary(self, crime: str) -> str:
        rows = [f for f in self.crimes.chronicle.facts("crime_reaction") if f.get("crime") == crime]
        return "\n".join(
            f"  {f['npc']}: {f['reaction']} ({f.get('role') or '?'}), "
            f"{f['hold']} → {f['verdict']}: {f['reason']}" for f in rows
        )

    def _on_fact(self, fact: Fact) -> None:
        if fact.get("fact") == "crime":
            self._fact_wake.set()

    def _on_noncrime(self, deed: str) -> None:
        self._fact_wake.set()

    def _on_message(self, message: GameMessage) -> None:
        if isinstance(message, (Session, Loading)):
            self._generation += 1
            self.roster.clear()
            self._deeds.clear()
            self._holds.clear()
            self._receipts.clear()
            self._reclaiming.clear()
            self._restitution_started.clear()
            self._prepared = False
            self._guild_retry_at = 0.0
            for task in tuple(self._tasks):
                task.cancel()
        elif isinstance(message, HeroDeed):
            self._deeds[message.id] = message
            for witness in message.witnesses_metadata:
                npc = self.crimes._npc(witness.npc)
                self.crimes.people[npc] = {
                    **self.crimes.people.get(npc, {}), "guild": witness.guild, "npctype": witness.npctype,
                }
            owners = (
                (message.owner, message.owner_npc_guild, message.owner_npctype),
                (message.room_owner, message.room_owner_guild, message.room_owner_npctype),
                (message.chest_owner, message.chest_owner_guild, message.chest_owner_npctype),
            )
            for owner, guild, npctype in owners:
                if owner:
                    npc = self.crimes._npc(owner)
                    self.crimes.people[npc] = {
                        **self.crimes.people.get(npc, {}), "guild": guild, "npctype": npctype,
                    }
        elif isinstance(message, NpcRoster):
            if message.full:
                self.roster.clear()
            self.roster.update((entry.id.upper(), entry) for entry in message.npcs)
        elif isinstance(message, NpcActResult):
            self._receipts[message.id] = message
            if len(self._receipts) > 256:
                self._receipts.pop(next(iter(self._receipts)))
            self._receipt_wake.set()
        elif isinstance(message, PerceptionHeld) and (message.deed or message.other.upper() in HERO):
            if message.kind in {"enter_room", "bandit_enemy", "party_enemy"}:
                return  # Room encounters and bandit sightings belong to their own rules.
            if message.hold in self._holds or self._closed:
                return
            self._holds.add(message.hold)
            task = asyncio.create_task(self._handle(message, self._generation))
            self._tasks.add(task)
            task.add_done_callback(self._tasks.discard)

    def _free(self, npc: str) -> bool:
        body_owned = getattr(self.link, "body_owned", lambda _: False)
        if body_owned(npc):
            return False
        entry = self.roster.get(npc.upper())
        # The held message itself proves freedom at interception. A newer roster can revoke it.
        return entry is None or entry.activity == "routine"

    async def _person(self, npc: str) -> None:
        npc = npc.upper()
        person = self.crimes.people.get(npc, {})
        if "guild" in person and "npctype" in person:
            return
        values = await self.link.story_eval(
            npc, [["field", ["inst", "SELF"], "GUILD"], ["field", ["inst", "SELF"], "NPCTYPE"]]
        )
        if len(values) != 2 or any(type(v) is not int for v in values):
            raise _Fallback("unknown_role")
        self.crimes.people[npc] = {
            **self.crimes.people.get(npc, {}), "guild": values[0], "npctype": values[1],
        }

    async def _guards(self, npc: str, crime: Fact) -> list[str]:
        if not crime.get("community"):
            return []
        nearby = await self.link.story_nearby(npc, SHOUT_RANGE_CM)
        guards = []
        for person in sorted(nearby, key=lambda p: p.get("distance", SHOUT_RANGE_CM + 1)):
            guard = str(person.get("npc", "")).upper()
            if not guard or person.get("distance", SHOUT_RANGE_CM + 1) > SHOUT_RANGE_CM:
                continue
            if self.crimes.side(guard, crime["id"]) is not None:
                continue
            # Only possible guards need a VM read to distinguish city from OldCamp membership.
            metadata = {**self.crimes.people.get(guard, {}), **person}
            if not self.law.is_guard(guard, crime.get("community"), metadata):
                guild = int(metadata.get("guild", -1))
                if guild not in {1, 2, 4, 7}:
                    continue
                await self._person(guard)
                metadata = {**self.crimes.people.get(guard, {}), **person}
            if self.law.is_guard(guard, crime.get("community"), metadata):
                # Farm/monastery guards can be identified from the nearby guild alone, before
                # prepare() has filled the world cache. called() uses that shared identity too.
                self.crimes.people[guard] = metadata
                guards.append(guard)
        return guards

    async def _act(self, npc: str, action: str, /, **args: Any) -> None:
        if not self._free(npc):
            raise _Fallback("busy")
        action_id = await self.link.npc_act(npc, action, **args)  # type: ignore[arg-type]
        while action_id not in self._receipts:
            self._receipt_wake.clear()
            await self._receipt_wake.wait()
        receipt = self._receipts.pop(action_id)
        if receipt.status != "queued":
            raise _Fallback("action_rejected:" + (receipt.detail or action))

    async def _handle(self, held: PerceptionHeld, generation: int) -> None:
        verdict, reason = "scripts", "missing_crime"
        role: Role | None = None
        reaction: Reaction | None = None
        npc = held.npc.upper()
        crime = None
        fighter: str | None = None
        try:
            async with asyncio.timeout(HOLD_SECONDS):
                if not held.deed:
                    raise _Fallback("missing_deed")
                while (crime := self.crimes.crime(held.deed or "")) is None:
                    if self.crimes.classified_noncrime(held.deed):
                        raise _Fallback("noncrime")
                    self._fact_wake.clear()
                    await self._fact_wake.wait()
                if held.other.upper() not in HERO or crime.get("method") == "pickpocket":
                    raise _Fallback("unsupported_deed")
                role = self.role(npc, crime)
                if role != "ally":
                    if not self._free(npc):
                        raise _Fallback("busy")
                    await self._person(npc)
                    role = self.role(npc, crime)
                if role is None:
                    raise _Fallback("unknown_role")
                card = self.cards.get(npc)
                courage, honesty = (card.courage, card.honesty) if card is not None else (3, 3)
                guards = []
                if crime.get("kind") == "theft" and (
                    (role == "owner" and courage < OWNER_FIGHT_COURAGE)
                    or (role == "member" and honesty >= REPORT_HONESTY)
                ):
                    guards = await self._guards(npc, crime)
                reaction = reaction_for(
                    crime.get("kind", ""), role, courage=courage, honesty=honesty, guards=bool(guards)
                )
                if reaction is None:
                    raise _Fallback("no_rule")
                if self.project is not None and role != "ally":
                    await self.project(npc)
                if reaction.svm and reaction.kind not in {"fight", "flee"}:
                    await self._act(npc, "say_svm", svm=reaction.svm, overlay=True)
                voice = {"svm": reaction.svm} if reaction.svm else {}
                if reaction.kind == "fight":
                    # Native startState clears queued speech. The bridge's existing personal SVM
                    # argument speaks after the state starts, preserving the witness's own voice.
                    await self._act(npc, "fight", npc="PLAYER", reason=reaction.fight_reason, **voice)
                    fighter = npc
                elif reaction.kind == "flee":
                    await self._act(npc, "flee", npc="PLAYER", **voice)
                elif reaction.kind == "call_guards":
                    self.law.called(crime["id"], npc, guards)
                    guard = next((guard for guard in guards if self._free(guard)), None)
                    if guard:
                        await self._act(guard, "fight", npc="PLAYER", reason="AR_GUARDCALLEDTOTHIEF")
                        fighter = guard
                if role != "ally" and not self._free(npc) and reaction.kind not in {"fight", "flee"}:
                    raise _Fallback("busy")
                verdict, reason = "server", f"{crime['kind']}:{role}"
        except _Fallback as error:
            reason = str(error)
        except TimeoutError:
            reason = "missing_crime" if crime is None else "deadline"
        except (GameError, ConnectionError) as error:
            reason = "game_unavailable"
            log.debug("crime reaction %s: %s", held.hold, error)
        except asyncio.CancelledError:
            reason = "closed" if self._closed else "world_changed"
        except Exception:
            reason = "rule_error"
            log.exception("crime reaction %s", held.hold)
        if generation != self._generation:
            return  # the old world and its held perceptions have already been discarded
        fact: Fact = {
            "fact": "crime_reaction", "crime": held.deed, "npc": npc, "hold": held.hold,
            "role": role, "reaction": reaction.kind if verdict == "server" and reaction else "fallback",
            "verdict": verdict, "reason": reason,
        }
        if reaction is not None and reaction.svm:
            fact["svm"] = reaction.svm
        self.history.append(fact)
        self.crimes.chronicle.record(fact)
        try:
            await self.link.send(HoldVerdictCmd(hold=held.hold, to=verdict))  # type: ignore[arg-type]
        except (GameError, ConnectionError):
            pass  # disconnect returns all holds to vanilla in the bridge
        if verdict == "server" and crime is not None and fighter is not None:
            self._track_reclaim(crime, fighter, generation)

    def _track_reclaim(self, crime: Fact, fighter: str, generation: int) -> None:
        """Vanilla AR_THEFT loots gold, not the stolen cup. Recover only the recorded goods."""
        watcher = (generation, crime["id"], fighter)
        if (
            crime.get("kind") != "theft"
            or not crime.get("owner")
            or not crime.get("item")
            or crime.get("count", 0) <= 0
            or watcher in self._reclaiming
            or (generation, crime["id"]) in self._restitution_started
            or any(f.get("crime") == crime["id"] for f in self.crimes.chronicle.facts("crime_restitution"))
        ):
            return
        self._reclaiming.add(watcher)
        task = asyncio.create_task(self._reclaim(crime, fighter, generation))
        self._tasks.add(task)
        task.add_done_callback(self._tasks.discard)

    async def _reclaim(self, crime: Fact, fighter: str, generation: int) -> None:
        deadline = self.clock() + RECLAIM_SECONDS
        fighting = False
        try:
            while generation == self._generation and self.clock() < deadline:
                if (generation, crime["id"]) in self._restitution_started:
                    return
                values = await self.link.story_eval(fighter, [
                    ["call", "NPC_ISINSTATE", ["inst", "OTHER"], ["sym", "ZS_UNCONSCIOUS"]],
                    ["call", "NPC_ISDEAD", ["inst", "OTHER"]],
                    ["call", "NPC_ISDEAD", ["inst", "SELF"]],
                    ["call", "NPC_HASITEMS", ["inst", "OTHER"], ["sym", crime["item"]]],
                    ["call", "NPC_ISINSTATE", ["inst", "SELF"], ["sym", "ZS_ATTACK"]],
                    ["call", "NPC_ISINSTATE", ["inst", "SELF"], ["sym", "ZS_RANSACKBODY"]],
                ])
                if generation != self._generation:
                    return
                if len(values) != 6 or any(type(v) is not int for v in values):
                    return
                unconscious, hero_dead, fighter_dead, count, attacking, ransacking = values
                if hero_dead or fighter_dead:
                    return
                if unconscious and (attacking or ransacking):
                    amount = min(int(crime["count"]), count)
                    if amount <= 0:
                        return
                    restitution = (generation, crime["id"])
                    if restitution in self._restitution_started:
                        return
                    # Several witnesses can win the same fight. Claim this case before yielding
                    # to the transfer, including when its acknowledgement later gets lost.
                    self._restitution_started.add(restitution)
                    # check=True rechecks the giver's inventory in the engine before a real transfer.
                    await self.link.story_effect(crime["owner"], [{
                        "do": "give", "item": crime["item"], "count": amount, "to": "self",
                    }], check=True)
                    if generation == self._generation:
                        self.crimes.chronicle.record({
                            "fact": "crime_restitution", "crime": crime["id"], "npc": crime["owner"],
                            "fighter": fighter, "item": crime["item"], "count": amount,
                            "how": "defeated_thief",
                        })
                    return
                if fighting and not attacking:
                    return  # the tracked fight ended: a later, unrelated defeat is not restitution
                fighting = fighting or bool(attacking)
                await asyncio.sleep(0.25)
        except (GameError, TimeoutError, ConnectionError) as error:
            log.debug("stolen goods %s: %s", crime["id"], error)
        finally:
            self._reclaiming.discard((generation, crime["id"], fighter))
