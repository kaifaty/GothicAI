"""Quick-model prewarming, prompt slots and surrender classification."""

from __future__ import annotations

import asyncio
import json
import logging
import time
from collections.abc import AsyncIterator
from contextlib import aclosing, asynccontextmanager
from typing import Any

from . import npc_tools
from .dialogue_constants import CLASSIFIER_SLOT, PREWARM_CM, PREWARM_SLOTS, SURRENDER_CLASSIFIER
from .dialogue_state import Conversation
from .llm import Item, LLMError, LLMEvent, Message, TextDelta, open_ahead
from .llm.base import QUICK_SLOT

log = logging.getLogger("gothic_ai.dialogue")


class DialogueQuickMixin:
    def _warm_candidates(self, focus: str | None = None) -> list[str]:
        """Up to three likely speakers: the focused npc and the nearest others, focus warmed last."""
        candidates = [n for n, c in self.conversations.items() if n == focus or self._listening(c)]
        looking_at = focus or self._focus
        candidates.sort(key=lambda n: (n != looking_at, self._distance(n)))
        return sorted(candidates[:PREWARM_SLOTS], key=lambda n: n == looking_at)

    def _prewarm_focus(self) -> None:
        """Open a nearby focused npc without a turn; repeated views reuse its unchanged history."""
        npc = self._focus
        if (
            npc not in self._nearby
            or self._distance(npc) > PREWARM_CM
            or not hasattr(self.quick, "prewarm")
        ):
            return
        conv = self.open(npc, self._nearby[npc][0], warm=False)
        if self._prewarm_history.get(npc) != conv.history:
            self.prewarm(npc)

    def prewarm(self, npc: str) -> None:
        """Fill one of the quick model's three slots for a nearby focused npc, recent partner or fighter.
        Its next words only read the situation and trigger. New history replaces a running warm."""
        conv = self.conversations.get(npc.upper())
        if conv is None or not self._listening(conv):
            return
        candidates = self._warm_candidates()
        if conv.npc in candidates:
            self._start_prewarm(conv, candidates)

    def _start_prewarm(self, conv: Conversation, candidates: list[str]) -> None:
        if self._surrender_probes:
            return
        self._open_ahead(conv.npc)
        warm = getattr(self.quick, "prewarm", None)
        if warm is None:
            return
        if conv.reply is not None and not conv.reply.done():
            return  # its turn is on (warming now would hold up its line): once it is over
        slot = self._quick_slot(conv.npc)
        if slot is None or self._quick_busy[slot]:
            return
        for other, task in self._prewarms.items():
            if other not in candidates:
                task.cancel()
        self._prewarm_history = {n: h for n, h in self._prewarm_history.items() if n in candidates}
        if (old := self._prewarms.get(conv.npc)) is not None:
            old.cancel()
        self._prewarm_history[conv.npc] = conv.history
        task = asyncio.create_task(self._prewarm(conv, warm, slot))
        self._prewarms[conv.npc] = task

        def forget(t: asyncio.Task[None], npc: str = conv.npc) -> None:
            if self._prewarms.get(npc) is t:
                del self._prewarms[npc]
                if t.cancelled():
                    self._prewarm_history.pop(npc, None)

        task.add_done_callback(forget)

    def prewarm_listeners(self, focus: str | None = None) -> None:
        """Push-to-talk warms up to three npc's: the focused one (even before a first conversation)
        and the nearest recent partners or fighters. Focus goes last, freshest in the three slots."""
        focus = focus.upper() if focus else None
        if focus in self._nearby and focus not in self.conversations and hasattr(self.quick, "prewarm"):
            self.open(focus, self._nearby[focus][0], warm=False)
        if focus in self.conversations:
            self._open_ahead(focus)  # though it has not talked lately: the words to it are on their way
        candidates = self._warm_candidates(focus)
        for npc in candidates:
            self._start_prewarm(self.conversations[npc], candidates)

    def _open_ahead(self, npc: str) -> None:
        """The conversational model's connection for the npc, opened before its turn (openai_responses.py)."""
        open_ahead(self.llm, npc)

    def _quick_slot(self, npc: str) -> int | None:
        npc = npc.upper()
        if npc not in self._quick_slots:
            free = (set(range(PREWARM_SLOTS)) - set(self._quick_slots.values())
                    - {s for s, count in enumerate(self._quick_busy) if count})
            if free:
                slot = min(free)
            else:
                idle = [n for n, s in self._quick_slots.items() if not self._quick_busy[s]]
                if not idle:
                    return None
                old = min(idle, key=self._quick_used.__getitem__)
                slot = self._quick_slots.pop(old)
                del self._quick_used[old]
                self._prewarm_history.pop(old, None)
                if warm := self._prewarms.get(old):
                    warm.cancel()
            self._quick_slots[npc] = slot
        self._quick_used[npc] = next(self._quick_clock)
        return self._quick_slots[npc]

    @asynccontextmanager
    async def _quick_request(self, npc: str) -> AsyncIterator[None]:
        started, requested_at = time.monotonic(), time.time()
        while (slot := self._quick_slot(npc)) is None:
            self._quick_free.clear()
            await self._quick_free.wait()
        slotted = time.monotonic()
        self._quick_busy[slot] += 1  # includes requests waiting for this NPC's previous real request
        if warm := self._prewarms.get(npc.upper()):
            warm.cancel()  # do not wait for a prewarm or share its slot with a real request
        try:
            async with self._quick_locks[slot]:
                conv = self.conversations.get(npc.upper())
                if self.trace is not None and conv is not None and conv.trigger is not None and (
                    conv.trigger.role == "user" and conv.reply is not None and not conv.reply.done()
                ):
                    self.trace.write({
                        "type": "quick_request", "t": round(time.time(), 3), "npc": npc,
                        "requested_at": requested_at, "slot": slot,
                        "slot_wait_ms": round((slotted - started) * 1000, 1),
                        "lock_wait_ms": round((time.monotonic() - slotted) * 1000, 1),
                    })
                token = QUICK_SLOT.set(slot)
                try:
                    yield
                finally:
                    QUICK_SLOT.reset(token)
        finally:
            self._quick_busy[slot] -= 1
            self._quick_free.set()

    async def _quick_stream(self, npc: str, **options: Any) -> AsyncIterator[LLMEvent]:
        assert self.quick is not None
        async with self._quick_request(npc), aclosing(self.quick.stream(**options)) as events:
            async for event in events:
                yield event

    def _check_slots(self) -> None:
        count = getattr(self.quick, "slot_count", None)
        if count is None or (self._slot_check is not None and not self._slot_check.done()):
            return  # each hello asks again: the quick server may have been restarted with other slots

        async def check() -> None:
            slots = await count()
            self._classifier_slot = CLASSIFIER_SLOT if slots is not None and slots > CLASSIFIER_SLOT else None
            log.info("quick model: %s slots, surrender classifier %s", slots,
                     "on its own slot" if self._classifier_slot is not None else "borrows an npc slot")

        task = self._slot_check = asyncio.create_task(check())
        self._background.add(task)
        task.add_done_callback(self._background.discard)

    async def _surrendering(
        self, conv: Conversation, request: list[Item], *, opponent: str | None = None,
    ) -> bool:
        """Classify outside the persona on its own slot, or else an idle slot other than the speaker's.

        Cancel warming and SVM generation first. Without CLASSIFIER_SLOT on the server, select an idle
        foreign LRU slot: never touch the speaker's prefix or another foreground request; no spare slot
        means no hold.
        """
        entered, wall = time.monotonic(), time.time()
        attempt: dict[str, Any] = {
            "type": "surrender_probe", "t": round(wall, 3), "npc": conv.npc,
            "received_at": wall - (entered - conv.talked_at),
            "entered_ms": round((entered - conv.talked_at) * 1000, 1),
            "slot_wait_ms": 0.0, "outcome": "unavailable",
        }
        self._surrender_probes += 1
        try:
            if (self.quick is None or getattr(self.quick, "_id_slot_supported", True) is False
                    or entered >= conv.talked_at + 2.0):
                return False
            async with asyncio.timeout_at(conv.talked_at + 2.0):
                background = {task for task in (*self._prewarms.values(), *self._svm_tasks.values())
                              if not task.done()}
                for task in background:
                    task.cancel()
                if background:
                    await asyncio.gather(*background, return_exceptions=True)
                attempt["cancel_ms"] = round((time.monotonic() - entered) * 1000, 1)
                if self._classifier_slot is not None:
                    slot = self._classifier_slot
                else:
                    own = self._quick_slots.get(conv.npc)
                    slots = [s for s in range(PREWARM_SLOTS)
                             if s != own and not self._quick_busy[s] and not self._quick_locks[s].locked()]
                    if not slots:
                        attempt["outcome"] = "no_slot"
                        return False
                    owners = {s: n for n, s in self._quick_slots.items()}
                    slot = min(slots, key=lambda s: self._quick_used.get(owners.get(s), -1))
                    if old := owners.get(slot):
                        del self._quick_slots[old]
                        self._quick_used.pop(old, None)
                        self._prewarm_history.pop(old, None)
                        self._prewarms.pop(old, None)
                attempt["slot"] = slot
                phrase = next(m.content for m in reversed(request)
                              if isinstance(m, Message) and m.role == "user")
                context = self.surrender_context(conv.npc) if opponent is None else opponent
                text_in = (f"Перед этим противник сказал: «{context}»\n" if context else "")
                text_in += f"Реплика игрока: «{phrase}»"
                # its own task, its slot held until the request really ends: httpx may swallow a cancel
                # that lands while it connects, and the deadline must not wait for the answer (2026-10-28)
                classify = asyncio.create_task(self._classify(slot, text_in, conv.talked_at, attempt))
                try:
                    text = await asyncio.shield(classify)
                except asyncio.CancelledError:
                    classify.cancel()
                    self._background.add(classify)

                    def abandoned(done: asyncio.Task[str]) -> None:
                        self._background.discard(done)
                        if not done.cancelled() and (error := done.exception()) is not None:
                            log.info("%s: late surrender probe: %s", conv.npc, error)

                    classify.add_done_callback(abandoned)
                    raise
                answer = text.strip().lower()
                attempt.update(answer=answer, outcome="yes" if answer == "да" else "no")
                return answer == "да"
        except (LLMError, TimeoutError) as error:
            attempt.update(outcome="late" if isinstance(error, TimeoutError) else "error",
                           error=type(error).__name__)
            return False
        finally:
            self._surrender_probes -= 1
            attempt["completed_ms"] = round((time.monotonic() - conv.talked_at) * 1000, 1)
            log.info("%s: surrender probe %s", conv.npc, json.dumps(attempt, ensure_ascii=False))
            if self.trace is not None:
                self.trace.write(attempt)

    async def _classify(self, slot: int, text_in: str, talked_at: float, attempt: dict[str, Any]) -> str:
        text = ""
        self._quick_busy[slot] += 1
        QUICK_SLOT.set(slot)  # this task's own context
        locked = time.monotonic()
        try:
            async with self._quick_locks[slot]:
                attempt["lock_wait_ms"] = round((time.monotonic() - locked) * 1000, 1)
                attempt["request_ms"] = round((time.monotonic() - talked_at) * 1000, 1)
                async with aclosing(self.quick.stream(
                    instructions=SURRENDER_CLASSIFIER, messages=[Message("user", text_in)],
                    tools=[], temperature=0, max_tokens=4, grammar='root ::= "да" | "нет"',
                )) as events:
                    async for event in events:
                        if isinstance(event, TextDelta):
                            first = round((time.monotonic() - talked_at) * 1000, 1)
                            attempt.setdefault("first_token_ms", first)
                            text += event.text
        finally:
            attempt["timings"] = dict(getattr(self.quick, "timings", {}).get(slot, {}))
            self._quick_busy[slot] -= 1
            self._quick_free.set()
        return text

    def _yield_to(self, npc: str) -> None:
        """Prioritize this NPC over other warming and SVM requests; its own warm is cancelled
        when the real request acquires its slot."""
        for other, task in list(self._prewarms.items()):
            if other != npc:
                task.cancel()
        for task in self._svm_tasks.values():
            task.cancel()

    def _brief(self, conv: Conversation) -> str:
        """What the npc perceives now, briefly: the situation of the quick model's words."""
        snap = self.link.snapshots.get(conv.npc)
        sight = self.sight(conv.npc, snap) if self.sight is not None else None
        text = npc_tools.render_situation(snap, conv.intent, sight, brief=True)
        if self.balance is not None and (balance := self.balance(conv.npc)):
            text += "\n" + balance
        return text
