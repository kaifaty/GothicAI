"""Short, optional quick-model cries on actual combat hits, through the ordinary voice path."""

from __future__ import annotations

import asyncio
import random
import time
from collections import deque
from collections.abc import Callable
from contextlib import aclosing
from dataclasses import dataclass, field
from typing import Any

from .dialogue import (
    _FIRST_SENTENCE,
    _LETTERS,
    Conversation,
    DialogueManager,
    first_words,
    paragraph_end,
    svm_text,
    to_game_text,
)
from .game import GameError
from .llm import LLMError, Message, TextDelta
from .perception import BEASTS
from .protocol import CombatHit, GameMessage, Loading, NpcRoster, NpcView, NpcViews, RosterEntry, Session

REPLY_S = 1.2
NPC_GAP_S = 5.0
WORLD_GAP_S = 1.5
FIGHT_S = 20.0
HINT = (
    "[Сейчас бой: {event}. Вырвется одна короткая фраза до шести слов — как у тебя, "
    "по характеру и по тому, как идёт бой. Ругнуться можно, если ты так говоришь.]"
)


@dataclass
class Fighter:
    hit_at: float = float("-inf")
    low_hp: bool = False
    first: bool = True
    phrases: deque[str] = field(default_factory=lambda: deque(maxlen=5))


