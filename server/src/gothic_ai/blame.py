"""A victim's belief about a culprit, kept apart from the Chronicle's truth.

The conversational model chooses whom to blame. A failed recovery tests that belief; only code
knows whether the hero's accusation was true, and uses the ordinary honesty consequences.
"""

from __future__ import annotations

import asyncio
import re
from collections.abc import Coroutine
from typing import TYPE_CHECKING, Any

from .chronicle import Chronicle, Fact
from .crimes import HERO, SUSPECT_MIN, Crimes
from .journal import Journal
from .knowledge import roots
from .llm import Tool, ToolCall
from .npc_crimes import NpcCrimes
from .people_knowledge import PeopleKnowledge
from .protocol import GameMessage, Loading, Session, Snapshot
from .quests import Lie, caught
from .sightings import HEARD, Sighting
from .turn import ToolOutcome

if TYPE_CHECKING:
    from .dialogue import Conversation, DialogueManager
    from .game import GameLink

BLAME = "blame"
BLAME_TOOL = Tool(
    BLAME,
    "Счесть человека виновным в деле против тебя: who — имя из обстановки, crime — id дела. "
    "Героя этим инструментом не обвиняют.",
    {
        "type": "object",
        "properties": {
            "who": {"type": "string"},
            "crime": {"type": "string"},
        },
        "required": ["who", "crime"],
        "additionalProperties": False,
    },
)


