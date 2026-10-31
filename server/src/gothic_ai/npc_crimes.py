"""Server-started physical NPC deeds, with evidence separate from every view of the hero.

The bridge marks only explicit commands and supplies its actual perception witnesses. These deeds
never enter hero law, held perceptions or hero attitudes. PeopleKnowledge/SOURCES owns their news.
"""

from __future__ import annotations

import asyncio
import logging
import time
from collections.abc import Callable, Mapping, MutableMapping
from typing import Any

from .characters import Card
from .chronicle import Chronicle, Fact
from .crime_law import CrimeLaw
from .crime_react import OWNER_FIGHT_COURAGE, RECLAIM_SECONDS, SHOUT_RANGE_CM
from .crimes import HERO, SUSPECT_MIN, Crimes
from .game import GameError, GameLink
from .protocol import GameMessage, Loading, NpcActResult, NpcDeed, NpcIdle, NpcRoster, RosterEntry, Session
from .storyguard import StoryGuard, asleep

log = logging.getLogger(__name__)
ACTION_SECONDS = 5.0
HUMAN_GUILD_LIMIT = 16  # Npc::isHuman(): guild < GIL_SEPERATOR_HUM


class NpcCrimes:
    def __init__(
        self, link: GameLink, chronicle: Chronicle, *, crimes: Crimes | None,
        cards: Mapping[str, Card] | None = None, guard: StoryGuard | None = None,
        enabled: bool = True, clock: Callable[[], float] = time.monotonic,
    ) -> None:
        self.link, self.chronicle, self.crimes = link, chronicle, crimes
        self.cards = cards if cards is not None else {}
        self.guard = guard or StoryGuard(None)
        self.enabled = enabled and crimes is not None
        self.law = CrimeLaw(crimes) if crimes is not None else None
        self.clock = clock
        self.roster: dict[str, RosterEntry] = {}
        self._generation = 0
        self._tasks: set[asyncio.Task[Any]] = set()
        self._receipts: dict[str, NpcActResult] = {}
        self._completed: dict[str, NpcIdle] = {}
        self._receipt_wake = asyncio.Event()
        self._reclaiming: set[tuple[int, str, str]] = set()
        self._restitution_started: set[tuple[int, str]] = set()
        link.listeners.append(self._on_message)

    async def close(self) -> None:
        if self._on_message in self.link.listeners:
            self.link.listeners.remove(self._on_message)
        for task in tuple(self._tasks):
            task.cancel()
        if self._tasks:
            await asyncio.gather(*tuple(self._tasks), return_exceptions=True)

    def _spawn(self, task: Any) -> None:
        pending = asyncio.create_task(task)
        self._tasks.add(pending)
        pending.add_done_callback(self._tasks.discard)

    def facts(self) -> list[Fact]:
        return self.chronicle.facts("npc_crime")

    def crime(self, crime: str) -> Fact | None:
        return next((f for f in self.facts() if f.get("id") == crime), None)

    def settled(self, crime: str) -> bool:
        return any(f.get("crime") == crime for f in self.chronicle.facts("npc_crime_settled"))

    def seen(self, npc: str) -> list[Fact]:
        ids = {
            f.get("crime") for f in self.chronicle.facts("npc_crime_seen")
            if f.get("npc") == npc.upper()
        }
        return [f for f in self.facts() if f.get("id") in ids]

    def suspect_chest(
        self, npc: str, chest: str, missing: Mapping[str, int], *, waypoint: str = "",
    ) -> list[Fact]:
        """A remembered presence explains a loss, without identifying the real thief.

        NPC deeds keep their own namespace. Suspecting the hero in one of these cases is deferred
        until that namespace has a bridge to hero accusations; it must never create a hero case.
        """
        crimes = self.crimes
        if not self.enabled or crimes is None or crimes.sightings is None:
            return []
        npc = npc.upper()
        out = []
        for crime in self.facts():
            if (
                crime.get("kind") != "theft" or crime.get("chest") != chest
                or crime.get("owner") != npc or crime.get("item") not in missing
            ):
                continue
            at = int(crime["day"]) * 1440 + int(crime["hour"]) * 60 + int(crime["minute"])
            place = crimes.places.of(crime.get("waypoint") or waypoint) if crimes.places else None
            if place is None:
                continue
            for sight in crimes.sightings.mind(npc).sightings:
                if (
                    sight.trace_kind or sight.who in HERO or sight.who == npc
                    or sight.end < at - SUSPECT_MIN or sight.start > at + SUSPECT_MIN
                    or place.id not in sight.place_ids
                ):
                    continue
                if any(
                    f.get("npc") == npc and f.get("crime") == crime["id"]
                    and f.get("suspect") == sight.who
                    for f in self.chronicle.facts("npc_crime_suspected")
                ):
                    continue
                fact = {
                    "fact": "npc_crime_suspected", "npc": npc, "crime": crime["id"],
                    "suspect": sight.who, "chest": chest, "sighting": sight.record(),
                }
                self.chronicle.record(fact)
                out.append(fact)
        return out

    def _human(self, npc: str, guild: int | None = None) -> bool | None:
        if guild is None and self.crimes is not None:
            guild = self.crimes.people.get(npc.upper(), {}).get("guild")
        if guild is None:
            entry = self.roster.get(npc.upper())
            if entry is not None and "guild" in entry.model_fields_set:
                guild = entry.guild
        return guild < HUMAN_GUILD_LIMIT if type(guild) is int and guild >= 0 else None

    async def _human_witness(self, npc: str) -> bool:
        """The live guild proves a speaker is human; unknown or unavailable metadata never speaks."""
        if (human := self._human(npc)) is not None:
            return human
        generation = self._generation
        try:
            values = await self.link.story_eval(npc, [["field", ["inst", "SELF"], "GUILD"]])
        except (GameError, TimeoutError, ConnectionError):
            return False
        if (
            generation != self._generation or len(values) != 1
            or type(values[0]) is not int or values[0] < 0
        ):
            return False
        if self.crimes is not None:
            self.crimes.people[npc.upper()] = {
                **self.crimes.people.get(npc.upper(), {}), "guild": values[0],
            }
        return values[0] < HUMAN_GUILD_LIMIT

    async def check_command(self, npc: str, action: str, args: Mapping[str, Any]) -> str | None:
        """Called before sending even a raw npc_act command; a refusal changes no world state."""
        marked = args.get("deed") is True
        if action != "steal" and not (action == "fight" and marked):
            return None
        if not self.enabled:
            return "дела NPC выключены"
        if not marked:
            return "кража требует deed: true"
        generation = self._generation
        npc = npc.upper()
        entry = self.roster.get(npc)
        actor_state = entry.state if entry is not None else ""
        if reason := self.guard.harm(npc, state=actor_state):
            return reason
        target, target_state, item, mission = str(args.get("npc") or "").upper(), "", "", False
        if action == "steal":
            target_data = await self.link.story_state(deed_target={**dict(args), "npc": npc})
            if generation != self._generation:
                return "мир сменился во время проверки дела"
            resolved = target_data.get("deed_target")
            if not isinstance(resolved, dict):
                return "не удалось проверить владельца и вещь"
            if resolved.get("error"):
                return str(resolved["error"])
            if not resolved.get("item"):
                return "не удалось проверить владельца и вещь"
            target = str(resolved.get("owner") or "").upper()
            target_state = str(resolved.get("owner_state") or "")
            item, mission = str(resolved["item"]), bool(resolved.get("mission"))
            if isinstance(args, MutableMapping):
                args["expected_owner"] = target
                args["expected_item"] = item
        entry = self.roster.get(target)
        if entry is not None:
            target_state = entry.state
        if not target:
            return "не указан пострадавший или владелец"
        if target == npc:
            return "нельзя вредить самому себе"
        return self.guard.harm(
            npc, target=target, state=actor_state, target_state=target_state, item=item, mission=mission,
        )

    def record(self, deed: NpcDeed, *, far: bool = False) -> Fact | None:
        """One truth and one discovery per actual witness, in the current save branch only."""
        crimes = self.crimes
        if not self.enabled or crimes is None or self.chronicle.branch is None or self.crime(deed.id):
            return None
        culprit = crimes._npc(deed.culprit)
        if not culprit or culprit in HERO:
            return None
        victim, owner, community = crimes._npc(deed.victim), "", None
        item, count = deed.item.upper(), deed.count
        if deed.kind == "take_item":
            if not item or count <= 0:
                return None
            ownership = crimes._ownership(deed, culprit=culprit)
            if ownership is None:
                return None
            owner, guild, npctype = ownership
            victim = owner
            community = crimes.community(guild, npctype)
            kind = "theft"
        elif deed.kind == "attack":
            if (
                not victim or victim == culprit or victim in HERO
                or deed.victim_is_monster or deed.victim_guild > 16
            ):
                return None
            community = crimes.community(deed.victim_guild, deed.victim_npctype)
            kind = "attack"
        else:
            return None
        metadata = {crimes._npc(person.npc): person for person in deed.witnesses_metadata}
        witnesses = list(dict.fromkeys(
            npc for raw in deed.witnesses if raw
            if (npc := crimes._npc(raw)) not in {culprit, crimes._npc(deed.victim), *HERO}
            # Old snapshots can omit metadata: keep the bridge's observation but prove the live
            # guild before allowing a voice or a body reaction. Current bridges supply every guild.
            if self._human(npc, metadata[npc].guild if npc in metadata else None) is not False
        ))
        for npc, person in metadata.items():
            crimes.people[npc] = {
                **crimes.people.get(npc, {}), "guild": person.guild, "npctype": person.npctype,
            }
        for npc, guild, npctype in (
            (culprit, deed.culprit_guild, deed.culprit_npctype),
            (victim, deed.victim_guild, deed.victim_npctype),
            (deed.owner, deed.owner_npc_guild, deed.owner_npctype),
            (deed.chest_owner, deed.chest_owner_guild, deed.chest_owner_npctype),
            (deed.room_owner, deed.room_owner_guild, deed.room_owner_npctype),
        ):
            if npc and guild >= 0:
                npc = crimes._npc(npc)
                crimes.people[npc] = {**crimes.people.get(npc, {}), "guild": guild, "npctype": npctype}
        fact: Fact = {
            "fact": "npc_crime", "id": deed.id, "culprit": culprit, "kind": kind,
            "victim": victim, "owner": owner, "community": community, "item": item, "count": count,
            "place": deed.position.model_dump(), "waypoint": deed.waypoint,
            "day": deed.day, "hour": deed.hour, "minute": deed.minute, "witnesses": witnesses,
            **({"far": True} if far else {}),
            **({"chest": deed.chest} if deed.chest else {}),
        }
        self.chronicle.record(fact)
        for npc in witnesses:
            self.chronicle.record({
                "fact": "npc_crime_seen", "npc": npc, "crime": deed.id,
                "culprit_name": crimes.names(culprit), "victim_name": crimes.names(victim),
                "name": crimes.names(npc),
            })
        return fact

    def _on_message(self, message: GameMessage) -> None:
        if isinstance(message, (Loading, Session)):
            self._generation += 1
            self.roster.clear()
            self._receipts.clear()
            self._completed.clear()
            self._reclaiming.clear()
            self._restitution_started.clear()
            for task in tuple(self._tasks):
                task.cancel()
        elif isinstance(message, NpcRoster):
            if message.full:
                self.roster.clear()
            self.roster.update((entry.id.upper(), entry) for entry in message.npcs)
        elif isinstance(message, NpcActResult):
            self._receipts[message.id] = message
            if len(self._receipts) > 256:
                self._receipts.pop(next(iter(self._receipts)))
            self._receipt_wake.set()
        elif isinstance(message, NpcIdle) and message.act:
            self._completed[message.act] = message
            if len(self._completed) > 256:
                self._completed.pop(next(iter(self._completed)))
        elif isinstance(message, NpcDeed):
            if crime := self.record(message):
                self._spawn(self._react(crime, self._generation))

    def _free(self, npc: str) -> bool:
        if getattr(self.link, "body_owned", lambda _: False)(npc):
            return False
        entry = self.roster.get(npc.upper())
        return entry is None or (entry.activity == "routine" and not asleep(entry.state))

    async def _act(self, npc: str, action: str, /, **args: Any) -> str:
        if (action == "say_svm" or args.get("svm")) and not await self._human_witness(npc):
            raise GameError(f"not a human speaker: {npc}")
        if not self._free(npc):
            raise GameError(f"busy: {npc}")
        async with asyncio.timeout(ACTION_SECONDS):
            action_id = await self.link.npc_act(npc, action, **args)  # type: ignore[arg-type]
            while action_id not in self._receipts:
                self._receipt_wake.clear()
                await self._receipt_wake.wait()
            receipt = self._receipts.pop(action_id)
        if receipt.status != "queued":
            raise GameError(receipt.detail or "action rejected")
        return action_id

    def _heard(self, crime: Fact, witness: str, guard: str) -> None:
        """A guard hears a nearby cry, never acquires a fictitious visual observation."""
        if self.crimes is None or guard in crime["witnesses"]:
            return
        key = f"npc_crime_seen:{crime['id']}"
        if any(
            f.get("quest") == key and f.get("to") == guard and f.get("subject") == crime["culprit"]
            for f in self.chronicle.facts("rumor")
        ):
            return
        names = self.crimes.names
        self.chronicle.record({
            "fact": "rumor", "quest": key, "kind": "npc_" + crime["kind"],
            "subject": crime["culprit"], "subject_name": names(crime["culprit"]),
            "about": crime["victim"], "about_name": names(crime["victim"]),
            "from": witness, "from_name": names(witness), "to": guard, "hops": 1,
            "how": "shout", "day": crime["day"],
        })

    async def _react(self, crime: Fact, generation: int) -> None:
        if self.settled(crime["id"]):
            return
        fighters: set[str] = set()
        svm = "DIRTYTHIEF" if crime["kind"] == "theft" else "HELP"
        for witness in crime["witnesses"]:
            if generation != self._generation or not self.enabled or self.settled(crime["id"]):
                return
            if witness in fighters or not self._free(witness) or not await self._human_witness(witness):
                continue
            try:
                card = self.cards.get(witness)
                courage = card.courage if card is not None else 3
                owner_fights = (
                    crime["kind"] == "theft" and witness == crime["owner"]
                    and courage >= OWNER_FIGHT_COURAGE
                )
                guard_fights = self.law is not None and self.law.is_guard(witness, crime["community"])
                if owner_fights or guard_fights:
                    if self._law_started(crime["id"]):
                        continue
                    # Starting ZS_ATTACK clears queued speech. Pass a fighting witness's own SVM
                    # through the same native state-start path as the hero's on-scene reactions.
                    await self._fight(
                        crime, witness, generation, role="owner" if owner_fights else "guard", svm=svm,
                    )
                    fighters.add(witness)
                else:
                    await self._act(witness, "say_svm", svm=svm, overlay=True)
                if generation != self._generation:
                    return
                self.chronicle.record({
                    "fact": "npc_crime_reaction", "crime": crime["id"], "npc": witness,
                    "reaction": "shout", "svm": svm,
                })
                nearby = await self.link.story_nearby(witness, SHOUT_RANGE_CM)
                nearby.insert(0, {"npc": witness, "distance": 0})
                for person in nearby:
                    guard = str(person.get("npc") or "").upper()
                    if (
                        not guard or guard == crime["culprit"] or guard in fighters
                        or person.get("distance", SHOUT_RANGE_CM + 1) > SHOUT_RANGE_CM
                        or self.law is None
                    ):
                        continue
                    if not self.law.is_guard(guard, crime["community"], person):
                        if int(person.get("guild", -1)) not in {1, 2, 4, 7} or self.crimes is None:
                            continue
                        values = await self.link.story_eval(guard, [
                            ["field", ["inst", "SELF"], "GUILD"],
                            ["field", ["inst", "SELF"], "NPCTYPE"],
                        ])
                        if len(values) != 2 or any(type(v) is not int for v in values):
                            continue
                        self.crimes.people[guard] = {
                            **self.crimes.people.get(guard, {}), "guild": values[0], "npctype": values[1],
                        }
                        if not self.law.is_guard(guard, crime["community"]):
                            continue
                    if self.settled(crime["id"]) or self._law_started(crime["id"]):
                        continue
                    if not self._free(guard):
                        continue
                    if self.guard.body(guard, str(person.get("state") or "")):
                        continue
                    guard_svm = svm if guard in crime["witnesses"] else ""
                    await self._fight(crime, guard, generation, role="guard", svm=guard_svm)
                    fighters.add(guard)
                    if guard_svm:
                        self.chronicle.record({
                            "fact": "npc_crime_reaction", "crime": crime["id"], "npc": guard,
                            "reaction": "shout", "svm": svm,
                        })
                    self._heard(crime, witness, guard)
            except (GameError, TimeoutError, ConnectionError) as error:
                log.debug("NPC crime %s reaction of %s: %s", crime["id"], witness, error)

    def _law_started(self, crime: str) -> bool:
        return any(
            f.get("crime") == crime and f.get("reaction") == "fight" and f.get("role") == "law"
            for f in self.chronicle.facts("npc_crime_reaction")
        )

    async def _fight(self, crime: Fact, npc: str, generation: int, *, role: str, svm: str = "") -> str:
        if self.settled(crime["id"]):
            raise GameError("дело NPC уже завершено")
        actor, target = self.roster.get(npc), self.roster.get(crime["culprit"])
        if reason := self.guard.harm(
            npc, target=crime["culprit"], state=actor.state if actor else "",
            target_state=target.state if target else "",
        ):
            raise GameError(reason)
        action_id = await self._act(
            npc, "fight", npc=crime["culprit"], reason="AR_NONE", deed=False, nonlethal=True,
            **({"svm": svm} if svm else {}),
        )
        if generation != self._generation:
            raise GameError("мир сменился во время защиты")
        self.chronicle.record({
            "fact": "npc_crime_reaction", "crime": crime["id"], "npc": npc,
            "culprit": crime["culprit"],
            "reaction": "fight", "role": role, "act": action_id,
            **({"blamed": crime["blamed"]} if crime.get("blamed") else {}),
        })
        self._track_reclaim(crime, npc, generation, action_id)
        return action_id

    def _track_reclaim(self, crime: Fact, fighter: str, generation: int, action_id: str = "") -> None:
        key = (generation, crime["id"], fighter)
        if (
            crime["kind"] != "theft" or not crime["owner"] or not crime["item"]
            or crime["count"] <= 0 or key in self._reclaiming
            or (generation, crime["id"]) in self._restitution_started
            or any(f.get("crime") == crime["id"] for f in self.chronicle.facts("npc_crime_restitution"))
        ):
            return
        self._reclaiming.add(key)
        self._spawn(self._reclaim(crime, fighter, generation, action_id))

    async def _reclaim(self, crime: Fact, fighter: str, generation: int, action_id: str = "") -> None:
        deadline = self.clock() + RECLAIM_SECONDS  # native scoped timeout is 120s; allow receipt reserve
        fighting = False
        outcome, reason, returned = "timeout", "deadline", 0
        culprit = ["inst", crime["culprit"]]
        try:
            while generation == self._generation and self.clock() < deadline:
                if self.settled(crime["id"]):
                    outcome, reason = "resolved_elsewhere", "case_settled"
                    return
                if (generation, crime["id"]) in self._restitution_started:
                    outcome, reason = "resolved_elsewhere", "another_defender"
                    return
                values = await self.link.story_eval(fighter, [
                    ["call", "NPC_ISINSTATE", culprit, ["sym", "ZS_UNCONSCIOUS"]],
                    ["call", "NPC_ISDEAD", culprit],
                    ["call", "NPC_ISDEAD", ["inst", "SELF"]],
                    ["call", "NPC_HASITEMS", culprit, ["sym", crime["item"]]],
                    ["call", "NPC_ISINSTATE", ["inst", "SELF"], ["sym", "ZS_ATTACK"]],
                    ["call", "NPC_ISINSTATE", ["inst", "SELF"], ["sym", "ZS_RANSACKBODY"]],
                    ["call", "NPC_ISINSTATE", ["inst", "SELF"], ["sym", "ZS_UNCONSCIOUS"]],
                ])
                if (
                    generation != self._generation or len(values) != 7
                    or any(type(v) is not int for v in values)
                ):
                    outcome, reason = "unavailable", "invalid_vm_result"
                    return
                unconscious, culprit_dead, fighter_dead, count, attacking, ransacking, fighter_down = values
                if fighter_dead:
                    outcome, reason = "defender_dead", "native_death"
                    return
                if fighter_down:
                    outcome, reason = "defender_down", "native_unconsciousness"
                    return
                if culprit_dead:
                    outcome, reason = "culprit_dead", "native_death"
                    return
                completed = self._completed.get(action_id)
                if completed is not None:
                    if completed.npc.upper() != fighter.upper():
                        outcome, reason = "ended", "foreign_receipt"
                        return
                    if completed.detail == "defender_dead":
                        outcome, reason = "defender_dead", "native_death"
                        return
                    if completed.detail == "defender_down":
                        outcome, reason = "defender_down", "native_unconsciousness"
                        return
                    if completed.detail == "target_dead":
                        outcome, reason = "culprit_dead", "native_death"
                        return
                    if completed.reason != "done":
                        outcome = "timeout" if completed.reason == "timeout" else "ended"
                        reason = completed.reason or "unknown_action_outcome"
                        return
                # The bridge ends this scoped nonlethal action on the knockout itself. Its receipt
                # proves the tracked fight ended even when ZS_ATTACK is already gone at this poll.
                native_knockout = completed is not None and completed.detail == "target_down"
                if (unconscious or native_knockout) and (attacking or ransacking or completed is not None):
                    amount = min(int(crime["count"]), count)
                    if amount <= 0:
                        outcome, reason = "no_goods", "stolen_item_missing"
                        return
                    if (generation, crime["id"]) in self._restitution_started:
                        outcome, reason = "resolved_elsewhere", "another_defender"
                        return
                    if self.settled(crime["id"]):
                        outcome, reason = "resolved_elsewhere", "case_settled"
                        return
                    self._restitution_started.add((generation, crime["id"]))
                    await self.link.story_effect(crime["culprit"], [{
                        "do": "give", "item": crime["item"], "count": amount,
                        "to": "npc", "npc": crime["owner"],
                    }], check=True)
                    if generation == self._generation:
                        self.chronicle.record({
                            "fact": "npc_crime_restitution", "crime": crime["id"],
                            "npc": crime["owner"], "culprit": crime["culprit"], "fighter": fighter,
                            "item": crime["item"], "count": amount, "how": "defeated_thief",
                            **({"blamed": crime["blamed"]} if crime.get("blamed") else {}),
                        })
                    outcome, reason, returned = "reclaimed", "defeated_thief", amount
                    return
                if completed is not None or (fighting and not attacking):
                    outcome, reason = "ended", completed.reason if completed else "left_attack_state"
                    return
                fighting = fighting or bool(attacking)
                await asyncio.sleep(0.25)
        except (GameError, TimeoutError, ConnectionError) as error:
            outcome, reason = "unavailable", "game_unavailable"
            log.warning(
                "NPC stolen goods %s (fighter %s, act %s): %s: %s",
                crime["id"], fighter, action_id, type(error).__name__, error,
            )
        except asyncio.CancelledError:
            outcome = ""
            raise
        finally:
            self._reclaiming.discard((generation, crime["id"], fighter))
            if outcome and generation == self._generation:
                self._defense_finished(crime, fighter, action_id, outcome, reason, returned)

    def _defense_finished(
        self, crime: Fact, fighter: str, action_id: str, outcome: str, reason: str, returned: int,
    ) -> None:
        """A terminal result is evidence about this defense, never evidence or a penalty for the hero."""
        if any(
            f.get("crime") == crime["id"] and f.get("fighter") == fighter and f.get("act") == action_id
            for f in self.chronicle.facts("npc_crime_defense")
        ):
            return
        self.chronicle.record({
            "fact": "npc_crime_defense", "crime": crime["id"], "culprit": crime["culprit"],
            "fighter": fighter, "act": action_id, "outcome": outcome, "reason": reason,
            "item": crime["item"], "count": crime["count"], "returned": returned,
            **({"blamed": crime["blamed"]} if crime.get("blamed") else {}),
        })
