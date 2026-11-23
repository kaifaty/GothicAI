"""No model: a scenario may declare givens, and every other request is an error."""

from __future__ import annotations

import json
import logging
from collections.abc import AsyncIterator, Collection, Sequence
from dataclasses import dataclass

from .base import Done, Item, LLMError, LLMEvent, Message, TextDelta, Tool, ToolCall, ToolChoice

log = logging.getLogger(__name__)


class ModelOff(LLMError):
    """A request was not covered by an applicable scenario given."""


@dataclass
class _Given:
    reason: str
    who: str
    when: str | None
    calls: tuple[ToolCall, ...]
    line: str
    remaining: int


class OffBackend:
    name = "off"

    def __init__(self) -> None:
        self._givens: list[_Given] = []
        self.attempts: list[str] = []
        self.used: list[str] = []

    def given(
        self,
        reason: str,
        *,
        who: str,
        when: str | None = None,
        calls: Sequence[ToolCall] = (),
        line: str = "",
        times: int = 1,
    ) -> None:
        self._givens.append(_Given(reason, who, when, tuple(calls), line, times))

    def silent(self, reason: str, *, who: str, when: str | None = None, times: int = 1) -> None:
        self.given(reason, who=who, when=when, times=times)

    def unused(self) -> list[str]:
        return [given.reason for given in self._givens if given.remaining > 0]

    async def aclose(self) -> None:
        pass

    async def prewarm(self, *, instructions: str, messages: Sequence[Item]) -> None:
        pass

    def open_ahead(self, key: str, *, keep: bool = False) -> None:
        pass

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
        max_tokens: int | None = None,
        grammar: str | None = None,
    ) -> AsyncIterator[LLMEvent]:
        last = next((item.content for item in reversed(messages) if isinstance(item, Message)), "")
        offered = [
            tool.name
            for tool in tools or []
            if tool_choice != "none" and (allowed is None or tool.name in allowed)
        ]
        context = f"who={cache_key}, message={last[:120]!r}, tools={offered}"
        given = next(
            (
                given
                for given in self._givens
                if given.remaining > 0
                and given.who == cache_key
                and (given.when is None or given.when in last)
            ),
            None,
        )
        error = f"model is off: {context}"
        if given is not None:
            for call in given.calls:
                if call.name not in offered:
                    error = f"given not applicable: {call.name} not offered to {cache_key}; {context}"
                    break
            else:
                error = ""
        if error:
            self.attempts.append(error)
            log.error("%s", error)
            raise ModelOff(error)

        assert given is not None
        given.remaining -= 1
        self.used.append(given.reason)
        log.info("given: %s", given.reason)
        for i in range(0, len(given.line), 7):
            yield TextDelta(given.line[i : i + 7])
        items: list[Item] = [Message("assistant", given.line)] if given.line else []
        for i, call in enumerate(given.calls):
            call = ToolCall(
                call.name,
                call.arguments,
                call.call_id or f"given_{len(self.used)}_{i}",
                call.raw or json.dumps(call.arguments, ensure_ascii=False),
            )
            items.append(call)
            yield call
        yield Done(given.line, tuple(items))