class CombatBarks:
    def __init__(self, dialogue: DialogueManager, *, rng: Callable[[], float] = random.random) -> None:
        self.dialogue = dialogue
        self.link = dialogue.link
        self.rng = rng
        self.companion: Callable[[str], bool] = lambda npc: False
        self.roster: dict[str, RosterEntry] = {}
        self.views: dict[str, NpcView] = {}
        self.fighters: dict[str, Fighter] = {}
        self.task: asyncio.Task[None] | None = None
        self.active: tuple[Conversation, str] | None = None
        self.last_at = float("-inf")
        self.branch = dialogue.branch
        self.link.listeners.append(self._on_message)

    def _on_message(self, msg: GameMessage) -> None:
        match msg:
            case NpcRoster():
                if msg.full:
                    self.roster.clear()
                self.roster.update((n.id.upper(), n) for n in msg.npcs)
            case NpcViews():
                self.views = {v.npc.upper(): v for v in msg.views}
            case Loading() | Session():
                if isinstance(msg, Session) and msg.branch == self.branch:
                    return
                if isinstance(msg, Session):
                    self.branch = msg.branch
                if self.task is not None:
                    self.task.cancel()
                if self.active is not None:
                    conv, line = self.active
                    self.dialogue._drop_unvoiced(conv, [line])
                self.active = None
                self.roster.clear()
                self.views.clear()
                self.fighters.clear()
                self.last_at = float("-inf")
            case CombatHit():
                if self.dialogue.quick is not None:
                    self._hit(msg)

    def _eligible(self, npc: str) -> bool:
        entry = self.roster.get(npc)
        view = self.views.get(npc)
        return bool(
            npc not in {"PLAYER", "PC_HERO"} and entry is not None
            and entry.guild < BEASTS and entry.distance <= 3000
            and entry.activity not in {"down", "dialog"}
            and not self.dialogue._svm_down.get(npc, False)
            and (view is None or (view.hp > 0 and view.body not in {"down", "dead"}))
        )

    def _name(self, npc: str) -> str:
        if npc in {"PLAYER", "PC_HERO"}:
            return "герой"
        snap = self.link.snapshots.get(npc)
        if snap is not None:
            return snap.npc.name
        if entry := self.roster.get(npc):
            return entry.name
        for view in self.views.values():
            if seen := next((n for n in view.sees if n.id.upper() == npc), None):
                return seen.name
        card = self.dialogue.personas.cards.get(npc)
        return card.name if card else npc

    def _hit(self, hit: CombatHit) -> None:
        now = time.monotonic()
        victim, attacker = hit.npc.upper(), hit.by.upper()
        new_fighters = []
        for npc in {victim, attacker}:
            fighter = self.fighters.setdefault(npc, Fighter())
            if now - fighter.hit_at >= FIGHT_S:
                fighter.first, fighter.low_hp = True, False
                if self._eligible(npc) and not (npc == victim and (hit.down or hit.dead)):
                    new_fighters.append(npc)
            fighter.hit_at = now
        if new_fighters and hasattr(self.dialogue.quick, "prewarm"):
            # A raw hit need not be an IncidentDesk incident: warm both human participants here.
            candidates = [*self.dialogue._prewarm_history, *new_fighters]
            for npc in sorted(new_fighters):
                conv = self.dialogue.open(npc, self._name(npc), warm=False)
                if self.dialogue._prewarm_history.get(npc) != conv.history:
                    self.dialogue._start_prewarm(conv, candidates)
        if hit.down or hit.dead:
            self.dialogue._svm_down[victim] = True
            # In a victim's view `att` is how the seen person treats that victim.
            view = self.views.get(victim)
            entry = self.roster.get(victim)
            allies = []
            if victim in {"PLAYER", "PC_HERO"}:
                friendly = {
                    seen.id.upper() for v in self.views.values() for seen in v.sees
                    if seen.att_hero == "friendly"
                }
                allies = [
                    (entry.distance, npc) for npc, entry in self.roster.items()
                    if entry.distance <= 1500 and self._eligible(npc)
                    and (npc in friendly or self.companion(npc) or self.link.party.get(npc, (0, 0))[1])
                ]
            elif view is not None:
                for seen in view.sees:
                    npc = seen.id.upper()
                    if (npc != victim and self._eligible(npc)
                        and seen.distance <= 1500 and seen.body not in {"down", "dead"}
                        and (seen.att == "friendly" or (entry and seen.guild == entry.guild))):
                        allies.append((seen.distance, npc))
            if allies:
                self._attempt(min(allies)[1], "ally_down", hit,
                              f"{self._name(victim)} упал — его сразил {self._name(attacker)}", now)
            return
        fighter = self.fighters[victim]
        low = hit.hp_max > 0 and hit.hp < hit.hp_max * 0.3 and not fighter.low_hp
        fighter.low_hp |= low
        if self._eligible(victim):
            event = (f"ты тяжело ранен, сил почти не осталось — бьёт {self._name(attacker)}" if low
                     else f"тебе больно досталось — {self._name(attacker)}")
            self._attempt(victim, "low_hp" if low else "hurt", hit, event, now)
        if self._eligible(attacker):
            self._attempt(attacker, "landed", hit, f"ты попал по {self._name(victim)}", now)

    def _busy(self, npc: str) -> bool:
        conv = self.dialogue.conversations.get(npc)
        return bool(conv and (
            (conv.reply is not None and not conv.reply.done())
            or self.dialogue.speaking(npc) or conv.overlay_free_at > time.monotonic()
            or (conv.voice is not None and conv.voice._unfinished_tasks > 0)
            or conv.svm_pending or conv.bark_pending
        ))

    def _cooldown(self, npc: str) -> bool:
        conv = self.dialogue.conversations.get(npc)
        return time.monotonic() - max(
            conv.said_at if conv else float("-inf"),
            self.dialogue._spoke_at.get(npc, float("-inf")),
        ) < NPC_GAP_S

    def _world_busy(self) -> bool:
        if self.task is not None and not self.task.done():
            return True
        if self.active is not None:
            conv, line = self.active
            return (line in conv.bark_pending
                    or (conv.voice is not None and conv.voice._unfinished_tasks > 0)
                    or self.dialogue._to_hear(conv, line, time.monotonic()))
        return False

    def _attempt(self, npc: str, reason: str, hit: CombatHit, event: str, received: float) -> None:
        fighter = self.fighters.setdefault(npc, Fighter())
        attempt: dict[str, Any] = {
            "type": "combat_bark", "npc": npc, "reason": reason,
            "hit": hit.model_dump(mode="json"), "by": hit.by, "victim": hit.npc,
            "text": "", "outcome": "busy", "overlay": True,
        }
        recorded = False

        def record(outcome: str) -> None:
            nonlocal recorded
            if recorded:
                return
            recorded = True
            attempt["outcome"] = outcome
            attempt["elapsed_ms"] = round((time.monotonic() - received) * 1000, 1)
            if outcome == "said":
                self.last_at = time.monotonic()
                attempt["voice_ms"] = attempt["elapsed_ms"]
                fighter.phrases.append(attempt["text"])
            if self.dialogue.trace is not None:
                self.dialogue.trace.write({"t": round(time.time(), 3), **attempt})

        if self._busy(npc) or self._world_busy():
            record("busy")
        elif self._cooldown(npc) or received - self.last_at < WORLD_GAP_S:
            record("cooldown")
        elif reason in {"hurt", "landed"} and not fighter.first and self.rng() >= 0.5:
            record("chance")
        else:
            fighter.first = False
            conv = self.dialogue.open(npc, self._name(npc), warm=False)
            self.task = asyncio.create_task(self._generate(conv, event, received, attempt, record))
            self.task.add_done_callback(lambda task: record("busy") if task.cancelled() else None)

    async def _generate(
        self, conv: Conversation, event: str, received: float, attempt: dict[str, Any],
        record: Callable[[str], None],
    ) -> None:
        quick = self.dialogue.quick
        assert quick is not None
        text = ""
        try:
            async with asyncio.timeout_at(received + REPLY_S):
                async with aclosing(self.dialogue._quick_stream(
                    conv.npc, instructions=conv.persona.instructions,
                    messages=[Message(
                        "developer", f"{self.dialogue._brief(conv)}\n{HINT.format(event=event)}",
                    )],
                    tools=[], temperature=0.7,
                )) as events:
                    async for delta in events:
                        if isinstance(delta, TextDelta):
                            attempt.setdefault(
                                "first_token_ms", round((time.monotonic() - received) * 1000, 1),
                            )
                            slot = self.dialogue._quick_slots[conv.npc]
                            if timings := getattr(quick, "timings", {}).get(slot):
                                attempt.update(
                                    slot=slot, cache_n=timings.get("cache_n"),
                                    prompt_n=timings.get("prompt_n"),
                                )
                            text += delta.text
                            if _FIRST_SENTENCE.search(text) or paragraph_end(text) is not None:
                                break
            words, mood = first_words(svm_text(text))
            words = to_game_text(words)
            attempt["text"] = words
            attempt["first_phrase_ms"] = round((time.monotonic() - received) * 1000, 1)
            if time.monotonic() >= received + REPLY_S:
                record("late")
            elif len(_LETTERS.findall(words)) > 8:
                record("too_long")
            elif words in self.fighters[conv.npc].phrases:
                record("same")
            elif not words:
                record("error")
            elif not self._eligible(conv.npc) or self._busy(conv.npc):
                record("busy")
            elif self._cooldown(conv.npc):
                record("cooldown")
            else:
                line = await self.dialogue._say(conv, words, None, overlay=True, mood=mood, bark=record)
                if line is not None:
                    self.active = (conv, line)
                else:
                    record("busy")
        except TimeoutError:
            record("late")
        except (LLMError, GameError):
            record("error")
        except asyncio.CancelledError:
            record("busy")
            raise
