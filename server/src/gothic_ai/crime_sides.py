"""A witness's side in a killing, decided before knowledge can trigger a reaction."""

from __future__ import annotations

import logging
import time
from collections.abc import Callable
from typing import TYPE_CHECKING, Literal

from .chronicle import Fact
from .game import GameError
from .protocol import GameMessage, HeroDeed, Loading, NpcEvent, NpcViews, Perception, Session

if TYPE_CHECKING:
    from .crimes import Crimes
    from .executor import Intent
    from .orders import Orders
    from .questbook import QuestBook

FIGHT_MEMORY_SECONDS = 600.0
SideReason = Literal["fought", "companion", "hostile", "quest"]
log = logging.getLogger(__name__)


class CrimeSides:
    def __init__(self, crimes: Crimes) -> None:
        self.crimes = crimes
        self.orders: Orders | None = None
        self.book: QuestBook | None = None
        self.clock: Callable[[], float] = time.monotonic
        # Directed: who attacked or threatened whom. A companion must not blame the hero's target
        # merely because the hero attacked it; rule 2 needs the victim to have attacked the group.
        self.fights: dict[tuple[str, str], float] = {}
        self.group_threats: dict[str, float] = {}  # attackers of the hero's group when the signal arrived
        self.hostile_guilds: set[tuple[int, int]] = set()
        self._prepared = False
        self._generation = 0

    def _npc(self, npc: str) -> str:
        npc = self.crimes._npc(npc)
        return "PLAYER" if npc in {"PLAYER", "HERO", "PC_HERO"} else npc

    def _prune(self) -> None:
        cutoff = self.clock() - FIGHT_MEMORY_SECONDS
        self.fights = {pair: at for pair, at in self.fights.items() if at >= cutoff}
        self.group_threats = {npc: at for npc, at in self.group_threats.items() if at >= cutoff}

    def note_fight(self, npc: str, target: str) -> None:
        self._prune()
        npc, target = self._npc(npc), self._npc(target)
        if npc and target and npc != target:
            self.fights[npc, target] = self.clock()
            friends = {npc for npc, person in self.crimes.people.items() if person.get("npctype") == 2}
            if target == "PLAYER" or self._companion(target, friends):
                self.group_threats[npc] = self.clock()

    def note_intent(self, npc: str, intent: Intent) -> None:
        if intent.goal == "fight" and intent.target:
            self.note_fight(npc, intent.target)

    def on_message(self, message: GameMessage) -> None:
        if isinstance(message, (Loading, Session)):
            self.fights.clear()
            self.group_threats.clear()
            self._prepared = False
            self._generation += 1
        elif isinstance(message, NpcEvent):
            if message.kind in {"npc_attack", "npc_threat"}:
                self.note_fight(message.other, message.npc)
            elif message.kind == "hero_attacked":
                self.note_fight(message.other, "PLAYER")
        elif isinstance(message, Perception) and message.perc.upper() == "ASSESSDAMAGE":
            self.note_fight(message.other, message.npc)
        elif isinstance(message, NpcViews):
            for view in message.views:
                for actor in [*view.sees, *([view.hero] if view.hero is not None else [])]:
                    if actor.combat_target:
                        self.note_fight(actor.id, actor.combat_target)

    async def prepare(self) -> None:
        """Read the world's guild matrix once; keep the last reading until a replacement is ready.

        The window without a matrix lasts only until the process's first successful reading.
        """
        if self._prepared:
            return
        generation = self._generation
        values = await self.crimes.link.story_eval("PC_HERO", [["var", "GIL_MAX"], ["var", "ATT_HOSTILE"]])
        if generation != self._generation:
            return
        if len(values) != 2 or not all(isinstance(value, int) for value in values) or values[0] <= 0:
            raise GameError("crime sides: guild constants unavailable")
        maximum, hostile = values
        pairs = [(first, second) for first in range(maximum) for second in range(maximum)]
        attitudes = await self.crimes.link.story_eval("PC_HERO", [
            ["call", "WLD_GETGUILDATTITUDE", ["int", first], ["int", second]]
            for first, second in pairs
        ])
        if generation != self._generation:
            return
        if len(attitudes) != len(pairs) or not all(isinstance(value, int) for value in attitudes):
            raise GameError("crime sides: guild attitudes unavailable")
        self.hostile_guilds = {
            pair for pair, attitude in zip(pairs, attitudes, strict=True) if attitude == hostile
        }
        self._prepared = True
        log.info("crime sides: guilds=%d hostile_pairs=%d", maximum, len(self.hostile_guilds))

    def _companion(self, npc: str, friends: set[str]) -> bool:
        if npc in friends:
            return True
        if self.orders is not None:
            standing = self.orders.standing.get(npc)
            if standing is not None and standing.kind == "follow":
                return True
        dialogue = self.crimes.dialogue
        conv = dialogue.conversations.get(npc) if dialogue is not None else None
        return conv is not None and conv.intent.goal == "follow_hero"

    def reason(self, npc: str, deed: HeroDeed) -> SideReason | None:
        """Keep the bridge's witness metadata and combat snapshot for direct witnesses."""
        npc, victim = self._npc(npc), self._npc(deed.victim)
        friends = {self._npc(p.npc) for p in deed.witnesses_metadata if p.npctype == 2}
        metadata = next((p for p in deed.witnesses_metadata if self._npc(p.npc) == npc), None)
        guild = metadata.guild if metadata is not None else self.crimes.people.get(npc, {}).get("guild", -1)
        return self._reason(npc, victim, friends, guild, deed.victim_guild, deed.victim_enemy)

    def heard_reason(self, npc: str, crime: Fact) -> SideReason | None:
        """A listener can have its own side, independent of the teller's version."""
        npc = self._npc(npc)
        victim = self._npc(crime.get("victim", ""))
        people = self.crimes.people
        friends = {self._npc(actor) for actor, person in people.items() if person.get("npctype") == 2}
        guild = people.get(npc, {}).get("guild", -1)
        victim_guild = crime.get("victim_guild")
        if victim_guild is None or victim_guild < 0:
            victim_guild = people.get(victim, {}).get("guild")
        if victim_guild is not None and victim_guild < 0:
            victim_guild = None
        return self._reason(npc, victim, friends, guild, victim_guild)

    def _reason(
        self,
        npc: str,
        victim: str,
        friends: set[str],
        guild: int,
        victim_guild: int | None,
        victim_enemy: bool = False,
    ) -> SideReason | None:
        dialogue = self.crimes.dialogue
        conversations = dialogue.conversations if dialogue is not None else {}
        for actor, conv in conversations.items():
            self.note_intent(actor, conv.intent)
        if self.orders is not None:
            for actor, fight in self.orders.fights.items():
                self.note_fight(actor, fight.target)
        self._prune()
        if (npc, victim) in self.fights or (victim, npc) in self.fights:
            return "fought"
        if self._companion(npc, friends):
            group = {"PLAYER", *friends}
            candidates = set(conversations)
            if self.orders is not None:
                candidates.update(self.orders.standing)
            group.update(actor for actor in candidates if self._companion(actor, friends))
            if (
                victim_enemy
                or victim in self.group_threats
                or any((victim, member) in self.fights for member in group)
            ):
                return "companion"
        if victim_guild is not None and (guild, victim_guild) in self.hostile_guilds:
            return "hostile"
        if self.book is not None:
            for quest in self.book.all().values():
                if self._npc(quest.giver) != npc:
                    continue
                progress = self.book.progress(quest.id)
                if progress.state != "given":
                    continue
                if any(
                    goal.kind == "kill" and victim in {self._npc(target) for target in goal.npcs}
                    for stage in quest.stages
                    for goal in stage.goals
                ):
                    return "quest"
        return None