class Blame:
    def __init__(
        self, link: GameLink, chronicle: Chronicle, crimes: Crimes, npc_crimes: NpcCrimes,
        dialogue: DialogueManager, *, journal: Journal | None = None,
    ) -> None:
        self.link, self.chronicle = link, chronicle
        self.crimes, self.npc_crimes, self.dialogue = crimes, npc_crimes, dialogue
        self.journal = journal or Journal()
        self.people = PeopleKnowledge(chronicle)
        self._tasks: set[asyncio.Task[Any]] = set()
        self._resolving: set[str] = set()
        self._loading = False
        crimes.blame = self
        chronicle.on_fact.append(self._on_fact)
        link.listeners.append(self._on_message)

    @property
    def enabled(self) -> bool:
        return self.npc_crimes.enabled

    async def close(self) -> None:
        if self._on_fact in self.chronicle.on_fact:
            self.chronicle.on_fact.remove(self._on_fact)
        if self._on_message in self.link.listeners:
            self.link.listeners.remove(self._on_message)
        if self.crimes.blame is self:
            self.crimes.blame = None
        for task in tuple(self._tasks):
            task.cancel()
        if self._tasks:
            await asyncio.gather(*tuple(self._tasks), return_exceptions=True)

    def _spawn(self, coro: Coroutine[Any, Any, None]) -> None:
        task = asyncio.create_task(coro)
        self._tasks.add(task)
        task.add_done_callback(self._tasks.discard)

    def beliefs(self, npc: str | None = None) -> list[Fact]:
        if not self.enabled:
            return []
        latest: dict[tuple[str, str], Fact] = {}
        for fact in self.chronicle.facts():
            key = (fact.get("npc", ""), fact.get("crime", ""))
            if fact.get("fact") == "blamed":
                latest[key] = fact
            elif fact.get("fact") == "blame_refuted":
                previous = latest.get(key)
                if previous is not None and self._same(previous, fact):
                    latest.pop(key)
        return [f for f in latest.values() if npc is None or f["npc"] == npc.upper()]

    def active(self, npc: str, crime: str) -> Fact | None:
        return next((f for f in self.beliefs(npc) if f["crime"] == crime), None)

    @staticmethod
    def _same(belief: Fact, fact: Fact) -> bool:
        return bool(
            fact.get("crime") == belief["crime"] and fact.get("culprit") == belief["culprit"]
            and (not fact.get("blamed") or fact["blamed"] == belief["id"])
        )

    def crime_for(self, belief: Fact) -> Fact | None:
        original = self.crimes.crime(belief["crime"]) or self.npc_crimes.crime(belief["crime"])
        if original is None:
            return None
        return {**original, "culprit": belief["culprit"], "blamed": belief["id"]}

    def _eligible(self, npc: str) -> list[Fact]:
        if not self.enabled:
            return []
        npc = npc.upper()
        known = {k.crime["id"] for k in self.crimes.known(npc)}
        known.update(f["id"] for f in self.npc_crimes.seen(npc))
        known.update(f.get("crime") for f in self.chronicle.facts("npc_crime_suspected")
                     if f.get("npc") == npc)
        known.update(c for f in self.chronicle.facts("testimony") if f.get("npc") == npc
                     and isinstance(f.get("about"), str) and f["about"].upper() not in HERO
                     for c in f.get("crime_told", []))
        for entry in self.people.of(npc):
            if entry.kind in {"npc_theft", "npc_attack"} and entry.key.startswith("npc_crime_seen:"):
                known.add(entry.key.removeprefix("npc_crime_seen:"))
        return [f for f in [*self.crimes.facts(), *self.npc_crimes.facts()]
                if f.get("id") in known and (f.get("victim") or f.get("owner")) == npc
                and f.get("kind") in {"theft", "attack", "murder", "sheep"}]

    def _title(self, crime: Fact) -> str:
        if crime.get("kind") == "theft":
            item = crime.get("item_name") or crime.get("item") or "твою вещь"
            return f"украл(а) {item}"
        victim = self.crimes.names(crime.get("victim") or crime.get("owner") or "")
        return f"напал(а) на {victim}" if crime.get("kind") == "attack" else f"убил(а) {victim}"

    async def prepare(self, npc: str) -> None:
        pass

    def situation(self, npc: str) -> str | None:
        if not self.enabled:
            return None
        lines = []
        for crime in self._eligible(npc):
            belief = self.active(npc, crime["id"])
            if belief is not None:
                lines.append(f"- {crime['id']}: ты считаешь, что {belief['culprit_name']} "
                             f"{self._title(crime)}.")
            elif crime.get("kind") == "theft":
                item = crime.get("item_name") or crime.get("item") or "твоя вещь"
                lines.append(f"- {crime['id']}: у тебя пропало {item}.")
            else:
                lines.append(f"- {crime['id']}: дело против тебя — {crime['kind']}.")
        return "Твои дела и пропажи (id для blame):\n" + "\n".join(lines) if lines else None

    @property
    def all_tools(self) -> tuple[Tool, ...]:
        return (BLAME_TOOL,) if self.enabled else ()

    def tools(self, npc: str) -> list[Tool]:
        return [BLAME_TOOL] if self._eligible(npc) else []

    def _person(self, npc: str, who: str) -> str | None:
        names = {key: card.name for key, card in self.npc_crimes.cards.items()}
        names.update((key, entry.name) for key, entry in self.npc_crimes.roster.items())
        names.update((key, self.crimes.names(key)) for key in self.crimes.people)
        if self.crimes.sightings is not None:
            names.update((s.who, s.name) for s in self.crimes.sightings.mind(npc).sightings)
        names.update((key, conv.persona.name) for key, conv in self.dialogue.conversations.items())
        normalized = who.casefold().replace("ё", "е").strip()
        return next((key.upper() for key, name in names.items()
                     if normalized in {key.casefold(), name.casefold().replace("ё", "е")}), None)

    def _trace_matches(self, sightings: list[Sighting], culprit: str, crime: Fact) -> bool:
        at = int(crime.get("day", 0)) * 1440 + int(crime.get("hour", 0)) * 60 + int(crime.get("minute", 0))
        place = self.crimes.places.of(crime.get("waypoint")) if self.crimes.places else None
        return any(s.who == culprit and not s.trace_kind and s.end >= at - SUSPECT_MIN
                   and s.start <= at + SUSPECT_MIN and place is not None and place.id in s.place_ids
                   for s in sightings)

    def _own_trace(self, npc: str, culprit: str, crime: Fact) -> bool:
        return self.crimes.sightings is not None and self._trace_matches(
            self.crimes.sightings.mind(npc).sightings, culprit, crime,
        )

    @staticmethod
    def _words(conv: Conversation, name: str) -> str:
        """The latest recent hero line naming the accused, with the witness name-stem rule."""
        name = name.casefold().replace("ё", "е").strip()
        if not name:
            return ""
        stem = name[:max(3, min(4, len(name) - 1))]
        named = re.compile(rf"(?<!\w){re.escape(name)}(?!\w)")

        def mentions(line: str) -> bool:
            words = line.casefold().replace("ё", "е")
            return bool(named.search(words)) or any(
                root.startswith(stem) and name.startswith(root) for root in roots(words)
            )

        lines = [e.content for e in conv.memory.entries if e.role == "user"][-HEARD:]
        if conv.trigger is not None and conv.trigger.role == "user":
            lines.append(conv.trigger.content)
        return next((line.strip() for line in reversed(lines) if mentions(line)), "")

    async def handle(
        self, dialogue: DialogueManager, conv: Conversation, call: ToolCall, snap: Snapshot | None,
    ) -> ToolOutcome | None:
        if call.name != BLAME:
            return None
        if not self.enabled or self._loading:
            return ToolOutcome("Отклонено: дела NPC выключены или мир загружается.", "rejected")
        npc, cid = conv.npc.upper(), str(call.arguments.get("crime") or "").strip()
        crime = next((f for f in self._eligible(npc) if f["id"] == cid), None)
        if crime is None:
            return ToolOutcome("Отклонено: такого твоего дела нет в обстановке.", "rejected")
        if any(f["id"] == cid for f in [*self.crimes.seen(npc), *self.npc_crimes.seen(npc)]):
            return ToolOutcome("Отклонено: ты сам видел настоящего виновника этого дела.", "rejected")
        who = str(call.arguments.get("who") or "").strip()
        culprit = self._person(npc, who)
        if who.upper() in HERO or who.casefold() in {"герой", "ты"}:
            culprit = "PLAYER"
        if culprit is None or culprit in {*HERO, npc}:
            return ToolOutcome("Отклонено: who — другой человек из обстановки, не герой.", "rejected")
        if self.npc_crimes._human(culprit) is False:
            return ToolOutcome("Отклонено: who — человек.", "rejected")
        if any(f.get("npc") == culprit for f in self.chronicle.facts("died")):
            return ToolOutcome("Отклонено: этот человек мёртв.", "rejected")
        existing = self.active(npc, cid)
        if existing is not None and existing["culprit"] == culprit:
            return ToolOutcome("Ты уже считаешь его виновным в этом деле.", "accepted")
        testimony = next((f for f in reversed(self.chronicle.facts("testimony"))
                          if f.get("npc") == npc and f.get("about") == culprit
                          and (cid in f.get("crime_told", []) or self._trace_matches(
                              [Sighting.of(row) for row in f.get("told", [])], culprit, crime,
                          ))), None)
        culprit_name, name = self.crimes.names(culprit), self.crimes.names(npc)
        said = self._words(conv, culprit_name)
        because = (
            f"testimony:{testimony['id']}" if testimony is not None
            else "own" if self._own_trace(npc, culprit, crime)
            else "hero_words" if said else "hunch"
        )
        number = 1 + len(self.chronicle.facts("blamed"))
        action = self._title(crime)
        fact = {
            "fact": "blamed", "id": f"BLAME_{number}", "npc": npc,
            "name": name, "culprit": culprit,
            "culprit_name": culprit_name, "crime": cid, "because": because,
            "truth": crime.get("fact") == "npc_crime" and crime.get("culprit") == culprit,
            "title": f"{name} считает, что {culprit_name} {action}",
            "day": snap.world.day if snap is not None else crime.get("day", 0),
            **({"said": said} if because == "hero_words" else {}),
        }
        self.chronicle.record(fact)
        dialogue.remember(npc, name, f"Ты считаешь, что {culprit_name} {action} ({cid}).")
        return ToolOutcome(f"Принято: ты считаешь, что {culprit_name} {action}.", "accepted")

    def _on_message(self, message: GameMessage) -> None:
        if isinstance(message, (Loading, Session)):
            for task in tuple(self._tasks):
                task.cancel()
            self._resolving.clear()
            self._loading = isinstance(message, Loading)
            if not self._loading and self.enabled:
                self._spawn(self._recover())

    def _on_fact(self, fact: Fact) -> None:
        if not self.enabled or self._loading:
            return
        if fact.get("fact") in {"npc_crime_defense", "npc_crime_restitution"}:
            self._spawn(self._resolve(fact))

    async def _resolve(self, fact: Fact) -> None:
        facts = self.chronicle.facts()
        for belief in self.beliefs():
            if not self._same(belief, fact):
                continue
            # Old defenses have no belief marker. A load must not use a result from before
            # this accusation to disprove a later belief about the same person and case.
            if not fact.get("blamed") and facts.index(fact) < facts.index(belief):
                continue
            if fact.get("outcome") == "no_goods":
                await self.refute(belief, "no_goods")
            elif fact.get("fact") == "npc_crime_restitution" or fact.get("outcome") == "reclaimed":
                await self.confirm(belief)

    async def _recover(self) -> None:
        for fact in self.chronicle.facts():
            if fact.get("fact") in {"npc_crime_defense", "npc_crime_restitution"}:
                await self._resolve(fact)
        for fact in self.chronicle.facts("blame_refuted"):
            belief = next((b for b in self.chronicle.facts("blamed") if b["id"] == fact.get("blamed")), None)
            if belief is not None:
                await self.refute(belief, str(fact.get("how") or "no_goods"))

    async def refute(self, belief: Fact, how: str = "no_goods") -> None:
        if not self.enabled or self._loading or belief["id"] in self._resolving:
            return
        self._resolving.add(belief["id"])
        try:
            if not any(f.get("blamed") == belief["id"] for f in self.chronicle.facts("blame_refuted")):
                self.chronicle.record({
                    "fact": "blame_refuted", "npc": belief["npc"], "culprit": belief["culprit"],
                    "crime": belief["crime"], "blamed": belief["id"], "how": how,
                })
                self.dialogue.remember(
                    belief["npc"], belief["name"],
                    f"У {belief['culprit_name']} вещи не оказалось. Его вина не подтвердилась.",
                )
            if belief["because"] != "hero_words" or belief["truth"]:
                return
            quest = f"SAID_{belief['id']}"
            if any(f.get("quest") == quest for f in self.chronicle.facts("caught")):
                return
            lie = Lie(quest, belief["npc"], belief["name"], belief["said"], "", said=True)
            await caught(self.link, self.dialogue, self.chronicle, self.journal, lie,
                         f"узнал, что у {belief['culprit_name']} вещи не оказалось",
                         extra={"blamed": belief["id"], "blame_crime": belief["crime"]})
        finally:
            self._resolving.discard(belief["id"])

    async def confirm(self, belief: Fact) -> None:
        if not self.enabled or self._loading or not belief["truth"] or belief["because"] != "hero_words":
            return
        quest = f"SAID_{belief['id']}"
        if any(f.get("quest") == quest for f in self.chronicle.facts("claim")):
            return
        self.chronicle.record({
            "fact": "claim", "quest": quest, "npc": belief["npc"], "name": belief["name"],
            "title": belief["said"], "said": True, "truth": True, "believed": True,
            "blamed": belief["id"], "blame_crime": belief["crime"],
        })
        self.dialogue.remember(
            belief["npc"], belief["name"], f"Герой сказал правду: «{belief['said']}». Вещь вернулась.",
        )
