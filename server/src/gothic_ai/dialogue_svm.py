"""SVM replacement lines, barks and game messages."""

from __future__ import annotations

import asyncio
import logging
import time
from collections.abc import Callable
from contextlib import aclosing
from typing import Any

from .dialogue_constants import PREWARM_SLOTS, SVM_QUIET_S, SVM_REPLY_S, SVM_WORDS_MAX
from .dialogue_state import Conversation
from .dialogue_text import (
    _FIRST_SENTENCE,
    _LETTERS,
    first_words,
    paragraph_end,
    svm_hint,
    svm_text,
    to_game_text,
)
from .game import GameError
from .llm import LLMError, Message, TextDelta
from .protocol import (
    GameMessage,
    Hello,
    LineStarted,
    LineVoice,
    NpcDied,
    NpcRoster,
    NpcViews,
    PttStart,
    Session,
    Snapshot,
    SvmLine,
    SvmModeCmd,
)

log = logging.getLogger("gothic_ai.dialogue")


class DialogueSvmMixin:
    def _on_message(self, msg: GameMessage) -> None:
        match msg:
            case Hello():
                self._svm_mode()
                self._check_slots()
            case SvmLine():
                self._start_svm(msg)
            case Session():
                self._switch_branch(msg)
            case LineStarted():
                self._spoke_at[msg.npc.upper()] = time.monotonic()
                if msg.line in self._lines:
                    self._lines[msg.line] = (self._lines[msg.line][0], time.monotonic())
                if (started := self._starts.pop(msg.line, None)) is not None:
                    started.set()
            case PttStart():
                self.prewarm_listeners(msg.focus)  # while he speaks
            case NpcDied():
                self._svm_down[msg.npc.upper()] = True
            case Snapshot():
                if msg.npc.hp <= 0:
                    self._svm_down[msg.npc.id.upper()] = True
                self._svm_down.update((e.id.upper(), True) for e in msg.nearby_npcs if e.dead)
            case NpcRoster():
                if msg.full:
                    self._nearby.clear()
                self._nearby.update((e.id.upper(), (e.name, e.distance)) for e in msg.npcs)
                self._svm_down.update((e.id.upper(), e.activity == "down") for e in msg.npcs)
                self._prewarm_focus()
            case NpcViews():
                self._focus = next(
                    (v.hero.focus.upper() for v in msg.views if v.hero is not None and v.hero.focus),
                    None,
                )
                for view in msg.views:
                    if view.hero is not None:
                        self._nearby[view.npc.upper()] = (view.name, view.hero.distance)
                self._prewarm_focus()
            case LineVoice():
                self._spawn_story_voice(msg)

    def _svm_mode(self) -> None:
        self._spawn(self.link.send(SvmModeCmd(
            replace=self.svm_lines and self.quick is not None and self._svm_available,
        )))

    def _start_svm(self, msg: SvmLine) -> None:
        received = time.monotonic()
        npc = msg.npc.upper()
        attempt: dict[str, Any] = {"outcome": "busy", "answer": ""}

        def record(outcome: str | None = None) -> None:
            if outcome is not None:
                attempt["outcome"] = outcome
            attempt["elapsed_ms"] = round((time.monotonic() - received) * 1000, 1)
            if outcome == "said":
                attempt["voice_ms"] = attempt["elapsed_ms"]
            log.info("svm_line %s %s «%s» -> %r (%s, %.1f ms)", npc, msg.svm, msg.text,
                     attempt["answer"], attempt["outcome"], attempt["elapsed_ms"])
            if self.trace is not None:
                self.trace.write({**msg.model_dump(mode="json"), "t": round(time.time(), 3), **attempt})

        if not self.svm_lines or self.quick is None or not self._svm_available:
            attempt["outcome"] = "error"
        elif self._distance(npc) > 3000:
            attempt["outcome"] = "far"
        elif (
            npc == "PLAYER" or npc in self._svm_tasks or len(self._svm_tasks) >= PREWARM_SLOTS
            or self._surrender_probes or self._svm_busy(npc)
        ):
            pass
        else:
            conv = self.open(npc, self._nearby.get(npc, (npc, 0))[0], warm=False)
            task = asyncio.create_task(self._svm_line(conv, msg, received, attempt, record))
            self._svm_tasks[npc] = task

            def finished(done: asyncio.Task[str | None]) -> None:
                if self._svm_tasks.get(npc) is done:
                    del self._svm_tasks[npc]
                if done.cancelled() or done.result() is None:
                    record()  # also records cancellation before the coroutine's first step

            task.add_done_callback(finished)
            return
        record()

    def _svm_busy(self, npc: str) -> bool:
        conv = self.conversations.get(npc)
        queued_at = max(
            (self._sent[line] for line in conv.svm_pending), default=float("-inf"),
        ) if conv is not None else float("-inf")
        return (
            (conv is not None and conv.reply is not None and not conv.reply.done())
            or time.monotonic() - max(
                conv.said_at if conv is not None else float("-inf"),
                self._spoke_at.get(npc, float("-inf")),
                queued_at,
            ) < SVM_QUIET_S
        )

    async def _svm_line(
        self, conv: Conversation, msg: SvmLine, received: float, attempt: dict[str, Any],
        record: Callable[[str], None],
    ) -> str | None:
        assert self.quick is not None
        text = ""
        target = (msg.target or "").upper()
        target_name = None
        target_down = False
        if target:
            snap = self.link.snapshots.get(target)
            nearby = self.link.snapshots.get(conv.npc)
            seen = next(
                (n for n in nearby.nearby_npcs if n.id.upper() == target), None,
            ) if nearby else None
            target_down = (snap is not None and snap.npc.hp <= 0) or (seen is not None and seen.dead)
            card = self.personas.cards.get(target)
            target_name = (
                "герой" if target in {"PLAYER", "PC_HERO"} else
                snap.npc.name if snap is not None else
                (seen.name if seen else None) or self._nearby.get(target, ("", 0))[0]
                or (card.name if card else target)
            )
        hint = svm_hint(msg.svm, msg.text, target_name=target_name, self_target=target == conv.npc)
        try:
            async with (
                asyncio.timeout_at(received + SVM_REPLY_S),
                aclosing(self._quick_stream(
                    conv.npc, instructions=conv.persona.instructions,
                    messages=[Message("developer", f"{self._brief(conv)}\n{hint}")],
                    tools=[], temperature=0.7,
                )) as events,
            ):
                async for event in events:
                    if isinstance(event, TextDelta):
                        text += event.text
                        if _FIRST_SENTENCE.search(text) or paragraph_end(text) is not None:
                            break
            self._svm_errors = 0
            words, mood = first_words(svm_text(text))
            words = to_game_text(words)
            attempt["answer"] = words
            attempt["first_phrase_ms"] = round((time.monotonic() - received) * 1000, 1)
            phrase = " ".join(_LETTERS.findall(words.casefold()))
            if time.monotonic() >= received + SVM_REPLY_S:
                attempt["outcome"] = "late"
            elif self._svm_busy(conv.npc):
                attempt["outcome"] = "busy"
            elif len(_LETTERS.findall(words)) > SVM_WORDS_MAX:
                attempt["outcome"] = "too_long"
            elif phrase in conv.svm_phrases or any(
                phrase == " ".join(_LETTERS.findall(self._lines[line][0].casefold()))
                for line in conv.svm_pending
            ):
                attempt["outcome"] = "repeat"
            elif not phrase:
                attempt["outcome"] = "nothing"
            else:
                line = await self._say(
                    conv, words, None, overlay=True, mood=mood,
                    to=msg.target if target != conv.npc and not self._svm_down.get(
                        target, target_down,
                    ) else None,
                )
                if line is not None:
                    conv.svm_pending[line] = record
                    if self.tts is None:
                        self._svm_started(conv, line)
                    return line  # its first sound or cancellation finishes the trace
        except TimeoutError:
            attempt["answer"] = first_words(svm_text(text))[0]
            attempt["outcome"] = "late"
        except LLMError as err:
            attempt["outcome"] = "error"
            attempt["error"] = str(err)
            log.info("%s: SVM quick model failed: %s", conv.npc, err)
            self._svm_errors += 1
            if self._svm_errors >= 3 and self._svm_available:
                self._svm_available = False
                self._svm_mode()
                self._svm_probe = asyncio.create_task(self._recover_svm())
        except GameError:
            attempt["outcome"] = "error"
        except asyncio.CancelledError:
            attempt["outcome"] = "busy"
            raise

    def _svm_started(self, conv: Conversation, line: str) -> None:
        if record := conv.svm_pending.pop(line, None):
            conv.earlier_lines.append(line)
            self._line_replies[line] = line
            words = self._lines[line][0]
            conv.svm_phrases.append(" ".join(_LETTERS.findall(words.casefold())))
            self._note(conv, f"Ты сказал вслух: «{words}»")
            record("said")

    def _bark_started(self, conv: Conversation, line: str) -> None:
        if record := conv.bark_pending.pop(line, None):
            conv.earlier_lines.append(line)
            self._line_replies[line] = line
            self._note(conv, f"Ты сказал вслух: «{self._lines[line][0]}»")
            record("said")
