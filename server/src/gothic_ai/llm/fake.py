"""Deterministic backend for tests and offline runs: replies with canned text, streamed in small chunks.

One reply for every call (`FakeBackend("text", tool_calls=[...])`), or a script with one step per call
(`FakeBackend(script=[FakeStep(...), FakeStep(...)])`; the last step repeats).
"""

from __future__ import annotations

import asyncio
import json
from collections.abc import AsyncIterator, Collection, Sequence
from dataclasses import dataclass, field

from .base import QUICK_SLOT, Done, Item, LLMEvent, Message, TextDelta, Tool, ToolCall, ToolChoice

# with mood tags, as the persona's rules ask (the server takes them out of what is said)
DEFAULT_REPLY = (
    "[friendly] Ты вернулся. Хорошо. "
    "[angry] Времени у нас мало, так что слушай внимательно. Драконы уже здесь."
)


@dataclass(frozen=True)
class FakeStep:
    text: str = ""
    tool_calls: list[ToolCall] = field(default_factory=list)


class FakeBackend:
    name = "fake"

    def __init__(
        self,
        reply: str = DEFAULT_REPLY,
        *,
        chunk: int = 7,
        delay: float = 0.02,
        first: float = 0.0,  # seconds before the first chunk: a model that thinks before it writes
        tool_calls: list[ToolCall] | None = None,
        script: list[FakeStep] | None = None,
    ) -> None:
        self.script = script or [FakeStep(reply, list(tool_calls or []))]
        self.chunk = chunk
        self.delay = delay
        self.first = first
        self.calls: list[tuple[str, list[Item], list[Tool]]] = []  # the tools: those it may call
        self.tool_choices: list[ToolChoice | None] = []
        self.sent_tools: list[list[Tool]] = []  # all the tools of each request
        self.cache_keys: list[str | None] = []
        self.temperatures: list[float | None] = []
        self.prewarms: list[tuple[str, list[Item]]] = []  # what was sent to fill the prompt cache
        self.slot_ids: list[int | None] = []
        self.prewarm_slot_ids: list[int | None] = []

    @property
    def reply(self) -> str:
        return self.script[0].text

    async def aclose(self) -> None:
        pass

    async def prewarm(self, *, instructions: str, messages: Sequence[Item]) -> None:
        self.prewarm_slot_ids.append(QUICK_SLOT.get())
        self.prewarms.append((instructions, list(messages)))

    async def stream(
        self,
        *,
        instructions: str,
        messages: Sequence[Item],
        tools: list[Tool] | None = None,
        tool_choice: ToolChoice | None = None,
        allowed: Collection[str] | None = None,
        cache_key: str | None = None,
        temperature: float | None = None,
    ) -> AsyncIterator[LLMEvent]:
        step = self.script[min(len(self.calls), len(self.script) - 1)]
        self.slot_ids.append(QUICK_SLOT.get())
        self.sent_tools.append(list(tools or []))
        self.cache_keys.append(cache_key)
        self.temperatures.append(temperature)
        tools = [t for t in tools or [] if allowed is None or t.name in allowed]
        self.calls.append((instructions, list(messages), tools))
        self.tool_choices.append(tool_choice)
        if self.first > 0:
            await asyncio.sleep(self.first)
        for i in range(0, len(step.text), self.chunk):
            await asyncio.sleep(self.delay)
            yield TextDelta(step.text[i : i + self.chunk])
        items: list[Item] = [Message("assistant", step.text)] if step.text else []
        if tool_choice != "none":
            offered = {t.name for t in tools}
            for n, call in enumerate(c for c in step.tool_calls if c.name in offered):
                call = ToolCall(
                    call.name,
                    call.arguments,
                    call.call_id or f"call_{len(self.calls)}_{n}",
                    call.raw or json.dumps(call.arguments, ensure_ascii=False),
                )
                items.append(call)
                yield call
        yield Done(step.text, tuple(items))
